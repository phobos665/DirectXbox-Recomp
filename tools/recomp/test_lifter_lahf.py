"""
Self-check for lahf after a float compare.

Run: py -3 tools/recomp/test_lifter_lahf.py

`ucomiss xmm0, [k]; lahf; test ah, 44h; jnp` is how MSVC branches on a float
!= when it wants NaN to count as "not equal". lahf used to lift to a comment,
so AH kept whatever eax held and the branch went either way. Outrun 2's
draw-sort loop (sub_00098750) took it on a garbage AH, read an unfilled slot's
index and copied from 0xE6DC9BE4.
"""

import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.lifter import Lifter, lift_basic_block, _make_lahf_value  # noqa: E402


class _Op:
    def __init__(self, reg=None, type="reg", size=4):
        self.type = type
        self.reg = reg
        self.size = size
        self.mem_size = None
        self.imm = 0


class _Insn:
    def __init__(self, mnemonic, operands, op_str=""):
        self.mnemonic = mnemonic
        self.operands = operands
        self.op_str = op_str
        self.address = 0x1000
        self.is_cond_jump = False
        self.is_branch = False
        self.is_call = False
        self.is_ret = False
        self.is_jump = False
        self.call_target = None
        self.jump_target = None


class _Block:
    def __init__(self, insns):
        self.instructions = insns


def test_lahf_after_ucomiss_writes_ah():
    block = _Block([
        _Insn("ucomiss", [_Op("xmm0"), _Op("xmm1")], "xmm0, xmm1"),
        _Insn("lahf", [], ""),
    ])
    stmts, _ = lift_basic_block(Lifter(), block)
    out = "\n".join(stmts)
    assert "SET_HI8(eax," in out, out
    assert "/* lahf - load AH" not in out, out


def test_lahf_with_no_known_setter_is_left_alone():
    # No flag state: the old comment is still the honest answer.
    stmts, _ = lift_basic_block(Lifter(), _Block([_Insn("lahf", [], "")]))
    assert "SET_HI8" not in "\n".join(stmts)


