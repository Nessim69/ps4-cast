#include "lang.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

// ISO 639-2/T, 639-1, 639-2/B (when different), English name, native name.
typedef struct { const char *t, *one, *b, *name, *native; } Lang;
static const Lang LANGS[] = {
    { "eng", "en", 0,     "English",    0 },
    { "fra", "fr", "fre", "French",     "francais" },
    { "deu", "de", "ger", "German",     "deutsch" },
    { "spa", "es", 0,     "Spanish",    "espanol" },
    { "ita", "it", 0,     "Italian",    "italiano" },
    { "por", "pt", 0,     "Portuguese", "portugues" },
    { "nld", "nl", "dut", "Dutch",      "nederlands" },
    { "ara", "ar", 0,     "Arabic",     0 },
    { "tur", "tr", 0,     "Turkish",    "turkce" },
    { "rus", "ru", 0,     "Russian",    0 },
    { "ukr", "uk", 0,     "Ukrainian",  0 },
    { "pol", "pl", 0,     "Polish",     "polski" },
    { "ces", "cs", "cze", "Czech",      "cestina" },
    { "hun", "hu", 0,     "Hungarian",  "magyar" },
    { "ron", "ro", "rum", "Romanian",   "romana" },
    { "ell", "el", "gre", "Greek",      0 },
    { "swe", "sv", 0,     "Swedish",    "svenska" },
    { "nor", "no", 0,     "Norwegian",  "norsk" },
    { "nob", "nb", 0,     "Norwegian",  "bokmal" },
    { "dan", "da", 0,     "Danish",     "dansk" },
    { "fin", "fi", 0,     "Finnish",    "suomi" },
    { "heb", "he", 0,     "Hebrew",     0 },
    { "fas", "fa", "per", "Persian",    "farsi" },
    { "urd", "ur", 0,     "Urdu",       0 },
    { "hin", "hi", 0,     "Hindi",      0 },
    { "ben", "bn", 0,     "Bengali",    0 },
    { "tam", "ta", 0,     "Tamil",      0 },
    { "tel", "te", 0,     "Telugu",     0 },
    { "zho", "zh", "chi", "Chinese",    0 },
    { "yue", 0,    0,     "Cantonese",  0 },
    { "jpn", "ja", 0,     "Japanese",   0 },
    { "kor", "ko", 0,     "Korean",     0 },
    { "vie", "vi", 0,     "Vietnamese", 0 },
    { "tha", "th", 0,     "Thai",       0 },
    { "ind", "id", 0,     "Indonesian", "bahasa indonesia" },
    { "msa", "ms", "may", "Malay",      "bahasa melayu" },
    { "fil", 0,    0,     "Filipino",   "tagalog" },
    { "cat", "ca", 0,     "Catalan",    "catala" },
    { "hrv", "hr", 0,     "Croatian",   "hrvatski" },
    { "srp", "sr", 0,     "Serbian",    0 },
    { "bul", "bg", 0,     "Bulgarian",  0 },
    { "slk", "sk", "slo", "Slovak",     0 },
    { "slv", "sl", 0,     "Slovenian",  0 },
    { "kur", "ku", 0,     "Kurdish",    0 },
    { "sqi", "sq", "alb", "Albanian",   "shqip" },
};
#define NLANGS (int)(sizeof(LANGS) / sizeof(LANGS[0]))

static const Lang *lookup(const char *tag) {
    if (!tag) return NULL;
    while (*tag == ' ') tag++;
    char s[40];
    int n = 0;
    // The primary subtag of "en-US" / "pt_BR"; whole words for names.
    for (; tag[n] && n < (int)sizeof(s) - 1; n++) {
        char c = (char)tolower((unsigned char)tag[n]);
        if (c == '-' || c == '_') break;
        s[n] = c;
    }
    s[n] = '\0';
    while (n > 0 && s[n - 1] == ' ') s[--n] = '\0';
    if (n < 2) return NULL;
    for (int i = 0; i < NLANGS; i++) {
        const Lang *l = &LANGS[i];
        if (n == 2 && l->one && strcmp(s, l->one) == 0) return l;
        if (n == 3 && (strcmp(s, l->t) == 0 || (l->b && strcmp(s, l->b) == 0))) return l;
        if (n > 3 && (strcasecmp(s, l->name) == 0 || (l->native && strcasecmp(s, l->native) == 0))) return l;
    }
    return NULL;
}

const char *lang_code(const char *tag) { const Lang *l = lookup(tag); return l ? l->t : NULL; }
const char *lang_name(const char *tag) { const Lang *l = lookup(tag); return l ? l->name : NULL; }

int lang_matches(const char *pref, const char *tag) {
    const Lang *a = lookup(pref), *b = lookup(tag);
    if (a && b) return a == b || strcmp(a->name, b->name) == 0;   // nor/nob are both "Norwegian"
    // Unknown codes: compare the primary subtags as written.
    if (!pref || !tag || !pref[0] || !tag[0]) return 0;
    size_t pl = strcspn(pref, "-_"), tl = strcspn(tag, "-_");
    return pl == tl && strncasecmp(pref, tag, pl) == 0;
}
