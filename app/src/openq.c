// openq.c — latest-wins request slot for the player's opener worker (see openq.h).
#include "openq.h"

#include <string.h>

void openq_init(Openq *q) {
    memset(q, 0, sizeof(*q));
}

unsigned openq_submit(Openq *q, int kind, const char *spec, int headstart,
                      double resumeSec, int *cancelRunning) {
    if (++q->lastSeq == 0) q->lastSeq = 1;    // 0 means "none" everywhere
    q->lastKind = kind;
    OpenqReq *r = &q->pending;
    r->seq = q->lastSeq;
    r->kind = kind;
    r->headstart = headstart;
    r->resumeSec = resumeSec;
    // Truncating a spec would silently drop its trailing |options (the very
    // headers a CDN demands), but the slot matches httpd's own 2048-byte limit.
    size_t n = spec ? strlen(spec) : 0;
    if (n >= sizeof(r->spec)) n = sizeof(r->spec) - 1;
    if (n) memcpy(r->spec, spec, n);
    r->spec[n] = '\0';
    q->havePending = 1;
    if (cancelRunning) *cancelRunning = q->runSeq != 0;
    return q->lastSeq;
}

int openq_take(Openq *q, OpenqReq *out) {
    if (!q->havePending) return 0;
    *out = q->pending;
    q->havePending = 0;
    q->runSeq = out->seq;
    return 1;
}

void openq_finish(Openq *q, unsigned seq, int rc) {
    if (q->runSeq == seq) q->runSeq = 0;
    q->doneSeq = seq;
    q->doneRc = rc;
    q->doneKind = (seq == q->lastSeq) ? q->lastKind : OPENQ_NONE;
}

int openq_busy(const Openq *q) {
    return q->havePending || q->runSeq != 0;
}

int openq_superseded(const Openq *q, unsigned seq) {
    return seq != q->lastSeq;
}

int openq_poll(Openq *q, int *rc) {
    // doneSeq == lastSeq also means nothing newer is queued or executing.
    if (!q->doneSeq || q->doneSeq != q->lastSeq || q->doneSeq == q->polledSeq) return 0;
    if (q->doneKind != OPENQ_PLAY) return 0;
    q->polledSeq = q->doneSeq;
    if (rc) *rc = q->doneRc;
    return 1;
}
