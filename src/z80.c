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
 *
 * Behavior references (no emulator source consulted):
 *  - Zilog Z80 CPU User Manual (UM008011)
 *  - Sean Young, "The Undocumented Z80 Documented"
 *  - Public MEMPTR (WZ) research notes (boo_boo / Vladimir Kladov)
 *  - Patrik Rak / David Banks published research on the Q register
 *    (SCF/CCF X/Y flags) and block-instruction flag behavior
 *  - Empirical validation against SingleStepTests Z80 vectors and
 *    Frank Cringle's ZEXDOC/ZEXALL
 */

#include "z80.h"

enum {
    CF = 0x01, NF = 0x02, PF = 0x04, XF = 0x08,
    HF = 0x10, YF = 0x20, ZF = 0x40, SF = 0x80
};

static uint8_t szp[256];
static int tables_ready;

static void init_tables(void)
{
    int v;
    for (v = 0; v < 256; v++) {
        int bits = v, ones = 0;
        while (bits) { ones += bits & 1; bits >>= 1; }
        szp[v] = (uint8_t)((v & (SF | XF | YF)) | (v == 0 ? ZF : 0) |
                           ((ones & 1) ? 0 : PF));
    }
    tables_ready = 1;
}

/* ---- bus helpers ---------------------------------------------------------- */

static uint8_t rd(z80 *z, uint16_t a) { return z->mem_read(z->ud, a); }
static void wr(z80 *z, uint16_t a, uint8_t v) { z->mem_write(z->ud, a, v); }
static uint8_t io_in(z80 *z, uint16_t p) { return z->io_read(z->ud, p); }
static void io_out(z80 *z, uint16_t p, uint8_t v) { z->io_write(z->ud, p, v); }

static uint16_t rd16(z80 *z, uint16_t a)
{
    uint8_t lo = rd(z, a);
    return (uint16_t)(lo | (rd(z, (uint16_t)(a + 1)) << 8));
}

static void wr16(z80 *z, uint16_t a, uint16_t v)
{
    wr(z, a, (uint8_t)v);
    wr(z, (uint16_t)(a + 1), (uint8_t)(v >> 8));
}

static uint8_t fetch8(z80 *z) { return rd(z, z->pc++); }

static uint16_t fetch16(z80 *z)
{
    uint8_t lo = fetch8(z);
    return (uint16_t)(lo | (fetch8(z) << 8));
}

static void push16(z80 *z, uint16_t v)
{
    wr(z, --z->sp, (uint8_t)(v >> 8));
    wr(z, --z->sp, (uint8_t)v);
}

static uint16_t pop16(z80 *z)
{
    uint8_t lo = rd(z, z->sp++);
    return (uint16_t)(lo | (rd(z, z->sp++) << 8));
}

static void r_inc(z80 *z) { z->r = (uint8_t)((z->r & 0x80) | ((z->r + 1) & 0x7F)); }

/* ---- register pair access ------------------------------------------------- */

#define BC(z) ((uint16_t)(((z)->b << 8) | (z)->c))
#define DE(z) ((uint16_t)(((z)->d << 8) | (z)->e))
#define HL(z) ((uint16_t)(((z)->h << 8) | (z)->l))
#define AF(z) ((uint16_t)(((z)->a << 8) | (z)->f))

static void set_bc(z80 *z, uint16_t v) { z->b = (uint8_t)(v >> 8); z->c = (uint8_t)v; }
static void set_de(z80 *z, uint16_t v) { z->d = (uint8_t)(v >> 8); z->e = (uint8_t)v; }
static void set_hl(z80 *z, uint16_t v) { z->h = (uint8_t)(v >> 8); z->l = (uint8_t)v; }

/* pfx: 0 = none (HL), 1 = DD (IX), 2 = FD (IY) */
static uint16_t get_hlp(z80 *z, int pfx)
{
    return pfx == 1 ? z->ix : pfx == 2 ? z->iy : HL(z);
}

static void set_hlp(z80 *z, int pfx, uint16_t v)
{
    if (pfx == 1) z->ix = v;
    else if (pfx == 2) z->iy = v;
    else set_hl(z, v);
}

/* 8-bit register by decode index (0..5,7); index 6 handled at call sites */
static uint8_t get_r(z80 *z, int i, int pfx)
{
    switch (i) {
    case 0: return z->b;
    case 1: return z->c;
    case 2: return z->d;
    case 3: return z->e;
    case 4: return pfx == 1 ? (uint8_t)(z->ix >> 8)
               : pfx == 2 ? (uint8_t)(z->iy >> 8) : z->h;
    case 5: return pfx == 1 ? (uint8_t)z->ix
               : pfx == 2 ? (uint8_t)z->iy : z->l;
    default: return z->a;
    }
}

static void set_r8(z80 *z, int i, int pfx, uint8_t v)
{
    switch (i) {
    case 0: z->b = v; break;
    case 1: z->c = v; break;
    case 2: z->d = v; break;
    case 3: z->e = v; break;
    case 4:
        if (pfx == 1) z->ix = (uint16_t)((z->ix & 0x00FF) | (v << 8));
        else if (pfx == 2) z->iy = (uint16_t)((z->iy & 0x00FF) | (v << 8));
        else z->h = v;
        break;
    case 5:
        if (pfx == 1) z->ix = (uint16_t)((z->ix & 0xFF00) | v);
        else if (pfx == 2) z->iy = (uint16_t)((z->iy & 0xFF00) | v);
        else z->l = v;
        break;
    default: z->a = v; break;
    }
}

