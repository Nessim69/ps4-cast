// Host tests for the pure HLS playlist parser (app/src/hls_parse.c).
#include "../../app/src/hls_parse.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void test_resolve_url(void) {
    char out[2048];
    hlspl_resolve_url("https://cdn.example.com/live/master.m3u8", "/seg/0.ts", out, sizeof out);
    CHECK(strcmp(out, "https://cdn.example.com/seg/0.ts") == 0);

    hlspl_resolve_url("https://cdn.example.com/live/master.m3u8", "0.ts", out, sizeof out);
    CHECK(strcmp(out, "https://cdn.example.com/live/0.ts") == 0);

    hlspl_resolve_url("https://cdn.example.com/live/master.m3u8", "//other.example/x.ts", out, sizeof out);
    CHECK(strcmp(out, "https://other.example/x.ts") == 0);

    hlspl_resolve_url("https://cdn.example.com/live/master.m3u8", "https://abs.example/a.ts?sig=1", out, sizeof out);
    CHECK(strcmp(out, "https://abs.example/a.ts?sig=1") == 0);

    // query on the base must not leak into a relative resolution
    hlspl_resolve_url("https://cdn.example.com/live/index.m3u8?token=abc", "0.ts", out, sizeof out);
    CHECK(strcmp(out, "https://cdn.example.com/live/0.ts") == 0);

    // amazonaws.com https downgraded to http (TLS-fingerprint workaround)
    hlspl_resolve_url("https://bucket.s3.amazonaws.com/pl.m3u8", "a.ts", out, sizeof out);
    CHECK(strncmp(out, "http://bucket.s3.amazonaws.com", 30) == 0);

    // dot segments, query-only and fragments resolve per RFC 3986
    hlspl_resolve_url("https://cdn.example.com/a/b/index.m3u8", "../c/0.ts", out, sizeof out);
    CHECK(strcmp(out, "https://cdn.example.com/a/c/0.ts") == 0);
    hlspl_resolve_url("https://cdn.example.com/a/index.m3u8?t=1", "0.ts#x", out, sizeof out);
    CHECK(strcmp(out, "https://cdn.example.com/a/0.ts") == 0);
    // a base with no path must not glue the reference onto the host name
    hlspl_resolve_url("https://cdn.example.com", "0.ts", out, sizeof out);
    CHECK(strcmp(out, "https://cdn.example.com/0.ts") == 0);
}

// HTTP Location resolution (aseg/httpsrc redirects): base = URL that answered.
static void test_resolve_redirect(void) {
    char out[2048];
    const char *base = "https://origin.example:8443/media/v1/play.php?id=7&sig=abc";
#define REDIR(loc, want) do { hlspl_resolve_ref(base, (loc), out, sizeof out); \
        if (strcmp(out, (want)) != 0) { printf("  got %s\n", out); } \
        CHECK(strcmp(out, (want)) == 0); } while (0)
    REDIR("https://cdn.example/f.mp4?e=1", "https://cdn.example/f.mp4?e=1");   // absolute
    REDIR("HTTP://cdn.example/f.mp4",      "http://cdn.example/f.mp4");        // scheme case
    REDIR("//cdn.example/f.mp4",           "https://cdn.example/f.mp4");       // scheme-relative
    REDIR("/files/f.mp4",                  "https://origin.example:8443/files/f.mp4");   // absolute-path
    REDIR("f.mp4",                         "https://origin.example:8443/media/v1/f.mp4"); // relative
    REDIR("?id=8",                         "https://origin.example:8443/media/v1/play.php?id=8"); // query-only
    REDIR("../f.mp4",                      "https://origin.example:8443/media/f.mp4");    // ../
    REDIR("../../../../f.mp4",             "https://origin.example:8443/f.mp4");          // clamps at root
    REDIR("./x/../f.mp4?a=../b#frag",      "https://origin.example:8443/media/v1/f.mp4?a=../b");
    REDIR("f.mp4#t=10",                    "https://origin.example:8443/media/v1/f.mp4");  // fragment dropped
    REDIR("",                              "https://origin.example:8443/media/v1/play.php?id=7&sig=abc");
    REDIR("https://s3.amazonaws.com/b/k",  "https://s3.amazonaws.com/b/k");   // no S3 rewrite on redirects
