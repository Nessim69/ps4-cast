// freetv.c — free-to-air channel list from iptv-org's playlists. See freetv.h.
#include "freetv.h"
#include "m3u.h"
#include "freetv_countries.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPEC_MAX 1024

typedef struct {
    char *name, *spec, *tvg, *logo, *cats;   // cats: lowercase, ';'-separated
    char  cc[3];
    int   whole;                             // from a "first" country's list
    int   order;                             // arrival order, for a stable sort
    const char *group;                       // bouquet (set by freetv_m3u)
    int   firstRank;                         // index in opts.first, else FREETV_MAX_FIRST
} Entry;

struct FreeTv {
    FreeTvOpts o;
    Entry     *e;
    int        n, cap;
    uint64_t  *seen;                         // URL hashes (open addressing, 0 = empty)
    size_t     seenCap, seenN;
};

// ---- options ----------------------------------------------------------------
void freetv_opts_default(FreeTvOpts *o) {
    memset(o, 0, sizeof(*o));
    static const char *L[] = { "ara", "eng", "fra" }, *C[] = { "documentary", "animation", "kids" };
    for (int i = 0; i < 3; i++) snprintf(o->langs[o->nLangs++], sizeof(o->langs[0]), "%s", L[i]);
    for (int i = 0; i < 3; i++) snprintf(o->cats[o->nCats++], sizeof(o->cats[0]), "%s", C[i]);
    snprintf(o->first[o->nFirst++], sizeof(o->first[0]), "TN");
}

static int all_of(const char *s, int n, int (*ok)(int)) {
    for (int i = 0; i < n; i++) if (!ok((unsigned char)s[i])) return 0;
    return 1;
}
static int is_lower(int c) { return c >= 'a' && c <= 'z'; }
static int is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

int freetv_opts_parse(const char *s, FreeTvOpts *o) {
    freetv_opts_default(o);
    if (!s) return 0;
    const char *p = s;
    while (*p) {
        const char *end = p;
        while (*end && *end != ';' && *end != '&' && *end != '\n' && *end != '\r') end++;
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (eq) {
            int kl = (int)(eq - p);
            const char *v = eq + 1;
            int isLang = kl == 4 && !memcmp(p, "lang", 4), isCat = kl == 3 && !memcmp(p, "cat", 3);
            int isFirst = kl == 5 && !memcmp(p, "first", 5), isGroup = kl == 5 && !memcmp(p, "group", 5);
            if (isGroup) o->byCategory = (end - v == 8 && !memcmp(v, "category", 8));
            if (isLang) o->nLangs = 0;
            if (isCat) o->nCats = 0;
            if (isFirst) o->nFirst = 0;
            while ((isLang || isCat || isFirst) && v < end) {
                const char *c = v;
                while (c < end && *c != ',') c++;
                int len = (int)(c - v);
                if (isLang && len == 3 && all_of(v, 3, is_lower) && o->nLangs < FREETV_MAX_LANGS)
                    memcpy(o->langs[o->nLangs++], v, 3);
                else if (isCat && len == 3 && !memcmp(v, "all", 3))
                    o->nCats = -1;                                   // every category
                else if (isCat && len > 0 && len < 16 && all_of(v, len, is_lower) && o->nCats >= 0 &&
                         o->nCats < FREETV_MAX_CATS)
                    memcpy(o->cats[o->nCats++], v, (size_t)len);
                else if (isFirst && len == 2 && all_of(v, 2, is_alpha) && o->nFirst < FREETV_MAX_FIRST) {
                    o->first[o->nFirst][0] = (char)(v[0] & ~0x20);
                    o->first[o->nFirst][1] = (char)(v[1] & ~0x20);
                    o->nFirst++;
                }
                v = c < end ? c + 1 : c;
            }
        }
        p = *end ? end + 1 : end;
    }
    if (o->nCats < 0) o->nCats = 0;
    for (int i = 0; i < o->nLangs; i++) o->langs[i][3] = '\0';
    return o->nLangs > 0 || o->nFirst > 0 ? 0 : -1;
}

