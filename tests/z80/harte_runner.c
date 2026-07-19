/*
 * adamcore - SingleStepTests (Tom Harte) Z80 vector runner
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Runs every test in tests/data/harte-z80/v1/*.json (or the files given on
 * the command line) against the adamcore Z80 core. Compares the full final
 * register state (including WZ, Q, IFF1/2, IM, R), RAM contents, I/O port
 * traffic, and the total T-state count (one "cycles" entry per T-state).
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "z80.h"

static uint8_t mem[65536];

typedef struct {
    cJSON *ports;   /* array or NULL */
    int cursor;
    int io_errors;
} io_ctx;

static io_ctx g_io;

static uint8_t t_mem_read(void *ud, uint16_t a) { (void)ud; return mem[a]; }
static void t_mem_write(void *ud, uint16_t a, uint8_t v) { (void)ud; mem[a] = v; }

static cJSON *next_port(const char *dir)
{
    if (!g_io.ports) return NULL;
    cJSON *e = cJSON_GetArrayItem(g_io.ports, g_io.cursor);
    if (!e) return NULL;
    const char *d = cJSON_GetArrayItem(e, 2)->valuestring;
    if (d[0] != dir[0]) return NULL;
    g_io.cursor++;
    return e;
}

static uint8_t t_io_read(void *ud, uint16_t p, uint8_t *ok)
{
    (void)ud; (void)ok;
    cJSON *e = next_port("r");
    if (!e) { g_io.io_errors++; return 0xFF; }
    if ((uint16_t)cJSON_GetArrayItem(e, 0)->valueint != p) g_io.io_errors++;
    return (uint8_t)cJSON_GetArrayItem(e, 1)->valueint;
}

static uint8_t io_read_cb(void *ud, uint16_t p) { return t_io_read(ud, p, NULL); }

static void io_write_cb(void *ud, uint16_t p, uint8_t v)
{
    (void)ud;
    cJSON *e = next_port("w");
    if (!e) { g_io.io_errors++; return; }
    if ((uint16_t)cJSON_GetArrayItem(e, 0)->valueint != p) g_io.io_errors++;
    if ((uint8_t)cJSON_GetArrayItem(e, 1)->valueint != v) g_io.io_errors++;
}

static int geti(cJSON *o, const char *k)
{
    cJSON *e = cJSON_GetObjectItemCaseSensitive(o, k);
    return e ? e->valueint : 0;
}

static void load_state(z80 *z, cJSON *st)
{
    memset(z, 0, sizeof(*z));
    z->pc = (uint16_t)geti(st, "pc");
    z->sp = (uint16_t)geti(st, "sp");
    z->a = (uint8_t)geti(st, "a");
    z->b = (uint8_t)geti(st, "b");
    z->c = (uint8_t)geti(st, "c");
    z->d = (uint8_t)geti(st, "d");
    z->e = (uint8_t)geti(st, "e");
    z->f = (uint8_t)geti(st, "f");
    z->h = (uint8_t)geti(st, "h");
    z->l = (uint8_t)geti(st, "l");
    z->i = (uint8_t)geti(st, "i");
    z->r = (uint8_t)geti(st, "r");
    z->wz = (uint16_t)geti(st, "wz");
    z->ix = (uint16_t)geti(st, "ix");
    z->iy = (uint16_t)geti(st, "iy");
    z->a2 = (uint8_t)(geti(st, "af_") >> 8);
    z->f2 = (uint8_t)(geti(st, "af_") & 0xFF);
    z->b2 = (uint8_t)(geti(st, "bc_") >> 8);
    z->c2 = (uint8_t)(geti(st, "bc_") & 0xFF);
    z->d2 = (uint8_t)(geti(st, "de_") >> 8);
    z->e2 = (uint8_t)(geti(st, "de_") & 0xFF);
    z->h2 = (uint8_t)(geti(st, "hl_") >> 8);
    z->l2 = (uint8_t)(geti(st, "hl_") & 0xFF);
    z->im = (uint8_t)geti(st, "im");
    z->q = (uint8_t)geti(st, "q");
    z->iff1 = (uint8_t)geti(st, "iff1");
    z->iff2 = (uint8_t)geti(st, "iff2");
    z->mem_read = t_mem_read;
    z->mem_write = t_mem_write;
    z->io_read = io_read_cb;
    z->io_write = io_write_cb;
}

#define CHK(field, got, want) \
    do { \
        if ((got) != (want)) { \
            if (shown < 8) \
                printf("      %-4s got %04X want %04X\n", field, (unsigned)(got), \
                       (unsigned)(want)); \
            bad = 1; \
        } \
    } while (0)

