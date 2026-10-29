/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t: not in stdlib.h off MSVC */
#include <stdlib.h>   /* getenv, exit: the spin verdict below */
#include <string.h>   /* strcmp: RECOMP_ICALL_FATAL */

/* One diagnostic report, one piece.
 *
 * Each report below is a dozen fprintf calls and the CRT takes its lock per
 * call, so with more than one guest thread running the lines interleave in
 * the middle of a report. Dino Crisis 3 runs six workers once it is past its
 * menus, and its log came out like this:
 *
 *   callers: 0x004DB424 <- 0x002705B4 <- 0x002705B4[ICALL] unresolved jump
 *   target 0x41B0B870 -- 1 time(s) (total calls: 6930990)
 *
 * -- two threads' reports spliced together, with a caller list that stops
 * mid-sentence and an address that belongs to neither line as read. These
 * diagnostics exist to be read during exactly the multi-threaded bring-up
 * that breaks them.
 *
 * The CRT exposes the lock these calls were already taking one at a time, so
 * hold it across the whole report instead. */
#if defined(_MSC_VER)
#  define RECOMP_DIAG_LOCK()   _lock_file(stderr)
#  define RECOMP_DIAG_UNLOCK() _unlock_file(stderr)
#elif defined(__unix__) || defined(__APPLE__)
#  define RECOMP_DIAG_LOCK()   flockfile(stderr)
#  define RECOMP_DIAG_UNLOCK() funlockfile(stderr)
#else
#  define RECOMP_DIAG_LOCK()   ((void)0)
#  define RECOMP_DIAG_UNLOCK() ((void)0)
#endif

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

/* The guest registers (g_eax, g_esp, g_icall_saved_esp, ...) are fields of
 * one thread-local struct, declared once in recomp_cpu.h, which the lifter
 * copies into gen/ beside the code it describes. Take them from there; never
 * redeclare one here. They used to be redeclared by hand, and a declaration
 * without the thread-local storage class linked and silently read other
 * storage, so every read got 0 -- which is why the log below once reported
 * no call site: not because the guest esp was stale, but because this file
 * was not reading the guest esp at all. The names are macros now, so a
 * redeclaration no longer compiles. */
#include "recomp/gen/recomp_cpu.h"

/* g_eax: recomp_cpu.h. */
/* The guest stack pointer. At the moment an indirect call is refused, the
 * caller has already pushed its guest return address, so the top of the
 * guest stack is the call site -- the one thing the old log did not say. */
/* g_esp: recomp_cpu.h. */
extern ptrdiff_t g_xbox_mem_offset;
/* The lifted code sections, so a value on the guest stack can be told
 * apart from data when naming the callers of a refused call. */
extern uint32_t g_xbox_code_lo;
extern uint32_t g_xbox_code_hi;
/* The esp the dispatch macro captured. Not g_esp, which is stale by the time
 * a refused call is reported. Zero when the title's generated header predates
 * this, and the log then says it has no callers rather than inventing them. */
/* g_icall_saved_esp: recomp_cpu.h. */
/* Which dispatch form was refused: 0 unknown, 1 call, 2 jump. Unknown
 * means this title was lifted before the macros published it. */
