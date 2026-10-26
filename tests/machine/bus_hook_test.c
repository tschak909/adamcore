/*
 * bus_hook_test: pins adamcore_set_bus_hook and adamcore_debug_beam.
 *
 * A hook sees every Z80 memory and I/O cycle with the address and the value,
 * never a peek or poke, nothing once it is removed, and installing over an
 * installed hook must not lose the real handlers. Needs no ROMs: the program
 * is hand-assembled into the console's RAM on a ColecoVision with a blank OS7.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "adamcore.h"
#include "adamcore_debug.h"

#define LOG_MAX 64

typedef struct { int kind; uint16_t addr; uint8_t val; uint8_t under; } ev;

static ev log_buf[LOG_MAX];
static int log_n;
static adamcore *core;
static int failures;
static uint8_t os7[ADAMCORE_OS7_ROM_SIZE];

static void fail(const char *what, const char *detail)
{
    fprintf(stderr, "FAIL %s: %s\n", what, detail);
    failures++;
}

static void hook(void *ud, int kind, uint16_t addr, uint8_t val)
{
    int *tag = ud;
    if (log_n < LOG_MAX) {
        log_buf[log_n].kind = kind;
        log_buf[log_n].addr = addr;
        log_buf[log_n].val = val;
        /* what memory holds right now: a write hook runs before it lands */
        log_buf[log_n].under = adamcore_peek(core, addr);
        log_n++;
    }
    if (tag)
        (*tag)++;
}

static int count(int kind, int addr_lo8, uint16_t addr)
{
    int i, n = 0;
    for (i = 0; i < log_n; i++)
        if (log_buf[i].kind == kind &&
            (addr_lo8 ? (log_buf[i].addr & 0xFF) == addr
                      : log_buf[i].addr == addr))
            n++;
    return n;
}

static const ev *find(int kind, int addr_lo8, uint16_t addr)
{
    int i;
    for (i = 0; i < log_n; i++)
        if (log_buf[i].kind == kind &&
            (addr_lo8 ? (log_buf[i].addr & 0xFF) == addr
                      : log_buf[i].addr == addr))
            return &log_buf[i];
    return NULL;
}

static void run_code(const uint8_t *code, size_t len, uint8_t a)
{
    adamcore_z80_regs r;
    size_t i;

    for (i = 0; i < 0x40; i++)
        adamcore_poke(core, (uint16_t)(0x6000 + i), 0x00);
    for (i = 0; i < len; i++)
        adamcore_poke(core, (uint16_t)(0x6000 + i), code[i]);
    adamcore_get_regs(core, &r);
    r.pc = 0x6000;
    r.a = a;
    adamcore_set_regs(core, &r);
    log_n = 0;
    adamcore_debug_run(core, 1, 0, NULL);
}

