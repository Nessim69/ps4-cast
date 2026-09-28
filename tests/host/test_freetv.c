// Host test for the free-channel builder (app/src/freetv.c): options, the
// country of a stream, category filtering, whole "first" countries, de-dup
// across playlists, bouquets per country / per kind, names, and that the
// result reads back through m3u.c with each channel's options intact.
#include "freetv.h"
#include "m3u.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct { int n; char line[64][256]; } Got;
static int collect(void *ctx, const M3uEntry *e) {
    Got *g = ctx;
    if (g->n < 64) snprintf(g->line[g->n], sizeof g->line[0], "%s|%s|%s|%s", e->group, e->name, e->tvgId, e->spec);
    g->n++;
    return 0;
}

static const char *ARA =
    "#EXTM3U\n"
    "#EXTINF:-1 tvg-id=\"AsharqDocumentary.sa@SD\" tvg-logo=\"https://l/a.png\" group-title=\"Documentary\",Asharq Documentary (1080p)\n"
    "https://s/asharq.m3u8\n"
    "#EXTINF:-1 tvg-id=\"SpacetoonArabic.ae@SD\" group-title=\"Animation;Kids\",Spacetoon Arabic (720p)\n"
    "https://s/spacetoon.m3u8\n"
    "#EXTINF:-1 tvg-id=\"AlJazeera.qa@SD\" group-title=\"News\",Al Jazeera (1080p)\n"
    "https://s/aj.m3u8\n"
    "#EXTINF:-1 tvg-id=\"ElWatania1.tn@SD\" group-title=\"General\",El Watania 1 (1080i)\n"
    "https://s/watania1.m3u8\n"
    "#EXTINF:-1 tvg-id=\"BabyFirst.us@SD\" group-title=\"Kids\",BabyFirst\n"
    "https://s/babyfirst.m3u8\n";
static const char *ENG =
    "#EXTM3U\n"
    "#EXTINF:-1 tvg-id=\"BabyFirst.us@SD\" group-title=\"Kids\",BabyFirst\n"          // same URL as in Arabic
    "https://s/babyfirst.m3u8\n"
    "#EXTINF:-1 tvg-id=\"NatureTime.us@SD\" group-title=\"Documentary\",Nature Time\n"
    "#EXTVLCOPT:http-referrer=https://site.example/\n"
    "https://s/nature.m3u8\n"
    "#EXTINF:-1 tvg-id=\"NatureTime.us@SD\" group-title=\"Documentary\",Nature Time\n"   // a second stream
    "https://s/nature-backup.m3u8\n"
    "#EXTINF:-1 tvg-id=\"Adult.us@SD\" group-title=\"XXX;Documentary\",Nope\n"
    "https://s/xxx.m3u8\n"
    "#EXTINF:-1 tvg-id=\"PlutoDoc.se@DK\" group-title=\"Documentary\",Pluto Dokumentar DK\n"
    "https://s/pluto-dk.m3u8\n"
    "#EXTINF:-1 tvg-id=\"\" group-title=\"Documentary\",No Country \"Doc\" [Geo-blocked]\n"
    "https://s/nocountry.m3u8\n";
static const char *TN =
    "#EXTM3U\n"
    "#EXTINF:-1 tvg-id=\"ElWatania1.tn@SD\" group-title=\"General\",El Watania 1 (1080i)\n"
    "https://s/watania1.m3u8\n"
    "#EXTINF:-1 tvg-id=\"JawharaTV.tn@SD\" group-title=\"Music\",Jawhara TV (720p) [Not 24/7]\n"
    "https://s/jawhara.m3u8\n";

