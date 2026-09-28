// Host test for the programme-guide store (app/src/guide.c) fed by the real
// XMLTV parser: matching by tvg-id and by normalised name, undeclared
// channels, the time window, duplicate and open-ended programmes, now/next
// lookup, and the programme cap.
#include "guide.h"
#include "xmltv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

#define T0 1790596800LL     // 2026-09-28 12:00:00 UTC

static Guide *build(const GuideWant *w, const char *doc, int64_t now) {
    Guide *g = guide_new(w, now);
    XmltvCallbacks cb = { guide_on_channel, guide_on_programme, g };
    Xmltv *x = xmltv_new(&cb);
    xmltv_push(x, doc, (int)strlen(doc));
    CHECK(xmltv_finish(x) == XMLTV_OK);
    xmltv_free(x);
    guide_finish(g);
    return g;
}

static const char *title_at(const Guide *g, int gch, int64_t t, int *onAir) {
    GuideProg p;
    int k = guide_at(g, gch, t, onAir);
    return k >= 0 && guide_prog(g, gch, k, &p) ? p.title : NULL;
}

int main(void) {
    char k[64];
    guide_name_key("UK: BBC One HD", k, sizeof k);        CHECK(strcmp(k, "bbcone") == 0);
    guide_name_key("FR | TF1 (1080p) [backup]", k, sizeof k); CHECK(strcmp(k, "tf1") == 0);
    guide_name_key("US - CNN International FHD", k, sizeof k); CHECK(strcmp(k, "cnninternational") == 0);
    guide_name_key("BBC One +1", k, sizeof k);             CHECK(strcmp(k, "bbcone+1") == 0);
    guide_name_key("HD", k, sizeof k);                     CHECK(k[0] == '\0');
    guide_name_key("Das Erste", k, sizeof k);              CHECK(strcmp(k, "daserste") == 0);
    guide_name_key("NHK: ニュース", k, sizeof k);           CHECK(strcmp(k, "ニュース") == 0);

    GuideWant *w = guide_want_new();
    guide_want_add(w, "BBC1.uk", "BBC One");        // 0: by id (case-insensitive)
    guide_want_add(w, "", "UK: ITV 1 HD");          // 1: by name only
    guide_want_add(w, "wrong.id", "Channel 4");     // 2: id misses, name hits
    guide_want_add(w, "ghost.tv", "Ghost");         // 3: id used by programmes only
    guide_want_add(w, "", "Not In Guide");          // 4
    uint64_t sig = guide_want_sig(w);

    char doc[8192];
    snprintf(doc, sizeof doc,
        "<tv>"
        "<channel id=\"bbc1.uk\"><display-name>BBC One</display-name></channel>"
        "<channel id=\"itv1.uk\"><display-name>ITV1</display-name><display-name>ITV 1</display-name></channel>"
        "<channel id=\"c4.uk\"><display-name>Channel 4 HD</display-name></channel>"
        "<channel id=\"other.uk\"><display-name>Unwanted</display-name></channel>"
        // bbc1: 10:00-11:00 (ended 1h ago: kept), 06:00-07:00 (too old: dropped),
        // 12:00 open-ended until 12:30, 12:30 duplicate listed twice, 13:00 no
        // stop and last (+1h), and one two days ahead (dropped).
        "<programme start=\"20260928100000 +0000\" stop=\"20260928110000 +0000\" channel=\"bbc1.uk\"><title>Morning</title></programme>"
        "<programme start=\"20260928060000 +0000\" stop=\"20260928070000 +0000\" channel=\"bbc1.uk\"><title>Too old</title></programme>"
        "<programme start=\"20260928123000 +0000\" stop=\"20260928130000 +0000\" channel=\"bbc1.uk\"><title>Half past</title></programme>"
        "<programme start=\"20260928120000 +0000\" channel=\"BBC1.UK\"><title>Noon News</title><desc>The news &amp; weather</desc><category>News</category></programme>"
        "<programme start=\"20260928123000 +0000\" stop=\"20260928130000 +0000\" channel=\"bbc1.uk\"><title>Half past</title></programme>"
        "<programme start=\"20260928130000 +0000\" channel=\"bbc1.uk\"><title>Last</title></programme>"
        "<programme start=\"20260930120000 +0000\" stop=\"20260930130000 +0000\" channel=\"bbc1.uk\"><title>Too far</title></programme>"
        "<programme start=\"20260928113000 +0000\" stop=\"20260928140000 +0000\" channel=\"itv1.uk\"><title>Film</title></programme>"
        "<programme start=\"20260928121500 +0000\" stop=\"20260928124500 +0000\" channel=\"c4.uk\"><title>Quiz</title></programme>"
        "<programme start=\"20260928120000 +0000\" stop=\"20260928130000 +0000\" channel=\"other.uk\"><title>Nobody</title></programme>"
        "<programme start=\"20260928120000 +0000\" stop=\"20260928150000 +0000\" channel=\"ghost.tv\"><title>Boo</title></programme>"
        "</tv>");
    Guide *g = build(w, doc, T0 + 600);    // "now" = 12:10
    CHECK(guide_channels(g) == 4);          // bbc1, itv1, c4, ghost
    CHECK(guide_programmes(g) == 7);         // bbc 4 (one duplicate folded), itv, c4, ghost
    CHECK(!guide_truncated(g));

    int bbc = guide_find(g, "bbc1.UK", "whatever");
    int itv = guide_find(g, "", "ITV 1 HD");
    int c4 = guide_find(g, "wrong.id", "Channel 4");
    int ghost = guide_find(g, "ghost.tv", "");
    CHECK(bbc >= 0 && itv >= 0 && c4 >= 0 && ghost >= 0 && bbc != itv && itv != c4);
    CHECK(guide_find(g, "", "Not In Guide") == -1 && guide_find(g, "other.uk", "Unwanted") == -1);
    CHECK(guide_find(g, "", "BBC One") == bbc);      // display-name also maps

    int on;
    CHECK(guide_count(g, bbc) == 4);
    const char *t = title_at(g, bbc, T0 + 600, &on);
    CHECK(t && strcmp(t, "Noon News") == 0 && on);
    GuideProg p;
    int kk = guide_at(g, bbc, T0 + 600, &on);
    CHECK(guide_prog(g, bbc, kk, &p) && p.start == T0 && p.stop == T0 + 1800);   // open end -> next start
    CHECK(strcmp(p.desc, "The news & weather") == 0 && strcmp(p.cat, "News") == 0 && p.sub[0] == '\0');
    CHECK(guide_prog(g, bbc, kk + 1, &p) && strcmp(p.title, "Half past") == 0);
    CHECK(guide_prog(g, bbc, kk + 2, &p) && strcmp(p.title, "Last") == 0 && p.stop == T0 + 7200);
    t = title_at(g, bbc, T0 - 1800, &on);            // 11:30: gap -> next is Noon News
    CHECK(t && strcmp(t, "Noon News") == 0 && !on);
    t = title_at(g, bbc, T0 - 7000, &on);            // 10:03: Morning on air
    CHECK(t && strcmp(t, "Morning") == 0 && on);
    CHECK(guide_at(g, bbc, T0 + 9000, &on) == -1);   // after the last one
    t = title_at(g, c4, T0 + 600, &on);
    CHECK(t && strcmp(t, "Quiz") == 0 && !on);
    t = title_at(g, itv, T0 + 600, &on);
    CHECK(t && strcmp(t, "Film") == 0 && on);
    guide_free(g);

    // The signature follows the list.
    GuideWant *w2 = guide_want_new();
    guide_want_add(w2, "BBC1.uk", "BBC One");
    CHECK(guide_want_sig(w2) != sig);
    guide_want_free(w2);

    // Cap: more programmes than GUIDE_MAX_PROGS for one wanted channel.
    {
        Guide *big = guide_new(w, T0);
        XmltvProgramme pr = { "bbc1.uk", 0, 0, "Show", "", "", "" };
        for (long i = 0; i < GUIDE_MAX_PROGS + 10; i++) {
            pr.start = T0 - 3600 + i / 10;
            pr.stop = pr.start + 60;
            guide_on_programme(big, &pr);
        }
        CHECK(guide_programmes(big) == GUIDE_MAX_PROGS && guide_truncated(big));
        guide_finish(big);
        CHECK(guide_programmes(big) < GUIDE_MAX_PROGS);   // same-start duplicates folded
        printf("test_guide: %ld programmes, %zu KB\n", guide_programmes(big), guide_bytes(big) / 1024);
        guide_free(big);
    }
    guide_want_free(w);
    if (failures) { printf("test_guide: %d failure(s)\n", failures); return 1; }
    printf("test_guide: all ok\n");
    return 0;
}
