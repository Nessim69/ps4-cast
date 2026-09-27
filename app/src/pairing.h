// pairing.h — pure helpers for the receiver's pairing token: the display
// alphabet, an 8-char validator, and seed -> token derivation. Deliberately
// free of PS4/BSD dependencies so tests/host/test_token.c can compile and run
// this module directly on a plain host (see pairing.c). The entropy-gathering
// half (TSC reads, jitter, process time/addresses) can't move here: it needs
// sceKernelReadTsc et al, which don't exist off-console.
#ifndef PS4CAST_PAIRING_H
#define PS4CAST_PAIRING_H

#include <stdint.h>

#define PAIRING_TOKEN_LEN 8
// 31 chars + NUL; see pairing.c for why it must never be indexed with `& 31`.
extern const char PAIRING_ALPHABET[];

// 1 iff s is exactly PAIRING_TOKEN_LEN characters, all members of
// PAIRING_ALPHABET, followed by '\0'. Rejects short/garbled tokens (including
// one with an embedded NUL) and anything outside the alphabet (lowercase,
// 0/O/1/I/L, punctuation, ...).
int pairing_token_valid(const char *s);

// One splitmix64 step (Vigna): folds `*state`'s linear accumulator forward
// and returns a separately whitened 64-bit output. Exposed so a caller with
// its own raw entropy (TSC samples, addresses, ...) can whiten it the same
// way before deriving a token or anything else from it.
uint64_t pairing_splitmix64(uint64_t *state);

// Derive an 8-char token from a 64-bit seed by running pairing_splitmix64
// once per character. `out` must hold PAIRING_TOKEN_LEN+1 bytes.
void pairing_token_from_seed(uint64_t seed, char *out);

#endif