// ---- countries ------------------------------------------------------------------
const char *freetv_country_name(const char *code) {
    int lo = 0, hi = (int)(sizeof(FREETV_COUNTRIES) / sizeof(FREETV_COUNTRIES[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(FREETV_COUNTRIES[mid].code, code);
        if (!c) return FREETV_COUNTRIES[mid].name;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return "";
}

void freetv_country_of(const char *tvgId, char *out) {
    out[0] = '\0';
    const char *at = strchr(tvgId, '@');
    if (at && strlen(at + 1) == 2 && at[1] >= 'A' && at[1] <= 'Z' && at[2] >= 'A' && at[2] <= 'Z' &&
        strcmp(at + 1, "SD") != 0 && strcmp(at + 1, "HD") != 0 && freetv_country_name(at + 1)[0]) {
        memcpy(out, at + 1, 3);
        return;
    }
    const char *end = at ? at : tvgId + strlen(tvgId);
    const char *dot = NULL;
    for (const char *p = tvgId; p < end; p++) if (*p == '.') dot = p;
    if (!dot || end - dot != 3 || !is_alpha((unsigned char)dot[1]) || !is_alpha((unsigned char)dot[2])) return;
    out[0] = (char)(dot[1] & ~0x20);
    out[1] = (char)(dot[2] & ~0x20);
    out[2] = '\0';
}

// ---- building -------------------------------------------------------------------
FreeTv *freetv_new(const FreeTvOpts *o) {
    FreeTv *b = calloc(1, sizeof(*b));
    if (b) b->o = *o;
    return b;
}

void freetv_free(FreeTv *b) {
    if (!b) return;
    for (int i = 0; i < b->n; i++) {
        Entry *e = &b->e[i];
        free(e->name); free(e->spec); free(e->tvg); free(e->logo); free(e->cats);
    }
    free(b->e);
    free(b->seen);
    free(b);
}

int freetv_count(const FreeTv *b) { return b->n; }

static uint64_t hash_s(const char *s) {
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return h ? h : 1;
}

// 1 if the URL was new (and is now remembered).
static int seen_add(FreeTv *b, const char *url) {
    if ((b->seenN + 1) * 2 > b->seenCap) {
        size_t cap = b->seenCap ? b->seenCap * 2 : 4096;
        uint64_t *s = calloc(cap, sizeof(uint64_t));
        if (!s) return 1;
        for (size_t i = 0; i < b->seenCap; i++) {
            if (!b->seen[i]) continue;
            size_t k = (size_t)b->seen[i] & (cap - 1);
            while (s[k]) k = (k + 1) & (cap - 1);
            s[k] = b->seen[i];
        }
        free(b->seen);
        b->seen = s; b->seenCap = cap;
    }
    uint64_t h = hash_s(url);
    size_t k = (size_t)h & (b->seenCap - 1);
    while (b->seen[k]) {
        if (b->seen[k] == h) return 0;
        k = (k + 1) & (b->seenCap - 1);
    }
    b->seen[k] = h;
    b->seenN++;
    return 1;
}

// "Documentary;News" -> "documentary;news"
static char *lower_dup(const char *s) {
    char *d = strdup(s ? s : "");
    for (char *p = d; p && *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
    return d;
}

static int has_cat(const char *cats, const char *cat) {
    size_t n = strlen(cat);
    for (const char *p = cats; *p;) {
        const char *e = strchr(p, ';');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !memcmp(p, cat, n)) return 1;
        p = e ? e + 1 : p + len;
    }
    return 0;
}

// "El Watania 1 (1080i) [Not 24/7]" -> "El Watania 1 [Not 24/7]": the picture
// size says little on a TV list; the labels say whether it will play.
static char *clean_name(const char *s) {
    char *d = strdup(s ? s : "");
    if (!d) return NULL;
    char *w = d;
    for (const char *p = d; *p;) {
        if (*p == '(' ) {
            const char *q = p + 1;
            int digits = 0;
            while (*q >= '0' && *q <= '9') { q++; digits++; }
            if (digits >= 3 && digits <= 4 && (*q == 'p' || *q == 'i') && q[1] == ')') {
                while (w > d && w[-1] == ' ') w--;
                p = q + 2;
                continue;
            }
        }
        *w++ = *p++;
    }
    *w = '\0';
    return d;
}

typedef struct { FreeTv *b; int whole; } AddCtx;

static int on_entry(void *ctx, const M3uEntry *m) {
    AddCtx *a = ctx;
    FreeTv *b = a->b;
    char *cats = lower_dup(m->group);
    if (!cats) return 1;
    int keep = !has_cat(cats, "xxx");
    if (keep && !a->whole && b->o.nCats > 0) {
        keep = 0;
        for (int i = 0; i < b->o.nCats && !keep; i++) keep = has_cat(cats, b->o.cats[i]);
    }
    if (!keep || !m->spec[0] || !seen_add(b, m->spec)) { free(cats); return 0; }
    if (b->n == b->cap) {
        int cap = b->cap ? b->cap * 2 : 512;
        Entry *ne = realloc(b->e, sizeof(Entry) * (size_t)cap);
        if (!ne) { free(cats); return 1; }
        b->e = ne; b->cap = cap;
    }
    Entry *e = &b->e[b->n];
    memset(e, 0, sizeof(*e));
    e->name = clean_name(m->name);
    e->spec = strdup(m->spec);
    e->tvg = strdup(m->tvgId);
    e->logo = strdup(m->logo);
    e->cats = cats;
    if (!e->name || !e->spec || !e->tvg || !e->logo) {
        free(e->name); free(e->spec); free(e->tvg); free(e->logo); free(cats);
        return 1;
    }
    freetv_country_of(m->tvgId, e->cc);
    e->whole = a->whole;
    e->order = b->n;
    b->n++;
    return 0;
}

void freetv_add(FreeTv *b, const char *text, int whole) {
    AddCtx a = { b, whole };
    if (text) m3u_parse(text, SPEC_MAX, on_entry, &a);
}

// Official free sources, per country. YouTube entries are the broadcaster's
// own channel's /live page (the channel id never changes; the stream does).
static const struct { const char *cc, *m3u; } OFFICIAL[] = {
    { "TN",
      "#EXTM3U\n"
      "#EXTINF:-1 tvg-id=\"HannibalTV.tn@SD\" tvg-logo=\"https://i.imgur.com/sIMmkBo.png\" group-title=\"General\",Hannibal TV\n"
      "https://www.youtube.com/channel/UCMowjs_MJ-oIWEeHUu3DrOQ/live\n"
      "#EXTINF:-1 tvg-id=\"NessmaElJadida.tn@SD\" tvg-logo=\"https://i.imgur.com/66CJtdz.png\" group-title=\"General\",Nessma\n"
      "https://live.nessma.tv/\n"
      "#EXTINF:-1 tvg-id=\"NessmaElJadida.tn@SD\" tvg-logo=\"https://i.imgur.com/66CJtdz.png\" group-title=\"General\",Nessma\n"
      "https://www.youtube.com/channel/UC-48PCT3flS86JkLzxlTA9g/live\n"
      "#EXTINF:-1 tvg-id=\"AttessiaTV.tn@SD\" tvg-logo=\"https://i.imgur.com/kmfRNVy.png\" group-title=\"General\",Attessia TV\n"
      "https://www.youtube.com/channel/UCQS3ejF2jBAhwmbGD9Q3oeA/live\n"
      // Elhiwar Ettounsi: its official YouTube channel (its own live platform,
      // H+ at hplus.tv, needs an account and a browser, so it is not used).
      "#EXTINF:-1 tvg-id=\"ElhiwarEttounsiTV.tn@SD\" tvg-logo=\"https://i.imgur.com/qYG1qLO.png\" group-title=\"General\",Elhiwar Ettounsi\n"
      "https://www.youtube.com/channel/UCXzmMkXaHxMVlutDBD8goHA/live\n"
      // Carthage+: its two official sites (several YouTube channels carry
      // the name; a site that embeds the right one is followed to it).
      "#EXTINF:-1 tvg-id=\"CarthagePlus.tn@SD\" tvg-logo=\"https://i.imgur.com/5BsDW4B.png\" group-title=\"General\",Carthage+\n"
      "http://www.carthageplus.live/\n"
      "#EXTINF:-1 tvg-id=\"CarthagePlus.tn@SD\" tvg-logo=\"https://i.imgur.com/5BsDW4B.png\" group-title=\"General\",Carthage+\n"
      "http://carthageplus.tv/\n" },
};

void freetv_add_official(FreeTv *b) {
    for (int k = 0; k < b->o.nFirst; k++)
        for (size_t i = 0; i < sizeof(OFFICIAL) / sizeof(OFFICIAL[0]); i++)
            if (!strcmp(b->o.first[k], OFFICIAL[i].cc)) freetv_add(b, OFFICIAL[i].m3u, 1);
}

static int ci_cmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y || !x) return x - y;
    }
}

