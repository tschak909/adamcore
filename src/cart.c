/*
 * adamcore - cartridge image loading
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "cart.h"

#include <stdio.h>
#include <string.h>

/* Mirror an image already sitting at the front of out[] across the window.
 *
 * Note this is a different model from the one a cartridge *mapper* uses,
 * where an 8K image only wires /8000 and everything above really is open
 * bus. Both are defensible; this one has been adamcore's behaviour from the
 * start and existing hosts' cartridges depend on it, so it stays. A host that
 * wants exact per-mapper decoding installs an adamcore_cart_ops device, which
 * bypasses this path entirely. Do not "fix" one to match the other. */
static void cart_mirror(uint8_t out[0x8000], size_t n)
{
    {
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
}

/* Place an in-memory image into the 32K window. */
int cart_fill(const uint8_t *image, uint32_t size, uint8_t out[0x8000])
{
    if (size > 0x8000) return -1;
    memset(out, 0xFF, 0x8000);
    if (!image || size == 0) return 0;
    memcpy(out, image, size);
    if (size < 0x8000)
        cart_mirror(out, size);
    return (int)size;
}

int cart_load(const char *path, uint8_t out[0x8000])
{
    FILE *fp = fopen(path, "rb");
    size_t n;
    if (!fp) return -1;
    memset(out, 0xFF, 0x8000);
    n = fread(out, 1, 0x8000, fp);
    fclose(fp);
    if (n == 0) return -1;
    if (n < 0x8000)
        cart_mirror(out, n);
    return (int)n;
}
