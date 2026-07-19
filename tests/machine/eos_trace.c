/*
 * adamcore - EOS boot hardware-interaction tracer (development tool)
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Boots the ADAM ROMs with the AdamNet master stubbed out and logs how
 * EOS BOOT talks to the hardware: ADAM-side I/O ports and high-RAM
 * structure writes/polls. Used to pin down the PCB/DCB contract the
 * HLE master must satisfy (EOS's observable behavior is the spec).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"

static adamcore *C;
static uint8_t (*orig_rd)(void *, uint16_t);
static void (*orig_wr)(void *, uint16_t, uint8_t);
static uint8_t (*orig_in)(void *, uint16_t);
static void (*orig_out)(void *, uint16_t, uint8_t);

static unsigned long rd_count[0x10000];
static int wr_events;

static uint8_t t_rd(void *ud, uint16_t a)
{
    if (a >= 0xF000) rd_count[a]++;
    return orig_rd(ud, a);
}

static void t_wr(void *ud, uint16_t a, uint8_t v)
{
    if (a >= 0xFEC0 && a <= 0xFED8 && wr_events < 400 && C->cpu.pc != 0x80BF && C->cpu.pc != 0xF914) {
        printf("W %04X <- %02X  (pc=%04X)\n", a, v, C->cpu.pc);
        wr_events++;
    }
    orig_wr(ud, a, v);
}

static uint8_t t_in(void *ud, uint16_t p)
{
    uint8_t v = orig_in(ud, p);
    if ((uint8_t)p < 0x80)
        printf("IN  %02X -> %02X (pc=%04X)\n", (uint8_t)p, v, C->cpu.pc);
    return v;
}

static void t_out(void *ud, uint16_t p, uint8_t v)
{
    if ((uint8_t)p < 0x80)
        printf("OUT %02X <- %02X (pc=%04X)\n", (uint8_t)p, v, C->cpu.pc);
    orig_out(ud, p, v);
}

int main(int argc, char **argv)
{
    adamcore_config cfg;
    int frames = argc > 4 ? atoi(argv[4]) : 300;
    int f, i;

    if (argc < 4) {
        fprintf(stderr, "usage: %s OS7 EOS WP [frames]\n", argv[0]);
        return 2;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.os7_rom_path = argv[1];
    cfg.eos_rom_path = argv[2];
    cfg.wp_rom_path = argv[3];
    cfg.start_machine = ADAMCORE_MACHINE_ADAM;
    cfg.audio_rate = 44100;

    C = adamcore_create(&cfg);
    if (!C) { fprintf(stderr, "create failed\n"); return 1; }

    orig_rd = C->cpu.mem_read;   C->cpu.mem_read = t_rd;
    orig_wr = C->cpu.mem_write;  C->cpu.mem_write = t_wr;
    orig_in = C->cpu.io_read;    C->cpu.io_read = t_in;
    orig_out = C->cpu.io_write;  C->cpu.io_write = t_out;

    for (f = 0; f < frames; f++) {
        if (f >= 1000 && f % 8 == 0)
            adamcore_inject_key(C, 'H');
        adamcore_run_frame(C);
    }

    printf("---- PCB/DCB dump (FEC0..FEDF) ----\n");
    for (i = 0xFEC0; i < 0xFEE0; i++)
        printf("%02X%s", C->ram[i], ((i + 1) & 15) ? " " : "\n");

    printf("---- top polled addresses >= F000 ----\n");
    for (i = 0xF000; i < 0x10000; i++)
        if (rd_count[i] > 500)
            printf("R %04X x%lu\n", i, rd_count[i]);
    printf("pc=%04X mem_ctrl=%02X net_ctrl=%02X halted=%d\n",
           C->cpu.pc, C->mem_ctrl, C->net_ctrl, C->cpu.halted);
    adamcore_destroy(C);
    return 0;
}
