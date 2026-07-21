/*
 * adamcore - AdamNet "Bus over IP" master-side link to FujiNet
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Wire protocol (from the author's "All About AdamNet" reference and the
 * author's fujinet-pc ADAM sources): every packet starts with
 * (type << 4) | device. Master->node: 0 RESET, 1 STATUS, 3 CLR, 4 RECEIVE,
 * 6 SEND (+ big-endian u16 length, payload, XOR checksum), D READY.
 * Node->master: 8 STATUS (size_lo size_hi devtype status ck), 9 ACK,
 * B DATA (len_be16, payload, ck), C NAK.
 *
 * Rules honored here: exactly one outstanding transaction; each command
 * sent as one atomic write; ~2 ms re-poll on silence (seek stalls answer
 * with silence, and a block RECEIVE is acknowledged exactly once); the
 * DCB status byte is written only when the transaction resolves.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "boip.h"
#include "machine.h"
#include "net.h"

/* Generous: on Android the whole process can stall for hundreds of ms
 * (scheduling, app start); the guest never perceives those stalls, so
 * giving up too early makes devices "vanish" from its point of view. */
#define CRD_GIVEUP_MS 3000

enum {
    B_IDLE = 0,
    B_STATUS_WAIT,
    B_BRD_BLKNUM_ACK,
    B_BRD_RECEIVE,
    B_BRD_DATA,
    B_BWR_BLKNUM_ACK,
    B_BWR_DATA_ACK,
    B_CRD_RECEIVE,
    B_CRD_DATA,
    B_CWR_ACK,
    /* purge of a stale unconsumed device response before a new command:
     * an abandoned read (guest gave up / host stalled) leaves the node's
     * prepared response pending; sending the next command without
     * consuming it desynchronizes every following read (stale data). */
    B_CWR_PURGE_RX,
    B_CWR_PURGE_DATA,
    B_CWR_GAP /* post-purge turnaround wait before the actual SEND */
};

enum {
    ST_OK = 0x80,
    ST_NAK = 0x8C, /* device NAKed: no data (CONFIG's fuji_error checks this) */
    ST_TIMEOUT = 0x9B,

    REPOLL_MS = 2,
    /* The block-read CLR re-poll needs a far longer cadence than the RECEIVE
     * re-poll. RECEIVE re-polls are idempotent (surplus ones are answered with
     * silence), but a re-polled CLR makes the node re-send the whole 1024-byte
     * block, so a re-poll that fires while a normal DATA is merely in flight
     * yields a stray block that desynchronizes a later read. On Android the
     * CLR->DATA round-trip is thread-scheduled and routinely exceeds a few ms,
     * so the cadence must clear that: long enough that a surviving CLR's DATA
     * always lands first, short enough to still recover a genuinely dropped
     * CLR well before TIMEOUT_MS. */
    BRD_CLR_REPOLL_MS = 60,
    /* After serving a response FujiNet's bus loop discards its input
     * FIFO (wait_for_idle); a command sent back-to-back lands in that
     * window and is silently eaten. The real bus's turnaround time
     * makes this impossible on hardware; enforce an equivalent gap. */
    INTER_TX_GAP_MS = 4,
    PURGE_TIMEOUT_MS = 250,
    CRD_RETRY_MS = 10, /* re-poll cadence for a char device that answered
                          with "nothing yet" (NAK or empty DATA) */
    TIMEOUT_MS = 5000,
    /* Devices answer STATUS within ~200us on the real bus; a short
     * per-attempt timeout keeps the EOS 15-address roll-call fast when
     * most addresses have no FujiNet device behind them, while the
     * retries ride out host-side scheduling stalls. */
    STATUS_TIMEOUT_MS = 120,
    STATUS_RETRIES = 5
};

struct boip {
    int listen_fd;
    int fd;
    struct adamcore *mc;

    int state;
    uint16_t dcb;
    uint8_t dev;
    uint64_t t_start, t_last_poll;
    int clr_retries;
    int status_retries;

    uint8_t rx[1100];
    int rxlen;

