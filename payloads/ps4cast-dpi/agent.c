// agent.c — PS4 Cast resident deploy agent (GoldHEN payload), TCP 9192.
//
// Replaces the unauthenticated agent that was built from local, never
// committed changes to DirectPackageInstaller. Same job -- remove the
// installed PS4 Cast build, install the PKG a dev host announces, then report
// "READY" once AppInstUtil sees the app -- but every command must carry an HMAC under the per-bootstrap secret (see
// agent_proto.h), the agent leaves after AGENT_IDLE_SEC without a valid
// command, and it can be stopped remotely (authenticated).
//
// Built from the pinned DirectPackageInstaller runtime (crt/syscalls/dl and
// ps4-libjbc) by payloads/ps4cast-dpi/Makefile; the install sequence follows
// DPI's own main.c.
//
// Loader note: the binary is a flat .text/.rodata/.data image. The agent's own
// state doesn't rely on .bss -- the only global is the initialised secret and
// everything else lives in one mmap'd block. The runtime's few .bss words
// (errno, libjbc) sit past the image end inside its last page, as in upstream
// DPI.
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <time.h>
#include <unistd.h>
#include "struct.h"
#include "agent_proto.h"
#include "sha256.h"

#ifndef AGENT_PORT
#define AGENT_PORT 9192
#endif
#ifndef AGENT_IDLE_SEC
#define AGENT_IDLE_SEC (8 * 3600)   // leave after 8 h without an authenticated command
#endif
#define AGENT_IO_TIMEOUT_SEC   10   // per-connection read/write bound
#define AGENT_READY_TIMEOUT_S  170  // host waits 180 s for the status line
#define AGENT_READY_STABLE     3    // consecutive AppExists polls
#define AGENT_UNINSTALL_WAIT_S 30   // for AppExists to drop after an uninstall

// Patched by scripts/push-goldhen-dpi.py before the payload is sent: exactly
// AGENT_SECRET_LEN bytes, no terminator. volatile keeps it a real memory
// object the compiler cannot fold into the comparisons below.
volatile uint8_t AgentSecret[AGENT_SECRET_LEN] = AGENT_SECRET_PLACEHOLDER;

void agent_elevate(void);   // jb.c

void *memset(void *dst, int value, size_t len) {
    unsigned char *p = (unsigned char *)dst;
    while (len--) *p++ = (unsigned char)value;
    return dst;
}
void *memcpy(void *dst, const void *src, size_t len) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (len--) *d++ = *s++;
    return dst;
}

asm("clear_stack:\nmov $0x800,%ecx\nxor %rax, %rax\n.L1:\npush %rax\nloop .L1\nadd $0x4000,%rsp\nret");
void clear_stack(void);

typedef struct {
    int (*notify)(int, const char *);
    int (*bgftRegister)(struct bgft_download_param *, int *);
    int (*bgftDebugRegister)(struct bgft_download_param *, int *);
    int (*bgftStart)(int);
    int (*appExists)(const char *, int32_t *);
    int (*appUninstall)(const char *);
    int  uid;
    int  listenFd;
    int  stop;
    int  authFailures;
    long lastActive;          // seconds (gettimeofday)
    uint8_t      secret[AGENT_SECRET_LEN];
    uint8_t      nonce[32];
    uint64_t     nonceCounter;
    uint8_t      packet[AGENT_MAX_PACKET];
    AgentRequest req;
    char         status[256];
} Agent;

// ---- small freestanding helpers --------------------------------------------
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void cat(char *out, int cap, const char *s) {
    int o = slen(out);
    for (int i = 0; s[i] && o < cap - 1; i++) out[o++] = s[i];
    out[o] = 0;
}

static void cat_hex32(char *out, int cap, uint32_t v) {
    char h[11] = "0x";
    for (int i = 7, x = 2; i >= 0; i--, x++) {
        int d = (v >> (4 * i)) & 15;
        h[x] = (char)(d < 10 ? '0' + d : 'A' + d - 10);
    }
    h[10] = 0;
    cat(out, cap, h);
}

static void cat_dec(char *out, int cap, long v) {
    char t[24]; int n = 0;
    unsigned long u = v < 0 ? (unsigned long)-v : (unsigned long)v;
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u && n < 22);
    if (v < 0) t[n++] = '-';
    char r[24]; int k = 0;
    while (n) r[k++] = t[--n];
    r[k] = 0;
    cat(out, cap, r);
}

static long now_sec(void) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return 0;
    return (long)tv.tv_sec;
}

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

