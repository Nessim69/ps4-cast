#include "subs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- cue list ----------------------------------------------------------------
void subs_list_init(SubList *l) { l->cues = NULL; l->n = l->cap = 0; }

static void cue_free(SubCue *c) { free(c->text); free(c->argb); c->text = NULL; c->argb = NULL; }

void subs_list_clear(SubList *l) {
    for (int i = 0; i < l->n; i++) cue_free(&l->cues[i]);
    l->n = 0;
}

void subs_list_free(SubList *l) {
    subs_list_clear(l);
    free(l->cues);
    subs_list_init(l);
}

static int same_cue(const SubCue *a, const SubCue *b) {
    if (a->startUs != b->startUs) return 0;
    if (a->text || b->text) return a->text && b->text && strcmp(a->text, b->text) == 0;
    return a->argb && b->argb && a->w == b->w && a->h == b->h && a->x == b->x && a->y == b->y;
}

int subs_list_add(SubList *l, SubCue *c) {
    // Sorted by start. Embedded tracks re-deliver cues after a seek: skip a
    // cue we already hold.
    int at = l->n;
    while (at > 0 && l->cues[at - 1].startUs > c->startUs) at--;
    for (int i = at - 1; i >= 0 && l->cues[i].startUs == c->startUs; i--)
        if (same_cue(&l->cues[i], c)) { cue_free(c); return 0; }
    if (l->n == l->cap) {
        int ncap = l->cap ? l->cap * 2 : 64;
        SubCue *nc = realloc(l->cues, sizeof(SubCue) * (size_t)ncap);
        if (!nc) { cue_free(c); return -1; }
        l->cues = nc; l->cap = ncap;
    }
    // An open-ended cue (PGS/DVB: shown until the next event) ends where a
    // LATER one starts; regions of the same event (same start) stay up together.
    for (int i = at - 1; i >= 0; i--)
        if (l->cues[i].endUs == SUB_END_OPEN && l->cues[i].startUs < c->startUs) l->cues[i].endUs = c->startUs;
    memmove(&l->cues[at + 1], &l->cues[at], sizeof(SubCue) * (size_t)(l->n - at));
    l->cues[at] = *c;
    l->n++;
    return 0;
}

int subs_list_active(const SubList *l, int64_t t, const SubCue **out, int max) {
    int k = 0;
    for (int i = 0; i < l->n && k < max; i++) {
        const SubCue *c = &l->cues[i];
        if (c->startUs > t) break;
        if (t < c->endUs) out[k++] = c;
    }
    return k;
}

void subs_list_prune(SubList *l, int64_t t) {
    int w = 0;
    for (int i = 0; i < l->n; i++) {
        if (l->cues[i].endUs < t) { cue_free(&l->cues[i]); continue; }
        l->cues[w++] = l->cues[i];
    }
    l->n = w;
}

void subs_list_end_open(SubList *l, int64_t t) {
    for (int i = 0; i < l->n; i++)
        if (l->cues[i].endUs == SUB_END_OPEN && l->cues[i].startUs < t) l->cues[i].endUs = t;
}

int subs_list_take_all(SubList *dst, SubList *src) {
    int moved = 0;
    for (int i = 0; i < src->n; i++) {
        SubCue c = src->cues[i];
        if (subs_list_add(dst, &c) == 0) moved++;
    }
    src->n = 0;               // ownership went to dst (or was freed by a failed add)
    return moved;
}

// ---- text clean-up -------------------------------------------------------------
static int is_tag_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '.';
}

