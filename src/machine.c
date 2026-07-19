/*
 * adamcore - machine glue: ADAM/CV memory switcher, I/O decode, frame loop,
 * and the public library API.
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
 *
 * Hardware model from the Coleco ADAM Technical Reference Manual:
 *  - Port 7FH memory map control: D0-D1 lower 32K (00 SmartWriter/EOS ROM,
 *    01 intrinsic RAM, 10 expansion RAM, 11 OS7 + 24K intrinsic RAM),
 *    D2-D3 upper 32K (00 intrinsic RAM, 01 expansion ROM, 10 expansion
 *    RAM, 11 cartridge).
 *  - Port 3FH: bit0 AdamNet reset (set-then-clear), bit1 EOS ROM enable
 *    (overlays the 8K EOS ROM on the top of the SmartWriter window).
 *  - Ports 80H-FFH: ColecoVision-compatible I/O (controllers, VDP, PSG).
 *  - Computer reset selects SmartWriter + intrinsic RAM; game reset
 *    selects OS7 + 24K RAM low and cartridge high.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cart.h"
#include "machine.h"
#include "palette.h"

/* ---- memory -------------------------------------------------------------- */

static uint8_t mem_read(void *ud, uint16_t a)
{
    adamcore *c = ud;
    if (a < 0x8000) {
        switch (c->mem_ctrl & 3) {
        case 0:
            if ((c->net_ctrl & 2) && a >= 0x6000)
                return c->eos[a - 0x6000];
            return c->wp[a];
        case 1: return c->ram[a];
        case 2: return c->xram[a];
        default:
            return a < 0x2000 ? c->os7[a] : c->ram[a];
        }
    }
    switch ((c->mem_ctrl >> 2) & 3) {
    case 0: return c->ram[a];
    case 1: return 0xFF; /* expansion ROM socket, unpopulated */
    case 2: return c->xram[a];
    default:
        return c->cart_size > 0 ? c->cart[a - 0x8000] : 0xFF;
    }
}

static void mem_write(void *ud, uint16_t a, uint8_t v)
{
    adamcore *c = ud;
    if (a < 0x8000) {
        switch (c->mem_ctrl & 3) {
        case 1: c->ram[a] = v; break;
        case 2: c->xram[a] = v; break;
        case 3: if (a >= 0x2000) c->ram[a] = v; break;
        default: break; /* ROM */
        }
        return;
    }
    switch ((c->mem_ctrl >> 2) & 3) {
    case 0: c->ram[a] = v; break;
    case 2: c->xram[a] = v; break;
    default: break; /* ROM */
    }
}

/* ---- controllers ---------------------------------------------------------- */

/* Keypad wire codes (active-low low-nibble patterns as read from the
 * controller port), from public ColecoVision interface documentation.
 * Index: 0-9, 10 = '*', 11 = '#', 12+ reserved/none. */
static const uint8_t keypad_code[16] = {
    0x0A, 0x0D, 0x07, 0x0C, 0x02, 0x03, 0x0E, 0x05,
    0x01, 0x0B, 0x09, 0x06, 0x0F, 0x0F, 0x0F, 0x0F
};

static uint8_t controller_read(adamcore *c, int portno)
{
    uint16_t s = c->joy[portno & 1];
    uint8_t hi = (uint8_t)(s >> 8);   /* joystick-mode byte, active low */
    uint8_t lo = (uint8_t)(s & 0xFF); /* keypad-mode byte, active low */

    if (c->cfg.swap_buttons) {
        uint8_t hf = (uint8_t)(hi & 0x40), lf = (uint8_t)(lo & 0x40);
        hi = (uint8_t)((hi & ~0x40) | lf);
        lo = (uint8_t)((lo & ~0x40) | hf);
    }

    if (c->joy_mode) /* joystick strobe: directions + left fire */
        return (uint8_t)(0xB0 | (hi & 0x4F));
    /* keypad strobe: encoded keypad + right fire */
    return (uint8_t)(0xB0 | (lo & 0x40) | keypad_code[lo & 0x0F]);
}

