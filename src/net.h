/*
 * adamcore - minimal nonblocking TCP helpers (POSIX)
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ADAMCORE_NET_H
#define ADAMCORE_NET_H

#include <stdint.h>

int net_listen(int port);              /* loopback listener, nonblocking */
int net_accept(int listen_fd);         /* -1 if none pending */
int net_read(int fd, void *buf, int n);   /* >0 bytes, 0 none, -1 closed */
int net_write(int fd, const void *buf, int n); /* full send; -1 on error */
void net_close(int fd);
uint64_t net_now_ms(void);

#endif
