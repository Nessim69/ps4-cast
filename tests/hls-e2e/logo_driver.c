// End-to-end channel logos: logo.c's worker fetches PNGs a playlist names
// from server.py (on the logo aseg channel), decodes them, and the "main
// loop" here adopts them in logo_tick() and draws them with the host gfx.
// A window of rows scrolls through more channels than the cache holds, so
// logos are evicted and fetched again; broken links and a non-image fail
// quietly. Usage: logo_driver BASE_URL
#include "logo.h"
#include "httpd.h"
#include "httpd_channels.h"
#include "aseg.h"
#include "gfx.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NCH 220          // more than logo.c keeps (160)

static uint32_t colour(int i) {     // server.py paints logo i this colour
    return (uint32_t)((i * 37) & 255) << 16 | (uint32_t)((i * 91) & 255) << 8 | (uint32_t)((i * 53) & 255);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    alarm(120);
    const char *base = argc > 1 ? argv[1] : "http://127.0.0.1:1";
    aseg_init();
    httpd_channels_init();

    size_t cap = (size_t)NCH * 200 + 64, o = 0;
    char *list = malloc(cap);
    o += (size_t)snprintf(list + o, cap - o, "#EXTM3U\n");
    for (int i = 0; i < NCH; i++) {
        const char *logo = i == 5 ? "%s/logo/missing.png" : i == 6 ? "%s/epg/page.html" : "%s/logo/%d.png";
        char l[256];
        snprintf(l, sizeof l, logo, base, i);
        o += (size_t)snprintf(list + o, cap - o, "#EXTINF:-1 tvg-logo=\"%s\",Channel %d\nhttp://tv.example/%d\n", l, i, i);
    }
    int jl = 0;
    free(httpd_channels_load_playlist(list, "http://p.example/l.m3u", &jl));
    free(list);
    if (httpd_chan_count() != NCH || !httpd_chan_has_logos()) { printf("playlist not loaded\n"); return 1; }

    Gfx g;
    memset(&g, 0, sizeof g);
    g.width = 640; g.height = 480; g.depth = 4;
    g.frameBuffers[0] = calloc((size_t)g.width * g.height, 4);
    logo_init();

    // Scroll a 10-row window over all channels twice; each position stays
    // until its logos are in (or 3 s pass).
    int drawnOnce[NCH] = { 0 }, bad = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int top = 0; top + 10 <= NCH; top += 10) {
            for (int frame = 0; frame < 300; frame++) {
                logo_tick();
                memset(g.frameBuffers[0], 0, (size_t)g.width * g.height * 4);
                int all = 1;
                for (int r = 0; r < 10; r++) {
                    int ch = top + r, y = r * 48;
                    int ok = logo_draw(&g, ch, 10, y, 64, 40);
                    if (ok) {
                        drawnOnce[ch] = 1;
                        uint32_t px = ((uint32_t *)g.frameBuffers[0])[(y + 20) * g.width + 42] & 0xffffff;
                        if (px != colour(ch)) { bad++; printf("channel %d: pixel %06x, want %06x\n", ch, px, colour(ch)); }
                    } else if (ch != 5 && ch != 6) {
                        all = 0;
                    }
                }
                if (all) break;
                usleep(10000);
            }
        }
    }
    int missing = 0;
    for (int i = 0; i < NCH; i++) if (i != 5 && i != 6 && !drawnOnce[i]) missing++;
    if (drawnOnce[5] || drawnOnce[6]) { printf("a broken logo was drawn\n"); return 1; }
    if (missing || bad) { printf("%d logos never drawn, %d wrong\n", missing, bad); return 1; }
    printf("logos ok\n");
    return 0;
}
