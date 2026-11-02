"""Guest registers in C locals inside chosen lifted functions.

Lifted code names the guest registers eax..edi and esp as fields of the
thread-local g_cpu, and xmm0..7 as thread-locals of their own. Every store to
guest memory may alias them as far as the compiler knows, so after each one it
reloads every register it is about to use, and keeps none of them in a host
register across a statement. Measured on Forza Motorsport (profil-race5..9):
the hottest lifted function went from 385 thread-local accesses to 58 and from
721 instructions to 343, and the race from 40 ms of work a frame to 26 ms --
20 fps to 30 -- with the 400 hottest functions done this way.

For each chosen function this rewrites the translated text so that

* the registers are locals, read from g_cpu on entry,
* they are written back before every call (direct, guarded or refused
  indirect, tail jump) and every return, and read again after every call,
* the call itself runs with the names mapped to the globals again, so a macro
  that sets eax or g_esp itself (RECOMP_ICALL_SAFE on a refused target) sets
  what the callee and the reload see.

A function is left as it is when it uses anything this cannot see through:
setjmp/longjmp, MMX registers, a register named by its global outside the
indirect-call capture, the ABI checker, an HLE hook, or a call to a C function
not known to leave the guest registers alone (PURE below). The x87 stack and
everything else in g_cpu stay where they are.

Which functions: a JSON list of addresses (tools.recomp --regpromote FILE,
scripts/recompile.py takes config/regpromote/<TITLEID>.json), or "all".
Cold code gains nothing, so a title lists its profile's hottest functions.
"""
import json
import re

GPRS = ('eax', 'ecx', 'edx', 'esp', 'ebx', 'esi', 'edi')

# Called from lifted code without touching the guest register file.
PURE = frozenset('''
MEM8 MEM16 MEM32 MEM64 SMEM8 SMEM16 SMEM32 SMEM64 MEMF MEMD XBOX_PTR
fp_push fp_pop fp_top fp_st fp_st1 RECOMP_FP_PC RECOMP_FCMP RECOMP_FCMP_CC
LO8 HI8 SET_LO8 SET_HI8 LO16 SET_LO16 ZX8 ZX16 SX8 SX16 RECOMP_PARITY8
RECOMP_DF_STEP RECOMP_BACKEDGE RECOMP_IDIV_CHECK RECOMP_DIV_CHECK
PUSH32 POP32 PUSH16 POP16
fabs fabsf sqrt sqrtf atan2 atan2f sin cos sinf cosf floor ceil floorf ceilf
tan tanf log2 exp2 ldexp memcpy memmove memset
recomp_cvtss2si recomp_cvttss2si recomp_fp_round24 recomp_parity8
recomp_frndint recomp_fist xbox_ReadTimeStampCounter
if while for switch sizeof return
void uint32_t int32_t uint16_t int16_t uint8_t int8_t uint64_t int64_t
float double int
'''.split())

# Calls that leave the function: everything around them is synchronised.
_CALL = re.compile(r'\b(RECOMP_ABI_CALL_POP|RECOMP_ABI_CALL|RECOMP_ICALL_SAFE_AT_CC|'
                   r'RECOMP_ICALL_SAFE_AT|RECOMP_ICALL_SAFE_CC|RECOMP_ICALL_SAFE|'
                   r'RECOMP_ITAIL|sub_[0-9A-F]+\w*)\s*\(')
_REFUSE = re.compile(r'setjmp|longjmp|\bg_(?:eax|ecx|edx|esp|ebx|esi|edi|xmm\d)\b|'
                     r'\bmm[0-7]\b|RECOMP_ABI_CHECK|HLE_|RECOMP_ICALL_FLAT|RECOMP_DISPATCH')
_ICALL_CAPTURE = 'uint32_t _icall_esp = g_esp;'


def load(path):
    """The functions to promote: a set of addresses, or "all"."""
    if not path:
        return None
    with open(path, 'r', encoding='utf-8') as fh:
        entries = json.load(fh)
    if entries == 'all':
        return 'all'
    if isinstance(entries, dict):
        entries = list(entries.keys())
    out = set()
    for e in entries:
        if isinstance(e, dict):
            e = e.get('start') or e.get('address')
        if isinstance(e, str):
            e = e[4:] if e.startswith('sub_') else e
            out.add(int(e, 16))
        else:
            out.add(int(e))
    return out


def _strip_comments(s):
    s = re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', s)