void subs_clean(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '{' && r[1] == '\\') {                  // ASS override block
            char *e = strchr(r, '}');
            if (e) { r = e + 1; continue; }
        }
        if (r[0] == '\\' && (r[1] == 'N' || r[1] == 'n')) { *w++ = '\n'; r += 2; continue; }
        if (r[0] == '\\' && r[1] == 'h') { *w++ = ' '; r += 2; continue; }
        if (r[0] == '<' && is_tag_start(r[1])) {            // <i>, </font>, <c.x>, <v Bob>, <00:01.000>
            char *e = strchr(r, '>');
            if (e) { r = e + 1; continue; }
        }
        if (r[0] == '&') {
            static const struct { const char *e; char c; } ent[] = {
                { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&nbsp;", ' ' },
                { "&quot;", '"' }, { "&apos;", '\'' }, { "&#39;", '\'' }, { "&lrm;", 0 }, { "&rlm;", 0 } };
            int hit = 0;
            for (unsigned i = 0; i < sizeof(ent) / sizeof(ent[0]); i++) {
                size_t n = strlen(ent[i].e);
                if (strncmp(r, ent[i].e, n) == 0) {
                    if (ent[i].c) *w++ = ent[i].c;
                    r += n; hit = 1; break;
                }
            }
            if (hit) continue;
        }
        if (*r == '\r') { r++; continue; }
        *w++ = *r++;
    }
    *w = '\0';
    // Trim every line, drop empty ones.
    r = w = s;
    while (*r) {
        char *e = strchr(r, '\n');
        size_t n = e ? (size_t)(e - r) : strlen(r);
        while (n > 0 && (*r == ' ' || *r == '\t')) { r++; n--; }
        while (n > 0 && (r[n - 1] == ' ' || r[n - 1] == '\t')) n--;
        if (n > 0) {
            if (w != s) *w++ = '\n';
            memmove(w, r, n);
            w += n;
        }
        if (!e) break;
        r = e + 1;
    }
    *w = '\0';
}

static char *dup_clean(const char *s, int len) {
    if (len <= 0) return NULL;
    char *t = malloc((size_t)len + 1);
    if (!t) return NULL;
    memcpy(t, s, (size_t)len);
    t[len] = '\0';
    subs_clean(t);
    if (!t[0]) { free(t); return NULL; }
    return t;
}

char *subs_text_plain(const char *s, int len) { return dup_clean(s, len); }

char *subs_text_ass(const char *s, int len) {
    // Matroska: "ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text"
    // (8 fields before Text); a raw "Dialogue: Layer,Start,End,..." line has 9.
    int skip = 8;
    if (len > 9 && strncmp(s, "Dialogue:", 9) == 0) { s += 9; len -= 9; skip = 9; }
    int i = 0;
    for (int commas = 0; i < len && commas < skip; i++)
        if (s[i] == ',') commas++;
    return dup_clean(s + i, len - i);
}

char *subs_text_movtext(const uint8_t *p, int len) {
    if (len < 2) return NULL;
    int n = p[0] << 8 | p[1];
    if (n > len - 2) n = len - 2;
    return dup_clean((const char *)p + 2, n);
}

// ---- encodings -----------------------------------------------------------------
static int valid_utf8(const unsigned char *p) {
    while (*p) {
        if (*p < 0x80) { p++; continue; }
        int n = (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0 || (*p == 0xC0 || *p == 0xC1)) return 0;
        for (int i = 1; i <= n; i++) if ((p[i] & 0xC0) != 0x80) return 0;
        p += n + 1;
    }
    return 1;
}

char *subs_to_utf8(const char *s) {
    if (!s || valid_utf8((const unsigned char *)s)) return NULL;
    // Windows-1252 0x80..0x9F (0 = undefined there); the rest is Latin-1.
    static const unsigned short cp1252[32] = {
        0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
        0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };
    size_t n = strlen(s);
    char *out = malloc(n * 3 + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned cp = *p;
        if (cp >= 0x80 && cp < 0xA0) cp = cp1252[cp - 0x80] ? cp1252[cp - 0x80] : '?';
        if (cp < 0x80) out[o++] = (char)cp;
        else if (cp < 0x800) { out[o++] = (char)(0xC0 | cp >> 6); out[o++] = (char)(0x80 | (cp & 0x3F)); }
        else { out[o++] = (char)(0xE0 | cp >> 12); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
    }
    out[o] = '\0';
    return out;
}

// ---- DLNA metadata ------------------------------------------------------------
static int copy_url(const char *s, int n, char *out, int cap) {
    while (n > 0 && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) { s++; n--; }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) n--;
    if (n < 8 || (strncmp(s, "http://", 7) != 0 && strncmp(s, "https://", 8) != 0)) return 0;
    int o = 0;
    for (int i = 0; i < n && o < cap - 1; i++) {
        if (strncmp(s + i, "&amp;", 5) == 0) { out[o++] = '&'; i += 4; continue; }
        out[o++] = s[i];
    }
    out[o] = '\0';
    return 1;
}

