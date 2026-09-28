// epg.h — the programme guide (XMLTV) for the channel list.
//
// A background thread downloads the guide the playlist links to (#EXTM3U
// x-tvg-url), or the one set in Settings, streaming it through inflate.c and
// xmltv.c into a guide.c store while it arrives; the download is kept in
// /data so a relaunch or a playlist change re-reads it instead of fetching
// again. It refreshes every 12 hours. Lookups copy out under a short lock.
#ifndef PS4CAST_EPG_H
#define PS4CAST_EPG_H

#include <stdint.h>

void epg_init(void);                       // mutex; once, before any thread uses epg
void epg_start(void);                      // the download thread; after epg_init
void epg_set_url(const char *url);         // Settings override ("" = the playlist's link)
void epg_get_url(char *out, int cap);      // the override
void epg_source(char *out, int cap);       // the link actually used ("" = none)
void epg_refresh(void);                    // download again now
int  epg_version(void);                    // changes whenever a new guide is in place
int  epg_loaded(void);                     // 1 while a guide is in place
void epg_status(char *out, int cap);       // one line for Settings

typedef struct {
    int64_t start, stop;                   // Unix seconds
    char    title[128];
    char    sub[96];
    char    cat[48];
    char    desc[400];
} EpgProgramme;

// Programme on air on playlist channel `chan` at `now` (bit 1) and the one
// after it (bit 2); either may be missing. 0 = no guide data for the channel.
int  epg_now_next(int chan, int64_t now, EpgProgramme *onAir, EpgProgramme *next);
// Up to `max` programmes of channel `chan` from the one on air at `from`.
int  epg_schedule(int chan, int64_t from, EpgProgramme *out, int max);

#endif
