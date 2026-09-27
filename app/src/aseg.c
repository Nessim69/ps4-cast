#include "aseg.h"
#include "tls.h"
#include "urlopt.h"
#include "native_http.h"
#include "hls_parse.h"

// main.c: pet the freeze watchdog during a legitimately-progressing blocking op.
// Safe here because every fetch is bounded by its channel's budget, so this can
// never mask a true freeze — it only stops a SLOW-but-bounded playlist/segment
// fetch from being mistaken for one (the "HANG watchdog stale=35s" fail-close
// seen while zapping past several unreachable channels in a row).
extern void watchdog_kick(void);
extern const char *watchdog_note(const char *w);

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <orbis/Net.h>
#include <orbis/libkernel.h>

// Minimal "download a whole resource into RAM" client: HLS playlists and
// segments, page scraping and the phone's IPTV import. Blocking fetch-into-RAM
// is plenty for those, and keeps this completely separate from the video
// read-ahead reader (httpsrc) — the two never share a socket.
//
// Fetches run on CHANNELS (aseg.h), each with its own lock, socket/TLS,
// keep-alive identity, budget and diagnostics. With one global connection the
// video prefetch, separate-audio prefetch, playlist opens/refreshes and the
// IPTV import all queued behind each other on one socket, and keep-alive
// thrashed whenever two of them talked to different hosts.

typedef struct {
    uint8_t  len;
    uint8_t  family;
    uint16_t port;   // network byte order
    uint32_t addr;   // network byte order
    uint8_t  zero[8];
} ps4_sockaddr_in;

#define ORBIS_NET_SOL_SOCKET   0xffff
#define ORBIS_NET_SO_SNDTIMEO  0x1005
#define ORBIS_NET_SO_RCVTIMEO  0x1006
#define ASEG_FETCH_CAP         (16 * 1024 * 1024)

#define ASEG_BUDGET_SEGMENT_US (25ULL * 1000 * 1000)
#define ASEG_BUDGET_PLAYLIST_US (6ULL * 1000 * 1000)
// ASEG_CH_UI: a big M3U/IPTV list legitimately takes longer than a playlist.
#define ASEG_BUDGET_UI_US      (15ULL * 1000 * 1000)

typedef struct {
    int  lastStatus;        // last parsed HTTP status (0 = unparseable)
    char lastLine[28];      // first bytes of the last status line
    int  badReuse, badHop;  // was the failing request on a REUSED socket, and which redirect hop
    char badLine[28];       // status line OF THE FAILING request
    char badPath[64];       // request path we sent
    int  badPathLen;        // full length before truncation into badPath
    char badStage[16];      // where the failing request died: dns/connect/tls/req/hdr
} AsegDiag;

typedef struct {
    OrbisPthreadMutex mtx;          // one fetch at a time on this channel
    int          stream;            // VIDEO/AUDIO/PLAYLIST: abortable, sends the urlopt headers
    int          sock;
    tls_ctx     *tls;
    volatile int abort;             // sticky until aseg_resume(); see aseg_fetch_inner
    volatile unsigned abortGen;     // bumped by every aseg_abort(); see kaGen
    // Per-fetch state, written under mtx (one fetch per channel at a time).
    // t0 = start of the current fetch, so do_request's header loop is bounded
    // by the SAME budget as the body loops.
    uint64_t     t0, budgetUs;
    char         host[256];
    char         path[1024];
    uint16_t     port;
    uint32_t     addr;
    int          tlsmode;
    char         redirect[1400];    // Location resolution scratch, off the small prefetch-thread stacks
    // HTTP keep-alive: live HLS hits the same CDN host for every segment, so reusing
    // the TLS connection (no per-segment handshake) is the difference between fetch
    // at ~realtime (can't build a buffer) and ~2.5x realtime (buffer fills, smooth).
    int          kaAlive;           // sock/tls are open + reusable
    char         kaHost[256];
    uint16_t     kaPort;
    int          kaTlsmode;
    unsigned     kaGen;             // abortGen when kept: a socket that outlived an abort is never reused
    AsegDiag     d;
} AsegCh;

static AsegCh       g_ch[ASEG_CH_COUNT];
static int          g_chInit = 0;
static volatile int g_playlistBudget = 0;   // aseg_set_playlist_budget(): tight PLAYLIST budget
// What aseg_last_status() & co report: the stream channel whose fetch failed
// last (PLAYLIST after aseg_clear_error). UI failures stay in their channel.
static AsegCh *volatile g_diagCh = &g_ch[ASEG_CH_PLAYLIST];

// Shared by every channel, under g_shareMtx (never held across network I/O).
static OrbisPthreadMutex g_shareMtx;
static int g_pool = -1;
static char g_nativeHost[256] = "";          // host whose CDN rejected BearSSL but accepted SceHttp

static const char *ci_strstr(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (!nl) return hay;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl) {
            char a = hay[i], b = needle[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            i++;
        }
        if (i == nl) return hay;
    }
    return NULL;
}

// One resolver pool for all channels (still one pool, as before), sized for a
// concurrent resolver per channel: 16 KB each, what the single channel had.
static int net_pool(void) {
    scePthreadMutexLock(&g_shareMtx);
    if (g_pool < 0) g_pool = sceNetPoolCreate("ps4cast_aseg", ASEG_CH_COUNT * 16 * 1024, 0);
    int pool = g_pool;
    scePthreadMutexUnlock(&g_shareMtx);
    return pool;
}

static void conn_close(AsegCh *c) {
    if (c->tls) { tls_close(c->tls); c->tls = NULL; }
    // Clear before closing: aseg_abort() reads sock from another thread.
    if (c->sock >= 0) { int s = c->sock; c->sock = -1; sceNetSocketClose(s); }
}

