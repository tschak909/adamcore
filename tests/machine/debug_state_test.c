/*
 * debug_state_test: pins the debugger state added on top of the bus hook --
 * hook ordering and old values, the T-state within the line, per-line VDP
 * register capture, the SN76489 register mirror and the device getters.
 *
 * Needs no ROMs: programs are hand-assembled into the console's RAM on a
 * ColecoVision with a blank OS7, and the VDP interrupt stays disabled (R1
 * IE clear) so nothing leaves the program.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "adamcore.h"
#include "adamcore_debug.h"

#define LOG_MAX 32

typedef struct { int kind; uint16_t addr; uint8_t val; uint8_t old; } ev;

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

static void check(int ok, const char *what, const char *detail)
{
    if (!ok)
        fail(what, detail);
}

static adamcore *make_core(int sgm)
{
    adamcore_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.os7_rom_data = os7;
    cfg.start_machine = ADAMCORE_MACHINE_CV;
    cfg.audio_rate = 44100;
    cfg.sgm = sgm;
    return adamcore_create(&cfg);
}

static void hook(void *ud, int kind, uint16_t addr, uint8_t val)
{
    uint8_t mem = 0, net = 0;
    (void)ud;
    if (log_n >= LOG_MAX)
        return;
    log_buf[log_n].kind = kind;
    log_buf[log_n].addr = addr;
    log_buf[log_n].val = val;
    adamcore_get_machine_state(core, &mem, &net, NULL);
    /* "old": what the target holds right now (a write hook runs first) */
    log_buf[log_n].old = kind == ADAMCORE_BUS_IO_W ? net
                                                   : adamcore_peek(core, addr);
    log_n++;
}

static void load(const uint8_t *code, size_t len)
{
    adamcore_z80_regs r;
    size_t i;
    for (i = 0; i < len; i++)
        adamcore_poke(core, (uint16_t)(0x6000 + i), code[i]);
    adamcore_get_regs(core, &r);
    r.pc = 0x6000;
    adamcore_set_regs(core, &r);
}

static void out_port(uint8_t port, uint8_t val)
{
    const uint8_t code[] = { 0x3E, val, 0xD3, port, 0x18, 0xFE };
    load(code, sizeof code);
    adamcore_debug_run(core, 2, 0, NULL);
}

static void vdp_reg(int reg, uint8_t val)
{
    out_port(0xBF, val);
    out_port(0xBF, (uint8_t)(0x80 | reg));
}

static void test_hook_order(void)
{
    /* LD A,$C3 ; LD ($6101),A ; OUT ($3F),A */
    static const uint8_t code[] = { 0x3E, 0xC3, 0x32, 0x01, 0x61, 0xD3, 0x3F };
    int i, w = -1, f = -1, o = -1, fetch_before = 0;

    adamcore_poke(core, 0x6101, 0x11);
    adamcore_set_bus_hook(core, hook, NULL);
    log_n = 0;
    load(code, sizeof code);
    adamcore_debug_run(core, 3, 0, NULL);
    adamcore_set_bus_hook(core, NULL, NULL);

    for (i = 0; i < log_n; i++) {
        if (log_buf[i].kind == ADAMCORE_BUS_MEM_R && log_buf[i].addr == 0x6004)
            f = i;
        if (log_buf[i].kind == ADAMCORE_BUS_MEM_W && log_buf[i].addr == 0x6101)
            w = i;
        if (log_buf[i].kind == ADAMCORE_BUS_IO_W && (log_buf[i].addr & 0xFF) == 0x3F)
            o = i;
    }
    fetch_before = f >= 0 && w > f;
    check(fetch_before, "order", "the store did not come after its operand fetch");
    check(o > w, "order", "OUT did not come after the store");
    check(w >= 0 && log_buf[w].old == 0x11 && log_buf[w].val == 0xC3,
          "old value", "MEM_W did not show the old byte and the new value");
    check(o >= 0 && log_buf[o].old == 0x00 && log_buf[o].val == 0xC3,
          "old value", "IO_W $3F did not show the old net_ctrl");
    if (!failures)
        printf("  ok  hook ordering and old values\n");
}

static void test_tstate(void)
{
    adamcore_z80_regs r0, r1;
    uint64_t c0;
    int line, i;

    adamcore_debug_run(core, 0, 0, NULL); /* to a frame boundary */
    check(adamcore_debug_beam_tstate(core) == 0, "tstate", "not 0 between frames");
    adamcore_get_regs(core, &r0);
    c0 = r0.cycles;
    {
        static const uint8_t spin[] = { 0x18, 0xFE };
        load(spin, sizeof spin);
    }
    for (i = 0; i < 1000; i++) {
        int t;
        adamcore_debug_run(core, 1, 0, NULL);
        adamcore_debug_beam(core, &line, NULL);
        t = adamcore_debug_beam_tstate(core);
        adamcore_get_regs(core, &r1);
        if (line * 228 + t != (int)(r1.cycles - c0) || t < 0) {
            fail("tstate", "line*228 + tstate != cycles since frame start");
            return;
        }
    }
    printf("  ok  T-state within the line\n");
}

