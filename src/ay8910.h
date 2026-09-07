/*
 * adamcore - General Instrument AY-3-8910 Programmable Sound Generator
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The sound half of the Opcode Super Game Module. Chip model only: psg.c
 * owns the write queue, the timeline and the mix, and calls ay_tick/ay_mix
 * from the audio thread.
 *
 * Written from the GI AY-3-8910/8912 data sheet (register map, the 16-level
 * logarithmic amplitude control and its 5-bit envelope resolution, the
 * envelope shape decode, the tone and envelope dividers and the 17-bit noise
 * shift register). No emulator source was consulted -- see PROVENANCE.md.
 */

#ifndef ADAMCORE_AY8910_H
#define ADAMCORE_AY8910_H

#include <stdint.h>

#define AY_NREG 16

typedef struct {
    uint8_t reg[AY_NREG];

    uint16_t tone_period[3];
    uint16_t tone_counter[3];
    uint8_t tone_out[3];

    uint16_t noise_period;
    uint16_t noise_counter;
    uint32_t noise_lfsr; /* 17 bits */
    uint8_t noise_out;

    uint16_t env_period;
    uint16_t env_counter;
    uint8_t env_pos;   /* 0..31 */
    uint8_t env_att;   /* shape bit 2 */
    uint8_t env_alt;   /* running ALTERNATE toggle */
    uint8_t env_hold;  /* ramp finished; hold env_held */
    uint8_t env_held;  /* level held after the ramp */
} ay8910;

void ay_reset(ay8910 *a);

/* One register write. psg.c resolves the AY's address-latch/data bus
 * protocol on the emulator thread, so this takes a register number, never a
 * raw bus byte -- see psg.h for why that distinction is load bearing. */
void ay_write_reg(ay8910 *a, uint8_t reg, uint8_t val);

/* Register read-back, masked to the width the chip actually implements.
 * Port $52 on a Super Game Module reads this. */
uint8_t ay_read_reg(const ay8910 *a, uint8_t reg);

/* One tick of the chip's internal clock (input clock / 16). */
void ay_tick(ay8910 *a);

/* Current output level, summed across the three channels. Unipolar, as the
 * chip is: its DAC swings from silence up to peak, unlike the SN76489's
 * square wave about zero. */
int ay_mix(const ay8910 *a);

#endif
