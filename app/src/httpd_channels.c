// httpd_channels.c — channel store + M3U parsing + channel endpoints.
// Extracted from httpd.c verbatim (v04.49); behavior unchanged.
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
// Real IPTV playlists routinely carry thousands of channels; 256 silently
// truncated them (the parse loop just stopped), losing most of the list.
#define MAX_CHAN      2000

// Worst-case serialized bytes for one channel's JSON: json_str can double a
// field's length escaping '"' and '\', plus its wrapping quotes; the rest is
// slack for keys, braces, the index/favourite fields and the separating
// comma. Used to size exact-fit response buffers so a started entry can
// never be cut short.
#define CHAN_JSON_ENTRY_MAX   (2*CHAN_NAME_MAX + 2*CHAN_GRP_MAX + 2*1000 + 48)
// Same, for chans_to_json()'s smaller {n,g,u} entries (no index/favourite).
#define CHAN_JSON_PL_ENTRY_MAX (2*90 + 2*44 + 2*1000 + 24)

static char g_chanName[MAX_CHAN][CHAN_NAME_MAX];
static char g_chanGroup[MAX_CHAN][CHAN_GRP_MAX];
static char g_chanUrl[MAX_CHAN][URL_MAX];
static unsigned char g_chanFav[MAX_CHAN];
// Bumped (under g_mtx) on every change the phone/browser needs to notice:
// playlist load, init-time restore, add/edit/del, favourites, and the
// tuned/current channel (it drives the LIVE marker in the channel list).
static int g_chanVer = 0;
static char g_filtLetter = 0;      // 0 = no letter filter
static int  g_filtFav = 0;         // 1 = favourites only
static int  g_filt[MAX_CHAN];
static int  g_filtN = 0;
static int  g_railRow = 0;      // selected bouquet row (0=All,1=Favourites,2+=groups)
static int  g_chanN = 0;
static int  g_chanCur = -1;

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
    const char *q = strpbrk(url, "?#");
    const char *slash = NULL;
    for (const char *p = url; p && *p && p != q; p++) if (*p == '/') slash = p;
    const char *start = slash ? slash + 1 : url;
    int len = q ? (int)(q - start) : (int)strlen(start);
    if (len <= 0 || len >= cap) { snprintf(out, cap, "channel"); return; }
    memcpy(out, start, (size_t)len); out[len] = '\0';
}


void httpd_channels_init(void) {
    scePthreadMutexInit(&g_mtx, NULL, "ps4cast_chan_mtx");
    chan_load_file();
}

