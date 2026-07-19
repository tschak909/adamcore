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
 * Synthesis runs on the audio thread from a timestamped SPSC write queue
 * so output follows the host audio clock (pull model).
 */

#include "sn76489.h"

#include <string.h>

/* 2 dB attenuation steps; index 15 mutes. Amplitudes precomputed from
 * 10^(-0.1*step) scaled to a per-channel peak that sums within S16. */
static const int16_t amp_table[16] = {
    6553, 5205, 4134, 3284, 2608, 2072, 1645, 1307,
    1038,  824,  655,  520,  413,  328,  261,    0
};

void sn_reset(sn76489 *s, uint32_t clock, uint32_t rate)
{
    memset(s, 0, sizeof(*s));
    s->clock = clock;
    s->rate = rate;
    s->atten[0] = s->atten[1] = s->atten[2] = s->atten[3] = 0x0F;
    s->lfsr = 0x4000;
    s->period[0] = s->period[1] = s->period[2] = 1;
    s->ticks_per_sample =
        (uint32_t)(((uint64_t)(clock / 16) << 16) / (rate ? rate : 44100));
}

void sn_write(sn76489 *s, uint64_t cpu_cycles, uint8_t val)
{
    uint32_t w = s->qw;
    uint64_t when = cpu_cycles * s->rate / s->clock;
    if (((w + 1) & (SN_QUEUE_LEN - 1)) == (s->qr & (SN_QUEUE_LEN - 1)))
        return; /* queue full; drop oldest-style overrun is not worth a lock */
    s->queue[w & (SN_QUEUE_LEN - 1)].when = when;
    s->queue[w & (SN_QUEUE_LEN - 1)].val = val;
    s->qw = w + 1;
}

static void apply_write(sn76489 *s, uint8_t val)
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

static void tick(sn76489 *s)
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

void sn_render(sn76489 *s, int16_t *out, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        /* apply queued writes due at this output sample */
        while (s->qr != s->qw &&
               s->queue[s->qr & (SN_QUEUE_LEN - 1)].when <= s->sample_pos) {
            apply_write(s, s->queue[s->qr & (SN_QUEUE_LEN - 1)].val);
            s->qr++;
        }

        s->tick_acc += s->ticks_per_sample;
        while (s->tick_acc >= 0x10000) {
            s->tick_acc -= 0x10000;
            tick(s);
        }

        {
            int acc = 0;
            int ch;
            for (ch = 0; ch < 3; ch++)
                acc += s->out[ch] ? amp_table[s->atten[ch]]
                                  : -amp_table[s->atten[ch]];
            acc += s->noise_out ? amp_table[s->atten[3]]
                                : -amp_table[s->atten[3]];
            out[i] = (int16_t)acc;
        }
        s->sample_pos++;
    }
}
