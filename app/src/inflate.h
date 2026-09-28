// inflate.h — push-style streaming DEFLATE decoder (RFC 1950/1951/1952).
//
// For XMLTV guides, which providers serve as .xml.gz and which unpack to
// hundreds of MB: bytes go in as they arrive from the network and come out
// through a callback, so nothing but a 32 KB window is ever held. The input
// format is detected from its first bytes: gzip (multi-member too), zlib, or
// anything else passed through untouched (an uncompressed .xml). Checksums
// (CRC-32 / Adler-32) are verified. Pure C, no platform calls.
#ifndef PS4CAST_INFLATE_H
#define PS4CAST_INFLATE_H

#include <stdint.h>

typedef struct Inflate Inflate;

// Receives decoded bytes in order; return nonzero to stop (the push/finish in
// progress then returns INFLATE_STOP).
typedef int (*InflateOut)(void *ctx, const uint8_t *p, int n);

#define INFLATE_OK     0    // all input taken, more wanted
#define INFLATE_END    1    // stream complete (anything after it is ignored)
#define INFLATE_STOP   2    // the output callback asked to stop
#define INFLATE_ERR   (-1)  // corrupt data or checksum mismatch
#define INFLATE_SHORT (-2)  // inflate_finish: the stream ended early
#define INFLATE_NOMEM (-3)

Inflate *inflate_new(InflateOut out, void *ctx);
int      inflate_push(Inflate *z, const uint8_t *p, int n);
// No more input: flush, and say whether the stream was complete.
int      inflate_finish(Inflate *z);
void     inflate_free(Inflate *z);
// 'g' gzip, 'z' zlib, 'p' passthrough, '?' not decided yet.
char     inflate_format(const Inflate *z);

#endif