/* pair by decode index p: 0 BC, 1 DE, 2 active HL, 3 SP (or AF for rp2) */
static uint16_t get_rp(z80 *z, int p, int pfx)
{
    switch (p) {
    case 0: return BC(z);
    case 1: return DE(z);
    case 2: return get_hlp(z, pfx);
    default: return z->sp;
    }
}

static void set_rp(z80 *z, int p, int pfx, uint16_t v)
{
    switch (p) {
    case 0: set_bc(z, v); break;
    case 1: set_de(z, v); break;
    case 2: set_hlp(z, pfx, v); break;
    default: z->sp = v; break;
    }
}

static int cond(z80 *z, int y)
{
    switch (y) {
    case 0: return !(z->f & ZF);
    case 1: return (z->f & ZF) != 0;
    case 2: return !(z->f & CF);
    case 3: return (z->f & CF) != 0;
    case 4: return !(z->f & PF);
    case 5: return (z->f & PF) != 0;
    case 6: return !(z->f & SF);
    default: return (z->f & SF) != 0;
    }
}

/* ---- ALU ------------------------------------------------------------------ */

static void set_f(z80 *z, uint8_t f) { z->f = f; z->q = f; }

static void alu_add(z80 *z, uint8_t v, int cy)
{
    unsigned r = (unsigned)z->a + v + (unsigned)cy;
    uint8_t r8 = (uint8_t)r;
    uint8_t f = (uint8_t)(r8 & (SF | XF | YF));
    if (!r8) f |= ZF;
    if (r > 0xFF) f |= CF;
    if (((z->a & 0x0F) + (v & 0x0F) + cy) > 0x0F) f |= HF;
    if ((~(z->a ^ v) & (z->a ^ r8)) & 0x80) f |= PF;
    z->a = r8;
    set_f(z, f);
}

static uint8_t sub_common(z80 *z, uint8_t v, int cy, uint8_t *fout)
{
    unsigned r = (unsigned)z->a - v - (unsigned)cy;
    uint8_t r8 = (uint8_t)r;
    uint8_t f = (uint8_t)((r8 & SF) | NF);
    if (!r8) f |= ZF;
    if (r & 0x100) f |= CF;
    if (((z->a & 0x0F) - (v & 0x0F) - cy) & 0x10) f |= HF;
    if (((z->a ^ v) & (z->a ^ r8)) & 0x80) f |= PF;
    *fout = f;
    return r8;
}

static void alu_sub(z80 *z, uint8_t v, int cy)
{
    uint8_t f;
    uint8_t r8 = sub_common(z, v, cy, &f);
    f |= (uint8_t)(r8 & (XF | YF));
    z->a = r8;
    set_f(z, f);
}

static void alu_cp(z80 *z, uint8_t v)
{
    uint8_t f;
    (void)sub_common(z, v, 0, &f);
    f |= (uint8_t)(v & (XF | YF)); /* CP takes X/Y from the operand */
    set_f(z, f);
}

static void alu_and(z80 *z, uint8_t v) { z->a &= v; set_f(z, (uint8_t)(szp[z->a] | HF)); }
static void alu_or(z80 *z, uint8_t v)  { z->a |= v; set_f(z, szp[z->a]); }
static void alu_xor(z80 *z, uint8_t v) { z->a ^= v; set_f(z, szp[z->a]); }

static void alu_op(z80 *z, int y, uint8_t v)
{
    switch (y) {
    case 0: alu_add(z, v, 0); break;
    case 1: alu_add(z, v, z->f & CF); break;
    case 2: alu_sub(z, v, 0); break;
    case 3: alu_sub(z, v, z->f & CF); break;
    case 4: alu_and(z, v); break;
    case 5: alu_xor(z, v); break;
    case 6: alu_or(z, v); break;
    default: alu_cp(z, v); break;
    }
}

static uint8_t inc8(z80 *z, uint8_t v)
{
    uint8_t r = (uint8_t)(v + 1);
    uint8_t f = (uint8_t)((z->f & CF) | (r & (SF | XF | YF)));
    if (!r) f |= ZF;
    if ((v & 0x0F) == 0x0F) f |= HF;
    if (v == 0x7F) f |= PF;
    set_f(z, f);
    return r;
}

static uint8_t dec8(z80 *z, uint8_t v)
{
    uint8_t r = (uint8_t)(v - 1);
    uint8_t f = (uint8_t)((z->f & CF) | NF | (r & (SF | XF | YF)));
    if (!r) f |= ZF;
    if ((v & 0x0F) == 0x00) f |= HF;
    if (v == 0x80) f |= PF;
    set_f(z, f);
    return r;
}

static uint16_t add16(z80 *z, uint16_t d, uint16_t s)
{
    uint32_t r = (uint32_t)d + s;
    uint8_t f = (uint8_t)(z->f & (SF | ZF | PF));
    z->wz = (uint16_t)(d + 1);
    if (r > 0xFFFF) f |= CF;
    if (((d & 0x0FFF) + (s & 0x0FFF)) > 0x0FFF) f |= HF;
    f |= (uint8_t)((r >> 8) & (XF | YF));
    set_f(z, f);
    return (uint16_t)r;
}

