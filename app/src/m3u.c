#include "m3u.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void m3u_opts_clear(M3uOpts *o) { memset(o, 0, sizeof(*o)); }

// Copy a value, trimmed, without surrounding quotes and without control
// characters (it ends up in an HTTP header).
static void set_val(char *dst, int cap, const char *v, int len) {
    while (len > 0 && (*v == ' ' || *v == '\t')) { v++; len--; }
    while (len > 0 && (v[len - 1] == ' ' || v[len - 1] == '\t')) len--;
    if (len >= 2 && ((v[0] == '"' && v[len - 1] == '"') || (v[0] == '\'' && v[len - 1] == '\''))) { v++; len -= 2; }
    int o = 0;
    for (int i = 0; i < len && o < cap - 1; i++) {
        unsigned char c = (unsigned char)v[i];
        if (c < 0x20 || c == 0x7f) continue;
        dst[o++] = (char)c;
    }
    dst[o] = '\0';
}

static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Percent-decode v[0..len) into a scratch buffer (Kodi header values are
// URL-encoded), then store it.
static void set_decoded(char *dst, int cap, const char *v, int len) {
    char tmp[1024];
    int o = 0;
    for (int i = 0; i < len && o < (int)sizeof(tmp) - 1; i++) {
        int c = (unsigned char)v[i];
        if (c == '%' && i + 2 < len && hexv(v[i + 1]) >= 0 && hexv(v[i + 2]) >= 0) {
            c = hexv(v[i + 1]) << 4 | hexv(v[i + 2]); i += 2;
        } else if (c == '+') c = ' ';
        tmp[o++] = (char)c;
    }
    set_val(dst, cap, tmp, o);
}

// Route a header name to its slot; 0 if it is not one we forward.
static char *slot_for(M3uOpts *o, const char *k, int klen, int *cap) {
    #define IS(s) (klen == (int)sizeof(s) - 1 && strncasecmp(k, s, (size_t)klen) == 0)
    if (IS("user-agent") || IS("http-user-agent") || IS("useragent")) { *cap = sizeof(o->ua); return o->ua; }
    if (IS("referer") || IS("referrer") || IS("http-referrer") || IS("http-referer")) { *cap = sizeof(o->referer); return o->referer; }
    if (IS("origin") || IS("http-origin")) { *cap = sizeof(o->origin); return o->origin; }
    if (IS("cookie") || IS("http-cookie")) { *cap = sizeof(o->cookie); return o->cookie; }
    #undef IS
    return 0;
}

// key="value" attributes of an #EXTINF / #EXTM3U line (up to the title comma).
static void scan_attrs(M3uOpts *o, const char *s) {
    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ',' ) break;                      // the title starts here
        const char *k = p;
        while (*p && *p != '=' && *p != ' ' && *p != ',' && *p != '"') p++;
        if (*p != '=' || p[1] != '"') {             // not key="..": skip one token
            while (*p && *p != ' ' && *p != ',') p++;
            continue;
        }
        int klen = (int)(p - k);
        const char *v = p + 2, *ve = strchr(v, '"');
        if (!ve) break;
        int cap = 0;
        char *dst = slot_for(o, k, klen, &cap);
        if (dst) set_val(dst, cap, v, (int)(ve - v));
        p = ve + 1;
    }
}

// {"User-Agent":"..","Referer":".."}: string values only, \" \\ \/ escapes.
static void scan_json(M3uOpts *o, const char *s) {
    const char *p = strchr(s, '{');
    if (!p) return;
    char key[64], val[1024];
    for (;;) {
        const char *q = strchr(p, '"');
        if (!q) return;
        int kl = 0;
        for (q++; *q && *q != '"' && kl < (int)sizeof(key) - 1; q++) key[kl++] = *q;
        key[kl] = '\0';
        if (*q != '"') return;
        q++;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != ':') { p = q; continue; }
        q++;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '"') { p = q; continue; }             // numbers/objects: not headers
        int vl = 0;
        for (q++; *q && *q != '"' && vl < (int)sizeof(val) - 1; q++) {
            if (*q == '\\' && q[1]) { q++; if (*q == 'n' || *q == 'r' || *q == 't') continue; }
            val[vl++] = *q;
        }
        if (*q != '"') return;
        int cap = 0;
        char *dst = slot_for(o, key, kl, &cap);
        if (dst) set_val(dst, cap, val, vl);
        p = q + 1;
    }
}

