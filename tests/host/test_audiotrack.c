// Host test for audio-track choice and labels (app/src/audiotrack.c) over
// in-memory FFmpeg contexts -- no media files. Needs a host FFmpeg with the
// AAC and AC-3 decoders (see FFMPEG_HOST in the Makefile).
#include "audiotrack.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static AVStream *add(AVFormatContext *f, enum AVMediaType type, enum AVCodecID id, int ch,
                     const char *lang, const char *title, int pid, int isDefault) {
    AVStream *st = avformat_new_stream(f, NULL);
    st->id = pid;
    st->codecpar->codec_type = type;
    st->codecpar->codec_id = id;
    if (type == AVMEDIA_TYPE_AUDIO) {
        av_channel_layout_default(&st->codecpar->ch_layout, ch);
        st->codecpar->sample_rate = 48000;
    } else {
        st->codecpar->width = 640; st->codecpar->height = 360;
    }
    if (lang) av_dict_set(&st->metadata, "language", lang, 0);
    if (title) av_dict_set(&st->metadata, "title", title, 0);
    st->disposition = isDefault ? AV_DISPOSITION_DEFAULT : 0;
    return st;
}

int main(void) {
    AVFormatContext *f = avformat_alloc_context();
    add(f, AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_MPEG4, 0, NULL, NULL, 0x100, 1);          // 0
    add(f, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC, 2, "eng", NULL, 0x101, 1);            // 1
    add(f, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AC3, 6, "fre", "Commentary", 0x102, 0);    // 2
    add(f, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_NONE, 2, "deu", NULL, 0x103, 0);           // 3: no decoder
    add(f, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC, 1, NULL, NULL, 0x104, 0);             // 4: untagged
    add(f, AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC, 2, "ger", NULL, 0x105, 0);            // 5
    const AVCodec *dec = NULL;

    CHECK(atrack_pick(f, -1, "", &dec) == 1 && dec);           // FFmpeg's best: the default track
    CHECK(atrack_pick(f, 2, "", &dec) == 2 && dec && dec->id == AV_CODEC_ID_AC3);   // the user's pick
    CHECK(atrack_pick(f, 0, "", &dec) == 1);                   // a video "pick" is ignored
    CHECK(atrack_pick(f, 42, "", &dec) == 1);                  // out of range
    CHECK(atrack_pick(f, -1, "fra", &dec) == 2);               // fre matches fra
    CHECK(atrack_pick(f, -1, "French", &dec) == 2);
    CHECK(atrack_pick(f, -1, "de", &dec) == 5);                // undecodable deu skipped -> ger
    CHECK(atrack_pick(f, 3, "", &dec) == 1);                   // undecodable pick -> best
    CHECK(atrack_pick(f, -1, "jpn", &dec) == 1);               // no match -> best
    CHECK(atrack_pick(f, 4, "fra", &dec) == 4);                // a pick beats the language

    char l[80];
    atrack_label(f->streams[1], 1, l, sizeof l); CHECK(strcmp(l, "English (AAC stereo)") == 0);
    atrack_label(f->streams[2], 2, l, sizeof l); CHECK(strcmp(l, "French - Commentary (AC-3 5.1)") == 0);
    atrack_label(f->streams[4], 4, l, sizeof l); CHECK(strcmp(l, "Track 4 (AAC mono)") == 0);
    atrack_label(f->streams[5], 5, l, sizeof l); CHECK(strcmp(l, "German (AAC stereo)") == 0);
    if (failures) printf("  last label: %s\n", l);

    CHECK(atrack_find_id(f, 0x102) == 2);
    CHECK(atrack_find_id(f, 0x100) == -1);                     // the video PID is not audio
    CHECK(atrack_find_id(f, 0x999) == -1);

    avformat_free_context(f);
    printf(failures ? "test_audiotrack: %d FAILURES\n" : "test_audiotrack: all ok\n", failures);
    return failures ? 1 : 0;
}
