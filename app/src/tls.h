// tls.h — minimal TLS 1.2 client over an existing OrbisNet TCP socket (BearSSL).
//
// Server certificates are validated (chain to the bundled Mozilla roots in
// ca_anchors.c, validity dates, host name) for public hosts. Private/LAN hosts
// (netpol_host_is_private) are exempt, and the user can turn validation off
// in Settings for a source whose chain BearSSL cannot build.
#ifndef PS4CAST_TLS_H
#define PS4CAST_TLS_H

#include <stdint.h>
#include <stddef.h>

typedef struct tls_ctx tls_ctx;

// Wrap an already-connected TCP socket in TLS and run the handshake against
// `host` (SNI + certificate name). Returns a context, or NULL on failure.
tls_ctx *tls_open(int sock, const char *host);

// Same, but with a read deadline armed before the handshake. Required when the
// socket is non-blocking (see tls.c).
tls_ctx *tls_open_bounded(int sock, const char *host, uint64_t deadlineUs);

// Read up to len bytes of plaintext. >0 = bytes, 0 = clean EOF, <0 = error.
int  tls_read(tls_ctx *t, uint8_t *buf, int len);

// Write len bytes of plaintext (all of it). 0 ok, <0 error.
int  tls_write(tls_ctx *t, const uint8_t *buf, int len);

// Bound how long reads may block, as an absolute sceKernelGetProcessTime value
// (0 = unbounded). Needed because BearSSL can loop internally on a trickling
// peer and never return to the caller's own budget check.
void tls_set_read_deadline(tls_ctx *t, uint64_t absUs);

// Last BearSSL engine error code (for diagnostics).
int  tls_last_error(tls_ctx *t);

void tls_close(tls_ctx *t);

// ---- certificate verification ----
// Global switch (Settings -> "Verify HTTPS certificates"; default on).
void tls_set_verify(int on);
int  tls_verify_enabled(void);
// 1 if a connection to `host` validates its certificate right now (the switch
// is on and the host is not a private/LAN name). The native SceHttp fallback
// applies the same policy.
int  tls_verify_applies(const char *host);
// Most recent certificate rejection: copies its host and BearSSL error code
// and returns a generation counter that increases with every rejection (0 if
// none yet), so a caller can tell whether one happened during its own open.
unsigned tls_verify_failure(char *host, int hostcap, int *err);
// Human-readable cause for a BearSSL X.509 error code.
const char *tls_verify_reason(int err);
// Generation date of the bundled root list (YYYY-MM-DD).
const char *tls_ca_bundle_date(void);

#endif
