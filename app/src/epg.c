// epg.c — programme-guide download thread and lookups. See epg.h.
#include "epg.h"
#include "guide.h"
#include "xmltv.h"
#include "inflate.h"
#include "aseg.h"
#include "httpd.h"
#include "httpd_channels.h"
#include "wallclock.h"
#include "netmon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <orbis/libkernel.h>

#define EPG_CACHE      "/data/ps4cast_epg.cache"   // the guide as downloaded (usually gzip)
#define EPG_CACHE_PART "/data/ps4cast_epg.part"
#define EPG_META       "/data/ps4cast_epg.meta"    // "<unix time>\t<url>\n"
#define EPG_CACHE_MAX  (96u << 20)                 // bigger downloads are not kept
#define EPG_REFRESH    (12 * 3600)
#define EPG_RETRY      (10 * 60)
#define EPG_RESLIDE    (6 * 3600)                  // re-read the cache so the window keeps up
#define EPG_BUDGET_US  (8ULL * 60 * 1000 * 1000)
#define URL_CAP        1024

static OrbisPthreadMutex g_mtx;
static Guide  *g_guide;                            // current guide (read under g_mtx)
static int     g_ver;
static char    g_override[URL_CAP];
static char    g_status[200] = "No guide yet";
static volatile int g_refresh, g_wake;

void epg_init(void) {
    scePthreadMutexInit(&g_mtx, NULL, "ps4cast_epg");
}

static void set_status(const char *fmt, const char *arg, long a, long b) {
    char s[sizeof(g_status)];
    snprintf(s, sizeof(s), fmt, arg ? arg : "", a, b);
    scePthreadMutexLock(&g_mtx);
    memcpy(g_status, s, sizeof(s));
    scePthreadMutexUnlock(&g_mtx);
}

void epg_status(char *out, int cap) {
    scePthreadMutexLock(&g_mtx);
    snprintf(out, (size_t)cap, "%s", g_status);
    scePthreadMutexUnlock(&g_mtx);
}

int epg_version(void) {
    scePthreadMutexLock(&g_mtx);
    int v = g_ver;
    scePthreadMutexUnlock(&g_mtx);
    return v;
}

int epg_loaded(void) {
    scePthreadMutexLock(&g_mtx);
    int on = g_guide != NULL;
    scePthreadMutexUnlock(&g_mtx);
    return on;
}

void epg_set_url(const char *url) {
    scePthreadMutexLock(&g_mtx);
    snprintf(g_override, sizeof(g_override), "%s", url ? url : "");
    scePthreadMutexUnlock(&g_mtx);
    g_wake = 1;
}

void epg_get_url(char *out, int cap) {
    scePthreadMutexLock(&g_mtx);
    snprintf(out, (size_t)cap, "%s", g_override);
    scePthreadMutexUnlock(&g_mtx);
}

void epg_source(char *out, int cap) {
    epg_get_url(out, cap);
    if (!out[0]) httpd_channels_epg_url(out, cap);
    if (strncmp(out, "http://", 7) != 0 && strncmp(out, "https://", 8) != 0) out[0] = '\0';
}

void epg_refresh(void) { g_refresh = 1; g_wake = 1; }

// ---- lookups ------------------------------------------------------------------
static void copy_prog(EpgProgramme *o, const GuideProg *p) {
    o->start = p->start; o->stop = p->stop;
    snprintf(o->title, sizeof(o->title), "%s", p->title);
    snprintf(o->sub, sizeof(o->sub), "%s", p->sub);
    snprintf(o->cat, sizeof(o->cat), "%s", p->cat);
    snprintf(o->desc, sizeof(o->desc), "%s", p->desc);
}

// Guide channel of playlist channel `chan` (caller holds g_mtx).
static int find_locked(int chan, const char *tvg, const char *name) {
    (void)chan;
    return g_guide ? guide_find(g_guide, tvg, name) : -1;
}

int epg_now_next(int chan, int64_t now, EpgProgramme *onAir, EpgProgramme *next) {
    char tvg[128], name[96];
    if (!httpd_chan_meta(chan, tvg, sizeof(tvg), NULL, 0)) return 0;
    httpd_chan_get(chan, name, sizeof(name), NULL, 0);
    int got = 0;
    scePthreadMutexLock(&g_mtx);
    int gch = find_locked(chan, tvg, name);
    if (gch >= 0) {
        int on = 0, k = guide_at(g_guide, gch, now, &on);
        GuideProg p;
        if (k >= 0 && on && guide_prog(g_guide, gch, k, &p)) {
            if (onAir) copy_prog(onAir, &p);
            got |= 1;
            k++;
        }
        if (k >= 0 && guide_prog(g_guide, gch, k, &p)) {
            if (next) copy_prog(next, &p);
            got |= 2;
        }
    }
    scePthreadMutexUnlock(&g_mtx);
    return got;
}