#undef REDIR

    // RFC 3986 section 5.4 normal and abnormal examples (fragments dropped)
    base = "http://a/b/c/d;p?q";
    static const char *rfc[][2] = {
        {"g", "http://a/b/c/g"}, {"./g", "http://a/b/c/g"}, {"g/", "http://a/b/c/g/"},
        {"/g", "http://a/g"}, {"//g", "http://g"}, {"?y", "http://a/b/c/d;p?y"},
        {"g?y", "http://a/b/c/g?y"}, {"#s", "http://a/b/c/d;p?q"}, {"g#s", "http://a/b/c/g"},
        {";x", "http://a/b/c/;x"}, {"", "http://a/b/c/d;p?q"}, {".", "http://a/b/c/"},
        {"./", "http://a/b/c/"}, {"..", "http://a/b/"}, {"../", "http://a/b/"},
        {"../g", "http://a/b/g"}, {"../..", "http://a/"}, {"../../", "http://a/"},
        {"../../g", "http://a/g"}, {"../../../g", "http://a/g"}, {"/./g", "http://a/g"},
        {"/../g", "http://a/g"}, {"g.", "http://a/b/c/g."}, {".g", "http://a/b/c/.g"},
        {"g..", "http://a/b/c/g.."}, {"..g", "http://a/b/c/..g"}, {"./../g", "http://a/b/g"},
        {"./g/.", "http://a/b/c/g/"}, {"g/./h", "http://a/b/c/g/h"}, {"g/../h", "http://a/b/c/h"},
        {"g;x=1/./y", "http://a/b/c/g;x=1/y"}, {"g;x=1/../y", "http://a/b/c/y"},
        {"g?y/./x", "http://a/b/c/g?y/./x"}, {"g#s/../x", "http://a/b/c/g"},
    };
    for (unsigned i = 0; i < sizeof(rfc) / sizeof(rfc[0]); i++) {
        hlspl_resolve_ref(base, rfc[i][0], out, sizeof out);
        if (strcmp(out, rfc[i][1]) != 0) printf("  rfc '%s': got %s want %s\n", rfc[i][0], out, rfc[i][1]);
        CHECK(strcmp(out, rfc[i][1]) == 0);
    }

    // truncation never overruns the output buffer
    char small[24];
    memset(small, 'Z', sizeof small);
    hlspl_resolve_ref("https://a-rather-long-host.example/x/y", "../z.ts", small, sizeof small);
    CHECK(strlen(small) == sizeof small - 1);
}

static void test_parse_media_vod(void) {
    HlsPlaylist pl; hlspl_init(&pl);
    char body[] =
        "#EXTM3U\n"
        "#EXT-X-TARGETDURATION:6\n"
        "#EXT-X-PLAYLIST-TYPE:VOD\n"
        "#EXT-X-MEDIA-SEQUENCE:1\n"
        "#EXTINF:5.005,\n"
        "seg0.ts\n"
        "#EXTINF:4.0,\n"
        "seg1.ts\n"
        "#EXT-X-ENDLIST\n";
    CHECK(hlspl_parse_media(&pl, body, "https://cdn.example.com/vod/index.m3u8") == 0);
    CHECK(pl.segCount == 2);
    CHECK(pl.isLive == 0);
    CHECK(pl.targetDurMs == 6000);
    CHECK(pl.totalDurMs == 9005);
    CHECK(strcmp(pl.segs[0], "https://cdn.example.com/vod/seg0.ts") == 0);
    CHECK(pl.segDurMs[1] == 4000);
    hlspl_free(&pl);
}