static const char *ci_find(const char *h, const char *n) {
    size_t nl = strlen(n);
    for (; *h; h++) {
        size_t i = 0;
        while (i < nl && h[i] && ((h[i] | 32) == (n[i] | 32))) i++;
        if (i == nl) return h;
    }
    return NULL;
}

int subs_didl_url(const char *d, char *out, int cap) {
    if (!d) return 0;
    for (const char *p = strstr(d, "CaptionInfo"); p; p = strstr(p + 1, "CaptionInfo")) {
        if (p == d || (p[-1] != ':' && p[-1] != '<')) continue;        // <sec:CaptionInfo...> / <CaptionInfo>
        const char *gt = strchr(p, '>'), *lt = gt ? strchr(gt, '<') : NULL;
        if (gt && lt && copy_url(gt + 1, (int)(lt - gt - 1), out, cap)) return 1;
    }
    for (const char *p = strstr(d, "<res"); p; p = strstr(p + 4, "<res")) {
        const char *gt = strchr(p, '>');
        if (!gt) break;
        char attrs[512];
        int al = (int)(gt - p) < (int)sizeof(attrs) - 1 ? (int)(gt - p) : (int)sizeof(attrs) - 1;
        memcpy(attrs, p, (size_t)al); attrs[al] = '\0';
        if (!ci_find(attrs, "text/srt") && !ci_find(attrs, "text/vtt") && !ci_find(attrs, "subrip") &&
            !ci_find(attrs, "webvtt")) continue;
        const char *e = strstr(gt, "</res>");
        if (e && copy_url(gt + 1, (int)(e - gt - 1), out, cap)) return 1;
    }
    return 0;
}

// ---- file parsing --------------------------------------------------------------
// "[[h]h:]mm:ss[.,]mmm" -> microseconds; advances *pp. -1 if malformed.
static int64_t parse_ts(const char **pp) {
    const char *p = *pp;
    while (*p == ' ' || *p == '\t') p++;
    int64_t parts[3] = { 0, 0, 0 };
    int np = 0;
    for (;;) {
        if (*p < '0' || *p > '9') return -1;
        int64_t v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        if (np < 3) parts[np++] = v;
        if (*p == ':') { p++; continue; }
        break;
    }
    if (np < 2) return -1;
    int64_t ms = 0;
    if (*p == ',' || *p == '.') {
        p++;
        int digits = 0;
        while (*p >= '0' && *p <= '9') { if (digits < 3) ms = ms * 10 + (*p - '0'); digits++; p++; }
        while (digits > 0 && digits < 3) { ms *= 10; digits++; }
    }
    int64_t h = np == 3 ? parts[0] : 0, m = parts[np - 2], s = parts[np - 1];
    *pp = p;
    return ((h * 3600 + m * 60 + s) * 1000 + ms) * 1000;
}

// Next line of `text` into line[] (no CR/LF). NULL at the end.
static const char *next_line(const char *p, char *line, int cap) {
    if (!p || !*p) return NULL;
    const char *e = strchr(p, '\n');
    int n = e ? (int)(e - p) : (int)strlen(p);
    int c = n < cap - 1 ? n : cap - 1;
    memcpy(line, p, (size_t)c);
    line[c] = '\0';
    if (c > 0 && line[c - 1] == '\r') line[c - 1] = '\0';
    return e ? e + 1 : p + n;
}

static int add_text_cue(SubList *l, int64_t st, int64_t en, const char *text, int len) {
    char *t = dup_clean(text, len);
    if (!t || en <= st) { free(t); return 0; }
    SubCue c = { st, en, t, NULL, 0, 0, 0, 0, 0, 0 };
    return subs_list_add(l, &c) == 0 ? 1 : 0;
}

static int parse_file_utf8(SubList *l, const char *text, int64_t offsetUs);

int subs_parse_file(SubList *l, const char *text, int64_t offsetUs) {
    if (!text) return -1;
    char *conv = subs_to_utf8(text);
    int rc = parse_file_utf8(l, conv ? conv : text, offsetUs);
    free(conv);
    return rc;
}