def test_ucomiss_ah_semantics_compiled():
    cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
    if not cc:
        print("  SKIP no C compiler")
        return
    ah = _make_lahf_value("ucomiss", [])
    src = f"""
#include <stdio.h>
#include <stdint.h>
#include <math.h>
static int ah_of(float _fca, float _fcb) {{ return {ah}; }}
int main(void) {{
    /* hardware: greater 0x02, less 0x03, equal 0x42, unordered 0x47 */
    printf("%02X %02X %02X %02X\\n", ah_of(2, 1), ah_of(1, 2), ah_of(1, 1),
           ah_of(NAN, 1));
    return 0;
}}
"""
    with tempfile.TemporaryDirectory() as d:
        c = os.path.join(d, "t.c")
        exe = os.path.join(d, "t.exe")
        open(c, "w").write(src)
        r = subprocess.run([cc, "-w", c, "-o", exe], capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        r = subprocess.run([exe], capture_output=True, text=True)
        assert r.stdout.split() == ["02", "03", "42", "47"], r.stdout


def test_ucomiss_ah_semantics_evaluated():
    # The same four cases without a compiler: the expression only uses
    # ==, !=, <, ||, ?: and |, which map onto Python directly.
    import math
    import re
    expr = _make_lahf_value("ucomiss", []).replace("(uint8_t)", "")
    expr = expr.replace("||", " or ")
    # Every ternary is fully parenthesised as ((c) ? 0xNN : 0), so
    # `c and 0xNN or 0` is the same value.
    expr = re.sub(r" \? (0x[0-9A-F]+) : 0",
                  lambda m: " and " + m.group(1) + " or 0", expr)
    cases = [(2.0, 1.0, 0x02), (1.0, 2.0, 0x03), (1.0, 1.0, 0x42),
             (math.nan, 1.0, 0x47)]
    for a, b, want in cases:
        got = eval(expr, {"_fca": a, "_fcb": b})
        assert got == want, (a, b, hex(got), hex(want), expr)


def test_fpu_unordered_reads_below_equal_and_parity():
    # fcomi/fucomi/sahf set ZF, PF and CF on an unordered compare. The
    # conditions read those bits from the _fa snapshot, so a later x87 compare
    # or an AH edit before sahf cannot change them.
    from tools.recomp.lifter import _make_condition
    ja = _make_condition("ja", "fucomip", [])[0]
    jb = _make_condition("jb", "fucomip", [])[0]
    jp = _make_condition("jp", "sahf", [])[0]
    assert "(!(_fa & 1u) && !(_fa & 0x40u))" in ja, ja
    assert "(_fa & 1u)" in jb, jb
    assert "(_fa & 4u)" in jp, jp
    assert "g_fp_cmp" not in ja + jb + jp


def _ax():
    return _Op("ax", size=2)


def test_sahf_snapshots_ah_and_keeps_the_previous_of():
    # fnstsw ax; sahf: the snapshot is AH's SF/ZF/AF/PF/CF; OF comes from
    # the setter before sahf (none here, so 0), put in _fb first.
    block = _Block([_Insn("fnstsw", [_ax()], "ax"), _Insn("sahf", [], "")])
    out = "\n".join(lift_basic_block(Lifter(), block)[0])
    assert "_fb = (0) != 0; /* sahf keeps OF */" in out, out
    assert "_fa = ((eax >> 8) & 0xD5u) | ((_fb & 1u) << 11); /* sahf */" in out, out
    assert out.index("sahf keeps OF") < out.index("0xD5u"), out
    assert "/* sahf - store AH" not in out, out


def test_sahf_writes_cf_only_when_the_function_tracks_it():
    lifter = Lifter()
    assert not any("_cf" in s for s in lifter.lift_instruction(_Insn("sahf", [], "")))
    lifter.needs_cf = True
    assert "_cf = _fa & 1u;" in lifter.lift_instruction(_Insn("sahf", [], ""))


def test_fcomi_snapshots_eflags_and_leaves_the_status_word():
    out = "\n".join(Lifter().lift_instruction(
        _Insn("fcomi", [_Op("st(1)")], "st(1)")))
    assert "_fa = RECOMP_FCMP_CC(RECOMP_FCMP(fp_top(), fp_st1())) >> 8;" in out, out
    assert "g_fp_cmp" not in out and "g_fp_cc" not in out, out


def test_signed_conditions_after_sahf_and_fcomi():
    from tools.recomp.lifter import _make_condition
    for setter in ("sahf", "fcomi"):
        for jcc in ("js", "jns", "jo", "jno", "jl", "jge", "jle", "jg"):
            r = _make_condition(jcc, setter, [])
            assert r is not None and "_fa" in r[0], (setter, jcc, r)


def test_comiss_signed_conditions_are_constants_or_zf():
    # comiss clears SF and OF: js/jo/jl never, jns/jno/jge always, and jle/jg
    # reduce to ZF. These fell back to the never-assigned _flags.
    from tools.recomp.lifter import _make_condition
    import math
    ops = [_Op("xmm0"), _Op("xmm1")]
    assert _make_condition("js", "comiss", ops)[0] == "0"
    assert _make_condition("jge", "ucomiss", ops)[0] == "1"

    def ev(jcc, a, b):
        e = _make_condition(jcc, "ucomiss", ops)[0]
        e = e.replace("||", " or ").replace("&&", " and ").replace("!(", "not (")
        return bool(eval(e, {"_fca": a, "_fcb": b}))
    # jle = ZF = equal or unordered; jg = !ZF.
    assert ev("jle", 1.0, 1.0) and ev("jle", math.nan, 1.0) and not ev("jle", 2.0, 1.0)
    assert ev("jg", 2.0, 1.0) and ev("jg", 1.0, 2.0) and not ev("jg", math.nan, 1.0)


def test_overflow_setcc_and_cmovcc_after_fcomi_are_lifted():
    for insn in (_Insn("seto", [_Op("al", size=1)], "al"),
                 _Insn("cmovno", [_Op("eax"), _Op("ecx")], "eax, ecx")):
        block = _Block([_Insn("fcomi", [_Op("st(1)")], "st(1)"),
                        insn])
        out = "\n".join(lift_basic_block(Lifter(), block)[0])
        assert "((_fa >> 11) & 1u)" in out, out


def test_sahf_only_function_declares_the_snapshot():
    # fnstsw ax; sahf; ret -- no cmp/test anywhere, yet the sahf lift writes
    # _fa and reads _fb, so the translator must declare them.
    from tools.recomp import config
    from tools.recomp.translator import FunctionTranslator
    from tools.conformance.corpus_run import _CONFIG_GLOBALS
    base = 0x00011000
    image = bytes.fromhex("DFE09EC3")
    saved = {k: getattr(config, k) for k in _CONFIG_GLOBALS if hasattr(config, k)}
    try:
        config._install(
            [config.Section(".text", base, len(image), 0, len(image), True)],
            entry_point=base, kernel_thunk_addr=base, origin="sahf-test")
        db = {base: {"start": f"0x{base:08X}", "end": base + len(image),
                     "_addr": base, "size": len(image)}}
        c = FunctionTranslator(image, db).translate_function(base, db[base])
    finally:
        for k, v in saved.items():
            setattr(config, k, v)
    assert "0xD5u" in c, c
    assert "uint32_t _fa = 0, _fb = 0;" in c, c


def test_comiss_unordered_sets_zf_pf_cf():
    # Same rule after comiss/ucomiss: jb, je and jp are taken on NaN, ja is not.
    from tools.recomp.lifter import _make_condition
    import math
    def ev(jcc, a, b):
        e = _make_condition(jcc, "ucomiss", [_Op("xmm0"), _Op("xmm1")])[0]
        e = e.replace("||", " or ").replace("&&", " and ").replace("!(", "not (")
        return bool(eval(e, {"_fca": a, "_fcb": b}))
    for jcc, want in (("ja", False), ("jae", False), ("jb", True),
                      ("jbe", True), ("je", True), ("jne", False),
                      ("jp", True), ("jnp", False)):
        assert ev(jcc, math.nan, 1.0) == want, jcc
    assert ev("jne", 1.0, 2.0) and not ev("jp", 1.0, 2.0)


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print("ok", name)