static void test_line_capture(void)
{
    uint8_t regs[8], status;
    uint64_t frame, f_done;
    int line, write_line, l, bad = 0;

    check(adamcore_vdp_line_regs(core, 0, regs, &status, &frame) == 0,
          "capture", "a record exists while capture is off");
    adamcore_vdp_line_capture_enable(core, 1);

    adamcore_debug_run(core, 0, 0, NULL); /* frame boundary */
    vdp_reg(7, 0x04);
    adamcore_debug_run(core, 0, 0, NULL); /* a whole frame with R7 = 4 */
    adamcore_debug_beam(core, NULL, &f_done);

    /* next frame: run to ~line 100 in a spin loop, then change R7 */
    {
        static const uint8_t spin[] = { 0x18, 0xFE };
        load(spin, sizeof spin);
    }
    do {
        adamcore_debug_run(core, 1, 0, NULL);
        adamcore_debug_beam(core, &line, NULL);
    } while (line < 100);
    {
        const uint8_t code[] = { 0x3E, 0x0B, 0xD3, 0xBF, 0x3E, 0x87, 0xD3, 0xBF,
                                 0x18, 0xFE };
        load(code, sizeof code);
        adamcore_debug_run(core, 4, 0, NULL);
        adamcore_debug_beam(core, &write_line, NULL);
        {   /* back to the spin loop, finish the frame */
            static const uint8_t spin2[] = { 0x18, 0xFE };
            load(spin2, sizeof spin2);
        }
    }
    adamcore_debug_run(core, 0, 0, NULL);

    for (l = 0; l < 262; l++) {
        uint8_t want = l < write_line ? 0x04 : 0x0B;
        if (!adamcore_vdp_line_regs(core, l, regs, &status, &frame)) {
            bad = 1;
            break;
        }
        if (frame != f_done || regs[7] != want)
            bad = 1;
    }
    check(!bad, "capture", "per-line R7 does not step from 4 to 11 at the write");
    check(write_line >= 100 && write_line < 192, "capture", "test setup: odd write line");
    check(adamcore_vdp_line_regs(core, 262, regs, NULL, NULL) == 0 &&
          adamcore_vdp_line_regs(core, -1, regs, NULL, NULL) == 0,
          "capture", "out-of-range line accepted");

    /* the status register is captured too: F is set from line 192 on (it also
     * stays set on earlier lines until the guest reads it) */
    adamcore_vdp_line_regs(core, 200, regs, &status, NULL);
    check(status & 0x80, "capture", "F not set in vertical blank");

    /* run_frame (the fast path) captures as well, with a newer stamp */
    adamcore_run_frame(core);
    adamcore_vdp_line_regs(core, 50, regs, NULL, &frame);
    check(frame == f_done + 1, "capture", "fast path did not record the next frame");

    adamcore_vdp_line_capture_enable(core, 0);
    check(adamcore_vdp_line_regs(core, 50, regs, NULL, NULL) == 0,
          "capture", "records survived disabling");
    if (!failures)
        printf("  ok  per-line VDP register capture\n");
}

static void test_sn_mirror(void)
{
    adamcore_sn_state s;

    adamcore_psg_sn_state(core, &s);
    check(s.period[0] == 1 && s.atten[0] == 15 && s.atten[3] == 15,
          "sn mirror", "not the chip reset values");

    out_port(0xFF, 0x8A); /* tone0 low nibble A  */
    out_port(0xFF, 0x15); /* tone0 high 6 bits   */
    out_port(0xFF, 0x91); /* tone0 atten 1       */
    out_port(0xFF, 0xC3); /* tone2 low nibble 3  */
    out_port(0xFF, 0x01); /* data byte: tone2 high */
    out_port(0xFF, 0xB4); /* tone1 atten latch, low nibble 4 */
    out_port(0xFF, 0x02); /* data byte: atten 2 */
    out_port(0xFF, 0xE5); /* noise control 5     */
    out_port(0xFF, 0xFA); /* noise atten 10      */
    adamcore_psg_sn_state(core, &s);
    check(s.period[0] == 0x15A, "sn mirror", "tone 0 period");
    check(s.period[2] == 0x013, "sn mirror", "tone 2 period (data byte)");
    check(s.atten[0] == 1, "sn mirror", "tone 0 attenuation");
    check(s.atten[1] == 2, "sn mirror", "tone 1 attenuation via data byte");
    check(s.noise_ctrl == 5, "sn mirror", "noise control");
    check(s.atten[3] == 10, "sn mirror", "noise attenuation");
    check(s.latched_reg == 7, "sn mirror", "latched register");
    check(s.period[1] == 1 && s.atten[2] == 15, "sn mirror", "untouched channel changed");
    if (!failures)
        printf("  ok  SN76489 register mirror\n");
}