static void adc16(z80 *z, uint16_t s)
{
    uint16_t hl = HL(z);
    int cy = z->f & CF;
    uint32_t r = (uint32_t)hl + s + (uint32_t)cy;
    uint16_t r16 = (uint16_t)r;
    uint8_t f = 0;
    z->wz = (uint16_t)(hl + 1);
    if (r > 0xFFFF) f |= CF;
    if (((hl & 0x0FFF) + (s & 0x0FFF) + (unsigned)cy) > 0x0FFF) f |= HF;
    if (!r16) f |= ZF;
    f |= (uint8_t)((r16 >> 8) & (SF | XF | YF));
    if ((~(hl ^ s) & (hl ^ r16)) & 0x8000) f |= PF;
    set_hl(z, r16);
    set_f(z, f);
}

static void sbc16(z80 *z, uint16_t s)
{
    uint16_t hl = HL(z);
    int cy = z->f & CF;
    uint32_t r = (uint32_t)hl - s - (uint32_t)cy;
    uint16_t r16 = (uint16_t)r;
    uint8_t f = NF;
    z->wz = (uint16_t)(hl + 1);
    if (r & 0x10000) f |= CF;
    if (((hl & 0x0FFF) - (s & 0x0FFF) - (unsigned)cy) & 0x1000) f |= HF;
    if (!r16) f |= ZF;
    f |= (uint8_t)((r16 >> 8) & (SF | XF | YF));
    if (((hl ^ s) & (hl ^ r16)) & 0x8000) f |= PF;
    set_hl(z, r16);
    set_f(z, f);
}

/* ---- rotates / shifts (CB) ------------------------------------------------ */

static uint8_t rot_apply(z80 *z, int y, uint8_t v)
{
    uint8_t c;
    switch (y) {
    case 0: /* RLC */ c = (uint8_t)(v >> 7); v = (uint8_t)((v << 1) | c); break;
    case 1: /* RRC */ c = (uint8_t)(v & 1); v = (uint8_t)((v >> 1) | (c << 7)); break;
    case 2: /* RL  */ c = (uint8_t)(v >> 7); v = (uint8_t)((v << 1) | (z->f & CF)); break;
    case 3: /* RR  */ c = (uint8_t)(v & 1); v = (uint8_t)((v >> 1) | ((z->f & CF) << 7)); break;
    case 4: /* SLA */ c = (uint8_t)(v >> 7); v = (uint8_t)(v << 1); break;
    case 5: /* SRA */ c = (uint8_t)(v & 1); v = (uint8_t)((v >> 1) | (v & 0x80)); break;
    case 6: /* SLL (undocumented) */ c = (uint8_t)(v >> 7); v = (uint8_t)((v << 1) | 1); break;
    default: /* SRL */ c = (uint8_t)(v & 1); v = (uint8_t)(v >> 1); break;
    }
    set_f(z, (uint8_t)(szp[v] | (c ? CF : 0)));
    return v;
}

static void bit_op(z80 *z, int b, uint8_t v, uint8_t xysrc)
{
    uint8_t t = (uint8_t)(v & (1 << b));
    uint8_t f = (uint8_t)((z->f & CF) | HF | (xysrc & (XF | YF)));
    if (!t) f |= ZF | PF;
    if (b == 7 && t) f |= SF;
    set_f(z, f);
}

/* ---- rotate/decimal accumulator ops --------------------------------------- */

static void rlca(z80 *z)
{
    uint8_t c = (uint8_t)(z->a >> 7);
    z->a = (uint8_t)((z->a << 1) | c);
    set_f(z, (uint8_t)((z->f & (SF | ZF | PF)) | (z->a & (XF | YF)) | (c ? CF : 0)));
}

static void rrca(z80 *z)
{
    uint8_t c = (uint8_t)(z->a & 1);
    z->a = (uint8_t)((z->a >> 1) | (c << 7));
    set_f(z, (uint8_t)((z->f & (SF | ZF | PF)) | (z->a & (XF | YF)) | (c ? CF : 0)));
}

static void rla(z80 *z)
{
    uint8_t c = (uint8_t)(z->a >> 7);
    z->a = (uint8_t)((z->a << 1) | (z->f & CF));
    set_f(z, (uint8_t)((z->f & (SF | ZF | PF)) | (z->a & (XF | YF)) | (c ? CF : 0)));
}

static void rra(z80 *z)
{
    uint8_t c = (uint8_t)(z->a & 1);
    z->a = (uint8_t)((z->a >> 1) | ((z->f & CF) << 7));
    set_f(z, (uint8_t)((z->f & (SF | ZF | PF)) | (z->a & (XF | YF)) | (c ? CF : 0)));
}

static void daa(z80 *z)
{
    uint8_t a = z->a, f = z->f, corr = 0;
    uint8_t c = (uint8_t)(f & CF);
    if ((f & HF) || (a & 0x0F) > 9) corr |= 0x06;
    if (c || a > 0x99) { corr |= 0x60; c = CF; }
    uint8_t r = (f & NF) ? (uint8_t)(a - corr) : (uint8_t)(a + corr);
    uint8_t nf = (uint8_t)((f & NF) | szp[r] | (((a ^ r) & 0x10) ? HF : 0) | c);
    z->a = r;
    set_f(z, nf);
}

