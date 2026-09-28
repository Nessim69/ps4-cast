// subs.h — subtitle cues: parsing (SRT, WebVTT, ASS/SSA and mov_text packets),
// text clean-up and wrapping, and a time-sorted cue list. Pure: no OS or
// FFmpeg, so tests/host/test_subs.c covers it. Rendering and track handling
// live in subtitle.c.
#ifndef PS4CAST_SUBS_H
#define PS4CAST_SUBS_H

#include <stdint.h>

#define SUB_END_OPEN INT64_MAX     // shown until the next cue replaces it (PGS/DVB)

typedef struct {
    int64_t   startUs, endUs;      // media time (the video's presentation timeline)
    char     *text;                // cleaned UTF-8, lines split by '\n'; NULL for a bitmap
    uint32_t *argb;                // bitmap cue (premultiplied-free ARGB), else NULL
    int       x, y, w, h;          // bitmap placement on its canvas
    int       canvasW, canvasH;    // canvas the placement refers to (e.g. 1920x1080)
} SubCue;

typedef struct {
    SubCue *cues;
    int     n, cap;
} SubList;

void subs_list_init(SubList *l);
void subs_list_free(SubList *l);                  // frees every cue's text/bitmap
void subs_list_clear(SubList *l);
// Insert (takes ownership of text/argb). An open-ended cue before it is
// closed at its start. -1 on allocation failure (cue freed).
int  subs_list_add(SubList *l, SubCue *c);
// Cues visible at time t, up to max, in start order. Returns the count.
int  subs_list_active(const SubList *l, int64_t t, const SubCue **out, int max);
// Drop cues that ended before t (live/embedded streams keep memory bounded).
void subs_list_prune(SubList *l, int64_t t);
// A "clear screen" event (PGS/DVB with no regions): open-ended cues end at t.
void subs_list_end_open(SubList *l, int64_t t);
// Move every cue of src into dst (src is left empty). Returns cues moved.
int  subs_list_take_all(SubList *dst, SubList *src);

// Parse a whole SRT or WebVTT file (detected from its content). offsetUs is
// added to every cue. For WebVTT with X-TIMESTAMP-MAP (HLS), cue times are
// mapped to the MPEG-TS timeline first (RFC 8216 3.5). Returns cues added,
// or -1 if the text is neither format.
int  subs_parse_file(SubList *l, const char *text, int64_t offsetUs);

// Packet payloads of embedded text tracks -> cleaned text (malloc'd, or NULL
// when empty). ass: a Matroska ASS/SSA event ("ReadOrder,Layer,Style,...,Text").
char *subs_text_plain(const char *s, int len);    // SubRip / WebVTT / plain text
char *subs_text_ass(const char *s, int len);
char *subs_text_movtext(const uint8_t *p, int len);

// Many SRT files are Windows-1252 (or Latin-1), not UTF-8. NULL if `s` is
// valid UTF-8 already; otherwise a malloc'd UTF-8 conversion.
char *subs_to_utf8(const char *s);

// Subtitle URL a DLNA sender put in its DIDL-Lite metadata: Samsung-style
// <sec:CaptionInfo(Ex)>, or a <res> whose protocolInfo is SRT/WebVTT. 1 if found.
int  subs_didl_url(const char *didl, char *out, int cap);

// Clean markup in place: HTML-ish tags (<i>, <font ..>, <c.x>, <v Name>),
// ASS overrides ({\an8}), \N line breaks and entities (&amp; &lt; &gt; &nbsp;).
void subs_clean(char *s);

// Wrap text into at most maxLines lines of at most maxW pixels as measured by
// width(); words longer than a line are kept whole. Lines are written into
// out (joined by '\n'). Returns the line count.
int  subs_wrap(const char *text, int maxW, int maxLines, int (*width)(const char *s, void *ctx),
               void *ctx, char *out, int cap);

#endif
