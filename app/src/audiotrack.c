#include "audiotrack.h"
#include "lang.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

const char *atrack_tag(const AVStream *st, const char *key) {
    const AVDictionaryEntry *e = av_dict_get(st->metadata, key, NULL, 0);
    return e && e->value[0] ? e->value : NULL;
}

static const AVCodec *decoder_for(const AVFormatContext *fmt, int i) {
    if (i < 0 || i >= (int)fmt->nb_streams) return NULL;
    const AVStream *st = fmt->streams[i];
    if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) return NULL;
    return avcodec_find_decoder(st->codecpar->codec_id);
}

int atrack_pick(AVFormatContext *fmt, int pick, const char *langPref, const AVCodec **dec) {
    const AVCodec *c = decoder_for(fmt, pick);
    if (c) { *dec = c; return pick; }
    for (unsigned i = 0; langPref && langPref[0] && i < fmt->nb_streams; i++) {
        const char *l = atrack_tag(fmt->streams[i], "language");
        if (!l || !lang_matches(langPref, l)) continue;
        if ((c = decoder_for(fmt, (int)i)) != NULL) { *dec = c; return (int)i; }
    }
    return av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, dec, 0);
}

void atrack_label(const AVStream *st, int ordinal, char *out, int cap) {
    const char *l = atrack_tag(st, "language");
    const char *title = atrack_tag(st, "title");
    const char *nm = l ? lang_name(l) : NULL;
    char base[40];
    if (nm) snprintf(base, sizeof(base), "%s", nm);
    else if (l && strcmp(l, "und") != 0) snprintf(base, sizeof(base), "%s", l);
    else snprintf(base, sizeof(base), "Track %d", ordinal);
    const char *id = avcodec_get_name(st->codecpar->codec_id);
    const char *codec = strcmp(id, "ac3") == 0 ? "AC-3" : strcmp(id, "eac3") == 0 ? "E-AC-3"
                      : strcmp(id, "truehd") == 0 ? "TrueHD" : strcmp(id, "opus") == 0 ? "Opus"
                      : strcmp(id, "vorbis") == 0 ? "Vorbis" : NULL;
    char cbuf[16];
    if (!codec) {
        int k = 0;
        for (; id[k] && k < (int)sizeof(cbuf) - 1; k++) cbuf[k] = (char)toupper((unsigned char)id[k]);
        cbuf[k] = '\0';
        codec = cbuf;
    }
    int ch = st->codecpar->ch_layout.nb_channels;
    char chs[16];
    snprintf(chs, sizeof(chs), "%s", ch == 1 ? "mono" : ch == 2 ? "stereo" : ch == 6 ? "5.1" : ch == 8 ? "7.1" : "");
    if (!chs[0] && ch > 0) snprintf(chs, sizeof(chs), "%dch", ch);
    snprintf(out, (size_t)cap, "%s%s%.24s (%s%s%s)", base, title ? " - " : "", title ? title : "",
             codec, chs[0] ? " " : "", chs);
}

int atrack_find_id(const AVFormatContext *fmt, int id) {
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && fmt->streams[i]->id == id)
            return (int)i;
    return -1;
}
