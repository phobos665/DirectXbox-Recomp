"""regpromote: guest registers in C locals must behave as the globals did.

The text checks pin the shape (sync around every call and return, the global
names back for the call itself); the run check compiles a promoted function
against a stand-in register file and compares what it leaves in memory and
in g_cpu with the unpromoted original.
"""
import os
import shutil
import subprocess
import tempfile

from . import regpromote

BODY = """void sub_00001000(void)
{
    esp = esp - 8;
    MEM32(esp) = eax;
    eax = eax + MEM32(esp + 0xC);
    PUSH32(esp, 0x00001010u); RECOMP_ABI_CALL_POP(0x00002000u, sub_00002000, 0u); /* call */
    ecx = eax * 3;
    { uint32_t _icall_esp = g_esp;
    { uint32_t _icall_target = MEM32(0x40); PUSH32(esp, 0x00001020u); RECOMP_ICALL_SAFE_AT(_icall_target, _icall_esp, 0x0000101Bu); } /* indirect call */
    }
    MEM32(0x44) = ecx + eax;
    esp = esp + 8;
    esp += 4; return; /* ret */
}
"""

CALLEE = """void sub_00002000(void)
{
    eax = eax ^ 0x55u;
    esp += 4; return;
}
"""

HEADER = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint8_t g_ram[0x10000];
#define XBOX_PTR(a) ((uintptr_t)g_ram + (uint32_t)(a))
#define MEM32(a) (*(volatile uint32_t *)XBOX_PTR(a))
struct recomp_cpu { uint32_t r_eax, r_ecx, r_edx, r_esp, r_ebx, r_esi, r_edi; };
static struct recomp_cpu g_cpu;
typedef struct { float f[4]; } RecompXmm;
#define g_eax g_cpu.r_eax
#define g_ecx g_cpu.r_ecx
#define g_edx g_cpu.r_edx
#define g_esp g_cpu.r_esp
#define g_ebx g_cpu.r_ebx
#define g_esi g_cpu.r_esi
#define g_edi g_cpu.r_edi
#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define esp g_esp
#define ebx g_ebx
#define esi g_esi
#define edi g_edi
#define PUSH32(sp, v) do { uint32_t _pv = (uint32_t)(v); (sp) -= 4; MEM32(sp) = _pv; } while (0)
#define RECOMP_ABI_CALL_POP(va, fn, pop) (fn)()
typedef void (*recomp_func_t)(void);
void sub_00002000(void);
static void sub_00003000(void) { g_eax += 7; g_esp += 4; }
#define RECOMP_ICALL_SAFE_AT(t, saved, site) do { \
    if ((t) == 0x3000u) sub_00003000(); else { g_esp = (saved); eax = 0; } } while (0)
"""

MAIN = r"""
int main(void) {
    uint32_t out[2][4];
    for (int k = 0; k < 2; k++) {
        for (int arm = 0; arm < 2; arm++) {
            memset(g_ram, 0, sizeof g_ram); memset(&g_cpu, 0, sizeof g_cpu);
            g_esp = 0x8000; g_eax = 5; MEM32(0x8004) = 11; MEM32(0x40) = arm ? 0x3000u : 0x9999u;
            if (k == 0) sub_00001000_orig(); else sub_00001000();
            out[k][arm * 2] = MEM32(0x44); out[k][arm * 2 + 1] = g_eax ^ (g_esp << 8) ^ (g_ecx << 16);
        }
    }
    printf("%u %u %u %u | %u %u %u %u\n", out[0][0], out[0][1], out[0][2], out[0][3],
           out[1][0], out[1][1], out[1][2], out[1][3]);
    return memcmp(out[0], out[1], sizeof out[0]) != 0;
}
"""


def test_shape():
    new, why = regpromote.promote(BODY, "sub_00001000")
    assert why.startswith("ok (2 calls, 1 returns"), why
    assert "_icall_esp = esp;" in new and "_icall_esp = g_esp;" not in new
    assert new.count("RP_OUT();") == 4          # two calls, one return, the end
    assert "#define eax g_eax\nRECOMP_ABI_CALL_POP" not in new
    assert "#define edi g_edi\nRECOMP_ICALL_SAFE_AT" in new


def test_refuses():
    for bad in ("    g_eax = 1;\n", "    mm0 = mm1;\n", "    foo(eax);\n"):
        body = BODY.replace("    ecx = eax * 3;\n", bad)
        new, why = regpromote.promote(body, "sub_00001000")
        assert new == body and not why.startswith("ok"), why


def test_runs_the_same():
    cc = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        return
    promoted, why = regpromote.promote(BODY, "sub_00001000")
    assert why.startswith("ok"), why
    orig = BODY.replace("sub_00001000(void)", "sub_00001000_orig(void)")
    src = HEADER + CALLEE + orig + promoted + MAIN
    with tempfile.TemporaryDirectory() as d:
        c, exe = os.path.join(d, "t.c"), os.path.join(d, "t")
        open(c, "w").write(src)
        subprocess.run([cc, "-O2", "-o", exe, c], check=True)
        r = subprocess.run([exe], capture_output=True, text=True)
        assert r.returncode == 0, r.stdout


if __name__ == "__main__":
    test_shape()
    test_refuses()
    test_runs_the_same()
    print("ok")
