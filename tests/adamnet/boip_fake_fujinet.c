/*
 * adamcore - scripted BoIP peer test
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Plays the FujiNet (node) side of the BoIP wire protocol against the
 * adamcore AdamNet master with a canned in-memory disk, by driving the
 * core's DCB interface directly (no EOS involved): posts DCB commands
 * the way EOS would and verifies the master's wire behavior:
 *   - STATUS fill-in of maxl/devtype/status and 0x80 completion
 *   - block read: SEND blocknum / single-ACK RECEIVE (incl. silent
 *     seek stall) / CLR / 1024-byte DATA with checksum
 *   - CLR retry on a corrupted first DATA (checksum mismatch)
 *   - block write: two SENDs, data committed
 *   - char read NAK -> bounded retries -> 0x8C completion
 *   - absent device -> 0x9B timeout
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

static adamcore *C;
static int peer_fd = -1;
static uint8_t disk[4][1024]; /* 4 canned blocks */
static int corrupt_next_data; /* force one bad checksum */
static int stall_receive_ms;  /* silent-seek simulation */
static int clr_discard_first; /* dev 4: swallow the first CLR, as a
                                 node long-read wait_for_idle would */
static int char9_recv_seen;   /* dev 9: swallow the first RECEIVE, as a
                                 node long-command wait_for_idle would */
static int net10_body;        /* dev 10: a one-shot protocol body that a
                                 RECEIVE probe would fetch/consume */
static int stray_before_ack;  /* dev 4: emit one unsolicited extra block just
                                 before the block-number ACK, as a node that
                                 served a re-polled CLR twice leaves behind */
static int corrupt_all_data;  /* dev 4: every DATA carries a bad checksum */

static uint8_t xck(const uint8_t *p, int n)
{
    uint8_t c = 0;
    while (n--) c ^= *p++;
    return c;
}

static void peer_send(const void *p, int n)
{
    if (send(peer_fd, p, n, MSG_NOSIGNAL) != n) {
        perror("peer send");
        exit(2);
    }
}

/* read exactly n bytes */
static void peer_read(uint8_t *p, int n)
{
    while (n > 0) {
        ssize_t r = recv(peer_fd, p, (size_t)n, 0);
        if (r <= 0) { perror("peer recv"); exit(2); }
        p += r;
        n -= (int)r;
    }
}

/* ---- node-side protocol engine (runs on its own thread) ------------------- */

