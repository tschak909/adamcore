/*
 * adamcore - minimal nonblocking TCP helpers (Win32 / Winsock2)
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "net.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <string.h>

/* net_listen() is always the first net_* call a session makes (see
 * boip_init()), so lazily starting Winsock here covers every caller. */
static void ensure_wsa_started(void)
{
    static int started = 0;
    if (!started) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        started = 1;
    }
}

static void set_nonblock(SOCKET s)
{
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
}

int net_listen(int port)
{
    struct sockaddr_in sa;
    SOCKET s;
    BOOL one = TRUE;

    ensure_wsa_started();

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)port);
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) == SOCKET_ERROR ||
        listen(s, 1) == SOCKET_ERROR) {
        closesocket(s);
        return -1;
    }
    set_nonblock(s);
    return (int)s;
}

int net_accept(int listen_fd)
{
    SOCKET s = accept((SOCKET)listen_fd, NULL, NULL);
    BOOL one = TRUE;
    if (s == INVALID_SOCKET) return -1;
    set_nonblock(s);
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    return (int)s;
}

int net_read(int fd, void *buf, int n)
{
    int r = recv((SOCKET)fd, (char *)buf, n, 0);
    if (r > 0) return r;
    if (r == 0) return -1; /* peer closed */
    return (WSAGetLastError() == WSAEWOULDBLOCK) ? 0 : -1;
}

/* select() rather than a polled sleep -- see the POSIX twin for why.
 * Winsock ignores the nfds argument but still wants a sane value. */
int net_wait_readable(int fd, int timeout_ms)
{
    fd_set rfds;
    struct timeval tv, *ptv = NULL;
    int r;

    if (fd < 0) return -1;
    FD_ZERO(&rfds);
    FD_SET((SOCKET)fd, &rfds);
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }
    r = select(fd + 1, &rfds, NULL, NULL, ptv);
    if (r == SOCKET_ERROR) return -1;
    return r > 0 ? 1 : 0;
}

void net_sleep_ms(int ms)
{
    if (ms > 0) Sleep((DWORD)ms);
}

int net_write(int fd, const void *buf, int n)
{
    /* Loopback with TCP_NODELAY: send the packet in one call so the peer
     * never observes an inter-byte gap. Brief WSAEWOULDBLOCK retries keep
     * the write whole if the socket buffer is momentarily full. */
    const char *p = (const char *)buf;
    int left = n;
    while (left > 0) {
        int w = send((SOCKET)fd, p, left, 0);
        if (w > 0) { p += w; left -= w; continue; }
        if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
        return -1;
    }
    return n;
}

void net_close(int fd)
{
    if (fd >= 0) closesocket((SOCKET)fd);
}

uint64_t net_now_ms(void)
{
    return (uint64_t)GetTickCount64();
}
