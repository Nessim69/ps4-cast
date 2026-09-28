#include "hls_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void hlspl_init(HlsPlaylist *pl) {
    memset(pl, 0, sizeof(*pl));
    pl->targetDurMs = 3000;
    pl->initRef.key = -1;
}

void hlspl_free_media(HlsPlaylist *pl) {
    if (pl->segs) {
        for (int i = 0; i < pl->segCount; i++) free(pl->segs[i]);
        free(pl->segs);
    }
    free(pl->initSeg);
    free(pl->segRef);
    for (int i = 0; i < pl->keyCount; i++) free(pl->keys[i].uri);
    free(pl->keys);
    pl->segs = NULL; pl->segCount = 0;
    pl->initSeg = NULL; pl->segRef = NULL;
    pl->keys = NULL; pl->keyCount = 0;
    memset(&pl->initRef, 0, sizeof(pl->initRef));
    pl->initRef.key = -1;
    pl->totalDurMs = 0; pl->pendDisc = 0; pl->isLive = 0;
    pl->targetDurMs = 3000; pl->mediaSeq = 0;
    pl->unsupported[0] = '\0';
}

void hlspl_free(HlsPlaylist *pl) {
    hlspl_free_media(pl);
    hlspl_init(pl);
}

int hlspl_codec_from_str(const char *codecs) {
    if (strstr(codecs, "avc1") || strstr(codecs, "avc3") || strstr(codecs, "h264")) return VC_H264;
    if (strstr(codecs, "hvc1") || strstr(codecs, "hev1") || strstr(codecs, "dvh"))  return VC_HEVC;
    if (strstr(codecs, "vp09") || strstr(codecs, "vp9"))  return VC_VP9;
    if (strstr(codecs, "av01"))                            return VC_AV1;
    return VC_OTHER;
}

// S3 signed URLs break when the PS4's TLS fingerprint meets some CDN edges;
// plain HTTP keeps those buckets working. Only rewrites amazonaws.com hosts.
void hlspl_prefer_plain_s3(char *url, int cap) {
    (void)cap;
    if (!url) return;
    if (strncmp(url, "https://", 8) != 0) return;
    const char *slash = strchr(url + 8, '/');
    int hostLen = slash ? (int)(slash - (url + 8)) : (int)strlen(url + 8);
    if (hostLen <= 0) return;
    if (strstr(url + 8, ".amazonaws.com") && strstr(url + 8, ".amazonaws.com") < url + 8 + hostLen) {
        memmove(url + 7, url + 8, strlen(url + 8) + 1);
        memcpy(url, "http://", 7);
    }
}

// Append [s, s+n) at out[*o], truncating at cap; out stays NUL-terminated.
static void put(char *out, int cap, int *o, const char *s, int n) {
    if (n > cap - 1 - *o) n = cap - 1 - *o;
    if (n > 0) { memcpy(out + *o, s, (size_t)n); *o += n; }
    out[*o] = '\0';
}

// RFC 3986 5.2.4 remove_dot_segments, in place on a path of n bytes that
// starts with '/'. Returns the new length. ".." never climbs above the root.
static int remove_dot_segments(char *p, int n) {
    int r = 0, w = 0;                               // w <= r always: in-place is safe
    while (r < n) {
        int s = r + 1, e = s;                       // p[r] is a '/'
        while (e < n && p[e] != '/') e++;
        int len = e - s, last = (e == n);
        if (len == 1 && p[s] == '.') {
            if (last) p[w++] = '/';
        } else if (len == 2 && p[s] == '.' && p[s + 1] == '.') {
            while (w > 0 && p[w - 1] != '/') w--;
            if (w > 0) w--;
            if (last) p[w++] = '/';
        } else {
            memmove(p + w, p + r, (size_t)(1 + len));
            w += 1 + len;
        }
        r = e;
    }
    if (w == 0) p[w++] = '/';
    return w;
}

