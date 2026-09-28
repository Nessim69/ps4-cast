// xmltv.c — push-style XMLTV parser. See xmltv.h.
//
// A small XML tokenizer (text, tags with quoted attributes, comments, CDATA,
// processing instructions, DOCTYPE with an internal subset) that keeps only
// the elements a guide needs. State survives between pushes at any byte.
#include "xmltv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG_MAX 4096
#define RAW_MAX (XMLTV_DESC_MAX * 2 + 64)

enum { S_TEXT, S_LT, S_TAG, S_BANG, S_COMMENT, S_CDATA, S_DOCTYPE, S_PI };
enum { C_NONE, C_NAME, C_TITLE, C_SUB, C_CAT, C_DESC };

struct Xmltv {
    XmltvCallbacks cb;
    int      state;
    int      sawTv, stopped;
    long     pushed;
    long     nChannels, nProgrammes;
    // tag being read
    char     tag[TAG_MAX];
    int      tagLen;
    char     quote;
    char     bang[8];
    int      bangLen;
    int      tail;                // last chars seen, for "-->", "]]>", "?>"
    int      dtDepth;             // DOCTYPE [ ] nesting
    // captured character data of the element named in capName
    int      cap;
    char     capName[16];
    char     raw[RAW_MAX];
    int      rawLen;
    // element being built
    int      inChannel, inProgramme;
    char     chId[256];
    char     names[XMLTV_MAX_NAMES][XMLTV_TEXT_MAX];
    int      nameCount;
    char     icon[512];
    char     pChan[256];
    char     pStart[64], pStop[64];
    char     title[XMLTV_TEXT_MAX], sub[XMLTV_TEXT_MAX], cat[128];
    char     desc[XMLTV_DESC_MAX];
};

Xmltv *xmltv_new(const XmltvCallbacks *cb) {
    Xmltv *x = calloc(1, sizeof(*x));
    if (x && cb) x->cb = *cb;
    return x;
}

void xmltv_free(Xmltv *x) { free(x); }
long xmltv_channels(const Xmltv *x) { return x->nChannels; }
long xmltv_programmes(const Xmltv *x) { return x->nProgrammes; }

// ---- text helpers -----------------------------------------------------------
static int put_utf8(char *o, int cap, int n, unsigned cp) {
    char b[4]; int k;
    if (cp < 0x80) { b[0] = (char)cp; k = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | cp >> 6); b[1] = (char)(0x80 | (cp & 63)); k = 2; }
    else if (cp < 0x10000) { b[0] = (char)(0xE0 | cp >> 12); b[1] = (char)(0x80 | (cp >> 6 & 63)); b[2] = (char)(0x80 | (cp & 63)); k = 3; }
    else if (cp < 0x110000) {
        b[0] = (char)(0xF0 | cp >> 18); b[1] = (char)(0x80 | (cp >> 12 & 63));
        b[2] = (char)(0x80 | (cp >> 6 & 63)); b[3] = (char)(0x80 | (cp & 63)); k = 4;
    } else return n;
    if (n + k >= cap) return n;
    memcpy(o + n, b, (size_t)k);
    return n + k;
}

// Decode entities and collapse whitespace runs into one space (trimmed).
static void decode(const char *s, int len, char *out, int cap) {
    int n = 0, space = 0;
    if (cap <= 0) return;
    for (int i = 0; i < len && n < cap - 1; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') { space = n > 0; continue; }
        if (space) { out[n++] = ' '; space = 0; if (n >= cap - 1) break; }
        if (ch == '&') {
            int j = i + 1;
            while (j < len && j - i < 12 && s[j] != ';') j++;
            if (j < len && s[j] == ';') {
                const char *e = s + i + 1;
                int el = j - i - 1;
                unsigned cp = 0; int ok = 1;
                if (el == 3 && !memcmp(e, "amp", 3)) cp = '&';
                else if (el == 2 && !memcmp(e, "lt", 2)) cp = '<';
                else if (el == 2 && !memcmp(e, "gt", 2)) cp = '>';
                else if (el == 4 && !memcmp(e, "quot", 4)) cp = '"';
                else if (el == 4 && !memcmp(e, "apos", 4)) cp = '\'';
                else if (el == 4 && !memcmp(e, "nbsp", 4)) cp = 0xA0;
                else if (el >= 2 && e[0] == '#') {
                    int hex = e[1] == 'x' || e[1] == 'X';
                    ok = el > (hex ? 2 : 1);
                    for (int k = hex ? 2 : 1; k < el && ok; k++) {
                        char d = e[k];
                        unsigned v = d >= '0' && d <= '9' ? (unsigned)(d - '0')
                                   : hex && d >= 'a' && d <= 'f' ? (unsigned)(d - 'a' + 10)
                                   : hex && d >= 'A' && d <= 'F' ? (unsigned)(d - 'A' + 10) : 99;
                        if (v > (hex ? 15u : 9u) || cp > 0x10FFFF) ok = 0;
                        else cp = cp * (hex ? 16 : 10) + v;
                    }
                    if (cp == 0) ok = 0;
                } else ok = 0;
                if (ok) { n = put_utf8(out, cap, n, cp); i = j; continue; }
            }
        }
        out[n++] = (char)ch;
    }
    out[n] = '\0';
}

