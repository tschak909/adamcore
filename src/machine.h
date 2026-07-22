/*
 * adamcore - machine glue (memory switcher, I/O decode, frame loop)
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ADAMCORE_MACHINE_H
#define ADAMCORE_MACHINE_H

#include "adamcore.h"
#include "adamnet.h"
#include "sn76489.h"
#include "tms9928a.h"
#include "z80.h"

#define ADAM_CPU_CLOCK   3579545
#define ADAM_LINE_TSTATES 228
#define ADAM_FRAME_LINES  262
#define ADAM_BORDER_TOP   10

#define KEYQ_LEN 256 /* power of two */

struct adamcore {
    z80 cpu;
    tms9928a vdp;
    sn76489 psg;
    adamnet an;

    adamcore_config cfg;

    uint8_t ram[0x10000];  /* 64K intrinsic */
    uint8_t xram[0x10000]; /* 64K expander */
    uint8_t os7[0x2000];
    uint8_t eos[0x2000];
    uint8_t wp[0x8000];
    uint8_t cart[0x8000];
    int cart_size;

    uint8_t mem_ctrl; /* port 0x7F: D0-D1 lower bank, D2-D3 upper bank */
    uint8_t net_ctrl; /* port 0x3F: bit0 net reset, bit1 EOS ROM enable */
    int game_mode;    /* 1 after a mode-1 (ColecoVision) reset */

    uint16_t palette_rgb[16];
    uint16_t fb[ADAMCORE_FB_WIDTH * ADAMCORE_FB_HEIGHT];
    uint16_t fb_prev[ADAMCORE_FB_WIDTH * ADAMCORE_FB_HEIGHT];

    /* input, written from other threads */
    volatile uint16_t joy[2];
    volatile int pending_reset; /* -1 none, 0 ADAM, 1 CV */
    uint8_t keyq[KEYQ_LEN];
    volatile uint32_t keyq_w, keyq_r;
    uint8_t joy_mode; /* strobe state: 0 keypad, 1 joystick */

    uint64_t frame_start_cycles;
    unsigned long nmi_count;

    /* Debugger support (adamcore_debug.h). A frame in progress under
     * adamcore_debug_run keeps its cursor here so run_frame and debug_run
     * can hand execution back and forth without losing a cycle. Zero cost
     * when the debugger is unused: run_frame only tests dbg_in_frame. */
    int dbg_in_frame;         /* 1 = partial frame outstanding */
    int dbg_line;             /* current scanline 0..261 */
    uint64_t dbg_line_target; /* cycle target ending the current line */
    uint8_t bp_map[0x2000];   /* 64K PC bitmap */
    int bp_count;
    int (*exec_hook)(void *ud, struct adamcore *c, uint16_t pc);
    void *exec_hook_ud;
};

/* Internal helpers shared between machine.c's frame loop and debug.c's
 * instruction-stepped equivalent. Both must produce bit-identical machine
 * state; keeping the pieces in one place is what guarantees it. */
void machine_apply_pending_reset(adamcore *c);
void machine_vblank_nmi(adamcore *c);          /* line == TMS_ACTIVE_H */
void machine_line_end(adamcore *c, int line);  /* render + adamnet scan */
int  machine_frame_tail(adamcore *c);          /* psg publish, borders, diff */
uint8_t machine_mem_read(adamcore *c, uint16_t a);  /* side-effect free */
void machine_mem_write(adamcore *c, uint16_t a, uint8_t v);

#endif
