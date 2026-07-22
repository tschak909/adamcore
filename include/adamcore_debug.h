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

#ifdef __cplusplus
}
#endif

#endif /* ADAMCORE_DEBUG_H */