// 1 while the secret is still the build-time placeholder. Compared against
// runtime-assembled pieces so the full 32-byte placeholder exists exactly
// once in the binary (the deploy script patches that single occurrence).
static int secret_unpatched(const Agent *a) {
    const char *p1 = "PS4CAST_AGENT_", *p2 = "SECRET_PLACEHOLDER";
    int n1 = slen(p1), n2 = slen(p2);
    if (n1 + n2 != AGENT_SECRET_LEN) return 1;
    for (int i = 0; i < n1; i++) if (a->secret[i] != (uint8_t)p1[i]) return 0;
    for (int i = 0; i < n2; i++) if (a->secret[n1 + i] != (uint8_t)p2[i]) return 0;
    return 1;
}

// A nonce only has to be unique per connection (it stops a captured request
// from being replayed); TSC + clock + a counter through SHA-256 is plenty.
static void new_nonce(Agent *a) {
    Sha256 s;
    sha256_init(&s);
    for (int i = 0; i < 16; i++) { uint64_t t = rdtsc(); sha256_update(&s, &t, sizeof t); }
    long t = now_sec();
    sha256_update(&s, &t, sizeof t);
    a->nonceCounter++;
    sha256_update(&s, &a->nonceCounter, sizeof a->nonceCounter);
    sha256_update(&s, a->nonce, sizeof a->nonce);   // chain from the previous nonce
    sha256_final(&s, a->nonce);
}

static int write_all(int fd, const void *buf, int len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        int n = write(fd, p, (size_t)len);
        if (n <= 0) return -1;
        p += n; len -= n;
    }
    return 0;
}

static int read_all(int fd, void *buf, int len) {
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        int n = read(fd, p, (size_t)len);
        if (n <= 0) return -1;      // EOF, error or the receive timeout
        p += n; len -= n;
    }
    return 0;
}

// ---- install ----------------------------------------------------------------
// Title id from a content id: "IV0000-PCST00001_00-..." -> "PCST00001".
static void title_of(const char *contentId, char out[10]) {
    int i = 0;
    while (contentId[i] && contentId[i] != '-') i++;
    int k = 0;
    if (contentId[i] == '-') for (i++; contentId[i] && contentId[i] != '_' && k < 9; i++) out[k++] = contentId[i];
    out[k] = 0;
}

// A deploy replaces the installed build: BGFT refuses to register a package
// whose title is already installed (0x80990088), so remove that title -- and
// only that one -- first, and wait until AppInstUtil no longer reports it (the
// readiness check below would otherwise see the old copy). 0 = nothing to
// remove or removed; else the failing rc (-1 = still present after the wait).
static int remove_installed(Agent *a, const char *title) {
    int32_t e = 0;
    if (slen(title) != 9 || !a->appExists || a->appExists(title, &e) != 0 || e != 1) return 0;
    if (!a->appUninstall) return -1;
    int rv = a->appUninstall(title);
    if (rv != 0) return rv;
    for (int s = 0; s < AGENT_UNINSTALL_WAIT_S; s++) {
        e = 1;
        if (a->appExists(title, &e) == 0 && e == 0) return 0;
        sleep_ms(1000);
    }
    return -1;
}