/* ---- I/O ------------------------------------------------------------------ */

static uint8_t io_read(void *ud, uint16_t port)
{
    adamcore *c = ud;
    uint8_t p = (uint8_t)port;

    switch (p & 0xE0) {
    case 0x20: return c->net_ctrl;
    case 0x60: return c->mem_ctrl;
    case 0xA0:
        if (p & 1) {
            uint8_t v = tms_read_status(&c->vdp);
            tms_int_pin(&c->vdp); /* reading status drops the INT line */
            return v;
        }
        return tms_read_data(&c->vdp);
    case 0xE0: return controller_read(c, (p >> 1) & 1);
    default: return 0xFF;
    }
}

static void io_write(void *ud, uint16_t port, uint8_t v)
{
    adamcore *c = ud;
    uint8_t p = (uint8_t)port;

    switch (p & 0xE0) {
    case 0x20:
        if (p == 0x3F) {
            uint8_t was = c->net_ctrl;
            c->net_ctrl = v;
            if ((was & 1) && !(v & 1))
                adamnet_reset(&c->an); /* reset on bit0 high->low */
        }
        break;
    case 0x60:
        if (p == 0x7F) c->mem_ctrl = (uint8_t)(v & 0x0F);
        break;
    case 0x80: c->joy_mode = 0; break; /* keypad strobe */
    case 0xA0:
        if (p & 1) {
            tms_write_ctrl(&c->vdp, v);
            /* enabling IE while F is pending must raise the line */
            if (tms_int_pin(&c->vdp))
                z80_nmi(&c->cpu);
        } else {
            tms_write_data(&c->vdp, v);
        }
        break;
    case 0xC0: c->joy_mode = 1; break; /* joystick strobe */
    case 0xE0: sn_write(&c->psg, c->cpu.cycles, v); break;
    default: break;
    }
}

/* ---- reset ---------------------------------------------------------------- */

static void machine_reset(adamcore *c, int mode)
{
    z80_reset(&c->cpu);
    tms_reset(&c->vdp);
    c->net_ctrl = 0;
    c->joy_mode = 1;
    if (mode == 1) {
        /* game (ColecoVision) reset: OS7 + 24K RAM low, cartridge high */
        c->game_mode = 1;
        c->mem_ctrl = 0x0F;
    } else {
        /* computer reset: SmartWriter low, intrinsic RAM high */
        c->game_mode = 0;
        c->mem_ctrl = 0x00;
    }
    adamnet_reset(&c->an);
}

/* ---- ROM loading ---------------------------------------------------------- */

static int load_rom(const char *path, uint8_t *dst, size_t want)
{
    FILE *fp;
    size_t n;
    if (!path) return -1;
    fp = fopen(path, "rb");
    if (!fp) return -1;
    n = fread(dst, 1, want, fp);
    fclose(fp);
    return n == want ? 0 : -1;
}

/* ---- public API ----------------------------------------------------------- */

adamcore *adamcore_create(const adamcore_config *cfg)
{
    adamcore *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->cfg = *cfg;

    if (load_rom(cfg->os7_rom_path, c->os7, sizeof(c->os7)) != 0)
        goto fail;
    if (cfg->start_machine == ADAMCORE_MACHINE_ADAM) {
        if (load_rom(cfg->eos_rom_path, c->eos, sizeof(c->eos)) != 0)
            goto fail;
        if (load_rom(cfg->wp_rom_path, c->wp, sizeof(c->wp)) != 0)
            goto fail;
    } else {
        /* optional in cart-only sessions */
        load_rom(cfg->eos_rom_path, c->eos, sizeof(c->eos));
        load_rom(cfg->wp_rom_path, c->wp, sizeof(c->wp));
    }
    c->cart_size = 0;
    if (cfg->cart_path && cfg->cart_path[0]) {
        c->cart_size = cart_load(cfg->cart_path, c->cart);
        if (c->cart_size < 0)
            goto fail;
    }

    c->cpu.ud = c;
    c->cpu.mem_read = mem_read;
    c->cpu.mem_write = mem_write;
    c->cpu.io_read = io_read;
    c->cpu.io_write = io_write;

    palette_build(cfg->palette, c->palette_rgb);
    sn_reset(&c->psg, ADAM_CPU_CLOCK,
             (uint32_t)(cfg->audio_rate > 0 ? cfg->audio_rate : 44100));
    adamnet_init(&c->an, c);

    c->joy[0] = c->joy[1] = 0x7F7F;
    c->pending_reset = -1;
    machine_reset(c, cfg->start_machine == ADAMCORE_MACHINE_CV ? 1 : 0);
    return c;

fail:
    free(c);
    return NULL;
}

