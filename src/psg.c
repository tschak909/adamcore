/*
 * adamcore - the shared sound pipeline. See psg.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "psg.h"

#include <string.h>

void psg_reset(psg *p, uint32_t clock, uint32_t rate, int have_ay)
{
    memset(p, 0, sizeof(*p));
    p->clock = clock;
    p->rate = rate ? rate : 44100;
    p->have_ay = have_ay;

    sn_reset(&p->sn);
    ay_reset(&p->ay);

    /* The SN divides its input clock by 16. The AY on a Super Game Module is
     * clocked at half the CPU rate and divides by 16 as well, so its tick
     * rate is exactly half the SN's -- but each chip gets its own accumulator
     * rather than the SN's being toggled, so nothing here depends on that
     * relationship holding. */
    p->sn_ticks_per_sample =
        (uint32_t)(((uint64_t)(clock / 16) << 16) / p->rate);
    p->ay_ticks_per_sample =
        (uint32_t)(((uint64_t)(clock / 2 / 16) << 16) / p->rate);
}

void psg_publish(psg *p, uint64_t cpu_cycles)
{
    p->emu_pos = cpu_cycles * p->rate / p->clock;
}

static void enqueue(psg *p, uint64_t cpu_cycles, uint8_t target, uint8_t reg,
                    uint8_t val)
{
    uint32_t w = p->qw;
    uint64_t when = cpu_cycles * p->rate / p->clock;
    if (((w + 1) & (PSG_QUEUE_LEN - 1)) == (p->qr & (PSG_QUEUE_LEN - 1)))
        return; /* queue full; a lock here would cost more than it saves */
    p->queue[w & (PSG_QUEUE_LEN - 1)].when = when;
    p->queue[w & (PSG_QUEUE_LEN - 1)].target = target;
    p->queue[w & (PSG_QUEUE_LEN - 1)].reg = reg;
    p->queue[w & (PSG_QUEUE_LEN - 1)].val = val;
    p->qw = w + 1;
}

void psg_write_sn(psg *p, uint64_t cpu_cycles, uint8_t val)
{
    enqueue(p, cpu_cycles, PSG_TARGET_SN, 0, val);
}

void psg_write_ay(psg *p, uint64_t cpu_cycles, uint8_t reg, uint8_t val)
{
    enqueue(p, cpu_cycles, PSG_TARGET_AY, reg, val);
}

/* Hand one queued write to whichever chip it names. A resync that skipped
 * the AY's entries would swallow a whole register setup and restart the
 * music wrong, so both chips are drained by the same loop. */
static void apply(psg *p, uint32_t idx)
{
    uint32_t i = idx & (PSG_QUEUE_LEN - 1);
    if (p->queue[i].target == PSG_TARGET_AY)
        ay_write_reg(&p->ay, p->queue[i].reg, p->queue[i].val);
    else
        sn_write_reg(&p->sn, p->queue[i].val);
}

void psg_render(psg *p, int16_t *out, int n)
{
    /* Re-sync the render timeline to the emulator's published clock.
     * Without this, the gap between core start and the first audio pull
     * becomes a permanent delay, and the ~0.1% difference between the
     * vsync-locked frame clock and the nominal NTSC rate accumulates
     * without bound. Trail the emulator by ~50 ms; snap (applying any
     * skipped writes in order) when drift leaves the window. */
    enum { TARGET_LEAD = 2205, MAX_LEAD = 8820 };
    uint64_t emu_pos = p->emu_pos;
    int64_t lead = (int64_t)(emu_pos - p->sample_pos);
    int i;

    if (emu_pos > TARGET_LEAD && (lead < 0 || lead > MAX_LEAD)) {
        uint64_t target = emu_pos - TARGET_LEAD;
        while (p->qr != p->qw &&
               p->queue[p->qr & (PSG_QUEUE_LEN - 1)].when <= target) {
            apply(p, p->qr);
            p->qr++;
        }
        p->sample_pos = target;
    }

    for (i = 0; i < n; i++) {
        int acc;

        /* apply queued writes due at this output sample */
        while (p->qr != p->qw &&
               p->queue[p->qr & (PSG_QUEUE_LEN - 1)].when <= p->sample_pos) {
            apply(p, p->qr);
            p->qr++;
        }

        p->sn_acc += p->sn_ticks_per_sample;
        while (p->sn_acc >= 0x10000) {
            p->sn_acc -= 0x10000;
            sn_tick(&p->sn);
        }

        acc = sn_mix(&p->sn);

        if (p->have_ay) {
            p->ay_acc += p->ay_ticks_per_sample;
            while (p->ay_acc >= 0x10000) {
                p->ay_acc -= 0x10000;
                ay_tick(&p->ay);
            }
            acc += ay_mix(&p->ay);
        }

        /* Saturate rather than wrap. Unreachable on a machine with no SGM --
         * four SN channels peak at 26212, well inside S16 -- so this cannot
         * change a single existing sample; it is here because the AY's three
         * channels can push the sum past the rail. */
        if (acc > 32767) acc = 32767;
        else if (acc < -32768) acc = -32768;
        out[i] = (int16_t)acc;

        p->sample_pos++;
    }
}
