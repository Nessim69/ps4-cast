// Host test for the XMLTV parser (app/src/xmltv.c): channels and programmes
// with entities, CDATA, comments, a DOCTYPE with an internal subset, time
// zones, first-of-many titles, clipping, stop, and non-XMLTV input -- fed
// whole and byte by byte.
#include "xmltv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct {
    char log[16384];
    int  n, stopAfter, calls;
} Log;

static void add(Log *l, const char *s) {
    size_t k = strlen(l->log);
    snprintf(l->log + k, sizeof(l->log) - k, "%s\n", s);
}

static int on_channel(void *ctx, const XmltvChannel *c) {
    Log *l = ctx;
    char b[1400];
    snprintf(b, sizeof b, "C id=[%s] n=%d [%s] [%s] icon=[%s]", c->id, c->nameCount, c->names[0], c->names[1], c->icon);
    add(l, b);
    return l->stopAfter && ++l->calls >= l->stopAfter;
}

static int on_prog(void *ctx, const XmltvProgramme *p) {
    Log *l = ctx;
    char b[2400];
    snprintf(b, sizeof b, "P ch=[%s] %lld-%lld t=[%s] s=[%s] c=[%s] d=[%.60s]%s", p->channel, (long long)p->start,
             (long long)p->stop, p->title, p->subTitle, p->category, p->desc, strlen(p->desc) > 60 ? "..." : "");
    add(l, b);
    return l->stopAfter && ++l->calls >= l->stopAfter;
}

static const char DOC[] =
    "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE tv SYSTEM \"xmltv.dtd\" [ <!ENTITY x \"y > z\"> <!-- ] > --> ]>\n"
    "<!-- a comment with <channel id=\"fake\"> inside -->\n"
    "<tv generator-info-name=\"test\" source-info-url='http://x/?a=1&amp;b=2'>\n"
    "  <channel id=\"bbc1.uk\">\n"
    "    <display-name lang=\"en\">BBC One</display-name>\n"
    "    <display-name>BBC  1\n  HD</display-name>\n"
    "    <icon src=\"http://logo/bbc1.png?a=1&amp;b=2\" width=\"100\"/>\n"
    "  </channel>\n"
    "  <channel id='arte.fr'><display-name>Arte &amp; Cie &#233;t&#xE9; &#x1F600;</display-name></channel>\n"
    "  <channel id=\"empty.x\"/>\n"
    "  <programme start=\"20260928120000 +0100\" stop=\"20260928130000 +0100\" channel=\"bbc1.uk\">\n"
    "    <title lang=\"en\">News at One</title>\n"
    "    <title lang=\"cy\">Newyddion</title>\n"
    "    <sub-title>Episode &quot;7&quot;</sub-title>\n"
    "    <desc lang=\"en\"><![CDATA[Headlines & <weather> ]] here]]></desc>\n"
    "    <category>News</category><category>Current affairs</category>\n"
    "    <icon src=\"http://x/p.png\"/>\n"
    "  </programme>\n"
    "  <programme start=\"20260928133000 +05:30\" channel=\"arte.fr\"><title>Sans fin</title></programme>\n"
    "  <programme start=\"202609281400 -0230\" stop=\"20260928150000\" channel=\"arte.fr\">\n"
    "    <title>  Spaced\t\tout  \n title  </title><desc/>\n"
    "  </programme>\n"
    "  <programme start=\"garbage\" channel=\"arte.fr\"><title>Dropped: bad start</title></programme>\n"
    "  <programme start=\"20260928150000 +0000\" channel=\"\"><title>Dropped: no channel</title></programme>\n"
    "  <?pi ignored?>\n"
    "</tv>\n";

static const char WANT[] =
    "C id=[bbc1.uk] n=2 [BBC One] [BBC 1 HD] icon=[http://logo/bbc1.png?a=1&b=2]\n"
    "C id=[arte.fr] n=1 [Arte & Cie \xC3\xA9t\xC3\xA9 \xF0\x9F\x98\x80] [] icon=[]\n"
    "C id=[empty.x] n=0 [] [] icon=[]\n"
    "P ch=[bbc1.uk] 1790593200-1790596800 t=[News at One] s=[Episode \"7\"] c=[News] d=[Headlines & <weather> ]] here]\n"
    "P ch=[arte.fr] 1790582400-0 t=[Sans fin] s=[] c=[] d=[]\n"
    "P ch=[arte.fr] 1790613000-1790607600 t=[Spaced out title] s=[] c=[] d=[]\n";

