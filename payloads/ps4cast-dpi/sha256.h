// sha256.h — freestanding SHA-256 / HMAC-SHA256 for the deploy agent payload
// (no libc). Also compiled on the host by tests/host/test_agent_crypto.c.
#ifndef PS4CAST_AGENT_SHA256_H
#define PS4CAST_AGENT_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;          // total bytes absorbed
    uint8_t  buf[64];
    size_t   used;
} Sha256;

void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *data, size_t len);
void sha256_final(Sha256 *s, uint8_t out[32]);

typedef struct {
    Sha256  inner;
    uint8_t okey[64];      // key ^ opad, for the outer hash
} HmacSha256;

void hmac_sha256_init(HmacSha256 *m, const uint8_t *key, size_t keylen);
void hmac_sha256_update(HmacSha256 *m, const void *data, size_t len);
void hmac_sha256_final(HmacSha256 *m, uint8_t out[32]);

// Constant-time equality of two 32-byte MACs (1 = equal).
int  mac_equal(const uint8_t a[32], const uint8_t b[32]);

#endif
