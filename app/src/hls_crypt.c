#include "hls_crypt.h"

#include <string.h>

#include "bearssl.h"

static int g_portable = 0;
void hls_crypt_force_portable(int on) { g_portable = on ? 1 : 0; }

static const br_block_cbcdec_class *cbcdec(void) {
    const br_block_cbcdec_class *vt = g_portable ? NULL : br_aes_x86ni_cbcdec_get_vtable();
    return vt ? vt : &br_aes_big_cbcdec_vtable;
}

static int pad_valid(const uint8_t *blk) {
    int p = blk[15];
    if (p < 1 || p > 16) return 0;
    for (int i = 16 - p; i < 16; i++) if (blk[i] != p) return 0;
    return 1;
}

int hls_aes128_padding_ok(const uint8_t key[16], const uint8_t iv[16], const uint8_t *buf, int len) {
    if (len <= 0 || (len & 15) != 0) return 0;
    const br_block_cbcdec_class *vt = cbcdec();
    br_aes_gen_cbcdec_keys ctx;
    uint8_t prev[16], last[16];
    memcpy(prev, len >= 32 ? buf + len - 32 : iv, 16);   // CBC: P_n = D(C_n) ^ C_{n-1}
    memcpy(last, buf + len - 16, 16);
    vt->init(&ctx.vtable, key, 16);
    vt->run(&ctx.vtable, prev, last, 16);
    return pad_valid(last);
}

int hls_aes128_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16], uint8_t *buf, int *len) {
    int n = *len;
    if (n <= 0 || (n & 15) != 0) return -1;
    const br_block_cbcdec_class *vt = cbcdec();
    br_aes_gen_cbcdec_keys ctx;
    uint8_t ivc[16];
    memcpy(ivc, iv, 16);
    vt->init(&ctx.vtable, key, 16);
    vt->run(&ctx.vtable, ivc, buf, (size_t)n);
    // PKCS#7: the last byte p (1..16) repeats p times.
    if (!pad_valid(buf + n - 16)) return -2;
    *len = n - buf[n - 1];
    return 0;
}

static int b64val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

int hls_key_from_data_uri(const char *uri, uint8_t key[16]) {
    if (strncmp(uri, "data:", 5) != 0) return -1;
    const char *p = strstr(uri, ";base64,");
    if (!p) return -2;
    p += 8;
    uint8_t out[18];
    int n = 0, acc = 0, bits = 0;
    for (; *p && *p != '='; p++) {
        int v = b64val((unsigned char)*p);
        if (v < 0) return -2;
        acc = ((acc << 6) | v) & 0x3FFF; bits += 6;   // <= 13 live bits
        if (bits >= 8) {
            bits -= 8;
            if (n >= (int)sizeof(out)) return -2;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    if (n != 16) return -2;
    memcpy(key, out, 16);
    return 0;
}
