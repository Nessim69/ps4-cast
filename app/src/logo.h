// logo.h — channel logos (M3U tvg-logo) on the TV.
//
// Drawing asks for a channel's logo; the first ask queues a download on a
// worker thread (its own aseg channel, 1 MB and 8 s at most), which decodes
// PNG/JPEG/GIF/BMP with stb_image and shrinks it to at most 160x96. Decoded
// logos are adopted by the main thread in logo_tick(), the only place a
// logo's pixels are ever freed: gfx queues image pointers until the frame is
// presented, so nothing drawn this frame may go away before then.
#ifndef PS4CAST_LOGO_H
#define PS4CAST_LOGO_H

#include "gfx.h"

void logo_init(void);          // once, before logo_draw; starts the worker
void logo_tick(void);          // main thread, each frame before drawing
// Draw channel `chan`'s logo fitted and centred in (x, y, w, h); 1 if drawn.
int  logo_draw(Gfx *g, int chan, int x, int y, int w, int h);

#endif