// Attribute `name` of a tag's text ("programme start=\"..\" ..."), decoded.
static int attr(const char *tag, const char *name, char *out, int cap) {
    size_t nl = strlen(name);
    const char *p = tag;
    out[0] = '\0';
    while ((p = strstr(p, name)) != NULL) {
        int wordStart = p > tag && (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r');
        const char *q = p + nl;
        p += nl;
        if (!wordStart) continue;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q != '=') continue;
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        char qc = *q;
        if (qc != '"' && qc != '\'') continue;
        const char *v = ++q;
        while (*q && *q != qc) q++;
        decode(v, (int)(q - v), out, cap);
        return 1;
    }
    return 0;
}

// ---- time -------------------------------------------------------------------
static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t xmltv_time(const char *s) {
    int dig[14], n = 0;
    while (*s == ' ') s++;
    while (n < 14 && *s >= '0' && *s <= '9') dig[n++] = *s++ - '0';
    if (n < 8) return 0;
    while (*s >= '0' && *s <= '9') s++;            // fractional/extra digits
    for (int i = n; i < 14; i++) dig[i] = 0;
    int y = dig[0] * 1000 + dig[1] * 100 + dig[2] * 10 + dig[3];
    int mo = dig[4] * 10 + dig[5], d = dig[6] * 10 + dig[7];
    int h = dig[8] * 10 + dig[9], mi = dig[10] * 10 + dig[11], se = dig[12] * 10 + dig[13];
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return 0;
    int64_t t = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
    while (*s == ' ') s++;
    if ((*s == '+' || *s == '-') && s[1] >= '0' && s[1] <= '9') {
        int sign = *s == '-' ? -1 : 1, z[4] = { 0 }, k = 0;
        s++;
        while (k < 4 && *s >= '0' && *s <= '9') z[k++] = *s++ - '0';
        if (*s == ':' && k == 2) { s++; while (k < 4 && *s >= '0' && *s <= '9') z[k++] = *s++ - '0'; }
        int off = (z[0] * 10 + z[1]) * 3600 + (z[2] * 10 + z[3]) * 60;
        t -= sign * off;
    }
    return t;
}

// ---- elements -----------------------------------------------------------------
static void cap_start(Xmltv *x, int what, const char *name) {
    x->cap = what;
    snprintf(x->capName, sizeof(x->capName), "%s", name);
    x->rawLen = 0;
}

static void cap_end(Xmltv *x) {
    char *dst = NULL; int dcap = 0;
    switch (x->cap) {
    case C_NAME:
        if (x->nameCount < XMLTV_MAX_NAMES) { dst = x->names[x->nameCount]; dcap = XMLTV_TEXT_MAX; }
        break;
    case C_TITLE: dst = x->title; dcap = sizeof(x->title); break;
    case C_SUB:   dst = x->sub;   dcap = sizeof(x->sub); break;
    case C_CAT:   dst = x->cat;   dcap = sizeof(x->cat); break;
    case C_DESC:  dst = x->desc;  dcap = sizeof(x->desc); break;
    }
    if (dst) {
        decode(x->raw, x->rawLen, dst, dcap);
        if (x->cap == C_NAME && dst[0]) x->nameCount++;
    }
    x->cap = C_NONE;
}

