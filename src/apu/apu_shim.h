/*
 * QEMU/SDL/libsamplerate Shim Layer for xemu MCPX APU extraction
 *
 * Replaces QEMU types, SDL audio, and libsamplerate functions with
 * minimal Win32 equivalents so that xemu's APU code can compile standalone.
 *
 * Copyright (c) 2026 Burnout 3 Static Recompilation Project
 * Based on xemu code: Copyright (c) 2012 espes, 2018-2025 Matt Borgerson et al.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#pragma once

/* ============================================================
 * Pull in base QEMU type shims from NV2A extraction
 * ============================================================ */
#include "../nv2a/qemu_shim.h"

#include <math.h>

/* ============================================================
 * Additional atomic ops needed by APU code
 * ============================================================ */

#ifndef qatomic_or
#ifdef _MSC_VER
#define qatomic_or(ptr, val)  _InterlockedOr((long volatile*)(ptr), (long)(val))
#define qatomic_and(ptr, val) _InterlockedAnd((long volatile*)(ptr), (long)(val))
#else
#define qatomic_or(ptr, val)  __sync_fetch_and_or((ptr), (val))
#define qatomic_and(ptr, val) __sync_fetch_and_and((ptr), (val))
#endif
#endif

/* ============================================================
 * QEMU clock helpers (beyond what NV2A shim provides)
 * ============================================================ */

#define QEMU_CLOCK_REALTIME 1

#ifndef qemu_clock_get_us
static inline int64_t qemu_clock_get_us(int type) {
    (void)type;
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    /* Whole seconds and the remainder, not count * 1e6 / freq: on POSIX the
     * QPC shim counts CLOCK_MONOTONIC nanoseconds (freq 1e9), and that
     * product overflows int64 after ~2.5 hours of host uptime (Windows' 10 MHz
     * counter: ~10 days). Same split as qemu_clock_get_ns in qemu_shim.h. */
    return (int64_t)(count.QuadPart / freq.QuadPart) * 1000000LL +
           (int64_t)(count.QuadPart % freq.QuadPart) * 1000000LL / freq.QuadPart;
}
#endif

#ifndef qemu_clock_get_ms
static inline int64_t qemu_clock_get_ms(int type) {
    return qemu_clock_get_us(type) / 1000;
}
#endif

/* ============================================================
 * Threading extensions (timedwait, thread create/join)
 * ============================================================ */

#ifndef qemu_cond_timedwait
static inline void qemu_cond_timedwait(QemuCond *c, QemuMutex *m, int ms) {
    SleepConditionVariableCS(&c->cv, &m->cs, (DWORD)ms);
}
#endif

#define QEMU_THREAD_JOINABLE 0

typedef void *(*QemuThreadFunc)(void *arg);

#ifndef qemu_thread_create
static inline void qemu_thread_create(QemuThread *t, const char *name,
                                       void *(*func)(void *), void *arg,
                                       int mode) {
    (void)name; (void)mode;
    t->thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)func, arg, 0, NULL);
}
#endif

#ifndef qemu_thread_join
static inline void qemu_thread_join(QemuThread *t) {
    WaitForSingleObject(t->thread, INFINITE);
    CloseHandle(t->thread);
}
#endif

/* ============================================================
 * Physical memory access
 *
 * The APU is handed physical addresses: voice structures, sample data,
 * scatter-gather tables, notifiers. In this runtime low RAM and the
 * contiguous window are separate storage, so a physical address alone does
 * not say where the bytes are; xbox_PhysToGuest asks the one place that
 * knows (MmGetPhysicalAddress records, per 4 KB page, which storage it
 * answered for). A buffer in the XBE image -- TimeSplitters 2 plays four
 * from 0x00209090 -- is in low RAM, and read through the window it was
 * another allocation's bytes.
 * ============================================================ */

extern uint8_t *g_apu_ram_ptr; /* the window's host address; kept for callers */
uint32_t xbox_PhysToGuest(uint32_t phys);      /* kernel/xbox_memory_layout.c */
extern ptrdiff_t g_xbox_mem_offset;