int main(void)
{
    adamcore_config cfg;
    int calls = 0, line;
    uint64_t f0, f1;

    memset(os7, 0, sizeof os7);
    memset(&cfg, 0, sizeof cfg);
    cfg.os7_rom_data = os7;
    cfg.start_machine = ADAMCORE_MACHINE_CV;
    cfg.audio_rate = 44100;
    core = adamcore_create(&cfg);
    if (!core) {
        fprintf(stderr, "FAIL: adamcore_create\n");
        return 1;
    }

    adamcore_set_bus_hook(core, hook, &calls);

    {   /* LD A,($6100) */
        static const uint8_t code[] = { 0x3A, 0x00, 0x61 };
        adamcore_poke(core, 0x6100, 0x5A);
        run_code(code, sizeof code, 0);
        if (count(ADAMCORE_BUS_MEM_R, 0, 0x6100) != 1)
            fail("LD A,(nn)", "not exactly one MEM_R at $6100");
        else if (find(ADAMCORE_BUS_MEM_R, 0, 0x6100)->val != 0x5A)
            fail("LD A,(nn)", "MEM_R carries the wrong value");
        else if (count(ADAMCORE_BUS_MEM_R, 0, 0x6000) != 1)
            fail("LD A,(nn)", "opcode fetch not seen");
        else
            printf("  ok  memory read\n");
    }
    {   /* LD ($6101),A */
        static const uint8_t code[] = { 0x32, 0x01, 0x61 };
        adamcore_poke(core, 0x6101, 0x11);
        run_code(code, sizeof code, 0xC3);
        if (count(ADAMCORE_BUS_MEM_W, 0, 0x6101) != 1)
            fail("LD (nn),A", "not exactly one MEM_W at $6101");
        else if (find(ADAMCORE_BUS_MEM_W, 0, 0x6101)->val != 0xC3)
            fail("LD (nn),A", "MEM_W carries the wrong value");
        else if (find(ADAMCORE_BUS_MEM_W, 0, 0x6101)->under != 0x11)
            fail("LD (nn),A", "the write had already landed when the hook ran");
        else if (adamcore_peek(core, 0x6101) != 0xC3)
            fail("LD (nn),A", "the write did not go through");
        else
            printf("  ok  memory write (before it lands, then lands)\n");
    }
    {   /* IN A,($FC): controller 1 port */
        static const uint8_t code[] = { 0xDB, 0xFC };
        run_code(code, sizeof code, 0);
        if (count(ADAMCORE_BUS_IO_R, 1, 0xFC) != 1)
            fail("IN A,(n)", "not exactly one IO_R for port $FC");
        else
            printf("  ok  port read\n");
    }
    {   /* OUT ($3F),A */
        static const uint8_t code[] = { 0xD3, 0x3F };
        run_code(code, sizeof code, 0x02);
        if (count(ADAMCORE_BUS_IO_W, 1, 0x3F) != 1 ||
            find(ADAMCORE_BUS_IO_W, 1, 0x3F)->val != 0x02)
            fail("OUT (n),A", "no IO_W for port $3F carrying $02");
        else {
            uint8_t mem, net;
            adamcore_get_machine_state(core, &mem, &net, NULL);
            if (net != 0x02)
                fail("OUT (n),A", "the port write did not reach net_ctrl");
            else
                printf("  ok  port write (and it still lands)\n");
        }
    }

    /* peek, poke and the state getters are not bus cycles */
    log_n = 0;
    (void)adamcore_peek(core, 0x6000);
    adamcore_poke(core, 0x6000, 0x00);
    {
        uint8_t blk[8];
        adamcore_peek_block(core, 0x6000, blk, sizeof blk);
    }
    if (log_n != 0)
        fail("peek/poke", "the debugger's own accesses fired the hook");
    else
        printf("  ok  peek/poke are silent\n");

    /* a second install keeps the real handlers, and NULL gives them back */
    {
        int calls2 = 0;
        static const uint8_t code[] = { 0x3A, 0x00, 0x61 };
        adamcore_set_bus_hook(core, hook, &calls2);
        run_code(code, sizeof code, 0);
        if (calls2 == 0)
            fail("re-install", "the replacement hook saw nothing");
        adamcore_set_bus_hook(core, NULL, NULL);
        calls = 0;
        adamcore_poke(core, 0x6100, 0x77);
        run_code(code, sizeof code, 0);
        {
            adamcore_z80_regs r;
            adamcore_get_regs(core, &r);
            if (calls != 0 || calls2 == 0)
                fail("remove", "events after the hook was removed");
            else if (r.a != 0x77)
                fail("remove", "the real memory read was lost");
            else
                printf("  ok  re-install and removal\n");
        }
    }

    /* beam: a frame boundary advances the frame counter, line is in range */
    adamcore_debug_beam(core, &line, &f0);
    adamcore_debug_run(core, 0, 0, NULL); /* to the end of the frame */
    adamcore_debug_beam(core, &line, &f1);
    if (f1 != f0 + 1)
        fail("beam", "frame count did not advance by one");
    else if (line != 0)
        fail("beam", "line is not 0 between frames");
    else {
        adamcore_debug_run(core, 40, 0, NULL);
        adamcore_debug_beam(core, &line, NULL);
        if (line < 0 || line > 261)
            fail("beam", "line out of range mid-frame");
        else
            printf("  ok  beam\n");
    }

    adamcore_destroy(core);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("bus_hook: all passed\n");
    return 0;
}