/* g_icall_dispatch_form: recomp_cpu.h. */

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    /*
     * Rate-limited per target, reporting at 1, 10, 100 ...
     *
     * An unresolved target inside a loop is the normal case, not the rare
     * one: Mortal Kombat: Deadly Alliance produced 46843 of these for a
     * single address in one 30-second run, seventeen lines each. That buries
     * every other diagnostic in the log and says nothing the first report did
     * not. The progression is the useful part -- one line says a target was
     * never identified, a run of them says the title is stuck on it.
     */
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }

    RECOMP_DIAG_LOCK();
    fprintf(stderr, "[ICALL] unresolved %starget 0x%08X -- %llu time(s) "
                    "(total calls: %llu)\n",
            g_icall_dispatch_form == 1 ? "call " :
            g_icall_dispatch_form == 2 ? "jump " : "",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    if (g_icall_dispatch_form == 2 && hits[i] == 1)
        fprintf(stderr, "  a jump, not a call: if this address is inside a "
                        "function rather than at its start, the guest is doing "
                        "its own control flow (a coroutine, a longjmp, or a "
                        "switch arm) and seeding it as a function will not "
                        "help\n");

    /*
     * Who called it. The lifted caller pushed its return address on the guest
     * stack, so code-range values above esp name the chain -- the same scan
     * the memory watchpoints use. It is a heuristic: stale return addresses
     * from earlier frames show up too, and the first entry is the reliable
     * one. It is still the difference between an address with no context and
     * a function to open, which is what an unresolved target needs.
     */
    {
        const uint8_t *stack = (const uint8_t *)g_xbox_mem_offset
                             + g_icall_saved_esp;
        int shown = 0;
        fprintf(stderr, "  callers:");
        for (i = 0; g_xbox_mem_offset && g_icall_saved_esp
                    && i < 160 && shown < 6; i++) {
            uint32_t v;
            memcpy(&v, stack + (size_t)i * 4, sizeof v);
            if (v >= g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "%s 0x%08X", shown ? " <-" : "", v);
                shown++;
            }
        }
        if (!shown)
            fprintf(stderr, " (none on the stack)");
        fprintf(stderr, "\n");
    }

    /* The recent-target ring buffer, once per address. What ran just before
     * an unresolved call is context for the first report and noise after. */
    if (hits[i] == 1) {
        fprintf(stderr, "  Recent ICALL targets:\n");
        for (i = 0; i < 16; i++) {
            int idx = (g_icall_trace_idx - 16 + i) & 15;
            if (g_icall_trace[idx])
                fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
        }
    }
    RECOMP_DIAG_UNLOCK();
    fflush(stderr);
}
/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va, uint32_t saved_esp)
{
    /* A power of ten, because the rate limiter above only reaches this
     * function body at 1, 10, 100 ... and the verdict has to land on one of
     * those. A hundred thousand skips of one target is unambiguous: no title
     * makes progress through that. */
    enum { SLOTS = 16, SPIN_VERDICT = 100000 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    static int said_it;   /* the verdict below is said once, not once per target */
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    {
        /* The call site, read off the guest stack. Without it the log
         * says a wild pointer was skipped but not by whom, and the
         * caller is the only thing that leads anywhere.
         *
         * It comes from the esp the dispatch macro captured, not from
         * g_esp. The lifted call site pushes its return address onto the
         * *local* esp and only syncs g_esp at certain points, so g_esp is
         * stale here -- it read 0 exactly when a null target most needed
         * explaining, which sent three rounds of Jet Set Radio Future
         * chasing inferences instead of a call site. saved_esp is the value
         * before that push, so the return address is the dword below it.
         *
         * Only for the call form. A tail jump pushes no return address, so
         * that dword is whatever the frame happened to leave there: Tony
         * Hawk's Pro Skater 2X reported "from 0x00000008" and then "from
         * 0x00000004" for the same million skipped calls, and both are
         * plainly not code. Printing a number that cannot be an address as
         * though it were the caller is worse than printing nothing, because
         * it is the one field a reader goes to next.
         *
         * So: name the form, and where the top of the stack cannot answer,
         * scan for code-range values the way the unresolved-target logger
         * above does. Stale return addresses from earlier frames come back
         * too and the first is the reliable one, but a list of real code
         * addresses is a place to start and 0x00000008 is not. */
        uint32_t caller = 0;
        if (g_icall_dispatch_form != 2 && saved_esp >= 4 && g_xbox_mem_offset)
            caller = *(const uint32_t *)((const uint8_t *)g_xbox_mem_offset
                                         + (saved_esp - 4));
        if (caller < g_xbox_code_lo || caller >= g_xbox_code_hi)
            caller = 0;
        RECOMP_DIAG_LOCK();
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped "
                        "%llu time(s) via a %s",
                va, (unsigned long long)hits[i],
                g_icall_dispatch_form == 2 ? "jump" : "call");
        if (caller)
            fprintf(stderr, " from 0x%08X", caller);
        fprintf(stderr, " (null or wild function pointer, at call #%llu)\n",
                (unsigned long long)g_icall_count);

        /* The stack scan, once per address: the same heuristic the
         * unresolved-target logger uses, and the only thing that names a
         * caller when the return address is not where it should be. */
        if (hits[i] == 1) {
            const uint8_t *stack = (const uint8_t *)g_xbox_mem_offset
                                 + saved_esp;
            int shown = 0, k;
            fprintf(stderr, "  callers:");
            for (k = 0; g_xbox_mem_offset && saved_esp
                        && k < 160 && shown < 6; k++) {
                uint32_t v;
                memcpy(&v, stack + (size_t)k * 4, sizeof v);
                if (v >= g_xbox_code_lo && v < g_xbox_code_hi) {
                    fprintf(stderr, "%s 0x%08X", shown ? " <-" : "", v);
                    shown++;
                }
            }
            if (!shown)
                fprintf(stderr, " (none on the stack)");
            fprintf(stderr, "\n");
        }

        /* RECOMP_ICALL_FATAL=1: fault here instead of skipping, so the crash
         * handler prints the guest call stack that reached this call.
         *
         * The caller printed above is read from the top of the guest stack,
         * which is right only when the lifted call site pushed a return
         * address there and nothing has since moved esp. When it reads 0 --
         * exactly the case where a null target most needs explaining -- the
         * line says a wild pointer was skipped and nothing about by whom. A
         * deliberate fault costs the run and buys the backtrace. */
        {
            static int fatal = -1;
            if (fatal < 0) {
                const char *v = getenv("RECOMP_ICALL_FATAL");
                fatal = v && *v && strcmp(v, "0") != 0;
            }
            if (fatal) {
                fprintf(stderr, "[ICALL] RECOMP_ICALL_FATAL: faulting here for "
                                "a backtrace\n");
                fflush(stderr);
                *(volatile int *)0 = 1;
            }
        }
    }

    /* Past a certain count this stops being a warning and becomes a verdict.
     *
     * A skipped call sets eax to 0 and returns. Zero is a fine answer for a
     * null function pointer in most code -- it reads as NULL, false, or
     * nothing -- but it is also S_OK, so in COM-shaped code a loop that
     * repeats while the result is non-negative can never leave. Panzer
     * Dragoon Orta hung exactly there, three billion skips in forty seconds,
     * with no frame ever presented.
     *
     * The value is deliberately left alone: there is no return value that is
     * right for both conventions, and guessing failure would break the
     * callers for which zero is correct. What can be fixed is the silence.
     * Say plainly that the title is hung, once, and say what to do about it.
     */
    if (hits[i] == SPIN_VERDICT && !said_it) {
        said_it = 1;
        fprintf(stderr,
            "[ICALL] ^^ this is a hang, not slow progress. One target has been\n"
            "        skipped %d times. The skip returns eax = 0, which is also\n"
            "        S_OK, so a loop testing for a non-negative result will\n"
            "        never exit. Try `py -3 -m tools.seed_from_log <log>` to\n"
            "        recover the target as a function, and read\n"
            "        docs/technical/memory-watchpoints.md for who wrote the\n"
            "        pointer. Set RECOMP_ICALL_SPIN_FATAL=1 to stop here\n"
            "        instead of spinning.\n", SPIN_VERDICT);
        fflush(stderr);
        if (getenv("RECOMP_ICALL_SPIN_FATAL")) {
            fprintf(stderr, "[ICALL] RECOMP_ICALL_SPIN_FATAL is set; exiting.\n");
            fflush(stderr);
            exit(3);
        }
    }
    RECOMP_DIAG_UNLOCK();
    fflush(stderr);
}