static inline uint8_t *apu_phys_ptr(hwaddr addr) {
    uint32_t va = xbox_PhysToGuest((uint32_t)(addr & 0x03FFFFFF));
    return (uint8_t *)((uintptr_t)va + g_xbox_mem_offset);
}

/* Copy `len` bytes from physical memory, page by page (two pages of one
 * block can be in different storage). */
static inline void apu_phys_read(hwaddr addr, void *out, size_t len) {
    uint8_t *o = (uint8_t *)out;
    while (len) {
        size_t in_page = 4096u - (size_t)(addr & 4095u);
        size_t n = len < in_page ? len : in_page;
        memcpy(o, apu_phys_ptr(addr), n);
        o += n; addr += n; len -= n;
    }
}

/* Little-endian physical memory reads */
static inline uint32_t ldl_le_phys(void *as, hwaddr addr) {
    uint32_t v; (void)as;
    memcpy(&v, apu_phys_ptr(addr), 4);
    return v;
}
static inline uint16_t lduw_le_phys(void *as, hwaddr addr) {
    uint16_t v; (void)as;
    memcpy(&v, apu_phys_ptr(addr), 2);
    return v;
}
static inline uint8_t ldub_phys(void *as, hwaddr addr) {
    (void)as;
    return *apu_phys_ptr(addr);
}

/* Little-endian physical memory writes */
static inline void stl_le_phys(void *as, hwaddr addr, uint32_t val) {
    (void)as;
    memcpy(apu_phys_ptr(addr), &val, 4);
}
static inline void stw_le_phys(void *as, hwaddr addr, uint16_t val) {
    (void)as;
    memcpy(apu_phys_ptr(addr), &val, 2);
}
static inline void stb_phys(void *as, hwaddr addr, uint8_t val) {
    (void)as;
    *apu_phys_ptr(addr) = val;
}

/* The address space the calls above are given and ignore. An object, not
 * a macro: current xemu passes &address_space_memory, the older extract
 * passed address_space_memory itself, and both convert to void *. */
static void *address_space_memory;

/* Xbox page size */
#ifndef TARGET_PAGE_SIZE
#define TARGET_PAGE_SIZE 4096
#endif

/* ============================================================
 * PCI / IRQ stubs (APU uses these minimally)
 * ============================================================ */

/* These are already in qemu_shim.h, but ensure they exist */
#ifndef pci_register_bar
#define pci_register_bar(dev, bar, flags, mr) ((void)0)
#endif

#define PCI_INTERRUPT_PIN       0x3D
#define PCI_BASE_ADDRESS_SPACE_MEMORY 0
#define PCI_VENDOR_ID_NVIDIA    0x10DE
#define PCI_DEVICE_ID_NVIDIA_MCPX_APU 0x01B0
#define PCI_CLASS_MULTIMEDIA_AUDIO 0x0401

/* ============================================================
 * Memory region stubs (beyond NV2A shim)
 * ============================================================ */

static inline void memory_region_init_io(MemoryRegion *mr, void *owner,
                                          const MemoryRegionOps *ops,
                                          void *opaque, const char *name,
                                          uint64_t size) {
    (void)owner; (void)ops; (void)name;
    mr->size = size;
    mr->opaque = opaque;
}

static inline void memory_region_add_subregion(MemoryRegion *parent,
                                                hwaddr offset,
                                                MemoryRegion *child) {
    (void)parent; (void)offset; (void)child;
}

static inline uint8_t *memory_region_get_ram_ptr(MemoryRegion *mr) {
    return (uint8_t *)mr->opaque;
}

/* ============================================================
 * libsamplerate: the real one (third_party/libsamplerate, 0.2.2). The
 * voice processor resamples every voice through it; the stubs this replaced
 * returned no frames, so a voice at any rate but 48 kHz was silent.
 * ============================================================ */

#include <samplerate.h>