def _match_paren(s, i):
    depth = 0
    for j in range(i, len(s)):
        if s[j] == '(':
            depth += 1
        elif s[j] == ')':
            depth -= 1
            if depth == 0:
                return j
    raise ValueError('unbalanced parentheses')


def _unknown_calls(code):
    code = re.sub(r'^\s*#.*$', '', _strip_comments(code), flags=re.M)
    out = set()
    for m in re.finditer(r'\b([A-Za-z_]\w*)\s*\(', code):
        n = m.group(1)
        if n in PURE or n.startswith(('XMM_', 'CMP_', 'TEST_')) or _CALL.match(n + '('):
            continue
        out.add(n)
    return out


def _defines(xmms, target):
    s = ''.join('#undef %s\n#define %s %s_%s\n' % (r, r, target, r) for r in GPRS)
    return s + ''.join('#undef xmm%s\n#define xmm%s %s_xmm%s\n' % (x, x, target, x)
                       for x in xmms)


def promote_body(name, body):
    """body: 'void NAME(void)\\n{\\n ... \\n}\\n'. Returns (new body or None, why)."""
    body = body.replace(_ICALL_CAPTURE, 'uint32_t _icall_esp = esp;')
    plain = _strip_comments(body)
    why = _REFUSE.search(plain)
    if why:
        return None, 'refused: ' + why.group(0)
    unknown = _unknown_calls(body)
    if unknown:
        return None, 'unknown calls: ' + ' '.join(sorted(unknown))
    xmms = sorted(set(re.findall(r'\bxmm([0-7])\b', plain)))
    open_i = body.index('{\n') + 2
    head, rest = body[:open_i], body[open_i:]
    close_i = rest.rstrip().rfind('}')
    inner, tail = rest[:close_i], rest[close_i:]
    to_global, to_local = _defines(xmms, 'g'), _defines(xmms, 'l')

    res, i, ncalls = [], 0, 0
    while True:
        m = _CALL.search(inner, i)
        if not m:
            res.append(inner[i:])
            break
        line_start = inner.rfind('\n', 0, m.start()) + 1
        seg = inner[line_start:m.start()]
        if '/*' in seg and '*/' not in seg[seg.rfind('/*'):]:
            res.append(inner[i:m.end()])
            i = m.end()
            continue
        j = _match_paren(inner, m.end() - 1) + 1
        while j < len(inner) and inner[j] in ' \t':
            j += 1
        if j >= len(inner) or inner[j] != ';':
            return None, 'call not a statement'
        j += 1
        res.append(inner[i:m.start()])
        res.append('{ RP_OUT();\n' + to_global + inner[m.start():j] + '\n' +
                   to_local + 'RP_IN(); }')
        ncalls += 1
        i = j
    inner = ''.join(res)
    inner, nret = re.subn(r'\breturn\s*;', '{ RP_OUT(); return; }', inner)

    out_parts = ['g_cpu.r_%s = l_%s;' % (r, r) for r in GPRS] + \
                ['g_xmm%s = l_xmm%s;' % (x, x) for x in xmms]
    in_parts = ['l_%s = g_cpu.r_%s;' % (r, r) for r in GPRS] + \
               ['l_xmm%s = g_xmm%s;' % (x, x) for x in xmms]
    decl = '    uint32_t ' + ', '.join('l_%s = g_cpu.r_%s' % (r, r) for r in GPRS) + ';\n'
    decl += ''.join('    RecompXmm l_xmm%s = g_xmm%s;\n' % (x, x) for x in xmms)
    pre = ('/* regpromote: guest registers in locals inside this function */\n'
           '#define RP_OUT() do { %s } while (0)\n'
           '#define RP_IN() do { %s } while (0)\n' % (' '.join(out_parts), ' '.join(in_parts)))
    pre += to_local
    post = '\n' + to_global + '#undef RP_OUT\n#undef RP_IN\n'
    new = pre + head + decl + inner + '    RP_OUT();\n' + tail + post
    return new, 'ok (%d calls, %d returns, xmm %s)' % (ncalls, nret, ''.join(xmms) or '-')


def promote(code, name):
    """Promote function NAME inside a translated chunk of text.

    Returns (text, why); text is unchanged when the function is left alone."""
    m = re.search(r'^void %s\(void\)\n\{\n' % re.escape(name), code, re.M)
    if not m:
        return code, 'not found'
    end = code.find('\n}\n', m.end())
    if end < 0:
        return code, 'no end'
    end += 3
    new, why = promote_body(name, code[m.start():end])
    if new is None:
        return code, why
    return code[:m.start()] + new + code[end:], why
