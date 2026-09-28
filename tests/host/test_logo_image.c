// Host test for channel-logo decoding (app/src/logo_image.c): PNG with alpha,
// baseline and progressive JPEG, GIF and BMP are decoded and shrunk to fit
// 160x96 (never enlarged), colours survive the box filter, and a cut-short
// file, an oversized image and an SVG are refused.
#include "logo_image.h"
#include "logo_vectors.h"

#include <stdio.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static int near(uint32_t px, uint32_t a, uint32_t r, uint32_t g, uint32_t b, int tol) {
    int d[4] = { (int)(px >> 24) - (int)a, (int)(px >> 16 & 255) - (int)r, (int)(px >> 8 & 255) - (int)g, (int)(px & 255) - (int)b };
    for (int i = 0; i < 4; i++) if (d[i] > tol || d[i] < -tol) return 0;
    return 1;
}

#define DECODE(arr, W, H) logo_image_decode(arr, (int)sizeof(arr), LOGO_MAX_W, LOGO_MAX_H, &W, &H)

int main(void) {
    int w, h;
    uint32_t *p;

    p = DECODE(img_png_rgba, w, h);                  // 320x120 -> 160x60
    CHECK(p && w == 160 && h == 60);
    if (p) {
        CHECK(near(p[10 * w + 5], 255, 255, 0, 0, 0));      // opaque red half
        CHECK(near(p[10 * w + 150], 0, 0, 0, 0, 0));        // transparent half
        free(p);
    }
    p = DECODE(img_jpeg, w, h);                      // 400x400 -> 96x96
    CHECK(p && w == 96 && h == 96);
    if (p) {
        CHECK(near(p[10 * w + 10], 255, 255, 0, 0, 12));
        CHECK(near(p[10 * w + 85], 255, 0, 255, 0, 12));
        CHECK(near(p[85 * w + 10], 255, 0, 0, 255, 12));
        CHECK(near(p[85 * w + 85], 255, 255, 255, 255, 12));
        free(p);
    }
    p = DECODE(img_jpeg_prog, w, h);
    CHECK(p && w == 96 && h == 96 && near(p[85 * w + 10], 255, 0, 0, 255, 12));
    free(p);
    p = DECODE(img_gif, w, h);                       // small: kept at 40x20
    CHECK(p && w == 40 && h == 20 && near(p[5 * w + 5], 255, 0, 200, 0, 0));
    free(p);
    p = DECODE(img_bmp, w, h);
    CHECK(p && w == 30 && h == 10 && near(p[0], 255, 0, 0, 255, 0));
    free(p);

    CHECK(DECODE(img_png_cut, w, h) == NULL);
    CHECK(DECODE(img_png_huge, w, h) == NULL);
    CHECK(DECODE(img_svg, w, h) == NULL);
    CHECK(logo_image_decode(img_png_rgba, 0, 160, 96, &w, &h) == NULL);

    if (failures) { printf("test_logo_image: %d failure(s)\n", failures); return 1; }
    printf("test_logo_image: all ok\n");
    return 0;
}
