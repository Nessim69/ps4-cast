// httpd_channels.c — channel store + M3U parsing + channel endpoints.
// Extracted from httpd.c (v04.49). Storage is sized for big IPTV lists: each
// channel's strings live in one heap block, and the per-frame queries the TV
// makes (bouquet rail, A-Z strip, filters) are answered from indexes rebuilt
// on change instead of rescanning the whole list.
#include "httpd_channels.h"
#include "m3u.h"
#include "httpd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <orbis/libkernel.h>

#define URL_MAX       1024
#define CHAN_NAME_MAX 96
#define CHAN_GRP_MAX  48
#define CHAN_TVG_MAX  128
#define CHAN_LOGO_MAX 512
// Big providers ship tens of thousands of channels; 2000 used to be the cap
// (and before that 256), silently dropping the rest. What still bounds a list
// is memory: ~24 bytes of index plus the strings per channel.
#define MAX_CHAN      100000

typedef struct {
    char          *s;                 // name\0group\0url\0tvgId\0logo\0 (one allocation)
    unsigned short oGrp, oUrl, oTvg, oLogo;
    int            grp;               // index into g_groups, -1 = none
    unsigned char  fav;
} Chan;

#define C_NAME(c)  ((c)->s)
#define C_GRP(c)   ((c)->s + (c)->oGrp)
#define C_URL(c)   ((c)->s + (c)->oUrl)
#define C_TVG(c)   ((c)->s + (c)->oTvg)
#define C_LOGO(c)  ((c)->s + (c)->oLogo)

// Static so the TV's per-frame readers (favourite flags, filter indices) never
// see a moved array; strings are read under g_mtx.
static Chan  g_ch[MAX_CHAN];
static int   g_chanN = 0;
static int   g_chanCur = -1;
static char **g_groups;               // distinct groups, playlist order
static int   g_groupN, g_groupCap;
static unsigned g_letterMask;         // bit c-'A' for A..Z, bit 26 for '#'
static char  g_epgUrl[URL_MAX];       // the playlist's XMLTV link (#EXTM3U x-tvg-url)
// Bumped (under g_mtx) on every change to the list itself: playlist load,
// init-time restore, add/edit/del, favourites. Not on tuning: /status carries
// chan_cur for the LIVE marker, and a bump there made the phone re-download
// the whole list (megabytes for a big one) on every zap.
static int   g_chanVer = 0;
static char  g_filtLetter = 0;        // 0 = no letter filter
static int   g_filtFav = 0;           // 1 = favourites only
static int   g_filt[MAX_CHAN];
static int   g_filtN = 0;
static int   g_railRow = 0;           // selected bouquet row (0=All,1=Favourites,2+=groups)

static OrbisPthreadMutex g_mtx;
static void (*g_pushCb)(const char *url) = NULL;

void httpd_channels_set_push_cb(void (*cb)(const char *url)) { g_pushCb = cb; }

int httpd_channels_version(void) {
    scePthreadMutexLock(&g_mtx);
    int v = g_chanVer;
    scePthreadMutexUnlock(&g_mtx);
    return v;
}

#define CHAN_PATH "/data/ps4cast_channels.txt"

static void chan_load_file(void);

// ---- JSON helpers ---------------------------------------------------------
static int json_len(const char *s) {
    int n = 2;
    for (; *s; s++) n += (*s == '"' || *s == '\\') ? 2 : 1;
    return n;
}

static void json_str(char *out, int cap, int *po, const char *s, int maxchars) {
    int o = *po, n = 0;
    if (o < cap - 2) out[o++] = '"';
    for (const char *p = s; *p && o < cap - 8 && n < maxchars; p++, n++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '"' || ch == '\\') { out[o++] = '\\'; out[o++] = ch; }
        else if (ch < 0x20)          { out[o++] = ' '; }   // strip control chars
        else                         { out[o++] = ch; }
    }
    if (o < cap - 1) out[o++] = '"';
    *po = o;
}

