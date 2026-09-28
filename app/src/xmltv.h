// xmltv.h — push-style XMLTV parser (the programme-guide format IPTV
// playlists link with #EXTM3U x-tvg-url).
//
// Bytes go in as they come out of the network/inflate; <channel> and
// <programme> elements come out through callbacks as each one closes, so a
// guide of any size parses in constant memory. Only what a now/next guide
// needs is kept: channel id, display names and icon; programme channel,
// start/stop, title, sub-title, first category and description. Entities
// (named and numeric) are decoded, whitespace is collapsed, CDATA is text.
// Pure C, no platform calls.
#ifndef PS4CAST_XMLTV_H
#define PS4CAST_XMLTV_H

#include <stdint.h>

#define XMLTV_MAX_NAMES 4

typedef struct {
    const char *id;
    const char *names[XMLTV_MAX_NAMES];   // <display-name>s, in order
    int         nameCount;
    const char *icon;                     // <icon src>, "" if none
} XmltvChannel;

typedef struct {
    const char *channel;
    int64_t     start, stop;              // Unix seconds (UTC); stop 0 if absent
    const char *title;                    // first <title>
    const char *subTitle;                 // "" if none
    const char *category;                 // first <category>, "" if none
    const char *desc;                     // "" if none (clipped to XMLTV_DESC_MAX)
} XmltvProgramme;

#define XMLTV_TEXT_MAX 256
#define XMLTV_DESC_MAX 1024

typedef struct {
    // Nonzero from either stops the parse (xmltv_push returns XMLTV_STOP).
    int  (*channel)(void *ctx, const XmltvChannel *c);
    int  (*programme)(void *ctx, const XmltvProgramme *p);
    void  *ctx;
} XmltvCallbacks;

typedef struct Xmltv Xmltv;

#define XMLTV_OK    0
#define XMLTV_STOP  1
#define XMLTV_ERR  (-1)    // not XMLTV (no <tv> root seen in the first 64 KB)

Xmltv *xmltv_new(const XmltvCallbacks *cb);
int    xmltv_push(Xmltv *x, const char *p, int n);
int    xmltv_finish(Xmltv *x);   // XMLTV_OK if a <tv> root was seen
void   xmltv_free(Xmltv *x);
// Counts so far, for progress/status.
long   xmltv_channels(const Xmltv *x);
long   xmltv_programmes(const Xmltv *x);

// "YYYYMMDDhhmmss +hhmm" (seconds, minutes and zone optional; no zone = UTC)
// to Unix seconds; 0 if unparseable.
int64_t xmltv_time(const char *s);

#endif
