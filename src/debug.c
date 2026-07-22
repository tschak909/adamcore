/*
 * adamcore - debugger interface: instruction-stepped execution with a
 * resumable mid-frame cursor, CPU/memory/VDP inspection, PC breakpoints,
 * and a per-instruction hook.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * The stepped loop below must stay observably identical to
 * adamcore_run_frame in machine.c -- same helpers, same ordering,
 * especially the VBlank-NMI-before-line-192-slice rule (see the comment on
 * machine_vblank_nmi). tests/machine/debug_step_test.c pins the
 * equivalence cycle-for-cycle over thousands of boot frames.
 */

#include <string.h>

#include "adamcore_debug.h"
#include "machine.h"

static int bp_test(const adamcore *c, uint16_t pc)
{
    return (c->bp_map[pc >> 3] >> (pc & 7)) & 1;
}

adamcore_run_status adamcore_debug_run(adamcore *c, uint32_t max_instructions,
                                       int check_breakpoints, int *fb_changed)
{
    if (fb_changed)
        *fb_changed = 0;

    if (!c->dbg_in_frame) {
        machine_apply_pending_reset(c);
        c->frame_start_cycles = c->cpu.cycles;
        c->dbg_line = 0;
        c->dbg_line_target = c->frame_start_cycles + ADAM_LINE_TSTATES;
        c->dbg_in_frame = 1;
    }

    for (;;) {
        uint16_t pc;

        /* Close out any lines whose cycle budget the CPU has consumed.
         * (One instruction is at most ~23 T-states, well under a 228
         * T-state line, but the loop keeps this robust regardless.) */
        while (c->cpu.cycles >= c->dbg_line_target) {
            machine_line_end(c, c->dbg_line);
            c->dbg_line++;
            if (c->dbg_line == ADAM_FRAME_LINES) {
                int changed = machine_frame_tail(c);
                c->dbg_in_frame = 0;
                if (fb_changed)
                    *fb_changed = changed;
                return ADAMCORE_RUN_FRAME_DONE;
            }
            c->dbg_line_target =
                c->frame_start_cycles +
                (uint64_t)(c->dbg_line + 1) * ADAM_LINE_TSTATES;
            if (c->dbg_line == TMS_ACTIVE_H)
                machine_vblank_nmi(c);
        }

        pc = c->cpu.pc;
        if (check_breakpoints && c->bp_count && bp_test(c, pc))
            return ADAMCORE_RUN_BREAKPOINT;
        if (c->exec_hook && c->exec_hook(c->exec_hook_ud, c, pc))
            return ADAMCORE_RUN_HOOK_STOP;

        z80_step(&c->cpu);

        if (max_instructions && --max_instructions == 0)
            return ADAMCORE_RUN_STEPPED;
    }
}

/* ---- CPU state ------------------------------------------------------------ */

void adamcore_get_regs(const adamcore *c, adamcore_z80_regs *out)
{
    const z80 *z = &c->cpu;
    out->a = z->a; out->f = z->f;
    out->b = z->b; out->c = z->c;
    out->d = z->d; out->e = z->e;
    out->h = z->h; out->l = z->l;
    out->a2 = z->a2; out->f2 = z->f2;
    out->b2 = z->b2; out->c2 = z->c2;
    out->d2 = z->d2; out->e2 = z->e2;
    out->h2 = z->h2; out->l2 = z->l2;
    out->ix = z->ix; out->iy = z->iy;
    out->sp = z->sp; out->pc = z->pc;
    out->wz = z->wz;
    out->i = z->i; out->r = z->r;
    out->iff1 = z->iff1; out->iff2 = z->iff2;
    out->im = z->im; out->halted = z->halted;
    out->cycles = z->cycles;
}

void adamcore_set_regs(adamcore *c, const adamcore_z80_regs *in)
{
    z80 *z = &c->cpu;
    z->a = in->a; z->f = in->f;
    z->b = in->b; z->c = in->c;
    z->d = in->d; z->e = in->e;
    z->h = in->h; z->l = in->l;
    z->a2 = in->a2; z->f2 = in->f2;
    z->b2 = in->b2; z->c2 = in->c2;
    z->d2 = in->d2; z->e2 = in->e2;
    z->h2 = in->h2; z->l2 = in->l2;
    z->ix = in->ix; z->iy = in->iy;
    z->sp = in->sp; z->pc = in->pc;
    z->wz = in->wz;
    z->i = in->i; z->r = in->r;
    z->iff1 = in->iff1; z->iff2 = in->iff2;
    z->im = in->im; z->halted = in->halted;
    /* cycles are the machine's timebase; deliberately not settable */
}

/* ---- memory --------------------------------------------------------------- */

uint8_t adamcore_peek(const adamcore *c, uint16_t addr)
{
    return machine_mem_read((adamcore *)c, addr);
}

void adamcore_peek_block(const adamcore *c, uint16_t addr, uint8_t *dst,
                         uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        dst[i] = machine_mem_read((adamcore *)c, (uint16_t)(addr + i));
}

void adamcore_poke(adamcore *c, uint16_t addr, uint8_t v)
{
    machine_mem_write(c, addr, v);
}

void adamcore_get_machine_state(const adamcore *c, uint8_t *mem_ctrl,
                                uint8_t *net_ctrl, int *game_mode)
{
    if (mem_ctrl) *mem_ctrl = c->mem_ctrl;
    if (net_ctrl) *net_ctrl = c->net_ctrl;
    if (game_mode) *game_mode = c->game_mode;
}

/* ---- VDP ------------------------------------------------------------------ */

const uint8_t *adamcore_vdp_vram(const adamcore *c)
{
    return c->vdp.vram;
}

void adamcore_vdp_state(const adamcore *c, uint8_t regs[8], uint8_t *status,
                        uint16_t *addr)
{
    if (regs)
        memcpy(regs, c->vdp.regs, 8);
    if (status) *status = c->vdp.status;
    if (addr) *addr = c->vdp.addr;
}

const uint16_t *adamcore_palette565(const adamcore *c)
{
    return c->palette_rgb;
}

/* ---- breakpoints / hook ---------------------------------------------------- */

void adamcore_bp_set(adamcore *c, uint16_t addr)
{
    if (!bp_test(c, addr)) {
        c->bp_map[addr >> 3] |= (uint8_t)(1u << (addr & 7));
        c->bp_count++;
    }
}

void adamcore_bp_clear(adamcore *c, uint16_t addr)
{
    if (bp_test(c, addr)) {
        c->bp_map[addr >> 3] &= (uint8_t)~(1u << (addr & 7));
        c->bp_count--;
    }
}

void adamcore_bp_clear_all(adamcore *c)
{
    memset(c->bp_map, 0, sizeof(c->bp_map));
    c->bp_count = 0;
}

int adamcore_bp_count(const adamcore *c)
{
    return c->bp_count;
}

void adamcore_set_exec_hook(adamcore *c,
                            int (*hook)(void *ud, adamcore *c, uint16_t pc),
                            void *ud)
{
    c->exec_hook = hook;
    c->exec_hook_ud = ud;
}