// RFC 3986 5.2 reference resolution against an http(s) base: absolute,
// scheme-relative, absolute-path, relative-path ("./", "../" merged with the
// base directory) and query-only references. HTTP redirects need this too:
// Location may be relative (RFC 7231 7.1.2), and both HTTP clients only take
// absolute URLs. The fragment is dropped -- it is never sent to a server, and
// a '#' left in a request path 404s on strict origins. The base's own query
// never leaks into a resolved path.
void hlspl_resolve_ref(const char *base, const char *ref, char *out, int cap) {
    if (!out || cap <= 0) return;
    out[0] = '\0';
    if (!base) base = "";
    if (!ref) ref = "";
    int rlen = (int)strcspn(ref, "#");
    int rpath = (int)strcspn(ref, "?#");          // ref path ends at its query
    if (strncasecmp(ref, "http://", 7) == 0 || strncasecmp(ref, "https://", 8) == 0) {
        snprintf(out, (size_t)cap, "%.*s", rlen, ref);
        for (int i = 0; i < 5 && out[i] && out[i] != ':'; i++)
            if (out[i] >= 'A' && out[i] <= 'Z') out[i] += 32;   // parse_url wants lowercase
        return;
    }
    const char *sch = strstr(base, "://");
    if (!sch) { snprintf(out, (size_t)cap, "%.*s", rlen, ref); return; }   // no usable base
    if (ref[0] == '/' && ref[1] == '/') {          // scheme-relative
        snprintf(out, (size_t)cap, "%.*s:%.*s", (int)(sch - base), base, rlen, ref);
        return;
    }
    const char *auth = sch + 3;
    const char *bend = base + strcspn(base, "#");
    const char *authEnd = auth + strcspn(auth, "/?#");
    const char *bpathEnd = authEnd + strcspn(authEnd, "?#");
    int o = 0;
    put(out, cap, &o, base, (int)(authEnd - base));
    int pathStart = o;
    if (rlen == 0 || ref[0] == '?') {              // same path; new (or kept) query
        if (bpathEnd > authEnd) put(out, cap, &o, authEnd, (int)(bpathEnd - authEnd));
        else put(out, cap, &o, "/", 1);
        if (rlen == 0) put(out, cap, &o, bpathEnd, (int)(bend - bpathEnd));
        else put(out, cap, &o, ref, rlen);
        return;
    }
    if (ref[0] != '/') {                           // merge with the base directory
        const char *slash = NULL;
        for (const char *s = authEnd; s < bpathEnd; s++) if (*s == '/') slash = s;
        if (slash) put(out, cap, &o, authEnd, (int)(slash + 1 - authEnd));
        else put(out, cap, &o, "/", 1);
    }
    put(out, cap, &o, ref, rpath);
    if (o > pathStart) o = pathStart + remove_dot_segments(out + pathStart, o - pathStart);
    out[o] = '\0';
    put(out, cap, &o, ref + rpath, rlen - rpath);
}

// Resolve a playlist reference against its playlist URL (see hlspl_resolve_ref),
// then apply the plain-HTTP S3 rewrite to the result.
void hlspl_resolve_url(const char *base, const char *ref, char *out, int cap) {
    hlspl_resolve_ref(base, ref, out, cap);
    hlspl_prefer_plain_s3(out, cap);
}

static void rstrip(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\r' || s[n-1] == '\n' || s[n-1] == ' ' || s[n-1] == '\t'))
        s[--n] = '\0';
}

// Value of attribute `name` in an attribute list (RFC 8216 4.2), quoted or
// not, into out. Matches whole names only (URI never matches KEYURI). 1 if
// present.
static int attr_value(const char *list, const char *name, char *out, int cap) {
    size_t nl = strlen(name);
    const char *p = list;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        const char *eq = p;
        while (*eq && *eq != '=' && *eq != ',') eq++;
        if (*eq != '=') { p = eq; continue; }
        int match = (size_t)(eq - p) == nl && strncmp(p, name, nl) == 0;
        const char *v = eq + 1, *ve;
        if (*v == '"') { v++; ve = strchr(v, '"'); if (!ve) ve = v + strlen(v); p = *ve ? ve + 1 : ve; }
        else { ve = v; while (*ve && *ve != ',') ve++; p = ve; }
        if (match) {
            int l = (int)(ve - v);
            if (l >= cap) l = cap - 1;
            memcpy(out, v, (size_t)l); out[l] = '\0';
            return 1;
        }
    }
    return 0;
}

