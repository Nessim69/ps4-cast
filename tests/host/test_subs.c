// Host test for subtitle parsing, clean-up, wrapping and the cue list
// (app/src/subs.c).
#include "subs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)
#define S(x) ((int64_t)(x) * 1000000LL)
#define MS(x) ((int64_t)(x) * 1000LL)

static int charw(const char *s, void *ctx) { (void)ctx; return (int)strlen(s) * 10; }   // 10 px per char

int main(void) {
    SubList l;
    subs_list_init(&l);

    const char *srt =
        "\xEF\xBB\xBF" "1\r\n"
        "00:00:01,500 --> 00:00:03,000\r\n"
        "<i>Hello</i> &amp; welcome\r\n"
        "{\\an8}second line\r\n"
        "\r\n"
        "2\r\n"
        "00:00:04,000 --> 00:00:05,250\r\n"
        "<font color=\"#ff0000\">Red</font>\r\n"
        "\r\n"
        "3\n"
        "01:02:03.004 --> 01:02:04.000\n"
        "dotted millis\n";
    CHECK(subs_parse_file(&l, srt, 0) == 3);
    CHECK(l.n == 3);
    CHECK(l.cues[0].startUs == MS(1500) && l.cues[0].endUs == S(3));
    CHECK(strcmp(l.cues[0].text, "Hello & welcome\nsecond line") == 0);
    CHECK(strcmp(l.cues[1].text, "Red") == 0);
    CHECK(l.cues[2].startUs == S(3723) + MS(4));
    const SubCue *act[4];
    CHECK(subs_list_active(&l, MS(2000), act, 4) == 1 && act[0] == &l.cues[0]);
    CHECK(subs_list_active(&l, MS(3000), act, 4) == 0);          // end is exclusive
    CHECK(subs_list_active(&l, MS(4100), act, 4) == 1);
    subs_list_clear(&l);

    // WebVTT with a header, NOTE/STYLE blocks, ids, settings, mm:ss times
    const char *vtt =
        "WEBVTT - title\n\n"
        "STYLE\n::cue { color: yellow }\n\n"
        "NOTE a comment\nspanning lines\n\n"
        "intro\n"
        "00:01.000 --> 00:02.500 align:start position:10%\n"
        "<v Roger>It's <c.loud>me</c>\n\n"
        "00:00:03.000 --> 00:00:04.000\n"
        "<00:00:03.500>karaoke &lt;tag&gt;\n";
    CHECK(subs_parse_file(&l, vtt, S(10)) == 2);                   // offset applied
    CHECK(l.cues[0].startUs == S(11) && l.cues[0].endUs == MS(12500));
    CHECK(strcmp(l.cues[0].text, "It's me") == 0);
    CHECK(strcmp(l.cues[1].text, "karaoke <tag>") == 0);
    subs_list_clear(&l);

    // HLS WebVTT segment: LOCAL 0 maps to MPEG-TS 900000 (10 s)
    const char *hlsvtt =
        "WEBVTT\nX-TIMESTAMP-MAP=LOCAL:00:00:00.000,MPEGTS:900000\n\n"
        "00:00:01.000 --> 00:00:02.000\nmapped\n";
    CHECK(subs_parse_file(&l, hlsvtt, 0) == 1 && l.cues[0].startUs == S(11));
    subs_list_clear(&l);
    CHECK(subs_parse_file(&l, "WEBVTT\nX-TIMESTAMP-MAP=MPEGTS:180000,LOCAL:00:00:01.000\n\n00:00:01.000 --> 00:00:02.000\nx\n", 0) == 1);
    CHECK(l.cues[0].startUs == S(2));                              // 2 s (MPEGTS) + (1 - 1)
    subs_list_clear(&l);

    // Windows-1252 file: "café" with 0xE9, curly quotes 0x93/0x94
    CHECK(subs_parse_file(&l, "1\n00:00:01,000 --> 00:00:02,000\ncaf\xE9 \x93hi\x94\n", 0) == 1);
    CHECK(strcmp(l.cues[0].text, "caf\xC3\xA9 \xE2\x80\x9Chi\xE2\x80\x9D") == 0);
    subs_list_clear(&l);
    CHECK(subs_to_utf8("already \xC3\xA9 utf-8") == NULL);

    CHECK(subs_parse_file(&l, "not a subtitle file\n", 0) == -1);
    CHECK(subs_parse_file(&l, "WEBVTT\n\n", 0) == 0);

    // embedded packet payloads
    char *t = subs_text_ass("12,0,Default,,0,0,0,,{\\i1}Italic\\Nnext, with comma", 51);
    CHECK(t && strcmp(t, "Italic\nnext, with comma") == 0); free(t);
    t = subs_text_ass("Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,Raw line", 59);
    CHECK(t && strcmp(t, "Raw line") == 0); free(t);
    const uint8_t mt[] = { 0, 5, 'H', 'e', 'l', 'l', 'o', 0, 0, 0, 12, 's', 't', 'y', 'l' };
    t = subs_text_movtext(mt, sizeof mt);
    CHECK(t && strcmp(t, "Hello") == 0); free(t);
    CHECK(subs_text_plain("  <i> </i>  ", 12) == NULL);           // nothing visible
    CHECK(subs_text_movtext(mt, 1) == NULL);

    // list: sorted insert, duplicates, open-ended bitmap cues
    SubCue c1 = { S(5), S(6), strdup("five"), NULL, 0, 0, 0, 0, 0, 0 };
    SubCue c2 = { S(2), S(3), strdup("two"), NULL, 0, 0, 0, 0, 0, 0 };
    SubCue c3 = { S(5), S(6), strdup("five"), NULL, 0, 0, 0, 0, 0, 0 };   // re-delivered after a seek
    CHECK(subs_list_add(&l, &c1) == 0 && subs_list_add(&l, &c2) == 0 && subs_list_add(&l, &c3) == 0);
    CHECK(l.n == 2 && l.cues[0].startUs == S(2));
    uint32_t *bm = calloc(4, sizeof(uint32_t));
    SubCue b1 = { S(10), SUB_END_OPEN, NULL, bm, 0, 900, 2, 2, 1920, 1080 };
    CHECK(subs_list_add(&l, &b1) == 0);
    CHECK(subs_list_active(&l, S(100), act, 4) == 1 && act[0]->argb);  // shown until replaced
    uint32_t *bm2 = calloc(4, sizeof(uint32_t));
    SubCue b2 = { S(20), SUB_END_OPEN, NULL, bm2, 0, 800, 2, 2, 1920, 1080 };
    CHECK(subs_list_add(&l, &b2) == 0);
    CHECK(l.cues[2].endUs == S(20));                               // the first one now ends
    CHECK(subs_list_active(&l, S(15), act, 4) == 1 && act[0]->y == 900);
    {   // two regions of one PGS event (same start) are both shown
        SubList m; subs_list_init(&m);
        SubCue r1 = { S(1), SUB_END_OPEN, NULL, calloc(4, 4), 0, 100, 2, 2, 1920, 1080 };
        SubCue r2 = { S(1), SUB_END_OPEN, NULL, calloc(4, 4), 0, 900, 2, 2, 1920, 1080 };
        CHECK(subs_list_add(&m, &r1) == 0 && subs_list_add(&m, &r2) == 0);
        CHECK(subs_list_active(&m, S(2), act, 4) == 2);
        subs_list_free(&m);
    }
    SubList tmp; subs_list_init(&tmp);
    CHECK(subs_parse_file(&tmp, "WEBVTT\n\n00:00:30.000 --> 00:00:31.000\nmoved\n", 0) == 1);
    CHECK(subs_list_take_all(&l, &tmp) == 1 && tmp.n == 0 && l.n == 5);
    subs_list_free(&tmp);
    uint32_t *bm3 = calloc(4, sizeof(uint32_t));
    SubCue b3 = { S(40), SUB_END_OPEN, NULL, bm3, 0, 800, 2, 2, 1920, 1080 };
    CHECK(subs_list_add(&l, &b3) == 0);
    subs_list_end_open(&l, S(42));                                 // PGS "clear" event
    CHECK(l.cues[l.n - 1].endUs == S(42));
    subs_list_prune(&l, S(19));
    CHECK(l.n == 4 && l.cues[0].startUs == S(10));
    subs_list_free(&l);

    // DLNA DIDL-Lite subtitle references
    char su[256];
    CHECK(subs_didl_url("<DIDL-Lite><item><res protocolInfo=\"http-get:*:video/mp4:*\">http://h/v.mp4</res>"
                        "<sec:CaptionInfoEx sec:type=\"srt\">http://h/s.srt?a=1&amp;b=2</sec:CaptionInfoEx></item></DIDL-Lite>",
                        su, sizeof su) == 1 && strcmp(su, "http://h/s.srt?a=1&b=2") == 0);
    CHECK(subs_didl_url("<item><res protocolInfo=\"http-get:*:video/x-matroska:*\">http://h/v.mkv</res>"
                        "<res protocolInfo=\"http-get:*:text/vtt:*\"> http://h/c.vtt </res></item>", su, sizeof su) == 1 &&
          strcmp(su, "http://h/c.vtt") == 0);
    CHECK(subs_didl_url("<item><res protocolInfo=\"http-get:*:video/mp4:*\">http://h/v.mp4</res></item>", su, sizeof su) == 0);
    CHECK(subs_didl_url("<sec:CaptionInfo>file:///x.srt</sec:CaptionInfo>", su, sizeof su) == 0);   // not http(s)

    // wrapping (10 px per char)
    char out[256];
    CHECK(subs_wrap("the quick brown fox jumps", 100, 3, charw, NULL, out, sizeof out) == 3);
    CHECK(strcmp(out, "the quick\nbrown fox\njumps") == 0);
    CHECK(subs_wrap("keep\nhard break", 200, 3, charw, NULL, out, sizeof out) == 2 && strcmp(out, "keep\nhard break") == 0);
    CHECK(subs_wrap("a b c d e f", 30, 2, charw, NULL, out, sizeof out) == 2 && strcmp(out, "a b\nc d") == 0);
    CHECK(subs_wrap("supercalifragilistic", 50, 2, charw, NULL, out, sizeof out) == 1);   // long word whole

    printf(failures ? "test_subs: %d FAILURES\n" : "test_subs: all ok\n", failures);
    return failures ? 1 : 0;
}
