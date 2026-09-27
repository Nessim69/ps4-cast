#include "netmon.h"
#include "netwatch.h"
#include "netutil.h"
#include "httpd.h"
#include "ssdp.h"
#include "notify.h"
#include "trace.h"

#include <stdio.h>
#include <string.h>

#include <orbis/libkernel.h>

#define NETMON_POLL_US (2ULL * 1000 * 1000)

static int               g_port;
static NetWatch          g_watch;       // netmon thread only (and the first, synchronous poll)
static int               g_netInit, g_httpUp, g_ssdpUp, g_announced;
static char              g_ip[32];      // published copy, under g_lock
static volatile unsigned g_gen;
static volatile int      g_lock;

static void lock(void)   { while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) sceKernelUsleep(50); }
static void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

void netmon_ip(char *out, int cap) {
    lock();
    snprintf(out, (size_t)cap, "%s", g_ip);
    unlock();
}

unsigned netmon_generation(void) { return g_gen; }

static void publish(const char *ip) {
    lock();
    snprintf(g_ip, sizeof(g_ip), "%s", ip);
    unlock();
    __atomic_add_fetch(&g_gen, 1, __ATOMIC_RELEASE);
}

static void poll_once(void) {
    if (!g_netInit) g_netInit = (net_init() == 0);
    char ip[32] = "";
    if (g_netInit && net_get_ip(ip, sizeof(ip)) != 0) ip[0] = '\0';
    // The server listens on INADDR_ANY, so it doesn't need an address -- but
    // it did need one to be started at all, which is what left a console that
    // booted before its Wi-Fi without a web server until relaunch.
    if (g_netInit && !g_httpUp) g_httpUp = (httpd_listen(g_port) == 0);

    NetWatchEvent ev = netwatch_feed(&g_watch, ip);
    if (ev == NW_NONE) return;
    trace_mark("netmon %s ip=%s", ev == NW_UP ? "up" : ev == NW_CHANGED ? "changed" : "down",
               g_watch.ip[0] ? g_watch.ip : "-");
    publish(g_watch.ip);
    if (ev == NW_DOWN) return;
    if (!g_ssdpUp) g_ssdpUp = (ssdp_start(g_watch.ip, g_port) == 0);
    else ssdp_set_ip(g_watch.ip);
    if (!g_announced) {
        // Explicit "ready" toast: tells the user (and the deploy script's
        // /status poll) the app is fully up and accepting casts.
        notify("PS4 Cast " APP_VER " ready  -  http://%s:%d", g_watch.ip, g_port);
        g_announced = 1;
    } else {
        notify("PS4 Cast network %s  -  now at http://%s:%d",
               ev == NW_UP ? "is back" : "changed", g_watch.ip, g_port);
    }
}

static void *netmon_main(void *arg) {
    (void)arg;
    for (;;) {
        sceKernelUsleep((unsigned)NETMON_POLL_US);
        poll_once();
    }
    return NULL;
}

void netmon_start(int http_port) {
    static int started = 0;
    if (started) return;
    started = 1;
    g_port = http_port;
    netwatch_init(&g_watch);
    poll_once();
    OrbisPthread th;
    if (scePthreadCreate(&th, NULL, netmon_main, NULL, "ps4cast_netmon") != 0)
        trace_mark("netmon thread failed; address changes need a relaunch");
}