// Stream channels only: what the player's request context says to send.
static const char *opt_headers(AsegCh *c) { return c->stream ? urlopt_headers() : ""; }

static void diag_clear(AsegCh *c) {
    c->d.lastStatus = -1; c->d.lastLine[0] = '\0';
    c->d.badReuse = c->d.badHop = c->d.badPathLen = -1;
    c->d.badLine[0] = c->d.badPath[0] = '\0';
    c->d.badStage[0] = '\0';
}

static void diag_stage(AsegCh *c, const char *stage) {
    snprintf(c->d.badStage, sizeof(c->d.badStage), "%s", stage);
    if (c->stream) g_diagCh = c;
}

static int parse_url(AsegCh *c, const char *url) {
    if (strncmp(url, "http://", 7) == 0)       { c->tlsmode = 0; url += 7; c->port = 80; }
    else if (strncmp(url, "https://", 8) == 0) { c->tlsmode = 1; url += 8; c->port = 443; }
    else return -1;

    const char *slash = strchr(url, '/');
    const char *hostend = slash ? slash : url + strlen(url);
    char hostport[300];
    int hl = (int)(hostend - url);
    if (hl <= 0 || hl >= (int)sizeof(hostport)) return -2;
    memcpy(hostport, url, hl); hostport[hl] = '\0';

    char *colon = strchr(hostport, ':');
    if (colon) { *colon = '\0'; int p = atoi(colon + 1); if (p > 0) c->port = (uint16_t)p; }
    strncpy(c->host, hostport, sizeof(c->host) - 1); c->host[sizeof(c->host) - 1] = '\0';

    if (slash) { strncpy(c->path, slash, sizeof(c->path) - 1); c->path[sizeof(c->path) - 1] = '\0'; }
    else strcpy(c->path, "/");
    return 0;
}

// Small DNS cache: live HLS hits the same CDN host for every segment, so
// resolving each time added 100s of ms of variable latency per fetch (a big part
// of the segment-fetch time that kept the prefetcher from getting ahead).
// Shared by all channels, so a stream's playlist fetch warms it for its
// segments; a few entries, so video/audio/UI hosts don't evict each other.
#define ASEG_DNS_SLOTS 4
static struct { char host[256]; uint32_t addr; } g_dns[ASEG_DNS_SLOTS];
static unsigned g_dnsNext = 0;

static int dns_cached(const char *host, uint32_t *addr) {
    int hit = 0;
    scePthreadMutexLock(&g_shareMtx);
    for (int i = 0; i < ASEG_DNS_SLOTS && !hit; i++)
        if (g_dns[i].addr && strcmp(g_dns[i].host, host) == 0) { *addr = g_dns[i].addr; hit = 1; }
    scePthreadMutexUnlock(&g_shareMtx);
    return hit;
}

static void dns_store(const char *host, uint32_t addr) {
    scePthreadMutexLock(&g_shareMtx);
    int slot = -1;
    for (int i = 0; i < ASEG_DNS_SLOTS && slot < 0; i++)
        if (strcmp(g_dns[i].host, host) == 0) slot = i;
    if (slot < 0) slot = (int)(g_dnsNext++ % ASEG_DNS_SLOTS);
    snprintf(g_dns[slot].host, sizeof(g_dns[slot].host), "%s", host);
    g_dns[slot].addr = addr;
    scePthreadMutexUnlock(&g_shareMtx);
}

static int resolve_host(AsegCh *c) {
    OrbisNetInAddr a;
    memset(&a, 0, sizeof(a));
    if (sceNetInetPton(ORBIS_NET_AF_INET, c->host, &a.s_addr) > 0) { c->addr = a.s_addr; return 0; }
    if (dns_cached(c->host, &c->addr)) return 0;                                         // cache hit
    int pool = net_pool();
    if (pool < 0) return -1;
    OrbisNetId rid = sceNetResolverCreate("ps4cast_ares", pool, 0);
    if (rid < 0) return -2;
    // 4s x2 (max 8s), not the original 8s x3 (24s) and not 3s x1. 24s ran before
    // the fetch budget could bail and blew the channel-switch watchdog grace, but
    // a single 3s try was too strict the other way: it failed on hosts that
    // resolve fine (a channel whose server answers in 0.4s from a PC reported
    // "hls fetch failed" on console). Two tries with a sane timeout covers a
    // transient DNS hiccup while staying well inside the grace.
    watchdog_kick();
    const char *pv_dns = watchdog_note("dns");   // unabortable; the one blocking call we cannot pet through
    int rc = sceNetResolverStartNtoa(rid, c->host, &a, 4 * 1000 * 1000, 2, 0);
    watchdog_note(pv_dns);
    watchdog_kick();
    sceNetResolverDestroy(rid);
    if (rc < 0) return -3;
    c->addr = a.s_addr;
    dns_store(c->host, a.s_addr);                                                        // cache it
    return 0;
}

#define ORBIS_NET_SO_NBIO 0x1200   // non-blocking I/O socket option

