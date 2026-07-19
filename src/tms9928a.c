/*
 * adamcore - TMS9928A Video Display Processor
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Implemented from the TI TMS9918A/9928A datasheet and the TI "Video
 * Display Processors Programmer's Guide": modes 0-3 (Graphics I/II,
 * Multicolor, Text), sprite processing with the 4-per-line limit,
 * fifth-sprite and coincidence status flags, read-ahead buffer semantics.
 */

#include "tms9928a.h"

#include <string.h>

enum {
    ST_INT = 0x80, /* F: frame interrupt */
    ST_5S  = 0x40, /* fifth sprite */
    ST_C   = 0x20  /* coincidence */
};

void tms_reset(tms9928a *v)
{
    memset(v, 0, sizeof(*v));
}

uint8_t tms_backdrop(const tms9928a *v)
{
    uint8_t c = (uint8_t)(v->regs[7] & 0x0F);
    return c ? c : 1; /* transparent backdrop displays as black */
}

/* ---- CPU port interface ---------------------------------------------------- */

uint8_t tms_read_data(tms9928a *v)
{
    uint8_t r = v->read_ahead;
    v->latched = 0;
    v->read_ahead = v->vram[v->addr];
    v->addr = (uint16_t)((v->addr + 1) & 0x3FFF);
    return r;
}

uint8_t tms_read_status(tms9928a *v)
{
    uint8_t s = v->status;
    v->latched = 0;
    v->status &= 0x1F; /* F, 5S, C cleared on read */
    return s;
}

void tms_write_data(tms9928a *v, uint8_t val)
{
    v->latched = 0;
    v->vram[v->addr] = val;
    v->read_ahead = val;
    v->addr = (uint16_t)((v->addr + 1) & 0x3FFF);
}

void tms_write_ctrl(tms9928a *v, uint8_t val)
{
    if (!v->latched) {
        v->latch = val;
        v->latched = 1;
        /* low address byte updates immediately per datasheet */
        v->addr = (uint16_t)((v->addr & 0x3F00) | val);
        return;
    }
    v->latched = 0;
    switch (val >> 6) {
    case 0: /* set VRAM read address; performs a prefetch */
        v->addr = (uint16_t)(((val & 0x3F) << 8) | v->latch);
        v->read_ahead = v->vram[v->addr];
        v->addr = (uint16_t)((v->addr + 1) & 0x3FFF);
        break;
    case 1: /* set VRAM write address */
        v->addr = (uint16_t)(((val & 0x3F) << 8) | v->latch);
        break;
    default: /* register write */
        v->regs[val & 7] = v->latch;
        break;
    }
}

/* ---- interrupt pin --------------------------------------------------------- */

int tms_int_pin(tms9928a *v)
{
    uint8_t was = v->int_line;
    v->int_line = (uint8_t)((v->status & ST_INT) && (v->regs[1] & 0x20));
    return v->int_line && !was;
}

int tms_vblank(tms9928a *v)
{
    v->status |= ST_INT;
    return tms_int_pin(v);
}

/* ---- rendering ------------------------------------------------------------- */

static void render_sprites(tms9928a *v, int y)
{
    uint16_t attr_base = (uint16_t)((v->regs[5] & 0x7F) << 7);
    uint16_t pat_base = (uint16_t)((v->regs[6] & 0x07) << 11);
    int size16 = v->regs[1] & 0x02;
    int mag = v->regs[1] & 0x01;
    int span = (size16 ? 16 : 8) << (mag ? 1 : 0);
    int shown = 0;
    uint8_t drawn[TMS_ACTIVE_W];
    int s;

    memset(drawn, 0, sizeof(drawn));

    for (s = 0; s < 32; s++) {
        const uint8_t *a = &v->vram[(attr_base + (unsigned)s * 4) & 0x3FFF];
        int sy = a[0];
        int sx = a[1];
        uint8_t name = a[2];
        uint8_t flags = a[3];
        uint8_t color = (uint8_t)(flags & 0x0F);
        int row;

        if (sy == 0xD0) break; /* end-of-list sentinel */

        /* sprite Y is the line above the first displayed line; Y >= 0xE1
         * bleeds in from the top (signed wrap) */
        sy = (sy > 0xE0) ? sy - 256 : sy;
        row = y - (sy + 1);
        if (row < 0 || row >= span) continue;

        if (shown == 4) {
            if (!(v->status & (ST_5S | ST_INT)))
                v->status = (uint8_t)((v->status & 0xE0) | ST_5S | s);
            break;
        }
        shown++;

        if (mag) row >>= 1;
        if (flags & 0x80) sx -= 32; /* early clock */

        {
            uint16_t pat;
            int cols = size16 ? 16 : 8;
            int px;
            if (size16) {
                uint16_t base = (uint16_t)(pat_base + ((name & 0xFC) << 3));
                uint8_t left = v->vram[(base + (unsigned)row) & 0x3FFF];
                uint8_t right = v->vram[(base + (unsigned)row + 16) & 0x3FFF];
                pat = (uint16_t)((left << 8) | right);
            } else {
                uint16_t base = (uint16_t)(pat_base + (name << 3));
                pat = (uint16_t)(v->vram[(base + (unsigned)row) & 0x3FFF] << 8);
            }
            for (px = 0; px < cols; px++) {
                if (pat & (0x8000 >> px)) {
                    int rep = mag ? 2 : 1;
                    int k;
                    for (k = 0; k < rep; k++) {
                        int x = sx + px * rep + k;
                        if (x < 0 || x >= TMS_ACTIVE_W) continue;
                        if (drawn[x]) {
                            v->status |= ST_C;
                            continue;
                        }
                        drawn[x] = 1;
                        if (color) v->line[x] = color;
                    }
                }
            }
        }
    }
}