void adamcore_destroy(adamcore *c)
{
    if (!c) return;
    adamnet_shutdown(&c->an);
    free(c);
}

int adamcore_run_frame(adamcore *c)
{
    int line;
    uint8_t bd;
    int reset = c->pending_reset;

    if (reset >= 0) {
        c->pending_reset = -1;
        machine_reset(c, reset);
    }

    c->frame_start_cycles = c->cpu.cycles;
    for (line = 0; line < ADAM_FRAME_LINES; line++) {
        uint64_t target =
            c->frame_start_cycles + (uint64_t)(line + 1) * ADAM_LINE_TSTATES;
        while (c->cpu.cycles < target)
            z80_step(&c->cpu);

        if (line < TMS_ACTIVE_H) {
            uint16_t *row =
                &c->fb[(line + ADAM_BORDER_TOP) * ADAMCORE_FB_WIDTH];
            int x;
            tms_render_line(&c->vdp, line);
            for (x = 0; x < ADAMCORE_FB_WIDTH; x++)
                row[x] = c->palette_rgb[c->vdp.line[x]];
        } else if (line == TMS_ACTIVE_H) {
            if (tms_vblank(&c->vdp)) {
                z80_nmi(&c->cpu);
                c->nmi_count++;
            }
        }
        adamnet_scan(&c->an);
    }

    /* borders */
    bd = tms_backdrop(&c->vdp);
    {
        uint16_t bc = c->palette_rgb[bd];
        int i;
        for (i = 0; i < ADAM_BORDER_TOP * ADAMCORE_FB_WIDTH; i++)
            c->fb[i] = bc;
        for (i = (ADAMCORE_FB_HEIGHT - ADAM_BORDER_TOP) * ADAMCORE_FB_WIDTH;
             i < ADAMCORE_FB_HEIGHT * ADAMCORE_FB_WIDTH; i++)
            c->fb[i] = bc;
    }

    if (memcmp(c->fb, c->fb_prev, sizeof(c->fb)) != 0) {
        memcpy(c->fb_prev, c->fb, sizeof(c->fb));
        return 1;
    }
    return 0;
}

const uint16_t *adamcore_framebuffer(const adamcore *c, int *w, int *h)
{
    if (w) *w = ADAMCORE_FB_WIDTH;
    if (h) *h = ADAMCORE_FB_HEIGHT;
    return c->fb;
}

int adamcore_render_audio(adamcore *c, int16_t *out, int nsamples)
{
    sn_render(&c->psg, out, nsamples);
    return nsamples;
}

void adamcore_inject_key(adamcore *c, uint8_t adam_char)
{
    uint32_t w = c->keyq_w;
    if (((w + 1) & (KEYQ_LEN - 1)) == (c->keyq_r & (KEYQ_LEN - 1)))
        return;
    c->keyq[w & (KEYQ_LEN - 1)] = adam_char;
    c->keyq_w = w + 1;
}

void adamcore_set_joystick(adamcore *c, int port, uint16_t state)
{
    if (port >= 0 && port < 2)
        c->joy[port] = state;
}

void adamcore_request_reset(adamcore *c, int mode)
{
    c->pending_reset = mode ? 1 : 0;
}
