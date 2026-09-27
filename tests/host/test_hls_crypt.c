// Host test for HLS AES-128 segment decryption (app/src/hls_crypt.c), against
// BearSSL's AES built from the portlibs source tree. Vectors: NIST SP 800-38A
// F.2.2 (CBC-AES128) extended with its PKCS#7 padding block, and an odd-length
// TS-sized payload with a media-sequence IV -- both from Python's
// `cryptography` package. Both AES implementations are exercised (AES-NI when
// the host has it, and the portable one).
#include "hls_crypt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static int unhex(const char *s, uint8_t *out) {
    int n = 0;
    for (; s[0] && s[1]; s += 2) { unsigned v; sscanf(s, "%2x", &v); out[n++] = (uint8_t)v; }
    return n;
}

static const char *KEY = "2b7e151628aed2a6abf7158809cf4f3c";
static const char *NIST_PT =
    "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
    "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710";
static const char *NIST_CT_PADDED =
    "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
    "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7"
    "8cb82807230e1321d3fae00d18cc2012";
static const char *TS_CT =
    "924f5ea498d5ab9641dd839dd9526bbc6151a1cdc9d6937e72b791c621387d38ce616bcaeb30a4f24dea7dff02e22487"
    "5178b35610b44c75f510997f33f73f71a138653e1432e78c399d5107e87e73873c746c3acd81294205e7155cf6d880b4"
    "6149b2a3c73bcda88610ae6383b69cb8ab2c4de33c6024e74d76c3d460f522480a2d511c8b5b926f2230d3c4f9f35d45"
    "8a1eb7b696b98c106d6d980265f4da7da8043f3398e2503a6867c559c639b30c2e3bea27cf709b2fd2989c6c0154ff14"
    "af2e8d46a52ee11cfc36d2ba36f89d1e090fdebdce2652de7eec3afa19bca28294110c04b33592a70f4c2928406f63ef"
    "be58a4be0f98e65c0cc1f251c9290a3d4d4a448c006e18892bf3507bd7ff0e372fc14ce8b1eecd5d59d3efaae71ff4cf"
    "7e06e70fb30e3ed58ea51f29b94c49a8c25f5fc80a4a0998f0f804a4760f0e5f0ae8258968cc09657ad0f8b83e8b6618"
    "a2563fd31d0e810e749ac7869fd9d4d8fcc40c225d4f25b485358405cbacb8c6ef19cd496d04c186f784c669fdab9c7e"
    "0efc13b13ccf8b4a906902d8b09cc4e8fa38e8882dc54554f7e02793320e42ded6040939d2d4e6b13a1d2a8bd0088d06"
    "7ebe9c4b8ca45fe28fd5484984e66abaa6be6887e59a88bbb2e761d5459cf739b4b2a2f4f034ffd2d4fe252cfbb1e2ce"
    "7b41bb5b2c4d7b4583fd9299702fc833760fc183ea23072a40feee99a6081c0bcfe4664748ad2ccb79b9d17711c97d65"
    "748d320a28bd8b6d8ea471a8af155931cc28a659eb7729ba136701d4e4e96d5bee337df166004f029541eca79a427827";

static void run(int portable) {
    hls_crypt_force_portable(portable);
    uint8_t key[16], iv[16], buf[1024], want[1024];
    unhex(KEY, key);

    for (int i = 0; i < 16; i++) iv[i] = (uint8_t)i;
    int n = unhex(NIST_CT_PADDED, buf);
    CHECK(n == 80);
    CHECK(hls_aes128_cbc_decrypt(key, iv, buf, &n) == 0);
    CHECK(n == 64 && unhex(NIST_PT, want) == 64 && memcmp(buf, want, 64) == 0);

    // 564 bytes (3 TS packets) -> 576 on the wire; IV = sequence number 301
    memset(iv, 0, 16); iv[14] = 0x01; iv[15] = 0x2d;
    n = unhex(TS_CT, buf);
    CHECK(n == 576);
    CHECK(hls_aes128_cbc_decrypt(key, iv, buf, &n) == 0);
    CHECK(n == 564);
    int ok = 1;
    for (int i = 0; i < 564; i++) if (buf[i] != (uint8_t)(i * 7 + 3)) ok = 0;
    CHECK(ok);

    // the padding pre-check agrees with the real decrypt, without touching buf
    for (int i = 0; i < 16; i++) iv[i] = (uint8_t)i;
    n = unhex(NIST_CT_PADDED, buf);
    memcpy(want, buf, (size_t)n);
    CHECK(hls_aes128_padding_ok(key, iv, buf, n) == 1 && memcmp(buf, want, (size_t)n) == 0);
    uint8_t other[16]; memcpy(other, key, 16); other[5] ^= 0x40;
    CHECK(hls_aes128_padding_ok(other, iv, buf, n) == 0);
    CHECK(hls_aes128_padding_ok(key, iv, buf, 79) == 0);
    {   // a single block: the previous "ciphertext" is the IV itself
        uint8_t one[16], zero[16] = { 0 };
        n = unhex(NIST_CT_PADDED, buf);
        memcpy(one, buf + 64, 16);                      // C5 = E(pad ^ C4) -> with IV = C4 it is valid
        CHECK(hls_aes128_padding_ok(key, buf + 48, one, 16) == 1);
        CHECK(hls_aes128_padding_ok(key, zero, one, 16) == 0);
    }

    // wrong IV only garbles the first block: padding still valid
    memset(iv, 0, 16);
    n = unhex(TS_CT, buf);
    CHECK(hls_aes128_cbc_decrypt(key, iv, buf, &n) == 0 && n == 564 && buf[100] == (uint8_t)(100 * 7 + 3));
    // wrong key: invalid padding
    uint8_t bad[16]; memcpy(bad, key, 16); bad[0] ^= 1;
    n = unhex(NIST_CT_PADDED, buf);
    for (int i = 0; i < 16; i++) iv[i] = (uint8_t)i;
    CHECK(hls_aes128_cbc_decrypt(bad, iv, buf, &n) == -2);
    // not a multiple of the block size / empty
    n = 79; CHECK(hls_aes128_cbc_decrypt(key, iv, buf, &n) == -1);
    n = 0;  CHECK(hls_aes128_cbc_decrypt(key, iv, buf, &n) == -1);
}

int main(void) {
    run(0);
    run(1);

    uint8_t k[16];
    CHECK(hls_key_from_data_uri("data:text/plain;base64,AAECAwQFBgcICQoLDA0ODw==", k) == 0);
    int seq = 1; for (int i = 0; i < 16; i++) if (k[i] != i) seq = 0;
    CHECK(seq);
    CHECK(hls_key_from_data_uri("https://x/k", k) == -1);
    CHECK(hls_key_from_data_uri("data:;base64,AAEC", k) == -2);                       // too short
    CHECK(hls_key_from_data_uri("data:;base64,AAECAwQFBgcICQoLDA0ODxAR", k) == -2);   // too long
    CHECK(hls_key_from_data_uri("data:;base64,AAEC*wQFBgcICQoLDA0ODw==", k) == -2);   // bad char

    printf(failures ? "test_hls_crypt: %d FAILURES\n" : "test_hls_crypt: all ok\n", failures);
    return failures ? 1 : 0;
}
