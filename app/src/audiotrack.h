// audiotrack.h — choosing and describing a container's audio tracks
// (MKV/MP4/TS languages). FFmpeg-only, so tests/host can run it against a
// host FFmpeg (test_audiotrack; see tests/host/Makefile).
#ifndef PS4CAST_AUDIOTRACK_H
#define PS4CAST_AUDIOTRACK_H

#include <libavformat/avformat.h>

// The audio stream to play: `pick` (a stream index the user chose, -1 = none)
// when it is audio this build can decode, else the first decodable stream in
// language `langPref` ("" = none), else FFmpeg's best. *dec = its decoder.
// Returns the stream index, or < 0 when there is no audio.
int  atrack_pick(AVFormatContext *fmt, int pick, const char *langPref, const AVCodec **dec);
// "English - Commentary (AC-3 5.1)"; `ordinal` numbers untagged tracks.
void atrack_label(const AVStream *st, int ordinal, char *out, int cap);
// Index of the audio stream whose container id (the TS PID) is `id`, or -1.
int  atrack_find_id(const AVFormatContext *fmt, int id);
// Metadata value (e.g. "language"), NULL when absent or empty.
const char *atrack_tag(const AVStream *st, const char *key);

#endif
