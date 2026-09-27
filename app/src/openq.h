// openq.h — latest-wins request slot for the player's opener worker.
//
// Opening a stream (DNS, TLS, page scraping, playlist fetches, a 4 MB demux
// probe) used to run on the render thread and froze the TV for up to the
// watchdog's 35s grace. player_ff.c now hands every stop+open to one worker
// thread; this is the pure bookkeeping between the two, kept free of Orbis
// calls so the coalescing rules are host-tested (tests/host/test_openq.c).
//
// Rules: exactly one request executes at a time; a newer request REPLACES a
// queued one (intermediate zaps are dropped, never replayed) and marks the one
// executing as superseded so it can bail at its next checkpoint. Not
// thread-safe by itself: the caller serializes every call on one mutex.
#ifndef PS4CAST_OPENQ_H
#define PS4CAST_OPENQ_H

#define OPENQ_SPEC_MAX 2048

enum { OPENQ_NONE = 0, OPENQ_PLAY = 1, OPENQ_STOP = 2 };

typedef struct {
    unsigned seq;              // 1-based, never 0
    int      kind;             // OPENQ_PLAY / OPENQ_STOP
    int      headstart;        // player_set_startup_headstart() captured at submit
    double   resumeSec;        // fMP4 reopen target (<0 = none); only internal restarts carry one
    char     spec[OPENQ_SPEC_MAX];   // "url|Referer=..&Type=hls" exactly as requested
} OpenqReq;

typedef struct {
    unsigned lastSeq;          // newest request issued (0 = none yet)
    int      lastKind;         // its kind
    unsigned runSeq;           // request the worker is executing (0 = none)
    int      havePending;
    OpenqReq pending;
    unsigned doneSeq;          // newest finished request
    int      doneKind, doneRc;
    unsigned polledSeq;        // last result handed out by openq_poll
} Openq;

void     openq_init(Openq *q);
// Queue a request, replacing any that has not started. Returns its seq. Sets
// *cancelRunning when an OLDER request is executing and should be aborted.
unsigned openq_submit(Openq *q, int kind, const char *spec, int headstart,
                      double resumeSec, int *cancelRunning);
// Worker: move the queued request to executing (1), or report none (0).
int      openq_take(Openq *q, OpenqReq *out);
// Worker: request `seq` finished with rc (0 = playing / stopped).
void     openq_finish(Openq *q, unsigned seq, int rc);
int      openq_busy(const Openq *q);                  // queued or executing
int      openq_superseded(const Openq *q, unsigned seq);
// 1 exactly once when the NEWEST request is a finished PLAY (*rc = its result).
// Superseded plays and stops never report: nobody is waiting on them.
int      openq_poll(Openq *q, int *rc);

#endif