static int run_test(cJSON *t, const char *fname, int *shown_p)
{
    z80 z;
    cJSON *ini = cJSON_GetObjectItemCaseSensitive(t, "initial");
    cJSON *fin = cJSON_GetObjectItemCaseSensitive(t, "final");
    cJSON *cyc = cJSON_GetObjectItemCaseSensitive(t, "cycles");
    cJSON *ram, *e;
    int bad = 0, shown = *shown_p;

    load_state(&z, ini);
    memset(mem, 0, sizeof(mem));
    ram = cJSON_GetObjectItemCaseSensitive(ini, "ram");
    cJSON_ArrayForEach(e, ram)
        mem[cJSON_GetArrayItem(e, 0)->valueint] =
            (uint8_t)cJSON_GetArrayItem(e, 1)->valueint;

    g_io.ports = cJSON_GetObjectItemCaseSensitive(t, "ports");
    g_io.cursor = 0;
    g_io.io_errors = 0;

    int tstates = z80_step(&z);

    CHK("pc", z.pc, (uint16_t)geti(fin, "pc"));
    CHK("sp", z.sp, (uint16_t)geti(fin, "sp"));
    CHK("a", z.a, (uint8_t)geti(fin, "a"));
    CHK("b", z.b, (uint8_t)geti(fin, "b"));
    CHK("c", z.c, (uint8_t)geti(fin, "c"));
    CHK("d", z.d, (uint8_t)geti(fin, "d"));
    CHK("e", z.e, (uint8_t)geti(fin, "e"));
    CHK("f", z.f, (uint8_t)geti(fin, "f"));
    CHK("h", z.h, (uint8_t)geti(fin, "h"));
    CHK("l", z.l, (uint8_t)geti(fin, "l"));
    CHK("i", z.i, (uint8_t)geti(fin, "i"));
    CHK("r", z.r, (uint8_t)geti(fin, "r"));
    CHK("wz", z.wz, (uint16_t)geti(fin, "wz"));
    CHK("ix", z.ix, (uint16_t)geti(fin, "ix"));
    CHK("iy", z.iy, (uint16_t)geti(fin, "iy"));
    CHK("af_", (unsigned)((z.a2 << 8) | z.f2), (unsigned)geti(fin, "af_"));
    CHK("bc_", (unsigned)((z.b2 << 8) | z.c2), (unsigned)geti(fin, "bc_"));
    CHK("de_", (unsigned)((z.d2 << 8) | z.e2), (unsigned)geti(fin, "de_"));
    CHK("hl_", (unsigned)((z.h2 << 8) | z.l2), (unsigned)geti(fin, "hl_"));
    CHK("im", z.im, (uint8_t)geti(fin, "im"));
    CHK("iff1", z.iff1, (uint8_t)geti(fin, "iff1"));
    CHK("iff2", z.iff2, (uint8_t)geti(fin, "iff2"));
    CHK("q", z.q, (uint8_t)geti(fin, "q"));
    CHK("cyc", (unsigned)tstates, (unsigned)cJSON_GetArraySize(cyc));

    ram = cJSON_GetObjectItemCaseSensitive(fin, "ram");
    cJSON_ArrayForEach(e, ram) {
        int addr = cJSON_GetArrayItem(e, 0)->valueint;
        uint8_t want = (uint8_t)cJSON_GetArrayItem(e, 1)->valueint;
        if (mem[addr] != want) {
            if (shown < 8)
                printf("      ram[%04X] got %02X want %02X\n", addr, mem[addr], want);
            bad = 1;
        }
    }
    if (g_io.io_errors) {
        if (shown < 8) printf("      io traffic mismatch (%d)\n", g_io.io_errors);
        bad = 1;
    }

    if (bad && shown < 8) {
        printf("    ^ %s test \"%s\"\n", fname,
               cJSON_GetObjectItemCaseSensitive(t, "name")->valuestring);
        (*shown_p)++;
    }
    return bad;
}

static int run_file(const char *path, long *total, long *failed)
{
    FILE *fp = fopen(path, "rb");
    char *buf;
    long len;
    int shown = 0;
    int file_failed = 0;

    if (!fp) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = malloc((size_t)len + 1);
    if (fread(buf, 1, (size_t)len, fp) != (size_t)len) { fclose(fp); free(buf); return -1; }
    buf[len] = 0;
    fclose(fp);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) { fprintf(stderr, "parse error in %s\n", path); return -1; }

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    cJSON *t;
    cJSON_ArrayForEach(t, root) {
        (*total)++;
        if (run_test(t, base, &shown)) { (*failed)++; file_failed++; }
    }
    cJSON_Delete(root);
    return file_failed;
}

static int cmpstr(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int main(int argc, char **argv)
{
    long total = 0, failed = 0;
    int nfiles = 0, nbadfiles = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <dir-or-json> [more...]\n", argv[0]);
        return 2;
    }

    for (int i = 1; i < argc; i++) {
        char pathbuf[4096];
        DIR *d;
        const char *dirpath = argv[i];

        snprintf(pathbuf, sizeof(pathbuf), "%s/v1", argv[i]);
        d = opendir(pathbuf);
        if (d) dirpath = strdup(pathbuf);
        else d = opendir(argv[i]);

        if (d) {
            struct dirent *de;
            char **names = NULL;
            int cap = 0;
            while ((de = readdir(d))) {
                size_t n = strlen(de->d_name);
                if (n > 5 && !strcmp(de->d_name + n - 5, ".json")) {
                    if (nfiles == cap) {
                        cap = cap ? cap * 2 : 2048;
                        names = realloc(names, sizeof(char *) * (size_t)cap);
                    }
                    names[nfiles++] = strdup(de->d_name);
                }
            }
            closedir(d);
            qsort(names, (size_t)nfiles, sizeof(char *), cmpstr);
            for (int k = 0; k < nfiles; k++) {
                snprintf(pathbuf, sizeof(pathbuf), "%s/%s", dirpath, names[k]);
                int r = run_file(pathbuf, &total, &failed);
                if (r > 0) {
                    nbadfiles++;
                    printf("  FAIL %-14s %d failing\n", names[k], r);
                }
                if ((k + 1) % 200 == 0)
                    fprintf(stderr, "... %d/%d files\n", k + 1, nfiles);
            }
        } else {
            nfiles++;
            if (run_file(argv[i], &total, &failed) > 0) nbadfiles++;
        }
    }

    printf("harte: %ld/%ld tests passed (%d/%d files clean)\n",
           total - failed, total, nfiles - nbadfiles, nfiles);
    return failed ? 1 : 0;
}
