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
#include <unistd.h>

#include "adamcore.h"

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
    int reset_at = -1, reset_mode = 0;
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
        else if (!strcmp(a, "--reset-at")) { reset_at = atoi(argv[++i]); reset_mode = atoi(argv[++i]); }
        else { fprintf(stderr, "unknown arg %s\n", a); return 2; }
    }

    c = adamcore_create(&cfg);
    if (!c) { fprintf(stderr, "adamcore_create failed (ROM paths?)\n"); return 1; }

    if (stdin_keys)
        fcntl(0, F_SETFL, O_NONBLOCK);

    {
        FILE *wf = NULL;
        long wav_samples = 0;
        int16_t abuf[1024];
        int f;

        if (wav) {
            wf = fopen(wav, "wb");
            if (wf) fseek(wf, 44, SEEK_SET); /* header written at end */
        }

        for (f = 0; f < frames; f++) {
            if (f == reset_at)
                adamcore_request_reset(c, reset_mode);
            adamcore_run_frame(c);

            if (stdin_keys) {
                unsigned char kb[64];
                ssize_t n = read(0, kb, sizeof(kb));
                ssize_t k;
                for (k = 0; k < n; k++)
                    adamcore_inject_key(c, kb[k] == '\n' ? 0x0D : kb[k]);
            }
            if (type_text && *type_text && f >= type_frame &&
                (f - type_frame) % 8 == 0) {
                uint8_t ch = (uint8_t)*type_text++;
                adamcore_inject_key(c, ch == '\n' ? 0x0D : ch);
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

    adamcore_destroy(c);
    return 0;
}
