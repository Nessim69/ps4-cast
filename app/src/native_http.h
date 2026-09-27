#ifndef PS4CAST_NATIVE_HTTP_H
#define PS4CAST_NATIVE_HTTP_H

#include <stdint.h>

// Lazy system HTTP fallback for HTTPS origins that reject the custom BearSSL
// client. The returned body is malloc-owned by the caller.
//
// Thread safety: every aseg channel fetches through its own SLOT (its own
// SceHttp connection and in-flight request), so channels run concurrently and
// an abort only ever hits the slot it names. One thread at a time per slot
// (the aseg channel lock guarantees it). native_http_init() must run once,
// before any thread fetches (aseg_init() calls it from main()).
#define NHTTP_SLOTS 4
void native_http_init(void);
// headers: CRLF block whose Referer/Origin/User-Agent/Cookie lines are sent
// ("" = none). max_bytes > 0: a probe -- keep at most that many body bytes and
// stop once timeout_us has passed in total (0 = whole body, 16 MB cap).
// abort_flag: the caller's sticky abort, re-checked under the slot lock before
// the request exists, so a racing native_http_abort() can never be missed.
// May be NULL.
int native_http_fetch(int slot, const char *url, const char *headers,
                      uint8_t **body, int *len, int *status, uint64_t timeout_us,
                      int max_bytes, const volatile int *abort_flag);
// Abort `slot`'s in-flight request; its connection is never reused.
void native_http_abort(int slot);
const char *native_http_debug(void);

#endif
