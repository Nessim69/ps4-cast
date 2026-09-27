// Host tests for resolve.c's page-scraping half. The interesting functions are
// static, so the test includes the translation unit directly and stubs its
// network dependency (aseg_fetch_opts) — resolve_page itself is on-device territory.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../app/src/aseg.h"

// --- minimal stubs for resolve.c's dependencies -------------------------
static int g_stubRc = 0;
static const char *g_stubBody = "";
static const char *g_stubCtype = "text/html; charset=utf-8";
static int g_stubCh = -1, g_stubMax = 0, g_stubResumed = -1, g_stubCalls = 0;
static uint64_t g_stubBudget = 0;
int aseg_fetch_opts(int ch, const char *url, uint8_t **buf, int *len, AsegOpts *o) {
    (void)url;
    g_stubCalls++;
    g_stubCh = ch; g_stubMax = o ? o->maxBytes : 0; g_stubBudget = o ? o->budgetUs : 0;
    *buf = NULL; *len = 0;
    if (o) snprintf(o->contentType, sizeof(o->contentType), "%s", g_stubCtype);
    if (g_stubRc != 0) return g_stubRc;
    // Like aseg: the predicate sees the final response's type before any body.
    if (o && o->stopAfterHeaders && o->stopAfterHeaders(g_stubCtype)) return ASEG_STOPPED;
    int n = (int)strlen(g_stubBody);
    if (n == 0) return -1;
    uint8_t *b = malloc((size_t)n + 1);
    memcpy(b, g_stubBody, (size_t)n + 1);
    *buf = b; *len = n;
    return 0;
}
void aseg_resume_ch(int ch) { g_stubResumed = ch; }
#include "../../app/src/urlopt.h"
// urlopt_apply etc. come from the real module:
#include "../../app/src/urlopt.c"
#include "../../app/src/netpolicy.c"

#include "../../app/src/resolve.c"

#include <stdio.h>
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void test_media_types(void) {
    static const char *media[] = {
        "video/mp4", "video/MP2T", "audio/mpegurl", "audio/x-mpegurl", "Audio/AAC",
        "application/vnd.apple.mpegurl", "application/vnd.apple.mpegurl; charset=UTF-8",
        "application/x-mpegURL", "application/dash+xml", "application/octet-stream",
        "binary/octet-stream", "  video/webm", 0 };
    static const char *pages[] = {
        "text/html", "text/html; charset=utf-8", "", "application/json", "text/plain",
        "application/octet-streamx", "video/", "application/xhtml+xml", 0 };
    for (int i = 0; media[i]; i++) {
        if (!is_media_type(media[i])) printf("  media not detected: '%s'\n", media[i]);
        CHECK(is_media_type(media[i]));
    }
    for (int i = 0; pages[i]; i++) {
        if (is_media_type(pages[i])) printf("  page taken for media: '%s'\n", pages[i]);
        CHECK(!is_media_type(pages[i]));
    }
    CHECK(!is_media_type(NULL));
}

static void test_is_page_kind(void) {
    char clean[512];
    urlopt_apply("https://iptv.example/live/user/pass/123", clean, sizeof clean);
    CHECK(resolve_is_page(clean) == 1);            // no extension, no kind: maybe a page
    urlopt_apply("https://iptv.example/live/user/pass/123|Type=file", clean, sizeof clean);
    CHECK(resolve_is_page(clean) == 0);            // sender says file: never scrape
    urlopt_apply("https://cdn.example/playlist?id=9|kind=hls&Referer=https://a.example/", clean, sizeof clean);
    CHECK(resolve_is_page(clean) == 0);
    urlopt_apply("https://dlna.example:8200/MediaItems/22|Type=FILE", clean, sizeof clean);
    CHECK(resolve_is_page(clean) == 0);            // case-insensitive
    urlopt_apply("https://site.example/watch/1|Type=page", clean, sizeof clean);
    CHECK(resolve_is_page(clean) == 1);            // unknown kind: extension rules apply
    urlopt_apply("https://cdn.example/v.mp4?sig=1", clean, sizeof clean);
    CHECK(resolve_is_page(clean) == 0);            // media extension
    CHECK(resolve_is_page("ftp://x/y") == 0);
}

int main(void) {
    char out[1600];

    // master playlist embedded in an HTML/JS page wins over renditions
    g_stubBody = "<html><script>var s=[\"https://cdn.example/hls/seg-2.m3u8\","
                 "\"https://cdn.example/hls/master.m3u8?token=abc\"];</script>";
    CHECK(resolve_page("https://site.example/watch/1", out, sizeof out) == 1);
    CHECK(strncmp(out, "https://cdn.example/hls/master.m3u8?token=abc", 30) == 0);
    CHECK(strstr(out, "Referer=https://site.example/watch/1") != NULL);
    // bounded probe on the stream's PLAYLIST channel, after clearing its stale abort
    CHECK(g_stubCh == ASEG_CH_PLAYLIST);
    CHECK(g_stubResumed == ASEG_CH_PLAYLIST);
    CHECK(g_stubMax == 2 * 1024 * 1024);
    CHECK(g_stubBudget == 8ULL * 1000 * 1000);

    // junk (ads/analytics) is skipped
    g_stubBody = "<html>https://google-analytics.com/a.m3u8 https://cdn.example/v.m3u8";
    CHECK(resolve_page("https://site.example/watch/2", out, sizeof out) == 1);
    CHECK(strncmp(out, "https://cdn.example/v.m3u8", 20) == 0);

    // no manifest -> clean failure with debug text
    g_stubBody = "<html>nothing here</html>";
    CHECK(resolve_page("https://site.example/watch/3", out, sizeof out) == 0);
    CHECK(strstr(resolve_debug(), "no manifest") != NULL);

    // a media Content-Type stops at the headers: "not a page", no body scanned
    g_stubBody = "https://cdn.example/should-not-be-found.m3u8";
    g_stubCtype = "video/mp2t";
    snprintf(out, sizeof out, "untouched");
    CHECK(resolve_page("https://iptv.example/live/user/pass/123", out, sizeof out) == 0);
    CHECK(strstr(resolve_debug(), "not a page") != NULL);
    CHECK(strstr(resolve_debug(), "video/mp2t") != NULL);
    CHECK(strcmp(out, "untouched") == 0);
    g_stubCtype = "application/octet-stream";
    CHECK(resolve_page("https://dlna.example:8200/MediaItems/22", out, sizeof out) == 0);
    CHECK(strstr(resolve_debug(), "not a page") != NULL);
    g_stubCtype = "";                               // no Content-Type: scanned as a page
    CHECK(resolve_page("https://site.example/watch/5", out, sizeof out) == 1);
    g_stubCtype = "text/html";

    // fetch failure propagates
    g_stubRc = -3; g_stubBody = "";
    CHECK(resolve_page("https://site.example/watch/4", out, sizeof out) == 0);
    CHECK(strstr(resolve_debug(), "fetch failed") != NULL);
    g_stubRc = 0;

    test_media_types();
    test_is_page_kind();

    printf(failures ? "test_resolve: %d FAILURES\n" : "test_resolve: all ok\n", failures);
    return failures ? 1 : 0;
}
