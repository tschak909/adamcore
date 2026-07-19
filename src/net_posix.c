/*
 * adamcore - minimal nonblocking TCP helpers (POSIX)
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int net_listen(int port)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    set_nonblock(fd);
    return fd;
}

int net_accept(int listen_fd)
{
    int fd = accept(listen_fd, NULL, NULL);
    int one = 1;
    if (fd < 0) return -1;
    set_nonblock(fd);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

int net_read(int fd, void *buf, int n)
{
    ssize_t r = recv(fd, buf, (size_t)n, 0);
    if (r > 0) return (int)r;
    if (r == 0) return -1; /* peer closed */
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
}

int net_write(int fd, const void *buf, int n)
{
    /* Loopback with TCP_NODELAY: send the packet in one call so the peer
     * never observes an inter-byte gap. Brief EAGAIN retries keep the
     * write whole if the socket buffer is momentarily full. */
    const char *p = buf;
    int left = n;
    while (left > 0) {
        ssize_t w = send(fd, p, (size_t)left, MSG_NOSIGNAL);
        if (w > 0) { p += w; left -= (int)w; continue; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
        return -1;
    }
    return n;
}

void net_close(int fd)
{
    if (fd >= 0) close(fd);
}

uint64_t net_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}