static void name_from_url(const char *url, char *out, int cap) {
    const char *q = strpbrk(url, "?#|");
    const char *slash = NULL;
    for (const char *p = url; p && *p && p != q; p++) if (*p == '/') slash = p;
    const char *start = slash ? slash + 1 : url;
    int len = q ? (int)(q - start) : (int)strlen(start);
    if (len <= 0 || len >= cap) { snprintf(out, cap, "channel"); return; }
    memcpy(out, start, (size_t)len); out[len] = '\0';
}

// ---- storage ------------------------------------------------------------------
// Copy at most max-1 bytes of `v`, turning tabs/newlines into spaces (the
// save file is tab separated, one channel per line).
static int put_field(char *dst, const char *v, int max) {
    int n = 0;
    for (; v && v[n] && n < max - 1; n++) {
        char ch = v[n];
        dst[n] = (ch == '\t' || ch == '\r' || ch == '\n') ? ' ' : ch;
    }
    dst[n] = '\0';
    return n + 1;
}

static int chan_set(Chan *c, const char *name, const char *group, const char *url,
                    const char *tvg, const char *logo) {
    char *s = malloc(CHAN_NAME_MAX + CHAN_GRP_MAX + URL_MAX + CHAN_TVG_MAX + CHAN_LOGO_MAX);
    if (!s) return -1;
    int o = put_field(s, name, CHAN_NAME_MAX);
    int oGrp = o;  o += put_field(s + o, group, CHAN_GRP_MAX);
    int oUrl = o;  o += put_field(s + o, url, URL_MAX);
    int oTvg = o;  o += put_field(s + o, tvg, CHAN_TVG_MAX);
    int oLogo = o; o += put_field(s + o, logo, CHAN_LOGO_MAX);
    char *fit = realloc(s, (size_t)o);
    free(c->s);
    c->s = fit ? fit : s;
    c->oGrp = (unsigned short)oGrp; c->oUrl = (unsigned short)oUrl;
    c->oTvg = (unsigned short)oTvg; c->oLogo = (unsigned short)oLogo;
    return 0;
}

static void chan_clear_all(void) {
    for (int i = 0; i < g_chanN; i++) { free(g_ch[i].s); g_ch[i].s = NULL; }
    g_chanN = 0; g_chanCur = -1;
}

static int chan_add_full(const char *name, const char *group, const char *url,
                         const char *tvg, const char *logo, int fav) {
    if (g_chanN >= MAX_CHAN || !url || !url[0]) return -1;
    Chan *c = &g_ch[g_chanN];
    c->s = NULL;
    if (chan_set(c, name, group, url, tvg, logo) != 0) return -1;
    c->fav = (unsigned char)(fav ? 1 : 0);
    c->grp = -1;
    g_chanN++;
    return 0;
}

static int letter_bit(const char *name) {
    char ch = name[0];
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    return (ch >= 'A' && ch <= 'Z') ? ch - 'A' : 26;
}

static int letter_ok(const Chan *c) {
    if (!g_filtLetter) return 1;
    int b = letter_bit(C_NAME(c));
    return g_filtLetter == '#' ? b == 26 : b == g_filtLetter - 'A';
}

static void filter_rebuild(void) {
    g_filtN = 0;
    int grp = g_railRow >= 2 ? g_railRow - 2 : -1;
    for (int i = 0; i < g_chanN; i++) {
        const Chan *c = &g_ch[i];
        if (g_filtFav && !c->fav) continue;
        if (grp >= 0 && c->grp != grp) continue;
        if (!letter_ok(c)) continue;
        g_filt[g_filtN++] = i;
    }
}

