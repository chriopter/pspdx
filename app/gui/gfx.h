#ifndef PSPDX_GFX_H
#define PSPDX_GFX_H

#include <stddef.h>

/* The hardware surface. The draw buffer is 512 wide because the GE wants a
   power of two; only the left 480 are shown. */
#define SCR_W 480
#define SCR_H 272

/* PSP colours are ABGR little-endian, which is why every literal in this
   codebase looks byte-swapped. */
#define RGBA(r, g, b, a) \
    ((unsigned)(((a) << 24) | ((b) << 16) | ((g) << 8) | (r)))
#define RGB(r, g, b) RGBA(r, g, b, 0xFF)

void gfx_init(void);
void gfx_shutdown(void);

/* One frame: begin, draw, end. end() syncs, waits for vblank and swaps, so it
   paces the caller at 60 Hz. */
void gfx_frame_begin(unsigned clear);
void gfx_frame_end(void);
unsigned gfx_frames(void);

void gfx_rect(int x, int y, int w, int h, unsigned color);
void gfx_vgrad(int x, int y, int w, int h, unsigned top, unsigned bottom);
void gfx_hgrad(int x, int y, int w, int h, unsigned left, unsigned right);

/* A ribbon of the kind XMB has: one sine period and a bit, solid along the
   crest and transparent at the bottom edge, added onto what is behind it.
   crest is a thin brighter line along the top edge -- the glassy highlight. */
void gfx_wave(float y, float amp, float thickness, float phase, unsigned color,
              unsigned crest);

/* A band through n points, half pixels above and below each, one colour per
   point so a line can fade with depth along its own length. Added onto what
   is behind it. */
void gfx_ribbon(const float *x, const float *y, const unsigned *color, int n,
                float half);

/* A surface: n vertices in triangle-strip order, each with its own place, its
   own colour, and where it sits in gfx's tiling ripple -- fine bands of light
   that give the surface detail between vertices that are far apart. Alternate
   the two edges -- near, far, near, far -- and a row of quads comes out lit
   corner by corner. Added onto what is behind it; u and v are in texels, so
   64 is one tile. */
void gfx_ripple_strip(const float *x, const float *y, const short *u,
                      const short *v, const unsigned *color, int n);

/* A soft radial light, added onto what is behind it. The alpha in color is
   how strong; the rgb is what it tints toward. Cheap enough to draw dozens
   of per frame. */
void gfx_glow(float cx, float cy, float w, float h, unsigned color);

/* Between these two, glows and ribbons pile up and go out as a handful of
   draws instead of one each -- one texture bind for every light on screen
   rather than hundreds. Every other gfx_ primitive empties what is pending
   first, so the order things were asked for is the order they land in.
   Nothing that draws behind gfx_'s back -- intraFont above all -- may run
   inside a batch, or it would end up under what was asked for before it. */
void gfx_batch_begin(void);
void gfx_batch_end(void);

/* Pixels live in system RAM and are read by the GE directly, so they must be
   16-byte aligned and written back out of the cache before use. w/h are the
   used area inside the power-of-two tw/th. */
struct gfx_texture {
    int w, h, tw, th;
    void *pixels;
    /* The alpha channel means nothing: take colour only. sceMpeg writes
       every pixel with alpha zero, as the PSP does. */
    int opaque;
};

void gfx_texture_draw(const struct gfx_texture *t, int x, int y, int w, int h,
                     unsigned tint);

/* The same image upside down under y, fading from alpha at the top edge to
   nothing over h pixels: a reflection in a dark floor. */
void gfx_texture_reflect(const struct gfx_texture *t, int x, int y, int w,
                         int h, int src_h, unsigned alpha);
void gfx_texture_free(struct gfx_texture *t);

/* A card in space: a picture on a plane that can turn a little toward or
   away from the viewer, drawn in real perspective. Everything else on screen
   is flat; this is the one thing that is not, on purpose. */
struct gfx_card {
    float cx, cy;           /* screen centre when facing the viewer */
    float w, h;             /* screen size when facing the viewer */
    float yaw, pitch;       /* radians; small */
    int alpha;              /* 0..255 for the picture */
    float gloss;            /* 0..1 where the light sweep is, outside = none */
    int reflect_h;          /* pixels of reflection below, 0 = none */
};

/* t may be NULL: then only the frame, its shadow and its gloss are drawn,
   which is what the card looks like while its picture is on its way. */
void gfx_card_draw(const struct gfx_texture *t, const struct gfx_card *c);

/* A plane in space to draw flat things on, like the card but for anyone:
   begin sets it up at a screen position with a lean, quads are placed in
   screen pixels relative to its centre (y down, z toward the viewer), end
   returns to flat drawing. */
void gfx_plane_begin(float cx, float cy, float yaw, float pitch);
void gfx_plane_quad(float x, float y, float w, float h, float z, unsigned color);
void gfx_plane_end(void);

/* A soft dark spot, composited rather than added: a shadow. */
void gfx_shade(float cx, float cy, float w, float h, int alpha);

/* Reads back whatever is on screen right now, GU or debug screen: it asks the
   display which buffer is front rather than assuming the start of VRAM. */
void gfx_screenshot(const char *path);

#endif