    /* char-device read backoff so an empty device isn't hammered */
    uint16_t last_crd_dcb;
    uint64_t last_crd_nak_ms;
    /* first attempt of the current char-read chain: after a bounded
     * window of NAK/empty answers the read completes with ST_NAK */
    uint16_t crd_chain_dcb;
    uint64_t crd_chain_start;

    uint64_t t_keepalive;
    uint64_t t_last_done; /* when the previous transaction resolved */

    int ever_connected;
    int first_connect_pending;
};

static uint8_t xor_ck(const uint8_t *p, int n)
{
    uint8_t c = 0;
    while (n--) c ^= *p++;
    return c;
}

struct boip *boip_create(int listen_port)
{
    struct boip *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->fd = -1;
    b->listen_fd = net_listen(listen_port);
    if (b->listen_fd < 0) {
        free(b);
        return NULL;
    }
    return b;
}

void boip_destroy(struct boip *b)
{
    if (!b) return;
    net_close(b->fd);
    net_close(b->listen_fd);
    free(b);
}

int boip_connected(struct boip *b) { return b && b->fd >= 0; }

int boip_take_first_connect(struct boip *b)
{
    if (b && b->first_connect_pending) {
        b->first_connect_pending = 0;
        return 1;
    }
    return 0;
}

static void drop_connection_at(struct boip *b, const char *why)
{
    fprintf(stderr, "adamcore boip: dropping connection (%s, state=%d)\n",
            why, b->state);
    net_close(b->fd);
    b->fd = -1;
    b->rxlen = 0;
    /* fail an in-flight transaction so the DCB does not hang forever */
    if (b->state != B_IDLE && b->mc)
        b->mc->ram[b->dcb] = ST_TIMEOUT;
    b->state = B_IDLE;
}

