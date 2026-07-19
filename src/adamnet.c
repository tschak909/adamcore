/*
 * adamcore - high-level AdamNet master (stub; full implementation in M-C)
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "adamnet.h"
#include "machine.h"

void adamnet_init(adamnet *an, struct adamcore *mc)
{
    an->mc = mc;
    an->pcb_addr = 0;
    an->active = 0;
    an->link = 0;
}

void adamnet_reset(adamnet *an)
{
    an->active = 1;
}

void adamnet_scan(adamnet *an)
{
    (void)an;
}

void adamnet_shutdown(adamnet *an)
{
    (void)an;
}