void tms_render_line(tms9928a *v, int y)
{
    uint8_t bd = tms_backdrop(v);
    int m1 = v->regs[1] & 0x10;
    int m2 = v->regs[1] & 0x08;
    int m3 = v->regs[0] & 0x02;
    uint16_t name_base = (uint16_t)((v->regs[2] & 0x0F) << 10);
    int x;

    if (!(v->regs[1] & 0x40)) { /* display disabled */
        memset(v->line, bd, TMS_ACTIVE_W);
        return;
    }

    if (m1) {
        /* Text mode: 40x24 cells of 6x8, 240 px centered, no sprites */
        uint16_t pat_base = (uint16_t)((v->regs[4] & 0x07) << 11);
        uint8_t fg = (uint8_t)(v->regs[7] >> 4);
        uint8_t fgc = fg ? fg : bd;
        int row = y >> 3, line = y & 7, col;
        memset(v->line, bd, TMS_ACTIVE_W);
        for (col = 0; col < 40; col++) {
            uint8_t ch = v->vram[(name_base + (unsigned)(row * 40 + col)) & 0x3FFF];
            uint8_t bits = v->vram[(pat_base + (unsigned)(ch * 8 + line)) & 0x3FFF];
            int px;
            for (px = 0; px < 6; px++)
                v->line[8 + col * 6 + px] =
                    (bits & (0x80 >> px)) ? fgc : bd;
        }
        return;
    }

    if (m2) {
        /* Multicolor: 4x4 px blocks; pattern byte selects two block colors */
        uint16_t pat_base = (uint16_t)((v->regs[4] & 0x07) << 11);
        int row = y >> 3, seg = (y >> 2) & 1, col;
        for (col = 0; col < 32; col++) {
            uint8_t ch = v->vram[(name_base + (unsigned)(row * 32 + col)) & 0x3FFF];
            uint8_t byte = v->vram[(pat_base + (unsigned)(ch * 8) +
                                    (unsigned)((row & 3) * 2 + seg)) & 0x3FFF];
            uint8_t ca = (uint8_t)(byte >> 4), cb = (uint8_t)(byte & 0x0F);
            for (x = 0; x < 4; x++) {
                v->line[col * 8 + x] = ca ? ca : bd;
                v->line[col * 8 + 4 + x] = cb ? cb : bd;
            }
        }
        render_sprites(v, y);
        return;
    }

    if (!m3) {
        /* Graphics I */
        uint16_t pat_base = (uint16_t)((v->regs[4] & 0x07) << 11);
        uint16_t col_base = (uint16_t)(v->regs[3] << 6);
        int row = y >> 3, line = y & 7, col;
        for (col = 0; col < 32; col++) {
            uint8_t ch = v->vram[(name_base + (unsigned)(row * 32 + col)) & 0x3FFF];
            uint8_t bits = v->vram[(pat_base + (unsigned)(ch * 8 + line)) & 0x3FFF];
            uint8_t clr = v->vram[(col_base + (unsigned)(ch >> 3)) & 0x3FFF];
            uint8_t fgc = (uint8_t)(clr >> 4), bgc = (uint8_t)(clr & 0x0F);
            int px;
            if (!fgc) fgc = bd;
            if (!bgc) bgc = bd;
            for (px = 0; px < 8; px++)
                v->line[col * 8 + px] = (bits & (0x80 >> px)) ? fgc : bgc;
        }
    } else {
        /* Graphics II: three 256-char thirds with maskable pattern/color.
         * R4 bits 1-0 mask character-index bits 9-8 (pattern table);
         * R3 bits 6-0 mask character-index bits 9-3 (color table): the
         * register bits stand in for the address lines the multiplied
         * index drives, per the TI datasheet's address-generation table. */
        uint16_t pat_base = (uint16_t)((v->regs[4] & 0x04) << 11);
        uint16_t col_base = (uint16_t)((v->regs[3] & 0x80) << 6);
        uint16_t pat_mask = (uint16_t)(((v->regs[4] & 0x03) << 8) | 0xFF);
        uint16_t col_mask = (uint16_t)(((v->regs[3] & 0x7F) << 3) | 7);
        int row = y >> 3, line = y & 7, col;
        int third = (row >> 3) << 8;
        for (col = 0; col < 32; col++) {
            uint8_t ch = v->vram[(name_base + (unsigned)(row * 32 + col)) & 0x3FFF];
            unsigned idx = (unsigned)(third + ch);
            uint8_t bits = v->vram[(pat_base + ((idx & pat_mask) << 3) +
                                    (unsigned)line) & 0x3FFF];
            uint8_t clr = v->vram[(col_base + ((idx & col_mask) << 3) +
                                   (unsigned)line) & 0x3FFF];
            uint8_t fgc = (uint8_t)(clr >> 4), bgc = (uint8_t)(clr & 0x0F);
            int px;
            if (!fgc) fgc = bd;
            if (!bgc) bgc = bd;
            for (px = 0; px < 8; px++)
                v->line[col * 8 + px] = (bits & (0x80 >> px)) ? fgc : bgc;
        }
    }
    render_sprites(v, y);
}