static void scf_ccf(z80 *z, int is_ccf, uint8_t old_q)
{
    /* NMOS: X/Y come from A, ORed with F's X/Y only if the previous
     * instruction did not write flags (Q model). */
    uint8_t f = z->f;
    uint8_t xy = (uint8_t)((((old_q ^ f) | z->a)) & (XF | YF));
    uint8_t nf = (uint8_t)((f & (SF | ZF | PF)) | xy);
    if (is_ccf) {
        if (f & CF) nf |= HF; else nf |= CF;
    } else {
        nf |= CF;
    }
    set_f(z, nf);
}

/* ---- block instructions --------------------------------------------------- */

static void block_repeat_xy(z80 *z)
{
    /* When a block instruction repeats, X/Y are exposed from PC-high */
    z->f = (uint8_t)((z->f & ~(XF | YF)) | ((z->pc >> 8) & (XF | YF)));
    z->q = z->f;
}

static int op_ldi_ldd(z80 *z, int dir, int rep)
{
    uint16_t hl = HL(z), de = DE(z);
    uint8_t v = rd(z, hl);
    uint16_t bc;
    wr(z, de, v);
    set_hl(z, (uint16_t)(hl + dir));
    set_de(z, (uint16_t)(de + dir));
    bc = (uint16_t)(BC(z) - 1);
    set_bc(z, bc);
    {
        uint8_t n = (uint8_t)(v + z->a);
        uint8_t f = (uint8_t)(z->f & (SF | ZF | CF));
        if (bc) f |= PF;
        if (n & 0x08) f |= XF;
        if (n & 0x02) f |= YF;
        set_f(z, f);
    }
    if (rep && bc) {
        z->pc = (uint16_t)(z->pc - 2);
        z->wz = (uint16_t)(z->pc + 1);
        block_repeat_xy(z);
        return 21;
    }
    return 16;
}

static int op_cpi_cpd(z80 *z, int dir, int rep)
{
    uint16_t hl = HL(z);
    uint8_t v = rd(z, hl);
    uint8_t r8 = (uint8_t)(z->a - v);
    int hf = ((z->a & 0x0F) - (v & 0x0F)) & 0x10;
    uint16_t bc;
    set_hl(z, (uint16_t)(hl + dir));
    bc = (uint16_t)(BC(z) - 1);
    set_bc(z, bc);
    z->wz = (uint16_t)(z->wz + dir);
    {
        uint8_t n = (uint8_t)(r8 - (hf ? 1 : 0));
        uint8_t f = (uint8_t)((z->f & CF) | NF | (r8 & SF));
        if (!r8) f |= ZF;
        if (hf) f |= HF;
        if (bc) f |= PF;
        if (n & 0x08) f |= XF;
        if (n & 0x02) f |= YF;
        set_f(z, f);
    }
    if (rep && bc && r8) {
        z->pc = (uint16_t)(z->pc - 2);
        z->wz = (uint16_t)(z->pc + 1);
        block_repeat_xy(z);
        return 21;
    }
    return 16;
}

static void block_io_flags(z80 *z, uint8_t data, uint16_t k)
{
    uint8_t f = (uint8_t)((z->b & (SF | XF | YF)) | (z->b == 0 ? ZF : 0));
    if (data & 0x80) f |= NF;
    if (k > 0xFF) f |= (HF | CF);
    f |= (uint8_t)(szp[(k & 7) ^ z->b] & PF);
    set_f(z, f);
}

static void block_io_repeat_flags(z80 *z, uint8_t data)
{
    /* Flag adjustment when an I/O block instruction repeats (NMOS quirk,
     * per published research on interrupted block instructions). */
    block_repeat_xy(z);
    if (z->f & CF) {
        if (data & 0x80) {
            z->f ^= (uint8_t)((szp[(z->b - 1) & 7] & PF) ^ PF);
            if ((z->b & 0x0F) == 0x00) z->f |= HF; else z->f &= (uint8_t)~HF;
        } else {
            z->f ^= (uint8_t)((szp[(z->b + 1) & 7] & PF) ^ PF);
            if ((z->b & 0x0F) == 0x0F) z->f |= HF; else z->f &= (uint8_t)~HF;
        }
    } else {
        z->f ^= (uint8_t)((szp[z->b & 7] & PF) ^ PF);
        z->f &= (uint8_t)~HF;
    }
    z->q = z->f;
}

static int op_ini_ind(z80 *z, int dir, int rep)
{
    uint16_t bc = BC(z);
    uint8_t v = io_in(z, bc);
    uint16_t hl = HL(z);
    z->wz = (uint16_t)(bc + dir);
    wr(z, hl, v);
    set_hl(z, (uint16_t)(hl + dir));
    z->b--;
    {
        uint16_t k = (uint16_t)(v + ((z->c + dir) & 0xFF));
        block_io_flags(z, v, k);
    }
    if (rep && z->b) {
        z->pc = (uint16_t)(z->pc - 2);
        z->wz = (uint16_t)(z->pc + 1);
        block_io_repeat_flags(z, v);
        return 21;
    }
    return 16;
}

