// inflate.c — push-style streaming DEFLATE decoder. See inflate.h.
//
// Resumable at any input byte: every decoding step (one literal, one
// length/distance pair, a block header including a dynamic block's code
// tables, a gzip/zlib header or trailer) either completes or is rolled back
// to where it started and retried when more input arrives. The unconsumed
// tail of the input (at most a few hundred bytes) is carried between pushes.
#include "inflate.h"

#include <stdlib.h>
#include <string.h>

#define WSIZE     32768u
#define FAST_BITS 9

typedef struct {
    uint16_t fast[1 << FAST_BITS];   // (sym << 4) | len for codes <= FAST_BITS; 0 = slow path
    uint16_t count[16];
    uint16_t sym[288];
} Huff;

enum {
    ST_DETECT, ST_GZHEAD, ST_ZHEAD, ST_BLOCK, ST_STORED, ST_CODES,
    ST_GZTRAIL, ST_ZTRAIL, ST_MEMBER_END, ST_DONE, ST_PLAIN, ST_ERR
};

struct Inflate {
    InflateOut out;
    void      *ctx;
    int        state;
    char       fmt;
    int        last;                 // current block is the final one
    uint32_t   storedLeft;
    // input carried between pushes, plus the bit offset into its first byte
    uint8_t   *in;
    size_t     inLen, inCap;
    size_t     bitpos;
    // output window
    uint8_t    win[WSIZE];
    uint32_t   wpos, flushed;        // flushed = start of the not-yet-emitted span
    uint64_t   total;                // bytes decoded in this member
    uint32_t   crc, adler1, adler2;
    uint32_t   crcTab[256];
    int        stopped;
    Huff       lit, dist;
};

typedef struct { const uint8_t *b; size_t len; size_t bitpos; } Br;

static size_t br_avail(const Br *r) { return r->len * 8 - r->bitpos; }

static uint32_t br_peek(const Br *r) {       // next 32 bits (zero past the end)
    size_t byte = r->bitpos >> 3;
    unsigned sh = (unsigned)(r->bitpos & 7);
    uint64_t v = 0;
    if (byte + 5 <= r->len) {
        for (int i = 0; i < 5; i++) v |= (uint64_t)r->b[byte + i] << (8 * i);
    } else {
        for (int i = 0; i < 5 && byte + (size_t)i < r->len; i++) v |= (uint64_t)r->b[byte + i] << (8 * i);
    }
    return (uint32_t)(v >> sh);
}

// n <= 24 bits; caller checked br_avail.
static uint32_t br_bits(Br *r, int n) {
    uint32_t v = n ? br_peek(r) & ((1u << n) - 1) : 0;
    r->bitpos += (size_t)n;
    return v;
}

static void br_align(Br *r) { r->bitpos = (r->bitpos + 7) & ~(size_t)7; }

// ---- Huffman tables ---------------------------------------------------------
static unsigned rev_bits(unsigned c, int n) {
    unsigned r = 0;
    for (int i = 0; i < n; i++) { r = (r << 1) | (c & 1); c >>= 1; }
    return r;
}

static int huff_build(Huff *h, const uint8_t *len, int n) {
    memset(h->count, 0, sizeof(h->count));
    for (int s = 0; s < n; s++) h->count[len[s]]++;
    h->count[0] = 0;
    int left = 1;
    for (int l = 1; l <= 15; l++) {
        left <<= 1;
        left -= h->count[l];
        if (left < 0) return -1;             // over-subscribed
    }
    uint16_t offs[16], next[16];
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = (uint16_t)(offs[l] + h->count[l]);
    for (int s = 0; s < n; s++) if (len[s]) h->sym[offs[len[s]]++] = (uint16_t)s;
    unsigned code = 0;
    next[0] = 0;
    for (int l = 1; l <= 15; l++) { code = (code + h->count[l - 1]) << 1; next[l] = (uint16_t)code; }
    memset(h->fast, 0, sizeof(h->fast));
    for (int s = 0; s < n; s++) {
        int l = len[s];
        if (!l) continue;
        unsigned c = next[l]++;
        if (l > FAST_BITS) continue;
        for (unsigned j = rev_bits(c, l); j < (1u << FAST_BITS); j += 1u << l)
            h->fast[j] = (uint16_t)((s << 4) | l);
    }
    return 0;
}

