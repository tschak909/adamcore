/*
 * adamcore - TMS9928A color palettes
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ADAMCORE_PALETTE_H
#define ADAMCORE_PALETTE_H

#include <stdint.h>

/* which: 0 datasheet NTSC, 1 grayscale, 2 saturated, 3 green phosphor */
void palette_build(int which, uint16_t out[16]);

#endif
