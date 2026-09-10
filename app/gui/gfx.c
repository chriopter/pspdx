/*
 * The 2D render layer. Everything the shell draws goes through here: flat
 * quads, gradients, waves, soft lights, and textures out of system RAM.
 *
 * There is no depth buffer on purpose. A 2D UI never needs one, and leaving
 * it out gives back 272 KB of the 2 MB of VRAM -- enough that both display
 * buffers fit with room to spare.
 */

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspgum.h>
#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gui/gfx.h"
#include "util/runtime.h"

#define BUF_W 512                       /* draw buffer stride, must be 2^n */
#define FRAME_SIZE (BUF_W * SCR_H * 4)  /* 0x88000 */
#define GLOW_SIZE 64

static unsigned int __attribute__((aligned(16))) g_list[64 * 1024];
static unsigned g_frames;
static int g_up;
static struct gfx_texture g_glow;

/* Vertex layouts. The GE reads the components in a fixed order -- texture,
   colour, position -- so the struct members have to be declared in that
   order too. */
struct vcol  { unsigned color; short x, y, z; };
struct vtex  { short u, v; short x, y, z; };
struct vtexc { short u, v; unsigned color; short x, y, z; };

/* A batch. The backdrop asks for a few hundred lights and three dozen
   strips a frame; sent one at a time that is a texture bind, a colour and a
   draw each. Held here they leave as one sprite array and one run of
   strips, out of two allocations instead of three hundred. Only one kind is
   ever pending, so appending the other kind sends what is waiting first and
   the order the caller drew in survives. */
#define BATCH_SPRITES 320
#define BATCH_STRIPS 40
#define BATCH_STRIP_VERTS 1200

static int g_batching;
static struct vtexc *g_sprite;          /* two vertices per sprite */
static int g_sprites;
static struct vcol *g_strip;
static int g_strip_verts, g_nstrips;
static struct { short first, count; } g_strip_at[BATCH_STRIPS];

/* One soft white disc, alpha falling off with the square of the distance.
   Every light on screen is this texture, scaled and tinted. */
static void make_glow(void) {
    g_glow.w = g_glow.h = g_glow.tw = g_glow.th = GLOW_SIZE;
    g_glow.pixels = memalign(16, GLOW_SIZE * GLOW_SIZE * 4);
    if (!g_glow.pixels) return;
    unsigned *px = g_glow.pixels;
    float c = (GLOW_SIZE - 1) / 2.0f;
    for (int y = 0; y < GLOW_SIZE; y++) {
        for (int x = 0; x < GLOW_SIZE; x++) {
            float dx = (x - c) / c, dy = (y - c) / c;
            float d = sqrtf(dx * dx + dy * dy);
            float a = d >= 1.0f ? 0.0f : (1.0f - d) * (1.0f - d);
            px[y * GLOW_SIZE + x] = RGBA(255, 255, 255, (unsigned)(a * 255.0f));
        }
    }
    sceKernelDcacheWritebackRange(g_glow.pixels, GLOW_SIZE * GLOW_SIZE * 4);
}

void gfx_init(void) {
    if (!g_glow.pixels) make_glow();
    sceGuInit();
    sceGuStart(GU_DIRECT, g_list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUF_W);
    sceGuDispBuffer(SCR_W, SCR_H, (void *)FRAME_SIZE, BUF_W);
    sceGuOffset(2048 - SCR_W / 2, 2048 - SCR_H / 2);
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    g_up = 1;
    logline("gu: up, %d KB of vram left after both buffers",
            2048 - 2 * (FRAME_SIZE / 1024));
}

void gfx_shutdown(void) {
    if (!g_up) return;
    sceGuTerm();
    g_up = 0;
}

void gfx_frame_begin(unsigned clear) {
    /* Batch memory comes out of the list that is about to be reset. */
    g_batching = g_sprites = g_nstrips = g_strip_verts = 0;
    g_sprite = 0;
    g_strip = 0;
    sceGuStart(GU_DIRECT, g_list);
    sceGuClearColor(clear);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_FAST_CLEAR_BIT);
}

void gfx_frame_end(void) {
    gfx_batch_end();
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
    g_frames++;
}

unsigned gfx_frames(void) { return g_frames; }

/* Every primitive sets the state it depends on instead of trusting what the
   last caller left. intraFont in particular re-enables the depth test after
   each print, and with no depth buffer that silently drops everything drawn
   afterwards. */
