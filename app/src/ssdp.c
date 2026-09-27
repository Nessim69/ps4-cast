#include "ssdp.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <orbis/Net.h>
#include <orbis/libkernel.h>

typedef struct {
    uint8_t  len;
    uint8_t  family;
    uint16_t port;
    uint32_t addr;
    uint8_t  zero[8];
} ps4_sockaddr_in;

#define SSDP_PORT 1900
#define SOL_SOCKET_PS4   0xffff
#define SO_REUSEADDR_PS4 0x0004
#define SO_RCVTIMEO_PS4  0x1006

// OrbisNet (SceNet) IP-level socket option numbers. These are Sony's own
// values and differ from BSD's <netinet/in.h>; the OpenOrbis headers don't
// export them, so define the ones we need. The multicast group JOIN is the
// piece the earlier in-app SSDP builds were missing: without it the socket is
// bound to :1900 but never actually receives the M-SEARCH multicast, so the
// PS4 stayed invisible to cast apps.
#define ORBIS_NET_IPPROTO_IP        0
#define ORBIS_NET_IP_MULTICAST_IF   9
#define ORBIS_NET_IP_MULTICAST_TTL  10
#define ORBIS_NET_IP_ADD_MEMBERSHIP 12
#define ORBIS_NET_IP_DROP_MEMBERSHIP 13

// Every earlier build advertised this SAME hard-coded UUID from every
// install, so two consoles on one LAN shared one SSDP USN / description.xml
// UDN and looked like a single flaky device to control points. Replaced by a
// per-install UUID (uuid_load_or_create below); kept only as the fallback if
// persistence is unavailable, so discovery still works rather than crashing.
#define PS4CAST_UUID_FALLBACK "uuid:7b2f63a8-2530-4e47-9f3a-0000000c5701"

// SceNet ip_mreq: two in_addr (network byte order), interface 0 = INADDR_ANY.
typedef struct {
    uint32_t imr_multiaddr;
    uint32_t imr_interface;
} ps4_ip_mreq;

// 239.255.255.250 in network byte order, built byte-wise so it is correct
// regardless of host endianness.
static uint32_t mcast_group_addr(void) {
    const uint8_t b[4] = { 239, 255, 255, 250 };
    uint32_t v;
    memcpy(&v, b, 4);
    return v;
}

static OrbisNetId g_sock = -1;
static OrbisPthread g_thread;
static char g_ip[32] = "0.0.0.0";
static int g_http_port = 8080;
static char g_diag[160] = "ssdp not started";
static unsigned g_seen = 0;
static char g_last_st[96] = "";
static char g_last_from[32] = "";

const char *ssdp_status(void) { return g_diag; }

// ---- per-install UUID ------------------------------------------------------
#define UUID_PATH   "/data/ps4cast_uuid.txt"
#define UUID_STRLEN 41   // "uuid:" + 8-4-4-4-12 hex
static char g_uuid[48] = PS4CAST_UUID_FALLBACK;
static int  g_uuid_ready = 0;

