/*
 * adamcore - SN76489AN Programmable Sound Generator
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ADAMCORE_SN76489_H
#define ADAMCORE_SN76489_H

#include <stdint.h>

#define SN_QUEUE_LEN 4096 /* power of two */

typedef struct {
    /* synthesis state (audio-thread side) */
    uint16_t period[3];
    int16_t counter[3];
    uint8_t out[3];
    uint8_t atten[4]; /* 3 tones + noise, 0x0F = silent */
    uint8_t noise_ctrl;
    uint16_t lfsr;
    int16_t noise_counter;
    uint8_t noise_out;
    uint8_t latched_reg;

    /* register mirror (emu-thread side, for latch semantics) */
    uint8_t wlatch;

    uint32_t clock;      /* PSG input clock, Hz */
    uint32_t rate;       /* output sample rate */
    uint64_t sample_pos; /* samples synthesized so far */
    uint32_t tick_acc;   /* 16.16 fixed-point tick accumulator */
    uint32_t ticks_per_sample; /* 16.16: (clock/16)/rate */

    /* SPSC timestamped write queue: emu thread produces, audio consumes */
    struct { uint64_t when; uint8_t val; } queue[SN_QUEUE_LEN];
    volatile uint32_t qw, qr;
} sn76489;

void sn_reset(sn76489 *s, uint32_t clock, uint32_t rate);

/* Emu thread: queue a PSG bus write occurring at absolute CPU cycle count. */
void sn_write(sn76489 *s, uint64_t cpu_cycles, uint8_t val);

/* Audio thread: synthesize n mono S16 samples. */
void sn_render(sn76489 *s, int16_t *out, int n);

#endif
