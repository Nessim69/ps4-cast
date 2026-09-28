// m3u.h — parse an M3U/IPTV channel list into entries. Pure (host-tested in
// tests/host/test_m3u.c); httpd_channels.c stores what it reports.
//
// Besides #EXTINF names and group-title / #EXTGRP groups, the per-channel
// HTTP options IPTV lists carry are turned into the player's own
// "url|User-Agent=..&Referer=.." form (urlopt.c), so channels that only play
// with the right Referer or User-Agent now play:
//   #EXTVLCOPT:http-referrer= / http-user-agent= / http-origin=   (VLC)
//   #EXTHTTP:{"User-Agent":"..","Referer":"..","Cookie":".."}      (TiviMate/OTT)
//   #KODIPROP:inputstream.adaptive.stream_headers=User-Agent=..&..  (Kodi)
//   #EXTINF:... user-agent="..", referrer="..", http-user-agent=".." (attributes)
//   #EXTM3U ... user-agent=".." / referrer=".."                     (list default)
// Options already on a "url|..." line win over the directives.
#ifndef PS4CAST_M3U_H
#define PS4CAST_M3U_H

typedef struct {
    char ua[256];
    char referer[512];
    char origin[256];
    char cookie[512];
} M3uOpts;

typedef struct {
    const char *name;     // #EXTINF title, or derived from the URL
    const char *group;    // group-title, else the sticky #EXTGRP, else ""
    const char *spec;     // URL plus its options, "url|K=V&.."
    const char *tvgId;    // tvg-id (the XMLTV channel id), or ""
    const char *logo;     // tvg-logo (or logo), or ""
} M3uEntry;

// The list's XMLTV guide link from its #EXTM3U line (x-tvg-url, url-tvg or
// tvg-url; the first of a comma-separated list). 1 if found.
int m3u_epg_url(const char *text, char *out, int cap);

// Calls add() for every channel, in order, until it returns nonzero.
// specCap bounds the spec (options that don't fit are dropped, most useful
// kept: User-Agent, Referer, Origin, then Cookie). Returns the entry count.
int m3u_parse(const char *text, int specCap,
              int (*add)(void *ctx, const M3uEntry *e), void *ctx);

// Building blocks, exposed for the tests.
void m3u_opts_clear(M3uOpts *o);
int  m3u_opts_line(M3uOpts *o, const char *line);     // 1 if the line was an option directive
int  m3u_spec(const M3uOpts *entry, const M3uOpts *defaults, const char *url, char *out, int cap);

#endif
