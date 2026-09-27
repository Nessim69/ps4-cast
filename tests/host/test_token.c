// test_token.c — host-only unit tests for the pure pairing-token helpers in
// pairing.c (alphabet, validator, seed -> token). The on-console entropy
// gathering (TSC reads via sceKernelReadTsc) can't be exercised here -- it
// lives in httpd.c's token_generate, which needs PS4 headers this build
// doesn't have.
#include "pairing.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int is_alphabet_char(char c) {
    return c && strchr(PAIRING_ALPHABET, c) != NULL;
}

// Regression test for the original bug: `cs[t & 31]` could select index 31,
// one past the last real character, into the array's own NUL terminator.
// Every seed, across a wide sweep, must still produce 8 valid characters.
static void test_from_seed_always_valid(void) {
    uint64_t seed = 0x0123456789ABCDEFULL;
    for (long i = 0; i < 1000000; i++) {
        char tok[PAIRING_TOKEN_LEN + 1];
        pairing_token_from_seed(seed, tok);
        assert(strlen(tok) == PAIRING_TOKEN_LEN);
        for (int j = 0; j < PAIRING_TOKEN_LEN; j++) assert(is_alphabet_char(tok[j]));
        assert(pairing_token_valid(tok));
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL + (uint64_t)i;
    }
    // Edge seeds most likely to expose an off-by-one.
    char tok[PAIRING_TOKEN_LEN + 1];
    pairing_token_from_seed(0, tok);
    assert(pairing_token_valid(tok));
    pairing_token_from_seed(~0ULL, tok);
    assert(pairing_token_valid(tok));
    printf("test_from_seed_always_valid: ok (1000002 seeds)\n");
}

static void test_validator_accepts_good_tokens(void) {
    assert(pairing_token_valid("23456789"));
    assert(pairing_token_valid("ZZZZZZZZ"));
    assert(pairing_token_valid("A2B3C4D5"));
    printf("test_validator_accepts_good_tokens: ok\n");
}

static void test_validator_rejects_bad_tokens(void) {
    assert(!pairing_token_valid(NULL));
    assert(!pairing_token_valid(""));
    assert(!pairing_token_valid("2345678"));       // 7 chars: short
    assert(!pairing_token_valid("234567890"));     // 9 chars: long
    assert(!pairing_token_valid("2345678\0Z"));    // embedded NUL at position 7
    assert(!pairing_token_valid("\0BCDEFGH"));     // embedded NUL at position 0 (the bug's worst case)
    assert(!pairing_token_valid("abcdefgh"));      // lowercase
    assert(!pairing_token_valid("ABCDEFGh"));      // one lowercase char
    assert(!pairing_token_valid("01234567"));      // 0 excluded (looks like O)
    assert(!pairing_token_valid("OABCDEFG"));      // O excluded
    assert(!pairing_token_valid("1ABCDEFG"));      // 1 excluded (looks like I/l)
    assert(!pairing_token_valid("IABCDEFG"));      // I excluded
    assert(!pairing_token_valid("LABCDEFG"));      // L excluded
    assert(!pairing_token_valid("AB CDEFG"));      // space
    printf("test_validator_rejects_bad_tokens: ok\n");
}

int main(void) {
    test_validator_accepts_good_tokens();
    test_validator_rejects_bad_tokens();
    test_from_seed_always_valid();
    printf("test_token: all tests passed\n");
    return 0;
}