int m3u_opts_line(M3uOpts *o, const char *s) {
    if (strncasecmp(s, "#EXTVLCOPT:", 11) == 0) {
        const char *kv = s + 11, *eq = strchr(kv, '=');
        if (!eq) return 1;
        int cap = 0;
        char *dst = slot_for(o, kv, (int)(eq - kv), &cap);
        if (dst) set_val(dst, cap, eq + 1, (int)strlen(eq + 1));
        return 1;
    }
    if (strncasecmp(s, "#EXTHTTP:", 9) == 0) { scan_json(o, s + 9); return 1; }
    if (strncasecmp(s, "#KODIPROP:", 10) == 0) {
        const char *kv = s + 10, *eq = strchr(kv, '=');
        if (!eq) return 1;
        int pl = (int)(eq - kv);
        #define PROP(n) (pl == (int)sizeof(n) - 1 && strncasecmp(kv, n, (size_t)pl) == 0)
        if (!PROP("inputstream.adaptive.stream_headers") && !PROP("inputstream.adaptive.manifest_headers") &&
            !PROP("inputstream.adaptive.common_headers")) return 1;
        #undef PROP
        for (const char *p = eq + 1; *p; ) {           // Header=urlencoded&Header2=..
            const char *amp = strchr(p, '&'), *end = amp ? amp : p + strlen(p);
            const char *e = memchr(p, '=', (size_t)(end - p));
            if (e) {
                int cap = 0;
                char *dst = slot_for(o, p, (int)(e - p), &cap);
                if (dst) set_decoded(dst, cap, e + 1, (int)(end - e - 1));
            }
            if (!amp) break;
            p = amp + 1;
        }
        return 1;
    }
    if (strncmp(s, "#EXTINF:", 8) == 0) {
        const char *a = s + 8;
        while (*a == '-' || (*a >= '0' && *a <= '9') || *a == '.') a++;   // duration
        scan_attrs(o, a);
        return 0;                                       // the caller still wants the title
    }
    if (strncmp(s, "#EXTM3U", 7) == 0) { scan_attrs(o, s + 7); return 1; }
    return 0;
}