static int tcp_connect(AsegCh *c) {
    if (c->abort) return -1;                 // already tearing down: don't begin a fresh connect
    int s = sceNetSocket("ps4cast_aseg", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_STREAM, 0);
    if (s < 0) return s;
    int tmo = 2 * 1000 * 1000;   // 2s: caps how long an in-flight segment recv blocks before the teardown abort check breaks it
    sceNetSetsockopt(s, ORBIS_NET_SOL_SOCKET, ORBIS_NET_SO_RCVTIMEO, &tmo, sizeof(tmo));
    sceNetSetsockopt(s, ORBIS_NET_SOL_SOCKET, ORBIS_NET_SO_SNDTIMEO, &tmo, sizeof(tmo));
    ps4_sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.len = sizeof(sa); sa.family = ORBIS_NET_AF_INET;
    sa.port = sceNetHtons(c->port); sa.addr = c->addr;

    // Non-blocking connect with a bounded budget. A blocking connect to a slow/dead
    // segment server can't be aborted (sceNetSocketAbort doesn't interrupt it), so it
    // wedged a channel switch's player_stop for ~13s. Here we start the connect
    // non-blocking, then poll a zero-length send (writable == connected) up to ~4s,
    // bailing immediately on teardown (abort), then restore blocking I/O.
    int nb = 1; sceNetSetsockopt(s, ORBIS_NET_SOL_SOCKET, ORBIS_NET_SO_NBIO, &nb, sizeof(nb));
    c->sock = s;
    sceNetConnect(s, (const OrbisNetSockaddr *)&sa, sizeof(sa));   // returns in-progress
    int connected = 0;
    uint64_t t0 = sceKernelGetProcessTime();
    uint64_t connectBudgetUs = c->budgetUs == ASEG_BUDGET_SEGMENT_US
                             ? 6000ULL * 1000 : 2500ULL * 1000;
    if (c->t0) {
        uint64_t elapsed = t0 - c->t0;
        if (elapsed >= c->budgetUs) connectBudgetUs = 0;
        else if (connectBudgetUs > c->budgetUs - elapsed)
            connectBudgetUs = c->budgetUs - elapsed;
    }
    sceKernelUsleep(15000);                                        // let the handshake start before probing
    while (sceKernelGetProcessTime() - t0 < connectBudgetUs) {
        if (c->abort) break;
        if (sceNetSend(s, "", 0, 0) >= 0) { connected = 1; break; } // writable -> connected
        sceKernelUsleep(20000);
    }
    // Stay NON-BLOCKING for request/read. SO_RCVTIMEO is not reliably honored on
    // this stack -- a single recv was measured blocking ~36s with a 2s timeout set
    // ("HANG stale=36s at=eof/read"), which no budget check around the call can
    // bound. conn_read/conn_write below poll with their own deadline instead,
    // exactly like this connect does.
    if (!connected) { c->sock = -1; sceNetSocketClose(s); return -1; }
    return s;
}

// No-progress ceiling for a single read/write on a non-blocking aseg socket.
#define ASEG_STALL_US (4ULL * 1000 * 1000)

static int aseg_past_budget(AsegCh *c) {
    return c->t0 && (sceKernelGetProcessTime() - c->t0) > c->budgetUs;
}

static int conn_read(AsegCh *c, uint8_t *buf, int len) {
    if (c->tls) return tls_read(c->tls, buf, len);
    uint64_t t0 = sceKernelGetProcessTime();
    int spins = 0;
    for (;;) {
        if (c->abort) return -1;
        int n = sceNetRecv(c->sock, buf, len, 0);
        if (n > 0) return n;
        if (n == 0) return 0;                                   // peer closed: clean EOF
        if (sceKernelGetProcessTime() - t0 > ASEG_STALL_US) return -1;
        if (aseg_past_budget(c)) return -1;
        watchdog_kick();
        // Adaptive: a flat 3ms sleep per not-ready read throttled throughput badly
        // (TLS reassembles a record over many small reads). Spin first, then back off.
        if (spins < 64) { spins++; sceKernelUsleep(200); }
        else sceKernelUsleep(2000);
    }
}
static int conn_write(AsegCh *c, const uint8_t *buf, int len) {
    if (c->tls) return tls_write(c->tls, buf, len);
    int sent = 0;
    uint64_t t0 = sceKernelGetProcessTime();
    while (sent < len) {
        if (c->abort) return -1;
        int n = sceNetSend(c->sock, buf + sent, len - sent, 0);
        if (n > 0) { sent += n; t0 = sceKernelGetProcessTime(); continue; }
        if (sceKernelGetProcessTime() - t0 > ASEG_STALL_US) return -1;
        if (aseg_past_budget(c)) return -1;
        watchdog_kick();
        sceKernelUsleep(3000);
    }
    return 0;
}

void aseg_abort(void) {
    // The three STREAM channels only. ASEG_CH_UI (the phone's IPTV import) is
    // not owned by playback: a Stop or zap must not kill it, and it never
    // needs aseg_resume().
    for (int i = 0; i < ASEG_CH_COUNT; i++) {
        AsegCh *c = &g_ch[i];
        if (!c->stream) continue;
        c->abort = 1;
        c->abortGen++;      // after abort=1: a fetch that reads the new gen also sees abort
        // An aborted socket must never be reused. aseg keeps connections alive across
        // channel switches, and a burst fires many aborts, so the NEXT tune could pick
        // up a socket that had sceNetSocketAbort() called on it -- where SO_RCVTIMEO
        // is not reliably honored and recv can block indefinitely. That matches the
        // failure exactly: only after a burst, only on the first tune. (kaAlive alone
        // could be re-set by a fetch finishing just after this; kaGen cannot.)
        c->kaAlive = 0;
        int s = c->sock;
        if (s >= 0) sceNetSocketAbort(s, 0);
        native_http_abort(i);
    }
}