/* ── Account creation, watched (RECOMP_XUACS_LOG=<file>) ─────────────────
 *
 * This project exists to learn what the dashboard sends Insignia to make an
 * account (docs/technical/xbox-live-accounts.md). Every Xbox Live web request
 * leaves through sub_001152B4 (service, ?, path, reply, reply size, ?, ?,
 * body, body size, timeout, ...), before the security gateway encrypts it;
 * each request object is destroyed once it is answered, its reply still in
 * its buffer: sub_0012CF27 for the table-driven ones (path at
 * [[obj+0x160]+8], status +0x168, reply +0x174, its size +0x178, bytes
 * received +0x17C), sub_00120CB6 for reservename's (reply +0x174, size
 * +0x170).
 *
 * The log holds the new account's key and whatever is typed into the forms,
 * so it is a file of its own, made private, and only when asked for. Never
 * commit one. */
extern void sub_001152B4_gen(void);
extern void sub_0012CF27_gen(void);
extern void sub_00120CB6_gen(void);

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#define XU_MEM32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define XU_PTR(va)   ((const uint8_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

static FILE *xu_log(void)
{
    static FILE *f;
    static int tried;
    const char *path;

    if (tried)
        return f;
    tried = 1;
    path = getenv("RECOMP_XUACS_LOG");
    if (!path || !*path)
        return NULL;
#if defined(_WIN32)
    f = fopen(path, "a");
#else
    {
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
        f = fd >= 0 ? fdopen(fd, "a") : NULL;
    }
#endif
    if (f)
        fprintf(stderr, "[XUACS] logging account requests to %s (private: holds keys)\n", path);
    return f;
}

