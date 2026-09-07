/*
 * sgm_test: the ColecoVision memory map, with and without an Opcode Super
 * Game Module.
 *
 * The stock-console half of this is a CORRECTION, not an addition. adamcore
 * used to serve a bare ColecoVision a full readable/writable 24K at
 * $2000-$7FFF -- which is what a machine with an SGM plugged in and its RAM
 * enabled looks like, not a stock one. A real console has 1K at $6000-$63FF,
 * mirrored up through $7FFF by partial address decoding, and nothing at all
 * below $6000.
 *
 * That mirror earns its own assertions because a FujiNet cartridge depends on
 * it: A15 does not reach the cartridge connector, so a RAM access at
 * $7C00-$7FFF puts the same bits on A0-A14 as a cartridge read of
 * $FC00-$FFFF. An emulator that does not alias here cannot reproduce the
 * situation the cartridge firmware is built to survive.
 *
 * Needs no ROMs and never skips.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "adamcore.h"
#include "adamcore_debug.h"

/* Drive real IN/OUT instructions rather than poking state: the port decode
 * is half of what is under test. */
static uint8_t io_prog[64];

static void out_port(adamcore *c, uint8_t port, uint8_t val)
{
    adamcore_z80_regs r;
    int i;
    const uint8_t code[] = { 0x3E, val, 0xD3, port, 0x76 }; /* LD A,v; OUT; HALT */
    for (i = 0; i < (int)sizeof code; i++)
        adamcore_poke(c, (uint16_t)(0x6000 + i), code[i]);
    memset(&r, 0, sizeof r);
    adamcore_get_regs(c, &r);
    r.pc = 0x6000;
    adamcore_set_regs(c, &r);
    adamcore_debug_run(c, 2, 0, NULL);
}

static uint8_t in_port(adamcore *c, uint8_t port)
{
    adamcore_z80_regs r;
    int i;
    const uint8_t code[] = { 0xDB, port, 0x76 };            /* IN A,(port); HALT */
    for (i = 0; i < (int)sizeof code; i++)
        adamcore_poke(c, (uint16_t)(0x6000 + i), code[i]);
    memset(&r, 0, sizeof r);
    adamcore_get_regs(c, &r);
    r.pc = 0x6000;
    adamcore_set_regs(c, &r);
    adamcore_debug_run(c, 1, 0, NULL);
    adamcore_get_regs(c, &r);
    (void)io_prog;
    return r.a;
}

static uint8_t os7[ADAMCORE_OS7_ROM_SIZE];
static uint8_t eos[ADAMCORE_EOS_ROM_SIZE];
static uint8_t wp[ADAMCORE_WP_ROM_SIZE];
static int failures;

