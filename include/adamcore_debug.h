/*
 * adamcore - debugger interface
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef ADAMCORE_DEBUG_H
#define ADAMCORE_DEBUG_H

#include "adamcore.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Instruction-stepped execution with a resumable mid-frame cursor. All
 * functions here are emulation-thread only unless noted. The fast path
 * (adamcore_run_frame) is unaffected while no breakpoints/hook are set;
 * if a debug run leaves a frame half-executed, the next run_frame call
 * finishes that frame through the stepped loop first, so the two entry
 * points interleave safely. */

typedef enum {
    ADAMCORE_RUN_FRAME_DONE = 0, /* frame boundary reached */
    ADAMCORE_RUN_BREAKPOINT,     /* stopped BEFORE executing the PC hit */
    ADAMCORE_RUN_STEPPED,        /* max_instructions consumed */
    ADAMCORE_RUN_HOOK_STOP,      /* exec hook returned nonzero */
} adamcore_run_status;

/* Runs instructions one at a time from the stored cursor (starting a new
 * frame if none is in progress), reproducing adamcore_run_frame exactly:
 * per-line rendering and AdamNet scans, the VBlank-NMI-before-line-192
 * ordering, and the end-of-frame PSG publish / border / diff work.
 * max_instructions == 0 means "until frame end or a stop condition".
 * When a frame completes, *fb_changed (may be NULL) receives what
 * run_frame would have returned. */
adamcore_run_status adamcore_debug_run(adamcore *c, uint32_t max_instructions,
                                       int check_breakpoints, int *fb_changed);

/* ---- CPU state ----------------------------------------------------------- */
typedef struct {
    uint8_t a, f, b, c, d, e, h, l;
    uint8_t a2, f2, b2, c2, d2, e2, h2, l2; /* shadow set */
    uint16_t ix, iy, sp, pc;
    uint16_t wz;
    uint8_t i, r;
    uint8_t iff1, iff2, im, halted;
    uint64_t cycles; /* read-only: the machine timebase */
} adamcore_z80_regs;

void adamcore_get_regs(const adamcore *c, adamcore_z80_regs *out);
void adamcore_set_regs(adamcore *c, const adamcore_z80_regs *in);

/* ---- memory (side-effect free: pure banked array decode) ----------------- */
uint8_t adamcore_peek(const adamcore *c, uint16_t addr);
void    adamcore_peek_block(const adamcore *c, uint16_t addr, uint8_t *dst,
                            uint32_t n);
void    adamcore_poke(adamcore *c, uint16_t addr, uint8_t v);

/* Memory-map state: port 0x7F bank control, port 0x3F net control, and
 * whether the machine last took a game (ColecoVision) reset. */
void adamcore_get_machine_state(const adamcore *c, uint8_t *mem_ctrl,
                                uint8_t *net_ctrl, int *game_mode);

/* ---- VDP ------------------------------------------------------------------ */
const uint8_t  *adamcore_vdp_vram(const adamcore *c); /* 16K */
void            adamcore_vdp_state(const adamcore *c, uint8_t regs[8],
                                   uint8_t *status, uint16_t *addr);
const uint16_t *adamcore_palette565(const adamcore *c); /* 16 entries */

/* ---- breakpoints / hook --------------------------------------------------- */
void adamcore_bp_set(adamcore *c, uint16_t addr);
void adamcore_bp_clear(adamcore *c, uint16_t addr);
void adamcore_bp_clear_all(adamcore *c);
int  adamcore_bp_count(const adamcore *c);

/* Called before each instruction with the current PC; return nonzero to
 * stop with ADAMCORE_RUN_HOOK_STOP. Used by hosts for transient step-over/
 * step-out conditions and instruction tracing. */
void adamcore_set_exec_hook(adamcore *c,
                            int (*hook)(void *ud, adamcore *c, uint16_t pc),
                            void *ud);

/* ---- bus hook -------------------------------------------------------------- */
typedef enum {
    ADAMCORE_BUS_MEM_R = 0, /* Z80 memory read, after the value is decoded */
    ADAMCORE_BUS_MEM_W,     /* Z80 memory write, before it lands */
    ADAMCORE_BUS_IO_R,      /* Z80 IN: addr is the port, val what it read */
    ADAMCORE_BUS_IO_W,      /* Z80 OUT: addr is the port */
} adamcore_bus_kind;

/* Called for every Z80 memory and I/O cycle (never for peek/poke, VDP or
 * DMA-style device access). Runs on the emulation thread, in the middle of an
 * instruction: a host that wants to stop sets a flag and stops from its exec
 * hook at the next instruction. fn = NULL removes the hook; with none
 * installed the bus costs nothing. */
