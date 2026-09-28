// httpd_channels.h — IPTV/M3U channel store shared by the web UI and the
// on-screen D-pad zapper. Split out of httpd.c: storage + parsing + the
// /channels and /channel/* endpoints live here.
#ifndef PS4CAST_HTTPD_CHANNELS_H
#define PS4CAST_HTTPD_CHANNELS_H

#include <stddef.h>
#include <orbis/Net.h>

// Load persisted channels and init the module mutex. Call once from httpd_start.
void httpd_channels_init(void);
void httpd_channels_save(void);   // persist current list (also used by DLNA paths)

// Bumped on every change to the channel list (playlist load, init-time
// restore, add/edit/del, favourites), so /status can hand the phone a
// "chan_ver" and it only re-fetches /channels when it moves. The tuned
// channel is /status "chan_cur" and does not bump it.
int httpd_channels_version(void);

// Endpoint router for GET /channels and POST /channel/{add,edit,del,fav}.
// Returns 1 if the request was handled. send_response is httpd.c's writer.
typedef void (*HttpdSendFn)(OrbisNetId c, const char *status, const char *ctype,
                            const char *body, int blen);
int httpd_channels_handle(OrbisNetId c, const char *method, const char *path,
                          const char *body, HttpdSendFn send_response);

// The store updates the receiver's HUD title when a channel is tuned.
void httpd_channels_set_push_cb(void (*cb)(const char *url));

// Parse an M3U/text playlist into the store, persist, and return the channel
// list as the same JSON GET /channels serves (malloc'd; NULL when nothing
// parsed). *len is its length.
char *httpd_channels_load_playlist(const char *text, const char *srcUrl, int *len);
// Channel i's XMLTV id (tvg-id) and logo URL; 1 if i exists.
int  httpd_chan_meta(int i, char *tvgId, int tvgCap, char *logo, int logoCap);
// The loaded playlist's XMLTV guide link ("" if it named none).
void httpd_channels_epg_url(char *out, int cap);
// Mark channel i tuned; returns 1 and copies its URL when valid.
int  httpd_channels_tune(int i, char *urlOut, int urlCap);

#endif
