// Host test for the deploy agent's crypto and wire parsing
// (payloads/ps4cast-dpi/sha256.c + agent_proto.c): SHA-256 / HMAC-SHA256
// known-answer vectors, the request MAC against a vector computed with
// Python's hmac module (what scripts/push-goldhen-dpi.py sends), and the
// packet parser's rejection of malformed input.
#include "sha256.h"
#include "agent_proto.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void hex(const uint8_t *b, int n, char *out) {
    for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
}
static int unhex(const char *s, uint8_t *out) {
    int n = 0;
    for (; s[0] && s[1]; s += 2) { unsigned v; sscanf(s, "%2x", &v); out[n++] = (uint8_t)v; }
    return n;
}
static int sha_is(const void *d, size_t n, const char *want) {
    Sha256 s; uint8_t o[32]; char h[65];
    sha256_init(&s); sha256_update(&s, d, n); sha256_final(&s, o); hex(o, 32, h);
    return strcmp(h, want) == 0;
}
static int hmac_is(const uint8_t *k, size_t kl, const void *d, size_t n, const char *want) {
    HmacSha256 m; uint8_t o[32]; char h[65];
    hmac_sha256_init(&m, k, kl); hmac_sha256_update(&m, d, n); hmac_sha256_final(&m, o); hex(o, 32, h);
    return strcmp(h, want) == 0;
}

int main(void) {
    // FIPS 180-2 vectors
    CHECK(sha_is("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK(sha_is("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK(sha_is("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    {   // one million 'a', fed in odd-sized pieces to exercise buffering
        Sha256 s; uint8_t o[32]; char h[65], a[997];
        memset(a, 'a', sizeof a);
        sha256_init(&s);
        size_t left = 1000000;
        while (left) { size_t n = left < sizeof a ? left : sizeof a; sha256_update(&s, a, n); left -= n; }
        sha256_final(&s, o); hex(o, 32, h);
        CHECK(strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
    }
    // RFC 4231 HMAC-SHA256 test cases 1, 2 and 6 (key longer than a block)
    uint8_t k1[20]; memset(k1, 0x0b, sizeof k1);
    CHECK(hmac_is(k1, 20, "Hi There", 8, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    CHECK(hmac_is((const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 28,
                  "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    uint8_t k6[131]; memset(k6, 0xaa, sizeof k6);
    CHECK(hmac_is(k6, 131, "Test Using Larger Than Block-Size Key - Hash Key First", 54,
                  "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));

    // The request MAC and packet layout the deploy script produces (vector
    // from Python: hmac.new(secret, label+nonce+len+pkt, sha256)).
    uint8_t secret[32], nonce[32], pkt[512], mac[32];
    for (int i = 0; i < 32; i++) { secret[i] = (uint8_t)i; nonce[i] = (uint8_t)(100 + i); }
    int plen = unhex("0100000020000000687474703a2f2f31302e302e302e323a393839382f6a736f6e2f302e6a736f6e0e000000"
                     "50533420436173742030342e3632240000004956303030302d5043535430303030315f30302d505334434153"
                     "543030303030303030310500000050533447440000f0000000000000000000", pkt);
    CHECK(plen == 119);
    agent_mac(secret, nonce, pkt, (uint32_t)plen, mac);
    char mh[65]; hex(mac, 32, mh);
    CHECK(strcmp(mh, "91e3bad0a6d1ed24b87431b865962ed35febee6a362113b5ecf8bdbc6402ef01") == 0);
    uint8_t other[32]; memcpy(other, mac, 32); other[31] ^= 1;
    CHECK(mac_equal(mac, mac) && !mac_equal(mac, other));

    static AgentRequest r;
    CHECK(agent_parse(pkt, (uint32_t)plen, &r) == 0);
    CHECK(r.cmd == AGENT_CMD_INSTALL);
    CHECK(strcmp(r.url, "http://10.0.0.2:9898/json/0.json") == 0);
    CHECK(strcmp(r.name, "PS4 Cast 04.62") == 0);
    CHECK(strcmp(r.id, "IV0000-PCST00001_00-PS4CAST000000001") == 0);
    CHECK(strcmp(r.type, "PS4GD") == 0);
    CHECK(r.size == 15728640ull);

    // malformed input is rejected, never read past
    CHECK(agent_parse(pkt, (uint32_t)plen - 1, &r) != 0);             // truncated
    uint8_t big[600]; memcpy(big, pkt, (size_t)plen); big[plen] = 0;
    CHECK(agent_parse(big, (uint32_t)plen + 1, &r) != 0);              // trailing byte
    uint8_t bad[512]; memcpy(bad, pkt, (size_t)plen);
    bad[plen - 4] = 1;                                                 // icon_len != 0
    CHECK(agent_parse(bad, (uint32_t)plen, &r) != 0);
    memcpy(bad, pkt, (size_t)plen); bad[4] = 0xff; bad[5] = 0xff;      // url length overruns
    CHECK(agent_parse(bad, (uint32_t)plen, &r) != 0);
    memcpy(bad, pkt, (size_t)plen); bad[10] = 0;                       // NUL inside url
    CHECK(agent_parse(bad, (uint32_t)plen, &r) != 0);
    memcpy(bad, pkt, (size_t)plen); bad[0] = 7;                        // unknown command
    CHECK(agent_parse(bad, (uint32_t)plen, &r) != 0);
    uint8_t stop[4] = { 0, 0, 0, 0 };
    CHECK(agent_parse(stop, 4, &r) == 0 && r.cmd == AGENT_CMD_STOP);
    uint8_t stop5[5] = { 0, 0, 0, 0, 9 };
    CHECK(agent_parse(stop5, 5, &r) != 0);
    CHECK(agent_parse(stop, 3, &r) != 0);
    CHECK(strlen(AGENT_SECRET_PLACEHOLDER) == AGENT_SECRET_LEN);

    if (fails) { printf("test_agent_crypto: %d failure(s)\n", fails); return 1; }
    printf("test_agent_crypto: all ok\n");
    return 0;
}
