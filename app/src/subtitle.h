// subtitle.h — subtitle tracks, their cues and drawing them over the video.
//
// Sources: tracks embedded in the container (text: SubRip, ASS/SSA, WebVTT,
// mov_text -- read straight from the packets; bitmap: PGS, DVB, DVD -- via
// FFmpeg's decoders when the build has them), an external SRT/WebVTT file
// (sent by the web UI as text or a URL, or named by a DLNA sender), and HLS
// WebVTT renditions (EXT-X-MEDIA TYPE=SUBTITLES), fetched in the background.
// Switching tracks never reopens the stream.
#ifndef PS4CAST_SUBTITLE_H
#define PS4CAST_SUBTITLE_H

#include <stdint.h>
#include "gfx.h"

struct AVFormatContext;
struct AVPacket;

#define SUB_MAX_TRACKS 24
#define SUB_ID_EXTERNAL 200          // ids: embedded stream index < 100, HLS rendition 100+, external 200
typedef struct { int id; char label[72]; char lang[16]; } SubTrack;

// ---- player hooks -----------------------------------------------------------
void sub_init(void);                  // once, from player_init
// A source opened (opener worker, before its decode thread starts). spec
// keys the per-source choice; startUs is where the media timeline starts
// (external files are relative to it).
void sub_stream_opened(struct AVFormatContext *fmt, int isHls, const char *spec, int64_t startUs);
void sub_stream_closed(void);         // teardown, after the decode threads are joined
// Decode thread: apply a pending track change to fmt (discard flags, bitmap
// decoder), then route packets: sub_stream_index says which stream of fmt
// (by index, or by TS PID for a segment demuxer) carries the chosen track.
void sub_apply_pending(struct AVFormatContext *fmt);
int  sub_stream_index(const struct AVFormatContext *fmt, int bySegmentPid);
void sub_packet(const struct AVFormatContext *fmt, struct AVPacket *pkt);
void sub_seek(void);                  // decode thread, after a seek: embedded cues re-arrive
// Main thread: the presented frame's media time and relative position, the
// on-screen video rectangle, and drawing (after the picture, before the HUD).
void sub_set_clock(int64_t mediaUs);
void sub_set_video_rect(int x, int y, int w, int h);
// avoidBottom: screen rows at the bottom to keep clear (the HUD while shown).
void sub_draw(Gfx *g, int avoidBottom);

// ---- control (any thread) ------------------------------------------------------
int  sub_tracks(SubTrack *out, int max, int *cur);   // count; *cur = shown id (-1 = off)
int  sub_select(int id);                              // -1 = off; 0 ok, -1 unknown id
int  sub_load_text(const char *text, const char *name);  // external file content; 0 ok, -1 not SRT/VTT
int  sub_load_url(const char *url);                   // for what plays now, fetched in the background; 0 = queued
// For the source `spec` (a DLNA sender names the subtitle with the video):
// loaded once that source has opened, or now if it already has.
int  sub_load_url_for(const char *spec, const char *url);
void sub_set_lang(const char *lang);                  // Settings: auto-select this language ("" = off)
const char *sub_lang(void);
const char *sub_status(void);                         // one line for /status (last load result)
// Text of the shown track's cues at mediaUs (bitmaps count, contribute no
// text). Returns the number of active cues. Diagnostics and tests.
int  sub_text_at(int64_t mediaUs, char *out, int cap);

#endif
