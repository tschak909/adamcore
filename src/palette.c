/*
 * adamcore - TMS9928A color palettes
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Palette 0 is derived at runtime from the TMS9918A/9928A datasheet color
 * table (luminance Y and color-difference R-Y / B-Y levels, 0.47 = zero
 * excursion) using the standard NTSC relations:
 *   R = Y + (R-Y) - 0.47,  B = Y + (B-Y) - 0.47,
 *   G = (Y - 0.30*R - 0.11*B) / 0.59
 * No emulator's RGB tables were consulted or copied.
 *
 * Palettes 1-3 are original adamcore variants: grayscale (Y only),
 * saturation-boosted, and green-phosphor monochrome.
 */

#include <stdint.h>

#include "palette.h"

/* Datasheet (Y, R-Y, B-Y) per color 1..15; index 0 (transparent) is
 * rendered as black by the VDP layer before reaching the palette. */
static const float tms_yuv[15][3] = {
    { 0.00f, 0.47f, 0.47f }, /* 1 black */
    { 0.53f, 0.07f, 0.20f }, /* 2 medium green */
    { 0.67f, 0.17f, 0.27f }, /* 3 light green */
    { 0.40f, 0.40f, 1.00f }, /* 4 dark blue */
    { 0.53f, 0.43f, 0.93f }, /* 5 light blue */
    { 0.47f, 0.83f, 0.30f }, /* 6 dark red */
    { 0.73f, 0.00f, 0.70f }, /* 7 cyan */
    { 0.53f, 0.93f, 0.27f }, /* 8 medium red */
    { 0.67f, 0.93f, 0.27f }, /* 9 light red */
    { 0.73f, 0.57f, 0.07f }, /* A dark yellow */
    { 0.80f, 0.57f, 0.17f }, /* B light yellow */
    { 0.47f, 0.13f, 0.23f }, /* C dark green */
    { 0.53f, 0.73f, 0.67f }, /* D magenta */
    { 0.80f, 0.47f, 0.47f }, /* E gray */
    { 1.00f, 0.47f, 0.47f }, /* F white */
};

static float clampf(float v)
{
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

static uint16_t pack565(float r, float g, float b)
{
    unsigned ri = (unsigned)(clampf(r) * 31.0f + 0.5f);
    unsigned gi = (unsigned)(clampf(g) * 63.0f + 0.5f);
    unsigned bi = (unsigned)(clampf(b) * 31.0f + 0.5f);
    return (uint16_t)((ri << 11) | (gi << 5) | bi);
}

void palette_build(int which, uint16_t out[16])
{
    int i;
    out[0] = 0; /* transparent: never emitted by the renderer */
    for (i = 0; i < 15; i++) {
        float y = tms_yuv[i][0];
        float ry = tms_yuv[i][1] - 0.47f;
        float by = tms_yuv[i][2] - 0.47f;
        float r = y + ry;
        float b = y + by;
        float g = (y - 0.30f * r - 0.11f * b) / 0.59f;

        switch (which) {
        case 1: /* grayscale */
            r = g = b = y;
            break;
        case 2: /* saturation boost */
            r = y + ry * 1.30f;
            b = y + by * 1.30f;
            g = (y - 0.30f * r - 0.11f * b) / 0.59f;
            break;
        case 3: /* green phosphor */
            r = b = 0.0f;
            g = y;
            break;
        default:
            break;
        }
        out[i + 1] = pack565(r, g, b);
    }
}
