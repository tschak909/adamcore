/*
 * adamcore - General Instrument AY-3-8910. See ay8910.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ay8910.h"

#include "sn76489.h" /* SN_CHANNEL_PEAK, for level parity between the chips */

#include <string.h>

/* Registers the chip does not implement in full read back masked. R14/R15
 * are the I/O ports; R7's bits 6-7 configure them, and on a Super Game
 * Module nothing is wired to them, so they read as inputs float high. */
static const uint8_t reg_mask[AY_NREG] = {
    0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
    0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF
};

/* The amplitude control is logarithmic: 16 levels about 3 dB apart, and the
 * envelope generator resolves the same range in 32 steps, so one envelope
 * step is about 1.5 dB. Peak matches SN_CHANNEL_PEAK so a chip's channel is
 * as loud as the other chip's; three AY channels plus four SN channels can
 * then exceed S16, which is what the saturating store in psg_render is for.
 *
 * amp[n] = SN_CHANNEL_PEAK * 10^(-1.5*(31-n)/20), amp[0] = 0.
 */
static const int16_t amp_table[32] = {
        0,    37,    44,    52,    62,    74,    87,   104,
      123,   147,   174,   207,   246,   293,   348,   413,
      491,   584,   694,   825,   980,  1165,  1385,  1646,
     1956,  2325,  2763,  3284,  3903,  4639,  5514,  6553
};

/* The 4-bit fixed volume selects every other envelope level: level n of 16
 * is 2n+1 of 32. */
static uint8_t fixed_level(uint8_t vol4) { return (uint8_t)(vol4 * 2 + 1); }

void ay_reset(ay8910 *a)
{
    memset(a, 0, sizeof(*a));
    a->noise_lfsr = 1; /* any non-zero seed; 0 would lock the register up */
    a->reg[7] = 0x3F;  /* all tones and noise disabled */
    a->tone_period[0] = a->tone_period[1] = a->tone_period[2] = 1;
    a->noise_period = 1;
    a->env_period = 1;
}

void ay_write_reg(ay8910 *a, uint8_t reg, uint8_t val)
{
    if (reg >= AY_NREG) return;
    a->reg[reg] = (uint8_t)(val & reg_mask[reg]);

    switch (reg) {
    case 0: case 1: case 2: case 3: case 4: case 5: {
        int ch = reg >> 1;
        uint16_t p = (uint16_t)(a->reg[ch * 2] |
                                ((a->reg[ch * 2 + 1] & 0x0F) << 8));
        a->tone_period[ch] = p ? p : 1; /* period 0 behaves as 1 */
        break;
    }
    case 6:
        a->noise_period = (uint16_t)((a->reg[6] & 0x1F) ? (a->reg[6] & 0x1F) : 1);
        break;
    case 11: case 12:
        a->env_period = (uint16_t)(a->reg[11] | (a->reg[12] << 8));
        if (!a->env_period) a->env_period = 1;
        break;
    case 13:
        /* Writing the shape restarts the envelope, whatever the value --
         * this is how a program retriggers a note without changing it. */
        a->env_att = (uint8_t)((a->reg[13] >> 2) & 1);
        a->env_alt = 0;
        a->env_hold = 0;
        a->env_held = 0;
        a->env_pos = 0;
        a->env_counter = a->env_period;
        break;
    default:
        break;
    }
}

uint8_t ay_read_reg(const ay8910 *a, uint8_t reg)
{
    if (reg >= AY_NREG) return 0xFF;
    /* Nothing is connected to the I/O ports on a Super Game Module. Read as
     * inputs (R7 bits 6-7 clear at reset), they float high. */
    if (reg >= 14 && !((a->reg[7] >> (reg - 8)) & 1)) return 0xFF;
    return (uint8_t)(a->reg[reg] & reg_mask[reg]);
}

/* Envelope level for the current position, honouring ATTACK and the running
 * ALTERNATE toggle. */
static uint8_t env_level(const ay8910 *a)
{
    if (a->env_hold) return a->env_held;
    return (a->env_att ^ a->env_alt) ? a->env_pos
                                     : (uint8_t)(31 - a->env_pos);
}

/* One envelope step. The eight distinct shapes come out of three bits --
 * CONTINUE (3), ALTERNATE (1) and HOLD (0) -- applied when a ramp completes;
 * ATTACK (2) only picks the ramp's direction. */
static void env_advance(ay8910 *a)
{
    uint8_t sh;
    if (a->env_hold) return;
    if (++a->env_pos <= 31) return;

    sh = a->reg[13];
    if (!(sh & 0x08)) {
        /* Not continuing: one ramp, then silence, whichever way it ran. */
        a->env_hold = 1;
        a->env_held = 0;
        a->env_pos = 31;
        return;
    }
    if (sh & 0x02) a->env_alt ^= 1; /* ALTERNATE flips the ramp direction */
    if (sh & 0x01) {
        /* HOLD: stop at the level the shape ends on. \___ and /___ hold
         * low; \~~~ and /~~~ hold high. */
        a->env_hold = 1;
        a->env_held = (uint8_t)(((sh >> 2) ^ (sh >> 1)) & 1 ? 31 : 0);
        a->env_pos = 31;
        return;
    }
    a->env_pos = 0;
}

void ay_tick(ay8910 *a)
{
    int ch;

    for (ch = 0; ch < 3; ch++) {
        if (--a->tone_counter[ch] == 0 || a->tone_counter[ch] > 0x1000) {
            a->tone_counter[ch] = a->tone_period[ch];
            a->tone_out[ch] ^= 1;
        }
    }

    if (--a->noise_counter == 0 || a->noise_counter > 0x1000) {
        a->noise_counter = a->noise_period;
        /* 17-bit shift register, feedback from bits 0 and 3 (x^17+x^14+1). */
        a->noise_lfsr = (a->noise_lfsr >> 1) |
            (((a->noise_lfsr ^ (a->noise_lfsr >> 3)) & 1) << 16);
        a->noise_out = (uint8_t)(a->noise_lfsr & 1);
    }

    if (--a->env_counter == 0 || a->env_counter > 0x8000) {
        a->env_counter = a->env_period;
        env_advance(a);
    }
}

int ay_mix(const ay8910 *a)
{
    uint8_t mixer = a->reg[7];
    uint8_t env = env_level(a);
    int acc = 0;
    int ch;

    for (ch = 0; ch < 3; ch++) {
        /* A disable bit does not silence the source, it removes it from the
         * gate: a channel with both disabled sits at its amplitude, which is
         * how a program plays pure envelope. */
        int t = a->tone_out[ch] | ((mixer >> ch) & 1);
        int nz = a->noise_out | ((mixer >> (ch + 3)) & 1);
        uint8_t amp = a->reg[8 + ch];
        uint8_t level = (amp & 0x10) ? env : fixed_level((uint8_t)(amp & 0x0F));
        if (t & nz)
            acc += amp_table[level];
    }
    return acc;
}
