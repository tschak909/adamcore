/*
 * adamcore - SN76489AN Programmable Sound Generator (chip model only)
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * State and behaviour of the chip and nothing else. The timestamped write
 * queue, the render timeline and the mix live in psg.c, which drives this and
 * -- on a machine with an Opcode Super Game Module -- an AY-3-8910 from one
 * shared clock. Two chips with two timelines would drift against each other
 * mid-note, so there is only ever one.
 */

#ifndef ADAMCORE_SN76489_H
#define ADAMCORE_SN76489_H

#include <stdint.h>

typedef struct {
    uint16_t period[3];
    int16_t counter[3];
    uint8_t out[3];
    uint8_t atten[4]; /* 3 tones + noise, 0x0F = silent */
    uint8_t noise_ctrl;
    uint16_t lfsr;
    int16_t noise_counter;
    uint8_t noise_out;
    uint8_t latched_reg;
} sn76489;

/* Peak per channel. Four channels sum to 26212, inside S16 with room to
 * spare -- which is what makes adding the AY's three channels safe without
 * rescaling this one and quietly making every existing machine quieter. */
#define SN_CHANNEL_PEAK 6553

void sn_reset(sn76489 *s);

/* One bus write to the chip's single data port. */
void sn_write_reg(sn76489 *s, uint8_t val);

/* One tick of the chip's internal clock (input clock / 16). */
void sn_tick(sn76489 *s);

/* Current output level, summed across the three tones and the noise. */
int sn_mix(const sn76489 *s);

#endif