void boip_bus_reset(struct boip *b)
{
    static const uint8_t devs[] = { 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    unsigned i;
    if (!b || b->fd < 0) return;
    b->state = B_IDLE;
    b->rxlen = 0;
    for (i = 0; i < sizeof(devs); i++) {
        uint8_t pkt = devs[i]; /* MN_RESET = type 0 -> the address byte */
        if (net_write(b->fd, &pkt, 1) < 0) {
            drop_connection_at(b, "bus-reset-write");
            return;
        }
    }
}

/* ---- transaction plumbing -------------------------------------------------- */

#ifdef __ANDROID__
#include <android/log.h>
#include <unistd.h>
/* On Android the trace is armed by a flag file (adb shell touch) and
 * lands in logcat under the AdamBoIP tag. */
static int wire_trace(void)
{
    static int t = -1;
    if (t < 0) t = access("/data/local/tmp/adamcore-trace", F_OK) == 0;
    return t;
}

static void dump_bytes(const char *dir, const uint8_t *p, int n)
{
    char line[128];
    int i, off = 0, lim = n > 16 ? 16 : n;
    for (i = 0; i < lim && off < (int)sizeof(line) - 4; i++)
        off += snprintf(line + off, sizeof(line) - (size_t)off, " %02X", p[i]);
    __android_log_print(ANDROID_LOG_INFO, "AdamBoIP", "[%llu] %s %d:%s%s",
                        (unsigned long long)net_now_ms(), dir, n, line,
                        n > 16 ? " ..." : "");
}
#else
static int wire_trace(void)
{
    static int t = -1;
    if (t < 0) t = getenv("ADAMCORE_BOIP_TRACE") != NULL;
    return t;
}

static void dump_bytes(const char *dir, const uint8_t *p, int n)
{
    int i, lim = n > 16 ? 16 : n;
    fprintf(stderr, "[%8llu] %s %d:", (unsigned long long)net_now_ms(), dir, n);
    for (i = 0; i < lim; i++) fprintf(stderr, " %02X", p[i]);
    fprintf(stderr, n > 16 ? " ...\n" : "\n");
}
#endif

static void tx(struct boip *b, const uint8_t *pkt, int n)
{
    if (wire_trace()) dump_bytes("TX", pkt, n);
    if (net_write(b->fd, pkt, n) < 0)
        drop_connection_at(b, "tx-write");
}

static void tx1(struct boip *b, uint8_t type)
{
    uint8_t pkt = (uint8_t)((type << 4) | b->dev);
    tx(b, &pkt, 1);
    b->t_last_poll = net_now_ms();
}

static void crd_nothing_yet(struct boip *b, uint64_t now);

static void complete(struct boip *b, uint8_t status)
{
    b->mc->ram[b->dcb] = status;
    b->state = B_IDLE;
    b->rxlen = 0;
    b->t_last_done = net_now_ms();
}

static void crd_nothing_yet(struct boip *b, uint64_t now)
{
    if (wire_trace())
        fprintf(stderr, "[%8llu] CRD-EMPTY dcb=%04X chain=%04X age=%llu\n",
                (unsigned long long)now, b->dcb, b->crd_chain_dcb,
                (unsigned long long)(now - b->crd_chain_start));
    if (b->crd_chain_dcb == b->dcb &&
        now - b->crd_chain_start > CRD_GIVEUP_MS) {
        b->crd_chain_dcb = 0;
        b->mc->ram[(uint16_t)(b->dcb + 3)] = 0; /* no data transferred */
        b->mc->ram[(uint16_t)(b->dcb + 4)] = 0;
        complete(b, ST_NAK);
        return;
    }
    b->last_crd_dcb = b->dcb;
    b->last_crd_nak_ms = now;
    b->state = B_IDLE;
    b->rxlen = 0;
    b->t_last_done = now;
}

static uint16_t dcb_buf_addr(struct boip *b)
{
    uint8_t *m = b->mc->ram;
    return (uint16_t)(m[(uint16_t)(b->dcb + 1)] |
                      (m[(uint16_t)(b->dcb + 2)] << 8));
}

static uint16_t dcb_buf_len(struct boip *b)
{
    uint8_t *m = b->mc->ram;
    return (uint16_t)(m[(uint16_t)(b->dcb + 3)] |
                      (m[(uint16_t)(b->dcb + 4)] << 8));
}

static void send_blocknum(struct boip *b)
{
    /* SEND with 5-byte payload: 32-bit block number (LE) + reserved */
    uint8_t *m = b->mc->ram;
    uint8_t pkt[9];
    pkt[0] = (uint8_t)(0x60 | b->dev);
    pkt[1] = 0x00;
    pkt[2] = 0x05; /* length, big-endian on the wire */
    pkt[3] = m[(uint16_t)(b->dcb + 5)];
    pkt[4] = m[(uint16_t)(b->dcb + 6)];
    pkt[5] = m[(uint16_t)(b->dcb + 7)];
    pkt[6] = m[(uint16_t)(b->dcb + 8)];
    pkt[7] = 0x00;
    pkt[8] = xor_ck(&pkt[3], 5);
    tx(b, pkt, 9);
    b->t_last_poll = net_now_ms();
}

static void send_payload(struct boip *b, const uint8_t *data, uint16_t len)
{
    uint8_t pkt[1100];
    pkt[0] = (uint8_t)(0x60 | b->dev);
    pkt[1] = (uint8_t)(len >> 8);
    pkt[2] = (uint8_t)(len & 0xFF);
    memcpy(&pkt[3], data, len);
    pkt[3 + len] = xor_ck(data, len);
    tx(b, pkt, 4 + len);
    b->t_last_poll = net_now_ms();
}

static void cwr_fire(struct boip *b)
{
    uint8_t *m = b->mc->ram;
    uint16_t len = dcb_buf_len(b);
    if (len == 0 || len > 1024) len = 1024;
    b->state = B_CWR_ACK;
    b->t_start = net_now_ms();
    b->rxlen = 0;
    send_payload(b, &m[dcb_buf_addr(b)], len);
}

/* The node's post-response input-discard window (wait_for_idle) must
 * pass before the command goes out, or it eats the command. */
static void cwr_send_now(struct boip *b)
{
    b->state = B_CWR_GAP;
    b->t_start = net_now_ms();
    b->rxlen = 0;
}

static int is_block_dev(uint8_t dev)
{
    return dev >= 4 && dev <= 8;
}

/* Network devices (N1..N6) run a stateful protocol whose RECEIVE is not a
 * side-effect-free "is a response pending?" probe: it drives the lazy
 * protocol read (e.g. kicks off the HTTP GET and stages the body). The
 * pre-write purge below must be skipped for them or it fetches and then
 * discards that body. */
static int is_net_dev(uint8_t dev)
{
    return dev >= 9 && dev <= 14;
}

/* ---- response parsing ------------------------------------------------------ */

static void handle_status_resp(struct boip *b)
{
    /* [0x8n size_lo size_hi devtype status ck] */
    uint8_t *m = b->mc->ram;
    m[(uint16_t)(b->dcb + 17)] = b->rx[1];
    m[(uint16_t)(b->dcb + 18)] = b->rx[2];
    m[(uint16_t)(b->dcb + 19)] = b->rx[3];
    /* NODE TYPE carries the device's error code, which EOS's read-verify
     * requires to be a ZERO nibble on success (it extracts one nibble,
     * selected per unit). FujiNet reports 0x40|code; strip the base and
     * mirror the code into both nibbles so either selection sees it. */
    {
        uint8_t code = (uint8_t)(b->rx[4] & 0x0F);
        m[(uint16_t)(b->dcb + 20)] = (uint8_t)(code | (code << 4));
    }
    complete(b, ST_OK);
}

static void handle_data_resp(struct boip *b)
{
    uint16_t len = (uint16_t)((b->rx[1] << 8) | b->rx[2]);
    uint8_t ck = b->rx[3 + len];
    uint8_t *m = b->mc->ram;
    uint16_t buf = dcb_buf_addr(b);
    uint16_t max = dcb_buf_len(b);
    uint16_t n = len;

    if (b->state == B_CRD_DATA && len == 0) {
        /* Empty response.send: nothing to deliver (yet). Re-poll for a
         * bounded window, then report ST_NAK so the guest can decide. */
        crd_nothing_yet(b, net_now_ms());
        return;
    }

    if (ck != xor_ck(&b->rx[3], len)) {
        if (b->state == B_BRD_DATA && b->clr_retries++ < 3) {
            /* disk CLR is retriable: the device's block buffer persists */
            b->rxlen = 0;
            tx1(b, 3);
            return;
        }
        /* generic CLR is not retriable; log and accept */
        fprintf(stderr, "adamcore boip: checksum mismatch dev %u\n", b->dev);
    }
    if (max && n > max) n = max;
    memcpy(&m[buf], &b->rx[3], n);
    /* Report the transferred length in the DCB buffer-length field:
     * EOS treats a completion without it as a failed transfer and
     * retries the whole operation (which also advances stateful
     * devices like the FujiNet directory cursor a second time). */
    m[(uint16_t)(b->dcb + 3)] = (uint8_t)n;
    m[(uint16_t)(b->dcb + 4)] = (uint8_t)(n >> 8);
    b->crd_chain_dcb = 0;
    complete(b, ST_OK);
}

static void feed_rx(struct boip *b)
{
    for (;;) {
        int r = net_read(b->fd, b->rx + b->rxlen,
                         (int)sizeof(b->rx) - b->rxlen);
        if (r < 0) { drop_connection_at(b, "read-eof-or-error"); return; }
        if (r == 0) return;
        if (wire_trace()) dump_bytes("RX", b->rx + b->rxlen, r);
        b->rxlen += r;
        if (b->rxlen >= (int)sizeof(b->rx)) return;
    }
}

static void advance(struct boip *b)
{
    uint8_t t;
    uint64_t now = net_now_ms();

    if (b->state == B_IDLE) return;
    feed_rx(b);
    if (b->fd < 0) return;

    if (b->rxlen > 0) {
        t = (uint8_t)(b->rx[0] >> 4);
        if ((b->rx[0] & 0x0F) != b->dev) {
            /* stray byte (e.g. a keep-alive ACK): not for this
             * transaction's device; discard and re-examine */
            memmove(b->rx, b->rx + 1, (size_t)--b->rxlen);
            return;
        }

        /* A recognized-but-incomplete packet must be WAITED for, never
         * trimmed: responses arrive fragmented on the socket. Only a
         * type that can't answer the current state is discarded. */
        switch (b->state) {
        case B_STATUS_WAIT:
            if (t == 0x8) {
                if (b->rxlen < 6) return;
                handle_status_resp(b);
                return;
            }
            if (t == 0xC) { complete(b, ST_TIMEOUT); return; }
            break;
        case B_BRD_BLKNUM_ACK:
        case B_BWR_BLKNUM_ACK:
            if (t == 0x9) {
                b->rxlen = 0;
                if (b->state == B_BRD_BLKNUM_ACK) {
                    b->state = B_BRD_RECEIVE;
                    tx1(b, 4); /* RECEIVE */
                } else {
                    uint8_t *m = b->mc->ram;
                    uint16_t len = dcb_buf_len(b);
                    if (len == 0 || len > 1024) len = 1024;
                    b->state = B_BWR_DATA_ACK;
                    send_payload(b, &m[dcb_buf_addr(b)], len);
                }
                return;
            }
            if (t == 0xC) { complete(b, ST_TIMEOUT); return; }
            break;
        case B_BRD_RECEIVE:
            if (t == 0x9) {
                /* single ACK: block is buffered; never re-poll past this */
                b->rxlen = 0;
                b->state = B_BRD_DATA;
                b->clr_retries = 0;
                tx1(b, 3); /* CLR */
                return;
            }
            if (t == 0xC) { complete(b, ST_TIMEOUT); return; }
            break;
        case B_BRD_DATA:
        case B_CRD_DATA:
            if (t == 0xB) {
                uint16_t len;
                if (b->rxlen < 3) return; /* wait for the length */
                len = (uint16_t)((b->rx[1] << 8) | b->rx[2]);
                if (len > 1024) { complete(b, ST_TIMEOUT); return; }
                if (b->rxlen < 4 + len) return; /* wait for payload+ck */
                handle_data_resp(b);
                return;
            }
            if (t == 0xC) {
                if (b->state == B_CRD_DATA)
                    crd_nothing_yet(b, now);
                else
                    complete(b, ST_TIMEOUT);
                return;
            }
            break;
        case B_CRD_RECEIVE:
            if (t == 0x9) {
                b->rxlen = 0;
                b->state = B_CRD_DATA;
                b->clr_retries = 0;
                tx1(b, 3); /* CLR */
                return;
            }
            if (t == 0xC) {
                crd_nothing_yet(b, now);
                return;
            }
            break;
        case B_BWR_DATA_ACK:
        case B_CWR_ACK:
            if (t == 0x9) { complete(b, ST_OK); return; }
            if (t == 0xC) { complete(b, ST_TIMEOUT); return; }
            break;
        case B_CWR_PURGE_RX:
            if (t == 0x9) { /* stale response exists: drain it */
                b->rxlen = 0;
                b->state = B_CWR_PURGE_DATA;
                tx1(b, 3); /* CLR */
                return;
            }
            if (t == 0xC) { cwr_send_now(b); return; } /* nothing stale */
            break;
        case B_CWR_PURGE_DATA:
            if (t == 0xB) {
                uint16_t len;
                if (b->rxlen < 3) return;
                len = (uint16_t)((b->rx[1] << 8) | b->rx[2]);
                if (len <= 1024 && b->rxlen < 4 + len) return;
                cwr_send_now(b); /* stale data discarded */
                return;
            }
            if (t == 0xC) { cwr_send_now(b); return; }
            break;
        default:
            break;
        }

        /* type can never serve this state: drop it and re-examine */
        memmove(b->rx, b->rx + 1, (size_t)--b->rxlen);
        return;
    }

    /* silence handling */
    if (b->state == B_CWR_GAP) {
        if (now - b->t_start >= INTER_TX_GAP_MS)
            cwr_fire(b);
        return;
    }
    if (b->state == B_CWR_PURGE_RX || b->state == B_CWR_PURGE_DATA) {
        if (now - b->t_start > PURGE_TIMEOUT_MS)
            cwr_send_now(b);
        return;
    }
    if (b->state == B_STATUS_WAIT) {
        if (now - b->t_start > STATUS_TIMEOUT_MS) {
            /* the real master retries a quiet node before giving up;
             * this also rides out FujiNet's startup registration */
            if (b->status_retries++ < STATUS_RETRIES) {
                b->t_start = now;
                b->rxlen = 0;
                tx1(b, 1);
                return;
            }
            complete(b, ST_TIMEOUT);
            return;
        }
    } else if (now - b->t_start > TIMEOUT_MS) {
        complete(b, ST_TIMEOUT);
        return;
    }
    if ((b->state == B_BRD_RECEIVE || b->state == B_CRD_RECEIVE) &&
        now - b->t_last_poll >= REPOLL_MS)
        /* Re-poll RECEIVE until the single ACK.
         *   Block (B_BRD_RECEIVE): rides out a silent seek stall.
         *   Char (B_CRD_RECEIVE): rides out a *node-side long command*.
         * When a FujiNet handler blocks the bus longer than
         * ADAMNET_LONG_CMD_US (a TNFS readdir or a network read is a real
         * network round-trip, unlike a local SD read), it finishes with
         * wait_for_idle()->discardInput(), which throws away the RECEIVE we
         * already sent. The node's resync is built on the assumption that the
         * master keeps re-polling RECEIVE every ~2 ms (see the
         * ADAMNET_LONG_CMD_US note in fujinet-pc); without this, a discarded
         * char RECEIVE stalled here until TIMEOUT_MS (5 s) and the guest
         * retried -- the intermittent multi-second hitch seen on TNFS
         * directory lists and network apps but never on SD. */
        tx1(b, 4);
    /* Same node-long-command hazard on the block-read CLR: a slow read (a TNFS
     * block is a real network round-trip) ends in wait_for_idle()->
     * discardInput(), which can drop the CLR we send right after the single
     * RECEIVE ACK. B_BRD_DATA otherwise stalled until TIMEOUT_MS and the read
     * returned late/wrong data -- observed as Donkey Kong Jr (mounted over
     * TNFS) loading a garbage VDP script and corrupting the display. Re-issuing
     * CLR is idempotent: the node's block buffer persists, so it just re-sends
     * the same 1024 bytes. Only fires on true silence (a slow-but-arriving DATA
     * leaves rx bytes buffered, so no spurious re-poll mid-transfer).
     * (The block-number SEND is not re-polled: it is not preceded by a long
     * command, so it is never discarded, and re-sending it could let a stray
     * duplicate ACK be mistaken for the following RECEIVE ACK.) */
    if (b->state == B_BRD_DATA && now - b->t_last_poll >= BRD_CLR_REPOLL_MS)
        tx1(b, 3); /* re-send CLR */
}

/* Pump the current transaction to completion in real wall-clock time.
 *
 * adamcore computes an entire video frame in microseconds and then sleeps
 * ~16 ms to pace; the peer's socket replies therefore only land during that
 * sleep, so a naive per-scan advance() makes every BoIP request/response
 * round-trip cost one whole emulated frame. A block read is 4 round-trips, so
 * it retired in ~3-4 frames (~50-70 ms). A real ADAM's AdamNet DMA -- and a
 * local-disk emulator -- finish a block in ~ms, and guests exploit that:
 * Super Games fire disk reads back to back without re-polling the DCB between
 * them. Against our slow retire the second read finds the DCB still marked
 * busy (0x04) and EOS drops it; the missed block leaves a graphics/descriptor
 * region unloaded and the display corrupts (Donkey Kong Jr's cage tiles).
 *
 * Settling gives the peer real time between polls so a block transfer retires
 * within its frame, matching hardware latency. Bounded so a slow/absent peer
 * degrades to the old per-frame path instead of hanging. */
static void boip_settle(struct boip *b)
{
    uint64_t start = net_now_ms();
    while (b->state != B_IDLE && b->fd >= 0) {
        struct timespec ts = { 0, 150000 }; /* 0.15 ms */
        advance(b);
        if (b->state == B_IDLE || b->fd < 0)
            break;
        if (net_now_ms() - start > 250)
            break; /* safety cap: resume the async path next frame */
        nanosleep(&ts, NULL);
    }
}

/* ---- public ---------------------------------------------------------------- */

void boip_poll(struct boip *b)
{
    int fd;
    if (!b) return;
    fd = net_accept(b->listen_fd);
    if (fd >= 0) {
        if (b->fd >= 0) drop_connection_at(b, "new-accept");
        b->fd = fd;
        b->rxlen = 0;
        if (!b->ever_connected) {
            b->ever_connected = 1;
            b->first_connect_pending = 1;
        }
    }
    if (b->fd >= 0 && b->state != B_IDLE) {
        advance(b);
    } else if (b->fd >= 0) {
        /* The real AdamNet bus is never silent (the master polls the
         * keyboard continuously); FujiNet's channel logic assumes that.
         * With the keyboard served locally we keep the link warm with a
         * READY to an address no FujiNet device claims: it generates no
         * response, so it can never be mistaken for a transaction ACK. */
        uint64_t now = net_now_ms();
        if (now - b->t_keepalive >= 300) {
            uint8_t pkt = 0xD3; /* READY -> unpopulated address 3 */
            b->t_keepalive = now;
            if (net_write(b->fd, &pkt, 1) < 0)
                drop_connection_at(b, "keepalive-write");
        }
        if (b->fd >= 0) {
            feed_rx(b);
            b->rxlen = 0; /* drain the READY ACK and any strays */
        }
    }
}

void boip_dispatch(struct boip *b, struct adamcore *mc, uint16_t d,
                   uint8_t dcmd, uint8_t dev)
{
    if (b->state != B_IDLE)
        return; /* one transaction at a time; this DCB stays pending */
    if (net_now_ms() - b->t_last_done < INTER_TX_GAP_MS)
        return; /* respect the node's post-response turnaround window */

    b->mc = mc;
    b->dcb = d;
    b->dev = dev;
    b->t_start = net_now_ms();
    feed_rx(b);
    b->rxlen = 0; /* drop anything stale before the new exchange */
    if (b->fd < 0) return;

    switch (dcmd) {
    case 1: /* status */
        b->state = B_STATUS_WAIT;
        b->status_retries = 0;
        tx1(b, 1);
        break;
    case 2: /* soft reset: wire RESET, no response defined */
        tx1(b, 0);
        complete(b, ST_OK);
        break;
    case 3: /* write */
        if (is_block_dev(dev)) {
            b->state = B_BWR_BLKNUM_ACK;
            send_blocknum(b);
        } else if (is_net_dev(dev)) {
            /* Never purge a network device: its RECEIVE triggers the lazy
             * protocol read, so a purge probe fetches the response body and
             * then throws it away. Observed as ISS hanging on "FETCHING
             * DATA" -- the pre-CHANNEL_MODE purge drained the iss-now.json
             * body (delivered as a 112-byte DATA packet) before the JSON
             * parser could read it. Send the command directly; these devices
             * don't accumulate the stale Fuji-style responses the purge
             * (added for the mount-corruption workaround, whose real cause
             * was the double-param(0) bug) was built for. */
            cwr_send_now(b);
        } else {
            b->state = B_CWR_PURGE_RX;
            tx1(b, 4); /* RECEIVE: probe for a stale pending response */
        }
        break;
    case 4: /* read */
        if (is_block_dev(dev)) {
            b->state = B_BRD_BLKNUM_ACK;
            send_blocknum(b);
        } else {
            /* back off empty char devices instead of hammering them */
            if (d == b->last_crd_dcb &&
                net_now_ms() - b->last_crd_nak_ms < CRD_RETRY_MS)
                return;
            if (b->crd_chain_dcb != d) {
                b->crd_chain_dcb = d;
                b->crd_chain_start = net_now_ms();
            }
            if (wire_trace())
                fprintf(stderr, "[%8llu] CRD-CLAIM dcb=%04X\n",
                        (unsigned long long)net_now_ms(), d);
            b->state = B_CRD_RECEIVE;
            tx1(b, 4);
        }
        break;
    default:
        break;
    }

    /* Block reads/writes are DMA-fast on real hardware; retire them within the
     * frame so a guest that fires them back-to-back never sees a stale busy
     * DCB. Char/network devices are intentionally lazy (their RECEIVE triggers
     * work host-side) and must stay on the async path. */
    if (is_block_dev(dev) && (dcmd == 3 || dcmd == 4) && b->state != B_IDLE)
        boip_settle(b);
}
