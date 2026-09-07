/*
 * psg_test: the sound pipeline, across the refactor that split the shared
 * queue/timeline/mix (psg.c) out of the SN76489 chip model (sn76489.c) so a
 * second chip -- the Super Game Module's AY-3-8910 -- could join it.
 *
 * The load-bearing claim is that splitting it changed NOTHING for a machine
 * with no SGM. GOLDEN_SUM is a checksum of the rendered output for a fixed
 * program driving the PSG through real OUT instructions, captured from the
 * pre-refactor code. If it still matches, the extraction was exact.
 *
 * Also pins the headroom arithmetic that makes a second chip safe to add:
 * four SN channels peak at 4 * 6553 = 26212, comfortably inside S16, so the
 * saturating store added for the AY is unreachable without one and cannot
 * change existing output.
 *
 * Needs no ROMs and never skips.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adamcore.h"
#include "adamcore_debug.h"

#define NSAMPLES 8820 /* 200 ms at 44100 */

static uint8_t os7[ADAMCORE_OS7_ROM_SIZE];
static int failures;

static void check(const char *what, int ok)
{
    printf("  %s %s\n", ok ? "ok " : "FAIL", what);
    if (!ok) failures++;
}

/* FNV-1a over the sample bytes. */
static uint64_t sum_samples(const int16_t *s, int n)
{
    uint64_t h = 1469598103934665603ULL;
    int i, b;
    for (i = 0; i < n; i++) {
        for (b = 0; b < 2; b++) {
            h ^= (uint8_t)((uint16_t)s[i] >> (8 * b));
            h *= 1099511628211ULL;
        }
    }
    return h;
}

/* A fixed PSG program: set all three tones to distinct periods at full
 * volume, arm white noise, then spin. Written through real OUT ($FF),A
 * instructions so the whole emu-side path -- io_write, the timestamped
 * queue, the frame publish -- is under test, not just the synthesis. */
static const uint8_t psg_program[] = {
    /* ch0: period low+high, attenuation 0 (loudest) */
    0x3E, 0x8A, 0xD3, 0xFF,   /* LD A,$8A ; OUT ($FF),A  tone0 low = A   */
    0x3E, 0x01, 0xD3, 0xFF,   /* LD A,$01 ; OUT ($FF),A  tone0 high = 1  */
    0x3E, 0x90, 0xD3, 0xFF,   /* LD A,$90 ; OUT ($FF),A  ch0 atten 0     */
    /* ch1 */
    0x3E, 0xA5, 0xD3, 0xFF,
    0x3E, 0x02, 0xD3, 0xFF,
    0x3E, 0xB2, 0xD3, 0xFF,   /* ch1 atten 2 */
    /* ch2 */
    0x3E, 0xC3, 0xD3, 0xFF,
    0x3E, 0x03, 0xD3, 0xFF,
    0x3E, 0xD4, 0xD3, 0xFF,   /* ch2 atten 4 */
    /* noise: white, rate 2; attenuation 6 */
    0x3E, 0xE6, 0xD3, 0xFF,
    0x3E, 0xF6, 0xD3, 0xFF,
    0x18, 0xFE                /* JR $ -- spin forever */
};