/* ============================================================
 * SDL Audio stubs
 *
 * The monitor output uses SDL. We stub it; actual audio output
 * will be connected via WASAPI separately.
 * ============================================================ */

typedef void SDL_AudioStream;
typedef uint32_t SDL_AudioDeviceID;

#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK 0
#define SDL_AUDIO_S16LE 0x8010
#define SDL_AUDIO_BYTESIZE(fmt) (((fmt) & 0xFF) / 8)
#define SDL_INIT_AUDIO 0x00000010

typedef struct SDL_AudioSpec {
    int freq;
    int format;
    int channels;
} SDL_AudioSpec;

static inline int SDL_Init(uint32_t flags) { (void)flags; return 0; }
static inline const char *SDL_GetError(void) { return "SDL stubbed"; }
static inline int SDL_GetAudioStreamQueued(SDL_AudioStream *stream) {
    (void)stream; return -1;
}
static inline int SDL_PutAudioStreamData(SDL_AudioStream *stream,
                                          const void *buf, int len) {
    (void)stream; (void)buf; (void)len; return 0;
}
static inline SDL_AudioStream *SDL_OpenAudioDeviceStream(
    SDL_AudioDeviceID dev, const SDL_AudioSpec *spec, void *cb, void *ud) {
    (void)dev; (void)spec; (void)cb; (void)ud; return NULL;
}
static inline SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *s) {
    (void)s; return 0;
}
static inline int SDL_GetAudioDeviceFormat(SDL_AudioDeviceID dev,
                                            SDL_AudioSpec *spec, int *frames) {
    (void)dev; (void)spec; (void)frames; return 0;
}
static inline void SDL_ResumeAudioDevice(SDL_AudioDeviceID dev) { (void)dev; }
static inline void SDL_DestroyAudioStream(SDL_AudioStream *s) { (void)s; }
static inline void SDL_SetAudioStreamGain(SDL_AudioStream *s, float g) {
    (void)s; (void)g;
}
/* The voice processor spreads voices over this many worker threads. */
#ifdef _WIN32
static inline int SDL_GetNumLogicalCPUCores(void) {
    SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors;
}
#else
#include <unistd.h>
static inline int SDL_GetNumLogicalCPUCores(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1;
}
#endif

/* ============================================================
 * VM state / migration stubs
 * ============================================================ */

typedef int VMStateField;
#define VMSTATE_UINT8(f, s)           0
#define VMSTATE_UINT16(f, s)          0
#define VMSTATE_UINT32(f, s)          0
#define VMSTATE_UINT64(f, s)          0
#define VMSTATE_INT16(f, s)           0
#define VMSTATE_INT32(f, s)           0
#define VMSTATE_BOOL(f, s)            0
#define VMSTATE_UINT8_ARRAY(f, s, n)  0
#define VMSTATE_UINT16_ARRAY(f, s, n) 0
#define VMSTATE_INT16_ARRAY(f, s, n)  0
#define VMSTATE_UINT32_ARRAY(f, s, n) 0
#define VMSTATE_UINT64_ARRAY(f, s, n) 0
#define VMSTATE_UINT32_2DARRAY(f, s, m, n) 0
#define VMSTATE_STRUCT(f, s, v, vmsd, type) 0
#define VMSTATE_STRUCT_POINTER(f, s, vmsd, type) 0
#define VMSTATE_STRUCT_ARRAY(f, s, n, v, vmsd, type) 0
#define VMSTATE_PCI_DEVICE(f, s)      0
#define VMSTATE_UNUSED(n)             0
#define VMSTATE_END_OF_LIST()         0

/* ============================================================
 * QEMU Object model stubs
 * ============================================================ */

typedef void Object;
typedef int ResetType;
typedef void ObjectClass;
typedef void InterfaceInfo;
typedef void DeviceClass;
typedef void ResettableClass;
typedef void PCIDeviceClass;
typedef void TypeInfo;