// RFC 4122 4.4 "version 4" UUID: mix jittered TSC samples (same technique as
// the pairing token's splitmix64 whitening in httpd.c's token_generate; not
// shared code because it's this file's only use of it and pulling in
// pairing.h for one function felt like more coupling than it's worth) into 16
// bytes, then force the version/variant nibbles the RFC requires.
static void uuid_generate(char *out) {
    uint8_t b[16];
    uint64_t state = sceKernelGetProcessTime() ^ (uint64_t)(uintptr_t)&state;
    for (int i = 0; i < 16; i++) {
        state ^= sceKernelReadTsc();
        uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        z ^= z >> 31;
        b[i] = (uint8_t)z;
        for (volatile int j = 0, spin = (int)(z & 0x3F); j < spin; j++) {}   // jitter
    }
    b[6] = (uint8_t)((b[6] & 0x0F) | 0x40);   // version 4
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80);   // variant 10 (RFC 4122)
    snprintf(out, UUID_STRLEN + 1,
             "uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

// Load the per-install UUID, minting and persisting one on first run (or if
// the file is missing/corrupt). Idempotent and cheap after the first call, so
// both ssdp_start and description.xml's handler (via ssdp_uuid, possibly
// before ssdp_start has run) can call it without coordinating.
static void ensure_uuid(void) {
    if (g_uuid_ready) return;
    g_uuid_ready = 1;   // set first: a corrupt file must not retry forever
    int fd = sceKernelOpen(UUID_PATH, 0 /*O_RDONLY*/, 0);
    if (fd >= 0) {
        char buf[64] = {0};
        int n = (int)sceKernelRead(fd, buf, sizeof(buf) - 1);
        sceKernelClose(fd);
        if (n == UUID_STRLEN && strncmp(buf, "uuid:", 5) == 0) {
            memcpy(g_uuid, buf, UUID_STRLEN);
            g_uuid[UUID_STRLEN] = '\0';
            return;
        }
    }
    uuid_generate(g_uuid);
    fd = sceKernelOpen(UUID_PATH, 0x0201 /*O_WRONLY|O_CREAT*/ | 0x0400 /*O_TRUNC*/, 0666);
    if (fd >= 0) { sceKernelWrite(fd, g_uuid, UUID_STRLEN); sceKernelClose(fd); }
}

const char *ssdp_uuid(void) { ensure_uuid(); return g_uuid; }

static void addr_to_str(uint32_t addr, char *out, int cap) {
    const uint8_t *b = (const uint8_t *)&addr;
    snprintf(out, cap, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

static void record_search(const ps4_sockaddr_in *from, const char *req) {
    g_seen++;
    addr_to_str(from->addr, g_last_from, sizeof(g_last_from));
    const char *st = NULL;
    for (const char *p = req; *p; p++) {
        int line_start = (p == req) || (p[-1] == '\n');
        if (line_start && (p[0] == 'S' || p[0] == 's') &&
            (p[1] == 'T' || p[1] == 't') && p[2] == ':') {
            st = p + 3;
            break;
        }
    }
    if (st) {
        while (*st == ' ' || *st == '\t') st++;
        const char *e = strstr(st, "\r\n");
        int n = e ? (int)(e - st) : (int)strlen(st);
        if (n >= (int)sizeof(g_last_st)) n = (int)sizeof(g_last_st) - 1;
        memcpy(g_last_st, st, n);
        g_last_st[n] = '\0';
    }
    snprintf(g_diag, sizeof(g_diag), "ssdp up ip=%s seen=%u from=%s st=%s",
             g_ip, g_seen, g_last_from, g_last_st);
}

static int contains_ci(const char *hay, const char *needle) {
    int nl = (int)strlen(needle);
    if (nl <= 0) return 1;
    for (int i = 0; hay[i]; i++) {
        int ok = 1;
        for (int j = 0; j < nl; j++) {
            char a = hay[i + j];
            char b = needle[j];
            if (!a) return 0;
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (a != b) { ok = 0; break; }
        }
        if (ok) return 1;
    }
    return 0;
}

static void send_ssdp_response(const ps4_sockaddr_in *to, const char *st) {
    char msg[1024];
    char usn[160];
    const char *uuid = ssdp_uuid();
    if (strcmp(st, uuid) == 0)
        snprintf(usn, sizeof(usn), "%s", uuid);
    else
        snprintf(usn, sizeof(usn), "%s::%s", uuid, st);
    int n = snprintf(msg, sizeof(msg),
        "HTTP/1.1 200 OK\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "EXT:\r\n"
        "LOCATION: http://%s:%d/description.xml\r\n"
        "SERVER: FreeBSD/9.0 UPnP/1.0 PS4-Cast/1.0\r\n"
        "ST: %s\r\n"
        "USN: %s\r\n"
        "BOOTID.UPNP.ORG: 1\r\n"
        "CONFIGID.UPNP.ORG: 1\r\n"
        "\r\n",
        g_ip, g_http_port, st, usn);
    sceNetSendto(g_sock, msg, n, 0, (const OrbisNetSockaddr *)to, sizeof(*to));
}

// Proactively announce ourselves to the multicast group. Some control points
// (including several phone cast apps) populate their device list from these
// ssdp:alive advertisements rather than issuing their own M-SEARCH, so sending
// a burst at startup makes the PS4 appear without the user re-scanning.
static void send_ssdp_alive(const char *st) {
    char msg[1024];
    char usn[160];
    const char *uuid = ssdp_uuid();
    if (strcmp(st, uuid) == 0)
        snprintf(usn, sizeof(usn), "%s", uuid);
    else
        snprintf(usn, sizeof(usn), "%s::%s", uuid, st);
    int n = snprintf(msg, sizeof(msg),
        "NOTIFY * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "LOCATION: http://%s:%d/description.xml\r\n"
        "SERVER: FreeBSD/9.0 UPnP/1.0 PS4-Cast/1.0\r\n"
        "NT: %s\r\n"
        "NTS: ssdp:alive\r\n"
        "USN: %s\r\n"
        "BOOTID.UPNP.ORG: 1\r\n"
        "CONFIGID.UPNP.ORG: 1\r\n"
        "\r\n",
        g_ip, g_http_port, st, usn);

    ps4_sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.len    = sizeof(to);
    to.family = ORBIS_NET_AF_INET;
    to.port   = sceNetHtons(SSDP_PORT);
    to.addr   = mcast_group_addr();
    sceNetSendto(g_sock, msg, n, 0, (const OrbisNetSockaddr *)&to, sizeof(to));
}

// ssdp:byebye counterpart to send_ssdp_alive, for ssdp_shutdown. No
// CACHE-CONTROL/LOCATION: byebye just retracts the NT, it doesn't refresh it.
static void send_ssdp_byebye(const char *st) {
    char msg[512];
    char usn[160];
    const char *uuid = ssdp_uuid();
    if (strcmp(st, uuid) == 0)
        snprintf(usn, sizeof(usn), "%s", uuid);
    else
        snprintf(usn, sizeof(usn), "%s::%s", uuid, st);
    int n = snprintf(msg, sizeof(msg),
        "NOTIFY * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "NT: %s\r\n"
        "NTS: ssdp:byebye\r\n"
        "USN: %s\r\n"
        "BOOTID.UPNP.ORG: 1\r\n"
        "\r\n",
        st, usn);

    ps4_sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.len    = sizeof(to);
    to.family = ORBIS_NET_AF_INET;
    to.port   = sceNetHtons(SSDP_PORT);
    to.addr   = mcast_group_addr();
    sceNetSendto(g_sock, msg, n, 0, (const OrbisNetSockaddr *)&to, sizeof(to));
}

static void announce_all(void) {
    send_ssdp_alive(ssdp_uuid());
    send_ssdp_alive("upnp:rootdevice");
    send_ssdp_alive("urn:schemas-upnp-org:device:MediaRenderer:1");
    send_ssdp_alive("urn:schemas-upnp-org:service:ConnectionManager:1");
    send_ssdp_alive("urn:schemas-upnp-org:service:AVTransport:1");
    send_ssdp_alive("urn:schemas-upnp-org:service:RenderingControl:1");
}

// Send ssdp:byebye for every NT we advertise. Called once at app exit (main.c)
// so control points drop us immediately instead of waiting out the
// (now 1800s) max-age on our last ssdp:alive.
void ssdp_shutdown(void) {
    if (g_sock < 0) return;
    send_ssdp_byebye(ssdp_uuid());
    send_ssdp_byebye("upnp:rootdevice");
    send_ssdp_byebye("urn:schemas-upnp-org:device:MediaRenderer:1");
    send_ssdp_byebye("urn:schemas-upnp-org:service:ConnectionManager:1");
    send_ssdp_byebye("urn:schemas-upnp-org:service:AVTransport:1");
    send_ssdp_byebye("urn:schemas-upnp-org:service:RenderingControl:1");
}

// Address changes (netmon.c) are handed over here and applied by the SSDP
// thread, the only thread that uses the socket and g_ip after ssdp_start.
static char         g_pendIp[32];
static volatile int g_ipDirty;
static volatile int g_ipLock;

void ssdp_set_ip(const char *ip) {
    if (g_sock < 0 || !ip) return;
    while (__atomic_exchange_n(&g_ipLock, 1, __ATOMIC_ACQUIRE)) sceKernelUsleep(50);
    snprintf(g_pendIp, sizeof(g_pendIp), "%s", ip);
    g_ipDirty = 1;
    __atomic_store_n(&g_ipLock, 0, __ATOMIC_RELEASE);
}

// Join the SSDP group on the interface that owns `ip` and send from it.
// INADDR_ANY for the membership interface does not reliably bind to wlan0 on
// this stack, which is why discovery failed before the join named it.
static void join_on(const char *ip, int *jrc, int *mif) {
    uint32_t ifaddr = 0;   // INADDR_ANY fallback
    sceNetInetPton(ORBIS_NET_AF_INET, ip, &ifaddr);
    ps4_ip_mreq mreq;
    mreq.imr_multiaddr = mcast_group_addr();
    mreq.imr_interface = ifaddr;
    *jrc = sceNetSetsockopt(g_sock, ORBIS_NET_IPPROTO_IP, ORBIS_NET_IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    *mif = sceNetSetsockopt(g_sock, ORBIS_NET_IPPROTO_IP, ORBIS_NET_IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr));
}

// SSDP thread: move to a new address handed over by ssdp_set_ip.
static void apply_new_ip(void) {
    char ip[32];
    while (__atomic_exchange_n(&g_ipLock, 1, __ATOMIC_ACQUIRE)) sceKernelUsleep(50);
    snprintf(ip, sizeof(ip), "%s", g_pendIp);
    g_ipDirty = 0;
    __atomic_store_n(&g_ipLock, 0, __ATOMIC_RELEASE);
    // Leave the group on the old interface (it may already be gone: ignore
    // the result), then join on the new one. Also done for the same address
    // coming back, since a link drop can take the membership with it.
    uint32_t old = 0;
    sceNetInetPton(ORBIS_NET_AF_INET, g_ip, &old);
    ps4_ip_mreq mreq;
    mreq.imr_multiaddr = mcast_group_addr();
    mreq.imr_interface = old;
    sceNetSetsockopt(g_sock, ORBIS_NET_IPPROTO_IP, ORBIS_NET_IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
    snprintf(g_ip, sizeof(g_ip), "%s", ip);
    int jrc, mif;
    join_on(g_ip, &jrc, &mif);
    snprintf(g_diag, sizeof(g_diag), "ssdp moved ip=%s join=%d mif=%d", g_ip, jrc, mif);
    for (int i = 0; i < 2; i++) announce_all();
}

// How often ssdp:alive repeats once the socket is up. Well under the
// CACHE-CONTROL max-age (1800s) above so a passive control point (one that
// only ever listens for alive/byebye and never sends its own M-SEARCH) never
// sees us expire, without re-announcing so often it's just wire noise.
#define SSDP_ANNOUNCE_INTERVAL_US (120ULL * 1000000ULL)

static void *ssdp_main(void *arg) {
    (void)arg;
    char buf[2048];
    // Initial advertisement burst (UPnP recommends repeating the alive set).
    for (int i = 0; i < 3; i++) announce_all();
    uint64_t lastAnnounce = sceKernelGetProcessTime();
    for (;;) {
        ps4_sockaddr_in from;
        OrbisNetSocklen_t fromlen = sizeof(from);
        int n = sceNetRecvfrom(g_sock, buf, sizeof(buf) - 1, 0,
                               (OrbisNetSockaddr *)&from, &fromlen);
        // The receive timeout set in ssdp_start (well under the announce
        // interval) guarantees this loop wakes up on its own even with zero
        // M-SEARCH traffic, so the periodic re-announce below can't be
        // starved by a blocking recvfrom that never returns.
        uint64_t now = sceKernelGetProcessTime();
        if (g_ipDirty) { apply_new_ip(); lastAnnounce = now; }
        if (now - lastAnnounce >= SSDP_ANNOUNCE_INTERVAL_US) {
            announce_all();
            lastAnnounce = now;
        }
        if (n <= 0) {
            // A healthy socket returns here on the receive timeout as well as
            // on a transient error; back off either way to avoid pegging a
            // core (the busy-spin that destabilized the earlier in-app SSDP
            // builds).
            sceKernelUsleep(200 * 1000);
            continue;
        }
        buf[n] = '\0';
        if (!contains_ci(buf, "M-SEARCH") || !contains_ci(buf, "SSDP:DISCOVER"))
            continue;
        record_search(&from, buf);

        const char *uuid = ssdp_uuid();
        if (contains_ci(buf, uuid)) {
            send_ssdp_response(&from, uuid);
        } else if (contains_ci(buf, "MediaRenderer")) {
            send_ssdp_response(&from, "urn:schemas-upnp-org:device:MediaRenderer:1");
        } else if (contains_ci(buf, "ConnectionManager")) {
            send_ssdp_response(&from, "urn:schemas-upnp-org:service:ConnectionManager:1");
        } else if (contains_ci(buf, "AVTransport")) {
            send_ssdp_response(&from, "urn:schemas-upnp-org:service:AVTransport:1");
        } else if (contains_ci(buf, "RenderingControl")) {
            send_ssdp_response(&from, "urn:schemas-upnp-org:service:RenderingControl:1");
        } else if (contains_ci(buf, "ssdp:all") || contains_ci(buf, "upnp:rootdevice")) {
            send_ssdp_response(&from, uuid);
            send_ssdp_response(&from, "upnp:rootdevice");
            send_ssdp_response(&from, "urn:schemas-upnp-org:device:MediaRenderer:1");
            send_ssdp_response(&from, "urn:schemas-upnp-org:service:ConnectionManager:1");
            send_ssdp_response(&from, "urn:schemas-upnp-org:service:AVTransport:1");
            send_ssdp_response(&from, "urn:schemas-upnp-org:service:RenderingControl:1");
        }
    }
    return NULL;
}

int ssdp_start(const char *ip, int http_port) {
    if (g_sock >= 0) return 0;
    ensure_uuid();   // load/mint before the first announce_all() needs it
    strncpy(g_ip, ip ? ip : "0.0.0.0", sizeof(g_ip) - 1);
    g_ip[sizeof(g_ip) - 1] = '\0';
    g_http_port = http_port;

    g_sock = sceNetSocket("ps4cast_ssdp", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_DGRAM, 0);
    if (g_sock < 0) { snprintf(g_diag, sizeof(g_diag), "ssdp socket failed %d", g_sock); return -1; }

    int on = 1;
    sceNetSetsockopt(g_sock, SOL_SOCKET_PS4, SO_REUSEADDR_PS4, &on, sizeof(on));

    // Bound the blocking recvfrom in ssdp_main so the periodic re-announce
    // (and an address change from ssdp_set_ip) runs even when nothing ever
    // arrives on the wire, rather than a separate timer thread for it.
    int rcvtmo = 5 * 1000 * 1000;
    sceNetSetsockopt(g_sock, SOL_SOCKET_PS4, SO_RCVTIMEO_PS4, &rcvtmo, sizeof(rcvtmo));

    ps4_sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.len = sizeof(addr);
    addr.family = ORBIS_NET_AF_INET;
    addr.port = sceNetHtons(SSDP_PORT);
    addr.addr = 0;

    if (sceNetBind(g_sock, (const OrbisNetSockaddr *)&addr, sizeof(addr)) < 0) {
        snprintf(g_diag, sizeof(g_diag), "ssdp bind :1900 failed");
        sceNetSocketClose(g_sock);
        g_sock = -1;
        return -2;
    }

    // Join the SSDP multicast group on our LAN interface so we actually
    // receive M-SEARCH probes, and send alive/responses out of it.
    int jrc, mif;
    join_on(g_ip, &jrc, &mif);
    int ttl = 2;
    sceNetSetsockopt(g_sock, ORBIS_NET_IPPROTO_IP, ORBIS_NET_IP_MULTICAST_TTL,
                     &ttl, sizeof(ttl));

    snprintf(g_diag, sizeof(g_diag), "ssdp up ip=%s join=%d mif=%d", g_ip, jrc, mif);

    if (scePthreadCreate(&g_thread, NULL, ssdp_main, NULL, "ps4cast_ssdp") != 0) {
        sceNetSocketClose(g_sock);
        g_sock = -1;
        return -3;
    }
    return 0;
}
