// pairing.c — pure helpers for the pairing token: the display alphabet, an
// 8-char validator, and seed -> token derivation. No PS4/BSD dependencies, so
// tests/host/test_token.c links and runs this file directly on macOS/Linux.
#include "pairing.h"

#include <string.h>

// 31 characters, chosen to be unambiguous when read off a TV or typed on a
// phone keyboard: no 0/O, 1/I/L, and uppercase only (mixed case is easy to
// mistype from a screen). Index with `% (sizeof(PAIRING_ALPHABET) - 1)`,
// NEVER `& 31`: 31 is not a power of two, so masking with 31 can select index
// 31 -- one past the last real character, into this array's own NUL
// terminator. That off-by-one used to mint a literal NUL byte into ~1 in 32
// characters (~22% of 8-char tokens), which either disabled pairing outright
// (a NUL in position 0; see token_ok's `!g_token[0]` check in httpd.c) or
// produced a token the phone/extension could never match, 401-ing the QR
// link with no recovery (both /token/regen and /pairing need the very token
// that was broken).
const char PAIRING_ALPHABET[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";

int pairing_token_valid(const char *s) {
    if (!s) return 0;
    int i;
    for (i = 0; i < PAIRING_TOKEN_LEN; i++) {
        char c = s[i];
        if (!c || !strchr(PAIRING_ALPHABET, c)) return 0;
    }
    return s[i] == '\0';   // exactly PAIRING_TOKEN_LEN chars, not a prefix of something longer
}

// splitmix64 (Vigna, public domain): `*state` accumulates linearly (giving it
// the full 2^64 period) while the returned value is a separately whitened
// avalanche of it, so consecutive outputs don't correlate the way the
// generator this replaced did (`t >>= 9; t *= k;` on the same variable, with
// the index bug described above on top). Not a CSPRNG -- see token_generate
// in httpd.c for why that's an acceptable tradeoff on a console with no
// /dev/urandom: this only needs to keep one install's token from helping
// guess another's, not to resist a determined attacker.
uint64_t pairing_splitmix64(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

void pairing_token_from_seed(uint64_t seed, char *out) {
    uint64_t state = seed;
    for (int i = 0; i < PAIRING_TOKEN_LEN; i++)
        out[i] = PAIRING_ALPHABET[pairing_splitmix64(&state) % (sizeof(PAIRING_ALPHABET) - 1)];
    out[PAIRING_TOKEN_LEN] = '\0';
}
