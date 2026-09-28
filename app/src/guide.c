// guide.c — compact programme-guide store. See guide.h.
#include "guide.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NONE UINT32_MAX

// ---- arena + string maps --------------------------------------------------
typedef struct { char *a; size_t len, cap, max; } Arena;

static uint32_t arena_put(Arena *ar, const char *s, size_t n) {
    if (ar->len + n + 1 > ar->max) return NONE;
    if (ar->len + n + 1 > ar->cap) {
        size_t cap = ar->cap ? ar->cap * 2 : 65536;
        while (cap < ar->len + n + 1) cap *= 2;
        if (cap > ar->max) cap = ar->max;
        char *na = realloc(ar->a, cap);
        if (!na) return NONE;
        ar->a = na; ar->cap = cap;
    }
    uint32_t off = (uint32_t)ar->len;
    memcpy(ar->a + off, s, n);
    ar->a[off + n] = '\0';
    ar->len += n + 1;
    return off;
}

static int arena_init(Arena *ar, size_t max) {
    memset(ar, 0, sizeof(*ar));
    ar->max = max;
    return arena_put(ar, "", 0) == 0 ? 0 : -1;     // offset 0 is ""
}

static uint64_t hash_n(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 1099511628211ull;
    return h;
}

// Open addressing: key = arena offset of a NUL-terminated string (0 = empty slot).
typedef struct { uint32_t *key, *val; size_t cap, n; } Map;

static uint32_t map_get(const Map *m, const Arena *ar, const char *k, size_t kl) {
    if (!m->cap) return NONE;
    size_t i = (size_t)hash_n(k, kl) & (m->cap - 1);
    for (;;) {
        uint32_t o = m->key[i];
        if (!o) return NONE;
        if (memcmp(ar->a + o, k, kl) == 0 && ar->a[o + kl] == '\0') return m->val[i];
        i = (i + 1) & (m->cap - 1);
    }
}

static int map_grow(Map *m, const Arena *ar) {
    size_t cap = m->cap ? m->cap * 2 : 1024;
    uint32_t *key = calloc(cap, sizeof(uint32_t)), *val = malloc(cap * sizeof(uint32_t));
    if (!key || !val) { free(key); free(val); return -1; }
    for (size_t j = 0; j < m->cap; j++) {
        uint32_t o = m->key[j];
        if (!o) continue;
        const char *s = ar->a + o;
        size_t i = (size_t)hash_n(s, strlen(s)) & (cap - 1);
        while (key[i]) i = (i + 1) & (cap - 1);
        key[i] = o; val[i] = m->val[j];
    }
    free(m->key); free(m->val);
    m->key = key; m->val = val; m->cap = cap;
    return 0;
}

// Adds k -> v unless k is present. Returns the value stored for k, or NONE
// when out of memory/arena. keyOff (optional) receives k's arena offset.
static uint32_t map_put(Map *m, Arena *ar, const char *k, size_t kl, uint32_t v, uint32_t *keyOff) {
    if ((m->n + 1) * 2 > m->cap && map_grow(m, ar) != 0) return NONE;
    size_t i = (size_t)hash_n(k, kl) & (m->cap - 1);
    for (;;) {
        uint32_t o = m->key[i];
        if (!o) break;
        if (memcmp(ar->a + o, k, kl) == 0 && ar->a[o + kl] == '\0') {
            if (keyOff) *keyOff = o;
            return m->val[i];
        }
        i = (i + 1) & (m->cap - 1);
    }
    uint32_t o = arena_put(ar, k, kl);
    if (o == NONE) return NONE;
    m->key[i] = o; m->val[i] = v; m->n++;
    if (keyOff) *keyOff = o;
    return v;
}

static void map_free(Map *m) { free(m->key); free(m->val); memset(m, 0, sizeof(*m)); }

static void lower_copy(const char *s, char *out, int cap) {
    int n = 0;
    for (; s[n] && n < cap - 1; n++) out[n] = (s[n] >= 'A' && s[n] <= 'Z') ? (char)(s[n] + 32) : s[n];
    out[n] = '\0';
}