static void flat_state(void) {
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
}

static void additive(void) {
    /* Added rather than composited: where two lights cross, the overlap
       gets brighter instead of just more opaque. */
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFF);
}

static void bind(const struct gfx_texture *t) {
    sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, GU_FALSE);
    sceGuTexImage(0, t->tw, t->th, t->tw, t->pixels);
    sceGuTexFunc(GU_TFX_MODULATE, t->opaque ? GU_TCC_RGB : GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
}

static void flush_sprites(void) {
    if (!g_sprites) return;
    bind(&g_glow);
    additive();
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT |
                   GU_TRANSFORM_2D, g_sprites * 2, 0, g_sprite);
    g_sprites = 0;
    g_sprite = 0;
    flat_state();
}

static void flush_strips(void) {
    if (!g_nstrips) return;
    flat_state();
    additive();
    for (int i = 0; i < g_nstrips; i++)
        sceGuDrawArray(GU_TRIANGLE_STRIP,
                       GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                       g_strip_at[i].count, 0, g_strip + g_strip_at[i].first);
    g_nstrips = 0;
    g_strip_verts = 0;
    g_strip = 0;
}

static void flush_batch(void) {
    flush_sprites();
    flush_strips();
}

void gfx_batch_begin(void) { g_batching = 1; }

void gfx_batch_end(void) {
    flush_batch();
    g_batching = 0;
}

/* Four corners as a strip: TL, BL, TR, BR. Culling is off, so winding does
   not matter. */
static void quad(int x, int y, int w, int h,
                 unsigned tl, unsigned bl, unsigned tr, unsigned br) {
    flush_batch();
    struct vcol *v = sceGuGetMemory(4 * sizeof(struct vcol));
    if (!v) return;
    v[0].color = tl; v[0].x = x;     v[0].y = y;     v[0].z = 0;
    v[1].color = bl; v[1].x = x;     v[1].y = y + h; v[1].z = 0;
    v[2].color = tr; v[2].x = x + w; v[2].y = y;     v[2].z = 0;
    v[3].color = br; v[3].x = x + w; v[3].y = y + h; v[3].z = 0;
    flat_state();
    sceGuDrawArray(GU_TRIANGLE_STRIP,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   4, 0, v);
}

void gfx_rect(int x, int y, int w, int h, unsigned color) {
    quad(x, y, w, h, color, color, color, color);
}

void gfx_vgrad(int x, int y, int w, int h, unsigned top, unsigned bottom) {
    quad(x, y, w, h, top, bottom, top, bottom);
}

void gfx_hgrad(int x, int y, int w, int h, unsigned left, unsigned right) {
    quad(x, y, w, h, left, left, right, right);
}

#define WAVE_SEGMENTS 48

void gfx_wave(float y, float amp, float thickness, float phase, unsigned color,
              unsigned crest) {
    flush_batch();
    const int n = (WAVE_SEGMENTS + 1) * 2;
    struct vcol *body = sceGuGetMemory(n * sizeof(struct vcol));
    struct vcol *line = sceGuGetMemory(n * sizeof(struct vcol));
    if (!body || !line) return;
    unsigned fade = color & 0x00FFFFFF;      /* same colour, alpha zero */
    unsigned crest_fade = crest & 0x00FFFFFF;
    for (int i = 0; i <= WAVE_SEGMENTS; i++) {
        float t = (float)i / WAVE_SEGMENTS;
        float top = y + sinf(t * 9.4248f + phase) * amp
                      + sinf(t * 4.1000f - phase * 0.7f) * (amp * 0.4f);
        short px = (short)(t * SCR_W);
        body[i * 2 + 0].color = color; body[i * 2 + 0].x = px;
        body[i * 2 + 0].y = (short)top; body[i * 2 + 0].z = 0;
        body[i * 2 + 1].color = fade;  body[i * 2 + 1].x = px;
        body[i * 2 + 1].y = (short)(top + thickness); body[i * 2 + 1].z = 0;
        line[i * 2 + 0].color = crest; line[i * 2 + 0].x = px;
        line[i * 2 + 0].y = (short)top; line[i * 2 + 0].z = 0;
        line[i * 2 + 1].color = crest_fade; line[i * 2 + 1].x = px;
        line[i * 2 + 1].y = (short)(top + 3); line[i * 2 + 1].z = 0;
    }
    flat_state();
    additive();
    sceGuDrawArray(GU_TRIANGLE_STRIP,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   n, 0, body);
    sceGuDrawArray(GU_TRIANGLE_STRIP,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   n, 0, line);
}