static void test_parse_media_live_disc_map(void) {
    HlsPlaylist pl; hlspl_init(&pl);
    char body[] =
        "#EXTM3U\n"
        "#EXT-X-TARGETDURATION:4\n"
        "#EXT-X-MEDIA-SEQUENCE:77\n"
        "#EXT-X-MAP:URI=\"init.mp4\"\n"
        "#EXTINF:4.0,\n"
        "a.m4s\n"
        "#EXT-X-DISCONTINUITY\n"
        "#EXTINF:4.0,\n"
        "b.m4s\n";
    CHECK(hlspl_parse_media(&pl, body, "https://cdn.example.com/hls/index.m3u8") == 0);
    CHECK(pl.isLive == 1);
    CHECK(pl.mediaSeq == 77);
    CHECK(pl.initSeg && strcmp(pl.initSeg, "https://cdn.example.com/hls/init.mp4") == 0);
    CHECK(pl.segDisc[1] == 1 && pl.segDisc[0] == 0);
    hlspl_free(&pl);
}

static int iv_is(const uint8_t iv[16], const char *hex) {
    char h[33];
    for (int i = 0; i < 16; i++) sprintf(h + 2 * i, "%02x", iv[i]);
    return strcmp(h, hex) == 0;
}

static void test_parse_media_aes128(void) {
    HlsPlaylist pl; hlspl_init(&pl);
    uint8_t iv[16];
    const HlsKey *k;
    char body[] =
        "#EXTM3U\n"
        "#EXT-X-TARGETDURATION:4\n"
        "#EXT-X-MEDIA-SEQUENCE:300\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"keys/k1?t=a,b\",IV=0x000102030405060708090A0B0C0D0E0F\n"
        "#EXTINF:4,\n"
        "a.ts\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"https://keys.example/k2\"\n"   // no IV: sequence number
        "#EXTINF:4,\n"
        "b.ts\n"
        "#EXTINF:4,\n"
        "c.ts\n"
        "#EXT-X-KEY:METHOD=NONE\n"
        "#EXTINF:4,\n"
        "d.ts\n"
        "#EXT-X-ENDLIST\n";
    CHECK(hlspl_parse_media(&pl, body, "https://cdn.example/v/index.m3u8") == 0);
    CHECK(pl.segCount == 4);
    CHECK(hlspl_has_seg_refs(&pl));
    CHECK(pl.keyCount == 2);
    k = hlspl_seg_key(&pl, 0, iv);
    CHECK(k && strcmp(k->uri, "https://cdn.example/v/keys/k1?t=a,b") == 0);   // quoted comma kept
    CHECK(iv_is(iv, "000102030405060708090a0b0c0d0e0f"));
    k = hlspl_seg_key(&pl, 1, iv);
    CHECK(k && strcmp(k->uri, "https://keys.example/k2") == 0);
    CHECK(iv_is(iv, "0000000000000000000000000000012d"));                     // seq 301
    k = hlspl_seg_key(&pl, 2, iv);
    CHECK(k && iv_is(iv, "0000000000000000000000000000012e"));                // seq 302
    CHECK(hlspl_seg_key(&pl, 3, iv) == NULL);                                 // METHOD=NONE
    int64_t off, len;
    hlspl_seg_range(&pl, 1, &off, &len);
    CHECK(len == 0);                                                          // whole resource
    hlspl_free(&pl);

    // A short IV is right-aligned; a live refresh repeating the same tag
    // before every segment keeps one key entry.
    hlspl_init(&pl);
    char live[] =
        "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:9\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"k\",IV=0xABC\n#EXTINF:2,\ns1.ts\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"k\",IV=0xABC\n#EXTINF:2,\ns2.ts\n";
    CHECK(hlspl_parse_media(&pl, live, "http://h/p/l.m3u8") == 0);
    CHECK(pl.keyCount == 1);
    CHECK(strcmp(pl.keys[0].uri, "http://h/p/k") == 0);
    CHECK(hlspl_seg_key(&pl, 1, iv) && iv_is(iv, "00000000000000000000000000000abc"));
    hlspl_free(&pl);
}

static void test_parse_media_inline_key(void) {
    HlsPlaylist pl; hlspl_init(&pl);
    uint8_t iv[16];
    char body[] = "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"data:text/plain;base64,AAECAwQFBgcICQoLDA0ODw==\"\n"
                  "#EXTINF:4,\na.ts\n#EXT-X-ENDLIST\n";
    CHECK(hlspl_parse_media(&pl, body, "https://x/v/y.m3u8") == 0);
    const HlsKey *k = hlspl_seg_key(&pl, 0, iv);
    CHECK(k && strcmp(k->uri, "data:text/plain;base64,AAECAwQFBgcICQoLDA0ODw==") == 0);   // not resolved
    hlspl_free(&pl);
}

