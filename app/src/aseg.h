// aseg.h — minimal blocking HTTP(S) "fetch a whole resource into RAM" client
// for HLS playlists and segments, page scraping (resolve.c) and the phone's
// IPTV import. It runs on its own sockets, independent of the video read-ahead
// reader (httpsrc).
//
// Fetches run on CHANNELS. Each channel has its own lock, socket/TLS,
// keep-alive host, time budget and diagnostics, so the stream's fetches no
// longer queue behind each other on one socket or evict each other's
// keep-alive connection. Fetches on the SAME channel are serialized.
#ifndef PS4CAST_ASEG_H
#define PS4CAST_ASEG_H

#include <stdint.h>

enum {
    ASEG_CH_VIDEO = 0,   // video segments: prefetch thread, segment fetch / decode thread
    ASEG_CH_AUDIO,       // separate audio rendition segments
    ASEG_CH_PLAYLIST,    // stream-owned: hls_open master/variant/audio playlists,
                         // live refresh, resolve_page
    ASEG_CH_UI,          // web UI (IPTV/M3U import): never aborted, no stream headers
    ASEG_CH_COUNT
};

// Create the channel locks. MUST be called once from main() before any thread
// fetches -- a lazy init inside aseg_fetch races and can wedge the lock forever.
void aseg_init(void);

// Download an entire resource into a malloc'd buffer. Caller frees *buf.
// http + https (BearSSL, SceHttp fallback), follows up to a few redirects.
// Returns 0, else < 0.
int aseg_fetch_ch(int ch, const char *url, uint8_t **buf, int *len);
// ASEG_CH_UI: sends only a default User-Agent (never the stream's urlopt
// headers), ignores aseg_abort/aseg_resume, 15s budget, 16 MB cap.
int aseg_fetch_ui(const char *url, uint8_t **buf, int *len);
// Legacy entry point: ASEG_CH_PLAYLIST.
int aseg_fetch(const char *url, uint8_t **buf, int *len);

// Per-call options, for probing a URL that may or may not be a web page
// (resolve.c). A zeroed AsegOpts behaves exactly like aseg_fetch_ch().
typedef struct {
    int       maxBytes;     // >0: keep at most this many body bytes (then drop the connection)
    uint64_t  budgetUs;     // >0: whole-fetch budget instead of the channel's
    // Called with the final 2xx response's Content-Type ("" if absent) before
    // any body is read; nonzero stops there and the fetch returns ASEG_STOPPED
    // with no body. (Not consulted on the SceHttp fallback, which cannot see
    // headers; maxBytes and budgetUs still bound it.)
    int     (*stopAfterHeaders)(const char *contentType);
    char      contentType[96];   // out: Content-Type of the final response
    // rangeLen > 0: fetch only bytes [rangeOff, rangeOff + rangeLen) (HLS
    // EXT-X-BYTERANGE). Sent as a Range request; a server that ignores it and
    // answers 200 gets the slice cut out of the full body (up to 16 MB in).
    int64_t   rangeOff, rangeLen;
} AsegOpts;
#define ASEG_STOPPED 1
int aseg_fetch_opts(int ch, const char *url, uint8_t **buf, int *len, AsegOpts *o);

// Abort fetches in progress (Stop / new cast / teardown) on the three stream
// channels (VIDEO, AUDIO, PLAYLIST). Sticky until aseg_resume().
void aseg_abort(void);
// Clear a stale abort on the stream channels before starting a new stream (see aseg.c).
void aseg_resume(void);
// Clear ONE stream channel's stale abort (resolve_page: see resolve.c).
void aseg_resume_ch(int ch);
// Predicate the resumes consult after clearing: nonzero while the stream open
// in progress has been superseded, in which case the abort is re-raised
// instead of cleared. Registered once by the player before any thread starts.
void aseg_set_cancel_check(int (*fn)(void));
// Tighten ASEG_CH_PLAYLIST's per-fetch time budget around small PLAYLIST
// fetches (1) and restore the generous SEGMENT budget (0). A single budget for
// both either starves slow segments (continuous rebuffering) or lets a dead
// host block a channel switch. VIDEO/AUDIO always use the segment budget.
void aseg_set_playlist_budget(int on);
// Reset stale HTTP failure telemetry when a new HLS source starts.
void aseg_clear_error(void);

// Diagnostics of the last failing fetch on any stream channel (UI excluded):
// last HTTP status seen (0 = status line unparseable) and its first bytes.
int aseg_last_status(void);
const char *aseg_last_line(void);
int aseg_bad_reuse(void);   // 1 if the failing request went out on a REUSED keep-alive socket
int aseg_bad_hop(void);
const char *aseg_bad_path(void);
int aseg_bad_pathlen(void);
// Which stage the failing request died in: "dns", "connect", "tls", "req", "hdr".
const char *aseg_bad_stage(void);
const char *aseg_native_debug(void);

#endif