void adamcore_set_bus_hook(adamcore *c,
                           void (*fn)(void *ud, int kind, uint16_t addr,
                                      uint8_t val),
                           void *ud);

/* Where the beam is: scanline 0..261 of the frame in progress (0 between
 * frames) and the number of frames completed since power-on. Either
 * out-pointer may be NULL. */
void adamcore_debug_beam(const adamcore *c, int *line, uint64_t *frame);

/* T-states into the scanline reported by adamcore_debug_beam (0 between
 * frames; may run a few T-states past 227 when the last instruction of a line
 * overshoots). Emulation-thread only. */
int adamcore_debug_beam_tstate(const adamcore *c);

/* ---- per-line VDP register capture ------------------------------------------
 * Off by default (zero cost beyond one flag test per line). While enabled,
 * every scanline 0..261 of every frame, on either run path, records the VDP
 * registers and status as that line finished -- for active lines 0..191 the
 * values the line was rendered with, so a register changed mid-frame shows up
 * from the line after the write. The model is line-granular: a change in the
 * middle of a line is not representable and lands on that line's record.
 * Each record is stamped with the index of the frame it belongs to (the same
 * counter as adamcore_debug_beam's frame: completed frames when the line
 * ended), so a reader can tell lines of the last finished frame from lines of
 * the frame in progress, and from older lines not yet overwritten.
 * Disabling clears the records. Emulation-thread only. */
void adamcore_vdp_line_capture_enable(adamcore *c, int enable);

/* Returns 1 and fills the outputs (each may be NULL) if the line has a
 * record, else 0. *frame is the frame the record belongs to. */
int adamcore_vdp_line_regs(const adamcore *c, int line, uint8_t regs[8],
                           uint8_t *status, uint64_t *frame);

/* ---- SN76489 register mirror ---------------------------------------------------
 * The emulation thread's own copy, updated on every write to the chip's port ($E0-$FF decode)
 * with the chip's latch rule (a byte with bit 7 set latches the register and
 * writes the low nibble; any other byte writes the latched register). The
 * live chip belongs to the audio thread and is ~50 ms behind: not exposed.
 * atten: 3 tones then noise, 0 loudest, 15 muted (2 dB per step).
 * Tone frequency = 3579545 / (32 * period), a period of 0 counting as 1024.
 * noise_ctrl bits 0-1 = shift rate, bit 2 = white noise. Values are the chip
 * reset values until the guest writes. */
typedef struct {
    uint8_t latched_reg; /* 0..7: tone0 period, tone0 atten, tone1 ... noise atten */
    uint16_t period[3];
    uint8_t noise_ctrl;
    uint8_t atten[4];
} adamcore_sn_state;

void adamcore_psg_sn_state(const adamcore *c, adamcore_sn_state *out);

/* ---- device state getters (side-effect free) --------------------------------- */
typedef struct {
    uint8_t latch;     /* first control-port byte */
    uint8_t latched;   /* 1 while waiting for the second byte */
    uint8_t read_ahead;/* the data-port read-ahead buffer */
    uint8_t int_line;  /* VDP INT pin (status F and R1 IE) */
} adamcore_vdp_latch;

void adamcore_vdp_latch_state(const adamcore *c, adamcore_vdp_latch *out);

typedef struct {
    uint8_t q;           /* Z80 Q register (F if the last instruction set flags) */
    uint8_t ei_pending;  /* interrupt acceptance blocked for one instruction */
    uint8_t irq_pending; /* maskable interrupt request line */
    uint8_t nmi_pending; /* NMI latched, not yet taken */
    uint64_t nmi_count;  /* VBlank NMIs raised since power-on */
} adamcore_cpu_aux_state;

void adamcore_cpu_aux(const adamcore *c, adamcore_cpu_aux_state *out);

#define ADAMCORE_AY_REGS 16

typedef struct {
    uint8_t configured; /* ColecoVision with an SGM fitted; else all zero */
    uint8_t ram_en;     /* port $53 bit 0: 24K RAM over $2000-$7FFF */
    uint8_t bios_off;   /* port $7F bit 1 clear: RAM over the BIOS */
    uint8_t ay_addr;    /* port $50: latched AY register */
    uint8_t ay_regs[ADAMCORE_AY_REGS]; /* emulation-thread mirror */
} adamcore_sgm_info;

void adamcore_sgm_state(const adamcore *c, adamcore_sgm_info *out);

/* Controller state: joy[port] is the packed word given to
 * adamcore_set_joystick (written from other threads); joy_mode is the
 * strobe: 0 = keypad half, 1 = joystick half. Either pointer may be NULL. */
void adamcore_controller_state(const adamcore *c, uint16_t joy[2],
                               uint8_t *joy_mode);

#ifdef __cplusplus
}
#endif

#endif /* ADAMCORE_DEBUG_H */