/* A guest string, if it is a short printable one; else "". */
static const char *xu_str(uint32_t va, char *out, size_t n)
{
    size_t i = 0;

    out[0] = 0;
    if (va < 0x10000 || va >= 0x80000000u)
        return out;
    for (; i + 1 < n; i++) {
        uint8_t c = XU_PTR(va)[i];
        if (!c)
            break;
        if (c < 0x20 || c >= 0x7F) {
            out[0] = 0;
            return out;
        }
        out[i] = (char)c;
    }
    out[i] = 0;
    return out;
}

static void xu_dump(FILE *f, const char *what, uint32_t va, uint32_t len)
{
    uint32_t i, j;

    if (va < 0x10000 || va >= 0x80000000u || len > 0x4000) {
        fprintf(f, "  %s: (at 0x%08X, %u bytes: not dumped)\n", what, va, len);
        return;
    }
    fprintf(f, "  %s: %u bytes\n", what, len);
    for (i = 0; i < len; i += 16) {
        fprintf(f, "    %04X ", i);
        for (j = 0; j < 16; j++)
            if (i + j < len) fprintf(f, " %02X", XU_PTR(va)[i + j]);
            else             fprintf(f, "   ");
        fprintf(f, "  ");
        for (j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = XU_PTR(va)[i + j];
            fputc(c >= 0x20 && c < 0x7F ? c : '.', f);
        }
        fputc('\n', f);
    }
}

/* The used part of a reply buffer: up to its last non-zero byte. */
static uint32_t xu_used(uint32_t va, uint32_t size)
{
    if (va < 0x10000 || va >= 0x80000000u || size > 0x100000)
        return size;
    while (size && XU_PTR(va)[size - 1] == 0)
        size--;
    return size;
}

void sub_001152B4(void)
{
    FILE *f = xu_log();

    if (f) {
        uint32_t esp = g_esp;
        char path[128];
        fprintf(f, "REQUEST service %u %s (reply buffer 0x%08X, %u bytes; timeout %u ms)\n",
                XU_MEM32(esp + 4), xu_str(XU_MEM32(esp + 0x0C), path, sizeof path),
                XU_MEM32(esp + 0x10), XU_MEM32(esp + 0x14), XU_MEM32(esp + 0x28));
        xu_dump(f, "body", XU_MEM32(esp + 0x20), XU_MEM32(esp + 0x24));
        fflush(f);
    }
    sub_001152B4_gen();
}

void sub_0012CF27(void)
{
    FILE *f = xu_log();

    if (f) {
        uint32_t obj = XU_MEM32(g_esp + 4), entry, reply, size;
        char path[128] = "";
        if (obj >= 0x10000 && obj < 0x80000000u) {
            entry = XU_MEM32(obj + 0x160);
            if (entry >= 0x10000 && entry < 0x80000000u)
                xu_str(XU_MEM32(entry + 8), path, sizeof path);
            reply = XU_MEM32(obj + 0x174);
            size = XU_MEM32(obj + 0x178);
            fprintf(f, "ANSWER %s: status 0x%08X, %u bytes received\n", path,
                    XU_MEM32(obj + 0x168), XU_MEM32(obj + 0x17C));
            xu_dump(f, "reply", reply, xu_used(reply, size));
            fflush(f);
        }
    }
    sub_0012CF27_gen();
}

