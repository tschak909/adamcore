/*
 * adamcore - TMS9928A Video Display Processor
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Reference: TI TMS9918A/9928A/9929A datasheet and the TI "Video Display
 * Processors Programmer's Guide". Scanline renderer producing 4-bit color
 * indices (0-15, 0 = transparent handled at render time).
 */

#ifndef ADAMCORE_TMS9928A_H
#define ADAMCORE_TMS9928A_H

#include <stdint.h>

#define TMS_ACTIVE_W 256
#define TMS_ACTIVE_H 192

typedef struct {
    uint8_t vram[0x4000];
    uint8_t regs[8];
    uint8_t status;
    uint8_t read_ahead;
    uint16_t addr;
    uint8_t latch;       /* first control-port byte */
    uint8_t latched;     /* waiting for second byte */
    uint8_t int_line;    /* current INT pin state (status F & IE) */
    /* 4-bit color index per pixel, one active frame */
    uint8_t line[TMS_ACTIVE_W];
} tms9928a;

void tms_reset(tms9928a *v);

uint8_t tms_read_data(tms9928a *v);
uint8_t tms_read_status(tms9928a *v);
void tms_write_data(tms9928a *v, uint8_t val);
void tms_write_ctrl(tms9928a *v, uint8_t val);

/* Renders active line y (0..191) into v->line as color indices with the
 * backdrop already substituted for transparent pixels. */
void tms_render_line(tms9928a *v, int y);

/* Called at the start of vertical blank (after line 191): sets status F.
 * Returns 1 if the INT pin (F & IE) went from low to high. */
int tms_vblank(tms9928a *v);

/* Re-evaluates the INT pin after a register/status change.
 * Returns 1 on a rising edge. */
int tms_int_pin(tms9928a *v);

uint8_t tms_backdrop(const tms9928a *v);

#endif
