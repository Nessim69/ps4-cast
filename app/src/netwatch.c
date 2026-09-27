#include "netwatch.h"

#include <stdio.h>
#include <string.h>

void netwatch_init(NetWatch *w) { memset(w, 0, sizeof(*w)); }

// A usable unicast IPv4 address in dotted form (not 0.0.0.0).
static int usable(const char *s) {
    if (!s) return 0;
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%3u.%3u.%3u.%3u%c", &a, &b, &c, &d, &tail) != 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    if (a == 0 || a >= 224) return 0;        // 0.x "this network", multicast, reserved
    return 1;
}

NetWatchEvent netwatch_feed(NetWatch *w, const char *obs) {
    if (!usable(obs)) {
        w->candSeen = 0; w->cand[0] = '\0';
        if (!w->ip[0]) return NW_NONE;
        if (++w->missing < NETWATCH_LOSS) return NW_NONE;
        w->ip[0] = '\0'; w->missing = 0;
        return NW_DOWN;
    }
    w->missing = 0;
    if (!w->ip[0]) {
        snprintf(w->ip, sizeof(w->ip), "%s", obs);
        w->candSeen = 0; w->cand[0] = '\0';
        return NW_UP;
    }
    if (strcmp(obs, w->ip) == 0) { w->candSeen = 0; w->cand[0] = '\0'; return NW_NONE; }
    if (strcmp(obs, w->cand) == 0) w->candSeen++;
    else { snprintf(w->cand, sizeof(w->cand), "%s", obs); w->candSeen = 1; }
    if (w->candSeen < NETWATCH_CONFIRM) return NW_NONE;
    snprintf(w->ip, sizeof(w->ip), "%s", obs);
    w->candSeen = 0; w->cand[0] = '\0';
    return NW_CHANGED;
}