static void run(const char *doc, size_t len, int step, Log *l, int *rcPush, int *rcFin) {
    XmltvCallbacks cb = { on_channel, on_prog, l };
    Xmltv *x = xmltv_new(&cb);
    int rc = XMLTV_OK;
    for (size_t off = 0; off < len && rc == XMLTV_OK; off += (size_t)step) {
        int n = (int)(len - off < (size_t)step ? len - off : (size_t)step);
        rc = xmltv_push(x, doc + off, n);
    }
    *rcPush = rc;
    *rcFin = xmltv_finish(x);
    xmltv_free(x);
}

int main(void) {
    // Time parsing.
    CHECK(xmltv_time("20260928133000 +05:30") == 1790582400);
    CHECK(xmltv_time("202609281400 -0230") == 1790613000);
    CHECK(xmltv_time("19700102000000 +0000") == 86400);
    CHECK(xmltv_time("20260928120000 +0100") == 1790593200);
    CHECK(xmltv_time("20260928120000") == 1790596800);
    CHECK(xmltv_time("202609281200") == 1790596800);
    CHECK(xmltv_time("20260928") == 1790553600);
    CHECK(xmltv_time("20260928120000 -0500") == 1790596800 + 5 * 3600);
    CHECK(xmltv_time("20240229235959 +0000") == 1709251199);
    CHECK(xmltv_time("20261328120000") == 0 && xmltv_time("2026") == 0 && xmltv_time("") == 0);

    const int steps[] = { 1 << 20, 1, 3, 17, 256 };
    for (size_t k = 0; k < sizeof(steps) / sizeof(steps[0]); k++) {
        static Log l;
        memset(&l, 0, sizeof l);
        int rp, rf;
        run(DOC, sizeof(DOC) - 1, steps[k], &l, &rp, &rf);
        if (strcmp(l.log, WANT) != 0 || rp != XMLTV_OK || rf != XMLTV_OK) {
            failures++;
            printf("FAIL step %d rc=%d/%d\n--- got\n%s--- want\n%s", steps[k], rp, rf, l.log, WANT);
        }
    }

    // Description clipped to XMLTV_DESC_MAX-1 bytes; title to XMLTV_TEXT_MAX-1.
    {
        size_t cap = 20000;
        char *doc = malloc(cap);
        int o = snprintf(doc, cap, "<tv><programme start=\"20260928120000\" channel=\"c\"><title>");
        for (int i = 0; i < 400; i++) doc[o++] = 'T';
        o += snprintf(doc + o, cap - (size_t)o, "</title><desc>");
        for (int i = 0; i < 5000; i++) doc[o++] = (char)('a' + i % 26);
        o += snprintf(doc + o, cap - (size_t)o, "</desc></programme></tv>");
        static Log l;
        memset(&l, 0, sizeof l);
        int rp, rf;
        run(doc, (size_t)o, 1000, &l, &rp, &rf);
        CHECK(rp == XMLTV_OK && rf == XMLTV_OK);
        char *t = strstr(l.log, "t=[");
        CHECK(t && strspn(t + 3, "T") == XMLTV_TEXT_MAX - 1);
        free(doc);
    }
    // Stop from a callback.
    {
        static Log l;
        memset(&l, 0, sizeof l);
        l.stopAfter = 2;
        int rp, rf;
        run(DOC, sizeof(DOC) - 1, 5, &l, &rp, &rf);
        CHECK(rp == XMLTV_STOP && rf == XMLTV_STOP);
        CHECK(strstr(l.log, "arte.fr") && !strstr(l.log, "empty.x"));
    }
    // Not a guide: an HTML error page.
    {
        static char html[70000];
        memset(html, ' ', sizeof html);
        memcpy(html, "<html><body>404", 15);
        static Log l;
        memset(&l, 0, sizeof l);
        int rp, rf;
        run(html, sizeof html, 4096, &l, &rp, &rf);
        CHECK(rp == XMLTV_ERR && rf == XMLTV_ERR);
        memset(&l, 0, sizeof l);
        run("<html></html>", 13, 13, &l, &rp, &rf);
        CHECK(rp == XMLTV_OK && rf == XMLTV_ERR);
    }
    if (failures) { printf("test_xmltv: %d failure(s)\n", failures); return 1; }
    printf("test_xmltv: all ok\n");
    return 0;
}
