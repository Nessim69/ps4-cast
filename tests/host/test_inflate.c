// Host test for the streaming inflater (app/src/inflate.c): gzip (all header
// flags, stored/fixed/dynamic blocks, multiple members, trailing junk), zlib,
// plain passthrough, checksum and truncation errors, and the callback stop --
// each fed whole, byte by byte and in odd-sized pieces.
#include "inflate.h"
#include "inflate_vectors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static const char ALPHA[] = "abc de\n<>/=\"tvgprogrammechannel";
static const char *WORDS[16] = { "<programme ", "channel=\"", "start=\"20260928", "</title>\n", "<desc>", "news ", "sport",
                                 " +0000\" ", "stop=\"", "\">", "<title lang=\"en\">", "</programme>\n", "film ", "the ", "and ", "BBC" };

// gen_inflate_vectors.py's gen(), appended to out.
static size_t gen(uint8_t *out, size_t n, unsigned seed) {
    unsigned long x = seed;
    size_t o = 0;
    while (o < n) {
        x = (x * 1103515245ul + 12345ul) & 0x7FFFFFFFul;
        unsigned k = (unsigned)((x >> 16) % 20);
        if (k < 16) {
            for (const char *w = WORDS[k]; *w && o < n; w++) out[o++] = (uint8_t)*w;
        } else {
            out[o++] = (uint8_t)ALPHA[(x >> 8) % (sizeof(ALPHA) - 1)];
        }
    }
    return o;
}

typedef struct { uint8_t *buf; size_t len, cap; int calls, stopAfter; } Sink;
static int sink(void *ctx, const uint8_t *p, int n) {
    Sink *s = ctx;
    s->calls++;
    if (s->len + (size_t)n > s->cap) { s->cap = (s->len + (size_t)n) * 2; s->buf = realloc(s->buf, s->cap); }
    memcpy(s->buf + s->len, p, (size_t)n);
    s->len += (size_t)n;
    return s->stopAfter && s->calls >= s->stopAfter;
}

// Feed v in pieces of `step` bytes (0 = pseudo-random 1..300).
static int run(const InflVec *v, int step, Sink *s, char *fmt) {
    Inflate *z = inflate_new(sink, s);
    int rc = INFLATE_OK, off = 0;
    unsigned r = 12345;
    while (off < v->zlen && (rc == INFLATE_OK || rc == INFLATE_END)) {
        int n = step;
        if (!n) { r = r * 1103515245u + 12345u; n = 1 + (int)((r >> 16) % 300); }
        if (n > v->zlen - off) n = v->zlen - off;
        rc = inflate_push(z, v->z + off, n);
        off += n;
    }
    if (rc == INFLATE_OK || rc == INFLATE_END) rc = inflate_finish(z);
    *fmt = inflate_format(z);
    inflate_free(z);
    return rc;
}

int main(void) {
    static uint8_t want[1 << 17];
    const int steps[] = { 1 << 20, 1, 2, 7, 1000, 0 };
    int nvec = (int)(sizeof(VECS) / sizeof(VECS[0]));
    for (int i = 0; i < nvec; i++) {
        const InflVec *v = &VECS[i];
        size_t wl = 0;
        for (int p = 0; p < v->nparts; p++) wl += gen(want + wl, (size_t)v->parts[p][0], (unsigned)v->parts[p][1]);
        for (size_t k = 0; k < sizeof(steps) / sizeof(steps[0]); k++) {
            Sink s = { 0 };
            char fmt = 0;
            int rc = run(v, steps[k], &s, &fmt);
            int ok;
            if (strcmp(v->expect, "end") == 0) {
                ok = rc == INFLATE_END && s.len == wl && (wl == 0 || memcmp(s.buf, want, wl) == 0);
            } else if (strcmp(v->expect, "plain") == 0) {
                ok = rc == INFLATE_END && fmt == 'p' && s.len == (size_t)v->zlen && memcmp(s.buf, v->z, s.len) == 0;
            } else if (strcmp(v->expect, "err") == 0) {
                ok = rc == INFLATE_ERR;
            } else {
                ok = rc == INFLATE_SHORT;
            }
            if (!ok) {
                failures++;
                printf("FAIL %s step=%d rc=%d fmt=%c out=%zu want=%zu\n", v->name, steps[k], rc, fmt, s.len, wl);
            }
            free(s.buf);
        }
    }

    // Stop: the callback's first "enough" ends it, and nothing more comes out.
    {
        const InflVec *v = NULL;
        for (int i = 0; i < nvec; i++) if (strcmp(VECS[i].name, "gzip_window") == 0) v = &VECS[i];
        Sink s = { 0 };
        s.stopAfter = 1;
        Inflate *z = inflate_new(sink, &s);
        int rc = inflate_push(z, v->z, v->zlen);
        CHECK(rc == INFLATE_STOP);
        CHECK(s.calls == 1 && s.len == 32768);
        CHECK(inflate_push(z, v->z, 10) == INFLATE_STOP);
        CHECK(inflate_finish(z) == INFLATE_STOP && s.calls == 1);
        inflate_free(z);
        free(s.buf);
    }
    // Formats.
    {
        Sink s = { 0 };
        char fmt;
        CHECK(run(&VECS[0], 1 << 20, &s, &fmt) == INFLATE_END && fmt == 'g');
        free(s.buf);
        s = (Sink){ 0 };
        for (int i = 0; i < nvec; i++)
            if (strcmp(VECS[i].name, "zlib_fixed") == 0) { run(&VECS[i], 5, &s, &fmt); CHECK(fmt == 'z'); }
        free(s.buf);
        // A lone byte is passed through at finish.
        s = (Sink){ 0 };
        Inflate *z = inflate_new(sink, &s);
        CHECK(inflate_push(z, (const uint8_t *)"<", 1) == INFLATE_OK && s.len == 0);
        CHECK(inflate_finish(z) == INFLATE_END && s.len == 1 && inflate_format(z) == 'p');
        inflate_free(z);
        free(s.buf);
    }
    if (failures) { printf("test_inflate: %d failure(s)\n", failures); return 1; }
    printf("test_inflate: all ok (%d vectors x %d feeds)\n", nvec, (int)(sizeof(steps) / sizeof(steps[0])));
    return 0;
}
