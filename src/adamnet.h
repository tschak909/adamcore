/*
 * adamcore - high-level AdamNet master (PCB/DCB) with BoIP forwarding
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * References: Coleco ADAM Technical Reference Manual ch. 3 (AdamNet,
 * DCB/PCB structures, port 3FH semantics) and the author's own
 * "All About AdamNet" protocol reference / fujinet-pc ADAM sources
 * for the wire behavior forwarded over BoIP.
 */
#ifndef ADAMCORE_ADAMNET_H
#define ADAMCORE_ADAMNET_H

#include <stdint.h>

struct adamcore;

typedef struct adamnet {
    struct adamcore *mc;
    uint16_t pcb_addr;
    int active; /* master running (post net-reset) */

    /* local keyboard device buffer is the core key queue in adamcore */

    /* BoIP link state lives in boip.c */
    struct boip *link;
} adamnet;

void adamnet_init(adamnet *an, struct adamcore *mc);
void adamnet_reset(adamnet *an);        /* port 3F bit0 hardware reset */
void adamnet_scan(adamnet *an);         /* called once per scanline */
void adamnet_shutdown(adamnet *an);

#endif