static int op_outi_outd(z80 *z, int dir, int rep)
{
    uint16_t hl = HL(z);
    uint8_t v = rd(z, hl);
    z->b--;
    io_out(z, BC(z), v);
    set_hl(z, (uint16_t)(hl + dir));
    z->wz = (uint16_t)(BC(z) + dir);
    {
        uint16_t k = (uint16_t)(v + z->l);
        block_io_flags(z, v, k);
    }
    if (rep && z->b) {
        z->pc = (uint16_t)(z->pc - 2);
        z->wz = (uint16_t)(z->pc + 1);
        block_io_repeat_flags(z, v);
        return 21;
    }
    return 16;
}

/* ---- CB / DDCB ------------------------------------------------------------ */

static int exec_cb(z80 *z)
{
    uint8_t op;
    int x, y, zf;
    r_inc(z);
    op = fetch8(z);
    x = op >> 6; y = (op >> 3) & 7; zf = op & 7;

    if (zf == 6) {
        uint16_t addr = HL(z);
        uint8_t v = rd(z, addr);
        switch (x) {
        case 0: wr(z, addr, rot_apply(z, y, v)); return 15;
        case 1: bit_op(z, y, v, (uint8_t)(z->wz >> 8)); return 12;
        case 2: wr(z, addr, (uint8_t)(v & ~(1 << y))); return 15;
        default: wr(z, addr, (uint8_t)(v | (1 << y))); return 15;
        }
    } else {
        uint8_t v = get_r(z, zf, 0);
        switch (x) {
        case 0: set_r8(z, zf, 0, rot_apply(z, y, v)); return 8;
        case 1: bit_op(z, y, v, v); return 8;
        case 2: set_r8(z, zf, 0, (uint8_t)(v & ~(1 << y))); return 8;
        default: set_r8(z, zf, 0, (uint8_t)(v | (1 << y))); return 8;
        }
    }
}

static int exec_ddcb(z80 *z, int pfx)
{
    /* DD/FD CB d op: always operates on (IX+d); non-(HL) register fields
     * additionally copy the result into that register (undocumented). */
    uint16_t base = pfx == 1 ? z->ix : z->iy;
    int8_t d = (int8_t)fetch8(z);
    uint8_t op = fetch8(z);
    uint16_t addr = (uint16_t)(base + d);
    int x = op >> 6, y = (op >> 3) & 7, zf = op & 7;
    uint8_t v, res;

    z->wz = addr;
    v = rd(z, addr);
    if (x == 1) {
        bit_op(z, y, v, (uint8_t)(addr >> 8));
        return 16; /* +4 prefix already counted = 20 total */
    }
    switch (x) {
    case 0: res = rot_apply(z, y, v); break;
    case 2: res = (uint8_t)(v & ~(1 << y)); break;
    default: res = (uint8_t)(v | (1 << y)); break;
    }
    wr(z, addr, res);
    if (zf != 6) set_r8(z, zf, 0, res);
    return 19; /* +4 prefix = 23 total */
}

/* ---- ED ------------------------------------------------------------------- */

static int exec_ed(z80 *z)
{
    static const uint8_t im_table[8] = { 0, 0, 1, 2, 0, 0, 1, 2 };
    uint8_t op;
    int x, y, zf, p, qq;
    r_inc(z);
    op = fetch8(z);
    x = op >> 6; y = (op >> 3) & 7; zf = op & 7; p = y >> 1; qq = y & 1;

    if (x == 1) {
        switch (zf) {
        case 0: { /* IN r,(C) / IN (C) */
            uint8_t v = io_in(z, BC(z));
            z->wz = (uint16_t)(BC(z) + 1);
            set_f(z, (uint8_t)((z->f & CF) | szp[v]));
            if (y != 6) set_r8(z, y, 0, v);
            return 12;
        }
        case 1: /* OUT (C),r / OUT (C),0 */
            io_out(z, BC(z), y == 6 ? 0 : get_r(z, y, 0));
            z->wz = (uint16_t)(BC(z) + 1);
            return 12;
        case 2: /* SBC/ADC HL,rp */
            if (qq) adc16(z, get_rp(z, p, 0));
            else sbc16(z, get_rp(z, p, 0));
            return 15;
        case 3: { /* LD (nn),rp / LD rp,(nn) */
            uint16_t nn = fetch16(z);
            if (qq) set_rp(z, p, 0, rd16(z, nn));
            else wr16(z, nn, get_rp(z, p, 0));
            z->wz = (uint16_t)(nn + 1);
            return 20;
        }
        case 4: { /* NEG (and mirrors) */
            uint8_t a = z->a;
            z->a = 0;
            alu_sub(z, a, 0);
            return 8;
        }
        case 5: /* RETN / RETI (and mirrors): IFF1 <- IFF2 */
            z->pc = pop16(z);
            z->wz = z->pc;
            z->iff1 = z->iff2;
            return 14;
        case 6:
            z->im = im_table[y];
            return 8;
        default:
            switch (y) {
            case 0: z->i = z->a; return 9;
            case 1: z->r = z->a; return 9;
            case 2: { /* LD A,I */
                z->a = z->i;
                set_f(z, (uint8_t)((z->f & CF) | (z->a & (SF | XF | YF)) |
                                   (z->a == 0 ? ZF : 0) | (z->iff2 ? PF : 0)));
                return 9;
            }
            case 3: { /* LD A,R */
                z->a = z->r;
                set_f(z, (uint8_t)((z->f & CF) | (z->a & (SF | XF | YF)) |
                                   (z->a == 0 ? ZF : 0) | (z->iff2 ? PF : 0)));
                return 9;
            }
            case 4: { /* RRD */
                uint16_t hl = HL(z);
                uint8_t m = rd(z, hl);
                uint8_t nm = (uint8_t)((m >> 4) | ((z->a & 0x0F) << 4));
                z->a = (uint8_t)((z->a & 0xF0) | (m & 0x0F));
                wr(z, hl, nm);
                z->wz = (uint16_t)(hl + 1);
                set_f(z, (uint8_t)((z->f & CF) | szp[z->a]));
                return 18;
            }
            case 5: { /* RLD */
                uint16_t hl = HL(z);
                uint8_t m = rd(z, hl);
                uint8_t nm = (uint8_t)((m << 4) | (z->a & 0x0F));
                z->a = (uint8_t)((z->a & 0xF0) | (m >> 4));
                wr(z, hl, nm);
                z->wz = (uint16_t)(hl + 1);
                set_f(z, (uint8_t)((z->f & CF) | szp[z->a]));
                return 18;
            }
            default:
                return 8; /* ED 77 / ED 7F: NOP */
            }
        }
    }
    if (x == 2 && zf < 4 && y >= 4) {
        int dir = (y & 1) ? -1 : 1;
        int rep = y >= 6;
        switch (zf) {
        case 0: return op_ldi_ldd(z, dir, rep);
        case 1: return op_cpi_cpd(z, dir, rep);
        case 2: return op_ini_ind(z, dir, rep);
        default: return op_outi_outd(z, dir, rep);
        }
    }
    return 8; /* undefined ED: NONI + NOP */
}