static void *peer_thread(void *arg)
{
    struct sockaddr_in sa;
    uint32_t pending_block = 0;
    int have_pending = 0;
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
        peer_read(&b, 1);
        {
            uint8_t type = (uint8_t)(b >> 4);
            uint8_t dev = (uint8_t)(b & 0x0F);

            if (dev == 3) continue; /* keep-alive READY: no response */
            if (dev == 6) continue; /* "absent" device: silence */

            if (dev == 10) {
                /* Network device: RECEIVE is *not* a passive probe -- it
                 * would drive the protocol read and stage the body. Model a
                 * one-shot body: RECEIVE ACKs while it's pending, CLR delivers
                 * and consumes it. If the master purges before a write, the
                 * body is drained here and a later read finds nothing. */
                if (type == 0x6) { /* SEND: a command write; consume + ACK */
                    uint8_t hdr[2], ack = (uint8_t)(0x90 | dev);
                    uint16_t l;
                    static uint8_t pay[1100];
                    peer_read(hdr, 2);
                    l = (uint16_t)((hdr[0] << 8) | hdr[1]);
                    peer_read(pay, l + 1); /* payload + ck */
                    peer_send(&ack, 1);
                } else if (type == 0x4) { /* RECEIVE probe */
                    uint8_t r = (uint8_t)((net10_body ? 0x90 : 0xC0) | dev);
                    peer_send(&r, 1);
                } else if (type == 0x3) { /* CLR: deliver + consume once */
                    if (net10_body) {
                        uint8_t r[12];
                        r[0] = (uint8_t)(0xB0 | dev);
                        r[1] = 0x00;
                        r[2] = 0x08;
                        memset(&r[3], 0xD5, 8);
                        r[11] = xck(&r[3], 8);
                        peer_send(r, 12);
                        net10_body = 0;
                    } else {
                        uint8_t nak = (uint8_t)(0xC0 | dev);
                        peer_send(&nak, 1);
                    }
                }
                continue;
            }

            if (dev == 9) {
                /* Char device modelling a *node-side long command*: the
                 * handler blocks past ADAMNET_LONG_CMD_US and finishes with
                 * wait_for_idle()->discardInput(), which drops the RECEIVE
                 * the master already sent. We emulate that by staying silent
                 * on the first RECEIVE. The master must re-poll RECEIVE to
                 * resync; a later RECEIVE is ACKed and CLR delivers the data. */
                if (type == 0x4) { /* RECEIVE */
                    if (!char9_recv_seen) {
                        char9_recv_seen = 1; /* "discarded": no response */
                    } else {
                        uint8_t ack = (uint8_t)(0x90 | dev);
                        peer_send(&ack, 1);
                    }
                } else if (type == 0x3) { /* CLR: deliver buffered response */
                    uint8_t r[12];
                    r[0] = (uint8_t)(0xB0 | dev);
                    r[1] = 0x00;
                    r[2] = 0x08;
                    memset(&r[3], 0xC9, 8);
                    r[11] = xck(&r[3], 8);
                    peer_send(r, 12);
                }
                continue;
            }

            switch (type) {
            case 0x0: /* RESET */
                break;
            case 0x1: { /* STATUS */
                uint8_t r[6];
                r[0] = (uint8_t)(0x80 | dev);
                if (dev == 4) { r[1] = 0x00; r[2] = 0x04; r[3] = 1; r[4] = 0x40; }
                else { r[1] = 0x00; r[2] = 0x04; r[3] = 0; r[4] = 0x00; }
                r[5] = xck(&r[1], 4);
                peer_send(r, 6);
                break;
            }
            case 0x6: { /* SEND */
                uint8_t hdr[2], ack = (uint8_t)(0x90 | dev);
                uint16_t len;
                static uint8_t pay[1100];
                peer_read(hdr, 2);
                len = (uint16_t)((hdr[0] << 8) | hdr[1]);
                peer_read(pay, len + 1); /* payload + ck */
                if (dev == 4 && len == 5) {
                    pending_block = (uint32_t)(pay[0] | (pay[1] << 8) |
                                               ((uint32_t)pay[2] << 16) |
                                               ((uint32_t)pay[3] << 24));
                    have_pending = 1;
                    if (stray_before_ack) {
                        /* An unsolicited, well-formed block landing while the
                         * master waits for this ACK: what a node that answered
                         * a re-polled CLR twice leaves on the wire. Carries a
                         * *different* block so a master that resyncs into the
                         * payload delivers detectably wrong data. */
                        static uint8_t s[1028];
                        stray_before_ack = 0;
                        s[0] = (uint8_t)(0xB0 | dev);
                        s[1] = 0x04;
                        s[2] = 0x00;
                        memcpy(&s[3], disk[(pending_block + 1) & 3], 1024);
                        s[1027] = xck(&s[3], 1024);
                        peer_send(s, 1028);
                    }
                } else if (dev == 4 && len == 1024 && have_pending) {
                    memcpy(disk[pending_block & 3], pay, 1024);
                    have_pending = 0;
                }
                peer_send(&ack, 1);
                break;
            }
            case 0x4: { /* RECEIVE */
                if (dev == 4) {
                    uint8_t ack = (uint8_t)(0x90 | dev);
                    if (stall_receive_ms > 0) {
                        /* stay silent while "seeking": master re-polls */
                        usleep((useconds_t)stall_receive_ms * 1000);
                        stall_receive_ms = 0;
                        /* swallow the re-polls that queued up meanwhile:
                           handled implicitly; only one ACK is sent */
                    }
                    peer_send(&ack, 1);
                } else {
                    uint8_t nak = (uint8_t)(0xC0 | dev); /* char: no data */
                    peer_send(&nak, 1);
                }
                break;
            }
            case 0x3: { /* CLR */
                if (dev == 4) {
                    static uint8_t r[1028];
                    if (clr_discard_first) {
                        /* a node long-read ends in wait_for_idle()/
                         * discardInput() that eats this CLR: answer nothing,
                         * so the master must re-poll CLR to get the block */
                        clr_discard_first = 0;
                        break;
                    }
                    r[0] = (uint8_t)(0xB0 | dev);
                    r[1] = 0x04;
                    r[2] = 0x00;
                    memcpy(&r[3], disk[pending_block & 3], 1024);
                    r[1027] = xck(&r[3], 1024);
                    if (corrupt_next_data) {
                        r[1027] ^= 0xFF;
                        corrupt_next_data = 0;
                    }
                    if (corrupt_all_data)
                        r[1027] ^= 0xFF;
                    peer_send(r, 1028);
                } else {
                    uint8_t nak = (uint8_t)(0xC0 | dev);
                    peer_send(&nak, 1);
                }
                break;
            }
            case 0xD: { /* READY */
                uint8_t ack = (uint8_t)(0x90 | dev);
                peer_send(&ack, 1);
                break;
            }
            default:
                break;
            }
        }
    }
    return NULL;
}