void gfx_ribbon(const float *x, const float *y, const unsigned *color, int n,
                float half) {
    if (n < 2) return;
    int batched = g_batching && n * 2 <= BATCH_STRIP_VERTS;
    struct vcol *v;
    if (batched) {
        flush_sprites();
        if (g_nstrips == BATCH_STRIPS || g_strip_verts + n * 2 > BATCH_STRIP_VERTS)
            flush_strips();
        if (!g_strip) {
            g_strip = sceGuGetMemory(BATCH_STRIP_VERTS * sizeof(struct vcol));
            if (!g_strip) return;
        }
        v = g_strip + g_strip_verts;
        g_strip_at[g_nstrips].first = (short)g_strip_verts;
        g_strip_at[g_nstrips].count = (short)(n * 2);
        g_nstrips++;
        g_strip_verts += n * 2;
    } else {
        flush_batch();
        v = sceGuGetMemory(n * 2 * sizeof(struct vcol));
        if (!v) return;
    }
    for (int i = 0; i < n; i++) {
        v[i * 2 + 0].color = color[i]; v[i * 2 + 0].x = (short)x[i];
        v[i * 2 + 0].y = (short)(y[i] - half); v[i * 2 + 0].z = 0;
        v[i * 2 + 1].color = color[i]; v[i * 2 + 1].x = (short)x[i];
        v[i * 2 + 1].y = (short)(y[i] + half); v[i * 2 + 1].z = 0;
    }
    if (batched) return;
    flat_state();
    additive();
    sceGuDrawArray(GU_TRIANGLE_STRIP,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   n * 2, 0, v);
}

void gfx_glow(float cx, float cy, float w, float h, unsigned color) {
    if (!g_glow.pixels) return;
    if (g_batching) {
        flush_strips();
        if (g_sprites == BATCH_SPRITES) flush_sprites();
        if (!g_sprite) {
            g_sprite = sceGuGetMemory(BATCH_SPRITES * 2 * sizeof(struct vtexc));
            if (!g_sprite) return;
        }
        /* The tint rides along per vertex so the batch needs no sceGuColor
           between sprites. The GE takes a sprite's colour from its second
           vertex; both carry it. */
        struct vtexc *b = g_sprite + g_sprites * 2;
        b[0].u = 0;         b[0].v = 0;         b[0].color = color;
        b[0].x = (short)(cx - w / 2); b[0].y = (short)(cy - h / 2); b[0].z = 0;
        b[1].u = GLOW_SIZE; b[1].v = GLOW_SIZE; b[1].color = color;
        b[1].x = (short)(cx + w / 2); b[1].y = (short)(cy + h / 2); b[1].z = 0;
        g_sprites++;
        return;
    }
    struct vtex *v = sceGuGetMemory(2 * sizeof(struct vtex));
    if (!v) return;
    bind(&g_glow);
    additive();
    sceGuColor(color);
    v[0].u = 0;         v[0].v = 0;
    v[0].x = (short)(cx - w / 2); v[0].y = (short)(cy - h / 2); v[0].z = 0;
    v[1].u = GLOW_SIZE; v[1].v = GLOW_SIZE;
    v[1].x = (short)(cx + w / 2); v[1].y = (short)(cy + h / 2); v[1].z = 0;
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   2, 0, v);
    sceGuColor(0xFFFFFFFF);
    flat_state();
}

void gfx_texture_draw(const struct gfx_texture *t, int x, int y, int w, int h,
                     unsigned tint) {
    if (!t || !t->pixels) return;
    flush_batch();
    struct vtex *v = sceGuGetMemory(2 * sizeof(struct vtex));
    if (!v) return;
    bind(t);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuColor(tint);
    v[0].u = 0;              v[0].v = 0;
    v[0].x = x;              v[0].y = y;              v[0].z = 0;
    v[1].u = (short)t->w;    v[1].v = (short)t->h;
    v[1].x = x + w;          v[1].y = y + h;          v[1].z = 0;
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   2, 0, v);
    sceGuColor(0xFFFFFFFF);
    flat_state();
}