// Clear a STALE abort at the start of a new stream. player_stop() raises the abort
// to unblock in-flight fetches; that flag then survived into the next channel's
// FIRST playlist fetch, which bailed immediately with rc=-9 ("hls fetch failed").
// The flag self-clears on that failed fetch, so the next attempt succeeded — which
// is why the failure looked random (measured 8/15 channel tunes failing, always
// rc=-9, on sources answering 200 in 0.3s from a PC).
//
// NOTE: this does NOT make the flag sticky. aseg_fetch keeps its own per-fetch
// clear; an earlier attempt to remove that in favour of this call wedged fetching
// entirely, because aseg_abort() is also raised from live-playback paths.
// (Clears all three stream channels together, exactly as aseg_abort raises them.)
void aseg_resume(void) {
    for (int i = 0; i < ASEG_CH_COUNT; i++) if (g_ch[i].stream) g_ch[i].abort = 0;
}

void aseg_clear_error(void) {
    for (int i = 0; i < ASEG_CH_COUNT; i++) if (g_ch[i].stream) diag_clear(&g_ch[i]);
    g_diagCh = &g_ch[ASEG_CH_PLAYLIST];
}

static int preferred_native_url(const char *url) {
    if (!url) return 0;
    const char *p = strstr(url, "://");
    if (!p) return 0;
    p += 3;
    scePthreadMutexLock(&g_shareMtx);
    size_t n = strlen(g_nativeHost);
    int hit = n && strncasecmp(p, g_nativeHost, n) == 0 &&
              (p[n] == '/' || p[n] == ':' || p[n] == '\0');
    scePthreadMutexUnlock(&g_shareMtx);
    return hit;
}

static void native_pin(const char *host) {   // NULL = unpin
    scePthreadMutexLock(&g_shareMtx);
    snprintf(g_nativeHost, sizeof(g_nativeHost), "%s", host ? host : "");
    scePthreadMutexUnlock(&g_shareMtx);
}

// SceHttp retry on this channel's own native slot: aseg_abort() reaches it,
// and the channel's header policy applies (UI: none).
static int native_fetch(AsegCh *c, const char *url, uint8_t **outBuf, int *outLen, int *status) {
    return native_http_fetch((int)(c - g_ch), url, opt_headers(c), outBuf, outLen, status,
                             c->budgetUs, &c->abort);
}

// One request: connect, GET (Connection: close), parse headers. On 3xx returns
// 1 and copies Location into `loc`. On 2xx returns 0 and leaves the connection
// positioned at the first body byte, with header-adjacent body bytes in *lead.
static int do_request(AsegCh *c, int reuse, int *status, char *loc, int loccap,
                      uint8_t *lead, int *leadLen, long *clen, int *chunked) {
    if (!reuse) {
        conn_close(c);
        watchdog_note("aseg/connect");
        c->sock = tcp_connect(c);
        if (c->sock < 0) { diag_stage(c, "connect"); return -1; }
        if (c->tlsmode) {
            const char *pv_tls = watchdog_note("tls");   // handshake to a half-open host can block for seconds
            c->tls = tls_open_bounded(c->sock, c->host, c->t0 + c->budgetUs);
            watchdog_note(pv_tls); watchdog_kick();
            if (!c->tls) { conn_close(c); diag_stage(c, "tls"); return -2; }
        }
    }

    char req[1600];
    // ASEG_CH_UI is not part of the stream: never send the stream's Referer/
    // Cookie/UA to an unrelated list host -- the default User-Agent only.
    const char *xh = opt_headers(c);
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "%s"
        "%s"
        "Accept: */*\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        c->path, c->host, xh,
        strstr(xh, "User-Agent:") ? "" : "User-Agent: PS4Cast/1.0\r\n");
    if (n <= 0 || n >= (int)sizeof(req)) { conn_close(c); diag_stage(c, "req"); return -3; }
    // Re-arm every request: a kept-alive TLS context outlives the fetch that
    // created it, so a deadline set at handshake time would already be in the
    // past on the next fetch and fail every read instantly.
    if (c->tls) tls_set_read_deadline(c->tls, c->t0 + c->budgetUs);
    watchdog_note("aseg/req-send");
    if (conn_write(c, (const uint8_t *)req, n) != 0) { conn_close(c); diag_stage(c, "req"); return -3; }
    watchdog_note("aseg/hdr-read");

    char hdr[4096];
    int hl = 0, end = -1;
    while (hl < (int)sizeof(hdr) - 1) {
        if (c->abort) { conn_close(c); diag_stage(c, "hdr"); return -9; }
        // Budget + watchdog, same as the body loops. Without them a server that
        // TRICKLES its response headers blocks the main thread indefinitely: each
        // conn_read can sit for the 2s RCVTIMEO and still return a byte, so r<=0
        // never fires, and this runs up to 4095 iterations -- twice per hop, five
        // hops. Nothing here petted the watchdog, which is the
        // "HANG stale=35s stage=source-open at=open/load-variant" fail-close.
        if (sceKernelGetProcessTime() - c->t0 > c->budgetUs) {
            conn_close(c); diag_stage(c, "hdr"); return -4;
        }
        watchdog_kick();
        int r = conn_read(c, (uint8_t *)hdr + hl, (int)sizeof(hdr) - 1 - hl);
        if (r <= 0) break;
        hl += r; hdr[hl] = '\0';
        char *e = strstr(hdr, "\r\n\r\n");
        if (e) { end = (int)(e - hdr) + 4; break; }
    }
    if (end < 0) { conn_close(c); diag_stage(c, "hdr"); return -4; }

    int st = 0;
    if (strncmp(hdr, "HTTP/", 5) == 0) { const char *sp = strchr(hdr, ' '); if (sp) st = atoi(sp + 1); }
    if (status) *status = st;
    { char *ll = c->d.lastLine; int k = 0; while (k < (int)sizeof(c->d.lastLine) - 1 && k < hl && hdr[k] != '\r' && hdr[k] != '\n') { char ch = hdr[k]; ll[k] = (ch >= 32 && ch < 127) ? ch : '.'; k++; } ll[k] = '\0'; }

    if (clen) {
        *clen = -1;
        const char *cl = ci_strstr(hdr, "Content-Length:");
        if (cl) *clen = atol(cl + (int)strlen("Content-Length:"));
    }
    if (chunked) {
        const char *te = ci_strstr(hdr, "Transfer-Encoding:");
        const char *eol = te ? strstr(te, "\r\n") : NULL;
        const char *coding = te ? ci_strstr(te, "chunked") : NULL;
        *chunked = coding && (!eol || coding < eol);
        if (*chunked && clen) *clen = -1;  // Transfer-Encoding wins over Content-Length.
    }

    if (st >= 300 && st < 400 && loc && loccap) {
        loc[0] = '\0';
        const char *l = ci_strstr(hdr, "Location:");
        if (l) {
            l += 9; while (*l == ' ' || *l == '\t') l++;
            const char *eol = strstr(l, "\r\n");
            int ln = eol ? (int)(eol - l) : (int)strlen(l);
            while (ln > 0 && (l[ln - 1] == ' ' || l[ln - 1] == '\t')) ln--;
            if (ln >= loccap) ln = loccap - 1;
            memcpy(loc, l, ln); loc[ln] = '\0';
        }
        return 1;  // redirect
    }

    int avail = hl - end;
    if (avail > 0) { memcpy(lead, hdr + end, avail); }
    *leadLen = avail > 0 ? avail : 0;
    return 0;
}

