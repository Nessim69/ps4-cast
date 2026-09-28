// freetv_net.h — fetch iptv-org's language/country playlists and build the
// free-channel list (freetv.c). Blocking; run it off the main and HTTP threads.
#ifndef PS4CAST_FREETV_NET_H
#define PS4CAST_FREETV_NET_H

#include "freetv.h"

// 0 and a malloc'd M3U in *m3u, or -1; msg says what happened either way.
int  freetv_load(const FreeTvOpts *o, char **m3u, int *count, char *msg, int msgCap);
// Fetch from here instead of iptv-org (tests): "http://127.0.0.1:8000/iptv/".
void freetv_set_base(const char *base);

#endif
