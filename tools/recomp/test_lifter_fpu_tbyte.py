"""fld and fstp with a 10-byte (tbyte) operand.

The CRT's libm loads its constants with `fld tbyte ptr [k]`. fld read any
operand that was not 4 or 8 bytes as a float, so 1.0L (significand
0x8000000000000000, exponent 0x3FFF) loaded as 0.0; fstp tbyte was a bare
comment, which stored nothing and left the value on the stack.

The lifted forms are checked here as text, and recomp_f80_load/_store are
compiled and run against bit patterns worked out by hand (skipped with no C
compiler). tools.recomp.fpdiff's fld_m80 case checks both against the CPU.
"""

import os
import re
import shutil
import subprocess
import tempfile
import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter

_HEADER = os.path.join(os.path.dirname(__file__), "..", "..",
                       "templates", "runtime", "recomp_types.h")


def _lift(mnemonic):
    insn = Instruction(0, 3, mnemonic, "tbyte ptr [esp + 0x10]", "")
    insn.operands = [Operand(type="mem", mem_base="esp", mem_disp=0x10,
                             mem_size=10)]
    return "\n".join(Lifter().lift_instruction(insn))


class TbyteLiftTest(unittest.TestCase):
    def test_fld_tbyte_converts_the_80_bit_value(self):
        out = _lift("fld")
        self.assertIn("fp_push(recomp_f80_load(", out)
        self.assertIn("MEM32((esp + 0x10) + 4) << 32", out)
        self.assertIn("MEM16((esp + 0x10) + 8)", out)
        self.assertNotIn("MEMF(", out)

    def test_fstp_tbyte_stores_ten_bytes_and_pops(self):
        out = _lift("fstp")
        self.assertIn("recomp_f80_store(fp_top(), &_m)", out)
        self.assertIn("MEM32(esp + 0x10) = (uint32_t)_m;", out)
        self.assertIn("MEM32((esp + 0x10) + 4) = (uint32_t)(_m >> 32);", out)
        self.assertIn("MEM16((esp + 0x10) + 8) = _se;", out)
        self.assertEqual(out.count("fp_pop();"), 1, out)


def _helpers():
    """recomp_f80_load and recomp_f80_store, cut out of the runtime header so
    the check compiles without the rest of the register model."""
    text = open(_HEADER, encoding="utf-8").read()
    m = re.search(r"static inline double recomp_f80_load.*?\n}\n"
                  r"static inline uint16_t recomp_f80_store.*?\n}\n", text, re.S)
    assert m, "recomp_f80_load/_store not found in recomp_types.h"
    return m.group(0)


_CHECK = r"""
#include <stdint.h>
#include <stdio.h>
#include <math.h>
@HELPERS@
static int bad;
static void load(uint64_t mant, uint16_t se, double want) {
    double got = recomp_f80_load(mant, se);
    if (!(got == want && signbit(got) == signbit(want)) && !(got != got && want != want)) {
        printf("load %016llX %04X: %.17g, want %.17g\n",
               (unsigned long long)mant, se, got, want);
        bad = 1;
    }
}
static void store(double v, uint64_t want_mant, uint16_t want_se) {
    uint64_t mant = 0; uint16_t se = recomp_f80_store(v, &mant);
    if (mant != want_mant || se != want_se) {
        printf("store %.17g: %016llX %04X, want %016llX %04X\n", v,
               (unsigned long long)mant, se, (unsigned long long)want_mant, want_se);
        bad = 1;
    }
    if (v == v && recomp_f80_load(mant, se) != v) { printf("round trip %.17g\n", v); bad = 1; }
}
int main(void) {
    load(0x8000000000000000ull, 0x3FFF, 1.0);
    load(0x8000000000000000ull, 0xBFFF, -1.0);
    load(0xA000000000000000ull, 0x4000, 2.5);
    load(0xC90FDAA22168C235ull, 0x4000, 3.141592653589793);   /* fldpi, rounded */
    load(0, 0, 0.0);
    load(0, 0x8000, -0.0);
    load(0x8000000000000000ull, 0x7FFF, INFINITY);
    load(0x8000000000000000ull, 0xFFFF, -INFINITY);
    load(0xC000000000000000ull, 0x7FFF, NAN);
    store(1.0, 0x8000000000000000ull, 0x3FFF);
    store(-2.5, 0xA000000000000000ull, 0xC000);
    store(0.1, 0xCCCCCCCCCCCCD000ull, 0x3FFB);
    store(0.0, 0, 0);
    store(-0.0, 0, 0x8000);
    store(INFINITY, 0x8000000000000000ull, 0x7FFF);
    store(4.9406564584124654e-324, 0x8000000000000000ull, 0x3BCD); /* double's least denormal */
    return bad;
}
"""


class TbyteHelpersCompiledTest(unittest.TestCase):
    def test_helpers_match_the_80_bit_format(self):
        src = _CHECK.replace("@HELPERS@", _helpers())
        with tempfile.TemporaryDirectory() as d:
            c = os.path.join(d, "f80.c")
            with open(c, "w", newline="\n") as f:
                f.write(src)
            cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
            if cc:
                exe = os.path.join(d, "f80")
                built = subprocess.run([cc, "-O2", c, "-o", exe, "-lm"],
                                       capture_output=True, text=True)
            else:
                from tools.conformance import __main__ as runner
                vcvars = runner._find_vcvars()
                if vcvars is None:
                    self.skipTest("no C compiler")
                exe = os.path.join(d, "f80.exe")
                built = runner._cl(vcvars, d, "/O2 f80.c /Fef80.exe")
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([exe], capture_output=True, text=True, timeout=30)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == "__main__":
    unittest.main()
