/*
 * adamcore - ZEXDOC/ZEXALL runner
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal CP/M harness: loads a .com at 0x0100, traps BDOS calls at 0x0005
 * (C=2 console out, C=9 print $-terminated string), ends at a jump to 0x0000.
 * Exits nonzero if the exerciser reported any ERROR or never completed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "z80.h"

static uint8_t mem[65536];

static uint8_t m_rd(void *u, uint16_t a) { (void)u; return mem[a]; }
static void m_wr(void *u, uint16_t a, uint8_t v) { (void)u; mem[a] = v; }
static uint8_t p_in(void *u, uint16_t p) { (void)u; (void)p; return 0xFF; }
static void p_out(void *u, uint16_t p, uint8_t v) { (void)u; (void)p; (void)v; }

int main(int argc, char **argv)
{
    z80 z;
    FILE *fp;
    size_t n;
    char out[1 << 16];
    size_t outlen = 0;
    uint64_t max_cycles = 200000000000ULL; /* generous; zexall is ~46G T */

    if (argc < 2) { fprintf(stderr, "usage: %s <zex.com>\n", argv[0]); return 2; }
    fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 2; }
    memset(mem, 0, sizeof(mem));
    n = fread(mem + 0x100, 1, sizeof(mem) - 0x100, fp);
    fclose(fp);
    if (n < 100) { fprintf(stderr, "short read\n"); return 2; }

    mem[0x0005] = 0xC9; /* RET (BDOS trap point) */

    memset(&z, 0, sizeof(z));
    z.mem_read = m_rd;
    z.mem_write = m_wr;
    z.io_read = p_in;
    z.io_write = p_out;
    z80_reset(&z);
    z.pc = 0x0100;
    z.sp = 0xF000;

    while (z.cycles < max_cycles) {
        if (z.pc == 0x0005) {
            if (z.c == 2) {
                char ch = (char)z.e;
                putchar(ch);
                if (outlen < sizeof(out) - 1) out[outlen++] = ch;
            } else if (z.c == 9) {
                uint16_t a = (uint16_t)((z.d << 8) | z.e);
                while (mem[a] != '$') {
                    char ch = (char)mem[a++];
                    putchar(ch);
                    if (outlen < sizeof(out) - 1) out[outlen++] = ch;
                }
            }
            fflush(stdout);
        }
        if (z.pc == 0x0000) break;
        z80_step(&z);
    }
    out[outlen] = 0;
    printf("\n[%llu T-states]\n", (unsigned long long)z.cycles);

    if (strstr(out, "ERROR")) return 1;
    if (!strstr(out, "Tests complete")) return 1;
    return 0;
}