int epg_schedule(int chan, int64_t from, EpgProgramme *out, int max) {
    char tvg[128], name[96];
    if (!httpd_chan_meta(chan, tvg, sizeof(tvg), NULL, 0)) return 0;
    httpd_chan_get(chan, name, sizeof(name), NULL, 0);
    int n = 0;
    scePthreadMutexLock(&g_mtx);
    int gch = find_locked(chan, tvg, name);
    if (gch >= 0) {
        int on = 0, k = guide_at(g_guide, gch, from, &on);
        GuideProg p;
        while (k >= 0 && n < max && guide_prog(g_guide, gch, k, &p)) { copy_prog(&out[n++], &p); k++; }
    }
    scePthreadMutexUnlock(&g_mtx);
    return n;
}

// ---- building a guide -------------------------------------------------------------
typedef struct {
    Inflate *z;
    Xmltv   *x;
    Guide   *g;
    int      fd;               // cache being written, -1 = not caching
    uint64_t in, cached, nextNote;
    int      xmlErr, zErr;
    const char *what;          // "Downloading" / "Reading"
} Pipe;

static int pipe_xml(void *ctx, const uint8_t *p, int n) {
    Pipe *pp = ctx;
    int r = xmltv_push(pp->x, (const char *)p, n);
    if (r != XMLTV_OK) { pp->xmlErr = r; return 1; }
    return 0;
}

static int pipe_in(void *ctx, const uint8_t *p, int n) {
    Pipe *pp = ctx;
    pp->in += (uint64_t)n;
    if (pp->fd >= 0) {
        if (pp->cached + (uint64_t)n > EPG_CACHE_MAX ||
            (long)sceKernelWrite(pp->fd, p, (size_t)n) != (long)n) {
            sceKernelClose(pp->fd);
            pp->fd = -1;
            sceKernelUnlink(EPG_CACHE_PART);
        } else {
            pp->cached += (uint64_t)n;
        }
    }
    int r = inflate_push(pp->z, p, n);
    if (r < 0) { pp->zErr = 1; return 1; }
    if (r == INFLATE_STOP) return 1;                 // the XML side gave up
    if (pp->in >= pp->nextNote) {
        pp->nextNote = pp->in + (1u << 20);
        set_status("%s guide: %ld MB, %ld programmes", pp->what, (long)(pp->in >> 20), guide_programmes(pp->g));
    }
    return 0;
}