// Symbol, -2 = need more input, -1 = invalid code.
static int huff_decode(Br *r, const Huff *h) {
    size_t avail = br_avail(r);
    uint32_t bits = br_peek(r);
    uint16_t e = h->fast[bits & ((1u << FAST_BITS) - 1)];
    if (e) {
        int l = e & 15;
        if ((size_t)l > avail) return -2;
        r->bitpos += (size_t)l;
        return e >> 4;
    }
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= 15; l++) {
        if ((size_t)l > avail) return -2;
        code |= (int)((bits >> (l - 1)) & 1);
        int count = h->count[l];
        if (code - count < first) { r->bitpos += (size_t)l; return h->sym[index + (code - first)]; }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t LBASE[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t  LEXT[29]  = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t DBASE[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,
                                    4097,6145,8193,12289,16385,24577 };
static const uint8_t  DEXT[30]  = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static void fixed_tables(Inflate *z) {
    uint8_t len[288];
    int s = 0;
    for (; s < 144; s++) len[s] = 8;
    for (; s < 256; s++) len[s] = 9;
    for (; s < 280; s++) len[s] = 7;
    for (; s < 288; s++) len[s] = 8;
    huff_build(&z->lit, len, 288);
    for (s = 0; s < 30; s++) len[s] = 5;
    huff_build(&z->dist, len, 30);
}

// 0 ok, -2 need more, -1 bad.
static int dynamic_tables(Inflate *z, Br *r) {
    static const uint8_t ORDER[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    if (br_avail(r) < 14) return -2;
    int nlen = (int)br_bits(r, 5) + 257, ndist = (int)br_bits(r, 5) + 1, ncode = (int)br_bits(r, 4) + 4;
    if (nlen > 286 || ndist > 30) return -1;
    uint8_t len[320];
    memset(len, 0, 19);
    if (br_avail(r) < (size_t)ncode * 3) return -2;
    for (int i = 0; i < ncode; i++) len[ORDER[i]] = (uint8_t)br_bits(r, 3);
    Huff cl;
    if (huff_build(&cl, len, 19) != 0) return -1;
    int i = 0;
    while (i < nlen + ndist) {
        int sym = huff_decode(r, &cl);
        if (sym < 0) return sym;
        if (sym < 16) { len[i++] = (uint8_t)sym; continue; }
        int rep, val = 0;
        if (sym == 16) {
            if (i == 0) return -1;
            if (br_avail(r) < 2) return -2;
            val = len[i - 1]; rep = 3 + (int)br_bits(r, 2);
        } else if (sym == 17) {
            if (br_avail(r) < 3) return -2;
            rep = 3 + (int)br_bits(r, 3);
        } else {
            if (br_avail(r) < 7) return -2;
            rep = 11 + (int)br_bits(r, 7);
        }
        if (i + rep > nlen + ndist) return -1;
        while (rep--) len[i++] = (uint8_t)val;
    }
    if (!len[256]) return -1;                       // no end-of-block code
    if (huff_build(&z->lit, len, nlen) != 0) return -1;
    if (huff_build(&z->dist, len + nlen, ndist) != 0) return -1;
    return 0;
}

// ---- output -----------------------------------------------------------------
static void crc_init(Inflate *z) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        z->crcTab[n] = c;
    }
}

static void sums(Inflate *z, const uint8_t *p, uint32_t n) {
    if (z->fmt == 'g') {
        uint32_t c = z->crc ^ 0xFFFFFFFFu;
        for (uint32_t i = 0; i < n; i++) c = z->crcTab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
        z->crc = c ^ 0xFFFFFFFFu;
    } else if (z->fmt == 'z') {
        uint32_t a = z->adler1, b = z->adler2;
        while (n) {
            uint32_t k = n < 5552 ? n : 5552;
            n -= k;
            while (k--) { a += *p++; b += a; }
            a %= 65521u; b %= 65521u;
        }
        z->adler1 = a; z->adler2 = b;
    }
}

static int flush(Inflate *z) {
    if (z->wpos > z->flushed) {
        const uint8_t *p = z->win + z->flushed;
        uint32_t n = z->wpos - z->flushed;
        sums(z, p, n);
        z->flushed = z->wpos;
        if (!z->stopped && z->out(z->ctx, p, (int)n)) z->stopped = 1;
    }
    if (z->wpos == WSIZE) z->wpos = z->flushed = 0;
    return z->stopped;
}

static inline int put(Inflate *z, uint8_t b) {
    z->win[z->wpos++] = b;
    z->total++;
    return z->wpos == WSIZE ? flush(z) : 0;
}

static void member_reset(Inflate *z) {
    z->total = 0; z->crc = 0; z->adler1 = 1; z->adler2 = 0;
}

// ---- the state machine ------------------------------------------------------
// Runs over r until it needs more input (0), ends (1), stops (2) or fails (-1).
static int run(Inflate *z, Br *r) {
    for (;;) {
        size_t save = r->bitpos;
        switch (z->state) {
        case ST_GZHEAD: {
            if (br_avail(r) < 80) return 0;
            const uint8_t *h = r->b + (r->bitpos >> 3);
            size_t have = r->len - (r->bitpos >> 3), k = 10;
            if (h[0] != 0x1f || h[1] != 0x8b || h[2] != 8) return -1;
            int flg = h[3];
            if (flg & 0xE0) return -1;
            if (flg & 4) {                                  // FEXTRA
                if (have < k + 2) return 0;
                k += 2 + (size_t)(h[k] | h[k + 1] << 8);
            }
            for (int f = 8; f <= 16; f <<= 1) {             // FNAME, FCOMMENT: zero-terminated
                if (!(flg & f)) continue;
                while (k < have && h[k]) k++;
                if (k >= have) return 0;
                k++;
            }
            if (flg & 2) k += 2;                            // FHCRC
            if (k > have) return 0;
            r->bitpos += k * 8;
            member_reset(z);
            z->state = ST_BLOCK;
            break;
        }
        case ST_ZHEAD: {
            if (br_avail(r) < 16) return 0;
            int cmf = (int)br_bits(r, 8), flg = (int)br_bits(r, 8);
            if ((cmf & 15) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 || (flg & 0x20)) return -1;
            member_reset(z);
            z->state = ST_BLOCK;
            break;
        }
        case ST_BLOCK: {
            if (br_avail(r) < 3) return 0;
            z->last = (int)br_bits(r, 1);
            int type = (int)br_bits(r, 2);
            if (type == 0) {
                br_align(r);
                if (br_avail(r) < 32) { r->bitpos = save; return 0; }
                uint32_t len = br_bits(r, 16), nlen = br_bits(r, 16);
                if ((len ^ 0xFFFF) != nlen) return -1;
                z->storedLeft = len;
                z->state = ST_STORED;
            } else if (type == 1) {
                fixed_tables(z);
                z->state = ST_CODES;
            } else if (type == 2) {
                int rc = dynamic_tables(z, r);
                if (rc == -2) { r->bitpos = save; return 0; }
                if (rc < 0) return -1;
                z->state = ST_CODES;
            } else {
                return -1;
            }
            break;
        }
        case ST_STORED: {
            while (z->storedLeft) {
                size_t byte = r->bitpos >> 3;
                if (byte >= r->len) return 0;
                size_t n = r->len - byte;
                if (n > z->storedLeft) n = z->storedLeft;
                for (size_t i = 0; i < n; i++) {
                    if (put(z, r->b[byte + i])) { r->bitpos += (i + 1) * 8; z->storedLeft -= (uint32_t)(i + 1); return 2; }
                }
                r->bitpos += n * 8;
                z->storedLeft -= (uint32_t)n;
            }
            z->state = z->last ? (z->fmt == 'g' ? ST_GZTRAIL : ST_ZTRAIL) : ST_BLOCK;
            break;
        }
        case ST_CODES: {
            for (;;) {
                save = r->bitpos;
                int sym = huff_decode(r, &z->lit);
                if (sym == -2) { r->bitpos = save; return 0; }
                if (sym < 0) return -1;
                if (sym < 256) {
                    if (put(z, (uint8_t)sym)) return 2;
                    continue;
                }
                if (sym == 256) break;
                sym -= 257;
                if (sym >= 29) return -1;
                if (br_avail(r) < LEXT[sym]) { r->bitpos = save; return 0; }
                uint32_t len = LBASE[sym] + br_bits(r, LEXT[sym]);
                int ds = huff_decode(r, &z->dist);
                if (ds == -2) { r->bitpos = save; return 0; }
                if (ds < 0 || ds >= 30) return -1;
                if (br_avail(r) < DEXT[ds]) { r->bitpos = save; return 0; }
                uint32_t dist = DBASE[ds] + br_bits(r, DEXT[ds]);
                if (dist > z->total || dist > WSIZE) return -1;
                // The copy is part of this step: once the length/distance are
                // consumed the bytes go out, even if a flush asks to stop.
                int stop = 0;
                while (len--) {
                    uint8_t b = z->win[(z->wpos - dist) & (WSIZE - 1)];
                    if (put(z, b)) stop = 1;
                }
                if (stop) return 2;
            }
            z->state = z->last ? (z->fmt == 'g' ? ST_GZTRAIL : ST_ZTRAIL) : ST_BLOCK;
            break;
        }
        case ST_GZTRAIL: {
            br_align(r);
            if (br_avail(r) < 64) { r->bitpos = save; return 0; }
            if (flush(z)) { r->bitpos = save; return 2; }
            uint32_t crc = br_bits(r, 16); crc |= br_bits(r, 16) << 16;
            uint32_t isz = br_bits(r, 16); isz |= br_bits(r, 16) << 16;
            if (crc != z->crc || isz != (uint32_t)z->total) return -1;
            z->state = ST_MEMBER_END;
            break;
        }
        case ST_ZTRAIL: {
            br_align(r);
            if (br_avail(r) < 32) { r->bitpos = save; return 0; }
            if (flush(z)) { r->bitpos = save; return 2; }
            uint32_t a = 0;
            for (int i = 0; i < 4; i++) a = (a << 8) | br_bits(r, 8);
            if (a != ((z->adler2 << 16) | z->adler1)) return -1;
            z->state = ST_DONE;
            break;
        }
        case ST_MEMBER_END: {
            // Concatenated gzip members make one stream; anything else after a
            // member is trailing junk, ignored as gzip(1) does.
            size_t byte = r->bitpos >> 3;
            if (r->len - byte < 2) return 0;
            if (r->b[byte] == 0x1f && r->b[byte + 1] == 0x8b) { z->state = ST_GZHEAD; break; }
            z->state = ST_DONE;
            return 1;
        }
        case ST_DONE:
            r->bitpos = r->len * 8;
            return 1;
        default:
            return -1;
        }
    }
}

Inflate *inflate_new(InflateOut out, void *ctx) {
    Inflate *z = calloc(1, sizeof(*z));
    if (!z) return NULL;
    z->out = out; z->ctx = ctx;
    z->state = ST_DETECT;
    z->fmt = '?';
    crc_init(z);
    member_reset(z);
    return z;
}

void inflate_free(Inflate *z) {
    if (!z) return;
    free(z->in);
    free(z);
}

char inflate_format(const Inflate *z) { return z->fmt; }

static int detect(Inflate *z, const uint8_t *p, size_t n) {
    if (n < 2) return 0;
    if (p[0] == 0x1f && p[1] == 0x8b) { z->fmt = 'g'; z->state = ST_GZHEAD; }
    else if ((p[0] & 15) == 8 && (p[0] >> 4) <= 7 && ((p[0] << 8) | p[1]) % 31 == 0) { z->fmt = 'z'; z->state = ST_ZHEAD; }
    else { z->fmt = 'p'; z->state = ST_PLAIN; }
    return 1;
}

int inflate_push(Inflate *z, const uint8_t *p, int n) {
    if (z->state == ST_ERR) return INFLATE_ERR;
    if (z->stopped) return INFLATE_STOP;
    if (z->state == ST_DONE) return INFLATE_END;
    if (n <= 0) return INFLATE_OK;
    if (z->state == ST_PLAIN) return z->out(z->ctx, p, n) ? (z->stopped = 1, INFLATE_STOP) : INFLATE_OK;
    // Work buffer: the carried tail followed by the new bytes.
    if (z->inLen + (size_t)n > z->inCap) {
        size_t cap = z->inLen + (size_t)n + 1024;
        uint8_t *nb = realloc(z->in, cap);
        if (!nb) { z->state = ST_ERR; return INFLATE_NOMEM; }
        z->in = nb; z->inCap = cap;
    }
    memcpy(z->in + z->inLen, p, (size_t)n);
    z->inLen += (size_t)n;
    if (z->state == ST_DETECT) {
        if (!detect(z, z->in, z->inLen)) return INFLATE_OK;
        if (z->state == ST_PLAIN) {
            size_t len = z->inLen;
            z->inLen = 0;
            return z->out(z->ctx, z->in, (int)len) ? (z->stopped = 1, INFLATE_STOP) : INFLATE_OK;
        }
    }
    Br r = { z->in, z->inLen, z->bitpos };
    int rc = run(z, &r);
    if (rc == -1) { z->state = ST_ERR; return INFLATE_ERR; }
    if (rc != 2 && flush(z)) rc = 2;
    // Keep only the unconsumed tail.
    size_t used = r.bitpos >> 3;
    memmove(z->in, z->in + used, z->inLen - used);
    z->inLen -= used;
    z->bitpos = r.bitpos & 7;
    if (rc == 2) return INFLATE_STOP;
    return rc == 1 ? INFLATE_END : INFLATE_OK;
}

int inflate_finish(Inflate *z) {
    if (z->state == ST_ERR) return INFLATE_ERR;
    if (z->stopped) return INFLATE_STOP;
    if (z->state == ST_DETECT) {                 // 0 or 1 byte in all: not compressed
        z->fmt = 'p'; z->state = ST_PLAIN;
        if (z->inLen && z->out(z->ctx, z->in, (int)z->inLen)) { z->stopped = 1; return INFLATE_STOP; }
        z->inLen = 0;
        return INFLATE_END;
    }
    if (z->state == ST_PLAIN) return INFLATE_END;
    if (z->state == ST_MEMBER_END) z->state = ST_DONE;
    if (z->state == ST_DONE) return INFLATE_END;
    return INFLATE_SHORT;
}