// ---- name keys -----------------------------------------------------------------
void guide_name_key(const char *name, char *out, int cap) {
    static const char *const DROP[] = { "hd", "fhd", "uhd", "sd", "4k", "8k", "hevc", "h264", "h265", "hdr",
                                        "1080p", "1080i", "720p", "576p", "50fps", "60fps", NULL };
    char low[256];
    lower_copy(name ? name : "", low, sizeof(low));
    const char *s = low;
    while (*s == ' ') s++;
    // "UK: BBC One", "FR | TF1", "US - CNN": a short letter tag before a separator.
    int k = 0;
    while (s[k] >= 'a' && s[k] <= 'z') k++;
    if (k >= 2 && k <= 4) {
        int j = k;
        while (s[j] == ' ') j++;
        if ((s[j] == ':' || s[j] == '|' || (s[j] == '-' && s[j + 1] == ' ')) && s[j + 1]) s += j + 1;
    }
    int n = 0, depth = 0;
    char tok[64];
    int tl = 0;
    for (const char *p = s;; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '(' || ch == '[' || ch == '{') { depth++; continue; }
        if (ch == ')' || ch == ']' || ch == '}') { if (depth) depth--; continue; }
        int word = ch && ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '+' || ch >= 0x80);
        if (word && !depth) { if (tl < (int)sizeof(tok) - 1) tok[tl++] = (char)ch; continue; }
        if (word) continue;
        if (tl) {
            tok[tl] = '\0';
            int drop = 0;
            for (int d = 0; DROP[d]; d++) if (strcmp(tok, DROP[d]) == 0) { drop = 1; break; }
            for (int i = 0; !drop && i < tl && n < cap - 1; i++) out[n++] = tok[i];
            tl = 0;
        }
        if (!ch) break;
    }
    out[n] = '\0';
}

// ---- what the playlist wants ---------------------------------------------------
struct GuideWant {
    Arena    ar;
    Map      ids, names;
    uint64_t sig;
};

GuideWant *guide_want_new(void) {
    GuideWant *w = calloc(1, sizeof(*w));
    if (w && arena_init(&w->ar, (size_t)64 << 20) != 0) { free(w); return NULL; }
    if (w) w->sig = 1469598103934665603ull;
    return w;
}

void guide_want_add(GuideWant *w, const char *tvgId, const char *name) {
    char key[256];
    lower_copy(tvgId ? tvgId : "", key, sizeof(key));
    if (key[0]) map_put(&w->ids, &w->ar, key, strlen(key), 1, NULL);
    w->sig = (w->sig ^ hash_n(key, strlen(key))) * 1099511628211ull;
    guide_name_key(name, key, sizeof(key));
    if (key[0]) map_put(&w->names, &w->ar, key, strlen(key), 1, NULL);
    w->sig = (w->sig ^ hash_n(key, strlen(key))) * 1099511628211ull;
}

uint64_t guide_want_sig(const GuideWant *w) { return w->sig; }

void guide_want_free(GuideWant *w) {
    if (!w) return;
    map_free(&w->ids); map_free(&w->names);
    free(w->ar.a);
    free(w);
}

// ---- the guide -------------------------------------------------------------------
typedef struct { uint32_t id, first, count; } GChan;
typedef struct { uint32_t start, stop, title, sub, cat, desc, gch; } Prog;

struct Guide {
    const GuideWant *w;
    Arena   ar;
    Map     strs, ids, names;
    GChan  *ch;
    int     chN, chCap;
    Prog   *pg;
    int     pgN, pgCap;
    int64_t now, lo, hi;
    int     truncated, finished;
};

Guide *guide_new(const GuideWant *w, int64_t now) {
    Guide *g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    if (arena_init(&g->ar, GUIDE_MAX_ARENA) != 0) { free(g); return NULL; }
    g->w = w;
    g->now = now;
    g->lo = now - GUIDE_BACK_SEC;
    g->hi = now + GUIDE_AHEAD_SEC;
    return g;
}

void guide_free(Guide *g) {
    if (!g) return;
    map_free(&g->strs); map_free(&g->ids); map_free(&g->names);
    free(g->ar.a); free(g->ch); free(g->pg);
    free(g);
}