static void do_install(Agent *a) {
    AgentRequest *r = &a->req;
    char *st = a->status;
    st[0] = 0;
    char title[10];
    title_of(r->id, title);
    int urc = remove_installed(a, title);
    if (urc != 0) {
        cat(st, sizeof a->status, "ERROR uninstall ");
        cat_hex32(st, sizeof a->status, (uint32_t)urc);
        cat(st, sizeof a->status, " title=");
        cat(st, sizeof a->status, title);
        cat(st, sizeof a->status, " (close the app, or uninstall it on the console)\n");
        return;
    }
    struct bgft_download_param p = {
        .user_id = a->uid,
        .entitlement_type = 5,
        .id = r->id,
        .content_url = r->url,
        .content_name = r->name,
        .icon_path = "",
        .package_type = r->type[0] ? r->type : "PS4GD",
        .package_sub_type = "",
        .playgo_scenario_id = "0",
        .option = BGFT_TASK_OPTION_DISABLE_CDN_QUERY_PARAM,
        .package_size = r->size,
    };
    int task = BGFT_INVALID_TASK_ID;
    int rv = a->bgftRegister(&p, &task);
    if (rv == (int)0x80990088 || task == BGFT_INVALID_TASK_ID) {
        task = BGFT_INVALID_TASK_ID;
        rv = a->bgftDebugRegister(&p, &task);
    }
    if (rv != (int)0x80990088 && task != BGFT_INVALID_TASK_ID) {
        rv = a->bgftStart(task);
    } else {
        cat(st, sizeof a->status, "ERROR bgft ");
        cat_hex32(st, sizeof a->status, (uint32_t)rv);
        if (rv == (int)0x80990086) cat(st, sizeof a->status, " stale download task: delete PS4 Cast from Notifications > Downloads");
        else if (rv == (int)0x80990088) cat(st, sizeof a->status, " already installed: uninstall it first");
        else if (rv == (int)0x80990039 || rv == (int)0x80A30026 || rv == (int)0x80990085) cat(st, sizeof a->status, " insufficient storage");
        cat(st, sizeof a->status, "\n");
        return;
    }
    if (rv != 0) {
        cat(st, sizeof a->status, "ERROR start ");
        cat_hex32(st, sizeof a->status, (uint32_t)rv);
        cat(st, sizeof a->status, "\n");
        return;
    }

    // Ready = AppInstUtil reports the title as existing on several consecutive
    // polls (install progress polling crashed the GoldHEN host on FW 11, so
    // only AppExists is consulted -- see STEERING.md).
    int stable = 0, exists = -1, erc = -1;
    for (int s = 0; s < AGENT_READY_TIMEOUT_S; s++) {
        int32_t e = -1;
        erc = a->appExists ? a->appExists(title, &e) : -2;
        exists = e;
        stable = (erc == 0 && e == 1) ? stable + 1 : 0;
        if (stable >= AGENT_READY_STABLE) break;
        sleep_ms(1000);
    }
    cat(st, sizeof a->status, stable >= AGENT_READY_STABLE ? "READY " : "ERROR timeout ");
    cat(st, sizeof a->status, "title=");
    cat(st, sizeof a->status, title);
    cat(st, sizeof a->status, " exists=");
    cat_dec(st, sizeof a->status, exists);
    cat(st, sizeof a->status, " rc=");
    cat_dec(st, sizeof a->status, erc);
    cat(st, sizeof a->status, " task=");
    cat_dec(st, sizeof a->status, task);
    cat(st, sizeof a->status, "\n");
}

// ---- one connection ---------------------------------------------------------
static void reply(int fd, const char *s) { write_all(fd, s, slen(s)); }

static void handle_client(Agent *a, int fd) {
    struct timeval tmo = { AGENT_IO_TIMEOUT_SEC, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof tmo);

    new_nonce(a);
    uint8_t hello[AGENT_GREETING_LEN + 32];
    memcpy(hello, AGENT_GREETING, AGENT_GREETING_LEN);
    memcpy(hello + AGENT_GREETING_LEN, a->nonce, 32);
    if (write_all(fd, hello, sizeof hello) != 0) return;

    uint8_t hdr[8], mac[32], want[32];
    if (read_all(fd, hdr, 8) != 0) return;
    uint32_t len = (uint32_t)hdr[4] | (uint32_t)hdr[5] << 8 | (uint32_t)hdr[6] << 16 | (uint32_t)hdr[7] << 24;
    if (hdr[0] != 'P' || hdr[1] != 'C' || hdr[2] != 'A' || hdr[3] != 'R' || len > AGENT_MAX_PACKET) {
        reply(fd, "ERROR protocol\n");
        return;
    }
    if (read_all(fd, a->packet, (int)len) != 0 || read_all(fd, mac, 32) != 0) return;

    agent_mac(a->secret, a->nonce, a->packet, len, want);
    if (!mac_equal(mac, want)) {
        reply(fd, "ERROR auth\n");
        // Slow down guessing; nothing unauthenticated touches the installer.
        if (a->authFailures < 30) a->authFailures++;
        sleep_ms(200L * a->authFailures);
        return;
    }
    a->authFailures = 0;
    a->lastActive = now_sec();

    if (agent_parse(a->packet, len, &a->req) != 0) { reply(fd, "ERROR packet\n"); return; }
    if (a->req.cmd == AGENT_CMD_STOP) { reply(fd, "BYE\n"); a->stop = 1; return; }
    do_install(a);
    reply(fd, a->status);
}