static int entry_cmp(const void *pa, const void *pb) {
    const Entry *a = pa, *b = pb;
    if (a->firstRank != b->firstRank) return a->firstRank - b->firstRank;
    int c = ci_cmp(a->group, b->group);
    if (c) return c;
    c = ci_cmp(a->name, b->name);
    return c ? c : a->order - b->order;
}

static const char *cat_label(const char *id) {
    static const struct { const char *id, *label; } L[] = {
        { "animation", "Cartoons" }, { "kids", "Kids" }, { "documentary", "Documentary" }, { "news", "News" },
        { "movies", "Movies" }, { "series", "Series" }, { "music", "Music" }, { "sports", "Sports" },
        { "religious", "Religious" }, { "general", "General" }, { "entertainment", "Entertainment" },
        { "education", "Education" }, { "culture", "Culture" }, { "comedy", "Comedy" }, { "business", "Business" },
        { "lifestyle", "Lifestyle" }, { "travel", "Travel" }, { "cooking", "Cooking" }, { "classic", "Classic" },
        { "family", "Family" }, { "outdoor", "Outdoor" }, { "science", "Science" }, { "weather", "Weather" },
        { "legislative", "Legislative" }, { "shop", "Shopping" }, { "auto", "Auto" }, { "relax", "Relax" },
        { "public", "Public" }, { "interactive", "Interactive" },
    };
    for (size_t i = 0; i < sizeof(L) / sizeof(L[0]); i++) if (!strcmp(L[i].id, id)) return L[i].label;
    return "Other";
}

