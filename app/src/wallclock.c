// wallclock.c — calendar time and time zone for the programme guide.
#include "wallclock.h"

#include <stdio.h>
#include <string.h>
#include <orbis/libkernel.h>

static volatile int g_phoneMin;          // minutes east of UTC, from the web UI
static volatile int g_phoneSet;

int64_t wallclock_utc(void) {
    OrbisKernelTimeval tv;
    if (sceKernelGettimeofday(&tv) != 0) return 0;
    // Before the console has a network clock it can report 2000-something.
    return tv.tv_sec > 1600000000 ? (int64_t)tv.tv_sec : 0;
}

void wallclock_set_phone_offset(int minutes) {
    if (minutes < -14 * 60 || minutes > 14 * 60) return;
    g_phoneMin = minutes;
    g_phoneSet = 1;
}

int wallclock_phone_offset(void) { return g_phoneSet ? g_phoneMin : 0; }

// libkernel's sceKernelConvertUtcToLocaltime(time_t, time_t *local,
// SceKernelTimesec *, unsigned long *dstsec). OpenOrbis only declares it
// without a prototype, so it is called through a typed pointer; the out
// buffers are oversized and zeroed so a mismatch in their layout cannot
// write past them, and the answer is only used when it is a plausible zone.
typedef int (*UtcToLocalFn)(int64_t utc, int64_t *local, void *timesec, void *dstsec);

static int console_offset(int64_t utc, int *out) {
    UtcToLocalFn fn = (UtcToLocalFn)(void *)sceKernelConvertUtcToLocaltime;
    int64_t local = 0;
    uint64_t ts[8], dst[4];
    memset(ts, 0, sizeof(ts));
    memset(dst, 0, sizeof(dst));
    if (fn(utc, &local, ts, dst) != 0) return 0;
    int64_t off = local - utc;
    if (off < -14 * 3600 || off > 14 * 3600 || off % 900) return 0;
    *out = (int)off;
    return 1;
}

int wallclock_offset(void) {
    static int64_t cachedAt;
    static int cached, cachedOk;
    int64_t now = wallclock_utc();
    if (!now) return g_phoneSet ? g_phoneMin * 60 : 0;
    // Recheck now and then: a daylight-saving change moves it.
    if (!cachedAt || now - cachedAt > 600 || now < cachedAt) {
        int off = 0;
        cachedOk = console_offset(now, &off);
        cached = off;
        cachedAt = now;
    }
    if (cachedOk && (cached != 0 || !g_phoneSet)) return cached;
    return g_phoneSet ? g_phoneMin * 60 : cached;
}

void wallclock_hhmm(int64_t t, char *out, int cap) {
    int64_t l = t + wallclock_offset();
    int64_t s = ((l % 86400) + 86400) % 86400;
    snprintf(out, (size_t)cap, "%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60));
}
