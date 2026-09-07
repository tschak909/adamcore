/*
 * adamcore - the sound pipeline: one timestamped write queue, one render
 * timeline, one mix, driving every sound chip the machine has.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A ColecoVision has an SN76489. With an Opcode Super Game Module plugged in
 * it also has an AY-3-8910, and the two must share this file rather than each
 * owning a renderer: psg_render() resynchronises its timeline to the
 * emulator's published clock and can jump sample_pos by thousands of samples
 * when drift leaves the window, so two renderers making that decision
 * independently would slide against each other mid-note. One queue, one
 * sample_pos, one resync -- and a per-chip tick accumulator, because the
 * chips divide their clocks differently.
 *
 * Queue entries name their target chip and carry a resolved (reg, value)
 * rather than a raw bus byte. That matters for the AY, whose bus protocol is
 * an address latch on one port and data on another: sn_write's silent
 * drop-on-full costs the SN a single register, but dropping half of a latch
 * pair would desynchronise the address for the rest of the session.
 * Resolving the register on the emulator thread -- which has to keep a
 * register mirror anyway, because the AY's data port is readable -- makes a
 * dropped entry cost exactly one register write there too.
 */

#ifndef ADAMCORE_PSG_H
#define ADAMCORE_PSG_H

#include <stdint.h>

#include "ay8910.h"
#include "sn76489.h"

#define PSG_QUEUE_LEN 8192 /* power of two; two chips, same OUT-bounded rate */

enum { PSG_TARGET_SN = 0, PSG_TARGET_AY = 1 };

typedef struct {
    sn76489 sn;
    ay8910 ay;
    int have_ay; /* an SGM is fitted */

    uint32_t clock;            /* CPU / SN clock, Hz */
    uint32_t rate;             /* output sample rate */
    uint64_t sample_pos;       /* samples synthesized so far */
    volatile uint64_t emu_pos; /* emulator's position on the sample clock */

    uint32_t sn_acc, sn_ticks_per_sample; /* 16.16 */
    uint32_t ay_acc, ay_ticks_per_sample; /* 16.16 */

    /* SPSC timestamped write queue: emu thread produces, audio consumes */
    struct {
        uint64_t when;
        uint8_t target;
        uint8_t reg;
        uint8_t val;
    } queue[PSG_QUEUE_LEN];
    volatile uint32_t qw, qr;
} psg;

/* have_ay fits the Super Game Module's AY-3-8910. */
void psg_reset(psg *p, uint32_t clock, uint32_t rate, int have_ay);

/* Emu thread: queue a bus write occurring at an absolute CPU cycle count. */
void psg_write_sn(psg *p, uint64_t cpu_cycles, uint8_t val);
void psg_write_ay(psg *p, uint64_t cpu_cycles, uint8_t reg, uint8_t val);

/* Emu thread, once per frame: publish the emulated clock so the renderer can
 * keep its timeline a fixed short latency behind the emulator. */
void psg_publish(psg *p, uint64_t cpu_cycles);

/* Audio thread: synthesize n mono S16 samples. */
void psg_render(psg *p, int16_t *out, int n);

#endif
