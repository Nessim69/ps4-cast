// hls_crypt.h — HLS segment decryption (EXT-X-KEY METHOD=AES-128).
//
// RFC 8216 4.3.2.4: the whole segment is AES-128-CBC encrypted with PKCS#7
// padding. Pure code over BearSSL's AES (AES-NI when the CPU has it -- the
// PS4's Jaguar does -- else BearSSL's portable table implementation), so it
// also runs in tests/host.
#ifndef PS4CAST_HLS_CRYPT_H
#define PS4CAST_HLS_CRYPT_H

#include <stdint.h>

// Decrypt buf[0..*len) in place and strip the padding. 0 = ok (*len is the
// plaintext length); -1 = length not a positive multiple of 16; -2 = invalid
// padding (almost always the wrong key or IV).
int hls_aes128_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16], uint8_t *buf, int *len);

// Decrypt only the last block and check its padding, leaving buf untouched:
// 1 = the key/IV look right. Lets a caller refetch a (possibly rotated) key
// before the in-place decrypt destroys the ciphertext.
int hls_aes128_padding_ok(const uint8_t key[16], const uint8_t iv[16], const uint8_t *buf, int len);

// Tests: 1 = never use AES-NI (exercise the portable implementation).
void hls_crypt_force_portable(int on);

// A key given inline as a data: URI (";base64," payload). 0 and key[16] filled,
// -1 if uri isn't a data: URI, -2 if it doesn't decode to exactly 16 bytes.
int hls_key_from_data_uri(const char *uri, uint8_t key[16]);

#endif