static unsigned hash_str(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

// Rebuild the group list, the A-Z mask and the filter after any change
// (caller holds g_mtx). One pass with a hash table: the rail used to be
// recomputed per frame by comparing every channel with every earlier one.
static void index_rebuild(void) {
    for (int i = 0; i < g_groupN; i++) free(g_groups[i]);
    g_groupN = 0;
    g_letterMask = 0;
    int hcap = 64;
    while (hcap < g_chanN * 2) hcap <<= 1;
    int *h = malloc(sizeof(int) * (size_t)hcap);
    if (h) for (int i = 0; i < hcap; i++) h[i] = -1;
    for (int i = 0; i < g_chanN; i++) {
        Chan *c = &g_ch[i];
        g_letterMask |= 1u << letter_bit(C_NAME(c));
        c->grp = -1;
        const char *gname = C_GRP(c);
        if (!gname[0] || !h) continue;
        unsigned k = hash_str(gname) & (unsigned)(hcap - 1);
        while (h[k] >= 0 && strcmp(g_groups[h[k]], gname) != 0) k = (k + 1) & (unsigned)(hcap - 1);
        if (h[k] < 0) {
            if (g_groupN == g_groupCap) {
                int ncap = g_groupCap ? g_groupCap * 2 : 64;
                char **ng = realloc(g_groups, sizeof(char *) * (size_t)ncap);
                if (!ng) continue;
                g_groups = ng; g_groupCap = ncap;
            }
            char *dup = strdup(gname);
            if (!dup) continue;
            g_groups[g_groupN] = dup;
            h[k] = g_groupN++;
        }
        c->grp = h[k];
    }
    free(h);
    if (g_railRow >= 2 + g_groupN) g_railRow = 0;
    filter_rebuild();
}

void httpd_channels_init(void) {
    scePthreadMutexInit(&g_mtx, NULL, "ps4cast_chan_mtx");
    chan_load_file();
}

// One write of the whole list (thousands of small writes were slow on /data).
void httpd_channels_save(void) {
    scePthreadMutexLock(&g_mtx);
    size_t cap = 64 + strlen(g_epgUrl);
    for (int i = 0; i < g_chanN; i++) {
        const Chan *c = &g_ch[i];
        cap += strlen(C_NAME(c)) + strlen(C_GRP(c)) + strlen(C_URL(c)) + strlen(C_TVG(c)) + strlen(C_LOGO(c)) + 12;
    }
    char *buf = malloc(cap);
    size_t o = 0;
    if (buf) {
        if (g_epgUrl[0]) o += (size_t)snprintf(buf + o, cap - o, "#EPG\t%s\n", g_epgUrl);
        for (int i = 0; i < g_chanN; i++) {
            const Chan *c = &g_ch[i];
            o += (size_t)snprintf(buf + o, cap - o, "%s\t%s\t%s\t%d\t%s\t%s\n", C_NAME(c), C_GRP(c), C_URL(c),
                                  c->fav ? 1 : 0, C_TVG(c), C_LOGO(c));
        }
    }
    scePthreadMutexUnlock(&g_mtx);
    if (!buf) return;
    int fd = sceKernelOpen(CHAN_PATH, 0x0201 | 0x0400, 0666);
    if (fd >= 0) { sceKernelWrite(fd, buf, o); sceKernelClose(fd); }
    free(buf);
}

// name \t group \t url [\t fav [\t tvg-id [\t logo]]] per line; an optional
// "#EPG\t<url>" first line. Older files have 3 or 4 columns.
static void chan_load_file(void) {
    int fd = sceKernelOpen(CHAN_PATH, 0, 0);
    if (fd < 0) return;
    long size = (long)sceKernelLseek(fd, 0, 2 /*SEEK_END*/);
    if (size <= 0 || size > 256L * 1024 * 1024) { sceKernelClose(fd); return; }
    sceKernelLseek(fd, 0, 0 /*SEEK_SET*/);
    char *buf = malloc((size_t)size + 1);
    int n = buf ? (int)sceKernelRead(fd, buf, (size_t)size) : -1;
    sceKernelClose(fd);
    if (n <= 0) { free(buf); return; }
    buf[n] = '\0';
    chan_clear_all();
    char *save = NULL;
    for (char *ln = strtok_r(buf, "\n", &save); ln && g_chanN < MAX_CHAN; ln = strtok_r(NULL, "\n", &save)) {
        if (strncmp(ln, "#EPG\t", 5) == 0) { snprintf(g_epgUrl, sizeof(g_epgUrl), "%s", ln + 5); continue; }
        char *col[6] = { ln, NULL, NULL, NULL, NULL, NULL };
        int k = 1;
        for (char *p = ln; *p && k < 6; p++) if (*p == '\t') { *p = '\0'; col[k++] = p + 1; }
        if (k < 3 || !col[2][0]) continue;
        chan_add_full(col[0], col[1], col[2], col[4] ? col[4] : "", col[5] ? col[5] : "",
                      col[3] ? atoi(col[3]) : 0);
    }
    free(buf);
    index_rebuild();
    g_chanVer++;   // restored from /data at init (single-threaded, no lock yet)
}

static int m3u_add(void *ctx, const M3uEntry *e) {
    (void)ctx;
    chan_add_full(e->name, e->group, e->spec, e->tvgId, e->logo, 0);
    return g_chanN >= MAX_CHAN;          // full: stop parsing
}

// Parse a fetched M3U/IPTV playlist into the shared channel store (caller holds
// g_mtx). A genuine HLS stream (#EXT-X- tags) is one castable entry, not a list.
// m3u.c does the parsing, including the per-channel Referer/User-Agent options.
static void playlist_store(const char *text, const char *srcUrl) {
    chan_clear_all();
    g_epgUrl[0] = '\0';
    if (strstr(text, "#EXT-X-STREAM-INF") || strstr(text, "#EXT-X-TARGETDURATION") ||
        strstr(text, "#EXT-X-MEDIA-SEQUENCE") || strstr(text, "#EXT-X-PLAYLIST-TYPE")) {
        char nm[CHAN_NAME_MAX]; name_from_url(srcUrl, nm, sizeof(nm));
        chan_add_full(nm, "", srcUrl, "", "", 0);
    } else {
        m3u_parse(text, URL_MAX, m3u_add, NULL);
        m3u_epg_url(text, g_epgUrl, sizeof(g_epgUrl));
    }
    index_rebuild();
    g_chanVer++;
}

// The channel list as JSON [{"i","n","g","u","f","l"},..] in an exact-fit
// buffer (caller holds g_mtx; frees *out). A worst-case-per-entry buffer was
// ~2.4 KB a channel -- over 100 MB for a big list.
static char *chans_json(int *outLen) {
    size_t cap = 8;
    for (int i = 0; i < g_chanN; i++) {
        const Chan *c = &g_ch[i];
        cap += 48 + (size_t)(json_len(C_NAME(c)) + json_len(C_GRP(c)) + json_len(C_URL(c)) + json_len(C_LOGO(c)));
    }
    char *j = malloc(cap);
    if (!j) return NULL;
    int o = 0, icap = (int)cap;
    j[o++] = '[';
    for (int i = 0; i < g_chanN; i++) {
        const Chan *c = &g_ch[i];
        if (i) j[o++] = ',';
        o += snprintf(j + o, (size_t)(icap - o), "{\"i\":%d,\"n\":", i);
        json_str(j, icap, &o, C_NAME(c), CHAN_NAME_MAX);
        o += snprintf(j + o, (size_t)(icap - o), ",\"g\":");
        json_str(j, icap, &o, C_GRP(c), CHAN_GRP_MAX);
        o += snprintf(j + o, (size_t)(icap - o), ",\"u\":");
        json_str(j, icap, &o, C_URL(c), URL_MAX);
        o += snprintf(j + o, (size_t)(icap - o), ",\"f\":%d", c->fav ? 1 : 0);
        if (C_LOGO(c)[0]) { o += snprintf(j + o, (size_t)(icap - o), ",\"l\":"); json_str(j, icap, &o, C_LOGO(c), CHAN_LOGO_MAX); }
        j[o++] = '}';
    }
    j[o++] = ']';
    *outLen = o;
    return j;
}

// ---- channel store accessors (for the on-screen D-pad zapper, main.c) -----

void httpd_chan_filter(char letter, int favOnly) {
    scePthreadMutexLock(&g_mtx);
    g_filtLetter = letter; g_filtFav = favOnly ? 1 : 0; g_railRow = favOnly ? 1 : 0;
    filter_rebuild();
    scePthreadMutexUnlock(&g_mtx);
}
char httpd_chan_filter_letter(void) { return g_filtLetter; }
int  httpd_chan_filter_fav(void)    { return g_filtFav; }
int  httpd_chan_filter_count(void)  { return (g_filtLetter || g_filtFav || g_railRow >= 2) ? g_filtN : g_chanN; }
int  httpd_chan_filter_abs(int n) {
    if (!(g_filtLetter || g_filtFav || g_railRow >= 2)) return (n >= 0 && n < g_chanN) ? n : -1;
    return (n >= 0 && n < g_filtN) ? g_filt[n] : -1;
}
int  httpd_chan_is_fav(int i) { return (i >= 0 && i < g_chanN) ? g_ch[i].fav : 0; }
void httpd_chan_toggle_fav(int i) {
    scePthreadMutexLock(&g_mtx);
    if (i >= 0 && i < g_chanN) {
        g_ch[i].fav = g_ch[i].fav ? 0 : 1;
        filter_rebuild();
        g_chanVer++;
    }
    scePthreadMutexUnlock(&g_mtx);
    httpd_channels_save();
}
// True if any channel starts with `letter` ('#' = non-alphabetic), so the A-Z
// strip can grey out letters that would show an empty list.
int httpd_chan_letter_has(char letter) {
    int b = letter == '#' ? 26 : (letter >= 'A' && letter <= 'Z') ? letter - 'A' : -1;
    return b >= 0 && (g_letterMask >> b & 1u);
}

// ---- bouquet rail ---------------------------------------------------------
// Row 0 = All, row 1 = Favourites, then each distinct group ("bouquet") in
// playlist order. Selecting a row just drives the filter.
int httpd_chan_rail_count(void) { return 2 + g_groupN; }

void httpd_chan_rail_name(int row, char *out, int cap) {
    if (cap <= 0) return;
    out[0] = 0;
    if (row == 0) { snprintf(out, cap, "All"); return; }
    if (row == 1) { snprintf(out, cap, "Favourites"); return; }
    scePthreadMutexLock(&g_mtx);
    if (row - 2 < g_groupN) snprintf(out, cap, "%s", g_groups[row - 2]);
    else snprintf(out, cap, "Group %d", row - 1);
    scePthreadMutexUnlock(&g_mtx);
}

// Apply a rail row as the active filter (keeps any A-Z letter narrowing).
void httpd_chan_rail_select(int row) {
    scePthreadMutexLock(&g_mtx);
    g_filtFav = row == 1;
    g_railRow = (row >= 2 && row - 2 < g_groupN) ? row : (row == 1 ? 1 : 0);
    filter_rebuild();
    scePthreadMutexUnlock(&g_mtx);
}

int httpd_chan_count(void) { return g_chanN; }
int httpd_chan_current(void) { return g_chanCur; }
// Copy channel i's name/url into caller buffers under lock (safe vs. reloads).
int httpd_chan_get(int i, char *name, int nameCap, char *url, int urlCap) {
    int ok = 0;
    scePthreadMutexLock(&g_mtx);
    if (i >= 0 && i < g_chanN) {
        if (name && nameCap > 0) snprintf(name, (size_t)nameCap, "%s", C_NAME(&g_ch[i]));
        if (url && urlCap > 0)   snprintf(url, (size_t)urlCap, "%s", C_URL(&g_ch[i]));
        ok = 1;
    }
    scePthreadMutexUnlock(&g_mtx);
    return ok;
}
// Copy channel i's group label (empty string if none).
void httpd_chan_group(int i, char *out, int cap) {
    if (!out || cap <= 0) return;
    out[0] = '\0';
    scePthreadMutexLock(&g_mtx);
    if (i >= 0 && i < g_chanN) snprintf(out, (size_t)cap, "%s", C_GRP(&g_ch[i]));
    scePthreadMutexUnlock(&g_mtx);
}
int httpd_chan_meta(int i, char *tvgId, int tvgCap, char *logo, int logoCap) {
    int ok = 0;
    scePthreadMutexLock(&g_mtx);
    if (i >= 0 && i < g_chanN) {
        if (tvgId && tvgCap > 0) snprintf(tvgId, (size_t)tvgCap, "%s", C_TVG(&g_ch[i]));
        if (logo && logoCap > 0) snprintf(logo, (size_t)logoCap, "%s", C_LOGO(&g_ch[i]));
        ok = 1;
    }
    scePthreadMutexUnlock(&g_mtx);
    return ok;
}
void httpd_channels_epg_url(char *out, int cap) {
    scePthreadMutexLock(&g_mtx);
    snprintf(out, (size_t)cap, "%s", g_epgUrl);
    scePthreadMutexUnlock(&g_mtx);
}
// Mark channel i as the one now tuned (also updates the HUD title source).
void httpd_chan_set_current(int i) {
    scePthreadMutexLock(&g_mtx);
    if (i >= -1 && i < g_chanN) {
        g_chanCur = i;
        if (i >= 0 && g_pushCb) g_pushCb(C_URL(&g_ch[i]));
    }
    scePthreadMutexUnlock(&g_mtx);
}

int httpd_channels_handle(OrbisNetId c, const char *method, const char *path,
                          const char *body, HttpdSendFn send_response) {
    // GET /channels -> [{i,n,g,u,f,l},...] so the phone/browser can manage the
    // list, which is far easier than editing it with a gamepad.
    if (strcmp(method, "GET") == 0 && strcmp(path, "/channels") == 0) {
        int len = 0;
        scePthreadMutexLock(&g_mtx);
        char *j = chans_json(&len);
        scePthreadMutexUnlock(&g_mtx);
        if (!j) {
            const char *m = "out of memory";
            send_response(c, "503 Service Unavailable", "text/plain", m, (int)strlen(m));
            return 1;
        }
        send_response(c, "200 OK", "application/json", j, len);
        free(j);
        return 1;
    }
    // POST /channel/add   body: name\tgroup\turl
    if (strcmp(method, "POST") == 0 && strcmp(path, "/channel/add") == 0) {
        char b[URL_MAX + CHAN_NAME_MAX + CHAN_GRP_MAX + 8];
        snprintf(b, sizeof(b), "%s", body);
        for (int i = (int)strlen(b) - 1; i >= 0 && (b[i]=='\r'||b[i]=='\n'); i--) b[i] = 0;
        char *t1 = strchr(b, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t1 || !t2) { send_response(c, "400 Bad Request", "text/plain", "need name\tgroup\turl", 20); return 1; }
        *t1 = 0; *t2 = 0;
        scePthreadMutexLock(&g_mtx);
        int rc = chan_add_full(b, t1 + 1, t2 + 1, "", "", 0);
        index_rebuild();
        g_chanVer++;
        scePthreadMutexUnlock(&g_mtx);
        httpd_channels_save();
        if (rc == 0) send_response(c, "200 OK", "text/plain", "ok", 2);
        else send_response(c, "507 Insufficient Storage", "text/plain", "channel list is full", 20);
        return 1;
    }
    // POST /channel/edit  body: index\tname\tgroup\turl
    if (strcmp(method, "POST") == 0 && strcmp(path, "/channel/edit") == 0) {
        char b[URL_MAX + CHAN_NAME_MAX + CHAN_GRP_MAX + 16];
        snprintf(b, sizeof(b), "%s", body);
        for (int i = (int)strlen(b) - 1; i >= 0 && (b[i]=='\r'||b[i]=='\n'); i--) b[i] = 0;
        char *t1 = strchr(b, '\t'); if (!t1) goto edit_bad; *t1 = 0;
        char *t2 = strchr(t1 + 1, '\t'); if (!t2) goto edit_bad; *t2 = 0;
        char *t3 = strchr(t2 + 1, '\t'); if (!t3) goto edit_bad; *t3 = 0;
        {
            int idx = atoi(b);
            scePthreadMutexLock(&g_mtx);
            if (idx >= 0 && idx < g_chanN) {
                Chan *ch = &g_ch[idx];
                char tvg[CHAN_TVG_MAX], logo[CHAN_LOGO_MAX];   // kept: the form doesn't carry them
                snprintf(tvg, sizeof(tvg), "%s", C_TVG(ch));
                snprintf(logo, sizeof(logo), "%s", C_LOGO(ch));
                chan_set(ch, t1 + 1, t2 + 1, t3 + 1, tvg, logo);
                index_rebuild();
                g_chanVer++;
            }
            scePthreadMutexUnlock(&g_mtx);
            httpd_channels_save();
            send_response(c, "200 OK", "text/plain", "ok", 2); return 1;
        }
    edit_bad:
        send_response(c, "400 Bad Request", "text/plain", "need i\tname\tgroup\turl", 23); return 1;
    }
    // POST /channel/del   body: index   (empty body = clear the whole list)
    if (strcmp(method, "POST") == 0 && strcmp(path, "/channel/del") == 0) {
        scePthreadMutexLock(&g_mtx);
        if (!body[0] || body[0] == '\n') { chan_clear_all(); g_epgUrl[0] = '\0'; g_chanVer++; }
        else {
            int idx = atoi(body);
            if (idx >= 0 && idx < g_chanN) {
                free(g_ch[idx].s);
                memmove(&g_ch[idx], &g_ch[idx + 1], sizeof(Chan) * (size_t)(g_chanN - idx - 1));
                g_chanN--;
                g_ch[g_chanN].s = NULL;
                if (g_chanCur == idx) g_chanCur = -1;
                else if (g_chanCur > idx) g_chanCur--;
                g_chanVer++;
            }
        }
        index_rebuild();
        scePthreadMutexUnlock(&g_mtx);
        httpd_channels_save();
        send_response(c, "200 OK", "text/plain", "ok", 2); return 1;
    }
    // POST /channel/fav   body: index   (toggles)
    if (strcmp(method, "POST") == 0 && strcmp(path, "/channel/fav") == 0) {
        httpd_chan_toggle_fav(atoi(body));
        send_response(c, "200 OK", "text/plain", "ok", 2); return 1;
    }
    return 0;
}

char *httpd_channels_load_playlist(const char *text, const char *srcUrl, int *len) {
    scePthreadMutexLock(&g_mtx);
    playlist_store(text, srcUrl);
    scePthreadMutexUnlock(&g_mtx);
    httpd_channels_save();
    scePthreadMutexLock(&g_mtx);
    char *j = g_chanN > 0 ? chans_json(len) : NULL;
    scePthreadMutexUnlock(&g_mtx);
    return j;
}

int httpd_channels_tune(int i, char *urlOut, int urlCap) {
    scePthreadMutexLock(&g_mtx);
    int ok = i >= 0 && i < g_chanN;
    if (ok) {
        g_chanCur = i;
        snprintf(urlOut, (size_t)urlCap, "%s", C_URL(&g_ch[i]));
        if (g_pushCb) g_pushCb(C_URL(&g_ch[i]));
    }
    scePthreadMutexUnlock(&g_mtx);
    return ok;
}
