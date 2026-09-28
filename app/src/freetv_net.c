// freetv_net.c — download iptv-org's playlists for freetv.c.
#include "freetv_net.h"
#include "aseg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// GitHub Pages first; the same files on raw.githubusercontent.com when a
// network blocks github.io.
static const char *BASES[] = {
    "https://iptv-org.github.io/iptv/",
    "https://raw.githubusercontent.com/iptv-org/iptv/gh-pages/",
};
static char g_base[256];              // freetv_set_base (tests)

void freetv_set_base(const char *base) { snprintf(g_base, sizeof(g_base), "%s", base ? base : ""); }

// One playlist, trying each mirror from the one that last worked. *text is
// NUL-terminated and malloc'd.
static int fetch_list(const char *path, int *baseIdx, char **text) {
    int nb = g_base[0] ? 1 : (int)(sizeof(BASES) / sizeof(BASES[0]));
    for (int t = 0; t < nb; t++) {
        int bi = (*baseIdx + t) % nb;
        char url[512];
        snprintf(url, sizeof(url), "%s%s", g_base[0] ? g_base : BASES[bi], path);
        uint8_t *buf = NULL;
        int len = 0;
        if (aseg_fetch_ui(url, &buf, &len) == 0 && buf && len > 0) {
            uint8_t *nt = realloc(buf, (size_t)len + 1);
            if (nt) {
                nt[len] = '\0';
                if (strstr((char *)nt, "#EXTM3U")) { *text = (char *)nt; *baseIdx = bi; return 0; }
                buf = nt;
            }
        }
        free(buf);
    }
    return -1;
}

int freetv_load(const FreeTvOpts *o, char **m3u, int *count, char *msg, int msgCap) {
    *m3u = NULL;
    *count = 0;
    FreeTv *b = freetv_new(o);
    if (!b) { snprintf(msg, (size_t)msgCap, "Out of memory"); return -1; }
    int baseIdx = 0, got = 0, failed = 0;
    char missed[128] = "";
    for (int i = 0; i < o->nLangs + o->nFirst; i++) {
        int whole = i >= o->nLangs;
        char path[64];
        if (!whole) {
            snprintf(path, sizeof(path), "languages/%s.m3u", o->langs[i]);
        } else {
            const char *cc = o->first[i - o->nLangs];
            // iptv-org names country files in lower case, and the UK "uk".
            snprintf(path, sizeof(path), "countries/%c%c.m3u", cc[0] | 0x20, cc[1] | 0x20);
        }
        char *text = NULL;
        if (fetch_list(path, &baseIdx, &text) == 0) {
            freetv_add(b, text, whole);
            free(text);
            got++;
        } else {
            failed++;
            size_t k = strlen(missed);
            snprintf(missed + k, sizeof(missed) - k, "%s%s", k ? ", " : "", whole ? o->first[i - o->nLangs] : o->langs[i]);
        }
    }
    if (!got) {
        freetv_free(b);
        snprintf(msg, (size_t)msgCap, "Could not reach the iptv-org channel directory");
        return -1;
    }
    *m3u = freetv_m3u(b, count);
    freetv_free(b);
    if (!*m3u) { snprintf(msg, (size_t)msgCap, "Out of memory"); return -1; }
    if (failed) snprintf(msg, (size_t)msgCap, "%d channels found (could not download: %s)", *count, missed);
    else snprintf(msg, (size_t)msgCap, "%d channels found", *count);
    return 0;
}
