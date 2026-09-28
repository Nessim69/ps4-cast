// logo.c — channel logos on the TV. See logo.h.
#include "logo.h"
#include "logo_image.h"
#include "aseg.h"
#include "httpd.h"
#include "httpd_channels.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <orbis/libkernel.h>

#define LOGO_SLOTS    160               // decoded logos kept (at most ~60 KB each)
#define LOGO_QUEUE    24                // pending downloads; the newest ask goes first
#define LOGO_DONE     16
#define LOGO_BYTES    (1 << 20)         // a larger file is cut short and fails to decode
#define LOGO_BUDGET   (8ULL * 1000 * 1000)
#define LOGO_RETRY_US (10ULL * 60 * 1000 * 1000)
#define URL_CAP       512

// ---- main thread only ---------------------------------------------------------
typedef struct {
    uint64_t  key;                      // 0 = empty
    uint32_t *px;                       // NULL = failed
    int       w, h;
    uint32_t  id;
    uint64_t  used, at;
} Slot;
static Slot     g_slot[LOGO_SLOTS];
static uint64_t g_frame;
static uint32_t g_idSeq;

// ---- shared with the worker (under g_mtx) ---------------------------------------
typedef struct { uint64_t key; char url[URL_CAP]; } Req;
typedef struct { uint64_t key; uint32_t *px; int w, h; } Done;
static OrbisPthreadMutex g_mtx;
static Req      g_q[LOGO_QUEUE];
static int      g_qn;
static Done     g_done[LOGO_DONE];
static int      g_dn;
static uint64_t g_busyKey;
static int      g_up;

static uint64_t key_of(const char *s) {
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return h ? h : 1;
}

static void *logo_worker(void *arg) {
    (void)arg;
    for (;;) {
        Req r;
        int have = 0;
        scePthreadMutexLock(&g_mtx);
        if (g_qn > 0 && g_dn < LOGO_DONE) {
            r = g_q[--g_qn];                           // newest first: what is on screen now
            g_busyKey = r.key;
            have = 1;
        }
        scePthreadMutexUnlock(&g_mtx);
        if (!have) { sceKernelUsleep(100 * 1000); continue; }

        Done d = { r.key, NULL, 0, 0 };
        uint8_t *buf = NULL;
        int len = 0;
        AsegOpts o;
        memset(&o, 0, sizeof(o));
        o.maxBytes = LOGO_BYTES;
        o.budgetUs = LOGO_BUDGET;
        if (aseg_fetch_opts(ASEG_CH_IMG, r.url, &buf, &len, &o) == 0 && buf && len > 0)
            d.px = logo_image_decode(buf, len, LOGO_MAX_W, LOGO_MAX_H, &d.w, &d.h);
        free(buf);
        scePthreadMutexLock(&g_mtx);
        g_done[g_dn++] = d;
        g_busyKey = 0;
        scePthreadMutexUnlock(&g_mtx);
    }
    return NULL;
}

void logo_init(void) {
    if (g_up) return;
    scePthreadMutexInit(&g_mtx, NULL, "ps4cast_logo");
    OrbisPthreadAttr attr;
    OrbisPthreadAttr *pattr = NULL;
    int attrInit = scePthreadAttrInit(&attr) == 0;
    // stb_image decodes on this stack (PNG/JPEG state is heap, but be generous).
    if (attrInit && scePthreadAttrSetstacksize(&attr, 512 * 1024) == 0) pattr = &attr;
    OrbisPthread t;
    if (scePthreadCreate(&t, pattr, logo_worker, NULL, "ps4cast_logo") == 0) {
        scePthreadDetach(t);
        g_up = 1;
    }
    if (attrInit) scePthreadAttrDestroy(&attr);
}

void logo_tick(void) {
    if (!g_up) return;
    g_frame++;
    Done got[LOGO_DONE];
    int n;
    scePthreadMutexLock(&g_mtx);
    n = g_dn;
    memcpy(got, g_done, sizeof(Done) * (size_t)n);
    g_dn = 0;
    scePthreadMutexUnlock(&g_mtx);
    uint64_t now = sceKernelGetProcessTime();
    for (int i = 0; i < n; i++) {
        // The slot of a retried logo, else an empty one, else the one drawn
        // longest ago. Nothing is queued for drawing at this point in the
        // frame, so its pixels can go.
        Slot *v = NULL;
        for (int k = 0; k < LOGO_SLOTS && !v; k++) if (g_slot[k].key == got[i].key) v = &g_slot[k];
        for (int k = 0; k < LOGO_SLOTS && !v; k++) if (!g_slot[k].key) v = &g_slot[k];
        if (!v) {
            v = &g_slot[0];
            for (int k = 1; k < LOGO_SLOTS; k++) if (g_slot[k].used < v->used) v = &g_slot[k];
        }
        free(v->px);
        v->key = got[i].key;
        v->px = got[i].px;
        v->w = got[i].w; v->h = got[i].h;
        v->id = ++g_idSeq | 0x40000000u;
        v->used = g_frame;
        v->at = now;
    }
}

// Queue a download unless it is already queued, running or just finished.
static void request(uint64_t key, const char *url) {
    scePthreadMutexLock(&g_mtx);
    int known = g_busyKey == key;
    for (int i = 0; i < g_qn && !known; i++) known = g_q[i].key == key;
    for (int i = 0; i < g_dn && !known; i++) known = g_done[i].key == key;
    if (!known) {
        if (g_qn == LOGO_QUEUE) {                     // full: the oldest ask goes
            memmove(g_q, g_q + 1, sizeof(Req) * (LOGO_QUEUE - 1));
            g_qn--;
        }
        g_q[g_qn].key = key;
        snprintf(g_q[g_qn].url, sizeof(g_q[g_qn].url), "%s", url);
        g_qn++;
    }
    scePthreadMutexUnlock(&g_mtx);
}

int logo_draw(Gfx *g, int chan, int x, int y, int w, int h) {
    if (!g_up || w <= 0 || h <= 0) return 0;
    char url[URL_CAP];
    if (!httpd_chan_meta(chan, NULL, 0, url, sizeof(url)) || !url[0]) return 0;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) return 0;
    uint64_t key = key_of(url);
    Slot *s = NULL;
    for (int k = 0; k < LOGO_SLOTS; k++) if (g_slot[k].key == key) { s = &g_slot[k]; break; }
    if (!s) { request(key, url); return 0; }
    s->used = g_frame;
    if (!s->px) {                                     // failed: try again after a while
        if (sceKernelGetProcessTime() - s->at > LOGO_RETRY_US) { s->at = sceKernelGetProcessTime(); request(key, url); }
        return 0;
    }
    int dw = w, dh = (int)((int64_t)s->h * w / s->w);
    if (dh > h) { dh = h; dw = (int)((int64_t)s->w * h / s->h); }
    if (dw < 1 || dh < 1) return 0;
    gfx_image(g, x + (w - dw) / 2, y + (h - dh) / 2, dw, dh, s->px, s->w, s->h, s->id);
    return 1;
}
