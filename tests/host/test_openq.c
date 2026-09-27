// Host tests for the opener worker's latest-wins request slot (app/src/openq.c).
// The worker/mutex live in player_ff.c; these pin the coalescing rules it relies
// on: one request at a time, newest wins, superseded work never reports.
#include "../../app/src/openq.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)

int main(void) {
    Openq q;
    OpenqReq r;
    int cancel = -1, rc = 99;

    // idle: nothing to take, nothing to report
    openq_init(&q);
    CHECK(!openq_busy(&q));
    CHECK(!openq_take(&q, &r));
    CHECK(!openq_poll(&q, &rc));

    // one play: busy from submit until finish, reports exactly once
    unsigned a = openq_submit(&q, OPENQ_PLAY, "http://a/x.m3u8|Referer=http://p/", 1, -1.0, &cancel);
    CHECK(a != 0);
    CHECK(cancel == 0);                        // nothing was executing
    CHECK(openq_busy(&q));
    CHECK(openq_take(&q, &r));
    CHECK(r.seq == a && r.kind == OPENQ_PLAY && r.headstart == 1 && r.resumeSec < 0);
    CHECK(strcmp(r.spec, "http://a/x.m3u8|Referer=http://p/") == 0);   // options survive
    CHECK(openq_busy(&q));                     // executing still counts as busy
    CHECK(!openq_superseded(&q, a));
    CHECK(!openq_poll(&q, &rc));               // not finished yet
    openq_finish(&q, a, 0);
    CHECK(!openq_busy(&q));
    CHECK(openq_poll(&q, &rc) && rc == 0);
    CHECK(!openq_poll(&q, &rc));               // once only

    // latest wins: queued requests are replaced, never replayed
    openq_submit(&q, OPENQ_PLAY, "http://b/", 0, -1.0, &cancel);
    unsigned c = openq_submit(&q, OPENQ_PLAY, "http://c/", 0, 12.5, &cancel);
    CHECK(cancel == 0);                        // b never started: nothing to abort
    CHECK(openq_take(&q, &r));
    CHECK(r.seq == c && strcmp(r.spec, "http://c/") == 0 && r.resumeSec == 12.5);
    CHECK(!openq_take(&q, &r));                // b is gone for good

    // a newer request supersedes the executing one; the stale result is silent
    unsigned d = openq_submit(&q, OPENQ_PLAY, "http://d/", 0, -1.0, &cancel);
    CHECK(cancel == 1);                        // c is executing -> abort it
    CHECK(openq_superseded(&q, c));
    CHECK(!openq_superseded(&q, d));
    openq_finish(&q, c, -9);
    CHECK(openq_busy(&q));                     // d still queued
    CHECK(!openq_poll(&q, &rc));               // c's failure is nobody's business
    CHECK(openq_take(&q, &r) && r.seq == d);
    openq_finish(&q, d, -2);
    CHECK(openq_poll(&q, &rc) && rc == -2);    // the newest play's failure IS reported

    // stop supersedes a play and never reports; the play's result is dropped
    unsigned e = openq_submit(&q, OPENQ_PLAY, "http://e/", 0, -1.0, &cancel);
    CHECK(openq_take(&q, &r) && r.seq == e);
    unsigned s = openq_submit(&q, OPENQ_STOP, "", 0, -1.0, &cancel);
    CHECK(cancel == 1);
    openq_finish(&q, e, 0);                    // e even opened fine -- still superseded
    CHECK(!openq_poll(&q, &rc));
    CHECK(openq_take(&q, &r) && r.kind == OPENQ_STOP && r.seq == s && r.spec[0] == '\0');
    openq_finish(&q, s, 0);
    CHECK(!openq_busy(&q));
    CHECK(!openq_poll(&q, &rc));

    // the executing request finishing while a newer one is queued stays busy
    openq_submit(&q, OPENQ_PLAY, "http://f/", 0, -1.0, &cancel);
    CHECK(openq_take(&q, &r));
    unsigned g = openq_submit(&q, OPENQ_PLAY, "http://g/", 0, -1.0, &cancel);
    openq_finish(&q, g - 1, 0);
    CHECK(openq_busy(&q));

    // an oversized spec is truncated, still NUL-terminated
    {
        static char big[OPENQ_SPEC_MAX + 100];
        memset(big, 'x', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        openq_submit(&q, OPENQ_PLAY, big, 0, -1.0, NULL);   // NULL cancel out is allowed
        CHECK(openq_take(&q, &r));
        CHECK(strlen(r.spec) == OPENQ_SPEC_MAX - 1);
        openq_submit(&q, OPENQ_PLAY, NULL, 0, -1.0, NULL);  // NULL spec -> empty
        CHECK(openq_take(&q, &r) && r.spec[0] == '\0');
    }

    // sequence numbers skip 0 on wrap (0 means "no request")
    openq_init(&q);
    q.lastSeq = 0xffffffffu;
    unsigned w = openq_submit(&q, OPENQ_PLAY, "http://w/", 0, -1.0, &cancel);
    CHECK(w == 1);
    CHECK(openq_take(&q, &r) && r.seq == 1);
    openq_finish(&q, w, 0);
    CHECK(openq_poll(&q, &rc) && rc == 0);

    printf(failures ? "test_openq: %d FAILURES\n" : "test_openq: all ok\n", failures);
    return failures ? 1 : 0;
}