typedef struct {
    AsegCh *c;
    const uint8_t *lead;
    int leadLen;
    int leadPos;
} ChunkReader;

static int chunk_read_some(ChunkReader *r, uint8_t *out, int len) {
    if (r->leadPos < r->leadLen) {
        int n = r->leadLen - r->leadPos;
        if (n > len) n = len;
        memcpy(out, r->lead + r->leadPos, (size_t)n);
        r->leadPos += n;
        return n;
    }
    return conn_read(r->c, out, len);
}

static int chunk_read_exact(ChunkReader *r, uint8_t *out, size_t len) {
    size_t done = 0;
    while (done < len) {
        if (r->c->abort || sceKernelGetProcessTime() - r->c->t0 > r->c->budgetUs) return -1;
        watchdog_kick();
        int want = (len - done) > INT32_MAX ? INT32_MAX : (int)(len - done);
        int got = chunk_read_some(r, out + done, want);
        if (got <= 0) return -1;
        done += (size_t)got;
    }
    return 0;
}

static int chunk_read_line(ChunkReader *r, char *line, int cap) {
    int n = 0;
    for (;;) {
        uint8_t c;
        if (chunk_read_exact(r, &c, 1) != 0) return -1;
        if (c == '\n') {
            if (n > 0 && line[n - 1] == '\r') n--;
            line[n] = '\0';
            return n;
        }
        if (n >= cap - 1) return -1;
        line[n++] = (char)c;
    }
}

// Decode RFC 7230 chunk framing while reading, so a chunk-size line can never
// leak into the HLS parser as a fake segment URI. The terminating chunk and all
// trailers are consumed, leaving same-host connections safe for keep-alive.
static int read_chunked_body(AsegCh *c, const uint8_t *lead, int leadLen, uint8_t **outBuf, int *outLen) {
    ChunkReader reader = { c, lead, leadLen, 0 };
    size_t cap = 256 * 1024, used = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) return -6;

    for (;;) {
        char line[128];
        int lineLen = chunk_read_line(&reader, line, sizeof(line));
        if (lineLen < 0) { free(buf); return -11; }

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        char *end = NULL;
        unsigned long long chunk = strtoull(p, &end, 16);
        if (end == p) { free(buf); return -13; }
        while (*end == ' ' || *end == '\t') end++;
        if (*end && *end != ';') { free(buf); return -13; }

        if (chunk == 0) {
            // Optional trailer fields end at the first blank line.
            do {
                lineLen = chunk_read_line(&reader, line, sizeof(line));
                if (lineLen < 0) { free(buf); return -11; }
            } while (lineLen != 0);
            break;
        }
        if (chunk > ASEG_FETCH_CAP || used > ASEG_FETCH_CAP - (size_t)chunk) {
            free(buf); return -10;
        }

        size_t need = used + (size_t)chunk;
        if (need > cap) {
            size_t next = cap;
            while (next < need && next < ASEG_FETCH_CAP) {
                size_t grown = next * 2;
                next = grown > ASEG_FETCH_CAP ? ASEG_FETCH_CAP : grown;
            }
            if (next < need) { free(buf); return -10; }
            uint8_t *larger = realloc(buf, next);
            if (!larger) { free(buf); return -7; }
            buf = larger; cap = next;
        }
        if (chunk_read_exact(&reader, buf + used, (size_t)chunk) != 0) {
            free(buf); return -11;
        }
        used += (size_t)chunk;

        uint8_t crlf[2];
        if (chunk_read_exact(&reader, crlf, sizeof(crlf)) != 0 || crlf[0] != '\r' || crlf[1] != '\n') {
            free(buf); return -13;
        }
    }

    if (used == 0) { free(buf); return -8; }
    *outBuf = buf;
    *outLen = (int)used;
    return 0;
}

