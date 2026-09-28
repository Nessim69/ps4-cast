// Host test for the M3U/IPTV parser (app/src/m3u.c), including the round trip
// of per-channel options through urlopt.c into the actual request headers.
#include "m3u.h"
#include "urlopt.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct { int n; char name[16][96]; char group[16][64]; char spec[16][1024]; char tvg[16][64]; char logo[16][256]; } Got;
static int collect(void *ctx, const M3uEntry *e) {
    Got *g = ctx;
    if (g->n < 16) {
        snprintf(g->name[g->n], sizeof g->name[0], "%s", e->name);
        snprintf(g->group[g->n], sizeof g->group[0], "%s", e->group);
        snprintf(g->spec[g->n], sizeof g->spec[0], "%s", e->spec);
        snprintf(g->tvg[g->n], sizeof g->tvg[0], "%s", e->tvgId);
        snprintf(g->logo[g->n], sizeof g->logo[0], "%s", e->logo);
    }
    g->n++;
    return 0;
}

// The header block the player would send for a spec.
static const char *headers_of(const char *spec) {
    static char url[1024];
    urlopt_apply(spec, url, sizeof url);
    return urlopt_headers();
}

int main(void) {
    static Got g;
    const char *list =
        "#EXTM3U x-tvg-url=\"http://epg/x.xml\"\n"
        "#EXTINF:-1 tvg-id=\"a\" group-title=\"News, World\",Channel A\n"
        "#EXTVLCOPT:http-referrer=https://site.example/live\n"
        "#EXTVLCOPT:http-user-agent=Mozilla/5.0 (X11; Linux) Safari & Co 100%\n"
        "http://cdn.example/a.m3u8\n"
        "#EXTINF:-1,Channel B\n"                                   // options must not leak into B
        "http://cdn.example/b.m3u8\n"
        "#EXTGRP:Sports\n"
        "#EXTINF:-1 user-agent=\"AttrUA/1\" http-referrer=\"https://ref.example/\",Channel C\n"
        "http://cdn.example/c.ts\n"
        "#EXTINF:-1,Channel D\n"
        "#EXTHTTP:{\"User-Agent\":\"Json \\\"UA\\\"\",\"Referer\":\"https://j.example/\",\"cookie\":\"sid=1; x=2\",\"n\":3}\n"
        "http://cdn.example/d.m3u8\n"
        "#EXTINF:-1,Channel E\n"
        "#KODIPROP:inputstream.adaptive.stream_headers=User-Agent=Kodi%2F20&Referer=https%3A%2F%2Fk.example%2F\n"
        "#KODIPROP:inputstream.adaptive.license_type=clearkey\n"
        "http://cdn.example/e.mpd\n"
        "#EXTINF:-1,Channel F\n"
        "#EXTVLCOPT:http-user-agent=VlcUA\n"
        "http://cdn.example/f.m3u8|Referer=https://mine.example/&User-Agent=MineUA\n"  // own options win
        "http://cdn.example/path/bare.ts?token=1\n";
    CHECK(m3u_parse(list, 1024, collect, &g) == 7);

    CHECK(strcmp(g.name[0], "Channel A") == 0 && strcmp(g.group[0], "News, World") == 0);
    CHECK(strcmp(g.tvg[0], "a") == 0 && g.logo[0][0] == 0);
    CHECK(g.tvg[1][0] == 0);                                      // not carried over
    CHECK(strstr(headers_of(g.spec[0]), "User-Agent: Mozilla/5.0 (X11; Linux) Safari & Co 100%\r\n") != NULL);
    CHECK(strstr(headers_of(g.spec[0]), "Referer: https://site.example/live\r\n") != NULL);
    CHECK(strncmp(g.spec[0], "http://cdn.example/a.m3u8|", 26) == 0);

    CHECK(strcmp(g.spec[1], "http://cdn.example/b.m3u8") == 0);            // nothing leaked
    CHECK(strcmp(g.group[1], "") == 0);

    CHECK(strcmp(g.group[2], "Sports") == 0);                              // sticky #EXTGRP
    CHECK(strstr(headers_of(g.spec[2]), "User-Agent: AttrUA/1\r\n") != NULL);
    CHECK(strstr(headers_of(g.spec[2]), "Referer: https://ref.example/\r\n") != NULL);

    CHECK(strstr(headers_of(g.spec[3]), "User-Agent: Json \"UA\"\r\n") != NULL);
    CHECK(strstr(headers_of(g.spec[3]), "Referer: https://j.example/\r\n") != NULL);
    CHECK(strstr(headers_of(g.spec[3]), "Cookie: sid=1; x=2\r\n") != NULL);

    CHECK(strstr(headers_of(g.spec[4]), "User-Agent: Kodi/20\r\n") != NULL);
    CHECK(strstr(headers_of(g.spec[4]), "Referer: https://k.example/\r\n") != NULL);

    CHECK(strstr(headers_of(g.spec[5]), "Referer: https://mine.example/\r\n") != NULL);
    CHECK(strstr(headers_of(g.spec[5]), "User-Agent: MineUA\r\n") != NULL);
    CHECK(strstr(headers_of(g.spec[5]), "VlcUA") == NULL);

    CHECK(strcmp(g.name[6], "bare.ts") == 0 && strcmp(g.spec[6], "http://cdn.example/path/bare.ts?token=1") == 0);

    // guide link and per-channel ids/logos
    char epg[256];
    CHECK(m3u_epg_url(list, epg, sizeof epg) == 1 && strcmp(epg, "http://epg/x.xml") == 0);
    CHECK(m3u_epg_url("#EXTM3U url-tvg=\"https://a/g.xml.gz,https://b/g.xml\"\n", epg, sizeof epg) == 1 &&
          strcmp(epg, "https://a/g.xml.gz") == 0);
    CHECK(m3u_epg_url("#EXTM3U\n#EXTINF:-1 x-tvg-url=\"http://no\",x\n", epg, sizeof epg) == 0);
    memset(&g, 0, sizeof g);
    CHECK(m3u_parse("#EXTM3U\n#EXTINF:-1 tvg-id=\"bbc1.uk\" tvg-name=\"BBC One\" tvg-logo=\"http://l/bbc1.png\",BBC One\nhttp://h/1\n"
                    "#EXTINF:-1 logo=\"http://l/x.png\" x-tvg-id=\"no\",X\nhttp://h/2\n", 1024, collect, &g) == 2);
    CHECK(strcmp(g.tvg[0], "bbc1.uk") == 0 && strcmp(g.logo[0], "http://l/bbc1.png") == 0);
    CHECK(g.tvg[1][0] == 0 && strcmp(g.logo[1], "http://l/x.png") == 0);

    // list-wide defaults on #EXTM3U, overridden per entry
    memset(&g, 0, sizeof g);
    const char *defs =
        "#EXTM3U user-agent=\"ListUA\" referrer=\"https://list.example/\"\n"
        "#EXTINF:-1,One\nhttp://h/1.m3u8\n"
        "#EXTINF:-1,Two\n#EXTVLCOPT:http-user-agent=OwnUA\nhttp://h/2.m3u8\n";
    CHECK(m3u_parse(defs, 1024, collect, &g) == 2);
    CHECK(strstr(headers_of(g.spec[0]), "User-Agent: ListUA\r\n") && strstr(headers_of(g.spec[0]), "Referer: https://list.example/\r\n"));
    CHECK(strstr(headers_of(g.spec[1]), "User-Agent: OwnUA\r\n") && strstr(headers_of(g.spec[1]), "Referer: https://list.example/\r\n"));

    // options that don't fit are dropped whole, the most useful kept
    M3uOpts e; m3u_opts_clear(&e);
    snprintf(e.ua, sizeof e.ua, "UA"); snprintf(e.cookie, sizeof e.cookie, "a-rather-long-cookie-value-here");
    char out[40];
    m3u_spec(&e, NULL, "http://h/x.m3u8", out, sizeof out);
    CHECK(strcmp(out, "http://h/x.m3u8|User-Agent=UA") == 0);

    // control characters never reach a header
    m3u_opts_clear(&e);
    CHECK(m3u_opts_line(&e, "#EXTVLCOPT:http-user-agent=Evil\x01UA\x7f") == 1);
    CHECK(strcmp(e.ua, "EvilUA") == 0);
    // unknown directives and keys are ignored
    CHECK(m3u_opts_line(&e, "#EXTVLCOPT:network-caching=1000") == 1 && strcmp(e.ua, "EvilUA") == 0);
    CHECK(m3u_opts_line(&e, "#EXT-X-SOMETHING:1") == 0);

    printf(failures ? "test_m3u: %d FAILURES\n" : "test_m3u: all ok\n", failures);
    return failures ? 1 : 0;
}