static int read_meta(char *url, int cap, int64_t *at) {
    int fd = sceKernelOpen(EPG_META, 0, 0);
    if (fd < 0) return 0;
    char buf[URL_CAP + 32];
    long n = sceKernelRead(fd, buf, sizeof(buf) - 1);
    sceKernelClose(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    char *tab = strchr(buf, '\t');
    if (!tab) return 0;
    *at = strtoll(buf, NULL, 10);
    char *nl = strchr(tab + 1, '\n');
    if (nl) *nl = '\0';
    snprintf(url, (size_t)cap, "%s", tab + 1);
    return 1;
}

static void write_meta(const char *url, int64_t at) {
    char buf[URL_CAP + 32];
    int n = snprintf(buf, sizeof(buf), "%lld\t%s\n", (long long)at, url);
    int fd = sceKernelOpen(EPG_META, 0x0201 | 0x0400, 0666);
    if (fd >= 0) { sceKernelWrite(fd, buf, (size_t)n); sceKernelClose(fd); }
}

static GuideWant *want_snapshot(void) {
    GuideWant *w = guide_want_new();
    if (!w) return NULL;
    int n = httpd_chan_count();
    char tvg[128], name[96];
    for (int i = 0; i < n; i++) {
        if (!httpd_chan_meta(i, tvg, sizeof(tvg), NULL, 0)) break;
        httpd_chan_get(i, name, sizeof(name), NULL, 0);
        guide_want_add(w, tvg, name);
    }
    return w;
}

// Build a guide from the network (fromNet) or the cached copy. 0 = installed.
static int build(const GuideWant *w, const char *url, int fromNet, int64_t now) {
    Pipe pp;
    memset(&pp, 0, sizeof(pp));
    pp.fd = -1;
    pp.what = fromNet ? "Downloading" : "Reading";
    pp.g = guide_new(w, now);
    XmltvCallbacks cb = { guide_on_channel, guide_on_programme, pp.g };
    pp.x = pp.g ? xmltv_new(&cb) : NULL;
    pp.z = pp.x ? inflate_new(pipe_xml, &pp) : NULL;
    if (!pp.z) {
        xmltv_free(pp.x); guide_free(pp.g);
        set_status("Guide: out of memory", NULL, 0, 0);
        return -1;
    }
    int rc;
    set_status("%s guide...", pp.what, 0, 0);
    if (fromNet) {
        pp.fd = sceKernelOpen(EPG_CACHE_PART, 0x0201 | 0x0400, 0666);
        AsegOpts o;
        memset(&o, 0, sizeof(o));
        o.budgetUs = EPG_BUDGET_US;
        o.sink = pipe_in; o.sinkCtx = &pp;
        uint8_t *buf = NULL; int len = 0;
        rc = aseg_fetch_opts(ASEG_CH_BG, url, &buf, &len, &o);
        free(buf);
        if (rc == ASEG_STOPPED && (pp.xmlErr || pp.zErr)) rc = -100;
    } else {
        int fd = sceKernelOpen(EPG_CACHE, 0, 0);
        rc = fd < 0 ? -1 : 0;
        uint8_t *buf = fd >= 0 ? malloc(64 * 1024) : NULL;
        for (;;) {
            if (!buf) { rc = -1; break; }
            long n = sceKernelRead(fd, buf, 64 * 1024);
            if (n <= 0) break;
            if (pipe_in(&pp, buf, (int)n)) { rc = -100; break; }
        }
        free(buf);
        if (fd >= 0) sceKernelClose(fd);
    }
    int zrc = rc == 0 ? inflate_finish(pp.z) : INFLATE_ERR;
    int xrc = rc == 0 && zrc == INFLATE_END ? xmltv_finish(pp.x) : XMLTV_ERR;
    int ok = rc == 0 && zrc == INFLATE_END && xrc == XMLTV_OK;
    if (pp.fd >= 0) {
        sceKernelClose(pp.fd);
        if (ok) {
            sceKernelUnlink(EPG_CACHE);
            if (sceKernelRename(EPG_CACHE_PART, EPG_CACHE) == 0) write_meta(url, now);
        } else {
            sceKernelUnlink(EPG_CACHE_PART);
        }
    }
    long nx = pp.x ? xmltv_programmes(pp.x) : 0;
    inflate_free(pp.z);
    xmltv_free(pp.x);
    if (!ok) {
        guide_free(pp.g);
        if (rc == -100 && pp.zErr) set_status("Guide: the download is damaged (bad gzip data)", NULL, 0, 0);
        else if (rc == -100) set_status("Guide: that link is not an XMLTV guide", NULL, 0, 0);
        else if (rc != 0) set_status(fromNet ? "Guide: download failed (%s%ld)" : "Guide: cached copy unreadable (%s%ld)",
                                     "error ", rc, 0);
        else if (zrc == INFLATE_ERR) set_status("Guide: the download is damaged (bad gzip data)", NULL, 0, 0);
        else if (zrc != INFLATE_END) set_status("Guide: the download ended early", NULL, 0, 0);
        else set_status("Guide: that link is not an XMLTV guide", NULL, 0, 0);
        return -1;
    }
    guide_finish(pp.g);
    Guide *old;
    scePthreadMutexLock(&g_mtx);
    old = g_guide;
    g_guide = pp.g;
    g_ver++;
    scePthreadMutexUnlock(&g_mtx);
    guide_free(old);
    char s[sizeof(g_status)];
    snprintf(s, sizeof(s), "Guide: %ld channels, %ld programmes%s (of %ld listed)", guide_channels(pp.g),
             guide_programmes(pp.g), guide_truncated(pp.g) ? ", some left out to save memory" : "", nx);
    scePthreadMutexLock(&g_mtx);
    memcpy(g_status, s, sizeof(s));
    scePthreadMutexUnlock(&g_mtx);
    return 0;
}

static void drop_guide(const char *why) {
    Guide *old;
    scePthreadMutexLock(&g_mtx);
    old = g_guide;
    g_guide = NULL;
    if (old) g_ver++;
    snprintf(g_status, sizeof(g_status), "%s", why);
    scePthreadMutexUnlock(&g_mtx);
    guide_free(old);
}

static void *epg_thread(void *arg) {
    (void)arg;
    char lastUrl[URL_CAP] = "";
    uint64_t lastSig = 0;
    int lastChanVer = -1, haveSig = 0;
    int64_t lastNet = 0, lastFail = 0, lastBuild = 0;
    for (;;) {
        for (int i = 0; i < 20 && !g_wake; i++) sceKernelUsleep(250 * 1000);
        g_wake = 0;
        char url[URL_CAP];
        epg_source(url, sizeof(url));
        if (!url[0]) {
            if (lastUrl[0] || !haveSig) drop_guide("No guide link: the playlist has no x-tvg-url; set one in Settings");
            lastUrl[0] = '\0';
            haveSig = 1;
            continue;
        }
        char ip[64];
        netmon_ip(ip, sizeof(ip));
        if (!ip[0]) continue;                           // no network yet: nothing to fetch with
        int64_t now = wallclock_utc();
        if (!now) { set_status("Guide: waiting for the console clock", NULL, 0, 0); continue; }
        if (httpd_chan_count() == 0) {
            if (lastBuild) drop_guide("Guide: no channels loaded");
            lastBuild = 0;
            lastUrl[0] = '\0';
            continue;
        }

        GuideWant *w = NULL;
        int cv = httpd_channels_version();
        if (cv != lastChanVer) {
            w = want_snapshot();
            if (!w) continue;
            lastChanVer = cv;
        }
        uint64_t sig = w ? guide_want_sig(w) : lastSig;
        int refresh = __atomic_exchange_n(&g_refresh, 0, __ATOMIC_SEQ_CST);
        int urlChanged = strcmp(url, lastUrl) != 0;
        int listChanged = !haveSig || sig != lastSig;
        int stale = lastNet && now - lastNet >= EPG_REFRESH;
        int slide = lastBuild && now - lastBuild >= EPG_RESLIDE;
        int retryOk = !lastFail || now - lastFail >= EPG_RETRY;
        if (!(refresh || urlChanged || listChanged || ((stale || slide) && retryOk) || (!lastBuild && retryOk))) {
            guide_want_free(w);
            continue;
        }
        if (!w) { w = want_snapshot(); if (!w) continue; sig = guide_want_sig(w); }
        // Another guide's data would only mislead while the new one loads.
        if (urlChanged && lastUrl[0]) drop_guide("Guide: switching to a new link...");

        // The copy in /data serves a playlist change or a restart, while fresh.
        char metaUrl[URL_CAP] = "";
        int64_t metaAt = 0;
        int cacheOk = read_meta(metaUrl, sizeof(metaUrl), &metaAt) && strcmp(metaUrl, url) == 0 &&
                      now >= metaAt && now - metaAt < EPG_REFRESH;
        int rc = -1;
        if (cacheOk && !refresh) {
            rc = build(w, url, 0, now);
            if (rc == 0) lastNet = metaAt;
        }
        if (rc != 0) {
            rc = build(w, url, 1, now);
            if (rc == 0) lastNet = now;
            else if (strcmp(metaUrl, url) == 0 && !g_guide) {
                char why[sizeof(g_status)];
                epg_status(why, sizeof(why));
                if (build(w, url, 0, now) == 0) set_status("%s (showing the last copy)", why, 0, 0);
            }
        }
        if (rc == 0) lastFail = 0; else lastFail = now;
        lastBuild = now;
        snprintf(lastUrl, sizeof(lastUrl), "%s", url);
        lastSig = sig;
        haveSig = 1;
        guide_want_free(w);
    }
    return NULL;
}

void epg_start(void) {
    OrbisPthreadAttr attr;
    OrbisPthreadAttr *pattr = NULL;
    int attrInit = scePthreadAttrInit(&attr) == 0;
    if (attrInit && scePthreadAttrSetstacksize(&attr, 256 * 1024) == 0) pattr = &attr;
    OrbisPthread t;
    if (scePthreadCreate(&t, pattr, epg_thread, NULL, "ps4cast_epg") == 0) scePthreadDetach(t);
    if (attrInit) scePthreadAttrDestroy(&attr);
}
