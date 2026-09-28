// gfx.h — minimal double-buffered framebuffer over sceVideoOut, plus 8x8 text.
// Trimmed from the OpenOrbis _common/graphics sample (proven init/flip sequence),
// rewritten in C and without the FreeType dependency.
#ifndef PS4CAST_GFX_H
#define PS4CAST_GFX_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#define GFX_BUFFER_COUNT 3

typedef struct {
    uint8_t r, g, b;
} GfxColor;

typedef struct {
    int width;
    int height;
    int depth;          // bytes per pixel (4)
    int video;          // sceVideoOut handle
    int activeIdx;      // current render target (cycles over the buffers)
    int lastSubmitted[GFX_BUFFER_COUNT]; // frame id last scanned from each surface
    int frameBufferSize;
    off_t directMemOff;
    size_t directMemSize;
    uintptr_t videoMemSP;
    void *videoMem;
    void *frameBuffers[GFX_BUFFER_COUNT]; // pipeline CPU conversion with scanout
    void *flipQueue;    // OrbisKernelEqueue (pointer-sized opaque handle)
    char attr[64];      // OrbisVideoOutBufferAttribute storage (over-sized, safe)
    uint64_t tag[GFX_BUFFER_COUNT]; // what each surface holds (see gfx_video); 0 = unknown
    int shownIdx;       // surface of the most recent flip (-1 before the first)
    int lastFlipId;     // frame id of the most recent flip
} Gfx;

// Lifecycle
int  gfx_init(Gfx *g, int width, int height);   // returns 0 on success
void gfx_present(Gfx *g, int frameID);          // submit flip + wait + swap
void gfx_present_stats(uint64_t *avg_us, uint64_t *max_us,
                       uint64_t *wait_avg_us, uint64_t *wait_max_us);
void gfx_present_stats_reset(void);
// Best-effort release of the display/GPU context before a fatal _exit, so the
// kernel can reclaim it and the process doesn't become unkillable. Idempotent.
void gfx_emergency_release(void);

// Drawing (operate on the active back buffer)
void gfx_clear(Gfx *g, GfxColor c);
void gfx_pixel(Gfx *g, int x, int y, GfxColor c);
void gfx_rect(Gfx *g, int x, int y, int w, int h, GfxColor c);

// Alpha-composited drawing for a modern, smooth look. `a` is 0..255 coverage
// blended over whatever is already in the buffer (read-modify-write), which is
// what gives anti-aliased edges and translucent panels.
void gfx_blend(Gfx *g, int x, int y, GfxColor c, int a);
void gfx_rect_a(Gfx *g, int x, int y, int w, int h, GfxColor c, int a);     // translucent fill
void gfx_circle(Gfx *g, int cx, int cy, int r, GfxColor c);                 // AA filled disc
void gfx_circle_a(Gfx *g, int cx, int cy, int r, GfxColor c, int a);        // AA filled disc, translucent
void gfx_round(Gfx *g, int x, int y, int w, int h, int r, GfxColor c);      // AA rounded-rect fill
void gfx_round_a(Gfx *g, int x, int y, int w, int h, int r, GfxColor c, int a);
void gfx_vgrad(Gfx *g, int x, int y, int w, int h, GfxColor top, GfxColor bot); // vertical gradient
void gfx_tri(Gfx *g, int x0, int y0, int x1, int y1, int x2, int y2, GfxColor c); // filled triangle
void gfx_arc(Gfx *g, int cx, int cy, int r, int thick, int quad, GfxColor c);  // AA quarter-arc ring

// Text with letter-spacing (tracking, in pixels) for tidier headings.
int  gfx_text_tr(Gfx *g, int x, int y, const char *s, int scale, GfxColor c, int track);
int  gfx_text_tr_w(const char *s, int scale, int track);

// ARGB image (alpha in the top byte) scaled into the w x h rectangle at x,y
// and blended. `id` must change whenever the pixels behind `argb` do (it is
// part of the frame's reuse tag), and `argb` must stay valid until the next
// gfx_present -- drawing is queued until then.
void gfx_image(Gfx *g, int x, int y, int w, int h, const uint32_t *argb, int sw, int sh, uint32_t id);

// Text using the embedded 8x8 font. `scale` enlarges each glyph pixel into a
// scale*scale block. Returns the x advance in pixels.
int  gfx_text(Gfx *g, int x, int y, const char *s, int scale, GfxColor c);
int  gfx_text_w(const char *s, int scale);      // measured pixel width

// ---- redraw skipping (PS4 build; the host preview draws immediately) -------
// Drawing calls between two gfx_present calls are queued, not drawn. At present
// the queued frame is summarised as a tag: the picture it starts from (a video
// tag, a full-screen fill, or what the target surface already held) hashed with
// every queued call and its arguments/text. Each surface remembers the tag of
// what was last drawn into it, so a frame identical to one a surface already
// holds costs no pixel work: the target is flipped as-is, or the on-screen
// surface is flipped again. Otherwise the queue runs into the target as usual.
// A frame with nothing drawn keeps the on-screen picture. Video tags have bit 63
// set; their owner guarantees one tag never names two different pictures.
#define GFX_TAG_VIDEO (1ull << 63)
// Use `paint` to put a full-screen video picture named `tag` under this frame's
// drawing. It runs from gfx_present (or at once when drawing is already under
// way), only if no surface already holds the result; it writes the target
// directly and returns 0, or nonzero if it painted nothing.
void     gfx_video(Gfx *g, uint64_t tag, int (*paint)(Gfx *g));
uint64_t gfx_tag(const Gfx *g);                 // what the target surface holds now
void     gfx_reuse_stats(uint64_t *reused, uint64_t *reshown);

#endif