void gfx_texture_reflect(const struct gfx_texture *t, int x, int y, int w,
                         int h, int src_h, unsigned alpha) {
    if (!t || !t->pixels || h <= 0) return;
    flush_batch();
    struct vtexc *v = sceGuGetMemory(4 * sizeof(struct vtexc));
    if (!v) return;
    /* Only the bottom src_h rows of the image are mirrored, sampled from the
       bottom up, so the reflection continues the picture's lower edge. */
    short v_bottom = (short)t->h;
    short v_top = (short)(t->h - src_h);
    unsigned top = RGBA(255, 255, 255, alpha);
    unsigned bottom = RGBA(255, 255, 255, 0);
    v[0].u = 0;            v[0].v = v_bottom; v[0].color = top;
    v[0].x = x;            v[0].y = y;        v[0].z = 0;
    v[1].u = 0;            v[1].v = v_top;    v[1].color = bottom;
    v[1].x = x;            v[1].y = y + h;    v[1].z = 0;
    v[2].u = (short)t->w;  v[2].v = v_bottom; v[2].color = top;
    v[2].x = x + w;        v[2].y = y;        v[2].z = 0;
    v[3].u = (short)t->w;  v[3].v = v_top;    v[3].color = bottom;
    v[3].x = x + w;        v[3].y = y + h;    v[3].z = 0;
    bind(t);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDrawArray(GU_TRIANGLE_STRIP,
                   GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT |
                   GU_TRANSFORM_2D, 4, 0, v);
    flat_state();
}

void gfx_shade(float cx, float cy, float w, float h, int alpha) {
    if (!g_glow.pixels) return;
    flush_batch();
    struct vtex *v = sceGuGetMemory(2 * sizeof(struct vtex));
    if (!v) return;
    bind(&g_glow);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuColor(RGBA(0, 0, 0, alpha));
    v[0].u = 0;         v[0].v = 0;
    v[0].x = (short)(cx - w / 2); v[0].y = (short)(cy - h / 2); v[0].z = 0;
    v[1].u = GLOW_SIZE; v[1].v = GLOW_SIZE;
    v[1].x = (short)(cx + w / 2); v[1].y = (short)(cy + h / 2); v[1].z = 0;
    sceGuDrawArray(GU_SPRITES,
                   GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   2, 0, v);
    sceGuColor(0xFFFFFFFF);
    flat_state();
}

/* ------------------------------------------------------------------ card */

/* The camera sits CARD_Z in front of the origin with a 45 degree view, so
   this many world units span one screen pixel. Screen coordinates convert to
   the card's plane through it, and the card ends up exactly the size asked
   for when it faces the viewer. */
#define CARD_Z 3.0f
#define CARD_FOV 45.0f
#define PX (2.0f * CARD_Z * 0.41421356f / SCR_H)   /* tan(22.5 deg) */

struct v3t { float u, v; unsigned color; float x, y, z; };
struct v3c { unsigned color; float x, y, z; };

#define FMT3T (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)
#define FMT3C (GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)

static void card_matrices(const struct gfx_card *c) {
    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(CARD_FOV, (float)SCR_W / SCR_H, 0.5f, 50.0f);
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    ScePspFVector3 pos = { (c->cx - SCR_W / 2) * PX, (SCR_H / 2 - c->cy) * PX, -CARD_Z };
    ScePspFVector3 rot = { c->pitch, c->yaw, 0.0f };
    sceGumTranslate(&pos);
    sceGumRotateXYZ(&rot);
}

/* A flat-coloured quad on the card's plane, corners given in card space. */
static void card_quad(float x0, float y0, float x1, float y1, float z,
                      unsigned tl, unsigned bl, unsigned tr, unsigned br) {
    struct v3c *v = sceGuGetMemory(4 * sizeof(struct v3c));
    if (!v) return;
    v[0].color = tl; v[0].x = x0; v[0].y = y0; v[0].z = z;
    v[1].color = bl; v[1].x = x0; v[1].y = y1; v[1].z = z;
    v[2].color = tr; v[2].x = x1; v[2].y = y0; v[2].z = z;
    v[3].color = br; v[3].x = x1; v[3].y = y1; v[3].z = z;
    sceGumDrawArray(GU_TRIANGLE_STRIP, FMT3C, 4, 0, v);
}

