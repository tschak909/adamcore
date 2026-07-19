/*
 * adamcore - Zilog Z80 CPU core
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
 */

#ifndef ADAMCORE_Z80_H
#define ADAMCORE_Z80_H

#include <stdint.h>

/* NMOS Z80 interpreter. Instruction-stepped; per-instruction T-state totals
 * (no intra-instruction bus timing). Models the undocumented behavior games
 * and EOS depend on: X/Y flags, MEMPTR (WZ), the Q register (SCF/CCF), and
 * the flag quirks of the block instructions. */

typedef struct z80 {
    uint8_t a, f, b, c, d, e, h, l;
    uint8_t a2, f2, b2, c2, d2, e2, h2, l2; /* shadow set */
    uint16_t ix, iy, sp, pc;
    uint16_t wz; /* MEMPTR */
    uint8_t i, r;
    uint8_t iff1, iff2, im;
    uint8_t q;          /* F if the previous instruction wrote flags, else 0 */
    uint8_t halted;
    uint8_t ei_pending; /* IRQ acceptance blocked for one instruction */

    uint8_t irq_line;   /* level-triggered maskable interrupt request */
    uint8_t irq_vector; /* data bus byte during interrupt acknowledge */
    uint8_t nmi_pending;

    uint64_t cycles;

    void *ud;
    uint8_t (*mem_read)(void *ud, uint16_t addr);
    void (*mem_write)(void *ud, uint16_t addr, uint8_t data);
    uint8_t (*io_read)(void *ud, uint16_t port);
    void (*io_write)(void *ud, uint16_t port, uint8_t data);
} z80;

void z80_reset(z80 *z);
/* Executes one instruction or accepts one pending interrupt.
 * Returns T-states consumed (also accumulated into z->cycles). */
int z80_step(z80 *z);
void z80_nmi(z80 *z);
void z80_set_irq(z80 *z, int level, uint8_t vector);

#endif /* ADAMCORE_Z80_H */
