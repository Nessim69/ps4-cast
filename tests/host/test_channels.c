// Host test for the channel store (app/src/httpd_channels.c): lists past the
// old 2000-channel cap, the bouquet rail / A-Z / favourite filters, the
// /channels JSON, save + reload (including the old 3/4-column file), and the
// /channel/* endpoints. Files go under a temp dir via the libkernel shim.
#include "httpd_channels.h"
#include "httpd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static char g_resp[256];
static char *g_body;
static int g_bodyLen;
static void fake_send(OrbisNetId c, const char *status, const char *ctype, const char *body, int blen) {
    (void)c; (void)ctype;
    snprintf(g_resp, sizeof g_resp, "%s", status);
    free(g_body);
    g_body = malloc((size_t)blen + 1);
    memcpy(g_body, body, (size_t)blen);
    g_body[blen] = '\0';
    g_bodyLen = blen;
}

static int count_of(const char *hay, const char *needle) {
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)); p += strlen(needle)) n++;
    return n;
}

static char *big_list(int n, int groups, size_t *outLen) {
    size_t cap = (size_t)n * 200 + 256, o = 0;
    char *s = malloc(cap);
    o += (size_t)snprintf(s + o, cap - o, "#EXTM3U url-tvg=\"http://epg.example/guide.xml.gz,http://b/x.xml\"\n");
    for (int i = 0; i < n; i++) {
        o += (size_t)snprintf(s + o, cap - o,
                              "#EXTINF:-1 tvg-id=\"ch%d.tv\" tvg-logo=\"http://logo/%d.png\" group-title=\"G%d\",%c Channel %d\n"
                              "http://cdn.example/live/%d.m3u8\n",
                              i, i, i % groups, 'A' + (i % 26), i, i);
    }
    *outLen = o;
    return s;
}

