// lang.h — language tags on audio tracks ("en", "eng", "en-US", "English",
// "fre" vs "fra") normalised for matching the user's preferred language and
// for readable track labels. Pure; tests/host/test_lang.c.
#ifndef PS4CAST_LANG_H
#define PS4CAST_LANG_H

// ISO 639-2/T code ("eng", "fra", "deu", ...) for a tag, or NULL if unknown.
const char *lang_code(const char *tag);
// 1 if `tag` names the language `pref` names (either may be any known form).
int lang_matches(const char *pref, const char *tag);
// English display name ("French"), or NULL if unknown.
const char *lang_name(const char *tag);

#endif
