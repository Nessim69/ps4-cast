// Host test for the address-change detector (app/src/netwatch.c).
#include "netwatch.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void) {
    NetWatch w;
    netwatch_init(&w);

    // no network at launch: nothing happens, however long it lasts
    for (int i = 0; i < 10; i++) CHECK(netwatch_feed(&w, NULL) == NW_NONE);
    CHECK(netwatch_feed(&w, "") == NW_NONE && netwatch_feed(&w, "0.0.0.0") == NW_NONE);
    CHECK(w.ip[0] == '\0');

    // late Wi-Fi: the first address is adopted immediately
    CHECK(netwatch_feed(&w, "192.168.1.20") == NW_UP);
    CHECK(strcmp(w.ip, "192.168.1.20") == 0);
    CHECK(netwatch_feed(&w, "192.168.1.20") == NW_NONE);

    // a one-poll blip to another address or to none changes nothing
    CHECK(netwatch_feed(&w, "10.0.0.9") == NW_NONE);
    CHECK(netwatch_feed(&w, "192.168.1.20") == NW_NONE);
    CHECK(netwatch_feed(&w, NULL) == NW_NONE);
    CHECK(netwatch_feed(&w, "192.168.1.20") == NW_NONE);
    CHECK(strcmp(w.ip, "192.168.1.20") == 0);

    // new DHCP lease: confirmed on the second consecutive poll
    CHECK(netwatch_feed(&w, "192.168.1.57") == NW_NONE);
    CHECK(netwatch_feed(&w, "192.168.1.57") == NW_CHANGED);
    CHECK(strcmp(w.ip, "192.168.1.57") == 0);

    // two alternating candidates never confirm
    CHECK(netwatch_feed(&w, "10.1.1.1") == NW_NONE);
    CHECK(netwatch_feed(&w, "10.1.1.2") == NW_NONE);
    CHECK(netwatch_feed(&w, "10.1.1.1") == NW_NONE);
    CHECK(strcmp(w.ip, "192.168.1.57") == 0);

    // link lost: DOWN after NETWATCH_LOSS polls, then UP again (same address too)
    CHECK(netwatch_feed(&w, NULL) == NW_NONE);
    CHECK(netwatch_feed(&w, "junk") == NW_NONE);
    CHECK(netwatch_feed(&w, "0.0.0.0") == NW_DOWN);
    CHECK(w.ip[0] == '\0');
    CHECK(netwatch_feed(&w, NULL) == NW_NONE);
    CHECK(netwatch_feed(&w, "192.168.1.57") == NW_UP);

    // malformed / unusable observations are "no address"
    const char *bad[] = { "1.2.3", "1.2.3.4.5", "256.1.1.1", "224.0.0.1", "0.1.2.3", "1.2.3.4x", "a.b.c.d" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        NetWatch t; netwatch_init(&t);
        CHECK(netwatch_feed(&t, bad[i]) == NW_NONE && t.ip[0] == '\0');
    }

    printf(failures ? "test_netwatch: %d FAILURES\n" : "test_netwatch: all ok\n", failures);
    return failures ? 1 : 0;
}
