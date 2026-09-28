// guide.h — compact programme-guide store built from an XMLTV stream.
//
// Only channels the playlist can use are kept (matched by tvg-id, else by a
// normalised name), only programmes inside a window around "now" are kept,
// and every string is interned into one arena, so a provider's full guide
// fits in a few MB. Built once off-thread, then read-only: lookups need no
// locking beyond keeping the Guide alive. Pure C, no platform calls.
#ifndef PS4CAST_GUIDE_H
#define PS4CAST_GUIDE_H

#include <stddef.h>
#include <stdint.h>
#include "xmltv.h"

#define GUIDE_BACK_SEC   (3 * 3600)     // programmes that ended up to 3 h ago
#define GUIDE_AHEAD_SEC  (36 * 3600)    // ...and those starting within 36 h
#define GUIDE_MAX_PROGS  200000         // 28 bytes each
#define GUIDE_MAX_ARENA  (16u << 20)    // all strings; descriptions stop at 3/4
#define GUIDE_DESC_KEEP  400            // description bytes kept per programme

// What the playlist wants: its tvg-ids and channel names.
typedef struct GuideWant GuideWant;
GuideWant *guide_want_new(void);
void       guide_want_add(GuideWant *w, const char *tvgId, const char *name);
uint64_t   guide_want_sig(const GuideWant *w);   // changes when the list does
void       guide_want_free(GuideWant *w);

typedef struct Guide Guide;
Guide *guide_new(const GuideWant *w, int64_t now);
// XmltvCallbacks for this guide (ctx = the Guide).
int    guide_on_channel(void *g, const XmltvChannel *c);
int    guide_on_programme(void *g, const XmltvProgramme *p);
void   guide_finish(Guide *g);                   // sort + fill gaps; call once
void   guide_free(Guide *g);

// Guide channel for a playlist channel, or -1.
int    guide_find(const Guide *g, const char *tvgId, const char *name);
int    guide_count(const Guide *g, int gch);     // programmes on it
typedef struct {
    int64_t     start, stop;
    const char *title, *sub, *cat, *desc;        // never NULL; valid while g lives
} GuideProg;
int    guide_prog(const Guide *g, int gch, int k, GuideProg *out);
// Programme on air at t (returns its index, *onAir = 1), else the next one
// after t (*onAir = 0); -1 when nothing is left.
int    guide_at(const Guide *g, int gch, int64_t t, int *onAir);

long   guide_channels(const Guide *g);           // matched channels
long   guide_programmes(const Guide *g);
size_t guide_bytes(const Guide *g);
int    guide_truncated(const Guide *g);          // a cap was hit
int64_t guide_built_at(const Guide *g);

// The name key used for matching ("UK: BBC One HD" -> "bbcone").
void   guide_name_key(const char *name, char *out, int cap);

#endif