static int emit_channel(Xmltv *x) {
    XmltvChannel c;
    c.id = x->chId;
    for (int i = 0; i < XMLTV_MAX_NAMES; i++) c.names[i] = i < x->nameCount ? x->names[i] : "";
    c.nameCount = x->nameCount;
    c.icon = x->icon;
    x->nChannels++;
    return x->cb.channel && x->chId[0] ? x->cb.channel(x->cb.ctx, &c) : 0;
}

static int emit_programme(Xmltv *x) {
    XmltvProgramme p;
    p.channel = x->pChan;
    p.start = xmltv_time(x->pStart);
    p.stop = x->pStop[0] ? xmltv_time(x->pStop) : 0;
    p.title = x->title; p.subTitle = x->sub; p.category = x->cat; p.desc = x->desc;
    x->nProgrammes++;
    if (!x->pChan[0] || !p.start) return 0;
    return x->cb.programme ? x->cb.programme(x->cb.ctx, &p) : 0;
}

static int name_is(const char *tag, int len, const char *name) {
    size_t n = strlen(name);
    return (size_t)len == n && memcmp(tag, name, n) == 0;
}

// A complete tag's text (without < >) is in x->tag.
static int on_tag(Xmltv *x) {
    char *t = x->tag;
    t[x->tagLen] = '\0';
    int closing = t[0] == '/';
    char *nm = t + closing;
    int nl = 0;
    while (nm[nl] && nm[nl] != ' ' && nm[nl] != '\t' && nm[nl] != '\n' && nm[nl] != '\r' && nm[nl] != '/') nl++;
    int len = x->tagLen;
    while (len > 0 && (t[len - 1] == ' ' || t[len - 1] == '\t' || t[len - 1] == '\n' || t[len - 1] == '\r')) len--;
    int selfClose = !closing && len > 0 && t[len - 1] == '/';

    if (closing) {
        if (x->cap != C_NONE && name_is(nm, nl, x->capName)) { cap_end(x); return 0; }
        if (x->cap != C_NONE) return 0;               // markup inside captured text
        if (x->inChannel && name_is(nm, nl, "channel")) { x->inChannel = 0; return emit_channel(x); }
        if (x->inProgramme && name_is(nm, nl, "programme")) { x->inProgramme = 0; return emit_programme(x); }
        return 0;
    }
    if (x->cap != C_NONE) return 0;
    if (name_is(nm, nl, "tv")) { x->sawTv = 1; return 0; }
    if (x->inChannel) {
        if (name_is(nm, nl, "display-name") && !selfClose && x->nameCount < XMLTV_MAX_NAMES) cap_start(x, C_NAME, "display-name");
        else if (name_is(nm, nl, "icon") && !x->icon[0]) attr(t, "src", x->icon, sizeof(x->icon));
        return 0;
    }
    if (x->inProgramme) {
        if (selfClose) return 0;
        if (name_is(nm, nl, "title") && !x->title[0]) cap_start(x, C_TITLE, "title");
        else if (name_is(nm, nl, "sub-title") && !x->sub[0]) cap_start(x, C_SUB, "sub-title");
        else if (name_is(nm, nl, "category") && !x->cat[0]) cap_start(x, C_CAT, "category");
        else if (name_is(nm, nl, "desc") && !x->desc[0]) cap_start(x, C_DESC, "desc");
        return 0;
    }
    if (name_is(nm, nl, "channel")) {
        attr(t, "id", x->chId, sizeof(x->chId));
        x->nameCount = 0; x->icon[0] = '\0';
        if (selfClose) return emit_channel(x);
        x->inChannel = 1;
    } else if (name_is(nm, nl, "programme")) {
        attr(t, "channel", x->pChan, sizeof(x->pChan));
        attr(t, "start", x->pStart, sizeof(x->pStart));
        attr(t, "stop", x->pStop, sizeof(x->pStop));
        x->title[0] = x->sub[0] = x->cat[0] = x->desc[0] = '\0';
        if (selfClose) return emit_programme(x);
        x->inProgramme = 1;
    }
    return 0;
}

static void raw_put(Xmltv *x, const char *p, int n) {
    int room = RAW_MAX - x->rawLen;
    if (n > room) n = room;
    if (n > 0) { memcpy(x->raw + x->rawLen, p, (size_t)n); x->rawLen += n; }
}