int main(void) {
    char dir[] = "/tmp/ps4cast_chan_XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    setenv("PS4CAST_DATA", dir, 1);
    httpd_channels_init();
    CHECK(httpd_chan_count() == 0);

    // 5000 channels: past the old cap of 2000.
    size_t len;
    char *list = big_list(5000, 7, &len);
    int jl = 0;
    char *j = httpd_channels_load_playlist(list, "http://p.example/list.m3u", &jl);
    CHECK(j && jl == (int)strlen(j));
    CHECK(httpd_chan_count() == 5000);
    CHECK(j && count_of(j, "{\"i\":") == 5000);
    CHECK(j && strstr(j, "{\"i\":4999,\"n\":\"H Channel 4999\",\"g\":\"G1\",\"u\":\"http://cdn.example/live/4999.m3u8\",\"f\":0,\"l\":\"http://logo/4999.png\"}]"));
    free(j);

    char name[96], url[1024], tvg[128], logo[512], epg[1024];
    CHECK(httpd_chan_get(4321, name, sizeof name, url, sizeof url));
    CHECK(strcmp(name, "F Channel 4321") == 0 && strcmp(url, "http://cdn.example/live/4321.m3u8") == 0);
    CHECK(httpd_chan_meta(4321, tvg, sizeof tvg, logo, sizeof logo));
    CHECK(strcmp(tvg, "ch4321.tv") == 0 && strcmp(logo, "http://logo/4321.png") == 0);
    CHECK(!httpd_chan_get(5000, name, sizeof name, url, sizeof url));
    httpd_channels_epg_url(epg, sizeof epg);
    CHECK(strcmp(epg, "http://epg.example/guide.xml.gz") == 0);

    // Bouquet rail: All, Favourites, then G0..G6 in playlist order.
    CHECK(httpd_chan_rail_count() == 9);
    char rn[64];
    httpd_chan_rail_name(0, rn, sizeof rn); CHECK(strcmp(rn, "All") == 0);
    httpd_chan_rail_name(1, rn, sizeof rn); CHECK(strcmp(rn, "Favourites") == 0);
    httpd_chan_rail_name(2, rn, sizeof rn); CHECK(strcmp(rn, "G0") == 0);
    httpd_chan_rail_name(8, rn, sizeof rn); CHECK(strcmp(rn, "G6") == 0);
    httpd_chan_rail_select(4);   // G2: i % 7 == 2
    CHECK(httpd_chan_filter_count() == 714);
    CHECK(httpd_chan_filter_abs(0) == 2 && httpd_chan_filter_abs(1) == 9);
    // A letter narrows the group: 'C' is i % 26 == 2, with i % 7 == 2 -> i % 182 == 2.
    httpd_chan_filter('C', 0);   // the A-Z strip resets the rail to All
    CHECK(httpd_chan_filter_count() == 193);
    httpd_chan_filter(0, 0);
    CHECK(httpd_chan_filter_count() == 5000 && httpd_chan_filter_abs(4999) == 4999);
    CHECK(httpd_chan_letter_has('Z') && !httpd_chan_letter_has('#'));

    // Favourites survive in the filter and in the file.
    httpd_chan_toggle_fav(10);
    httpd_chan_toggle_fav(20);
    CHECK(httpd_chan_is_fav(10) && httpd_chan_is_fav(20) && !httpd_chan_is_fav(11));
    httpd_chan_rail_select(1);
    CHECK(httpd_chan_filter_fav() && httpd_chan_filter_count() == 2 && httpd_chan_filter_abs(1) == 20);
    httpd_chan_rail_select(0);

    // Edit keeps tvg-id/logo; tabs in fields never break the saved file.
    CHECK(httpd_channels_handle(0, "POST", "/channel/edit", "10\tRenamed\tNew\tGroup\thttp://x/10\n", fake_send));
    CHECK(strcmp(g_resp, "400 Bad Request") != 0);
    CHECK(httpd_channels_handle(0, "POST", "/channel/edit", "10\tRenamed\tNewGroup\thttp://x/10\n", fake_send));
    CHECK(strcmp(g_resp, "200 OK") == 0);
    httpd_chan_get(10, name, sizeof name, url, sizeof url);
    CHECK(strcmp(name, "Renamed") == 0 && strcmp(url, "http://x/10") == 0);
    httpd_chan_meta(10, tvg, sizeof tvg, logo, sizeof logo);
    CHECK(strcmp(tvg, "ch10.tv") == 0 && strcmp(logo, "http://logo/10.png") == 0);
    CHECK(httpd_chan_rail_count() == 10);   // NewGroup appended

    // Delete shifts the tuned index.
    httpd_chan_set_current(100);
    CHECK(httpd_channels_handle(0, "POST", "/channel/del", "5", fake_send));
    CHECK(httpd_chan_count() == 4999 && httpd_chan_current() == 99);
    httpd_chan_get(5, name, sizeof name, url, sizeof url);
    CHECK(strcmp(name, "G Channel 6") == 0);

    // Save + reload: everything comes back, including favourites and the EPG link.
    int ver = httpd_channels_version();
    httpd_channels_init();
    CHECK(httpd_channels_version() != ver);
    CHECK(httpd_chan_count() == 4999);
    CHECK(httpd_chan_is_fav(9) && httpd_chan_is_fav(19));   // 10 and 20 before the delete
    httpd_chan_get(9, name, sizeof name, url, sizeof url);
    CHECK(strcmp(name, "Renamed") == 0);
    httpd_chan_meta(4998, tvg, sizeof tvg, logo, sizeof logo);
    CHECK(strcmp(tvg, "ch4999.tv") == 0 && strcmp(logo, "http://logo/4999.png") == 0);
    httpd_channels_epg_url(epg, sizeof epg);
    CHECK(strcmp(epg, "http://epg.example/guide.xml.gz") == 0);

    // GET /channels.
    CHECK(httpd_channels_handle(0, "GET", "/channels", "", fake_send));
    CHECK(count_of(g_body, "{\"i\":") == 4999 && g_bodyLen == (int)strlen(g_body));
    CHECK(strstr(g_body, "\"f\":1,\"l\":\"http://logo/10.png\"") != NULL);

    // Add; clear.
    CHECK(httpd_channels_handle(0, "POST", "/channel/add", "Mine\t\thttp://mine/x.m3u8", fake_send));
    CHECK(httpd_chan_count() == 5000);
    CHECK(httpd_channels_handle(0, "POST", "/channel/del", "", fake_send));
    CHECK(httpd_chan_count() == 0 && httpd_chan_rail_count() == 2 && httpd_chan_filter_count() == 0);
    httpd_channels_epg_url(epg, sizeof epg);
    CHECK(epg[0] == '\0');

    // An old 3/4-column file still loads.
    char path[512];
    snprintf(path, sizeof path, "%s/ps4cast_channels.txt", dir);
    FILE *f = fopen(path, "w");
    fputs("One\tNews\thttp://a/1\nTwo\t\thttp://a/2\t1\nbad line\n", f);
    fclose(f);
    httpd_channels_init();
    CHECK(httpd_chan_count() == 2 && httpd_chan_is_fav(1) && httpd_chan_rail_count() == 3);
    httpd_chan_meta(0, tvg, sizeof tvg, logo, sizeof logo);
    CHECK(tvg[0] == '\0' && logo[0] == '\0');

    // A plain HLS stream is one entry named after the URL.
    j = httpd_channels_load_playlist("#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXTINF:6,\nseg1.ts\n",
                                     "http://h.example/live/stream.m3u8?x=1", &jl);
    CHECK(httpd_chan_count() == 1);
    httpd_chan_get(0, name, sizeof name, url, sizeof url);
    CHECK(strcmp(name, "stream.m3u8") == 0);
    free(j);

    // 100000 is the cap; the parser stops there.
    free(list);
    list = big_list(100010, 500, &len);
    clock_t t0 = clock();
    j = httpd_channels_load_playlist(list, "http://p.example/huge.m3u", &jl);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    CHECK(httpd_chan_count() == 100000);
    CHECK(httpd_chan_rail_count() == 502);
    CHECK(j && count_of(j, "{\"i\":") == 100000);
    printf("test_channels: 100000 channels parsed, indexed, saved and listed in %.2fs (%d KB of JSON)\n", secs, jl / 1024);
    free(j);
    free(list);
    httpd_channels_handle(0, "POST", "/channel/del", "", fake_send);
    free(g_body);

    remove(path);
    rmdir(dir);
    if (failures) { printf("test_channels: %d failure(s)\n", failures); return 1; }
    printf("test_channels: all passed\n");
    return 0;
}