void httpd_channels_save(void) {
    int fd = sceKernelOpen(CHAN_PATH, 0x0201 | 0x0400, 0666);
    if (fd < 0) return;
    char line[URL_MAX + CHAN_NAME_MAX + CHAN_GRP_MAX + 8];
    for (int i = 0; i < g_chanN; i++) {
        int n = snprintf(line, sizeof(line), "%s\t%s\t%s\t%d\n", g_chanName[i], g_chanGroup[i], g_chanUrl[i], g_chanFav[i] ? 1 : 0);
        sceKernelWrite(fd, line, n);
    }
    sceKernelClose(fd);
}
static void chan_load_file(void) {
    int fd = sceKernelOpen(CHAN_PATH, 0, 0);
    if (fd < 0) return;
    static char buf[MAX_CHAN * (URL_MAX + CHAN_NAME_MAX + CHAN_GRP_MAX + 8)];
    int n = (int)sceKernelRead(fd, buf, sizeof(buf) - 1);
    sceKernelClose(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    g_chanN = 0; g_chanCur = -1;
    char *save = NULL;
    for (char *ln = strtok_r(buf, "\n", &save); ln && g_chanN < MAX_CHAN; ln = strtok_r(NULL, "\n", &save)) {
        char *t1 = strchr(ln, '\t'); if (!t1) continue; *t1 = '\0';
        char *t2 = strchr(t1 + 1, '\t'); if (!t2) continue; *t2 = '\0';
        const char *grp = t1 + 1, *url = t2 + 1; if (!url[0]) continue;
        int fav = 0;
        char *t3 = strchr(t2 + 1, '\t');           // optional 4th column: favourite
        if (t3) { *t3 = '\0'; fav = atoi(t3 + 1) ? 1 : 0; }
        strncpy(g_chanName[g_chanN], ln, CHAN_NAME_MAX - 1);   g_chanName[g_chanN][CHAN_NAME_MAX - 1] = '\0';
        strncpy(g_chanGroup[g_chanN], grp, CHAN_GRP_MAX - 1);  g_chanGroup[g_chanN][CHAN_GRP_MAX - 1] = '\0';
        strncpy(g_chanUrl[g_chanN], url, URL_MAX - 1);         g_chanUrl[g_chanN][URL_MAX - 1] = '\0';
        g_chanFav[g_chanN] = (unsigned char)fav;
        g_chanN++;
    }
    g_chanVer++;   // restored from /data at init (single-threaded, no lock yet)
}
static void chan_add(const char *name, const char *group, const char *url) {
    if (g_chanN >= MAX_CHAN) return;
    strncpy(g_chanName[g_chanN], name, CHAN_NAME_MAX - 1);  g_chanName[g_chanN][CHAN_NAME_MAX - 1] = '\0';
    strncpy(g_chanGroup[g_chanN], group ? group : "", CHAN_GRP_MAX - 1); g_chanGroup[g_chanN][CHAN_GRP_MAX - 1] = '\0';
    strncpy(g_chanUrl[g_chanN], url, URL_MAX - 1);          g_chanUrl[g_chanN][URL_MAX - 1] = '\0';
    g_chanN++;
}

static int m3u_add(void *ctx, const M3uEntry *e) {
    (void)ctx;
    chan_add(e->name, e->group, e->spec);
    return g_chanN >= MAX_CHAN;          // full: stop parsing
}

// Parse a fetched M3U/IPTV playlist into the shared channel store (caller holds
// g_mtx). A genuine HLS stream (#EXT-X- tags) is one castable entry, not a list.
// m3u.c does the parsing, including the per-channel Referer/User-Agent options.
static void playlist_store(const char *text, const char *srcUrl) {
    g_chanN = 0; g_chanCur = -1;
    if (strstr(text, "#EXT-X-STREAM-INF") || strstr(text, "#EXT-X-TARGETDURATION") ||
        strstr(text, "#EXT-X-MEDIA-SEQUENCE") || strstr(text, "#EXT-X-PLAYLIST-TYPE")) {
        char nm[CHAN_NAME_MAX]; name_from_url(srcUrl, nm, sizeof(nm));
        chan_add(nm, "", srcUrl);
        g_chanVer++;
        return;
    }
    m3u_parse(text, URL_MAX, m3u_add, NULL);
    g_chanVer++;
}

// Serialize the channel store to JSON [{"n":..,"u":..},..] (caller holds g_mtx).
static int chans_to_json(char *out, int cap) {
    int o = 0;
    out[o++] = '[';
    for (int i = 0; i < g_chanN && o < cap - CHAN_JSON_PL_ENTRY_MAX; i++) {
        if (i) out[o++] = ',';
        out[o++] = '{';
        o += snprintf(out + o, cap - o, "\"n\":"); json_str(out, cap, &o, g_chanName[i], 90);
        o += snprintf(out + o, cap - o, ",\"g\":"); json_str(out, cap, &o, g_chanGroup[i], 44);
        o += snprintf(out + o, cap - o, ",\"u\":"); json_str(out, cap, &o, g_chanUrl[i], 1000);
        out[o++] = '}';
    }
    out[o++] = ']';
    return o;
}

// ---- channel store accessors (for the on-screen D-pad zapper, main.c) -----

static void filter_rebuild(void) {
    g_filtN = 0;
    for (int i = 0; i < g_chanN; i++) {
        if (g_filtFav && !g_chanFav[i]) continue;
        if (g_filtLetter) {
            char c = g_chanName[i][0];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            if (g_filtLetter == '#') { if (c >= 'A' && c <= 'Z') continue; }
            else if (c != g_filtLetter) continue;
        }
        g_filt[g_filtN++] = i;
    }
}

void httpd_chan_filter(char letter, int favOnly) {
    g_filtLetter = letter; g_filtFav = favOnly ? 1 : 0; g_railRow = favOnly ? 1 : 0;
    scePthreadMutexLock(&g_mtx); filter_rebuild(); scePthreadMutexUnlock(&g_mtx);
}
char httpd_chan_filter_letter(void) { return g_filtLetter; }
int  httpd_chan_filter_fav(void)    { return g_filtFav; }
int  httpd_chan_filter_count(void)  { return (g_filtLetter || g_filtFav || g_railRow >= 2) ? g_filtN : g_chanN; }
int  httpd_chan_filter_abs(int n) {
    if (!(g_filtLetter || g_filtFav || g_railRow >= 2)) return (n >= 0 && n < g_chanN) ? n : -1;
    return (n >= 0 && n < g_filtN) ? g_filt[n] : -1;
}
int  httpd_chan_is_fav(int i) { return (i >= 0 && i < g_chanN) ? g_chanFav[i] : 0; }
void httpd_chan_toggle_fav(int i) {
    if (i < 0 || i >= g_chanN) return;
    scePthreadMutexLock(&g_mtx);
    g_chanFav[i] = g_chanFav[i] ? 0 : 1;
    filter_rebuild();
    g_chanVer++;
    scePthreadMutexUnlock(&g_mtx);
    httpd_channels_save();
}
// True if any channel starts with `letter` ('#' = non-alphabetic), so the A-Z
// strip can grey out letters that would show an empty list.
int httpd_chan_letter_has(char letter) {
    for (int i = 0; i < g_chanN; i++) {
        char c = g_chanName[i][0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (letter == '#') { if (!(c >= 'A' && c <= 'Z')) return 1; }
        else if (c == letter) return 1;
    }
    return 0;
}

// ---- bouquet rail ---------------------------------------------------------
// Row 0 = All, row 1 = Favourites, then each distinct group ("bouquet") in
// playlist order. Selecting a row just drives httpd_chan_filter().
int httpd_chan_rail_count(void) {
    int n = 2;
    for (int i = 0; i < g_chanN; i++) {
        if (!g_chanGroup[i][0]) continue;
        int seen = 0;
        for (int j = 0; j < i; j++)
            if (g_chanGroup[j][0] && strcmp(g_chanGroup[j], g_chanGroup[i]) == 0) { seen = 1; break; }
        if (!seen) n++;
    }
    return n;
}

void httpd_chan_rail_name(int row, char *out, int cap) {
    if (cap <= 0) return;
    out[0] = 0;
    if (row == 0) { snprintf(out, cap, "All"); return; }
    if (row == 1) { snprintf(out, cap, "Favourites"); return; }
    int n = 2;
    for (int i = 0; i < g_chanN; i++) {
        if (!g_chanGroup[i][0]) continue;
        int seen = 0;
        for (int j = 0; j < i; j++)
            if (g_chanGroup[j][0] && strcmp(g_chanGroup[j], g_chanGroup[i]) == 0) { seen = 1; break; }
        if (seen) continue;
        if (n == row) { snprintf(out, cap, "%s", g_chanGroup[i]); return; }
        n++;
    }
    snprintf(out, cap, "Group %d", row - 1);
}

// Apply a rail row as the active filter (keeps any A-Z letter narrowing).
void httpd_chan_rail_select(int row) {
    char letter = httpd_chan_filter_letter();
    if (row == 1) { httpd_chan_filter(letter, 1); return; }
    if (row <= 0) { httpd_chan_filter(letter, 0); return; }
    char grp[CHAN_GRP_MAX]; httpd_chan_rail_name(row, grp, sizeof(grp));
    scePthreadMutexLock(&g_mtx);
    g_filtFav = 0; g_filtLetter = letter;
    g_filtN = 0;
    for (int i = 0; i < g_chanN; i++) {
        if (strcmp(g_chanGroup[i], grp) != 0) continue;
        if (g_filtLetter) {
            char c = g_chanName[i][0];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            if (g_filtLetter == '#') { if (c >= 'A' && c <= 'Z') continue; }
            else if (c != g_filtLetter) continue;
        }
        g_filt[g_filtN++] = i;
    }
    g_railRow = row;
    scePthreadMutexUnlock(&g_mtx);
}

int httpd_chan_count(void) { return g_chanN; }
int httpd_chan_current(void) { return g_chanCur; }
// Copy channel i's name/url into caller buffers under lock (safe vs. reloads).
int httpd_chan_get(int i, char *name, int nameCap, char *url, int urlCap) {
    int ok = 0;
    scePthreadMutexLock(&g_mtx);
    if (i >= 0 && i < g_chanN) {
        if (name && nameCap > 0) { strncpy(name, g_chanName[i], nameCap - 1); name[nameCap - 1] = '\0'; }
        if (url && urlCap > 0)   { strncpy(url, g_chanUrl[i], urlCap - 1);   url[urlCap - 1] = '\0'; }
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
    if (i >= 0 && i < g_chanN) { strncpy(out, g_chanGroup[i], cap - 1); out[cap - 1] = '\0'; }
    scePthreadMutexUnlock(&g_mtx);
}
// Mark channel i as the one now tuned (also updates the HUD title source).
void httpd_chan_set_current(int i) {
    scePthreadMutexLock(&g_mtx);
    if (i >= -1 && i < g_chanN) {
        g_chanCur = i;
        g_chanVer++;   // moves the LIVE marker in the phone's channel list
        if (i >= 0 && g_pushCb) g_pushCb(g_chanUrl[i]);
    }
    scePthreadMutexUnlock(&g_mtx);
}

int httpd_channels_handle(OrbisNetId c, const char *method, const char *path,
                          const char *body, HttpdSendFn send_response) {
    (void)body;
    // GET /channels -> [{i,n,g,u,f},...] so the phone/browser can manage the
    // list, which is far easier than editing it with a gamepad.
    if (strcmp(method, "GET") == 0 && strcmp(path, "/channels") == 0) {
        scePthreadMutexLock(&g_mtx);
        // Exact-fit buffer for the worst case (every field maxed out and
        // fully escaped): a fixed-size buffer silently truncated large
        // playlists to whatever fraction fit, so allocate for all of them.
        int cap = 2 + g_chanN * CHAN_JSON_ENTRY_MAX + 8;
        char *j = malloc((size_t)cap);
        if (!j) {
            scePthreadMutexUnlock(&g_mtx);
            const char *m = "out of memory";
            send_response(c, "503 Service Unavailable", "text/plain", m, (int)strlen(m));
            return 1;
        }
        int o = 0; j[o++] = '[';
        for (int i = 0; i < g_chanN; i++) {
            if (i) j[o++] = ',';
            o += snprintf(j + o, cap - o, "{\"i\":%d,\"n\":", i);
            json_str(j, cap, &o, g_chanName[i], CHAN_NAME_MAX);
            o += snprintf(j + o, cap - o, ",\"g\":");
            json_str(j, cap, &o, g_chanGroup[i], CHAN_GRP_MAX);
            o += snprintf(j + o, cap - o, ",\"u\":");
            json_str(j, cap, &o, g_chanUrl[i], 1000);
            o += snprintf(j + o, cap - o, ",\"f\":%d}", g_chanFav[i] ? 1 : 0);
        }
        j[o++] = ']';
        scePthreadMutexUnlock(&g_mtx);
        send_response(c, "200 OK", "application/json", j, o);
        free(j);
        return 1;
    }
    // POST /channel/add   body: name\tgroup\turl
    if (strcmp(method, "POST") == 0 && strcmp(path, "/channel/add") == 0) {
        char b[URL_MAX + CHAN_NAME_MAX + CHAN_GRP_MAX + 8];
        strncpy(b, body, sizeof(b) - 1); b[sizeof(b) - 1] = 0;
        for (int i = (int)strlen(b) - 1; i >= 0 && (b[i]=='\r'||b[i]=='\n'); i--) b[i] = 0;
        char *t1 = strchr(b, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t1 || !t2) { send_response(c, "400 Bad Request", "text/plain", "need name\tgroup\turl", 20); return 1; }
        *t1 = 0; *t2 = 0;
        scePthreadMutexLock(&g_mtx);
        chan_add(b, t1 + 1, t2 + 1);
        g_chanVer++;
        scePthreadMutexUnlock(&g_mtx);
        httpd_channels_save();
        send_response(c, "200 OK", "text/plain", "ok", 2); return 1;
    }
    // POST /channel/edit  body: index\tname\tgroup\turl
    if (strcmp(method, "POST") == 0 && strcmp(path, "/channel/edit") == 0) {
        char b[URL_MAX + CHAN_NAME_MAX + CHAN_GRP_MAX + 16];
        strncpy(b, body, sizeof(b) - 1); b[sizeof(b) - 1] = 0;
        for (int i = (int)strlen(b) - 1; i >= 0 && (b[i]=='\r'||b[i]=='\n'); i--) b[i] = 0;
        char *t1 = strchr(b, '\t'); if (!t1) goto edit_bad; *t1 = 0;
        char *t2 = strchr(t1 + 1, '\t'); if (!t2) goto edit_bad; *t2 = 0;
        char *t3 = strchr(t2 + 1, '\t'); if (!t3) goto edit_bad; *t3 = 0;
        {
            int idx = atoi(b);
            scePthreadMutexLock(&g_mtx);
            if (idx >= 0 && idx < g_chanN) {
                strncpy(g_chanName[idx], t1 + 1, CHAN_NAME_MAX - 1); g_chanName[idx][CHAN_NAME_MAX-1] = 0;
                strncpy(g_chanGroup[idx], t2 + 1, CHAN_GRP_MAX - 1); g_chanGroup[idx][CHAN_GRP_MAX-1] = 0;
                strncpy(g_chanUrl[idx], t3 + 1, URL_MAX - 1);        g_chanUrl[idx][URL_MAX-1] = 0;
                g_chanVer++;
            }
            filter_rebuild();
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
        if (!body[0] || body[0] == '\n') { g_chanN = 0; g_chanCur = -1; g_chanVer++; }
        else {
            int idx = atoi(body);
            if (idx >= 0 && idx < g_chanN) {
                for (int i = idx; i < g_chanN - 1; i++) {
                    memcpy(g_chanName[i], g_chanName[i+1], CHAN_NAME_MAX);
                    memcpy(g_chanGroup[i], g_chanGroup[i+1], CHAN_GRP_MAX);
                    memcpy(g_chanUrl[i], g_chanUrl[i+1], URL_MAX);
                    g_chanFav[i] = g_chanFav[i+1];
                }
                g_chanN--;
                if (g_chanCur == idx) g_chanCur = -1;
                else if (g_chanCur > idx) g_chanCur--;
                g_chanVer++;
            }
        }
        filter_rebuild();
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

int httpd_channels_load_playlist(const char *text, const char *srcUrl,
                                 char *out, int cap) {
    scePthreadMutexLock(&g_mtx);
    playlist_store(text, srcUrl);
    httpd_channels_save();
    int n = chans_to_json(out, cap);
    scePthreadMutexUnlock(&g_mtx);
    return n;
}

int httpd_channels_tune(int i, char *urlOut, int urlCap) {
    scePthreadMutexLock(&g_mtx);
    int ok = i >= 0 && i < g_chanN;
    if (ok) {
        g_chanCur = i;
        g_chanVer++;   // moves the LIVE marker in the phone's channel list
        strncpy(urlOut, g_chanUrl[i], (size_t)urlCap - 1); urlOut[urlCap - 1] = '\0';
        if (g_pushCb) g_pushCb(g_chanUrl[i]);
    }
    scePthreadMutexUnlock(&g_mtx);
    return ok;
}