static void check(const char *what, int ok)
{
    if (ok) {
        printf("  ok  %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        failures++;
    }
}

static adamcore *make(int machine, int sgm)
{
    adamcore_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.os7_rom_data = os7;
    cfg.eos_rom_data = eos;   /* ADAM mode refuses to create without these */
    cfg.wp_rom_data = wp;
    cfg.start_machine = machine;
    cfg.sgm = sgm;
    cfg.audio_rate = 44100;
    return adamcore_create(&cfg);
}

/* Write then read back through the CPU's own map. adamcore_poke/peek go
 * through the same decode, so this is the map under test, not a shortcut. */
static int rw(adamcore *c, uint16_t a, uint8_t v)
{
    adamcore_poke(c, a, v);
    return adamcore_peek(c, a);
}

int main(void)
{
    adamcore *c;

    memset(os7, 0xA5, sizeof os7); /* a recognisable BIOS pattern */

    /* ---- stock ColecoVision, no SGM --------------------------------------- */
    printf("stock ColecoVision (no SGM):\n");
    c = make(ADAMCORE_MACHINE_CV, 0);
    if (!c) { fprintf(stderr, "create failed\n"); return 1; }

    check("$0000-$1FFF is the BIOS",
          adamcore_peek(c, 0x0000) == 0xA5 && adamcore_peek(c, 0x1FFF) == 0xA5);
    check("the BIOS is not writable", rw(c, 0x0100, 0x5A) == 0xA5);

    check("$2000 is open bus", rw(c, 0x2000, 0x11) == 0xFF);
    check("$4000 is open bus", rw(c, 0x4000, 0x22) == 0xFF);
    check("$5FFF is open bus", rw(c, 0x5FFF, 0x33) == 0xFF);

    check("$6000 is RAM", rw(c, 0x6000, 0x44) == 0x44);

    /* The 1K mirror: four windows onto the same kilobyte. */
    adamcore_poke(c, 0x6000, 0x5A);
    check("$6400 mirrors $6000", adamcore_peek(c, 0x6400) == 0x5A);
    check("$6800 mirrors $6000", adamcore_peek(c, 0x6800) == 0x5A);
    check("$7000 mirrors $6000", adamcore_peek(c, 0x7000) == 0x5A);
    check("$7C00 mirrors $6000", adamcore_peek(c, 0x7C00) == 0x5A);
    adamcore_poke(c, 0x7FFF, 0x3C);
    check("a write at $7FFF lands in the 1K",
          adamcore_peek(c, 0x63FF) == 0x3C);
    /* ...and the aliasing that makes the cartridge firmware two-tier: the
     * console's stack area at $73B9 is the same byte as $63B9. */
    adamcore_poke(c, 0x73B9, 0x99);
    check("$73B9 and $63B9 are the same byte",
          adamcore_peek(c, 0x63B9) == 0x99);

    check("$50-$53 read $FF with no SGM", in_port(c, 0x52) == 0xFF);
    check("$7F is not readable on a ColecoVision", in_port(c, 0x7F) == 0xFF);
    /* Writing $7F on a stock console must do nothing at all -- above all it
     * must not reach mem_ctrl and unmap the cartridge. */
    out_port(c, 0x7F, 0x02);
    check("a $7F write leaves the BIOS mapped", adamcore_peek(c, 0x0000) == 0xA5);
    adamcore_destroy(c);

    /* ---- SGM present but its RAM not enabled ------------------------------ */
    printf("SGM present, RAM disabled (power-on state):\n");
    c = make(ADAMCORE_MACHINE_CV, 1);
    if (!c) { fprintf(stderr, "create failed\n"); return 1; }
    check("$2000 still open bus until the cartridge enables it",
          rw(c, 0x2000, 0x11) == 0xFF);
    check("$6000 still the mirrored 1K", rw(c, 0x6000, 0x44) == 0x44);
    adamcore_poke(c, 0x6000, 0x77);
    check("$7000 still mirrors $6000", adamcore_peek(c, 0x7000) == 0x77);

    /* ---- the SGM's ports -------------------------------------------------- */
    printf("SGM ports:\n");
    /* $53 bit 0 opens the 24K. */
    out_port(c, 0x53, 0x01);
    check("$53=1 makes $2000 real RAM", rw(c, 0x2000, 0x11) == 0x11);
    check("$53=1 makes $4000 real RAM", rw(c, 0x4000, 0x22) == 0x22);
    adamcore_poke(c, 0x6000, 0x5A);
    adamcore_poke(c, 0x7000, 0xC3);
    check("$53=1 stops the 1K mirror ($6000 and $7000 distinct)",
          adamcore_peek(c, 0x6000) == 0x5A && adamcore_peek(c, 0x7000) == 0xC3);
    out_port(c, 0x53, 0x00);
    check("$53=0 closes it again", rw(c, 0x2000, 0x33) == 0xFF);

    /* $7F bit 1 clear maps SGM RAM over the BIOS. */
    out_port(c, 0x7F, 0x0D);
    check("$7F bit1 clear maps RAM over the BIOS",
          rw(c, 0x0100, 0x77) == 0x77);
    out_port(c, 0x7F, 0x0F);
    check("$7F bit1 set brings the BIOS back",
          adamcore_peek(c, 0x0100) == 0xA5);
    /* And whatever is written to $7F, the cartridge must stay mapped: this
     * is the failure that letting $7F reach mem_ctrl would cause. */
    {
        int v, ok = 1;
        for (v = 0; v < 256; v++) {
            out_port(c, 0x7F, (uint8_t)v);
            /* no cartridge loaded, so the window reads as open bus -- what
             * matters is that it is not silently switched to RAM or xram */
            if (adamcore_peek(c, 0x8000) != 0xFF) { ok = 0; break; }
        }
        check("no $7F value can unmap the cartridge window", ok);
    }
    out_port(c, 0x7F, 0x0F);

    /* The AY register file, through $50/$51/$52. */
    out_port(c, 0x50, 0x02);   /* select R2 (channel B tone, fine) */
    out_port(c, 0x51, 0xA5);
    check("AY register round trips through $50/$51/$52",
          in_port(c, 0x52) == 0xA5);
    out_port(c, 0x50, 0x01);   /* R1 is 4 bits wide */
    out_port(c, 0x51, 0xFF);
    check("AY R1 reads back masked to 4 bits", in_port(c, 0x52) == 0x0F);
    out_port(c, 0x50, 0x0E);   /* R14: an unconnected I/O port */
    check("AY R14 floats high", in_port(c, 0x52) == 0xFF);

    /* A console reset restores the BIOS and closes the RAM: the SGM hangs
     * off the expansion connector, which does carry /RESET. */
    out_port(c, 0x53, 0x01);
    adamcore_request_reset(c, 1);
    /* Two frames, not one: out_port drives the machine with
     * adamcore_debug_run, which leaves a partial frame outstanding, and the
     * first run_frame finishes that abandoned frame before a new one -- and
     * with it the pending reset -- begins. */
    adamcore_run_frame(c);
    adamcore_run_frame(c);
    check("a console reset closes the SGM RAM again",
          rw(c, 0x2000, 0x44) == 0xFF);
    adamcore_destroy(c);

    /* ---- ADAM is untouched ------------------------------------------------ */
    printf("ADAM in game mode is unchanged:\n");
    c = make(ADAMCORE_MACHINE_ADAM, 0);
    if (!c) { fprintf(stderr, "create failed\n"); return 1; }
    adamcore_request_reset(c, 1);   /* game reset: OS7 low, 24K RAM above */
    adamcore_run_frame(c);
    check("$2000 is real RAM on an ADAM", rw(c, 0x2000, 0x11) == 0x11);
    check("$4000 is real RAM on an ADAM", rw(c, 0x4000, 0x22) == 0x22);
    /* No 1K mirror on an ADAM: $6000 and $7000 are distinct bytes of its own
     * 64K, which is exactly the difference this commit introduces. */
    adamcore_poke(c, 0x6000, 0x5A);
    adamcore_poke(c, 0x7000, 0xC3);
    check("$6000 and $7000 are distinct bytes on an ADAM",
          adamcore_peek(c, 0x6000) == 0x5A && adamcore_peek(c, 0x7000) == 0xC3);
    adamcore_destroy(c);

    if (failures) {
        fprintf(stderr, "\nsgm_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("\nsgm_test: PASS\n");
    return 0;
}
