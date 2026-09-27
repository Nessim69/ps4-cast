#include "agent_proto.h"
#include "sha256.h"

void agent_mac(const uint8_t secret[AGENT_SECRET_LEN], const uint8_t nonce[32],
               const uint8_t *packet, uint32_t len, uint8_t out[32]) {
    uint8_t le[4] = { (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24) };
    HmacSha256 m;
    hmac_sha256_init(&m, secret, AGENT_SECRET_LEN);
    hmac_sha256_update(&m, AGENT_MAC_LABEL, sizeof(AGENT_MAC_LABEL) - 1);
    hmac_sha256_update(&m, nonce, 32);
    hmac_sha256_update(&m, le, 4);
    hmac_sha256_update(&m, packet, len);
    hmac_sha256_final(&m, out);
}

typedef struct { const uint8_t *p; uint32_t left; } Cursor;

static int take_u32(Cursor *c, uint32_t *v) {
    if (c->left < 4) return -1;
    *v = (uint32_t)c->p[0] | (uint32_t)c->p[1] << 8 | (uint32_t)c->p[2] << 16 | (uint32_t)c->p[3] << 24;
    c->p += 4; c->left -= 4;
    return 0;
}

static int take_u64(Cursor *c, uint64_t *v) {
    uint32_t lo, hi;
    if (take_u32(c, &lo) || take_u32(c, &hi)) return -1;
    *v = (uint64_t)hi << 32 | lo;
    return 0;
}

// A length-prefixed string into out[cap] (NUL-terminated). Rejects a length
// that doesn't fit or overruns the packet, and embedded NULs.
static int take_str(Cursor *c, char *out, uint32_t cap) {
    uint32_t n;
    if (take_u32(c, &n) || n >= cap || n > c->left) return -1;
    for (uint32_t i = 0; i < n; i++) {
        if (c->p[i] == 0) return -1;
        out[i] = (char)c->p[i];
    }
    out[n] = '\0';
    c->p += n; c->left -= n;
    return 0;
}

int agent_parse(const uint8_t *packet, uint32_t len, AgentRequest *out) {
    Cursor c = { packet, len };
    out->url[0] = out->name[0] = out->id[0] = out->type[0] = '\0';
    out->size = 0;
    if (take_u32(&c, &out->cmd)) return -1;
    if (out->cmd == AGENT_CMD_STOP) return c.left == 0 ? 0 : -1;
    if (out->cmd != AGENT_CMD_INSTALL) return -1;
    uint32_t icon;
    if (take_str(&c, out->url, sizeof(out->url)) ||
        take_str(&c, out->name, sizeof(out->name)) ||
        take_str(&c, out->id, sizeof(out->id)) ||
        take_str(&c, out->type, sizeof(out->type)) ||
        take_u64(&c, &out->size) ||
        take_u32(&c, &icon)) return -1;
    // Icons are not supported (the deploy script sends none); anything left
    // over means a different or corrupted layout.
    if (icon != 0 || c.left != 0) return -1;
    if (!out->url[0] || !out->id[0]) return -1;
    return 0;
}
