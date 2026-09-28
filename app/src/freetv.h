// freetv.h — free-to-air channels from iptv-org's public directory.
//
// iptv-org (github.com/iptv-org/iptv) lists publicly available streams and
// publishes them as M3U playlists per language and per country. This builds
// a channel list from those: the chosen languages' playlists filtered by
// category, plus every channel of the "first" countries, de-duplicated,
// with bouquets per country (or per category) and the first countries on
// top. The playlists are fetched when the list is built, so stream links are
// as fresh as the directory. Pure C: freetv_net.c does the downloading.
#ifndef PS4CAST_FREETV_H
#define PS4CAST_FREETV_H

#define FREETV_MAX_LANGS 8
#define FREETV_MAX_CATS  16
#define FREETV_MAX_FIRST 8

typedef struct {
    char langs[FREETV_MAX_LANGS][4];   // ISO 639-3: "ara", "eng", "fra"
    int  nLangs;
    char cats[FREETV_MAX_CATS][16];    // iptv-org category ids; none = every category
    int  nCats;
    char first[FREETV_MAX_FIRST][3];   // country codes kept whole and listed first: "TN"
    int  nFirst;
    int  byCategory;                   // bouquets per category instead of per country
} FreeTvOpts;

// What the TV's Square button adds: Arabic, English and French channels that
// are documentaries or cartoons (animation, kids), plus all of Tunisia.
void freetv_opts_default(FreeTvOpts *o);
// "lang=ara,eng,fra;cat=documentary,kids;first=TN;group=country" (any key may
// be left out and keeps its default; "cat=all" = every category). 0 if usable.
int  freetv_opts_parse(const char *s, FreeTvOpts *o);

typedef struct FreeTv FreeTv;
FreeTv *freetv_new(const FreeTvOpts *o);
// One downloaded playlist: a language's (filtered by category) or, with
// whole = 1, a "first" country's (kept whole).
void    freetv_add(FreeTv *b, const char *m3uText, int whole);
// The result as M3U text (malloc'd); *count = channels in it.
char   *freetv_m3u(FreeTv *b, int *count);
int     freetv_count(const FreeTv *b);
void    freetv_free(FreeTv *b);

// "TN" -> "Tunisia" ("" for an unknown code).
const char *freetv_country_name(const char *code);
// The country a stream is for (out has room for 3; "" when unknown): the
// feed's when the feed is named after one ("PlutoTVKids.de@FR" -> "FR", the
// French feed of a channel registered in Germany), else the channel's
// ("ElWatania1.tn@SD" -> "TN"; "SD" there means standard definition).
void freetv_country_of(const char *tvgId, char *out);

#endif