/* ---- master-side driver ---------------------------------------------------- */

static void run_ms(int ms)
{
    /* one frame is ~16.7ms of emulated time; the master's wall-clock
     * pacing runs off real time, so just run frames for that span */
    uint64_t end = net_now_ms() + (uint64_t)ms;
    while (net_now_ms() < end)
        adamcore_run_frame(C);
}

static uint16_t dcb0;

static void post_dcb(uint8_t dev, uint8_t cmd, uint16_t buf, uint16_t len,
                     uint32_t blk)
{
    uint8_t *m = C->ram;
    memset(&m[dcb0], 0, 21);
    m[dcb0 + 1] = (uint8_t)buf;
    m[dcb0 + 2] = (uint8_t)(buf >> 8);
    m[dcb0 + 3] = (uint8_t)len;
    m[dcb0 + 4] = (uint8_t)(len >> 8);
    m[dcb0 + 5] = (uint8_t)blk;
    m[dcb0 + 6] = (uint8_t)(blk >> 8);
    m[dcb0 + 7] = (uint8_t)(blk >> 16);
    m[dcb0 + 8] = (uint8_t)(blk >> 24);
    m[dcb0 + 16] = dev;
    m[dcb0 + 0] = cmd;
}

static uint8_t wait_done(int max_ms)
{
    uint64_t end = net_now_ms() + (uint64_t)max_ms;
    while (net_now_ms() < end) {
        adamcore_run_frame(C);
        if (C->ram[dcb0] & 0x80)
            return C->ram[dcb0];
    }
    return C->ram[dcb0];
}

/* A block read takes two DCB posts: the drive reports busy (0x9B) while it
 * positions on the block, and the guest posts the DCB again to collect the
 * data. EOS drives every block read that way, so the tests do too. Returns
 * the status of the second post (or the first, if it was not a busy). */
static uint8_t block_read(uint8_t dev, uint16_t buf, uint16_t len,
                          uint32_t blk, int max_ms)
{
    uint8_t st;
    post_dcb(dev, 4, buf, len, blk);
    st = wait_done(max_ms);
    if (st != 0x9B)
        return st;
    post_dcb(dev, 4, buf, len, blk);
    return wait_done(max_ms);
}

static int failures;

