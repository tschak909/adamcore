/*
 * cart_ops_test: pins the adamcore_cart_ops contract.
 *
 * A cartridge device with read side effects only works if the core is exact
 * about three things, none of which the compiler can check:
 *
 *   1. Exactly ONE read(commit=1) per Z80 bus read. Real hardware asserts the
 *      cartridge chip select once per M-cycle; a device counts those. An
 *      interpreter that read an address twice for one cycle would append a
 *      duplicate byte to a FujiNet cartridge's outgoing stream and corrupt
 *      every transaction. Read-modify-write instructions are the interesting
 *      case: 1 read + 1 write is correct, because silicon does that too.
 *
 *   2. NO events at all from adamcore_peek / peek_block / poke. Those are the
 *      debugger's path, and a memory view or disassembly refresh must not arm
 *      a protocol register or switch a bank.
 *
 *   3. reset() at install and never again. The cartridge edge carries no
 *      reset line, so a console reset does not reach a cartridge.
 *
 * Needs no ROMs and never skips: the "BIOS" is a hand-assembled program in a
 * synthetic 8K image, which is also what makes the per-instruction event
 * counts exact rather than incidental.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "adamcore.h"
#include "adamcore_debug.h"

/* ---- the fake cartridge --------------------------------------------------- */

#define LOG_MAX 64

typedef enum { EV_READ, EV_WRITE } ev_kind;

typedef struct {
    ev_kind kind;
    uint16_t off;
    int commit;
} event;

static event log_buf[LOG_MAX];
static int log_n;
static int reset_calls;
static uint8_t fake_byte = 0x00;

static uint8_t fake_read(void *ud, uint16_t off, int commit)
{
    (void)ud;
    if (log_n < LOG_MAX) {
        log_buf[log_n].kind = EV_READ;
        log_buf[log_n].off = off;
        log_buf[log_n].commit = commit;
        log_n++;
    }
    return fake_byte;
}

static void fake_write(void *ud, uint16_t off, uint8_t v)
{
    (void)ud;
    (void)v;
    if (log_n < LOG_MAX) {
        log_buf[log_n].kind = EV_WRITE;
        log_buf[log_n].off = off;
        log_buf[log_n].commit = 1;
        log_n++;
    }
}

static void fake_reset(void *ud) { (void)ud; reset_calls++; }

static const adamcore_cart_ops fake_ops = { fake_read, fake_write, fake_reset };

/* ---- harness -------------------------------------------------------------- */

static uint8_t os7[ADAMCORE_OS7_ROM_SIZE];
static int failures;

static void fail(const char *what, const char *detail)
{
    fprintf(stderr, "FAIL %s: %s\n", what, detail);
    failures++;
}

static int count(ev_kind kind)
{
    int i, n = 0;
    for (i = 0; i < log_n; i++)
        if (log_buf[i].kind == kind) n++;
    return n;
}

static void dump(void)
{
    int i;
    for (i = 0; i < log_n; i++)
        fprintf(stderr, "    [%d] %s off=%04X commit=%d\n", i,
                log_buf[i].kind == EV_READ ? "read " : "write",
                log_buf[i].off, log_buf[i].commit);
}

static adamcore *core;

/* Assemble `code` into console RAM at $6000 -- the real 1K, which stays RAM
 * in every configuration -- point the CPU at it with the given registers, run
 * `steps` instructions, and check the event counts.
 * Every read must carry commit=1: an uncommitted read reaching the device
 * from the Z80 would mean the flag is wired backwards. */
static void run_case(const char *name, const uint8_t *code, size_t len,
                     const adamcore_z80_regs *setup, uint32_t steps,
                     int want_reads, int want_writes)
{
    adamcore_z80_regs r = *setup;
    char msg[160];
    int i;

    for (i = 0; i < 0x40; i++)
        adamcore_poke(core, (uint16_t)(0x6000 + i), 0x00);
    for (i = 0; i < (int)len; i++)
        adamcore_poke(core, (uint16_t)(0x6000 + i), code[i]);

    r.pc = 0x6000;
    adamcore_set_regs(core, &r);

    log_n = 0;
    adamcore_debug_run(core, steps, 0, NULL);

    if (count(EV_READ) != want_reads || count(EV_WRITE) != want_writes) {
        snprintf(msg, sizeof msg,
                 "expected %d read / %d write, got %d / %d",
                 want_reads, want_writes, count(EV_READ), count(EV_WRITE));
        fail(name, msg);
        dump();
        return;
    }
    for (i = 0; i < log_n; i++) {
        if (log_buf[i].kind == EV_READ && !log_buf[i].commit) {
            fail(name, "a Z80 bus read reached the device with commit=0");
            dump();
            return;
        }
    }
    printf("  ok  %-22s %d read / %d write\n", name, want_reads, want_writes);
}