void gfx_plane_begin(float cx, float cy, float yaw, float pitch) {
    struct gfx_card c = { cx, cy, 0, 0, yaw, pitch, 255, -1.0f, 0 };
    card_matrices(&c);
    flat_state();
}

void gfx_plane_quad(float x, float y, float w, float h, float z, unsigned color) {
    card_quad(x * PX, -y * PX, (x + w) * PX, -(y + h) * PX, z * PX,
              color, color, color, color);
}

void gfx_plane_end(void) {
    flat_state();
}

void gfx_card_draw(const struct gfx_texture *t, const struct gfx_card *c) {
    float hw = c->w * PX / 2, hh = c->h * PX / 2;

    /* The shadow is flat, under the card, offset the way the card leans. */
    gfx_shade(c->cx + c->yaw * 40.0f, c->cy + 10.0f - c->pitch * 40.0f,
              c->w + 60.0f, c->h + 60.0f, 150);

    card_matrices(c);
    flat_state();

    /* Frame: a hair wider than the picture, black. */
    float f = 1.5f * PX;
    card_quad(-hw - f, hh + f, hw + f, -hh - f, -0.002f,
              RGBA(0, 0, 0, 200), RGBA(0, 0, 0, 200), RGBA(0, 0, 0, 200), RGBA(0, 0, 0, 200));

    if (t && t->pixels) {
        float u1 = (float)t->w / t->tw, v1 = (float)t->h / t->th;
        unsigned white = RGBA(255, 255, 255, c->alpha);
        bind(t);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);

        /* Reflection first, so the card lies over its top edge. Mirrored in
           v, fading to nothing over reflect_h pixels. */
        if (c->reflect_h > 0) {
            float rh = c->reflect_h * PX;
            float rv = v1 * (float)c->reflect_h / c->h;
            unsigned top = RGBA(255, 255, 255, c->alpha * 30 / 100);
            unsigned bottom = RGBA(255, 255, 255, 0);
            struct v3t *r = sceGuGetMemory(4 * sizeof(struct v3t));
            if (r) {
                r[0].u = 0;  r[0].v = v1;      r[0].color = top;    r[0].x = -hw; r[0].y = -hh - f;      r[0].z = 0;
                r[1].u = 0;  r[1].v = v1 - rv; r[1].color = bottom; r[1].x = -hw; r[1].y = -hh - f - rh; r[1].z = 0;
                r[2].u = u1; r[2].v = v1;      r[2].color = top;    r[2].x = hw;  r[2].y = -hh - f;      r[2].z = 0;
                r[3].u = u1; r[3].v = v1 - rv; r[3].color = bottom; r[3].x = hw;  r[3].y = -hh - f - rh; r[3].z = 0;
                sceGumDrawArray(GU_TRIANGLE_STRIP, FMT3T, 4, 0, r);
            }
        }

        struct v3t *v = sceGuGetMemory(4 * sizeof(struct v3t));
        if (v) {
            v[0].u = 0;  v[0].v = 0;  v[0].color = white; v[0].x = -hw; v[0].y = hh;  v[0].z = 0;
            v[1].u = 0;  v[1].v = v1; v[1].color = white; v[1].x = -hw; v[1].y = -hh; v[1].z = 0;
            v[2].u = u1; v[2].v = 0;  v[2].color = white; v[2].x = hw;  v[2].y = hh;  v[2].z = 0;
            v[3].u = u1; v[3].v = v1; v[3].color = white; v[3].x = hw;  v[3].y = -hh; v[3].z = 0;
            sceGumDrawArray(GU_TRIANGLE_STRIP, FMT3T, 4, 0, v);
        }
        flat_state();
    } else {
        card_quad(-hw, hh, hw, -hh, 0.0f, RGBA(255, 255, 255, 10), RGBA(255, 255, 255, 3),
                  RGBA(255, 255, 255, 10), RGBA(255, 255, 255, 3));
    }

    /* Glass: a hairline of light along the top edge, and the sweep -- a
       soft diagonal band of light crossing the picture, added on. */
    card_quad(-hw, hh + f, hw, hh - 1.0f * PX, 0.001f,
              RGBA(255, 255, 255, 40), RGBA(255, 255, 255, 40),
              RGBA(255, 255, 255, 130), RGBA(255, 255, 255, 130));
    if (c->gloss >= 0.0f && c->gloss <= 1.0f) {
        additive();
        float band = hw * 0.55f;
        float x = -hw - band + c->gloss * (2 * hw + 2 * band);
        float lean = hh * 0.6f;
        unsigned clear = RGBA(255, 255, 255, 0), lit = RGBA(255, 255, 255, 70);
        struct v3c *g = sceGuGetMemory(6 * sizeof(struct v3c));
        if (g) {
            g[0].color = clear; g[0].x = x - band + lean; g[0].y = hh;  g[0].z = 0.002f;
            g[1].color = clear; g[1].x = x - band - lean; g[1].y = -hh; g[1].z = 0.002f;
            g[2].color = lit;   g[2].x = x + lean;        g[2].y = hh;  g[2].z = 0.002f;
            g[3].color = lit;   g[3].x = x - lean;        g[3].y = -hh; g[3].z = 0.002f;
            g[4].color = clear; g[4].x = x + band + lean; g[4].y = hh;  g[4].z = 0.002f;
            g[5].color = clear; g[5].x = x + band - lean; g[5].y = -hh; g[5].z = 0.002f;
            sceGumDrawArray(GU_TRIANGLE_STRIP, FMT3C, 6, 0, g);
        }
        flat_state();
    }
}