int xmltv_push(Xmltv *x, const char *p, int n) {
    if (x->stopped) return XMLTV_STOP;
    for (int i = 0; i < n; i++) {
        char ch = p[i];
        switch (x->state) {
        case S_TEXT: {
            const char *lt = memchr(p + i, '<', (size_t)(n - i));
            int end = lt ? (int)(lt - p) : n;
            if (x->cap != C_NONE) raw_put(x, p + i, end - i);
            i = end;
            if (lt) x->state = S_LT;
            break;
        }
        case S_LT:
            if (ch == '!') { x->state = S_BANG; x->bangLen = 0; }
            else if (ch == '?') { x->state = S_PI; x->tail = 0; }
            else { x->state = S_TAG; x->tagLen = 0; x->quote = 0; i--; }
            break;
        case S_TAG:
            if (x->quote) { if (ch == x->quote) x->quote = 0; }
            else if (ch == '"' || ch == '\'') x->quote = ch;
            else if (ch == '>') {
                x->state = S_TEXT;
                if (on_tag(x)) { x->stopped = 1; return XMLTV_STOP; }
                break;
            }
            if (x->tagLen < TAG_MAX - 1) x->tag[x->tagLen++] = ch;
            break;
        case S_BANG:
            x->bang[x->bangLen++] = ch;
            if (x->bangLen == 2 && !memcmp(x->bang, "--", 2)) { x->state = S_COMMENT; x->tail = 0; break; }
            if (x->bangLen == 7 && !memcmp(x->bang, "[CDATA[", 7)) { x->state = S_CDATA; x->tail = 0; break; }
            if (memcmp(x->bang, "--", (size_t)(x->bangLen < 2 ? x->bangLen : 2)) != 0 &&
                memcmp(x->bang, "[CDATA[", (size_t)(x->bangLen < 7 ? x->bangLen : 7)) != 0) {
                // <!DOCTYPE ...> or another declaration: skip to its '>'.
                x->state = S_DOCTYPE; x->dtDepth = 0; x->quote = 0;
                for (int k = 0; k < x->bangLen; k++) {
                    char b = x->bang[k];
                    if (b == '[') x->dtDepth++;
                    else if (b == ']') x->dtDepth--;
                    else if (b == '>' && x->dtDepth <= 0) { x->state = S_TEXT; break; }
                }
            }
            break;
        case S_COMMENT:
            x->tail = (x->tail << 8 | (unsigned char)ch) & 0xFFFFFF;
            if (x->tail == ('-' << 16 | '-' << 8 | '>')) x->state = S_TEXT;
            break;
        case S_CDATA:
            x->tail = (x->tail << 8 | (unsigned char)ch) & 0xFFFFFF;
            if (x->tail == (']' << 16 | ']' << 8 | '>')) {
                if (x->cap != C_NONE && x->rawLen >= 2) x->rawLen -= 2;   // drop the "]]"
                x->state = S_TEXT;
            } else if (x->cap != C_NONE) {
                // Raw text is entity-decoded later; CDATA must come out as written.
                if (ch == '&') raw_put(x, "&amp;", 5);
                else if (ch == '<') raw_put(x, "&lt;", 4);
                else raw_put(x, &ch, 1);
            }
            break;
        case S_DOCTYPE:
            if (x->quote) { if (ch == x->quote) x->quote = 0; }
            else if (ch == '"' || ch == '\'') x->quote = ch;
            else if (ch == '[') x->dtDepth++;
            else if (ch == ']') x->dtDepth--;
            else if (ch == '>' && x->dtDepth <= 0) x->state = S_TEXT;
            break;
        case S_PI:
            x->tail = (x->tail << 8 | (unsigned char)ch) & 0xFFFF;
            if (x->tail == ('?' << 8 | '>')) x->state = S_TEXT;
            break;
        }
    }
    x->pushed += n;
    if (!x->sawTv && x->pushed > 64 * 1024) return XMLTV_ERR;
    return XMLTV_OK;
}

int xmltv_finish(Xmltv *x) {
    if (x->stopped) return XMLTV_STOP;
    return x->sawTv ? XMLTV_OK : XMLTV_ERR;
}