static int add_chan(Guide *g, const char *lowId) {
    if (g->chN == g->chCap) {
        int cap = g->chCap ? g->chCap * 2 : 256;
        GChan *nc = realloc(g->ch, sizeof(GChan) * (size_t)cap);
        if (!nc) return -1;
        g->ch = nc; g->chCap = cap;
    }
    uint32_t off;
    if (map_put(&g->ids, &g->ar, lowId, strlen(lowId), (uint32_t)g->chN, &off) != (uint32_t)g->chN) return -1;
    g->ch[g->chN].id = off;
    g->ch[g->chN].first = g->ch[g->chN].count = 0;
    return g->chN++;
}

int guide_on_channel(void *ctx, const XmltvChannel *c) {
    Guide *g = ctx;
    char id[256], nk[256];
    lower_copy(c->id, id, sizeof(id));
    if (!id[0] || map_get(&g->ids, &g->ar, id, strlen(id)) != NONE) return 0;
    int wanted = map_get(&g->w->ids, &g->w->ar, id, strlen(id)) != NONE;
    for (int i = 0; i < c->nameCount && !wanted; i++) {
        guide_name_key(c->names[i], nk, sizeof(nk));
        if (nk[0] && map_get(&g->w->names, &g->w->ar, nk, strlen(nk)) != NONE) wanted = 1;
    }
    if (!wanted) return 0;
    int gch = add_chan(g, id);
    if (gch < 0) { g->truncated = 1; return 0; }
    for (int i = 0; i < c->nameCount; i++) {
        guide_name_key(c->names[i], nk, sizeof(nk));
        if (nk[0]) map_put(&g->names, &g->ar, nk, strlen(nk), (uint32_t)gch, NULL);   // first channel wins
    }
    return 0;
}

static uint32_t intern(Guide *g, const char *s, size_t maxLen) {
    size_t n = strlen(s);
    if (!n) return 0;
    if (n > maxLen) {                                  // clip on a UTF-8 boundary
        n = maxLen;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    }
    uint32_t off;
    return map_put(&g->strs, &g->ar, s, n, 0, &off) == NONE ? NONE : off;
}

int guide_on_programme(void *ctx, const XmltvProgramme *p) {
    Guide *g = ctx;
    char id[256];
    lower_copy(p->channel, id, sizeof(id));
    uint32_t gch = map_get(&g->ids, &g->ar, id, strlen(id));
    if (gch == NONE) {
        // Guides that never declare a <channel> for an id still count when the
        // playlist names that id.
        if (!id[0] || map_get(&g->w->ids, &g->w->ar, id, strlen(id)) == NONE) return 0;
        int n = add_chan(g, id);
        if (n < 0) { g->truncated = 1; return 0; }
        gch = (uint32_t)n;
    }
    int64_t stop = p->stop > p->start ? p->stop : p->start + 4 * 3600;
    if (p->start >= g->hi || stop <= g->lo || p->start <= 0 || p->start > 0xFFFFFFFFll) return 0;
    if (g->pgN >= GUIDE_MAX_PROGS) { g->truncated = 1; return 0; }
    if (g->pgN == g->pgCap) {
        int cap = g->pgCap ? g->pgCap * 2 : 4096;
        if (cap > GUIDE_MAX_PROGS) cap = GUIDE_MAX_PROGS;
        Prog *np = realloc(g->pg, sizeof(Prog) * (size_t)cap);
        if (!np) { g->truncated = 1; return 0; }
        g->pg = np; g->pgCap = cap;
    }
    Prog *q = &g->pg[g->pgN];
    q->gch = gch;
    q->start = (uint32_t)p->start;
    q->stop = p->stop > p->start && p->stop <= 0xFFFFFFFFll ? (uint32_t)p->stop : 0;
    q->title = intern(g, p->title, XMLTV_TEXT_MAX);
    q->sub = intern(g, p->subTitle, XMLTV_TEXT_MAX);
    q->cat = intern(g, p->category, 64);
    // Descriptions are the bulk: past 3/4 of the arena, keep titles only.
    q->desc = g->ar.len < GUIDE_MAX_ARENA / 4 * 3 ? intern(g, p->desc, GUIDE_DESC_KEEP) : 0;
    if (q->title == NONE) { g->truncated = 1; return 0; }
    if (q->sub == NONE) q->sub = 0;
    if (q->cat == NONE) q->cat = 0;
    if (q->desc == NONE) { q->desc = 0; g->truncated = 1; }
    g->pgN++;
    return 0;
}