// Percent-encode what urlopt.c treats as syntax ('%', '&', '|').
static int put_enc(char *out, int cap, int o, const char *v) {
    for (; *v; v++) {
        unsigned char c = (unsigned char)*v;
        if (c == '%' || c == '&' || c == '|') {
            if (o + 3 >= cap) return -1;
            o += snprintf(out + o, (size_t)(cap - o), "%%%02X", c);
        } else {
            if (o + 1 >= cap) return -1;
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return o;
}

// Does the "K=V&K2=V2" option list already set this header (any alias)?
static int has_opt(const char *opts, const char *name) {
    M3uOpts probe;
    for (const char *p = opts; p && *p; ) {
        const char *amp = strchr(p, '&'), *end = amp ? amp : p + strlen(p);
        const char *e = memchr(p, '=', (size_t)(end - p));
        int cap = 0;
        if (e) {
            char *a = slot_for(&probe, p, (int)(e - p), &cap);
            char *b = slot_for(&probe, name, (int)strlen(name), &cap);
            if (a && a == b) return 1;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return 0;
}

int m3u_spec(const M3uOpts *entry, const M3uOpts *defs, const char *url, char *out, int cap) {
    int o = snprintf(out, (size_t)cap, "%s", url);
    if (o < 0 || o >= cap) { out[cap - 1] = '\0'; return -1; }
    const char *bar = strchr(out, '|');
    int haveOpts = bar && bar[1];
    char existing[1024] = "";
    if (bar) snprintf(existing, sizeof(existing), "%s", bar + 1);
    struct { const char *name; const char *e; const char *d; } kv[] = {
        { "User-Agent", entry->ua,      defs ? defs->ua : "" },
        { "Referer",    entry->referer, defs ? defs->referer : "" },
        { "Origin",     entry->origin,  defs ? defs->origin : "" },
        { "Cookie",     entry->cookie,  defs ? defs->cookie : "" },
    };
    for (unsigned i = 0; i < sizeof(kv) / sizeof(kv[0]); i++) {
        const char *v = kv[i].e[0] ? kv[i].e : kv[i].d;
        if (!v || !v[0] || has_opt(existing, kv[i].name)) continue;
        int save = o;
        int n = snprintf(out + o, (size_t)(cap - o), "%s%s=", haveOpts ? "&" : (bar ? "" : "|"), kv[i].name);
        if (n < 0 || o + n >= cap) { out[save] = '\0'; continue; }
        int r = put_enc(out, cap, o + n, v);
        if (r < 0) { out[save] = '\0'; o = save; continue; }   // doesn't fit: skip this one
        o = r;
        haveOpts = 1;
        if (!bar) bar = out;   // from now on append with '&'
    }
    return 0;
}

// Last path segment of a URL (no query/options) as a fallback title.
static void name_from_url(const char *url, char *out, int cap) {
    const char *end = url + strcspn(url, "?#|");
    const char *slash = end;
    while (slash > url && slash[-1] != '/') slash--;
    int n = (int)(end - slash);
    if (n <= 0 || n >= cap) { snprintf(out, (size_t)cap, "stream"); return; }
    memcpy(out, slash, (size_t)n); out[n] = '\0';
}

// Quoted attribute `key="..."` of an #EXTINF/#EXTM3U line, matched as a
// whole word (tvg-id must not match inside x-tvg-id). 1 if present.
static int quoted_attr(const char *s, const char *key, char *out, int cap) {
    size_t kl = strlen(key);
    for (const char *p = s; (p = strstr(p, key)) != NULL; p += kl) {
        if (p > s && p[-1] != ' ' && p[-1] != '\t' && p[-1] != ':') continue;
        if (p[kl] != '=' || p[kl + 1] != '"') continue;
        const char *v = p + kl + 2, *e = strchr(v, '"');
        if (!e) return 0;
        set_val(out, cap, v, (int)(e - v));
        return 1;
    }
    return 0;
}

int m3u_epg_url(const char *text, char *out, int cap) {
    if (!text) return 0;
    while (*text == '\xEF' || *text == '\xBB' || *text == '\xBF' || *text == ' ') text++;   // BOM
    if (strncmp(text, "#EXTM3U", 7) != 0) return 0;
    int n = (int)strcspn(text, "\r\n");
    char line[2048];
    snprintf(line, sizeof(line), "%.*s", n < (int)sizeof(line) - 1 ? n : (int)sizeof(line) - 1, text);
    if (!quoted_attr(line, "x-tvg-url", out, cap) && !quoted_attr(line, "url-tvg", out, cap) &&
        !quoted_attr(line, "tvg-url", out, cap)) return 0;
    out[strcspn(out, ", ")] = '\0';                  // the first of several
    return strncmp(out, "http://", 7) == 0 || strncmp(out, "https://", 8) == 0;
}

int m3u_parse(const char *text, int specCap, int (*add)(void *ctx, const M3uEntry *e), void *ctx) {
    M3uOpts defs, pend;
    m3u_opts_clear(&defs); m3u_opts_clear(&pend);
    char title[256] = "", grp[64] = "", sticky[64] = "", tvg[128] = "", logo[512] = "";
    char *spec = malloc((size_t)specCap), *line = malloc(4096);
    int count = 0;
    if (!spec || !line) { free(spec); free(line); return 0; }
    for (const char *p = text; p && *p; ) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        int ll = len < 4095 ? len : 4095;
        memcpy(line, p, (size_t)ll); line[ll] = '\0';
        for (int i = ll - 1; i >= 0 && (line[i] == '\r' || line[i] == ' ' || line[i] == '\t'); i--) line[i] = '\0';
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0') {
        } else if (strncmp(s, "#EXTM3U", 7) == 0) {
            m3u_opts_line(&defs, s);
        } else if (strncmp(s, "#EXTINF:", 8) == 0) {
            // Title = text after the first comma outside quotes (attributes
            // like group-title="A,B" may contain commas).
            const char *cur = s + 8, *name = NULL;
            int inq = 0;
            for (; *cur; cur++) {
                if (*cur == '"') inq = !inq;
                else if (*cur == ',' && !inq) { name = cur + 1; break; }
            }
            title[0] = '\0';
            if (name) { while (*name == ' ' || *name == '\t') name++; snprintf(title, sizeof(title), "%s", name); }
            grp[0] = '\0';
            const char *g = strstr(s, "group-title=\"");
            if (g) { g += 13; int k = 0; while (g[k] && g[k] != '"' && k < (int)sizeof(grp) - 1) { grp[k] = g[k]; k++; } grp[k] = '\0'; }
            tvg[0] = logo[0] = '\0';
            quoted_attr(s, "tvg-id", tvg, sizeof(tvg));
            if (!quoted_attr(s, "tvg-logo", logo, sizeof(logo))) quoted_attr(s, "logo", logo, sizeof(logo));
            m3u_opts_line(&pend, s);
        } else if (strncmp(s, "#EXTGRP:", 8) == 0) {
            const char *g = s + 8;
            while (*g == ' ' || *g == '\t') g++;
            snprintf(sticky, sizeof(sticky), "%s", g);
        } else if (s[0] == '#') {
            m3u_opts_line(&pend, s);                  // #EXTVLCOPT / #EXTHTTP / #KODIPROP; others ignored
        } else {
            char nm[256];
            if (title[0]) snprintf(nm, sizeof(nm), "%s", title);
            else name_from_url(s, nm, sizeof(nm));
            m3u_spec(&pend, &defs, s, spec, specCap);
            M3uEntry e = { nm, grp[0] ? grp : sticky, spec, tvg, logo };
            count++;
            int stop = add(ctx, &e);
            title[0] = '\0'; grp[0] = '\0'; tvg[0] = '\0'; logo[0] = '\0';
            m3u_opts_clear(&pend);
            if (stop) break;
        }
        if (!nl) break;
        p = nl + 1;
    }
    free(spec); free(line);
    return count;
}
