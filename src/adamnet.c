/*
 * adamcore - high-level AdamNet master (6801 replacement)
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Implements the Z80-visible AdamNet master contract:
 *  - PCB (4 bytes at 0xFEC0 after reset): +0 command/status, +1/+2 address
 *    operand, +3 DCB count. Z80 commands 1 (sync Z80), 2 (sync master),
 *    3 (relocate PCB); the master acknowledges with 0x80 | command.
 *    (Contract confirmed by the EOS ROM itself: its sync loop writes 01
 *    and polls for 0x81 at the PCB, pointer variable at 0xFD70.)
 *  - DCBs (21 bytes each, following the PCB): +0 command/status, +1/+2
 *    buffer address, +3/+4 buffer length, +5..8 block number (LE),
 *    +9 device number, +14/15 retry, +16 add code, +17/18 max length,
 *    +19 device type, +20 node type (per ADAM TRM ch. 3 DCB equates).
 *    Z80 device commands: 1 status, 2 soft reset, 3 write, 4 read.
 *  - The master DMAs intrinsic RAM directly, regardless of the Z80's
 *    current bank switching (TRM: "DMA transfers to AdamNet can take
 *    place only in intrinsic RAM").
 *
 * Device routing: keyboard (0x01) is served locally from the host key
 * queue; every other device is forwarded over BoIP to FujiNet when the
 * link is up (M-D), else completed with the timeout status.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adamnet.h"
#include "boip.h"
#include "machine.h"

enum {
    PCB_DEFAULT = 0xFEC0,
    DCB_SIZE = 21,

    CMD_STATUS = 1,
    CMD_SOFT_RESET = 2,
    CMD_WRITE = 3,
    CMD_READ = 4,

    ST_OK = 0x80,
    ST_TIMEOUT = 0x9B
};

static uint8_t *ram(adamnet *an) { return an->mc->ram; }

static uint16_t dcb_base(adamnet *an, int i)
{
    return (uint16_t)(an->pcb_addr + 4 + i * DCB_SIZE);
}

/* ---- local keyboard device (0x01) ----------------------------------------- */

static int key_available(adamnet *an)
{
    return an->mc->keyq_r != an->mc->keyq_w;
}

static uint8_t key_pop(adamnet *an)
{
    adamcore *mc = an->mc;
    uint8_t k = mc->keyq[mc->keyq_r & (KEYQ_LEN - 1)];
    mc->keyq_r++;
    return k;
}

static void keyboard_dispatch(adamnet *an, uint16_t d, uint8_t cmd)
{
    uint8_t *m = ram(an);
    switch (cmd) {
    case CMD_STATUS:
        m[(uint16_t)(d + 17)] = 1; /* max message length = 1 */
        m[(uint16_t)(d + 18)] = 0;
        m[(uint16_t)(d + 19)] = 0; /* character device */
        m[(uint16_t)(d + 20)] = 1;
        m[(uint16_t)(d + 0)] = ST_OK;
        break;
    case CMD_SOFT_RESET:
        an->mc->keyq_r = an->mc->keyq_w;
        m[(uint16_t)(d + 0)] = ST_OK;
        break;
    case CMD_READ: {
        /* Completes only when a key is available; stays pending
         * otherwise, exactly like the real master polling the
         * keyboard node. */
        if (key_available(an)) {
            uint16_t buf =
                (uint16_t)(m[(uint16_t)(d + 1)] | (m[(uint16_t)(d + 2)] << 8));
            uint16_t len =
                (uint16_t)(m[(uint16_t)(d + 3)] | (m[(uint16_t)(d + 4)] << 8));
            uint16_t n = 0;
            if (len == 0) len = 1;
            while (n < len && key_available(an))
                m[(uint16_t)(buf + n++)] = key_pop(an);
            m[(uint16_t)(d + 0)] = ST_OK;
        }
        break;
    }
    case CMD_WRITE: /* keyboard is read-only */
        m[(uint16_t)(d + 0)] = ST_OK;
        break;
    default:
        break;
    }
}

/* ---- master scan ----------------------------------------------------------- */

void adamnet_init(adamnet *an, struct adamcore *mc)
{
    memset(an, 0, sizeof(*an));
    an->mc = mc;
    an->pcb_addr = PCB_DEFAULT;
    if (mc->cfg.boip_listen_port > 0)
        an->link = boip_create(mc->cfg.boip_listen_port);
}

void adamnet_reset(adamnet *an)
{
    an->active = 1;
    an->pcb_addr = PCB_DEFAULT;
    if (an->link)
        boip_bus_reset(an->link);
}

void adamnet_shutdown(adamnet *an)
{
    if (an->link) {
        boip_destroy(an->link);
        an->link = 0;
    }
}

void adamnet_scan(adamnet *an)
{
    uint8_t *m;
    uint8_t cmd;
    int i, ndev;

    if (!an->active) return;
    m = ram(an);

    if (an->link) {
        boip_poll(an->link);
        /* EOS's boot-device scan usually finishes before FujiNet's
         * client manages to connect; reboot once so the scan sees it. */
        if (boip_take_first_connect(an->link) &&
            an->mc->cfg.start_machine == ADAMCORE_MACHINE_ADAM)
            adamcore_request_reset(an->mc, 0);
    }

    /* PCB commands */
    cmd = m[an->pcb_addr];
    switch (cmd) {
    case 1: /* synchronize Z80 */
        m[an->pcb_addr] = 0x81;
        return;
    case 2: /* synchronize master */
        m[an->pcb_addr] = 0x82;
        return;
    case 3: { /* relocate PCB (EOS moves the whole structure itself) */
        uint16_t na = (uint16_t)(m[(uint16_t)(an->pcb_addr + 1)] |
                                 (m[(uint16_t)(an->pcb_addr + 2)] << 8));
        m[an->pcb_addr] = 0x83;
        an->pcb_addr = na;
        return;
    }
    default:
        break;
    }

    /* DCB commands. The DCB table is owned by EOS: PCB+3 holds the
     * count EOS maintains, and each DCB names its target AdamNet node
     * in the ADDRESS CODE field at +16 (this is how the EOS roll-call
     * probes addresses 1..15 through a single DCB). */
    ndev = m[(uint16_t)(an->pcb_addr + 3)];
    if (ndev > 32) ndev = 32;
    for (i = 0; i < ndev; i++) {
        uint16_t d = dcb_base(an, i);
        uint8_t dcmd = m[(uint16_t)(d + 0)];
        uint8_t dev = (uint8_t)(m[(uint16_t)(d + 16)] & 0x0F);

        if (dcmd < CMD_STATUS || dcmd > CMD_READ)
            continue;

        if (getenv("ADAMCORE_AN_TRACE")) {
            static uint32_t last_tag;
            uint32_t tag = ((uint32_t)d << 8) | (dev << 4) | dcmd;
            if (tag != last_tag) {
                fprintf(stderr, "AN: dcb=%04X dev=%X cmd=%d len=%u\n", d,
                        dev, dcmd,
                        m[(uint16_t)(d + 3)] | (m[(uint16_t)(d + 4)] << 8));
                last_tag = tag;
            }
        }

        if (dev == 0x01) {
            keyboard_dispatch(an, d, dcmd);
        } else if (an->link && boip_connected(an->link)) {
            boip_dispatch(an->link, an->mc, d, dcmd, dev);
        } else {
            m[(uint16_t)(d + 0)] = ST_TIMEOUT;
        }
    }
}
