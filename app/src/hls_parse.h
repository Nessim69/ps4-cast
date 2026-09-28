// hls_parse.h — pure HLS playlist parsing: URL resolution, media-playlist
// segments, master-playlist variants, variant selection. No OS or network
// dependencies, so it compiles both on the PS4 and on the macOS host test
// harness (tests/host/).
#ifndef PS4CAST_HLS_PARSE_H
#define PS4CAST_HLS_PARSE_H

#include <stdint.h>

#define HLS_MAX_SEGMENTS 8192
#define HLS_MAX_VARIANTS 24

enum { VC_H264 = 0, VC_HEVC, VC_VP9, VC_AV1, VC_OTHER };
typedef struct { int bw, height, fps, codec; char url[2048]; char agroup[64]; char sgroup[64]; } HlsVariant;

// EXT-X-KEY METHOD=AES-128 (RFC 8216 4.3.2.4): whole-segment AES-128-CBC with
// PKCS#7 padding, 16-byte key fetched from `uri`.
typedef struct {
    char   *uri;          // resolved key URI (malloc'd)
    uint8_t iv[16];       // explicit IV, when hasIv
    int     hasIv;        // 0: IV = the segment's media sequence number
} HlsKey;

// How to fetch one segment (or the init segment): an optional byte range of
// its resource (EXT-X-BYTERANGE / EXT-X-MAP BYTERANGE) and an optional key.
typedef struct {
    int64_t off;          // first byte, when len > 0
    int64_t len;          // 0 = the whole resource
    int     key;          // index into HlsPlaylist.keys, -1 = not encrypted
} HlsSegRef;

typedef struct {
    char        **segs;                       // resolved absolute segment URLs
    int           segCount;
    // RFC 8216: segDisc[i] = the segment AFTER an #EXT-X-DISCONTINUITY tag.
    unsigned char segDisc[HLS_MAX_SEGMENTS];
    int           segDurMs[HLS_MAX_SEGMENTS]; // EXTINF duration for VOD seeking
    int64_t       totalDurMs;
    int           pendDisc;                   // next segment starts a discontinuity
    char         *initSeg;                    // fMP4 init segment URL (EXT-X-MAP)
    int           isLive;                     // no EXT-X-ENDLIST
    int           targetDurMs;                // EXT-X-TARGETDURATION
    int           mediaSeq;                   // EXT-X-MEDIA-SEQUENCE
    HlsVariant    variants[HLS_MAX_VARIANTS];
    int           variantCount;
    // Byte ranges and keys. segRef stays NULL for the common playlist that has
    // neither, so plain streams keep their exact old fetch path.
    HlsSegRef    *segRef;                     // [HLS_MAX_SEGMENTS] when allocated
    HlsSegRef     initRef;                    // EXT-X-MAP's range and key
    HlsKey       *keys;
    int           keyCount;
    char          unsupported[64];            // why hlspl_parse_media returned -2
} HlsPlaylist;

void hlspl_init(HlsPlaylist *pl);                       // zero, no allocation
void hlspl_free(HlsPlaylist *pl);                       // release everything, reset
// Release the media playlist only (segments, init, ranges, keys). The master's
// variants survive: switching variants reloads the media playlist but still
// needs the list to pick from.
void hlspl_free_media(HlsPlaylist *pl);

void hlspl_prefer_plain_s3(char *url, int cap);
// RFC 3986 reference resolution (absolute, "//host", "/path", "rel", "../",
// "?query"); drops the fragment. Also used for relative HTTP Location headers.
void hlspl_resolve_ref(const char *base, const char *ref, char *out, int cap);
// hlspl_resolve_ref + the plain-HTTP S3 rewrite, for playlist URIs.
void hlspl_resolve_url(const char *base, const char *ref, char *out, int cap);
int  hlspl_codec_from_str(const char *codecs);          // CODECS="avc1.x,mp4a.y"

// Parse a media playlist. body is MUTATED (line tokenizing). Returns 0, or
// -1 (no segments), -2 (encryption we cannot play: SAMPLE-AES or a DRM key
// format; pl->unsupported says which).
int  hlspl_parse_media(HlsPlaylist *pl, char *body, const char *base);

// Fetch parameters of segment `seg` (-1 = the EXT-X-MAP init segment): byte
// range (*len 0 = whole resource) and key (NULL = clear), with the IV to use.
void hlspl_seg_range(const HlsPlaylist *pl, int seg, int64_t *off, int64_t *len);
const HlsKey *hlspl_seg_key(const HlsPlaylist *pl, int seg, uint8_t iv[16]);
// 1 if any segment (or the init segment) needs a range or a key.
int  hlspl_has_seg_refs(const HlsPlaylist *pl);
// 1 if segment `seg` (-1 = init) is a plain whole-resource, unencrypted fetch.
int  hlspl_seg_is_plain(const HlsPlaylist *pl, int seg);

// Parse all master-playlist variants, sorted by bandwidth. Returns count.
int  hlspl_collect_variants(HlsPlaylist *pl, const char *body, const char *base);

// Variant selection. pick_best: only variants strictly below maxBw (0 = any).
int  hlspl_variant_score(const HlsVariant *v);
int  hlspl_pick_best(const HlsPlaylist *pl, int maxBw);
int  hlspl_pick_start_variant(const HlsPlaylist *pl);
int  hlspl_pick_fmp4_start_variant(const HlsPlaylist *pl, int autoMaxHeight);

#endif