static void test_parse_media_drm_and_sample_aes(void) {
    HlsPlaylist pl; hlspl_init(&pl);
    uint8_t iv[16];
    // identity AES-128 offered next to FairPlay for the same segments: playable
    char both[] =
        "#EXTM3U\n"
        "#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"skd://x\",KEYFORMAT=\"com.apple.streamingkeydelivery\",KEYFORMATVERSIONS=\"1\"\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"k.bin\"\n"
        "#EXTINF:4,\na.ts\n#EXT-X-ENDLIST\n";
    CHECK(hlspl_parse_media(&pl, both, "https://x/y.m3u8") == 0);
    CHECK(hlspl_seg_key(&pl, 0, iv) != NULL);
    hlspl_free(&pl);

    hlspl_init(&pl);
    char drm[] =
        "#EXTM3U\n"
        "#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"skd://x\",KEYFORMAT=\"com.apple.streamingkeydelivery\"\n"
        "#EXTINF:4,\na.ts\n#EXT-X-ENDLIST\n";
    CHECK(hlspl_parse_media(&pl, drm, "https://x/y.m3u8") == -2);
    CHECK(strstr(pl.unsupported, "DRM") != NULL);
    hlspl_free(&pl);

    hlspl_init(&pl);
    char sample[] = "#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"k\"\n#EXTINF:4,\na.ts\n#EXT-X-ENDLIST\n";
    CHECK(hlspl_parse_media(&pl, sample, "https://x/y.m3u8") == -2);
    CHECK(strstr(pl.unsupported, "SAMPLE-AES") != NULL);
    hlspl_free(&pl);

    hlspl_init(&pl);
    char plain[] = "#EXTM3U\n#EXT-X-KEY:METHOD=NONE\n#EXTINF:4,\na.ts\n#EXT-X-ENDLIST\n";
    CHECK(hlspl_parse_media(&pl, plain, "https://x/y.m3u8") == 0);
    CHECK(!hlspl_has_seg_refs(&pl) && pl.segRef == NULL);    // plain: old fetch path
    CHECK(hlspl_seg_key(&pl, 0, iv) == NULL);
    hlspl_free(&pl);
}

static void test_parse_media_byterange(void) {
    HlsPlaylist pl; hlspl_init(&pl);
    int64_t off, len;
    char body[] =
        "#EXTM3U\n"
        "#EXT-X-MAP:URI=\"main.mp4\",BYTERANGE=\"720@0\"\n"
        "#EXTINF:4,\n#EXT-X-BYTERANGE:1000@720\nmain.mp4\n"
        "#EXTINF:4,\n#EXT-X-BYTERANGE:500\nmain.mp4\n"          // continues at 1720
        "#EXTINF:4,\n#EXT-X-BYTERANGE:300\nother.mp4\n"         // new resource: from 0
        "#EXTINF:4,\nwhole.ts\n"
        "#EXT-X-ENDLIST\n";
    CHECK(hlspl_parse_media(&pl, body, "https://cdn.example/v/index.m3u8") == 0);
    CHECK(pl.segCount == 4);
    CHECK(pl.initSeg && strcmp(pl.initSeg, "https://cdn.example/v/main.mp4") == 0);
    hlspl_seg_range(&pl, -1, &off, &len);  CHECK(off == 0 && len == 720);
    hlspl_seg_range(&pl, 0, &off, &len);   CHECK(off == 720 && len == 1000);
    hlspl_seg_range(&pl, 1, &off, &len);   CHECK(off == 1720 && len == 500);
    hlspl_seg_range(&pl, 2, &off, &len);   CHECK(off == 0 && len == 300);
    hlspl_seg_range(&pl, 3, &off, &len);   CHECK(len == 0);
    uint8_t iv[16];
    CHECK(hlspl_seg_key(&pl, 0, iv) == NULL && hlspl_seg_key(&pl, -1, iv) == NULL);
    hlspl_free(&pl);

    // EXT-X-MAP after a key: the init segment is encrypted with it
    hlspl_init(&pl);
    char enc[] =
        "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"k\",IV=0x1\n"
        "#EXT-X-MAP:URI=\"init.mp4\"\n#EXTINF:4,\na.m4s\n#EXT-X-ENDLIST\n";
    CHECK(hlspl_parse_media(&pl, enc, "https://x/y.m3u8") == 0);
    CHECK(hlspl_seg_key(&pl, -1, iv) != NULL && iv_is(iv, "00000000000000000000000000000001"));
    hlspl_free(&pl);
}