// The response ended exactly at its framing: keep the socket for the next
// same-host fetch on this channel -- unless an abort raced this fetch (gen0 is
// then stale, so the reuse check refuses it).
static void keep_alive(AsegCh *c, unsigned gen0) {
    c->kaAlive = 1; c->kaGen = gen0; c->kaPort = c->port; c->kaTlsmode = c->tlsmode;
    strncpy(c->kaHost, c->host, sizeof(c->kaHost) - 1); c->kaHost[sizeof(c->kaHost) - 1] = '\0';
}

// Hard ceiling on ONE fetch (all redirect hops + the body read). Without it a
// dead host could burn ~2.5s connect + 3s read per hop across 5 hops, and
// hls_open does TWO fetches (master + variant) — enough to block the main thread
// past the 35s channel-switch watchdog grace and fail-close the app. Observed as
// "HANG watchdog stale=36002ms" while zapping through unreachable channels.
// Default budget for a SEGMENT fetch. Segments are large and a slow-but-working
// CDN legitimately needs many seconds — an over-tight cap here makes every slow
// segment fail and the player rebuffer continuously (observed after this was set
// to a flat 9s for everything). Playlists are small, so hls_open lowers it around
// the master/variant fetches, where the real risk is blocking a channel switch.
// That lowering applies to ASEG_CH_PLAYLIST only: VIDEO/AUDIO segment fetches
// keep the segment budget even while hls_open runs.

void aseg_set_playlist_budget(int on) {
    g_playlistBudget = on ? 1 : 0;
}