static void test_getters(void)
{
    adamcore_vdp_latch vl;
    adamcore_cpu_aux_state aux;
    adamcore_sgm_info sgm;
    uint16_t joy[2];
    uint8_t mode;
    unsigned long long n0;

    out_port(0xBF, 0x12);
    adamcore_vdp_latch_state(core, &vl);
    check(vl.latched == 1 && vl.latch == 0x12, "vdp latch", "first control byte");
    out_port(0xBF, 0x87);
    adamcore_vdp_latch_state(core, &vl);
    check(vl.latched == 0, "vdp latch", "second control byte did not clear it");

    /* write two VRAM bytes, then point the read address at the first */
    out_port(0xBF, 0x00); out_port(0xBF, 0x50);
    out_port(0xBE, 0x42); out_port(0xBE, 0x43);
    out_port(0xBF, 0x00); out_port(0xBF, 0x10);
    adamcore_vdp_latch_state(core, &vl);
    check(vl.read_ahead == 0x00 || vl.read_ahead == 0x42, "vdp latch", "read-ahead");

    {   /* SCF sets Q; EI blocks acceptance for one instruction */
        static const uint8_t scf[] = { 0x37, 0xFB, 0x18, 0xFE };
        adamcore_z80_regs r;
        load(scf, sizeof scf);
        adamcore_debug_run(core, 1, 0, NULL);
        adamcore_cpu_aux(core, &aux);
        adamcore_get_regs(core, &r);
        check(aux.q != 0 && aux.q == r.f, "cpu aux", "q after SCF");
        adamcore_debug_run(core, 1, 0, NULL);
        adamcore_cpu_aux(core, &aux);
        check(aux.ei_pending == 1, "cpu aux", "ei_pending after EI");
    }
    adamcore_cpu_aux(core, &aux);
    n0 = aux.nmi_count;
    {   /* IN A,($BF): read (and clear) the VDP status so F can rise again */
        static const uint8_t in_status[] = { 0xDB, 0xBF, 0x18, 0xFE };
        load(in_status, sizeof in_status);
        adamcore_debug_run(core, 1, 0, NULL);
    }
    vdp_reg(1, 0x20); /* IE on: the next VBlank raises an NMI */
    adamcore_debug_run(core, 0, 0, NULL);
    adamcore_debug_run(core, 0, 0, NULL);
    adamcore_cpu_aux(core, &aux);
    check(aux.nmi_count == n0 + 1, "cpu aux", "nmi_count did not count the VBlank");
    check(aux.irq_pending == 0, "cpu aux", "irq pending with no source");
    vdp_reg(1, 0x00);

    adamcore_sgm_state(core, &sgm);
    check(sgm.configured == 0 && sgm.ram_en == 0, "sgm", "stock console reports an SGM");

    adamcore_set_joystick(core, 0, 0x1234);
    out_port(0xC0, 0);
    adamcore_controller_state(core, joy, &mode);
    check(joy[0] == 0x1234 && joy[1] == 0x7F7F && mode == 1, "controller", "joystick strobe");
    out_port(0x80, 0);
    adamcore_controller_state(core, NULL, &mode);
    check(mode == 0, "controller", "keypad strobe");
    if (!failures)
        printf("  ok  vdp latch, cpu aux, controller getters\n");
}

static void test_sgm(void)
{
    adamcore_sgm_info sgm;
    adamcore *plain = core;

    core = make_core(1);
    if (!core) {
        fail("sgm", "adamcore_create with an SGM");
        core = plain;
        return;
    }
    out_port(0x50, 0x03);
    out_port(0x51, 0x77);
    out_port(0x53, 0x01);
    out_port(0x7F, 0x00); /* bit 1 clear: RAM over the BIOS */
    adamcore_sgm_state(core, &sgm);
    check(sgm.configured == 1 && sgm.ram_en == 1 && sgm.bios_off == 1 &&
          sgm.ay_addr == 3 && sgm.ay_regs[3] == 0x77,
          "sgm", "state after OUT $50/$51/$53/$7F");
    out_port(0x7F, 0x02);
    adamcore_sgm_state(core, &sgm);
    check(sgm.bios_off == 0, "sgm", "BIOS not back after bit 1 set");
    adamcore_destroy(core);
    core = plain;
    if (!failures)
        printf("  ok  SGM state\n");
}

int main(void)
{
    memset(os7, 0, sizeof os7);
    core = make_core(0);
    if (!core) {
        fprintf(stderr, "FAIL: adamcore_create\n");
        return 1;
    }
    test_hook_order();
    test_tstate();
    test_line_capture();
    test_sn_mirror();
    test_getters();
    test_sgm();
    adamcore_destroy(core);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("debug_state: all passed\n");
    return 0;
}