void sub_00120CB6(void)
{
    FILE *f = xu_log();

    if (f) {
        uint32_t obj = XU_MEM32(g_esp + 4);
        if (obj >= 0x10000 && obj < 0x80000000u) {
            uint32_t reply = XU_MEM32(obj + 0x174), size = XU_MEM32(obj + 0x170);
            fprintf(f, "ANSWER /xuacs/reservename.ashx: status 0x%08X\n", XU_MEM32(obj + 0x10));
            xu_dump(f, "reply", reply, xu_used(reply, size));
            fflush(f);
        }
    }
    sub_00120CB6_gen();
}


/* ── Account requests made directly (RECOMP_XUACS_DRIVE=<file>) ──────────
 *
 * The dashboard's own sign-up screens close at once when it is started
 * straight into NEW ACCOUNT, so the requests are made here instead, through
 * the same Xbox Live library, on the console it has already signed in, from
 * inside XOnlineTaskContinue (sub_0011770B, the library object's method),
 * which the dashboard calls on its main thread every frame.
 *
 * The file names what to do, one key = value a line, and is the player's
 * own (it holds their subscription code): never commit one.
 *
 *     steps    = reserve,offer     in order: download, reserve, offer, create
 *     download = <path>            download: a file of the dashboard's, as
 *                                  /Europe/Country/CountryMaster2.INX
 *     save     = <file>            download: where its bytes go
 *     gamertag = <name>            reserve: the name to hold
 *     country  = <id>              the country byte both requests carry
 *     code     = <code>            offer: the subscription code to check
 *     email    = <address>         create: the one the code was sent to
 *     confirm_create = yes         create runs only with this: it spends
 *                                  the subscription code
 *     start_ms = 25000             wait this long after the first call, so
 *                                  the dashboard's own sign-in has finished
 *
 * reserve is reservename (sub_00121DBC: name, 5 alternatives, country,
 * event, &task); offer is verifyvouchergetoffer (sub_00121B61: code,
 * country, event, &task); create is createowneracct (sub_0012248A: the
 * sign-up state -- gamertag, PIN, birth date -- the owner record, event,
 * &task), whose answer the library itself turns into the account on the
 * disk (sub_00121F40); download is the dashboard's file download
 * (sub_001209AF: service 11, path, buffer, size, 0, 0, timeout, event,
 * &task), as it fetches its country list. Neither makes the account: creating one is a step
 * this file does not have. Each task is pumped (sub_0011770B) until it
 * fails, succeeds (0x001500F0) or 60 s pass, then closed (sub_00117720),
 * and the close hooks above write its answer to RECOMP_XUACS_LOG. */
extern void sub_0011770B_gen(void);
extern void sub_00117720(void);
extern void sub_00121DBC(void);
extern void sub_00121B61(void);
extern void sub_001209AF(void);
extern void sub_0012248A(void);
uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);

#include <time.h>

#define XU_CXO 0x00194E0Cu   /* the library object, as every wrapper loads it */

static struct {
    int      loaded, on, busy;
    char     steps[64], gamertag[32], code[48], download[128], save[512], email[130];
    int      confirm_create;
    uint32_t dl_buf;
    uint32_t country, start_ms;
    uint64_t first, began;
    int      step;            /* index into steps */
    uint32_t task, ptask, last_hr;
    char     name[16];        /* the step running, "" when none */
} xd;

static uint64_t xu_now_ms(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void xu_trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
}

