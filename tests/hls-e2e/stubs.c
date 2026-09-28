// Host stubs for the end-to-end HLS harness: no TLS, no SceHttp, no watchdog.
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "tls.h"
#include "native_http.h"
void watchdog_kick(void) {}
const char *watchdog_note(const char *w) { (void)w; return "-"; }
void watchdog_open_job(int on) { (void)on; }
void watchdog_set_busy(int on) { (void)on; }
void trace_mark(const char *fmt, ...) { if (getenv("E2E_TRACE")) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); } }
const char *trace_path(void) { return ""; }
tls_ctx *tls_open_bounded(int sock, const char *host, uint64_t d) { (void)sock; (void)host; (void)d; return NULL; }
tls_ctx *tls_open(int sock, const char *host) { (void)sock; (void)host; return NULL; }
int tls_read(tls_ctx *t, uint8_t *b, int l) { (void)t; (void)b; (void)l; return -1; }
int tls_write(tls_ctx *t, const uint8_t *b, int l) { (void)t; (void)b; (void)l; return -1; }
void tls_close(tls_ctx *t) { (void)t; }
int tls_last_error(tls_ctx *t) { (void)t; return 0; }
void tls_set_read_deadline(tls_ctx *t, uint64_t d) { (void)t; (void)d; }
void native_http_init(void) {}
int native_http_fetch(int slot, const char *url, const char *headers, uint8_t **body, int *len, int *status,
                      uint64_t t, int m, int cap, const volatile int *a) { (void)slot; (void)url; (void)headers; (void)body; (void)len; (void)status; (void)t; (void)m; (void)cap; (void)a; return -1; }
void native_http_abort(int slot) { (void)slot; }
const char *native_http_debug(void) { return ""; }