// RFC 3986 scheme ("data:", "skd:", "https:") at the start of a reference.
static int has_scheme(const char *u) {
    if (!((*u >= 'a' && *u <= 'z') || (*u >= 'A' && *u <= 'Z'))) return 0;
    for (const char *p = u + 1; *p; p++) {
        if (*p == ':') return 1;
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '+' || *p == '-' || *p == '.')) return 0;
    }
    return 0;
}

// "0x" + up to 32 hex digits, right-aligned into 16 bytes. 1 if valid.
static int parse_iv(const char *s, uint8_t iv[16]) {
    if (s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return 0;
    s += 2;
    int n = (int)strlen(s);
    if (n < 1 || n > 32) return 0;
    memset(iv, 0, 16);
    for (int i = 0; i < n; i++) {
        char c = s[n - 1 - i];
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        iv[15 - i / 2] |= (uint8_t)(i % 2 ? d << 4 : d);
    }
    return 1;
}

// "<n>[@<o>]" (EXT-X-BYTERANGE, EXT-X-MAP BYTERANGE). *off = -1 when absent.
static int parse_byterange(const char *s, int64_t *len, int64_t *off) {
    char *e;
    long long n = strtoll(s, &e, 10);
    if (e == s || n <= 0) return 0;
    *len = n; *off = -1;
    if (*e == '@') {
        const char *o = e + 1;
        long long v = strtoll(o, &e, 10);
        if (e == o || v < 0) return 0;
        *off = v;
    }
    return 1;
}

// segRef is allocated the first time a segment needs a range or a key; the
// segments before it are plain.
static int ensure_seg_refs(HlsPlaylist *pl) {
    if (pl->segRef) return 0;
    pl->segRef = malloc(sizeof(HlsSegRef) * HLS_MAX_SEGMENTS);
    if (!pl->segRef) return -1;
    for (int i = 0; i < HLS_MAX_SEGMENTS; i++) { pl->segRef[i].off = 0; pl->segRef[i].len = 0; pl->segRef[i].key = -1; }
    return 0;
}

// Index of an AES-128 key (reusing the previous entry when a tag repeats it
// unchanged, as live playlists do every refresh), or -1 on allocation failure.
static int add_key(HlsPlaylist *pl, const char *uri, const uint8_t *iv) {
    if (pl->keyCount > 0) {
        HlsKey *k = &pl->keys[pl->keyCount - 1];
        if (strcmp(k->uri, uri) == 0 && k->hasIv == (iv != NULL) && (!iv || memcmp(k->iv, iv, 16) == 0))
            return pl->keyCount - 1;
    }
    if (pl->keyCount >= HLS_MAX_SEGMENTS) return -1;
    if ((pl->keyCount & 15) == 0) {
        HlsKey *nk = realloc(pl->keys, sizeof(HlsKey) * (size_t)(pl->keyCount + 16));
        if (!nk) return -1;
        pl->keys = nk;
    }
    HlsKey *k = &pl->keys[pl->keyCount];
    k->uri = strdup(uri);
    if (!k->uri) return -1;
    k->hasIv = iv != NULL;
    if (iv) memcpy(k->iv, iv, 16); else memset(k->iv, 0, 16);
    return pl->keyCount++;
}

#define KEY_CLEAR  (-1)
#define KEY_NOPLAY (-2)   // SAMPLE-AES, or only a DRM key format was offered

int hlspl_parse_media(HlsPlaylist *pl, char *body, const char *base) {
    pl->segs = malloc(sizeof(char *) * HLS_MAX_SEGMENTS);
    if (!pl->segs) return -1;
    pl->segCount = 0;
    pl->isLive = strstr(body, "#EXT-X-ENDLIST") ? 0 : 1;
    pl->targetDurMs = 3000;
    pl->mediaSeq = 0;
    pl->pendDisc = 0;
    pl->totalDurMs = 0;
    pl->unsupported[0] = '\0';
    pl->initRef.off = pl->initRef.len = 0; pl->initRef.key = KEY_CLEAR;
    int pendingDurMs = 0;

    // Key state (RFC 8216 4.3.2.4): a key applies to every following segment
    // until the next EXT-X-KEY. Consecutive EXT-X-KEY tags offer the SAME
    // segments under different KEYFORMATs (e.g. identity AES-128 next to
    // FairPlay); only "identity" is playable, so a group that offers nothing
    // else is DRM.
    int curKey = KEY_CLEAR, groupIdentity = 0, keySinceSeg = 0;
    // EXT-X-BYTERANGE: pending sub-range for the next URI; without "@o" it
    // continues right after the previous sub-range of the same resource.
    int64_t pendLen = 0, pendOff = -1, prevEnd = 0;
    char prevRangeUri[2048] = "";

    char resolved[2048], val[2048];
    char *save = NULL;
    for (char *line = strtok_r(body, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        rstrip(line);
        if (line[0] == '\0') continue;
        if (line[0] == '#') {
            const char *td = strstr(line, "#EXT-X-TARGETDURATION:");
            if (td) { int s = atoi(td + 22); if (s > 0 && s < 120) pl->targetDurMs = s * 1000; }
            const char *ms = strstr(line, "#EXT-X-MEDIA-SEQUENCE:");
            if (ms) pl->mediaSeq = atoi(ms + 22);
            const char *inf = strstr(line, "#EXTINF:");
            if (inf) {
                double sec = strtod(inf + 8, NULL);
                if (sec > 0.0 && sec < 36000.0)
                    pendingDurMs = (int)(sec * 1000.0 + 0.5);
            }
            if (strstr(line, "#EXT-X-DISCONTINUITY")) pl->pendDisc = 1;
            if (strncmp(line, "#EXT-X-BYTERANGE:", 17) == 0) {
                if (!parse_byterange(line + 17, &pendLen, &pendOff)) pendLen = 0;
                continue;
            }
            if (strncmp(line, "#EXT-X-KEY:", 11) == 0) {
                const char *attrs = line + 11;
                if (keySinceSeg == 0) { groupIdentity = 0; keySinceSeg = 1; }
                char fmt[96] = "identity";
                attr_value(attrs, "KEYFORMAT", fmt, sizeof(fmt));
                char method[32] = "";
                attr_value(attrs, "METHOD", method, sizeof(method));
                if (strcmp(fmt, "identity") != 0) {
                    if (!groupIdentity && strcmp(method, "NONE") != 0) {
                        curKey = KEY_NOPLAY;
                        snprintf(pl->unsupported, sizeof(pl->unsupported), "DRM (%.40s)", fmt);
                    }
                    continue;
                }
                groupIdentity = 1;
                if (strcmp(method, "NONE") == 0) {
                    curKey = KEY_CLEAR;
                } else if (strcmp(method, "AES-128") == 0) {
                    uint8_t iv[16];
                    int hasIv = attr_value(attrs, "IV", val, sizeof(val)) && parse_iv(val, iv);
                    if (!attr_value(attrs, "URI", val, sizeof(val)) || !val[0]) {
                        curKey = KEY_NOPLAY;
                        snprintf(pl->unsupported, sizeof(pl->unsupported), "AES-128 key without URI");
                        continue;
                    }
                    // Keys may be inline (data:) or use another scheme:
                    // only http(s) and relative references are resolved.
                    if (has_scheme(val) && strncasecmp(val, "http", 4) != 0)
                        snprintf(resolved, sizeof(resolved), "%s", val);
                    else
                        hlspl_resolve_url(base, val, resolved, sizeof(resolved));
                    curKey = add_key(pl, resolved, hasIv ? iv : NULL);
                    if (curKey < 0) return -1;
                } else {
                    curKey = KEY_NOPLAY;
                    snprintf(pl->unsupported, sizeof(pl->unsupported), "%.40s encryption",
                             method[0] ? method : "unknown");
                }
                continue;
            }
            const char *map = strstr(line, "#EXT-X-MAP:");
            if (map) {
                if (attr_value(map + 11, "URI", val, sizeof(val)) && val[0]) {
                    hlspl_resolve_url(base, val, resolved, sizeof(resolved));
                    free(pl->initSeg);
                    pl->initSeg = strdup(resolved);
                    pl->initRef.off = pl->initRef.len = 0;
                    char br[64];
                    int64_t l, o;
                    if (attr_value(map + 11, "BYTERANGE", br, sizeof(br)) && parse_byterange(br, &l, &o)) {
                        pl->initRef.len = l;
                        pl->initRef.off = o < 0 ? 0 : o;
                    }
                    // The init segment is encrypted with the key in effect here.
                    pl->initRef.key = curKey;
                    if (curKey == KEY_NOPLAY) return -2;
                }
            }
            continue;
        }
        if (pl->segCount >= HLS_MAX_SEGMENTS) break;
        keySinceSeg = 0;
        if (curKey == KEY_NOPLAY) return -2;
        hlspl_resolve_url(base, line, resolved, sizeof(resolved));
        int64_t rOff = 0, rLen = 0;
        if (pendLen > 0) {
            rLen = pendLen;
            rOff = pendOff >= 0 ? pendOff : (strcmp(prevRangeUri, resolved) == 0 ? prevEnd : 0);
            prevEnd = rOff + rLen;
            snprintf(prevRangeUri, sizeof(prevRangeUri), "%s", resolved);
            pendLen = 0; pendOff = -1;
        }
        if ((rLen > 0 || curKey >= 0) && ensure_seg_refs(pl) != 0) return -1;
        pl->segs[pl->segCount] = strdup(resolved);
        if (pl->segs[pl->segCount]) {
            int dur = pendingDurMs > 0 ? pendingDurMs : pl->targetDurMs;
            pl->segDisc[pl->segCount] = (unsigned char)pl->pendDisc;
            pl->segDurMs[pl->segCount] = dur;
            if (pl->segRef) {
                pl->segRef[pl->segCount].off = rOff;
                pl->segRef[pl->segCount].len = rLen;
                pl->segRef[pl->segCount].key = curKey;
            }
            pl->totalDurMs += dur;
            pl->pendDisc = 0;
            pendingDurMs = 0;
            pl->segCount++;
        }
    }
    return pl->segCount > 0 ? 0 : -1;
}

void hlspl_seg_range(const HlsPlaylist *pl, int seg, int64_t *off, int64_t *len) {
    const HlsSegRef *r = seg < 0 ? &pl->initRef
                       : (pl->segRef && seg < pl->segCount) ? &pl->segRef[seg] : NULL;
    *off = r ? r->off : 0;
    *len = r ? r->len : 0;
}

const HlsKey *hlspl_seg_key(const HlsPlaylist *pl, int seg, uint8_t iv[16]) {
    const HlsSegRef *r = seg < 0 ? &pl->initRef
                       : (pl->segRef && seg < pl->segCount) ? &pl->segRef[seg] : NULL;
    if (!r || r->key < 0 || r->key >= pl->keyCount) return NULL;
    const HlsKey *k = &pl->keys[r->key];
    if (k->hasIv) {
        memcpy(iv, k->iv, 16);
    } else {
        // The segment's media sequence number, big-endian (RFC 8216 5.2). The
        // init segment has none (its IV is mandatory); fall back to the first
        // segment's.
        uint64_t seq = (uint64_t)(int64_t)pl->mediaSeq + (uint64_t)(seg < 0 ? 0 : seg);
        memset(iv, 0, 16);
        for (int i = 0; i < 8; i++) iv[15 - i] = (uint8_t)(seq >> (8 * i));
    }
    return k;
}

int hlspl_has_seg_refs(const HlsPlaylist *pl) {
    return pl->segRef != NULL || pl->initRef.len > 0 || pl->initRef.key >= 0;
}

int hlspl_seg_is_plain(const HlsPlaylist *pl, int seg) {
    const HlsSegRef *r = seg < 0 ? &pl->initRef
                       : (pl->segRef && seg < pl->segCount) ? &pl->segRef[seg] : NULL;
    return !r || (r->len <= 0 && (r->key < 0 || r->key >= pl->keyCount));
}

int hlspl_collect_variants(HlsPlaylist *pl, const char *body, const char *base) {
    pl->variantCount = 0;
    const char *p = body;
    while ((p = strstr(p, "#EXT-X-STREAM-INF")) != NULL && pl->variantCount < HLS_MAX_VARIANTS) {
        const char *eol = strchr(p, '\n'); if (!eol) eol = p + strlen(p);
        int bw = 0, height = 0, fps = 0, codec = VC_OTHER;
        const char *bwp = strstr(p, "BANDWIDTH="); if (bwp && bwp < eol) bw = atoi(bwp + 10);
        const char *rp = strstr(p, "RESOLUTION="); if (rp && rp < eol) { const char *x = strchr(rp, 'x'); if (x) height = atoi(x + 1); }
        const char *fp = strstr(p, "FRAME-RATE="); if (fp && fp < eol) fps = atoi(fp + 11);
        const char *cp = strstr(p, "CODECS=\"");
        if (cp && cp < eol) { char cbuf[128]; const char *cs = cp + 8; const char *ce = strchr(cs, '"');
            int cl = ce ? (int)(ce - cs) : 0; if (cl > 0 && cl < (int)sizeof(cbuf)) { memcpy(cbuf, cs, (size_t)cl); cbuf[cl] = '\0'; codec = hlspl_codec_from_str(cbuf); } }
        char agroup[64] = "";
        const char *ap = strstr(p, "AUDIO=\"");
        if (ap && ap < eol) { const char *as = ap + 7; const char *ae = strchr(as, '"');
            int al = ae ? (int)(ae - as) : 0; if (al > 0 && al < (int)sizeof(agroup)) { memcpy(agroup, as, (size_t)al); agroup[al] = '\0'; } }
        char sgroup[64] = "";
        const char *sp = strstr(p, "SUBTITLES=\"");
        if (sp && sp < eol) { const char *ss = sp + 11; const char *se = strchr(ss, '"');
            int sl = se ? (int)(se - ss) : 0; if (sl > 0 && sl < (int)sizeof(sgroup)) { memcpy(sgroup, ss, (size_t)sl); sgroup[sl] = '\0'; } }
        const char *nl = eol;
        while (nl) {
            const char *ls = nl + 1; const char *le = strchr(ls, '\n');
            int llen = le ? (int)(le - ls) : (int)strlen(ls);
            while (llen > 0 && (ls[llen-1] == '\r' || ls[llen-1] == ' ')) llen--;
            if (llen > 0 && ls[0] != '#') {
                char ref[2048]; int l = llen < (int)sizeof(ref) ? llen : (int)sizeof(ref) - 1;
                memcpy(ref, ls, (size_t)l); ref[l] = '\0';
                hlspl_resolve_url(base, ref, pl->variants[pl->variantCount].url, sizeof(pl->variants[0].url));
                pl->variants[pl->variantCount].bw = bw;
                pl->variants[pl->variantCount].height = height;
                pl->variants[pl->variantCount].fps = fps;
                pl->variants[pl->variantCount].codec = codec;
                strncpy(pl->variants[pl->variantCount].agroup, agroup, sizeof(pl->variants[0].agroup) - 1);
                pl->variants[pl->variantCount].agroup[sizeof(pl->variants[0].agroup) - 1] = '\0';
                snprintf(pl->variants[pl->variantCount].sgroup, sizeof(pl->variants[0].sgroup), "%s", sgroup);
                pl->variantCount++;
                break;
            }
            nl = le;
        }
        p += 17;
    }
    for (int i = 1; i < pl->variantCount; i++) {     // insertion sort by bandwidth
        HlsVariant v = pl->variants[i]; int j = i - 1;
        while (j >= 0 && pl->variants[j].bw > v.bw) { pl->variants[j+1] = pl->variants[j]; j--; }
        pl->variants[j+1] = v;
    }
    return pl->variantCount;
}

// Variant score (lower = better) now that H.264 hardware decode is solid.
// Prefer H.264 up to 1080p, avoid 4K/HEVC/VP9/AV1, and keep 60fps as a
// cautious opt-in unless it is the only good option.
int hlspl_variant_score(const HlsVariant *v) {
    int s = 0;
    switch (v->codec) {
        case VC_H264:  s += 0;        break;
        case VC_HEVC:  s += 700000;   break;
        case VC_VP9:   s += 800000;   break;
        case VC_AV1:   s += 1000000;  break;
        default:       s += 250000;   break;
    }
    if (v->height > 1080)      s += 3000000;
    else if (v->height <= 0)   s += 20000;
    else                       s += (1080 - v->height) / 4;
    if (v->bw > 12000000)      s += (v->bw - 12000000) / 100;
    if (v->fps > 30)           s += 25000;
    int q = v->bw < 10000000 ? v->bw : 10000000;
    s += (10000000 - q) / 100000;
    return s;
}

int hlspl_pick_best(const HlsPlaylist *pl, int maxBw) {
    int best = -1, bestScore = 0x7fffffff;
    for (int i = 0; i < pl->variantCount; i++) {
        if (maxBw > 0 && pl->variants[i].bw >= maxBw) continue;
        int sc = hlspl_variant_score(&pl->variants[i]);
        if (sc < bestScore) { bestScore = sc; best = i; }
    }
    return best;
}

int hlspl_pick_start_variant(const HlsPlaylist *pl) {
    int start = -1;
    for (int i = 0; i < pl->variantCount; i++) {
        const HlsVariant *v = &pl->variants[i];
        if (v->codec != VC_H264 || v->height > 1080 || v->bw <= 0 || v->bw > 2500000) continue;
        if (start < 0 || v->bw > pl->variants[start].bw) start = i;
    }
    if (start >= 0) return start;
    for (int i = 0; i < pl->variantCount; i++) {
        const HlsVariant *v = &pl->variants[i];
        if (v->codec != VC_H264 || v->height > 1080) continue;
        if (start < 0 || (v->bw > 0 && v->bw < pl->variants[start].bw)) start = i;
    }
    if (start >= 0) return start;
    int best = hlspl_pick_best(pl, 0);
    return best < 0 ? 0 : best;
}

// fMP4 cannot replace initialization segments after FFmpeg opens its MOV
// demuxer. Select the final rendition before exposing bytes.
int hlspl_pick_fmp4_start_variant(const HlsPlaylist *pl, int autoMaxHeight) {
    int best = -1;
    int maxBw = autoMaxHeight > 720 ? 10000000 : 4000000;
    for (int i = 0; i < pl->variantCount; i++) {
        const HlsVariant *v = &pl->variants[i];
        if (v->codec != VC_H264 || v->height <= 0 || v->height > autoMaxHeight ||
            v->bw <= 0 || v->bw > maxBw) continue;
        if (best < 0 || v->height > pl->variants[best].height ||
            (v->height == pl->variants[best].height && v->bw > pl->variants[best].bw))
            best = i;
    }
    return best;
}
