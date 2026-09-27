// Host test for audio-language normalisation (app/src/lang.c).
#include "lang.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void) {
    CHECK(strcmp(lang_code("en"), "eng") == 0);
    CHECK(strcmp(lang_code("eng"), "eng") == 0);
    CHECK(strcmp(lang_code("en-US"), "eng") == 0);
    CHECK(strcmp(lang_code("EN_gb"), "eng") == 0);
    CHECK(strcmp(lang_code("English"), "eng") == 0);
    CHECK(strcmp(lang_code("fre"), "fra") == 0);          // 639-2/B -> /T
    CHECK(strcmp(lang_code("ger"), "deu") == 0);
    CHECK(strcmp(lang_code("Deutsch"), "deu") == 0);
    CHECK(strcmp(lang_code("pt-BR"), "por") == 0);
    CHECK(strcmp(lang_code("chi"), "zho") == 0);
    CHECK(lang_code("und") == NULL && lang_code("") == NULL && lang_code(NULL) == NULL && lang_code("x") == NULL);

    CHECK(lang_matches("eng", "en"));
    CHECK(lang_matches("fra", "fre"));
    CHECK(lang_matches("fr", "French"));
    CHECK(lang_matches("ara", "ar"));
    CHECK(lang_matches("no", "nob"));                     // both Norwegian
    CHECK(!lang_matches("eng", "fra"));
    CHECK(!lang_matches("eng", ""));
    CHECK(!lang_matches("", "eng"));
    CHECK(lang_matches("qaa", "qaa-x"));                  // unknown codes compare as written
    CHECK(!lang_matches("qaa", "qab"));

    CHECK(strcmp(lang_name("spa"), "Spanish") == 0);
    CHECK(strcmp(lang_name("es-419"), "Spanish") == 0);
    CHECK(lang_name("zzz") == NULL);

    printf(failures ? "test_lang: %d FAILURES\n" : "test_lang: all ok\n", failures);
    return failures ? 1 : 0;
}