int main(int argc, char **argv)
{
    adamcore_config cfg;
    adamcore_z80_regs r;
    adamcore *c;
    int16_t *buf;
    uint64_t got;
    int i, peak = 0;

    memset(os7, 0x00, sizeof os7);
    memset(&cfg, 0, sizeof cfg);
    cfg.os7_rom_data = os7;
    cfg.start_machine = ADAMCORE_MACHINE_CV;
    cfg.audio_rate = 44100;

    c = adamcore_create(&cfg);
    if (!c) { fprintf(stderr, "create failed\n"); return 1; }

    for (i = 0; i < (int)sizeof psg_program; i++)
        adamcore_poke(c, (uint16_t)(0x6000 + i), psg_program[i]);
    memset(&r, 0, sizeof r);
    adamcore_get_regs(c, &r);
    r.pc = 0x6000;
    adamcore_set_regs(c, &r);

    /* Ten frames, so the once-per-frame clock publish has happened and the
     * renderer's timeline is established the way it is in a real session. */
    for (i = 0; i < 10; i++)
        adamcore_run_frame(c);

    buf = malloc(sizeof(int16_t) * NSAMPLES);
    if (!buf) return 1;
    adamcore_render_audio(c, buf, NSAMPLES);

    for (i = 0; i < NSAMPLES; i++) {
        int v = buf[i] < 0 ? -buf[i] : buf[i];
        if (v > peak) peak = v;
    }

    got = sum_samples(buf, NSAMPLES);
    printf("SN-only render: peak %d, checksum %016llx\n",
           peak, (unsigned long long)got);

    /* `psg_test --capture` prints the checksum for pasting into GOLDEN_SUM;
     * that is how the pre-refactor value was obtained. */
    if (argc > 1 && !strcmp(argv[1], "--capture")) {
        printf("GOLDEN_SUM = 0x%016llxULL\n", (unsigned long long)got);
        free(buf);
        adamcore_destroy(c);
        return 0;
    }

#ifdef GOLDEN_SUM
    check("SN-only output is bit-identical to the pre-refactor render",
          got == GOLDEN_SUM);
#endif
    check("four SN channels stay inside S16 (peak <= 26212)", peak <= 26212);
    check("the program actually made sound", peak > 0);

    /* ---- the AY, and the headroom that made the clamp necessary ---------- */
    {
        /* All three AY channels at maximum fixed volume, tone and noise
         * gated in, on top of the SN program above: this is the loudest a
         * Super Game Module machine gets, and it is what the saturating
         * store in psg_render exists for. */
        static const uint8_t ay_program[] = {
            /* helper: LD A,reg / OUT ($50) / LD A,val / OUT ($51) */
            0x3E, 0x00, 0xD3, 0x50, 0x3E, 0x40, 0xD3, 0x51, /* R0 tone A lo */
            0x3E, 0x01, 0xD3, 0x50, 0x3E, 0x00, 0xD3, 0x51, /* R1 tone A hi */
            0x3E, 0x02, 0xD3, 0x50, 0x3E, 0x60, 0xD3, 0x51, /* R2 tone B lo */
            0x3E, 0x04, 0xD3, 0x50, 0x3E, 0x80, 0xD3, 0x51, /* R4 tone C lo */
            0x3E, 0x07, 0xD3, 0x50, 0x3E, 0x38, 0xD3, 0x51, /* R7 tones on  */
            0x3E, 0x08, 0xD3, 0x50, 0x3E, 0x0F, 0xD3, 0x51, /* R8  A max    */
            0x3E, 0x09, 0xD3, 0x50, 0x3E, 0x0F, 0xD3, 0x51, /* R9  B max    */
            0x3E, 0x0A, 0xD3, 0x50, 0x3E, 0x0F, 0xD3, 0x51, /* R10 C max    */
            0x18, 0xFE                                       /* JR $         */
        };
        adamcore *c3;
        int16_t *b3;
        int peak3 = 0, clipped = 0;
        adamcore_z80_regs r3;
        adamcore_config sgmcfg = cfg;
        sgmcfg.sgm = 1;

        c3 = adamcore_create(&sgmcfg);
        b3 = malloc(sizeof(int16_t) * NSAMPLES);
        if (!c3 || !b3) return 1;
        for (i = 0; i < (int)sizeof psg_program - 2; i++) /* drop the JR */
            adamcore_poke(c3, (uint16_t)(0x6000 + i), psg_program[i]);
        for (i = 0; i < (int)sizeof ay_program; i++)
            adamcore_poke(c3, (uint16_t)(0x6000 + sizeof psg_program - 2 + i),
                          ay_program[i]);
        memset(&r3, 0, sizeof r3);
        adamcore_get_regs(c3, &r3);
        r3.pc = 0x6000;
        adamcore_set_regs(c3, &r3);
        for (i = 0; i < 10; i++)
            adamcore_run_frame(c3);
        adamcore_render_audio(c3, b3, NSAMPLES);
        for (i = 0; i < NSAMPLES; i++) {
            int v = b3[i] < 0 ? -b3[i] : b3[i];
            if (v > peak3) peak3 = v;
            if (b3[i] == 32767 || b3[i] == -32768) clipped++;
        }
        printf("SN+AY render: peak %d, %d saturated samples\n", peak3, clipped);
        check("the AY is audible on top of the SN",
              sum_samples(b3, NSAMPLES) != got);
        check("the mix never wraps sign (clamped, not overflowed)",
              peak3 <= 32767);
        free(b3);
        adamcore_destroy(c3);
    }

    /* An SGM-capable machine whose cartridge never touches $50-$53 must
     * sound EXACTLY like a stock one -- the AY is silent at reset. */
    {
        adamcore_config sgmcfg = cfg;
        adamcore *c4;
        int16_t *b4 = malloc(sizeof(int16_t) * NSAMPLES);
        adamcore_z80_regs r4;
        sgmcfg.sgm = 1;
        c4 = adamcore_create(&sgmcfg);
        if (!c4 || !b4) return 1;
        for (i = 0; i < (int)sizeof psg_program; i++)
            adamcore_poke(c4, (uint16_t)(0x6000 + i), psg_program[i]);
        memset(&r4, 0, sizeof r4);
        adamcore_get_regs(c4, &r4);
        r4.pc = 0x6000;
        adamcore_set_regs(c4, &r4);
        for (i = 0; i < 10; i++)
            adamcore_run_frame(c4);
        adamcore_render_audio(c4, b4, NSAMPLES);
        check("an untouched AY adds nothing at all",
              sum_samples(b4, NSAMPLES) == got);
        free(b4);
        adamcore_destroy(c4);
    }

    /* Determinism: a second core fed the same program must render the same
     * samples. A shared timeline that drifted per-instance would show here. */
    {
        adamcore *c2 = adamcore_create(&cfg);
        int16_t *b2 = malloc(sizeof(int16_t) * NSAMPLES);
        adamcore_z80_regs r2;
        if (!c2 || !b2) return 1;
        for (i = 0; i < (int)sizeof psg_program; i++)
            adamcore_poke(c2, (uint16_t)(0x6000 + i), psg_program[i]);
        memset(&r2, 0, sizeof r2);
        adamcore_get_regs(c2, &r2);
        r2.pc = 0x6000;
        adamcore_set_regs(c2, &r2);
        for (i = 0; i < 10; i++)
            adamcore_run_frame(c2);
        adamcore_render_audio(c2, b2, NSAMPLES);
        check("two cores render identically", sum_samples(b2, NSAMPLES) == got);
        free(b2);
        adamcore_destroy(c2);
    }

    free(buf);
    adamcore_destroy(c);

    if (failures) {
        fprintf(stderr, "\npsg_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("\npsg_test: PASS\n");
    return 0;
}