// Attribute values go inside double quotes: keep them free of quotes/newlines.
static void put_attr(char **o, const char *s) {
    for (; *s; s++) *(*o)++ = (*s == '"' || *s == '\n' || *s == '\r') ? '\'' : *s;
}

char *freetv_m3u(FreeTv *b, int *count) {
    // Bouquet of each entry: its country ("first" countries keep theirs on
    // top), or with byCategory the first selected category it is in.
    for (int i = 0; i < b->n; i++) {
        Entry *e = &b->e[i];
        e->firstRank = FREETV_MAX_FIRST;
        for (int k = 0; k < b->o.nFirst; k++) if (!strcmp(e->cc, b->o.first[k])) { e->firstRank = k; break; }
        if (b->o.byCategory && e->firstRank == FREETV_MAX_FIRST) {
            const char *id = NULL;
            for (int k = 0; k < b->o.nCats && !id; k++) if (has_cat(e->cats, b->o.cats[k])) id = b->o.cats[k];
            if (!id) {
                static char first[32];
                const char *semi = strchr(e->cats, ';');
                size_t n = semi ? (size_t)(semi - e->cats) : strlen(e->cats);
                if (n >= sizeof(first)) n = sizeof(first) - 1;
                memcpy(first, e->cats, n); first[n] = '\0';
                id = first;
            }
            e->group = cat_label(id);
        } else {
            const char *nm = e->cc[0] ? freetv_country_name(e->cc) : "";
            e->group = nm[0] ? nm : "International";
        }
    }
    qsort(b->e, (size_t)b->n, sizeof(Entry), entry_cmp);
    size_t cap = 16;
    for (int i = 0; i < b->n; i++) {
        const Entry *e = &b->e[i];
        cap += 80 + strlen(e->tvg) + strlen(e->logo) + strlen(e->group) + strlen(e->name) + strlen(e->spec);
    }
    char *out = malloc(cap), *o = out;
    if (!out) return NULL;
    o += sprintf(o, "#EXTM3U\n");
    for (int i = 0; i < b->n; i++) {
        const Entry *e = &b->e[i];
        // A channel listed with several streams: number the extra ones, so
        // a dead link has a neighbour to try.
        int alt = 1;
        for (int k = i - 1; k >= 0 && !ci_cmp(b->e[k].group, e->group) && !ci_cmp(b->e[k].name, e->name); k--) alt++;
        o += sprintf(o, "#EXTINF:-1 tvg-id=\"");
        put_attr(&o, e->tvg);
        o += sprintf(o, "\" tvg-logo=\"");
        put_attr(&o, e->logo);
        o += sprintf(o, "\" group-title=\"");
        put_attr(&o, e->group);
        o += sprintf(o, "\",");
        for (const char *s = e->name; *s; s++) *o++ = (*s == '\n' || *s == '\r') ? ' ' : *s;
        if (alt > 1) o += sprintf(o, " (%d)", alt);
        *o++ = '\n';
        for (const char *s = e->spec; *s; s++) *o++ = (*s == '\n' || *s == '\r') ? ' ' : *s;
        *o++ = '\n';
    }
    *o = '\0';
    if (count) *count = b->n;
    return out;
}
