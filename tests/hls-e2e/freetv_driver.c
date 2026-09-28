// End-to-end free channels: freetv_net.c downloads the language and country
// playlists from server.py (standing in for iptv-org), one of them missing,
// and the result is added to a channel list that already has a channel;
// adding again brings nothing new. Usage: freetv_driver BASE_URL
#include "freetv_net.h"
#include "httpd.h"
#include "httpd_channels.h"
#include "aseg.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    alarm(60);
    aseg_init();
    httpd_channels_init();
    int jl = 0;
    free(httpd_channels_load_playlist("#EXTM3U\n#EXTINF:-1 group-title=\"Mine\",My Channel\nhttp://mine.example/1.m3u8\n",
                                      "http://p.example/mine.m3u", &jl));
    CHECK(httpd_chan_count() == 1);

    char base[256];
    snprintf(base, sizeof base, "%s/iptv/", argv[1]);
    freetv_set_base(base);
    FreeTvOpts o;
    freetv_opts_default(&o);                      // ara, eng, fra (fra is missing on the server), first TN
    char *m3u = NULL, msg[160];
    int found = 0;
    CHECK(freetv_load(&o, &m3u, &found, msg, sizeof msg) == 0);
    printf("  %s\n", msg);
    CHECK(found == 5 && strstr(msg, "could not download: fra"));

    int added = 0;
    free(httpd_channels_add_playlist(m3u, &added, &jl));
    CHECK(added == 5 && httpd_chan_count() == 6);
    char name[96], url[1024], grp[64];
    CHECK(httpd_chan_get(0, name, sizeof name, url, sizeof url) && !strcmp(name, "My Channel"));   // kept
    httpd_chan_get(1, name, sizeof name, NULL, 0);
    httpd_chan_group(1, grp, sizeof grp);
    CHECK(!strcmp(name, "El Watania 1") && !strcmp(grp, "Tunisia"));
    httpd_chan_rail_name(3, grp, sizeof grp);                 // All, Favourites, Mine, Tunisia, ...
    CHECK(!strcmp(grp, "Tunisia"));

    free(httpd_channels_add_playlist(m3u, &added, &jl));       // again: nothing new
    CHECK(added == 0 && httpd_chan_count() == 6);
    free(m3u);

    // Nothing reachable: a clear error.
    snprintf(base, sizeof base, "%s/nowhere/", argv[1]);
    freetv_set_base(base);
    CHECK(freetv_load(&o, &m3u, &found, msg, sizeof msg) == -1 && strstr(msg, "Could not reach"));
    printf("freetv ok\n");
    return 0;
}