static int aseg_fetch_inner(AsegCh *c, const char *url, uint8_t **outBuf, int *outLen) {
    uint64_t budget0 = sceKernelGetProcessTime();
    c->t0 = budget0;
    // Abort is STICKY until aseg_resume(). It must not self-clear here.
    //
    // Every fetch is serialized on its channel lock. Clearing the flag on entry
    // meant a teardown's abort was consumed by whichever fetch ran next, so a thread
    // already QUEUED on the mutex (past its own stop-flag check, unable to see it)
    // then started a brand-new full-budget fetch. hls_close()'s unbounded
    // scePthreadJoin waited for it: ~25s segment budget on top of the fetch in
    // flight ~= the 36s that fail-closed the app on every
    // "HANG stale=36s stage=source-open" since v03.81.
    //
    // v03.65 removed this clear WITHOUT adding a resume point and wedged fetching
    // permanently. The resume calls in hls.c (after hls_close, and at
    // prefetch_start/apref_start) are what make stickiness safe.
    unsigned gen0 = c->abortGen;     // read BEFORE abort (see aseg_abort)
    if (c->abort) { *outBuf = NULL; *outLen = 0; return -9; }
    *outBuf = NULL; *outLen = 0;

    // Some Cloudflare configurations reject BearSSL's TLS fingerprint even when
    // the URL and browser compatibility headers are valid. Once SceHttp proves
    // itself for a host, keep using the PS4's native TLS stack for that host.
    if (preferred_native_url(url)) {
        int status = 0;
        int rc = native_fetch(c, url, outBuf, outLen, &status);
        if (rc == 0 && (status == 200 || status == 206)) {
            diag_clear(c);
            c->d.lastStatus = status;
            snprintf(c->d.lastLine, sizeof(c->d.lastLine), "native HTTP");
            return 0;
        }
        if (*outBuf) { free(*outBuf); *outBuf = NULL; *outLen = 0; }
        // An abort (Stop, zap, ABR switch) says nothing about the host: keep the
        // pin, or every other channel pays a BearSSL failure to rediscover it.
        if (!c->abort) native_pin(NULL);
    }

    char cur[1400];
    strncpy(cur, url, sizeof(cur) - 1); cur[sizeof(cur) - 1] = '\0';

    uint8_t lead[4096]; int leadLen = 0; long clen = -1; int chunked = 0;
    int opened = 0;
    for (int hop = 0; hop < 5 && !opened; hop++) {
        if (c->abort) { conn_close(c); c->kaAlive = 0; return -9; }   // bail between redirect hops on teardown
        if (sceKernelGetProcessTime() - budget0 > c->budgetUs) {
            conn_close(c); c->kaAlive = 0; return -12;               // dead/slow host: fail fast
        }
        watchdog_kick();
        if (parse_url(c, cur) != 0) { conn_close(c); c->kaAlive = 0; return -1; }
        if (resolve_host(c) != 0) { conn_close(c); c->kaAlive = 0; diag_stage(c, "dns"); return -2; }
        // Reuse the kept-alive socket if it's to the same host:port:tls.
        // This was disabled in v04.12 to test a response-desync theory. That theory
        // was disproved (the wrong-playlist symptom turned out to be the main thread
        // stuck in a retry loop), and leaving it off costs a full TCP+TLS handshake
        // on EVERY segment -- measured open=1.6-3.4s against 0.3s from a PC.
        int reuse = (c->kaAlive && c->kaGen == c->abortGen && c->sock >= 0 && c->kaPort == c->port &&
                     c->kaTlsmode == c->tlsmode && strcmp(c->kaHost, c->host) == 0);
        int status = 0; char loc[1400];
        int rc = do_request(c, reuse, &status, loc, sizeof(loc), lead, &leadLen, &clen, &chunked);
        if (rc != 0 && reuse) {            // stale keep-alive socket -> fresh connect once
            conn_close(c); c->kaAlive = 0;
            rc = do_request(c, 0, &status, loc, sizeof(loc), lead, &leadLen, &clen, &chunked);
        }
        // Location may be relative ("/path", "seg.ts", "?x=1"); parse_url only
        // takes absolute http(s) URLs, so resolve it against the URL that
        // answered (RFC 7231 7.1.2) instead of failing the next hop.
        if (rc == 1 && loc[0]) {
            hlspl_resolve_ref(cur, loc, c->redirect, sizeof(c->redirect));
            snprintf(cur, sizeof(cur), "%s", c->redirect);
            c->kaAlive = 0; continue;
        }
        if (rc != 0) {
            conn_close(c); c->kaAlive = 0;
            // A hard failure BEFORE any HTTP status — connect refused/timeout,
            // TLS handshake stall or rejection, request write stall — on an
            // HTTPS origin gets one bounded retry through the PS4's native
            // SceHttp stack. Cloudflare edges were observed hanging BearSSL
            // handshakes instead of answering 403, which left this fetch with
            // no fallback at all (rc=-3 len=0 st=-1). Same pinning rule as the
            // 403 path: success keeps using SceHttp for exactly this host.
            if (!c->abort && c->tlsmode) {
                int nstatus = 0;
                int nrc = native_fetch(c, cur, outBuf, outLen, &nstatus);
                if (nrc == 0 && (nstatus == 200 || nstatus == 206)) {
                    native_pin(c->host);
                    diag_clear(c);
                    c->d.lastStatus = nstatus;
                    snprintf(c->d.lastLine, sizeof(c->d.lastLine), "native HTTP");
                    return 0;
                }
                if (*outBuf) { free(*outBuf); *outBuf = NULL; *outLen = 0; }
            }
            return -3;
        }
        if (status == 403 && c->stream && urlopt_has_page_headers()) {
            // Browser extensions can observe the outer page before the exact
            // manifest request headers arrive. Some CDNs reject that incorrect
            // context but intentionally accept no Referer/Origin. Retry clean
            // once, retaining UA/Cookie, and keep the winning policy for all
            // variant and segment requests in this stream. (Stream channels
            // only: UI sends no page headers and must not flip stream policy.)
            conn_close(c); c->kaAlive = 0;
            urlopt_set_page_headers_enabled(0);
            leadLen = 0; clen = -1; chunked = 0;
            int clean_status = 0;
            int clean_rc = do_request(c, 0, &clean_status, loc, sizeof(loc),
                                      lead, &leadLen, &clen, &chunked);
            if (clean_rc == 0 && (clean_status == 200 || clean_status == 206)) {
                status = clean_status;
                diag_clear(c);
                c->d.lastStatus = status;
                snprintf(c->d.lastLine, sizeof(c->d.lastLine), "clean-header HTTP");
            } else {
                urlopt_set_page_headers_enabled(1);
                status = clean_status ? clean_status : status;
            }
        }
        if (status != 200 && status != 206) {
            c->d.lastStatus = status;           // capture HERE: a later OK fetch must not overwrite it
            c->d.badReuse = reuse; c->d.badHop = hop;
            snprintf(c->d.badLine, sizeof(c->d.badLine), "%s", c->d.lastLine);
            c->d.badPathLen = (int)strlen(c->path);
            snprintf(c->d.badPath, sizeof(c->d.badPath), "%s", c->path);
            if (c->stream) g_diagCh = c;
            conn_close(c); c->kaAlive = 0;
            // Retry an HTTPS 403 once through the native PS4 HTTP/TLS stack. A
            // successful retry pins only this host to SceHttp; all other origins
            // stay on the established BearSSL path.
            if (c->tlsmode && status == 403) {
                int native_status = 0;
                int native_rc = native_fetch(c, cur, outBuf, outLen, &native_status);
                if (native_rc == 0 && (native_status == 200 || native_status == 206)) {
                    native_pin(c->host);
                    diag_clear(c);
                    c->d.lastStatus = native_status;
                    snprintf(c->d.lastLine, sizeof(c->d.lastLine), "native HTTP");
                    return 0;
                }
                if (*outBuf) { free(*outBuf); *outBuf = NULL; *outLen = 0; }
            }
            return -4;
        }
        opened = 1;
    }
    if (!opened) { conn_close(c); c->kaAlive = 0; return -5; }

    if (chunked) {
        watchdog_note("aseg/body-chunked");
        int rc = read_chunked_body(c, lead, leadLen, outBuf, outLen);
        if (rc != 0) { conn_close(c); c->kaAlive = 0; return rc; }
        keep_alive(c, gen0);
        return 0;
    }

    watchdog_note("aseg/alloc");
    size_t cap = 256 * 1024, used = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { conn_close(c); c->kaAlive = 0; return -6; }
    if (leadLen > 0) { memcpy(buf, lead, leadLen); used = leadLen; }

    if (clen >= 0) {
        // Known length: read exactly Content-Length bytes and KEEP the socket open
        // for the next same-host segment (skips the TLS handshake — the big win).
        watchdog_note("aseg/body-known");
        size_t need = (size_t)clen;
        if (need > ASEG_FETCH_CAP) { free(buf); conn_close(c); c->kaAlive = 0; return -10; }
        while (used < need) {
            if (c->abort) { free(buf); conn_close(c); c->kaAlive = 0; return -9; }
            // Budget + watchdog, exactly as the unknown-length loop below has.
            // Their absence here was the long-standing zap crash: a server that
            // sends Content-Length and then TRICKLES the body keeps conn_read
            // returning >0, so the 2s RCVTIMEO never fires, the loop never exits,
            // and NOTHING pets the watchdog -> an unbounded block inside
            // aseg_fetch_inner, past DNS and TLS ("HANG stale=35s
            // stage=source-open at=open/load-variant"). It showed up only on the
            // first tune AFTER a burst because aseg keeps sockets alive across
            // channel switches, so a burst leaves a degraded keep-alive
            // connection that then trickles.
            if (sceKernelGetProcessTime() - budget0 > c->budgetUs) {
                free(buf); conn_close(c); c->kaAlive = 0; return -12;
            }
            watchdog_kick();
            if (used + 64 * 1024 > cap) {
                size_t ncap = cap * 2; if (ncap < need) ncap = need;
                if (ncap > ASEG_FETCH_CAP) ncap = ASEG_FETCH_CAP;
                uint8_t *nb = realloc(buf, ncap);
                if (!nb) { free(buf); conn_close(c); c->kaAlive = 0; return -7; }
                buf = nb; cap = ncap;
            }
            int want = (int)(need - used); if (want > (int)(cap - used)) want = (int)(cap - used);
            int r = conn_read(c, buf + used, want);
            if (r <= 0) { free(buf); conn_close(c); c->kaAlive = 0; return -11; }  // short read -> fail+reconnect
            used += r;
        }
        keep_alive(c, gen0);                                   // reusable next time
    } else {
        watchdog_note("aseg/body-eof");
        // Unknown length (no Content-Length): read to EOF, then close (no reuse).
        for (;;) {
            if (c->abort) { free(buf); conn_close(c); c->kaAlive = 0; return -9; }
            if (sceKernelGetProcessTime() - budget0 > c->budgetUs) {
                free(buf); conn_close(c); c->kaAlive = 0; return -12;
            }
            watchdog_kick();
            if (used + 64 * 1024 > cap) {
                watchdog_note("eof/grow");
                size_t ncap = cap * 2;
                if (ncap > ASEG_FETCH_CAP) ncap = ASEG_FETCH_CAP;
                if (ncap <= cap) { free(buf); conn_close(c); c->kaAlive = 0; return -10; }
                uint8_t *nb = realloc(buf, ncap);
                if (!nb) { free(buf); conn_close(c); c->kaAlive = 0; return -7; }
                buf = nb; cap = ncap;
            }
            watchdog_note("eof/read");
            int r = conn_read(c, buf + used, (int)(cap - used));
            if (r > 0) { used += r; continue; }
            break;
        }
        conn_close(c); c->kaAlive = 0;
    }

    if (used == 0) { free(buf); return -8; }
    *outBuf = buf; *outLen = (int)used;
    return 0;
}