static int prog_cmp(const void *a, const void *b) {
    const Prog *x = a, *y = b;
    if (x->gch != y->gch) return x->gch < y->gch ? -1 : 1;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return 0;
}

void guide_finish(Guide *g) {
    if (g->finished) return;
    g->finished = 1;
    // qsort is not stable: equal (channel, start) keep whichever sorts first,
    // which is fine for duplicates a guide lists twice.
    qsort(g->pg, (size_t)g->pgN, sizeof(Prog), prog_cmp);
    int w = 0;
    for (int i = 0; i < g->pgN; i++) {
        if (w > 0 && g->pg[w - 1].gch == g->pg[i].gch && g->pg[w - 1].start == g->pg[i].start) continue;
        g->pg[w++] = g->pg[i];
    }
    g->pgN = w;
    map_free(&g->strs);                                  // interning is over
    if (g->pgN && g->pgN < g->pgCap) {                   // give back the slack
        Prog *np = realloc(g->pg, sizeof(Prog) * (size_t)g->pgN);
        if (np) { g->pg = np; g->pgCap = g->pgN; }
    }
    if (g->ar.len < g->ar.cap) {
        char *na = realloc(g->ar.a, g->ar.len);
        if (na) { g->ar.a = na; g->ar.cap = g->ar.len; }
    }
    for (int i = 0; i < g->pgN; i++) {
        Prog *q = &g->pg[i];
        GChan *c = &g->ch[q->gch];
        if (!c->count) c->first = (uint32_t)i;
        c->count++;
        if (!q->stop) {                                  // no stop: until the next one
            const Prog *nx = i + 1 < g->pgN && g->pg[i + 1].gch == q->gch ? &g->pg[i + 1] : NULL;
            q->stop = nx ? nx->start : q->start + 3600;
        }
    }
    g->w = NULL;
}

int guide_find(const Guide *g, const char *tvgId, const char *name) {
    char key[256];
    lower_copy(tvgId ? tvgId : "", key, sizeof(key));
    uint32_t v = key[0] ? map_get(&g->ids, &g->ar, key, strlen(key)) : NONE;
    if (v == NONE) {
        guide_name_key(name, key, sizeof(key));
        if (key[0]) v = map_get(&g->names, &g->ar, key, strlen(key));
    }
    return v == NONE ? -1 : (int)v;
}

int guide_count(const Guide *g, int gch) {
    return gch >= 0 && gch < g->chN ? (int)g->ch[gch].count : 0;
}

int guide_prog(const Guide *g, int gch, int k, GuideProg *out) {
    if (k < 0 || k >= guide_count(g, gch)) return 0;
    const Prog *q = &g->pg[g->ch[gch].first + (uint32_t)k];
    out->start = q->start; out->stop = q->stop;
    out->title = g->ar.a + q->title; out->sub = g->ar.a + q->sub;
    out->cat = g->ar.a + q->cat; out->desc = g->ar.a + q->desc;
    return 1;
}

int guide_at(const Guide *g, int gch, int64_t t, int *onAir) {
    int n = guide_count(g, gch);
    if (onAir) *onAir = 0;
    if (!n) return -1;
    const Prog *p = &g->pg[g->ch[gch].first];
    int lo = 0, hi = n;                                  // first with start > t
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if ((int64_t)p[mid].start <= t) lo = mid + 1; else hi = mid;
    }
    if (lo > 0 && (int64_t)p[lo - 1].stop > t) { if (onAir) *onAir = 1; return lo - 1; }
    return lo < n ? lo : -1;
}

long guide_channels(const Guide *g) { return g->chN; }
long guide_programmes(const Guide *g) { return g->pgN; }
size_t guide_bytes(const Guide *g) {
    return g->ar.cap + sizeof(Prog) * (size_t)g->pgCap + sizeof(GChan) * (size_t)g->chCap +
           (g->strs.cap + g->ids.cap + g->names.cap) * 2 * sizeof(uint32_t);
}
int guide_truncated(const Guide *g) { return g->truncated; }
int64_t guide_built_at(const Guide *g) { return g->now; }