static int parse_file_utf8(SubList *l, const char *text, int64_t offsetUs) {
    if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) text += 3;
    const char *p = text;
    while (*p == '\r' || *p == '\n' || *p == ' ') p++;
    int vtt = strncmp(p, "WEBVTT", 6) == 0;
    char line[2048];
    int64_t map = 0;
    if (vtt) {
        // Header: up to the first blank line. X-TIMESTAMP-MAP=MPEGTS:n,LOCAL:ts
        // (either order) maps cue time LOCAL to MPEG-TS time n (90 kHz).
        const char *q = p;
        while ((q = next_line(q, line, sizeof(line))) != NULL && line[0]) {
            const char *m = strstr(line, "X-TIMESTAMP-MAP=");
            if (!m) continue;
            const char *mt = strstr(m, "MPEGTS:"), *lo = strstr(m, "LOCAL:");
            if (mt && lo) {
                long long ts = strtoll(mt + 7, NULL, 10);
                const char *lp = lo + 6;
                int64_t local = parse_ts(&lp);
                if (local >= 0) map = ts * 1000000LL / 90000 - local;
            }
        }
        p = q;
    }
    int added = 0, sawTiming = 0;
    const char *q = p;
    char *buf = malloc(8192);
    if (!buf) return -1;
    while (q) {
        // Find the timing line of the next block.
        const char *arrow = NULL;
        const char *ln;
        while ((ln = next_line(q, line, sizeof(line))) != NULL) {
            q = ln;
            if (vtt && (strncmp(line, "NOTE", 4) == 0 || strncmp(line, "STYLE", 5) == 0 ||
                        strncmp(line, "REGION", 6) == 0)) {
                while ((ln = next_line(q, line, sizeof(line))) != NULL && line[0]) q = ln;   // skip block
                if (ln) q = ln;
                continue;
            }
            if ((arrow = strstr(line, "-->")) != NULL) break;
        }
        if (!ln) break;
        const char *sp = line, *ep = arrow + 3;
        int64_t st = parse_ts(&sp), en = parse_ts(&ep);
        if (st < 0 || en < 0) continue;
        sawTiming = 1;
        int bl = 0;
        while ((ln = next_line(q, line, sizeof(line))) != NULL) {
            q = ln;
            if (!line[0]) break;
            int n = (int)strlen(line);
            if (bl + n + 2 >= 8192) break;
            if (bl) buf[bl++] = '\n';
            memcpy(buf + bl, line, (size_t)n); bl += n;
        }
        added += add_text_cue(l, st + map + offsetUs, en + map + offsetUs, buf, bl);
        if (!ln) break;
    }
    free(buf);
    return (vtt || sawTiming) ? added : -1;
}

// ---- wrapping ------------------------------------------------------------------
int subs_wrap(const char *text, int maxW, int maxLines, int (*width)(const char *s, void *ctx),
              void *ctx, char *out, int cap) {
    int lines = 0, o = 0;
    char cur[512];
    int cl = 0;
    out[0] = '\0';
    #define FLUSH() do { \
        if (cl > 0 && lines < maxLines) { \
            if (lines && o < cap - 1) out[o++] = '\n'; \
            int k = cl < cap - 1 - o ? cl : cap - 1 - o; \
            memcpy(out + o, cur, (size_t)k); o += k; out[o] = '\0'; lines++; \
        } cl = 0; } while (0)
    const char *p = text;
    while (*p) {
        if (*p == '\n') { FLUSH(); p++; continue; }
        if (*p == ' ') { p++; continue; }
        const char *e = p;
        while (*e && *e != ' ' && *e != '\n') e++;
        int wl = (int)(e - p);
        char word[256];
        int wk = wl < (int)sizeof(word) - 1 ? wl : (int)sizeof(word) - 1;
        memcpy(word, p, (size_t)wk); word[wk] = '\0';
        if (cl > 0) {
            char trial[512];
            int tl = snprintf(trial, sizeof(trial), "%.*s %s", cl, cur, word);
            if (tl < (int)sizeof(trial) && width(trial, ctx) <= maxW) {
                memcpy(cur, trial, (size_t)tl); cl = tl;
                p = e; continue;
            }
            FLUSH();
        }
        cl = wk < (int)sizeof(cur) - 1 ? wk : (int)sizeof(cur) - 1;
        memcpy(cur, word, (size_t)cl);
        p = e;
    }
    FLUSH();
    #undef FLUSH
    return lines;
}
