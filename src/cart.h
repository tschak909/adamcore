/*
 * adamcore - cartridge image loading
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ADAMCORE_CART_H
#define ADAMCORE_CART_H

#include <stdint.h>

/* Loads a cartridge image (up to 32K) into the 32K window at out[],
 * mirroring smaller images across the window the way partial board
 * decoding does. Returns image size, or -1 on error. */
int cart_load(const char *path, uint8_t out[0x8000]);

#endif
