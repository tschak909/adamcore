/*
 * adamcore - cartridge image loading
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "cart.h"

#include <stdio.h>
#include <string.h>

int cart_load(const char *path, uint8_t out[0x8000])
{
    FILE *fp = fopen(path, "rb");
    size_t n;
    if (!fp) return -1;
    memset(out, 0xFF, 0x8000);
    n = fread(out, 1, 0x8000, fp);
    fclose(fp);
    if (n == 0) return -1;

    /* Mirror sub-32K images across the window (round size up to a power
     * of two first so odd dumps still land sensibly). */
    if (n < 0x8000) {
        size_t span = 1;
        while (span < n) span <<= 1;
        if (span > n)
            memset(out + n, 0xFF, span - n);
        {
            size_t at = span;
            while (at + span <= 0x8000) {
                memcpy(out + at, out, span);
                at += span;
            }
            if (at < 0x8000)
                memcpy(out + at, out, 0x8000 - at);
        }
    }
    return (int)n;
}
