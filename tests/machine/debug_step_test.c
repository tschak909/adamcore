/*
 * debug_step_test: pins adamcore_debug_run to adamcore_run_frame.
 *
 * Two cores boot the same machine from the same ROMs; one runs whole
 * frames, the other runs the instruction-stepped debug loop to each frame
 * boundary. After every frame the CPU registers, cycle counter, and
 * framebuffer must match bit-for-bit -- this is what protects the VBlank
 * NMI-before-line-192 ordering (see machine_vblank_nmi) from regressing in
 * either loop. Also exercises breakpoints, single-stepping, the exec hook,
 * and the run_frame guard that finishes a debugger-abandoned frame.
 *
 * ROMs: pass a directory containing OS7.rom/EOS.rom/WP.rom as argv[1]
 * (default: tests/data/roms, else the ADAMCORE_TEST_ROMS environment
 * variable). Exits 77 (ctest SKIP) when no ROMs are available.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adamcore.h"
#include "adamcore_debug.h"

#define FRAMES 2000

static char rom_os7[512], rom_eos[512], rom_wp[512];

static int roms_at(const char *dir)
{
    FILE *f;
    snprintf(rom_os7, sizeof(rom_os7), "%s/OS7.rom", dir);
    snprintf(rom_eos, sizeof(rom_eos), "%s/EOS.rom", dir);
    snprintf(rom_wp, sizeof(rom_wp), "%s/WP.rom", dir);
    f = fopen(rom_os7, "rb");
    if (!f) return 0;
    fclose(f);
    f = fopen(rom_eos, "rb");
    if (!f) return 0;
    fclose(f);
    f = fopen(rom_wp, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static adamcore *make_core(void)
{
    adamcore_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.os7_rom_path = rom_os7;
    cfg.eos_rom_path = rom_eos;
    cfg.wp_rom_path = rom_wp;
    cfg.start_machine = ADAMCORE_MACHINE_ADAM;
    cfg.audio_rate = 44100;
    return adamcore_create(&cfg);
}

static int trace_hits;
static int count_hook(void *ud, adamcore *c, uint16_t pc)
{
    (void)ud;
    (void)c;
    (void)pc;
    trace_hits++;
    return 0;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : NULL;
    adamcore *ref, *dbg;
    int frame;

    if (!dir || !roms_at(dir)) {
        const char *env = getenv("ADAMCORE_TEST_ROMS");
        if (!((env && roms_at(env)) || roms_at("tests/data/roms") ||
              roms_at("../tests/data/roms"))) {
            fprintf(stderr, "debug_step_test: no ROMs (dir arg or "
                            "ADAMCORE_TEST_ROMS); skipping\n");
            return 77;
        }
    }

    ref = make_core();
    dbg = make_core();
    if (!ref || !dbg) {
        fprintf(stderr, "debug_step_test: core creation failed\n");
        return 1;
    }

    /* Inject the same keys into both so post-boot code paths diverge from
     * pure idling (SmartWriter reacts to keys). */
    for (frame = 0; frame < FRAMES; frame++) {
        adamcore_z80_regs r1, r2;
        int chg_ref, chg_dbg;
        const uint16_t *fb1, *fb2;
        adamcore_run_status st;

        if (frame == 500) {
            adamcore_inject_key(ref, 'a');
            adamcore_inject_key(dbg, 'a');
        }

        chg_ref = adamcore_run_frame(ref);
        do {
            st = adamcore_debug_run(dbg, 0, 0, &chg_dbg);
        } while (st != ADAMCORE_RUN_FRAME_DONE);

        adamcore_get_regs(ref, &r1);
        adamcore_get_regs(dbg, &r2);
        if (memcmp(&r1, &r2, sizeof(r1)) != 0) {
            fprintf(stderr,
                    "debug_step_test: register/cycle divergence at frame %d "
                    "(ref pc=%04X cyc=%llu, dbg pc=%04X cyc=%llu)\n",
                    frame, r1.pc, (unsigned long long)r1.cycles, r2.pc,
                    (unsigned long long)r2.cycles);
            return 1;
        }
        if (chg_ref != chg_dbg) {
            fprintf(stderr, "debug_step_test: fb-changed divergence at "
                            "frame %d (%d vs %d)\n", frame, chg_ref, chg_dbg);
            return 1;
        }
        fb1 = adamcore_framebuffer(ref, NULL, NULL);
        fb2 = adamcore_framebuffer(dbg, NULL, NULL);
        if (memcmp(fb1, fb2, ADAMCORE_FB_WIDTH * ADAMCORE_FB_HEIGHT * 2) != 0) {
            fprintf(stderr, "debug_step_test: framebuffer divergence at "
                            "frame %d\n", frame);
            return 1;
        }
    }

    /* Breakpoint at the current PC must hit before executing, then a
     * single step must move past it, and run_frame must cleanly finish the
     * partial frame the stopped debug run left behind. */
    {
        adamcore_z80_regs r;
        adamcore_run_status st;
        uint16_t bp_pc;
        int chg;

        adamcore_get_regs(dbg, &r);
        bp_pc = r.pc;
        adamcore_bp_set(dbg, bp_pc);
        st = adamcore_debug_run(dbg, 0, 1, NULL);
        if (st != ADAMCORE_RUN_BREAKPOINT) {
            fprintf(stderr, "debug_step_test: expected BREAKPOINT, got %d\n",
                    st);
            return 1;
        }
        adamcore_get_regs(dbg, &r);
        if (r.pc != bp_pc) {
            fprintf(stderr, "debug_step_test: breakpoint stopped at %04X, "
                            "expected %04X\n", r.pc, bp_pc);
            return 1;
        }
        st = adamcore_debug_run(dbg, 1, 0, NULL);
        if (st != ADAMCORE_RUN_STEPPED) {
            fprintf(stderr, "debug_step_test: expected STEPPED, got %d\n", st);
            return 1;
        }
        adamcore_bp_clear_all(dbg);

        adamcore_set_exec_hook(dbg, count_hook, NULL);
        chg = adamcore_run_frame(dbg); /* finishes the partial frame */
        (void)chg;
        adamcore_set_exec_hook(dbg, NULL, NULL);
        if (trace_hits == 0) {
            fprintf(stderr, "debug_step_test: exec hook never ran during the "
                            "partial-frame completion\n");
            return 1;
        }
    }

    printf("debug_step_test: %d frames equivalent; breakpoint/step/hook ok\n",
           FRAMES);
    adamcore_destroy(ref);
    adamcore_destroy(dbg);
    return 0;
}
