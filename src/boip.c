/*
 * adamcore - AdamNet "Bus over IP" (stub; full implementation in M-D)
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stddef.h>

#include "boip.h"

struct boip *boip_create(int listen_port)
{
    (void)listen_port;
    return NULL;
}

void boip_destroy(struct boip *b) { (void)b; }
void boip_poll(struct boip *b) { (void)b; }
int boip_connected(struct boip *b) { (void)b; return 0; }
void boip_bus_reset(struct boip *b) { (void)b; }

void boip_dispatch(struct boip *b, struct adamcore *mc, uint16_t d,
                   uint8_t dcmd, uint8_t dev)
{
    (void)b; (void)mc; (void)d; (void)dcmd; (void)dev;
}