#define DEVICE_CLASS(k) (k)
#define RESETTABLE_CLASS(k) (k)
#define PCI_DEVICE_CLASS(k) (k)
#define INTERFACE_CONVENTIONAL_PCI_DEVICE NULL
#define TYPE_PCI_DEVICE "pci-device"

static inline void type_register_static(const void *info) { (void)info; }
#define type_init(fn) /* no-op */

/* ============================================================
 * Run state / VM change handler stubs
 * ============================================================ */

typedef int RunState;
typedef void (*VMChangeStateHandler)(void *opaque, bool running, RunState state);

static inline void *qemu_add_vm_change_state_handler(VMChangeStateHandler fn,
                                                       void *opaque) {
    (void)fn; (void)opaque;
    return NULL;
}

/* ============================================================
 * Tracing stubs (APU-specific)
 * ============================================================ */

#define trace_mcpx_apu_reg_read(addr, size, val)    ((void)0)
#define trace_mcpx_apu_reg_write(addr, size, val)   ((void)0)
/* RECOMP_APU_TRACE: every front-end method xemu's voice processor runs
 * (apu_core.c apu_trace_method), which is how DirectSound talks to the chip. */
void apu_trace_method(uint32_t method, uint32_t arg);
#define trace_mcpx_apu_method(method, arg)          apu_trace_method((uint32_t)(method), (uint32_t)(arg))

/* ============================================================
 * Warn/error reporting
 * ============================================================ */

#ifndef warn_reportf_err
static inline void warn_reportf_err(Error *err, const char *fmt, ...) {
    if (err) {
        fprintf(stderr, "[APU-WARN] %s%s\n", fmt ? fmt : "", err->msg);
        error_free(err);
    }
}
#endif

/* ============================================================
 * g_config stub (xemu settings used by APU)
 * ============================================================ */

static struct {
    struct {
        float volume_limit;
        bool hrtf;
        struct {
            int num_workers;
        } vp;
    } audio;
} g_config = {
    .audio = {
        .volume_limit = 1.0f,
        /* HRTF on, as xemu ships it: 3D voices are filtered for direction
         * (the crosstalk bins). Off, a 3D sound has distance but no side. */
        .hrtf = true,
        /* Workers for the voice processor; 0 is xemu's "one per core",
         * capped at MAX_VOICE_WORKERS. Four is plenty for 256 voices and
         * leaves the cores to the title. */
        .vp = { .num_workers = 4 },
    },
};

/* ============================================================
 * RCU stubs (used by worker threads)
 * ============================================================ */

static inline void rcu_register_thread(void) {}
static inline void rcu_unregister_thread(void) {}

/* ============================================================
 * g_malloc0_n (QEMU/GLib allocation)
 * ============================================================ */

#ifndef g_malloc0_n
#define g_malloc0_n(n, size) calloc((n), (size))
#endif
#ifndef g_free
#define g_free(p) free(p)
#endif

/* ============================================================
 * HWADDR_PRIx format specifier
 * ============================================================ */

#ifndef HWADDR_PRIx
#define HWADDR_PRIx PRIx64
#endif

/* ============================================================
 * ctz64 (count trailing zeros 64-bit)
 * ============================================================ */

#ifdef _MSC_VER
static inline int ctz64(uint64_t val) {
    unsigned long index;
#ifdef _WIN64
    _BitScanForward64(&index, val);
#else
    if ((uint32_t)val) {
        _BitScanForward(&index, (uint32_t)val);
    } else {
        _BitScanForward(&index, (uint32_t)(val >> 32));
        index += 32;
    }
#endif
    return (int)index;
}
#else
static inline int ctz64(uint64_t val) {
    return __builtin_ctzll(val);
}
#endif

/* ============================================================
 * MCPX APU device macro
 * ============================================================ */

#define MCPX_APU_DEVICE(obj) ((MCPXAPUState *)(obj))

/* ============================================================
 * PCI helper (create simple - stub)
 * ============================================================ */

static inline void *pci_create_simple(void *bus, int devfn, const char *name) {
    (void)bus; (void)devfn; (void)name;
    return NULL;
}
