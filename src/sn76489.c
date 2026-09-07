/*
 * adamcore - SN76489AN Programmable Sound Generator
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Implemented from the TI SN76489 datasheet plus published ColecoVision
 * notes on the noise generator: 15-bit LFSR seeded at 0x4000 on noise
 * control writes; white noise feedback XORs the two low taps; periodic
 * mode recirculates bit 0. Tone period 0 counts as 1024 (SN76489AN).
 *
 * The chip divides its input clock by 16; tone frequency = clock/(32*N).
 * This file is the chip alone: psg.c owns the timestamped write queue, the
 * render timeline and the mix, and calls sn_tick/sn_mix from the audio
 * thread.
 */

#include "sn76489.h"

#include <string.h>

/* 2 dB attenuation steps; index 15 mutes. Amplitudes precomputed from
 * 10^(-0.1*step) scaled to a per-channel peak that sums within S16. */
static const int16_t amp_table[16] = {
    6553, 5205, 4134, 3284, 2608, 2072, 1645, 1307,
    1038,  824,  655,  520,  413,  328,  261,    0
};

void sn_reset(sn76489 *s)
{
    memset(s, 0, sizeof(*s));
    s->atten[0] = s->atten[1] = s->atten[2] = s->atten[3] = 0x0F;
    s->lfsr = 0x4000;
    s->period[0] = s->period[1] = s->period[2] = 1;
}

void sn_write_reg(sn76489 *s, uint8_t val)
{
    if (val & 0x80) {
        uint8_t reg = (uint8_t)((val >> 4) & 7);
        s->latched_reg = reg;
        switch (reg) {
        case 0: case 2: case 4: { /* tone period low nibble */
            int ch = reg >> 1;
            s->period[ch] = (uint16_t)((s->period[ch] & 0x3F0) | (val & 0x0F));
            break;
        }
        case 6: /* noise control */
            s->noise_ctrl = (uint8_t)(val & 0x07);
            s->lfsr = 0x4000;
            break;
        default: /* attenuation */
            s->atten[reg >> 1] = (uint8_t)(val & 0x0F);
            break;
        }
    } else {
        uint8_t reg = s->latched_reg;
        switch (reg) {
        case 0: case 2: case 4: { /* tone period high 6 bits */
            int ch = reg >> 1;
            s->period[ch] =
                (uint16_t)((s->period[ch] & 0x00F) | ((val & 0x3F) << 4));
            break;
        }
        case 6:
            s->noise_ctrl = (uint8_t)(val & 0x07);
            s->lfsr = 0x4000;
            break;
        default:
            s->atten[reg >> 1] = (uint8_t)(val & 0x0F);
            break;
        }
    }
}

static void noise_shift(sn76489 *s)
{
    uint16_t in;
    if (s->noise_ctrl & 0x04)
        in = (uint16_t)(((s->lfsr ^ (s->lfsr >> 1)) & 1) << 14); /* white */
    else
        in = (uint16_t)((s->lfsr & 1) << 14); /* periodic */
    s->lfsr = (uint16_t)((s->lfsr >> 1) | in);
    s->noise_out = (uint8_t)(s->lfsr & 1);
}

void sn_tick(sn76489 *s)
{
    int ch;
    for (ch = 0; ch < 3; ch++) {
        if (--s->counter[ch] <= 0) {
            uint16_t p = s->period[ch] ? s->period[ch] : 1024;
            s->counter[ch] = (int16_t)p;
            s->out[ch] ^= 1;
            /* noise rate 3 clocks from tone 2 output transitions */
            if (ch == 2 && (s->noise_ctrl & 0x03) == 0x03 && s->out[2])
                noise_shift(s);
        }
    }
    if ((s->noise_ctrl & 0x03) != 0x03) {
        if (--s->noise_counter <= 0) {
            s->noise_counter = (int16_t)(32 << (s->noise_ctrl & 0x03));
            noise_shift(s);
        }
    }
}

int sn_mix(const sn76489 *s)
{
    int acc = 0;
    int ch;
    for (ch = 0; ch < 3; ch++)
        acc += s->out[ch] ? amp_table[s->atten[ch]] : -amp_table[s->atten[ch]];
    acc += s->noise_out ? amp_table[s->atten[3]] : -amp_table[s->atten[3]];
    return acc;
}
