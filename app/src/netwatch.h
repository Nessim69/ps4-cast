// netwatch.h — decide when the console's IPv4 address has really appeared,
// changed or gone, from periodic observations (netmon.c polls; this part is
// pure so tests/host can drive it).
//
// A first address is adopted at once (startup must not wait). A different
// address must persist for NETWATCH_CONFIRM polls and a missing one for
// NETWATCH_LOSS polls before they count: a DHCP renewal or Wi-Fi roam can
// report a transient value for a moment, and each accepted change re-joins
// SSDP multicast and toasts the user.
#ifndef PS4CAST_NETWATCH_H
#define PS4CAST_NETWATCH_H

#define NETWATCH_CONFIRM 2
#define NETWATCH_LOSS    3

typedef enum {
    NW_NONE = 0,     // nothing to act on
    NW_UP,           // an address appeared (first one, or back after NW_DOWN)
    NW_CHANGED,      // the address is now a different one
    NW_DOWN          // no address any more
} NetWatchEvent;

typedef struct {
    char ip[32];         // adopted address, "" = none
    char cand[32];       // a different address waiting for confirmation
    int  candSeen;
    int  missing;        // consecutive polls without an address
} NetWatch;

void netwatch_init(NetWatch *w);
// Feed one poll: obs = dotted IPv4 address, or NULL / "" / "0.0.0.0" / junk
// for "no address". w->ip holds the adopted address afterwards.
NetWatchEvent netwatch_feed(NetWatch *w, const char *obs);

#endif
