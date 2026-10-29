/*
 * adamcore - net_listen address handling: loopback by default, an IPv4
 * literal when given, -1 for anything else.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <unistd.h>

#include "net.h"

static int failures;

static void check(const char *name, int ok)
{
    printf("%-44s %s\n", name, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

int main(void)
{
    int fd;

    fd = net_listen(NULL, 65391);
    check("NULL binds loopback", fd >= 0);
    if (fd >= 0) close(fd);
    fd = net_listen("", 65391);
    check("empty binds loopback", fd >= 0);
    if (fd >= 0) close(fd);
    fd = net_listen("localhost", 65391);
    check("localhost binds loopback", fd >= 0);
    if (fd >= 0) close(fd);
    fd = net_listen("0.0.0.0", 65391);
    check("0.0.0.0 binds every interface", fd >= 0);
    if (fd >= 0) close(fd);
    fd = net_listen("127.0.0.1", 65391);
    check("127.0.0.1 binds", fd >= 0);
    if (fd >= 0) close(fd);
    check("garbage address refused", net_listen("not-an-address", 65391) < 0);
    check("a hostname is refused", net_listen("example.com", 65391) < 0);
    check("an address not on this host refused",
          net_listen("203.0.113.9", 65391) < 0);
    return failures ? 1 : 0;
}
