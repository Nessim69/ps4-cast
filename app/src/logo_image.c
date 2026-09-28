// logo_image.c — channel logo decoding (stb_image) and shrinking. See logo_image.h.
#include "logo_image.h"

#include <stdlib.h>
#include <string.h>

// stb_image, vendored unmodified (v2.30, public domain / MIT): only the
// formats logos come in, memory input only, and no asserts that could abort
// on a hostile file. It is third-party code, so our warning set stays off it.
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS LOGO_MAX_SRC
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

uint32_t *logo_image_decode(const uint8_t *data, int len, int maxW, int maxH, int *ow, int *oh) {
    int w, h, n;
    if (!data || len <= 0 || maxW <= 0 || maxH <= 0) return NULL;
    if (!stbi_info_from_memory(data, len, &w, &h, &n)) return NULL;
    if (w <= 0 || h <= 0 || w > LOGO_MAX_SRC || h > LOGO_MAX_SRC) return NULL;
    unsigned char *rgba = stbi_load_from_memory(data, len, &w, &h, &n, 4);
    if (!rgba) return NULL;
    int dw = w, dh = h;
    if (dw > maxW) { dh = (int)((int64_t)dh * maxW / dw); dw = maxW; }
    if (dh > maxH) { dw = (int)((int64_t)dw * maxH / dh); dh = maxH; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    uint32_t *out = malloc(sizeof(uint32_t) * (size_t)dw * (size_t)dh);
    if (!out) { stbi_image_free(rgba); return NULL; }
    // Box filter: each output pixel averages its block of source pixels,
    // colours weighted by alpha so transparent edges don't darken.
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((int64_t)y * h / dh), y1 = (int)((int64_t)(y + 1) * h / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((int64_t)x * w / dw), x1 = (int)((int64_t)(x + 1) * w / dw);
            if (x1 <= x0) x1 = x0 + 1;
            uint64_t sa = 0, sr = 0, sg = 0, sb = 0, cnt = 0;
            for (int yy = y0; yy < y1; yy++) {
                const unsigned char *p = rgba + ((size_t)yy * (size_t)w + (size_t)x0) * 4;
                for (int xx = x0; xx < x1; xx++, p += 4) {
                    uint32_t a = p[3];
                    sa += a; sr += (uint64_t)p[0] * a; sg += (uint64_t)p[1] * a; sb += (uint64_t)p[2] * a;
                    cnt++;
                }
            }
            uint32_t a = (uint32_t)(sa / cnt), r = 0, g = 0, b = 0;
            if (sa) { r = (uint32_t)(sr / sa); g = (uint32_t)(sg / sa); b = (uint32_t)(sb / sa); }
            out[(size_t)y * (size_t)dw + (size_t)x] = a << 24 | r << 16 | g << 8 | b;
        }
    }
    stbi_image_free(rgba);
    *ow = dw;
    *oh = dh;
    return out;
}