/* ---- main decode ----------------------------------------------------------- */

/* Does this unprefixed opcode reference (HL) as an operand? */
static int uses_hl_mem(uint8_t op)
{
    int x = op >> 6, y = (op >> 3) & 7, zf = op & 7;
    switch (x) {
    case 0: return zf >= 4 && zf <= 6 && y == 6;
    case 1: return (y == 6) != (zf == 6); /* both = HALT */
    case 2: return zf == 6;
    default: return 0;
    }
}

static int exec_main(z80 *z, uint8_t op, int pfx, uint8_t old_q)
{
    int x = op >> 6, y = (op >> 3) & 7, zf = op & 7;
    int p = y >> 1, qq = y & 1;
    int cyc = 0;
    uint16_t maddr = 0;
    int mem = uses_hl_mem(op);

    if (mem) {
        if (pfx) {
            int8_t d = (int8_t)fetch8(z);
            maddr = (uint16_t)(get_hlp(z, pfx) + d);
            z->wz = maddr;
            cyc += (op == 0x36) ? 5 : 8;
            pfx = 0; /* register fields revert to real H/L alongside (IX+d) */
        } else {
            maddr = HL(z);
        }
    }

    switch (x) {
    case 0:
        switch (zf) {
        case 0:
            switch (y) {
            case 0: return cyc + 4; /* NOP */
            case 1: { /* EX AF,AF' */
                uint8_t t;
                t = z->a; z->a = z->a2; z->a2 = t;
                t = z->f; z->f = z->f2; z->f2 = t;
                return cyc + 4;
            }
            case 2: { /* DJNZ d */
                int8_t d = (int8_t)fetch8(z);
                if (--z->b) {
                    z->pc = (uint16_t)(z->pc + d);
                    z->wz = z->pc;
                    return cyc + 13;
                }
                return cyc + 8;
            }
            case 3: { /* JR d */
                int8_t d = (int8_t)fetch8(z);
                z->pc = (uint16_t)(z->pc + d);
                z->wz = z->pc;
                return cyc + 12;
            }
            default: { /* JR cc,d */
                int8_t d = (int8_t)fetch8(z);
                if (cond(z, y - 4)) {
                    z->pc = (uint16_t)(z->pc + d);
                    z->wz = z->pc;
                    return cyc + 12;
                }
                return cyc + 7;
            }
            }
        case 1:
            if (!qq) { /* LD rp,nn */
                set_rp(z, p, pfx, fetch16(z));
                return cyc + 10;
            }
            /* ADD HL,rp */
            set_hlp(z, pfx, add16(z, get_hlp(z, pfx), get_rp(z, p, pfx)));
            return cyc + 11;
        case 2:
            switch (y) {
            case 0: /* LD (BC),A */
                wr(z, BC(z), z->a);
                z->wz = (uint16_t)(((BC(z) + 1) & 0xFF) | (z->a << 8));
                return cyc + 7;
            case 1: /* LD A,(BC) */
                z->a = rd(z, BC(z));
                z->wz = (uint16_t)(BC(z) + 1);
                return cyc + 7;
            case 2: /* LD (DE),A */
                wr(z, DE(z), z->a);
                z->wz = (uint16_t)(((DE(z) + 1) & 0xFF) | (z->a << 8));
                return cyc + 7;
            case 3: /* LD A,(DE) */
                z->a = rd(z, DE(z));
                z->wz = (uint16_t)(DE(z) + 1);
                return cyc + 7;
            case 4: { /* LD (nn),HL */
                uint16_t nn = fetch16(z);
                wr16(z, nn, get_hlp(z, pfx));
                z->wz = (uint16_t)(nn + 1);
                return cyc + 16;
            }
            case 5: { /* LD HL,(nn) */
                uint16_t nn = fetch16(z);
                set_hlp(z, pfx, rd16(z, nn));
                z->wz = (uint16_t)(nn + 1);
                return cyc + 16;
            }
            case 6: { /* LD (nn),A */
                uint16_t nn = fetch16(z);
                wr(z, nn, z->a);
                z->wz = (uint16_t)(((nn + 1) & 0xFF) | (z->a << 8));
                return cyc + 13;
            }
            default: { /* LD A,(nn) */
                uint16_t nn = fetch16(z);
                z->a = rd(z, nn);
                z->wz = (uint16_t)(nn + 1);
                return cyc + 13;
            }
            }
        case 3: /* INC/DEC rp */
            set_rp(z, p, pfx, (uint16_t)(get_rp(z, p, pfx) + (qq ? -1 : 1)));
            return cyc + 6;
        case 4: /* INC r */
            if (y == 6) { wr(z, maddr, inc8(z, rd(z, maddr))); return cyc + 11; }
            set_r8(z, y, pfx, inc8(z, get_r(z, y, pfx)));
            return cyc + 4;
        case 5: /* DEC r */
            if (y == 6) { wr(z, maddr, dec8(z, rd(z, maddr))); return cyc + 11; }
            set_r8(z, y, pfx, dec8(z, get_r(z, y, pfx)));
            return cyc + 4;
        case 6: { /* LD r,n */
            uint8_t n = fetch8(z);
            if (y == 6) { wr(z, maddr, n); return cyc + 10; }
            set_r8(z, y, pfx, n);
            return cyc + 7;
        }
        default:
            switch (y) {
            case 0: rlca(z); return cyc + 4;
            case 1: rrca(z); return cyc + 4;
            case 2: rla(z); return cyc + 4;
            case 3: rra(z); return cyc + 4;
            case 4: daa(z); return cyc + 4;
            case 5: /* CPL */
                z->a = (uint8_t)~z->a;
                set_f(z, (uint8_t)((z->f & (SF | ZF | PF | CF)) | HF | NF |
                                   (z->a & (XF | YF))));
                return cyc + 4;
            case 6: scf_ccf(z, 0, old_q); return cyc + 4;
            default: scf_ccf(z, 1, old_q); return cyc + 4;
            }
        }
    case 1: /* LD r,r' (or HALT) */
        if (y == 6 && zf == 6) { /* HALT */
            z->halted = 1;
            return cyc + 4;
        }
        if (y == 6) { wr(z, maddr, get_r(z, zf, pfx)); return cyc + 7; }
        if (zf == 6) { set_r8(z, y, pfx, rd(z, maddr)); return cyc + 7; }
        set_r8(z, y, pfx, get_r(z, zf, pfx));
        return cyc + 4;
    case 2: /* ALU A,r */
        alu_op(z, y, zf == 6 ? rd(z, maddr) : get_r(z, zf, pfx));
        return cyc + (zf == 6 ? 7 : 4);
    default:
        switch (zf) {
        case 0: /* RET cc */
            if (cond(z, y)) {
                z->pc = pop16(z);
                z->wz = z->pc;
                return cyc + 11;
            }
            return cyc + 5;
        case 1:
            if (!qq) { /* POP rp2 */
                uint16_t v = pop16(z);
                if (p == 3) { z->a = (uint8_t)(v >> 8); z->f = (uint8_t)v; }
                else set_rp(z, p, pfx, v);
                return cyc + 10;
            }
            switch (p) {
            case 0: /* RET */
                z->pc = pop16(z);
                z->wz = z->pc;
                return cyc + 10;
            case 1: { /* EXX */
                uint8_t t;
                t = z->b; z->b = z->b2; z->b2 = t;
                t = z->c; z->c = z->c2; z->c2 = t;
                t = z->d; z->d = z->d2; z->d2 = t;
                t = z->e; z->e = z->e2; z->e2 = t;
                t = z->h; z->h = z->h2; z->h2 = t;
                t = z->l; z->l = z->l2; z->l2 = t;
                return cyc + 4;
            }
            case 2: /* JP (HL) */
                z->pc = get_hlp(z, pfx);
                return cyc + 4;
            default: /* LD SP,HL */
                z->sp = get_hlp(z, pfx);
                return cyc + 6;
            }
        case 2: { /* JP cc,nn */
            uint16_t nn = fetch16(z);
            z->wz = nn;
            if (cond(z, y)) z->pc = nn;
            return cyc + 10;
        }
        case 3:
            switch (y) {
            case 0: { /* JP nn */
                uint16_t nn = fetch16(z);
                z->wz = nn;
                z->pc = nn;
                return cyc + 10;
            }
            case 1: /* CB prefix: handled in z80_step */
                return cyc; /* unreachable */
            case 2: { /* OUT (n),A */
                uint8_t n = fetch8(z);
                uint16_t port = (uint16_t)((z->a << 8) | n);
                io_out(z, port, z->a);
                z->wz = (uint16_t)(((port + 1) & 0xFF) | (z->a << 8));
                return cyc + 11;
            }
            case 3: { /* IN A,(n) */
                uint8_t n = fetch8(z);
                uint16_t port = (uint16_t)((z->a << 8) | n);
                z->a = io_in(z, port);
                z->wz = (uint16_t)(port + 1);
                return cyc + 11;
            }
            case 4: { /* EX (SP),HL */
                uint16_t old = get_hlp(z, pfx);
                uint16_t v = rd16(z, z->sp);
                wr16(z, z->sp, old);
                set_hlp(z, pfx, v);
                z->wz = v;
                return cyc + 19;
            }
            case 5: { /* EX DE,HL (never indexed) */
                uint16_t de = DE(z), hl = HL(z);
                set_de(z, hl);
                set_hl(z, de);
                return cyc + 4;
            }
            case 6: /* DI */
                z->iff1 = z->iff2 = 0;
                return cyc + 4;
            default: /* EI */
                z->iff1 = z->iff2 = 1;
                z->ei_pending = 1;
                return cyc + 4;
            }
        case 4: { /* CALL cc,nn */
            uint16_t nn = fetch16(z);
            z->wz = nn;
            if (cond(z, y)) {
                push16(z, z->pc);
                z->pc = nn;
                return cyc + 17;
            }
            return cyc + 10;
        }
        case 5:
            if (!qq) { /* PUSH rp2 */
                push16(z, p == 3 ? AF(z) : get_rp(z, p, pfx));
                return cyc + 11;
            }
            /* p==0: CALL nn (p 1,2,3 are DD/ED/FD prefixes, handled in step) */
            {
                uint16_t nn = fetch16(z);
                z->wz = nn;
                push16(z, z->pc);
                z->pc = nn;
                return cyc + 17;
            }
        case 6: /* ALU A,n */
            alu_op(z, y, fetch8(z));
            return cyc + 7;
        default: /* RST */
            push16(z, z->pc);
            z->pc = (uint16_t)(y * 8);
            z->wz = z->pc;
            return cyc + 11;
        }
    }
}