static void check(const char *name, int ok)
{
    printf("%-40s %s\n", name, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

int main(void)
{
    adamcore_config cfg;
    pthread_t th;
    uint8_t *m;
    int i, j;

    /* Canned disk content. Deliberately varied rather than a constant fill:
     * a stray block only misleads a byte-at-a-time resync if its payload
     * contains bytes that look like response headers (0x94 ACK / 0xB4 DATA /
     * 0xC4 NAK for device 4), which real disk data does and 0xA0 fill does
     * not. Every block below contains all three. */
    for (i = 0; i < 4; i++)
        for (j = 0; j < 1024; j++)
            disk[i][j] = (uint8_t)((i * 7919 + j * 31 + (j >> 3) * 17) ^
                                   (j * j));

    memset(&cfg, 0, sizeof(cfg));
    cfg.start_machine = ADAMCORE_MACHINE_CV; /* no ROLL call needed */
    cfg.boip_listen_port = PORT;
    cfg.audio_rate = 44100;
    /* OS7 required by create: use a scratch 8K zero file */
    {
        FILE *f = fopen("/tmp/adamcore-fake-os7.rom", "wb");
        static uint8_t z[0x2000];
        fwrite(z, 1, sizeof(z), f);
        fclose(f);
    }
    cfg.os7_rom_path = "/tmp/adamcore-fake-os7.rom";

    C = adamcore_create(&cfg);
    if (!C) { fprintf(stderr, "create failed\n"); return 2; }
    m = C->ram;

    /* enable the master without EOS: PCB at the default location with a
     * one-entry DCB table */
    C->an.active = 1;
    m[PCB] = 0;
    m[PCB + 3] = 1;
    dcb0 = PCB + 4;

    pthread_create(&th, NULL, peer_thread, NULL);
    run_ms(300); /* let the peer connect */

    /* 1. STATUS of block dev 4 */
    post_dcb(4, 1, 0, 0, 0);
    check("status completes 0x80", wait_done(1000) == 0x80);
    check("status maxl 1024", m[dcb0 + 17] == 0x00 && m[dcb0 + 18] == 0x04);
    check("status devtype block", m[dcb0 + 19] == 1);

    /* 2. block read. The drive answers the first request for a block it is
     *    not positioned on with 0x9B and serves the data on the repeat; a
     *    loader that counts on that exchange (Donkey Kong Super Game) takes
     *    a different path if the first request is answered outright. */
    post_dcb(4, 4, 0x4000, 1024, 2);
    check("first read of a new block reports busy", wait_done(2000) == 0x9B);
    post_dcb(4, 4, 0x4000, 1024, 2);
    check("repeat read completes 0x80", wait_done(2000) == 0x80);
    check("repeat read data", memcmp(&m[0x4000], disk[2], 1024) == 0);

    /* the same block again is already under the head: served at once */
    memset(&m[0x4000], 0, 1024);
    post_dcb(4, 4, 0x4000, 1024, 2);
    check("re-read of the staged block completes 0x80",
          wait_done(2000) == 0x80);
    check("block read completes 0x80",
          block_read(4, 0x4000, 1024, 2, 2000) == 0x80);
    check("block read data", memcmp(&m[0x4000], disk[2], 1024) == 0);

    /* 3. block read with silent seek stall */
    stall_receive_ms = 120;
    check("stalled read completes 0x80",
          block_read(4, 0x4000, 1024, 1, 3000) == 0x80);
    check("stalled read data", memcmp(&m[0x4000], disk[1], 1024) == 0);

    /* 3b. block read whose CLR the node discarded (long TNFS read ->
     *     wait_for_idle): the master must re-poll CLR and still complete
     *     promptly. Without the B_BRD_DATA re-poll it stalls to TIMEOUT_MS
     *     and the read fails/returns late -- the DKJr-over-TNFS corruption. */
    clr_discard_first = 1;
    check("clr-discard read completes 0x80",
          block_read(4, 0x4000, 1024, 0, 1500) == 0x80);
    check("clr-discard read data", memcmp(&m[0x4000], disk[0], 1024) == 0);

    /* 4. CLR retry on corrupted checksum */
    corrupt_next_data = 1;
    check("ck-retry read completes 0x80",
          block_read(4, 0x4000, 1024, 3, 3000) == 0x80);
    check("ck-retry read data", memcmp(&m[0x4000], disk[3], 1024) == 0);

    /* 5. block write round-trip */
    memset(&m[0x4400], 0x5A, 1024);
    post_dcb(4, 3, 0x4400, 1024, 2);
    check("block write completes 0x80", wait_done(2000) == 0x80);
    run_ms(50);
    check("block write committed", disk[2][0] == 0x5A && disk[2][1023] == 0x5A);

    /* 6. char read with no data -> bounded retries -> 0x8C */
    post_dcb(15, 4, 0x4000, 1024, 0);
    check("char read NAK completes 0x8C", wait_done(6000) == 0x8C);

    /* 7. absent device -> timeout 0x9B */
    post_dcb(6, 1, 0, 0, 0);
    check("absent device status 0x9B", wait_done(2000) == 0x9B);

    /* 8. char read whose first RECEIVE the node discarded (long command):
     *    the master must re-poll RECEIVE and still complete promptly. Without
     *    the char re-poll it stalls until TIMEOUT_MS (5 s), so a 500 ms budget
     *    both proves the fix and fails loudly on a regression. */
    char9_recv_seen = 0;
    post_dcb(9, 4, 0x4800, 16, 0);
    check("discarded char read completes 0x80", wait_done(500) == 0x80);
    check("discarded char read data",
          m[0x4800] == 0xC9 && m[0x4800 + 7] == 0xC9);

    /* 9. A char WRITE to a network device must NOT purge (drain) a pending
     *    protocol response: on a network device RECEIVE drives the read, so a
     *    purge fetches and discards the body. Stage a one-shot body, write a
     *    command, then read -- the read must still get the body. Without the
     *    net-device purge skip the write consumes it and the read is empty. */
    net10_body = 1;
    m[0x4900] = 0x11;
    m[0x4901] = 0x22; /* a 2-byte command payload */
    post_dcb(10, 3, 0x4900, 2, 0);
    check("net write completes 0x80", wait_done(1000) == 0x80);
    post_dcb(10, 4, 0x4A00, 16, 0);
    check("net read after write keeps body 0x80", wait_done(1000) == 0x80);
    check("net read body intact",
          m[0x4A00] == 0xD5 && m[0x4A00 + 7] == 0xD5);

    /* 10. A stray but perfectly well-formed block arriving mid-transaction
     *     must be discarded as ONE packet. Scanning it byte by byte instead
     *     finds a byte matching the header the master is waiting for almost
     *     every time (1024 payload bytes, ~99.97% for one type+device), so the
     *     master resyncs into the middle of the payload and hands the guest
     *     whatever follows -- silent read corruption. */
    stray_before_ack = 1;
    {
        uint64_t t0;
        uint8_t st;
        /* position first, so the timing below measures only the exchange
         * that meets the stray, not the drive's busy round trip */
        post_dcb(4, 4, 0x4000, 1024, 1);
        wait_done(2000);
        t0 = net_now_ms();
        post_dcb(4, 4, 0x4000, 1024, 1);
        st = wait_done(2000);
        uint64_t dt = net_now_ms() - t0;
        check("stray-block read completes 0x80", st == 0x80);
        check("stray-block read data", memcmp(&m[0x4000], disk[1], 1024) == 0);
        /* Skipping the stray whole costs one poll; rescanning it byte by byte
         * costs one poll per byte, which is ~150 ms for a 1028-byte block and
         * is the observable signature of the unsafe resync. */
        check("stray block skipped as one packet", dt < 120);
        if (dt >= 120)
            printf("    (stray read took %llu ms)\n", (unsigned long long)dt);
    }
    check("read after stray completes 0x80",
          block_read(4, 0x4000, 1024, 3, 2000) == 0x80);
    check("read after stray data", memcmp(&m[0x4000], disk[3], 1024) == 0);

    /* 11. A DATA whose checksum never verifies must FAIL the DCB rather than
     *     be delivered as success. EOS retries a block read that reports an
     *     error; it cannot tell that an ST_OK one is garbage, so accepting a
     *     bad payload is exactly how a link hiccup becomes corrupt graphics. */
    corrupt_all_data = 1;
    memset(&m[0x4000], 0xEE, 1024);
    {
        uint8_t st = block_read(4, 0x4000, 1024, 1, 3000);
        check("bad-checksum read does not report success", st != 0x80);
        check("bad-checksum read leaves the buffer untouched",
              m[0x4000] == 0xEE && m[0x4000 + 1023] == 0xEE);
    }
    corrupt_all_data = 0;
    check("read after checksum failure recovers",
          block_read(4, 0x4000, 1024, 3, 2000) == 0x80);
    check("read after checksum failure data",
          memcmp(&m[0x4000], disk[3], 1024) == 0);

    printf("boip_fake_fujinet: %s\n", failures ? "FAILURES" : "all ok");
    adamcore_destroy(C);
    return failures ? 1 : 0;
}