// ---- main -------------------------------------------------------------------
int main(void) {
    agent_elevate();
    clear_stack();

    Agent *a = mmap(NULL, sizeof(Agent), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (a == MAP_FAILED || !a) return -1;
    memset(a, 0, sizeof *a);
    for (int i = 0; i < AGENT_SECRET_LEN; i++) a->secret[i] = AgentSecret[i];
    a->listenFd = -1;

    void *sysutil = dlopen("/system/common/lib/libSceSysUtil.sprx", 0);
    a->notify = dlsym(sysutil, "sceSysUtilSendSystemNotificationWithText");
    if (secret_unpatched(a)) {
        // Never run with the public placeholder as the key: that would be an
        // agent anyone could drive. Only the deploy script's patched copy runs.
        if (a->notify) a->notify(222, "PS4 Cast deploy agent: unpatched build refused (send it with scripts/push-goldhen-dpi.py)");
        return -1;
    }

    void *usrsrv = dlopen("/system/common/lib/libSceUserService.sprx", 0);
    int (*usInit)(OrbisUserServiceInitializeParams *) = dlsym(usrsrv, "sceUserServiceInitialize");
    int (*usFg)(int *) = dlsym(usrsrv, "sceUserServiceGetForegroundUser");
    int (*usTerm)(void) = dlsym(usrsrv, "sceUserServiceTerminate");
    OrbisUserServiceInitializeParams up = { .priority = ORBIS_KERNEL_PRIO_FIFO_NORMAL };
    if (usInit) usInit(&up);
    if (usFg) usFg(&a->uid);
    if (usTerm) usTerm();

    void *appinst = dlopen("/system/common/lib/libSceAppInstUtil.sprx", 0);
    int (*aiInit)(void) = dlsym(appinst, "sceAppInstUtilInitialize");
    a->appExists = dlsym(appinst, "sceAppInstUtilAppExists");
    a->appUninstall = dlsym(appinst, "sceAppInstUtilAppUnInstall");
    int rv = aiInit ? aiInit() : -1;
    if (rv) {
        char m[96] = "PS4 Cast deploy agent: AppInstUtil error ";
        cat_hex32(m, sizeof m, (uint32_t)rv);
        if (a->notify) a->notify(222, m);
        return -1;
    }

    void *bgft = dlopen("/system/common/lib/libSceBgft.sprx", 0);
    int (*bgftInit)(struct bgft_init_params *) = dlsym(bgft, "sceBgftServiceIntInit");
    a->bgftRegister = dlsym(bgft, "sceBgftServiceDownloadRegisterTask");
    a->bgftDebugRegister = dlsym(bgft, "sceBgftServiceIntDebugDownloadRegisterPkg");
    a->bgftStart = dlsym(bgft, "sceBgftServiceIntDownloadStartTask");
    struct bgft_init_params ip = {
        .mem = mmap(NULL, 0x100000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0),
        .size = 0x100000,
    };
    rv = (bgftInit && a->bgftRegister && a->bgftDebugRegister && a->bgftStart) ? bgftInit(&ip) : -1;
    if (rv && rv != (int)0x80990001) {
        char m[96] = "PS4 Cast deploy agent: BGFT init failed ";
        cat_hex32(m, sizeof m, (uint32_t)rv);
        if (a->notify) a->notify(222, m);
        return -1;
    }

    a->listenFd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(a->listenFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_len = sizeof sin;
    sin.sin_family = AF_INET;
    sin.sin_port = (uint16_t)((AGENT_PORT >> 8) | ((AGENT_PORT & 0xff) << 8));   // htons
    sin.sin_addr.s_addr = 0;                                                      // INADDR_ANY
    if (a->listenFd < 0 || bind(a->listenFd, (struct sockaddr *)&sin, sizeof sin) != 0 ||
        listen(a->listenFd, 4) != 0) {
        // Most likely another agent already holds the port (it keeps its own
        // secret; reboot the console to replace it).
        if (a->notify) a->notify(222, "PS4 Cast deploy agent: port 9192 busy (an agent is already running)");
        if (a->listenFd >= 0) close(a->listenFd);
        return -1;
    }

    a->lastActive = now_sec();
    while (!a->stop) {
        if (now_sec() - a->lastActive > AGENT_IDLE_SEC) break;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(a->listenFd, &rd);
        struct timeval tv = { 30, 0 };
        int n = select(a->listenFd + 1, &rd, NULL, NULL, &tv);
        if (n < 0) { sleep_ms(500); continue; }
        if (n == 0) continue;
        int fd = accept(a->listenFd, NULL, NULL);
        if (fd < 0) continue;
        handle_client(a, fd);
        close(fd);
    }
    close(a->listenFd);
    if (a->notify) a->notify(222, a->stop ? "PS4 Cast deploy agent stopped" : "PS4 Cast deploy agent stopped (idle)");
    return 0;
}