void gfx_texture_free(struct gfx_texture *t) {
    if (!t) return;
    free(t->pixels);
    memset(t, 0, sizeof(*t));
}

/* Where the front buffer's pixels come from depends on who drew them. The
   debug screen writes VRAM with the CPU and the CPU can read it straight
   back. The GE's output is different: on PPSSPP the emulated VRAM behind a
   GE-rendered frame is not kept current, and a CPU read hands back whatever
   was last written there -- frames old. Copying through the GE itself is
   what games do for their save icons, and it is the path the emulator keeps
   honest. */
static unsigned __attribute__((aligned(16))) g_readback[SCR_W * SCR_H];

static const unsigned *front_pixels(int *stride) {
    void *top = 0;
    int format = 0;
    *stride = BUF_W;
    sceDisplayWaitVblankStart();
    if (sceDisplayGetFrameBuf(&top, stride, &format,
                              PSP_DISPLAY_SETBUF_IMMEDIATE) < 0 || !top) {
        logline("screenshot: no framebuffer");
        return 0;
    }
    if (format != PSP_DISPLAY_PIXEL_FORMAT_8888) {
        logline("screenshot: pixel format %d not 8888", format);
        return 0;
    }
    if (!g_up)
        return (const unsigned *)((unsigned)top | 0x40000000);

    void *src = (void *)((unsigned)top & 0x1FFFFFFF);
    sceGuStart(GU_DIRECT, g_list);
    sceGuCopyImage(GU_PSM_8888, 0, 0, SCR_W, SCR_H, *stride, src,
                   0, 0, SCR_W, g_readback);
    sceGuTexSync();
    sceGuFinish();
    sceGuSync(0, 0);
    *stride = SCR_W;
    return (const unsigned *)((unsigned)g_readback | 0x40000000);
}

void gfx_screenshot(const char *path) {
    enum { W = SCR_W, H = SCR_H };
    int stride;
    const unsigned *pixels = front_pixels(&stride);
    if (!pixels) return;

    int fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    unsigned rowbytes = W * 3;
    unsigned datasize = rowbytes * H;
    unsigned char hdr[54] = { 'B', 'M' };
    unsigned value;
    value = 54 + datasize; memcpy(hdr + 2, &value, 4);
    value = 54;            memcpy(hdr + 10, &value, 4);
    value = 40;            memcpy(hdr + 14, &value, 4);
    value = W;             memcpy(hdr + 18, &value, 4);
    value = H;             memcpy(hdr + 22, &value, 4);
    hdr[26] = 1; hdr[28] = 24;
    value = datasize;      memcpy(hdr + 34, &value, 4);
    sceIoWrite(fd, hdr, sizeof(hdr));
    static unsigned char row[W * 3];
    for (int y = H - 1; y >= 0; y--) {
        const unsigned *src = pixels + y * stride;
        for (int x = 0; x < W; x++) {
            unsigned px = src[x];
            row[x * 3 + 0] = (px >> 16) & 0xff;
            row[x * 3 + 1] = (px >> 8) & 0xff;
            row[x * 3 + 2] = px & 0xff;
        }
        sceIoWrite(fd, row, sizeof(row));
    }
    sceIoClose(fd);
}