int main(void) {
    char cc[3];
    freetv_country_of("ElWatania1.tn@SD", cc);        CHECK(!strcmp(cc, "TN"));
    freetv_country_of("PlutoTVKids.de@FR", cc);       CHECK(!strcmp(cc, "FR"));
    freetv_country_of("BBCOne.uk@HD", cc);             CHECK(!strcmp(cc, "UK"));
    freetv_country_of("9Go.au@Sydney", cc);            CHECK(!strcmp(cc, "AU"));
    freetv_country_of("Plain.fr", cc);                 CHECK(!strcmp(cc, "FR"));
    freetv_country_of("NoDot", cc);                    CHECK(cc[0] == '\0');
    freetv_country_of("", cc);                         CHECK(cc[0] == '\0');
    CHECK(!strcmp(freetv_country_name("TN"), "Tunisia") && !strcmp(freetv_country_name("UK"), "United Kingdom"));
    CHECK(freetv_country_name("ZZ")[0] == '\0');

    FreeTvOpts o;
    freetv_opts_default(&o);
    CHECK(o.nLangs == 3 && !strcmp(o.langs[2], "fra") && o.nCats == 3 && o.nFirst == 1 && !strcmp(o.first[0], "TN") && !o.byCategory);
    CHECK(freetv_opts_parse("lang=eng;cat=all;first=fr,ma;group=category", &o) == 0);
    CHECK(o.nLangs == 1 && o.nCats == 0 && o.nFirst == 2 && !strcmp(o.first[1], "MA") && o.byCategory);
    CHECK(freetv_opts_parse("lang=;first=", &o) == -1);                 // nothing to fetch
    CHECK(freetv_opts_parse("lang=Eng,x,abcd,spa;cat=News,documentary", &o) == 0);
    CHECK(o.nLangs == 1 && !strcmp(o.langs[0], "spa") && o.nCats == 1 && !strcmp(o.cats[0], "documentary"));
    CHECK(freetv_opts_parse("", &o) == 0 && o.nLangs == 3);

    // Defaults: Arabic + English, documentaries and cartoons, Tunisia whole.
    freetv_opts_default(&o);
    FreeTv *b = freetv_new(&o);
    freetv_add(b, ARA, 0);
    freetv_add(b, ENG, 0);
    freetv_add(b, TN, 1);
    int n = 0;
    char *m = freetv_m3u(b, &n);
    static Got g;
    memset(&g, 0, sizeof g);
    m3u_parse(m, 1024, collect, &g);
    CHECK(n == g.n);
    // Tunisia first (whole: Jawhara is music), then countries A-Z; names
    // without the picture size; the second Nature Time stream numbered;
    // BabyFirst once; no adult channel; no news.
    const char *want[] = {
        "Tunisia|El Watania 1|ElWatania1.tn@SD|https://s/watania1.m3u8",
        "Tunisia|Jawhara TV [Not 24/7]|JawharaTV.tn@SD|https://s/jawhara.m3u8",
        "Denmark|Pluto Dokumentar DK|PlutoDoc.se@DK|https://s/pluto-dk.m3u8",
        "International|No Country \"Doc\" [Geo-blocked]||https://s/nocountry.m3u8",
        "Saudi Arabia|Asharq Documentary|AsharqDocumentary.sa@SD|https://s/asharq.m3u8",
        "United Arab Emirates|Spacetoon Arabic|SpacetoonArabic.ae@SD|https://s/spacetoon.m3u8",
        "United States|BabyFirst|BabyFirst.us@SD|https://s/babyfirst.m3u8",
        "United States|Nature Time|NatureTime.us@SD|https://s/nature.m3u8|Referer=https://site.example/",
        "United States|Nature Time (2)|NatureTime.us@SD|https://s/nature-backup.m3u8",
    };
    int nw = (int)(sizeof(want) / sizeof(want[0]));
    CHECK(g.n == nw);
    for (int i = 0; i < nw && i < g.n; i++) {
        if (strcmp(g.line[i], want[i]) != 0) { failures++; printf("FAIL row %d\n  got  %s\n  want %s\n", i, g.line[i], want[i]); }
    }
    free(m);
    freetv_free(b);

    // Bouquets per kind; "first" countries keep theirs on top.
    freetv_opts_parse("lang=ara,eng;cat=documentary,animation,kids;first=TN;group=category", &o);
    b = freetv_new(&o);
    freetv_add(b, ARA, 0);
    freetv_add(b, ENG, 0);
    freetv_add(b, TN, 1);
    m = freetv_m3u(b, &n);
    memset(&g, 0, sizeof g);
    m3u_parse(m, 1024, collect, &g);
    CHECK(g.n == nw);
    CHECK(!strncmp(g.line[0], "Tunisia|", 8) && !strncmp(g.line[2], "Cartoons|Spacetoon", 18));
    CHECK(!strncmp(g.line[3], "Documentary|", 12) && !strncmp(g.line[8], "Kids|BabyFirst", 14));
    free(m);
    freetv_free(b);

    // Official sources for Tunisia join its bouquet (Nessma twice: site, then
    // YouTube as "(2)"), and only when Tunisia is one of the "first".
    freetv_opts_default(&o);
    b = freetv_new(&o);
    freetv_add(b, TN, 1);
    freetv_add_official(b);
    m = freetv_m3u(b, &n);
    memset(&g, 0, sizeof g);
    m3u_parse(m, 1024, collect, &g);
    CHECK(g.n == 6);
    CHECK(!strcmp(g.line[0], "Tunisia|Attessia TV|AttessiaTV.tn@SD|https://www.youtube.com/channel/UCQS3ejF2jBAhwmbGD9Q3oeA/live"));
    CHECK(!strcmp(g.line[2], "Tunisia|Hannibal TV|HannibalTV.tn@SD|https://www.youtube.com/channel/UCMowjs_MJ-oIWEeHUu3DrOQ/live"));
    CHECK(!strcmp(g.line[5], "Tunisia|Nessma (2)|NessmaElJadida.tn@SD|https://www.youtube.com/channel/UC-48PCT3flS86JkLzxlTA9g/live"));
    CHECK(!strcmp(g.line[4], "Tunisia|Nessma|NessmaElJadida.tn@SD|https://live.nessma.tv/"));
    free(m);
    freetv_free(b);
    freetv_opts_parse("first=MA", &o);
    b = freetv_new(&o);
    freetv_add_official(b);
    CHECK(freetv_count(b) == 0);
    freetv_free(b);

    // Everything, English only: news and the rest come in, adult still out.
    freetv_opts_parse("lang=eng;cat=all;first=", &o);
    b = freetv_new(&o);
    freetv_add(b, ENG, 0);
    CHECK(freetv_count(b) == 5);
    freetv_free(b);

    if (failures) { printf("test_freetv: %d failure(s)\n", failures); return 1; }
    printf("test_freetv: all ok\n");
    return 0;
}
