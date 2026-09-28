// logo_image.h — decode a downloaded channel logo into a small ARGB picture.
// Pure C (stb_image inside), so it runs in host tests.
#ifndef PS4CAST_LOGO_IMAGE_H
#define PS4CAST_LOGO_IMAGE_H

#include <stdint.h>

#define LOGO_MAX_W 160
#define LOGO_MAX_H 96
#define LOGO_MAX_SRC 2048   // larger images are refused before decoding

// PNG, JPEG (baseline and progressive), GIF (first frame) or BMP -> malloc'd
// 0xAARRGGBB pixels (straight alpha), shrunk to fit maxW x maxH keeping the
// aspect ratio (never enlarged). NULL if it is not such an image.
uint32_t *logo_image_decode(const uint8_t *data, int len, int maxW, int maxH, int *w, int *h);

#endif
