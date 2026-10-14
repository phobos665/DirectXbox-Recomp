/*
 * MCPX APU State Structures (adapted from xemu)
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "apu_shim.h"
#include "apu_regs.h"
#include "apu_debug.h"

/* ============================================================
 * DSP CPU registers
 * ============================================================ */

#define DSP_REG_MAX     0x40
#define DSP_XRAM_SIZE   4096
#define DSP_YRAM_SIZE   2048
#define DSP_PRAM_SIZE   4096
#define DSP_MIXBUFFER_SIZE 1024
#define DSP_PERIPH_SIZE 128

/* ============================================================
 * DSP Core State (from dsp_cpu.h)
 * ============================================================ */

typedef struct dsp_core_s {
    bool is_gp;
    bool is_idle;
    uint32_t cycle_count;

    uint16_t instr_cycle;

    uint32_t pc;
    uint32_t registers[DSP_REG_MAX];

    /* stack[0=ssh], stack[1=ssl] */
    uint32_t stack[2][16];

    uint32_t xram[DSP_XRAM_SIZE];
    uint32_t yram[DSP_YRAM_SIZE];
    uint32_t pram[DSP_PRAM_SIZE];
    const void *pram_opcache[DSP_PRAM_SIZE];

    uint32_t mixbuffer[DSP_MIXBUFFER_SIZE];

    /* peripheral space */
    uint32_t periph[DSP_PERIPH_SIZE];

    uint32_t loop_rep;
    uint32_t pc_on_rep;

    /* Interruptions */
    uint16_t interrupt_state;
    uint16_t interrupt_instr_fetch;
    uint16_t interrupt_save_pc;
    uint16_t interrupt_counter;
    uint16_t interrupt_ipl_to_raise;
    uint16_t interrupt_pipeline_count;
    int16_t interrupt_ipl[12];
    uint16_t interrupt_is_pending[12];

    /* callbacks */
    uint32_t (*read_peripheral)(struct dsp_core_s *core, uint32_t address);
    void (*write_peripheral)(struct dsp_core_s *core, uint32_t address, uint32_t value);

    uint32_t num_inst;
    uint32_t cur_inst_len;
    uint32_t cur_inst;

    char str_disasm_memory[2][50];
    uint32_t disasm_memory_ptr;
    bool exception_debugging;

    uint32_t disasm_prev_inst_pc;
    bool disasm_is_looping;

    uint32_t disasm_cur_inst;
    uint16_t disasm_cur_inst_len;

    char disasm_str_instr[256];
    char disasm_str_instr2[523];
    char disasm_parallelmove_name[64];

    uint32_t disasm_registers_save[64];
} dsp_core_t;

/* ============================================================
 * DSP DMA State (from dsp_dma.h)
 * ============================================================ */

typedef void (*dsp_scratch_rw_func)(
    void *opaque, uint8_t *ptr, uint32_t addr, size_t len, bool dir);
typedef void (*dsp_fifo_rw_func)(
    void *opaque, uint8_t *ptr, unsigned int index, size_t len, bool dir);

typedef struct DSPDMAState {
    dsp_core_t *core;

    void *rw_opaque;
    dsp_scratch_rw_func scratch_rw;
    dsp_fifo_rw_func fifo_rw;

    uint32_t configuration;
    uint32_t control;
    uint32_t start_block;
    uint32_t next_block;

    bool error;
    bool eol;
} DSPDMAState;

/* ============================================================
 * DSP State (from dsp_state.h)
 * ============================================================ */

typedef struct DSPState {
    dsp_core_t core;
    DSPDMAState dma;
    int save_cycles;
    uint32_t interrupts;
    bool is_gp;
} DSPState;

/* ============================================================
 * VP State: xemu's own vp/vp.h (third_party/xemu), with its filters,
 * HRTF and ADPCM decoder. These used to be copied here from an older
 * xemu; the voice processor is now compiled from xemu's source
 * (apu_vp_xemu.c), so its state comes from the same place.
 * ============================================================ */

#include "hw/xbox/mcpx/apu/vp/vp.h"

/* ============================================================
 * GP/EP State (from dsp/gp_ep.h)
 * ============================================================ */

typedef struct MCPXAPUGPState {
    bool realtime;
    MemoryRegion mmio;
    DSPState *dsp;
    uint32_t regs[0x10000];
} MCPXAPUGPState;

typedef struct MCPXAPUEPState {
    bool realtime;
    MemoryRegion mmio;
    DSPState *dsp;
    uint32_t regs[0x10000];
} MCPXAPUEPState;

/* ============================================================
 * Main APU State (from apu_int.h)
 * ============================================================ */

struct MCPXAPUState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

    bool exiting;
    bool set_irq;

    QemuThread apu_thread;
    QemuMutex lock;
    QemuCond cond;
    QemuCond idle_cond;
    bool pause_requested;
    bool is_idle;

    MemoryRegion *ram;
    uint8_t *ram_ptr;
    MemoryRegion mmio;

    MCPXAPUVPState vp;
    MCPXAPUGPState gp;
    MCPXAPUEPState ep;

    uint32_t regs[0x20000];

    int ep_frame_div;
    int sleep_acc_us;
    int frame_count;
    int64_t frame_count_time_ms;
    int64_t next_frame_time_us;

    struct {
        struct {
            int backoff, ok, speedup;
        } pacing;
        struct {
            int64_t last_us;
            int64_t min_us, max_us, sum_us;
            int count;
        } deviation;
        int queued_bytes_min, queued_bytes_max;
        int64_t queued_bytes_sum;
        int queued_bytes_count;
    } throttle;

    struct {
        McpxApuDebugMonitorPoint point;
        int16_t frame_buf[256][2];
        void *stream; /* SDL_AudioStream* - stubbed */
        int queued_bytes_low, queued_bytes_high;
    } monitor;
};

/* ============================================================
 * Forward declarations for APU sub-module functions
 * ============================================================ */

/* VP functions */
void mcpx_apu_vp_init(MCPXAPUState *d);
void mcpx_apu_vp_finalize(MCPXAPUState *d);
void mcpx_apu_vp_frame(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME]);
void mcpx_apu_vp_reset(MCPXAPUState *d);

/* DSP functions (stubbed) */
void mcpx_apu_dsp_init(MCPXAPUState *d);
void mcpx_apu_update_dsp_preference(MCPXAPUState *d);
void mcpx_apu_dsp_frame(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME]);

/* Debug globals */
extern MCPXAPUState *g_state;
extern struct McpxApuDebug g_dbg, g_dbg_cache;
extern int g_dbg_voice_monitor;
extern uint64_t g_dbg_muted_voices[4];
