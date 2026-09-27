// End-to-end: hls_open + hls_read (+ hls_audio_read, or hls_next_segment with
// SEGDEMUX=1) against tests/hls-e2e/server.py, compared byte-for-byte with the
// expected plaintext. Usage: driver URL EXPECTED|- [EXPECTED_AUDIO]
// ("-" = the open must fail, e.g. DRM).
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "hls.h"
#include "aseg.h"
#include <stdint.h>

static unsigned char *slurp(const char *p, long *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(*n ? *n : 1);
    if (fread(b, 1, (size_t)*n, f) != (size_t)*n) { fclose(f); free(b); return NULL; }
    fclose(f); return b;
}

static int read_all(int audio, unsigned char **out, long *outN) {
    long cap = 1 << 20, n = 0; unsigned char *b = malloc(cap);
    for (;;) {
        if (n + 65536 > cap) { cap *= 2; b = realloc(b, cap); }
        int r = audio ? hls_audio_read(b + n, 65536) : hls_read(b + n, 65536);
        if (r <= 0) break;
        n += r;
    }
    *out = b; *outN = n; return 0;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    alarm(120);             // a stuck stream (e.g. an init segment replayed forever) fails, never hangs
    aseg_init();
    const char *url = argv[1], *want = argv[2];
    int rc = hls_open(url);
    if (strcmp(want, "-") == 0) {
        printf("open rc=%d reason=[%s] dbg=[%s]\n", rc, hls_unsupported_reason(), hls_debug());
        return rc == 0;   // must fail
    }
    if (rc != 0) { printf("OPEN FAILED rc=%d dbg=%s\n", rc, hls_debug()); return 1; }
    printf("dbg: %.160s\n", hls_debug());
    long wn, gn; unsigned char *w = slurp(want, &wn), *g;
    if (getenv("SEGDEMUX")) {           // the player's TS segment-demux path
        hls_set_external_segment_fetch(1);
        long cap = 1 << 20; gn = 0; g = malloc(cap);
        uint8_t *sb; int sl, rg;
        while (hls_next_segment(&sb, &sl, &rg) == 0) {
            while (gn + sl > cap) { cap *= 2; g = realloc(g, cap); }
            memcpy(g + gn, sb, sl); gn += sl; free(sb);
        }
    } else read_all(0, &g, &gn);
    int ok = (gn == wn && memcmp(g, w, wn) == 0);
    printf("video: got %ld want %ld %s\n", gn, wn, ok ? "MATCH" : "MISMATCH");
    if (!ok) { long i = 0; while (i < gn && i < wn && g[i] == w[i]) i++; printf("  first diff at %ld\n", i); }
    free(g); free(w);
    if (argc > 3) {
        long an, agn; unsigned char *a = slurp(argv[3], &an), *ag;
        if (!hls_has_audio()) { printf("audio: rendition not ready\n"); ok = 0; }
        else {
            read_all(1, &ag, &agn);
            int aok = (agn == an && memcmp(ag, a, an) == 0);
            printf("audio: got %ld want %ld %s\n", agn, an, aok ? "MATCH" : "MISMATCH");
            ok = ok && aok;
            free(ag);
        }
        free(a);
    }
    hls_close();
    return ok ? 0 : 1;
}