/* ---- interrupts ------------------------------------------------------------ */

static int accept_nmi(z80 *z)
{
    z->nmi_pending = 0;
    z->halted = 0;
    r_inc(z);
    z->iff1 = 0;
    push16(z, z->pc);
    z->pc = 0x0066;
    z->wz = z->pc;
    return 11;
}

static int accept_irq(z80 *z)
{
    z->halted = 0;
    r_inc(z);
    z->iff1 = z->iff2 = 0;
    push16(z, z->pc);
    switch (z->im) {
    case 2: {
        uint16_t vaddr = (uint16_t)((z->i << 8) | z->irq_vector);
        z->pc = rd16(z, vaddr);
        z->wz = z->pc;
        return 19;
    }
    case 1:
        z->pc = 0x0038;
        z->wz = z->pc;
        return 13;
    default:
        /* IM 0: execute the bus byte; RST vectors are the practical case */
        if ((z->irq_vector & 0xC7) == 0xC7)
            z->pc = (uint16_t)(z->irq_vector & 0x38);
        else
            z->pc = 0x0038;
        z->wz = z->pc;
        return 13;
    }
}

/* ---- public ---------------------------------------------------------------- */

void z80_reset(z80 *z)
{
    if (!tables_ready) init_tables();
    z->pc = 0;
    z->i = 0;
    z->r = 0;
    z->iff1 = z->iff2 = 0;
    z->im = 0;
    z->sp = 0xFFFF;
    z->a = 0xFF;
    z->f = 0xFF;
    z->wz = 0;
    z->q = 0;
    z->halted = 0;
    z->ei_pending = 0;
    z->nmi_pending = 0;
    z->irq_line = 0;
}