int main(void)
{
    adamcore_config cfg;
    adamcore_z80_regs r;
    int i;

    /* A ColecoVision with a synthetic BIOS: OS7 at $0000-$1FFF, RAM above,
     * cartridge at $8000-$FFFF. No ROM files, so this never skips. */
    memset(os7, 0x00, sizeof os7);
    memset(&cfg, 0, sizeof cfg);
    cfg.os7_rom_data = os7;
    cfg.start_machine = ADAMCORE_MACHINE_CV;
    cfg.audio_rate = 44100;

    core = adamcore_create(&cfg);
    if (!core) {
        fprintf(stderr, "FAIL: adamcore_create\n");
        return 1;
    }

    /* ---- 3. reset() at install, and only there ---------------------------- */
    reset_calls = 0;
    adamcore_set_cart_ops(core, &fake_ops, NULL);
    if (reset_calls != 1)
        fail("install", "ops->reset was not called exactly once at install");

    memset(&r, 0, sizeof r);
    adamcore_get_regs(core, &r);

    /* ---- 1. one event per bus access -------------------------------------- */
    printf("one event per Z80 bus access:\n");
    {
        /* LD A,($8100) */
        static const uint8_t code[] = { 0x3A, 0x00, 0x81 };
        run_case("LD A,(nn)", code, sizeof code, &r, 1, 1, 0);
    }
    {
        /* LD A,(HL), HL = $8200 */
        static const uint8_t code[] = { 0x7E };
        adamcore_z80_regs s = r;
        s.h = 0x82; s.l = 0x00;
        run_case("LD A,(HL)", code, sizeof code, &s, 1, 1, 0);
    }
    {
        /* LD B,(IX+4), IX = $8300 */
        static const uint8_t code[] = { 0xDD, 0x46, 0x04 };
        adamcore_z80_regs s = r;
        s.ix = 0x8300;
        run_case("LD r,(IX+d)", code, sizeof code, &s, 1, 1, 0);
    }
    {
        /* INC (HL), HL = $8400 -- read-modify-write, 1r + 1w is correct */
        static const uint8_t code[] = { 0x34 };
        adamcore_z80_regs s = r;
        s.h = 0x84; s.l = 0x00;
        run_case("INC (HL)", code, sizeof code, &s, 1, 1, 1);
    }
    {
        /* LDIR, HL = $8500 -> DE = $2000 (RAM), BC = 3: one read per pass */
        static const uint8_t code[] = { 0xED, 0xB0 };
        adamcore_z80_regs s = r;
        s.h = 0x85; s.l = 0x00; s.d = 0x20; s.e = 0x00;
        s.b = 0x00; s.c = 0x03;
        run_case("LDIR x3", code, sizeof code, &s, 3, 3, 0);
    }
    {
        /* CPIR, HL = $8600, BC = 2, A = 0xFF vs a device returning 0x00 */
        static const uint8_t code[] = { 0xED, 0xB1 };
        adamcore_z80_regs s = r;
        s.h = 0x86; s.l = 0x00; s.b = 0x00; s.c = 0x02; s.a = 0xFF;
        run_case("CPIR x2", code, sizeof code, &s, 2, 2, 0);
    }
    {
        /* OUTI, HL = $8700, to port $00 (nothing decodes it) */
        static const uint8_t code[] = { 0xED, 0xA3 };
        adamcore_z80_regs s = r;
        s.h = 0x87; s.l = 0x00; s.b = 0x01; s.c = 0x00;
        run_case("OUTI", code, sizeof code, &s, 1, 1, 0);
    }
    {
        /* RES 0,(IY+0), IY = $8800 -- read-modify-write again */
        static const uint8_t code[] = { 0xFD, 0xCB, 0x00, 0x86 };
        adamcore_z80_regs s = r;
        s.iy = 0x8800;
        run_case("RES b,(IY+d)", code, sizeof code, &s, 1, 1, 1);
    }
    {
        /* EX (SP),HL, SP = $8900 -- two reads and two writes */
        static const uint8_t code[] = { 0xE3 };
        adamcore_z80_regs s = r;
        s.sp = 0x8900;
        run_case("EX (SP),HL", code, sizeof code, &s, 1, 2, 2);
    }
    {
        /* POP BC, SP = $8A00 */
        static const uint8_t code[] = { 0xC1 };
        adamcore_z80_regs s = r;
        s.sp = 0x8A00;
        run_case("POP BC", code, sizeof code, &s, 1, 2, 0);
    }
    {
        /* An M1 fetch from inside the cartridge window: the device serves the
         * opcode itself (0x00 = NOP), so executing from cart space is one
         * read like any other. */
        adamcore_z80_regs s = r;
        s.pc = 0x8B00;
        adamcore_set_regs(core, &s);
        log_n = 0;
        adamcore_debug_run(core, 1, 0, NULL);
        if (count(EV_READ) != 1 || count(EV_WRITE) != 0) {
            fail("M1 fetch from cart", "expected exactly one read");
            dump();
        } else if (log_buf[0].off != 0x0B00) {
            fail("M1 fetch from cart", "wrong offset");
            dump();
        } else {
            printf("  ok  %-22s 1 read / 0 write\n", "M1 fetch from cart");
        }
    }

    /* ---- 2. the debugger's path must be silent ---------------------------- */
    printf("debugger path is silent:\n");
    {
        /* A peek DOES reach the device -- it has to, or a memory view of a
         * device-served window would show the stale cart[] array instead of
         * what is really there. What must hold is that every such read
         * carries commit=0, so the device knows to do nothing, and that no
         * writes happen at all. */
        uint8_t block[256];
        int bad = 0, writes = 0, n = 0;
        for (i = 0; i < 0x8000; i += 256) {
            log_n = 0;
            adamcore_peek_block(core, (uint16_t)(0x8000 + i), block, 256);
            for (n = 0; n < log_n; n++) {
                if (log_buf[n].kind == EV_WRITE) writes++;
                else if (log_buf[n].commit) bad++;
            }
        }
        for (i = 0; i < 0x8000; i++) {
            log_n = 0;
            (void)adamcore_peek(core, (uint16_t)(0x8000 + i));
            for (n = 0; n < log_n; n++) {
                if (log_buf[n].kind == EV_WRITE) writes++;
                else if (log_buf[n].commit) bad++;
            }
        }
        log_n = 0;
        if (bad || writes) {
            fail("peek/peek_block",
                 "a debugger read reached the device with commit=1, or wrote");
        } else {
            printf("  ok  %-22s every read commit=0, no writes\n",
                   "peek + peek_block");
        }
    }
    {
        log_n = 0;
        for (i = 0; i < 0x8000; i += 1)
            adamcore_poke(core, (uint16_t)(0x8000 + i), 0x5A);
        if (log_n != 0) {
            fail("poke", "the debugger write path reached the device");
            dump();
        } else {
            printf("  ok  %-22s 0 events over the whole window\n", "poke");
        }
    }

    /* ---- 3, continued: a console reset must not reach the cartridge ------- */
    reset_calls = 0;
    adamcore_request_reset(core, 1);
    adamcore_run_frame(core);
    adamcore_request_reset(core, 0);
    adamcore_run_frame(core);
    if (reset_calls != 0) {
        fail("console reset", "ops->reset fired on a console reset");
    } else {
        printf("  ok  %-22s survives both reset modes\n", "cart not reset");
    }

    /* Uninstalling goes back to the plain image path. */
    adamcore_set_cart_ops(core, NULL, NULL);
    log_n = 0;
    (void)adamcore_peek(core, 0x8000);
    if (log_n != 0)
        fail("uninstall", "the device still received events after removal");

    adamcore_destroy(core);

    if (failures) {
        fprintf(stderr, "\ncart_ops_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("\ncart_ops_test: PASS\n");
    return 0;
}
