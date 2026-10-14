/* Whether a run's sound was heard: see audio_health.h. */
#include "platform/xbox_winnt.h"
#include "platform/host_timer.h"
#include "audio_health.h"

#include <stdio.h>
#include <string.h>

#define VOICES 512u
#define REPORT_MS 10000u

enum { USED = 1, HEARD = 2, STREAM = 4 };

static CRITICAL_SECTION g_lock;
static volatile LONG    g_lock_ready;
static uint8_t          g_voice[VOICES];
static uint64_t         g_refused;
static uint64_t         g_start_ms, g_last_ms;
static const char      *g_path = "native";

static void lock(void)
{
    /* First caller sets the lock up; the rest wait for it. */
    if (g_lock_ready != 2) {
        if (InterlockedCompareExchange(&g_lock_ready, 1, 0) == 0) {
            InitializeCriticalSection(&g_lock);
            InterlockedExchange(&g_lock_ready, 2);
        } else {
            while (g_lock_ready != 2)
                Sleep(0);
        }
    }
    EnterCriticalSection(&g_lock);
}

static void unlock(void) { LeaveCriticalSection(&g_lock); }

static void print_locked(uint64_t now)
{
    uint32_t i, bu = 0, bh = 0, su = 0, sh = 0;
    for (i = 0; i < VOICES; i++) {
        if (!(g_voice[i] & USED))
            continue;
        if (g_voice[i] & STREAM) { su++; sh += (g_voice[i] & HEARD) != 0; }
        else                     { bu++; bh += (g_voice[i] & HEARD) != 0; }
    }
    fprintf(stderr, "[AUDIO] health t=%llus path=%s: buffers used %u heard %u silent %u; "
            "streams used %u heard %u silent %u; refused %llu\n",
            (unsigned long long)((now - g_start_ms) / 1000u), g_path,
            bu, bh, bu - bh, su, sh, su - sh, (unsigned long long)g_refused);
    fflush(stderr);
    g_last_ms = now;
}

void recomp_audio_health_path(const char *path)
{
    if (path && *path)
        g_path = path;
}

void recomp_audio_health_sound(uint32_t voice, int stream, int nonzero)
{
    uint64_t now = GetTickCount64();
    lock();
    if (!g_start_ms)
        g_start_ms = g_last_ms = now;
    if (voice < VOICES) {
        g_voice[voice] |= USED | (stream ? STREAM : 0) | (nonzero ? HEARD : 0);
    }
    if (now - g_last_ms >= REPORT_MS)
        print_locked(now);
    unlock();
}

void recomp_audio_health_refused(void)
{
    lock();
    g_refused++;
    unlock();
}

void recomp_audio_health_report(void)
{
    lock();
    if (g_start_ms)
        print_locked(GetTickCount64());
    unlock();
}