static void xu_load(void)
{
    const char *path = getenv("RECOMP_XUACS_DRIVE");
    char line[160];
    FILE *f;

    xd.loaded = 1;
    xd.start_ms = 25000;
    if (!path || !*path || !(f = fopen(path, "r")))
        return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *k = line, *v;
        if (!eq || line[0] == '#')
            continue;
        *eq = 0;
        v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        xu_trim(k);
        xu_trim(v);
        if (!strcmp(k, "steps"))         snprintf(xd.steps, sizeof xd.steps, "%s", v);
        else if (!strcmp(k, "gamertag")) snprintf(xd.gamertag, sizeof xd.gamertag, "%s", v);
        else if (!strcmp(k, "code")) {
            /* As the dashboard's code screen keeps it: no dashes. */
            size_t o = 0;
            for (; *v && o + 1 < sizeof xd.code; v++)
                if (*v != '-' && *v != ' ')
                    xd.code[o++] = *v;
            xd.code[o] = 0;
        }
        else if (!strcmp(k, "download")) snprintf(xd.download, sizeof xd.download, "%s", v);
        else if (!strcmp(k, "save"))     snprintf(xd.save, sizeof xd.save, "%s", v);
        else if (!strcmp(k, "email"))    snprintf(xd.email, sizeof xd.email, "%s", v);
        else if (!strcmp(k, "confirm_create")) xd.confirm_create = !strcmp(v, "yes");
        else if (!strcmp(k, "country"))  xd.country = (uint32_t)strtoul(v, NULL, 0);
        else if (!strcmp(k, "start_ms")) xd.start_ms = (uint32_t)strtoul(v, NULL, 0);
    }
    fclose(f);
    xd.on = xd.steps[0] != 0;
    if (xd.on)
        fprintf(stderr, "[XUACS] driving: steps %s, country %u, start after %u ms\n",
                xd.steps, xd.country, xd.start_ms);
}

/* A guest string as UTF-16, in guest memory. */
static uint32_t xu_wide(const char *s)
{
    size_t n = strlen(s), i;
    uint32_t va = xbox_HeapAlloc((uint32_t)(n * 2 + 2), 16);
    if (!va)
        return 0;
    for (i = 0; i <= n; i++) {
        *(volatile uint16_t *)((uintptr_t)(va + 2 * i) + g_xbox_mem_offset) =
            (uint16_t)(unsigned char)s[i];
    }
    return va;
}

/* A string into a UTF-16 slot of `chars` characters at va. */
static void xu_put_wide(uint32_t va, const char *s, size_t chars)
{
    size_t i;
    for (i = 0; i + 1 < chars && s[i]; i++)
        *(volatile uint16_t *)((uintptr_t)(va + 2 * i) + g_xbox_mem_offset) = (uint16_t)(unsigned char)s[i];
    *(volatile uint16_t *)((uintptr_t)(va + 2 * i) + g_xbox_mem_offset) = 0;
}

static void xu_put_u32(uint32_t va, uint32_t v) { XU_MEM32(va) = v; }

/* Call a lifted thiscall/stdcall function with these stack arguments, the
 * registers the caller had put back afterwards. */
static uint32_t xu_call(void (*fn)(void), uint32_t ecx, int n, const uint32_t *args)
{
    uint32_t eax = g_eax, oecx = g_ecx, edx = g_edx, esp = g_esp, r;
    int i;

    for (i = n - 1; i >= 0; i--) {
        g_esp -= 4;
        XU_MEM32(g_esp) = args[i];
    }
    g_esp -= 4;
    XU_MEM32(g_esp) = 0;             /* the return address; ret pops it */
    g_ecx = ecx;
    fn();
    r = g_eax;
    g_esp = esp;
    g_eax = eax;
    g_ecx = oecx;
    g_edx = edx;
    return r;
}

/* The n-th name in the comma list, or 0 when there is none. */
static int xu_step_name(int n, char *out, size_t size)
{
    const char *p = xd.steps;
    size_t k;

    while (n-- > 0) {
        p = strchr(p, ',');
        if (!p)
            return 0;
        p++;
    }
    for (k = 0; p[k] && p[k] != ',' && k + 1 < size; k++)
        out[k] = p[k];
    out[k] = 0;
    xu_trim(out);
    return out[0] != 0;
}

