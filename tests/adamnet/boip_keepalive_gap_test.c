/*
 * adamcore - BoIP keep-alive turnaround test
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * FujiNet flushes its input FIFO after every packet it handles, the idle
 * keep-alive READY (0xD3) included, so a command sent right behind one is
 * eaten. A real EOS boot lost its drive-1 STATUS that way and fell through
 * to SmartWriter. Posts STATUS after STATUS to address 4 for ~1.5 s (several
 * keep-alives) and checks that none reaches the node within the turnaround
 * gap of the keep-alive before it.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include "machine.h"
#include "net.h"

#define PORT 65299
#define PCB 0xFEC0
#define GAP_MS 4

static adamcore *C;
static int peer_fd = -1;
static volatile uint64_t last_ka;
static volatile int min_gap = 1000000;
static volatile int statuses;

static void *peer_thread(void *arg)
{
    struct sockaddr_in sa;
    (void)arg;

    peer_fd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(PORT);
    while (connect(peer_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        usleep(20000);
    {
        int one = 1;
        setsockopt(peer_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    for (;;) {
        uint8_t b;
        if (recv(peer_fd, &b, 1, 0) <= 0)
            break;
        if (b == 0xD3) {
            last_ka = net_now_ms();
        } else if (b == 0x14) {
            uint8_t r[6] = { 0x84, 0x00, 0x04, 0x01, 0x40, 0x45 };
            int gap = last_ka ? (int)(net_now_ms() - last_ka) : 1000000;
            if (gap < min_gap)
                min_gap = gap;
            statuses++;
            send(peer_fd, r, sizeof(r), MSG_NOSIGNAL);
        }
    }
    return NULL;
}

int main(void)
{
    adamcore_config cfg;
    pthread_t th;
    uint8_t *m;
    uint16_t dcb0 = PCB + 4;
    uint64_t end;
    int ok;

    memset(&cfg, 0, sizeof(cfg));
    cfg.start_machine = ADAMCORE_MACHINE_CV;
    cfg.boip_listen_port = PORT;
    cfg.audio_rate = 44100;
    {
        static uint8_t z[0x2000];
        cfg.os7_rom_data = z;
    }

    C = adamcore_create(&cfg);
    if (!C) { fprintf(stderr, "create failed\n"); return 2; }
    m = C->ram;
    C->an.active = 1;
    m[PCB] = 0;
    m[PCB + 3] = 1;

    pthread_create(&th, NULL, peer_thread, NULL);
    end = net_now_ms() + 2000;
    while (!adamcore_boip_connected(C) && net_now_ms() < end)
        adamcore_run_frame(C);

    end = net_now_ms() + 1500;
    while (net_now_ms() < end) {
        if (m[dcb0] & 0x80 || m[dcb0] == 0) {
            memset(&m[dcb0], 0, 21);
            m[dcb0 + 16] = 4;
            m[dcb0] = 1;
        }
        adamcore_run_frame(C);
    }

    ok = statuses > 20 && min_gap >= GAP_MS - 1;
    printf("statuses %d, closest to a keep-alive %d ms: %s\n", statuses,
           min_gap, ok ? "ok" : "FAIL");
    adamcore_destroy(C);
    return ok ? 0 : 1;
}