// Create the channel locks ONCE, from main(), before any thread can fetch.
//
// This used to be a lazy "if (!inited) { init(); inited = 1; }" inside
// aseg_fetch. Several threads fetch (main, ps4cast_hlsp, ps4cast_hlsap, the
// segment fetch/decode threads, httpd), and a channel-switch burst restarts both
// prefetch threads repeatedly, so two could enter that window together and BOTH
// run scePthreadMutexInit over the same storage. The second init replaces the
// handle the first thread is locking, so that thread's unlock releases a
// different object than it acquired and the lock stays held forever -- every
// later aseg_fetch then blocks indefinitely.
//
// That is the long-standing zap crash: "HANG stale=36s stage=source-open
// at=open/load-variant", always on the FIRST tune after a burst, never during it.
int aseg_last_status(void) { return g_diagCh->d.lastStatus; }
int aseg_bad_reuse(void) { return g_diagCh->d.badReuse; }
int aseg_bad_hop(void) { return g_diagCh->d.badHop; }
const char *aseg_last_line(void) { AsegCh *c = g_diagCh; return c->d.badLine[0] ? c->d.badLine : c->d.lastLine; }
const char *aseg_bad_path(void) { return g_diagCh->d.badPath; }
int aseg_bad_pathlen(void) { return g_diagCh->d.badPathLen; }
const char *aseg_bad_stage(void) { return g_diagCh->d.badStage; }
const char *aseg_native_debug(void) { return native_http_debug(); }

void aseg_init(void) {
    static const char *names[ASEG_CH_COUNT] = {
        "ps4cast_aseg_v", "ps4cast_aseg_a", "ps4cast_aseg_p", "ps4cast_aseg_u" };
    if (g_chInit) return;
    for (int i = 0; i < ASEG_CH_COUNT; i++) {
        AsegCh *c = &g_ch[i];
        c->stream = (i != ASEG_CH_UI);
        c->sock = -1; c->port = 80;
        diag_clear(c);
        scePthreadMutexInit(&c->mtx, NULL, names[i]);
    }
    scePthreadMutexInit(&g_shareMtx, NULL, "ps4cast_aseg_s");
    native_http_init();     // its locks too: first native fetches can race from two channels
    g_chInit = 1;
}

int aseg_fetch_ch(int ch, const char *url, uint8_t **outBuf, int *outLen) {
    if (!g_chInit || ch < 0 || ch >= ASEG_CH_COUNT) return -1;   // aseg_init() not run: fail, never race an init
    AsegCh *c = &g_ch[ch];
    const char *pv = watchdog_note("aseg/lock");
    scePthreadMutexLock(&c->mtx);
    watchdog_note(pv);
    c->budgetUs = ch == ASEG_CH_UI ? ASEG_BUDGET_UI_US
                : (ch == ASEG_CH_PLAYLIST && g_playlistBudget) ? ASEG_BUDGET_PLAYLIST_US
                : ASEG_BUDGET_SEGMENT_US;
    int rc = aseg_fetch_inner(c, url, outBuf, outLen);
    scePthreadMutexUnlock(&c->mtx);
    return rc;
}

int aseg_fetch_ui(const char *url, uint8_t **outBuf, int *outLen) {
    return aseg_fetch_ch(ASEG_CH_UI, url, outBuf, outLen);
}

int aseg_fetch(const char *url, uint8_t **outBuf, int *outLen) {
    return aseg_fetch_ch(ASEG_CH_PLAYLIST, url, outBuf, outLen);
}
