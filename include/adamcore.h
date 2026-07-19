/*
 * adamcore - clean-room Coleco ADAM / ColecoVision emulator core
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

#ifndef ADAMCORE_H
#define ADAMCORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADAMCORE_FB_WIDTH  256
#define ADAMCORE_FB_HEIGHT 212 /* 192 active + 10/10 vertical border */

typedef struct adamcore adamcore;

typedef enum {
    ADAMCORE_MACHINE_ADAM = 0, /* EOS / SmartWriter; reset mode 0 */
    ADAMCORE_MACHINE_CV   = 1  /* ColecoVision OS7 + cartridge; reset mode 1 */
} adamcore_machine;

typedef struct {
    const char *os7_rom_path; /* 8K ColecoVision OS-7 BIOS (required) */
    const char *eos_rom_path; /* 8K ADAM EOS (required for ADAM mode) */
    const char *wp_rom_path;  /* 32K SmartWriter (required for ADAM mode) */
    const char *cart_path;    /* optional cartridge image */

    adamcore_machine start_machine;

    int boip_listen_port; /* AdamNet-over-TCP listener on loopback; 0 = off */
    int palette;          /* 0..3 */
    int expansion;        /* controller expansion module 0..7 */
    int joystick_mode;    /* 0 none, 1 both ports, 2 port 2 only, 3 port 1 only */
    int swap_buttons;     /* 0/1 */
    int reverse_keypad;   /* 0/1 */
    int audio_rate;       /* host sample rate, e.g. 44100 */
} adamcore_config;

/* Create loads the ROMs and, if configured, opens the BoIP listener.
 * Returns NULL on failure (missing/short ROM, socket error). */
adamcore *adamcore_create(const adamcore_config *cfg);
void      adamcore_destroy(adamcore *c);

/* Emulation-thread only. Runs one NTSC frame (262 lines x 228 T-states).
 * Returns 1 if the framebuffer changed since the previous call, else 0. */
int adamcore_run_frame(adamcore *c);

/* Emulation-thread only. RGB565, tightly packed, valid until the next
 * run_frame call. w and h receive ADAMCORE_FB_WIDTH/HEIGHT. */
const uint16_t *adamcore_framebuffer(const adamcore *c, int *w, int *h);

/* Audio-thread safe. Synthesizes up to nsamples mono S16 samples at the
 * configured rate from the timestamped PSG write queue. Returns the number
 * of samples written (always nsamples; synthesizes silence/held tone if the
 * emulation thread is behind). */
int adamcore_render_audio(adamcore *c, int16_t *out, int nsamples);

/* Any-thread safe; queued and applied at the next frame boundary. */
void adamcore_inject_key(adamcore *c, uint8_t adam_char);
void adamcore_set_joystick(adamcore *c, int port, uint16_t state);
void adamcore_request_reset(adamcore *c, int mode); /* 0 = ADAM, 1 = CV */

#ifdef __cplusplus
}
#endif

#endif /* ADAMCORE_H */
