/*
 * adamcore - BoIP wire trace test
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pins adamcore_set_boip_trace and adamcore_boip_connected against a scripted
 * node, on the boip_fake_fujinet pattern (DCBs posted directly, no EOS):
 *   - not connected before the node connects, connected after
 *   - the idle keepalive (READY to address 3) is traced as sent
 *   - a STATUS round trip: the request arrives as one whole dir-1 packet, and
 *     the node's six reply bytes as dir 0, in however many reads they took
 *   - a bus reset traces one RESET per address it goes to
 *   - fn = NULL stops the trace
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

#define PORT 65298
#define PCB 0xFEC0

static adamcore *C;
static int peer_fd = -1;

static uint8_t xck(const uint8_t *p, int n)
{
    uint8_t c = 0;
    while (n--) c ^= *p++;
    return c;
}

/* ---- the node: answers STATUS for address 4, silent otherwise -------------- */

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
        if (b == 0x14) { /* STATUS, address 4 */
            uint8_t r[6] = { 0x84, 0x00, 0x04, 0x01, 0x40, 0 };
            r[5] = xck(&r[1], 4);
            send(peer_fd, r, sizeof(r), MSG_NOSIGNAL);
        }
    }
    return NULL;
}

/* ---- the trace sink ---------------------------------------------------------- */

#define MAX_TX 64

static int sink_tag;
static int bad_ud;
static int tx_n;
static uint8_t tx_pkt[MAX_TX][16];
static int tx_len[MAX_TX];
static uint8_t rx_buf[256];
static int rx_n;
static int events;

static void sink(void *ud, int dir, const uint8_t *buf, int len)
{
    if (ud != &sink_tag)
        bad_ud = 1;
    events++;
    if (dir == 1) {
        if (tx_n < MAX_TX) {
            memcpy(tx_pkt[tx_n], buf, len < 16 ? (size_t)len : 16);
            tx_len[tx_n++] = len;
        }
    } else {
        int k;
        for (k = 0; k < len && rx_n < (int)sizeof(rx_buf); k++)
            rx_buf[rx_n++] = buf[k];
    }
}

static void sink_clear(void)
{
    tx_n = rx_n = events = 0;
}

/* ---- master-side driver -------------------------------------------------------- */

static void run_ms(int ms)
{
    uint64_t end = net_now_ms() + (uint64_t)ms;
    while (net_now_ms() < end)
        adamcore_run_frame(C);
}

static uint8_t wait_done(uint16_t d, int max_ms)
{
    uint64_t end = net_now_ms() + (uint64_t)max_ms;
    while (net_now_ms() < end) {
        adamcore_run_frame(C);
        if (C->ram[d] & 0x80)
            return C->ram[d];
    }
    return C->ram[d];
}

static int failures;

static void check(const char *name, int ok)
{
    printf("%-44s %s\n", name, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

int main(void)
{
    adamcore_config cfg;
    pthread_t th;
    uint8_t *m;
    uint16_t dcb0 = PCB + 4;
    uint64_t end;
    int i;

    memset(&cfg, 0, sizeof(cfg));
    cfg.start_machine = ADAMCORE_MACHINE_CV; /* no roll-call */
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

    adamcore_set_boip_trace(C, sink, &sink_tag);
    check("not connected before the node", adamcore_boip_connected(C) == 0);

    pthread_create(&th, NULL, peer_thread, NULL);
    end = net_now_ms() + 2000;
    while (!adamcore_boip_connected(C) && net_now_ms() < end)
        adamcore_run_frame(C);
    check("connected once the node is", adamcore_boip_connected(C) == 1);

    /* 1. idle: the keepalive READY to address 3, every 300 ms */
    run_ms(400);
    {
        int seen = 0;
        for (i = 0; i < tx_n; i++)
            if (tx_len[i] == 1 && tx_pkt[i][0] == 0xD3) seen = 1;
        check("keepalive READY traced", seen);
    }

    /* 2. a STATUS round trip */
    sink_clear();
    memset(&m[dcb0], 0, 21);
    m[dcb0 + 16] = 4;
    m[dcb0] = 1;
    check("status completes 0x80", wait_done(dcb0, 1000) == 0x80);
    {
        int req = 0;
        for (i = 0; i < tx_n; i++)
            if (tx_pkt[i][0] == 0x14) req += tx_len[i] == 1 ? 1 : 100;
        check("STATUS request traced once, whole", req == 1);
    }
    check("STATUS reply traced", rx_n == 6 && rx_buf[0] == 0x84 &&
                                 rx_buf[2] == 0x04 && rx_buf[4] == 0x40 &&
                                 rx_buf[5] == xck(&rx_buf[1], 4));

    /* 3. a bus reset: one RESET per address */
    sink_clear();
    adamcore_request_reset(C, 1);
    adamcore_run_frame(C);
    adamcore_run_frame(C);
    {
        int resets = 0, ok = 1;
        for (i = 0; i < tx_n; i++) {
            if (tx_pkt[i][0] == 0xD3) continue;
            if (tx_len[i] != 1 || (tx_pkt[i][0] >> 4) != 0) ok = 0;
            resets++;
        }
        check("bus reset traced as single RESETs", ok && resets == 13);
    }

    /* 4. removed: nothing more arrives */
    adamcore_set_boip_trace(C, NULL, NULL);
    sink_clear();
    run_ms(400);
    check("no events after removal", events == 0);
    check("trace handed its ud back", !bad_ud);

    printf("boip_trace_test: %s\n", failures ? "FAILURES" : "all ok");
    adamcore_destroy(C);
    return failures ? 1 : 0;
}