static void xu_start_next(uint32_t cxo)
{
    FILE *f = xu_log();
    uint32_t args[5], hr = 0;

    xd.name[0] = 0;
    if (!xu_step_name(xd.step, xd.name, sizeof xd.name)) {
        if (f) fprintf(f, "DRIVE: all steps done\n");
        fprintf(stderr, "[XUACS] all steps done\n");
        xd.on = 0;
        return;
    }
    if (!xd.ptask)
        xd.ptask = xbox_HeapAlloc(4, 4);
    XU_MEM32(xd.ptask) = 0;
    if (!strcmp(xd.name, "reserve")) {
        args[0] = xu_wide(xd.gamertag);
        args[1] = 5;                  /* alternatives, as the dashboard asks */
        args[2] = xd.country;
        args[3] = 0;                  /* no event: pumped */
        args[4] = xd.ptask;
        hr = xu_call(sub_00121DBC, cxo, 5, args);
    } else if (!strcmp(xd.name, "download")) {
        uint32_t path = xbox_HeapAlloc((uint32_t)strlen(xd.download) + 1, 16), i;
        uint32_t dl_args[9];
        if (!xd.dl_buf)
            xd.dl_buf = xbox_HeapAlloc(0x100000, 16);
        if (!path || !xd.dl_buf) {
            xd.on = 0;
            return;
        }
        for (i = 0; i <= strlen(xd.download); i++)
            *(volatile char *)((uintptr_t)(path + i) + g_xbox_mem_offset) = xd.download[i];
        memset((void *)((uintptr_t)xd.dl_buf + g_xbox_mem_offset), 0, 0x100000);
        dl_args[0] = 11; dl_args[1] = path; dl_args[2] = xd.dl_buf; dl_args[3] = 0x100000;
        dl_args[4] = 0; dl_args[5] = 0; dl_args[6] = 30000; dl_args[7] = 0; dl_args[8] = xd.ptask;
        hr = xu_call(sub_001209AF, cxo, 9, dl_args);
    } else if (!strcmp(xd.name, "create")) {
        /* The sign-up state (the gamertag at +0, the PIN at +0x20, the birth
         * date at +0x24 as a UTC FILETIME) and the owner record (the fields
         * docs/technical/xbox-live-accounts.md lists), with the details
         * Insignia's guide says to make up: a test card, a made-up name and
         * address. Only the e-mail is the player's. */
        uint32_t state = xbox_HeapAlloc(0x100, 16), owner = xbox_HeapAlloc(0x1000, 16);
        if (!xd.confirm_create || !xd.email[0] || !xd.gamertag[0] || !state || !owner) {
            fprintf(stderr, "[XUACS] create: needs gamertag, email and confirm_create = yes; stopping\n");
            xd.on = 0;
            xd.name[0] = 0;
            return;
        }
        memset((void *)((uintptr_t)state + g_xbox_mem_offset), 0, 0x100);
        memset((void *)((uintptr_t)owner + g_xbox_mem_offset), 0, 0x1000);
        xu_put_wide(state, xd.gamertag, 16);
        xu_put_u32(state + 0x20, 0);                       /* no PIN */
        xu_put_u32(state + 0x24, 0x18d64000u);             /* 1 Jan 1990 */
        xu_put_u32(state + 0x28, 0x01b41e2au);
        xu_put_wide(owner + 0x000, "Test", 64);            /* first name */
        xu_put_wide(owner + 0x080, "Player", 64);          /* last name */
        xu_put_wide(owner + 0x100, "1 Test Street", 128);
        xu_put_wide(owner + 0x400, "London", 64);          /* city */
        *(volatile uint8_t *)((uintptr_t)(owner + 0x500) + g_xbox_mem_offset) = (uint8_t)xd.country;
        xu_put_wide(owner + 0x502, "SW1A 1AA", 16);        /* postal code */
        xu_put_wide(owner + 0x53A, "02070000000", 12);     /* phone */
        xu_put_wide(owner + 0x56A, "Test Player", 64);     /* card holder */
        *(volatile uint8_t *)((uintptr_t)(owner + 0x5EA) + g_xbox_mem_offset) = 0;  /* Visa */
        xu_put_wide(owner + 0x5EC, "4111111111111111", 24);
        xu_put_u32(owner + 0x61C, 0xf5fe4000u);            /* expires 1 Dec 2030 */
        xu_put_u32(owner + 0x620, 0x01e1fd35u);
        xu_put_wide(owner + 0x624, xd.email, 129);
        /* The subscription code, in the record's last text slot: the
         * dashboard's code screen keeps it at state +0x8D2 (owner +0x8A6),
         * and the code check reads it from there. 26 characters: the 25
         * without their dashes. Without it Insignia answers 500, X-Err
         * 0x80154007. */
        xu_put_wide(owner + 0x8A6, xd.code, 26);
        args[0] = state;
        args[1] = owner;
        args[2] = 0;
        args[3] = xd.ptask;
        hr = xu_call(sub_0012248A, cxo, 4, args);
    } else if (!strcmp(xd.name, "offer")) {
        args[0] = xu_wide(xd.code);
        args[1] = xd.country;
        args[2] = 0;
        args[3] = xd.ptask;
        hr = xu_call(sub_00121B61, cxo, 4, args);
    } else {
        fprintf(stderr, "[XUACS] no step called '%s'; stopping\n", xd.name);
        xd.on = 0;
        return;
    }
    xd.task = XU_MEM32(xd.ptask);
    xd.began = xu_now_ms();
    xd.last_hr = 0xFFFFFFFFu;
    fprintf(stderr, "[XUACS] %s started: 0x%08X, task 0x%08X\n", xd.name, hr, xd.task);
    if (f) { fprintf(f, "DRIVE: %s started: 0x%08X, task 0x%08X\n", xd.name, hr, xd.task); fflush(f); }
    if ((int32_t)hr < 0 || !xd.task) {
        xd.on = 0;
        xd.name[0] = 0;
    }
}