// Switching variants reloads only the media playlist; the master's variant
// list must survive (it was wiped, which silently disabled ABR switching).
static void test_free_media_keeps_variants(void) {
    HlsPlaylist pl; hlspl_init(&pl);
    char master[] =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360\nlo.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2000000,RESOLUTION=1280x720\nhi.m3u8\n";
    CHECK(hlspl_collect_variants(&pl, master, "https://x/m.m3u8") == 2);
    char media[] = "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"k\"\n#EXTINF:4,\na.ts\n";
    CHECK(hlspl_parse_media(&pl, media, "https://x/lo.m3u8") == 0);
    hlspl_free_media(&pl);
    CHECK(pl.variantCount == 2 && strstr(pl.variants[1].url, "hi.m3u8"));
    CHECK(pl.segs == NULL && pl.segCount == 0 && pl.keyCount == 0 && pl.segRef == NULL);
    char media2[] = "#EXTM3U\n#EXTINF:4,\nb.ts\n";
    CHECK(hlspl_parse_media(&pl, media2, "https://x/hi.m3u8") == 0);
    CHECK(pl.variantCount == 2 && pl.segCount == 1);
    hlspl_free(&pl);
    CHECK(pl.variantCount == 0);
}

static void test_collect_variants(void) {
    HlsPlaylist pl; hlspl_init(&pl);
    char body[] =
        "#EXTM3U\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=2400000,RESOLUTION=1280x536,CODECS=\"avc1.64001f,mp4a.40.2\"\n"
        "mid.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=670000,RESOLUTION=640x268,CODECS=\"avc1.42c015\"\n"
        "low.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=4400000,RESOLUTION=1920x804,CODECS=\"hvc1.1.6.L123.00\"\n"
        "hi.m3u8\n";
    CHECK(hlspl_collect_variants(&pl, body, "https://cdn.example.com/master.m3u8") == 3);
    // sorted by bandwidth
    CHECK(pl.variants[0].bw == 670000 && strstr(pl.variants[0].url, "low.m3u8"));
    CHECK(pl.variants[2].codec == VC_HEVC);
    CHECK(hlspl_codec_from_str("avc1.64001f") == VC_H264);

    // start variant: best H.264 at or under 2.5Mbps -> the 2.4M rendition
    CHECK(hlspl_pick_start_variant(&pl) == 1);
    // fMP4 lock: highest H.264 height within the bw cap -> 1280x536
    CHECK(hlspl_pick_fmp4_start_variant(&pl, 1080) == 1);
    CHECK(hlspl_pick_best(&pl, 700000) == 0);    // strictly-below filter
    CHECK(hlspl_pick_best(&pl, 0) == 1);         // best score: H264 mid tier
    CHECK(hlspl_pick_best(&pl, 2400000) == 0);   // excludes mid+hi
    hlspl_free(&pl);
}

int main(void) {
    test_resolve_url();
    test_resolve_redirect();
    test_parse_media_vod();
    test_parse_media_live_disc_map();
    test_parse_media_aes128();
    test_parse_media_inline_key();
    test_parse_media_drm_and_sample_aes();
    test_parse_media_byterange();
    test_free_media_keeps_variants();
    test_collect_variants();
    printf(failures ? "test_hls_parse: %d FAILURES\n" : "test_hls_parse: all ok\n", failures);
    return failures ? 1 : 0;
}
