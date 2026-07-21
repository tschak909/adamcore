/*
 * adamcore - Linux development runner
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Headless driver for testing: accepts the same flag names the Android
 * host uses, runs N frames, dumps PPM frames / WAV audio, and injects
 * keys from stdin (each byte; "\n" maps to ADAM RETURN 0x0D).
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "adamcore.h"
#include "../src/machine.h"

static uint16_t g_watch_base;
static struct adamcore *g_wc;
static uint8_t (*g_orig_rd)(void *, uint16_t);
static void (*g_orig_wr)(void *, uint16_t, uint8_t);
static int g_watch_events;

static uint8_t watch_rd(void *ud, uint16_t a)
{
    uint8_t v = g_orig_rd(ud, a);
    if (a >= g_watch_base && a < g_watch_base + 21 && g_watch_events < 4000 &&
        g_wc->cpu.pc != 0xF914 && g_wc->cpu.pc != 0xF9E3) {
        static uint16_t lastpc; static uint16_t lasta;
        if (g_wc->cpu.pc != lastpc || a != lasta) {
            fprintf(stderr, "DCB R +%-2d =%02X pc=%04X\n",
                    a - g_watch_base, v, g_wc->cpu.pc);
            g_watch_events++; lastpc = g_wc->cpu.pc; lasta = a;
        }
    }
    return v;
}

static void watch_wr(void *ud, uint16_t a, uint8_t v)
{
    if (a >= g_watch_base && a < g_watch_base + 21 && g_watch_events < 4000 &&
        g_wc->cpu.pc != 0xF914 && g_wc->cpu.pc != 0xF9E3) {
        fprintf(stderr, "DCB W +%-2d =%02X pc=%04X\n",
                a - g_watch_base, v, g_wc->cpu.pc);
        g_watch_events++;
    }
    g_orig_wr(ud, a, v);
}

static void write_ppm(const char *path, const uint16_t *fb, int w, int h)
{
    FILE *fp = fopen(path, "wb");
    int i;
    if (!fp) return;
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    for (i = 0; i < w * h; i++) {
        uint16_t p = fb[i];
        unsigned char rgb[3];
        rgb[0] = (unsigned char)(((p >> 11) & 0x1F) << 3);
        rgb[1] = (unsigned char)(((p >> 5) & 0x3F) << 2);
        rgb[2] = (unsigned char)((p & 0x1F) << 3);
        fwrite(rgb, 1, 3, fp);
    }
    fclose(fp);
}

int main(int argc, char **argv)
{
    adamcore_config cfg;
    adamcore *c;
    int frames = 600;
    const char *ppm = NULL;
    const char *ppm_prefix = NULL;
    int ppm_every = 0;
    const char *wav = NULL;
    int stdin_keys = 0;
    int type_frame = -1;
    const char *type_text = NULL;
    int type2_frame = -1;
    const char *type2_text = NULL;
    int reset_at = -1, reset_mode = 0;
    int throttle = 0;
    int jitter = 0;
    int watch_dcb = 0;
    int pc_hist = 0;
    const char *dump_ram = NULL;
    const char *dump_vram = NULL;
    static unsigned long hist[65536];
    int kp_frame[8], kp_digit[8], kp_n = 0;
    int i;

    memset(&cfg, 0, sizeof(cfg));
    cfg.start_machine = ADAMCORE_MACHINE_CV;
    cfg.audio_rate = 44100;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-adam")) cfg.start_machine = ADAMCORE_MACHINE_ADAM;
        else if (!strcmp(a, "-os7")) cfg.os7_rom_path = argv[++i];
        else if (!strcmp(a, "-eos")) cfg.eos_rom_path = argv[++i];
        else if (!strcmp(a, "-wp")) cfg.wp_rom_path = argv[++i];
        else if (!strcmp(a, "-cart")) cfg.cart_path = argv[++i];
        else if (!strcmp(a, "-fujinet")) cfg.boip_listen_port = atoi(argv[++i]);
        else if (!strcmp(a, "-palette")) cfg.palette = atoi(argv[++i]);
        else if (!strcmp(a, "-expansion")) cfg.expansion = atoi(argv[++i]);
        else if (!strcmp(a, "-joystick")) cfg.joystick_mode = atoi(argv[++i]);
        else if (!strcmp(a, "-swapbuttons")) cfg.swap_buttons = atoi(argv[++i]);
        else if (!strcmp(a, "-keypad")) cfg.reverse_keypad = atoi(argv[++i]);
        else if (!strcmp(a, "--frames")) frames = atoi(argv[++i]);
        else if (!strcmp(a, "--ppm")) ppm = argv[++i];
        else if (!strcmp(a, "--ppm-every")) { ppm_every = atoi(argv[++i]); ppm_prefix = argv[++i]; }
        else if (!strcmp(a, "--wav")) wav = argv[++i];
        else if (!strcmp(a, "--stdin-keys")) stdin_keys = 1;
        else if (!strcmp(a, "--type")) { type_frame = atoi(argv[++i]); type_text = argv[++i]; }
        else if (!strcmp(a, "--type2")) { type2_frame = atoi(argv[++i]); type2_text = argv[++i]; }
        else if (!strcmp(a, "--reset-at")) { reset_at = atoi(argv[++i]); reset_mode = atoi(argv[++i]); }
        else if (!strcmp(a, "--throttle")) throttle = 1;
        else if (!strcmp(a, "--jitter")) jitter = 1;
        else if (!strcmp(a, "--watch-dcb")) watch_dcb = (int)strtol(argv[++i], NULL, 16);
        else if (!strcmp(a, "--pc-hist")) pc_hist = 1;
        else if (!strcmp(a, "--dump-ram")) dump_ram = argv[++i];
        else if (!strcmp(a, "--dump-vram")) dump_vram = argv[++i];
        else if (!strcmp(a, "--keypad")) {
            if (kp_n < 8) {
                kp_frame[kp_n] = atoi(argv[++i]);
                kp_digit[kp_n] = atoi(argv[++i]);
                kp_n++;
            } else { i += 2; }
        }
        else { fprintf(stderr, "unknown arg %s\n", a); return 2; }
    }

    c = adamcore_create(&cfg);
    if (!c) { fprintf(stderr, "adamcore_create failed (ROM paths?)\n"); return 1; }

    if (stdin_keys)
        fcntl(0, F_SETFL, O_NONBLOCK);

    if (watch_dcb) {
        g_watch_base = (uint16_t)watch_dcb;
        g_wc = (struct adamcore *)c;
        g_orig_rd = g_wc->cpu.mem_read;
        g_orig_wr = g_wc->cpu.mem_write;
        g_wc->cpu.mem_read = watch_rd;
        g_wc->cpu.mem_write = watch_wr;
    }

    {
        FILE *wf = NULL;
        long wav_samples = 0;
        int16_t abuf[1024];
        int f;

        if (wav) {
            wf = fopen(wav, "wb");
            if (wf) fseek(wf, 44, SEEK_SET); /* header written at end */
        }

        struct timespec next;
        clock_gettime(CLOCK_MONOTONIC, &next);
        for (f = 0; f < frames; f++) {
            if (throttle) {
                /* jitter mode: emulate a phone's uneven frame pacing */
                long extra = jitter ? (long)(rand() % 25) * 1000000L : 0;
                next.tv_nsec += 16688000 + extra; /* 59.922 Hz */
                while (next.tv_nsec >= 1000000000L) {
                    next.tv_nsec -= 1000000000;
                    next.tv_sec++;
                }
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
            }
            if (f == reset_at)
                adamcore_request_reset(c, reset_mode);
            if (kp_n) {
                uint16_t word = 0x7F7F; /* idle */
                int j;
                for (j = 0; j < kp_n; j++)
                    if (f >= kp_frame[j] && f < kp_frame[j] + 12)
                        word = (uint16_t)(0x7F00 | (0x70 | (kp_digit[j] & 0x0F)));
                adamcore_set_joystick(c, 0, word);
            }
            adamcore_run_frame(c);
            if (pc_hist) hist[((struct adamcore *)c)->cpu.pc]++;

            if (stdin_keys) {
                unsigned char kb[64];
                ssize_t n = read(0, kb, sizeof(kb));
                ssize_t k;
                for (k = 0; k < n; k++)
                    adamcore_inject_key(c, kb[k] == '\n' ? 0x0D : kb[k]);
            }
            if (type2_text && *type2_text && f >= type2_frame &&
                (f - type2_frame) % 8 == 0) {
                uint8_t c2 = (uint8_t)*type2_text++;
                adamcore_inject_key(c, c2 == '\n' ? 0x0D : c2);
            }
            if (type_text && *type_text && f >= type_frame &&
                (f - type_frame) % 8 == 0) {
                uint8_t ch = (uint8_t)*type_text++;
                if (ch == '\n') ch = 0x0D;
                else if (ch == '^') { /* ^1..^6 = SmartKeys I..VI */
                    ch = (uint8_t)(0x80 + (*type_text++ - '0'));
                } else if (ch == '\b') {
                    ch = 0x08;
                }
                adamcore_inject_key(c, ch);
            }
            if (wf) {
                /* 44100/59.922 = 736 samples per frame */
                int want = 736;
                while (want > 0) {
                    int chunk = want > 1024 ? 1024 : want;
                    adamcore_render_audio(c, abuf, chunk);
                    fwrite(abuf, 2, (size_t)chunk, wf);
                    wav_samples += chunk;
                    want -= chunk;
                }
            }
            if (ppm_every && (f % ppm_every) == 0) {
                char path[512];
                int w, h;
                const uint16_t *fb = adamcore_framebuffer(c, &w, &h);
                snprintf(path, sizeof(path), "%s%05d.ppm", ppm_prefix, f);
                write_ppm(path, fb, w, h);
            }
        }

        if (ppm) {
            int w, h;
            const uint16_t *fb = adamcore_framebuffer(c, &w, &h);
            write_ppm(ppm, fb, w, h);
        }
        if (wf) {
            /* minimal WAV header */
            uint32_t data = (uint32_t)(wav_samples * 2);
            uint32_t riff = data + 36;
            uint32_t rate = 44100, brate = 44100 * 2;
            uint16_t align = 2, bits = 16, fmt = 1, ch = 1;
            fseek(wf, 0, SEEK_SET);
            fwrite("RIFF", 1, 4, wf); fwrite(&riff, 4, 1, wf);
            fwrite("WAVEfmt ", 1, 8, wf);
            riff = 16; fwrite(&riff, 4, 1, wf);
            fwrite(&fmt, 2, 1, wf); fwrite(&ch, 2, 1, wf);
            fwrite(&rate, 4, 1, wf); fwrite(&brate, 4, 1, wf);
            fwrite(&align, 2, 1, wf); fwrite(&bits, 2, 1, wf);
            fwrite("data", 1, 4, wf); fwrite(&data, 4, 1, wf);
            fclose(wf);
        }
    }

    if (dump_ram) {
        FILE *df = fopen(dump_ram, "wb");
        if (df) {
            fwrite(((struct adamcore *)c)->ram, 1, 65536, df);
            fclose(df);
        }
    }
    if (dump_ram) { /* also dump expansion RAM alongside */
        char p[600];
        snprintf(p, sizeof(p), "%s.xram", dump_ram);
        FILE *xf = fopen(p, "wb");
        if (xf) { fwrite(((struct adamcore *)c)->xram, 1, 65536, xf); fclose(xf); }
    }
    if (dump_vram) {
        FILE *df = fopen(dump_vram, "wb");
        if (df) {
            fwrite(((struct adamcore *)c)->vdp.vram, 1, 16384, df);
            fclose(df);
        }
        fprintf(stderr, "VDP regs: R0=%02X R1=%02X R2=%02X R3=%02X R4=%02X R5=%02X R6=%02X R7=%02X\n",
                ((struct adamcore *)c)->vdp.regs[0], ((struct adamcore *)c)->vdp.regs[1],
                ((struct adamcore *)c)->vdp.regs[2], ((struct adamcore *)c)->vdp.regs[3],
                ((struct adamcore *)c)->vdp.regs[4], ((struct adamcore *)c)->vdp.regs[5],
                ((struct adamcore *)c)->vdp.regs[6], ((struct adamcore *)c)->vdp.regs[7]);
    }
    if (pc_hist) {
        int k;
        fprintf(stderr, "NMIs delivered: %lu over %d frames (VDP R1=%02X)\n",
                ((struct adamcore *)c)->nmi_count, frames,
                ((struct adamcore *)c)->vdp.regs[1]);
        fprintf(stderr, "PC histogram (top):\n");
        for (k = 0; k < 65536; k++)
            if (hist[k] > frames / 50)
                fprintf(stderr, "  %04X x%lu\n", k, hist[k]);
    }

    adamcore_destroy(c);
    return 0;
}