static void xu_drive(void)
{
    uint32_t cxo, hr;
    uint64_t now = xu_now_ms();

    if (!xd.loaded)
        xu_load();
    if (!xd.on)
        return;
    if (!xd.first)
        xd.first = now;
    if (now - xd.first < xd.start_ms)
        return;
    cxo = XU_MEM32(XU_CXO);
    if (!cxo)
        return;
    if (!xd.name[0]) {
        xu_start_next(cxo);
        return;
    }
    hr = xu_call(sub_0011770B_gen, cxo, 1, &xd.task);
    if (hr != xd.last_hr) {
        FILE *f = xu_log();
        fprintf(stderr, "[XUACS] %s: 0x%08X after %llu ms\n", xd.name, hr,
                (unsigned long long)(now - xd.began));
        if (f) { fprintf(f, "DRIVE: %s: 0x%08X after %llu ms\n", xd.name, hr,
                         (unsigned long long)(now - xd.began)); fflush(f); }
        xd.last_hr = hr;
    }
    if ((int32_t)hr < 0 || hr == 0x001500F0u || now - xd.began > 60000u) {
        if (!strcmp(xd.name, "download") && xd.dl_buf && xd.save[0]) {
            uint32_t used = xu_used(xd.dl_buf, 0x100000);
            FILE *out = fopen(xd.save, "wb");
            if (out) {
                fwrite((const void *)((uintptr_t)xd.dl_buf + g_xbox_mem_offset), 1, used, out);
                fclose(out);
            }
            fprintf(stderr, "[XUACS] download: %u bytes to %s\n", used, xd.save);
        }
        xu_call(sub_00117720, cxo, 1, &xd.task);   /* the close hooks log the answer */
        xd.task = 0;
        xd.name[0] = 0;
        if ((int32_t)hr < 0) {
            fprintf(stderr, "[XUACS] stopping after a failure\n");
            xd.on = 0;
        } else {
            xd.step++;
        }
    }
}

/* Sleep (sub_0007DDE6). The dashboard's DirectSound worker (0x0009106C)
 * calls DirectSoundDoWork and Sleep(100) in a loop for as long as it runs:
 * the driver's clock, ten times a second. XOnlineTaskContinue alone stops
 * being called once the dashboard's own sign-in task is closed, about ten
 * seconds in. One guest thread runs at a time (the guest lock), so calling
 * the library from this thread does not race the main one. */
extern void sub_0007DDE6_gen(void);
void sub_0007DDE6(void)
{
    uint32_t ms = XU_MEM32(g_esp + 4);

    sub_0007DDE6_gen();
    if (ms == 100 && !xd.busy) {
        xd.busy = 1;
        xu_drive();
        xd.busy = 0;
    }
}

void sub_0011770B(void)
{
    static unsigned long calls;
    static uint64_t t0;
    uint64_t now = xu_now_ms();

    if (!t0)
        t0 = now;
    if (getenv("RECOMP_XUACS_DRIVE") && (calls++ % 100u) == 0)
        fprintf(stderr, "[XUACS] TaskContinue #%lu at %llu ms (task 0x%08X)\n", calls,
                (unsigned long long)(now - t0), XU_MEM32(g_esp + 4));
    sub_0011770B_gen();
    if (!xd.busy) {
        xd.busy = 1;
        xu_drive();
        xd.busy = 0;
    }
}
