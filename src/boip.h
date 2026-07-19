/*
 * adamcore - AdamNet "Bus over IP" master-side link to FujiNet
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The emulator is the AdamNet master and LISTENS on loopback TCP; the
 * FujiNet runtime (fujinet-pc ADAM target, NetAdamNet) connects in as
 * the client. Wire protocol per the author's "All About AdamNet"
 * reference and the fujinet-pc sources. One transaction outstanding at
 * a time; nonblocking; advanced from the AdamNet scan hook.
 */
#ifndef ADAMCORE_BOIP_H
#define ADAMCORE_BOIP_H

#include <stdint.h>

struct adamcore;
struct boip;

struct boip *boip_create(int listen_port);
void boip_destroy(struct boip *b);

void boip_poll(struct boip *b);      /* accept/reconnect housekeeping */
int  boip_connected(struct boip *b);
void boip_bus_reset(struct boip *b); /* broadcast wire RESET to devices */

/* Advance/start the transaction for the DCB at d (intrinsic-RAM address)
 * carrying command dcmd for AdamNet device dev. Writes the DCB status
 * byte when the transaction completes or fails. */
void boip_dispatch(struct boip *b, struct adamcore *mc, uint16_t d,
                   uint8_t dcmd, uint8_t dev);

#endif