void z80_nmi(z80 *z) { z->nmi_pending = 1; }

void z80_set_irq(z80 *z, int level, uint8_t vector)
{
    z->irq_line = (uint8_t)(level != 0);
    z->irq_vector = vector;
}

int z80_step(z80 *z)
{
    uint8_t old_q;
    uint8_t op;
    int pfx = 0;
    int cyc = 0;

    if (!tables_ready) init_tables();

    if (z->nmi_pending) {
        cyc = accept_nmi(z);
        z->cycles += (uint64_t)cyc;
        return cyc;
    }
    if (z->irq_line && z->iff1 && !z->ei_pending) {
        cyc = accept_irq(z);
        z->cycles += (uint64_t)cyc;
        return cyc;
    }
    if (z->ei_pending) z->ei_pending = 0;

    if (z->halted) {
        r_inc(z);
        z->cycles += 4;
        return 4;
    }

    old_q = z->q;
    z->q = 0;

    for (;;) {
        op = fetch8(z);
        r_inc(z);
        /* A DD/FD prefix is its own M1 cycle that writes no flags, so it
         * clears Q — observable in prefixed SCF/CCF. */
        if (op == 0xDD) { pfx = 1; cyc += 4; old_q = 0; continue; }
        if (op == 0xFD) { pfx = 2; cyc += 4; old_q = 0; continue; }
        break;
    }

    if (op == 0xCB)
        cyc += pfx ? exec_ddcb(z, pfx) : exec_cb(z);
    else if (op == 0xED)
        cyc += exec_ed(z); /* preceding DD/FD acts as NONI */
    else
        cyc += exec_main(z, op, pfx, old_q);

    z->cycles += (uint64_t)cyc;
    return cyc;
}
