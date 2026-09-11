#include <pspkernel.h>
#include <malloc.h>
#include <math.h>
#include <string.h>

#include "gui/title.h"
#include "gui/font.h"
#include "gui/gfx.h"

#define TARGET_W 512            /* the texture */
#define BAKE_W 480              /* what fits in the draw buffer */
#define TARGET_H 64

static char g_text[32];
static struct gfx_texture g_tex;
static float g_width;
static int g_rendered;

/* Bake the word: a dark copy a pixel down for a bevel, then the face. */
static void bake(const char *text, struct rgb tint) {
    if (!g_tex.pixels) {
        g_tex.tw = TARGET_W; g_tex.th = TARGET_H;
        g_tex.pixels = memalign(16, TARGET_W * TARGET_H * 4);
        if (!g_tex.pixels) return;
    }
    if (gfx_bake_begin(BAKE_W, TARGET_H) != 0) return;
    g_width = font_width(FONT_DISPLAY, text);
    float x = (BAKE_W - g_width) / 2, y = 46;
    unsigned face = rgb_pack(rgb_mix(RGB_WHITE, tint, 0.18f), 255);
    unsigned dark = rgb_pack(rgb_mix(tint, RGB_WHITE, 0.1f), 255);
    font_print(FONT_DISPLAY, x, y + 2, dark, text);
    font_print(FONT_DISPLAY, x, y, face, text);
    gfx_bake_end(&g_tex);
    /* The GE leaves alpha at zero in what it blended, so the letters' own
       brightness becomes their coverage: they were drawn on black, and
       nothing else is in the corner. Once per word, thirty thousand pixels. */
    unsigned *px = (unsigned *)((unsigned)g_tex.pixels | 0x40000000);
    for (int i = 0; i < TARGET_W * TARGET_H; i++) {
        unsigned c = px[i];
        unsigned r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
        unsigned a = r > g ? (r > b ? r : b) : (g > b ? g : b);
        px[i] = (c & 0x00FFFFFF) | (a << 24);
    }
    g_rendered = 1;
}

void title_prepare(const char *text, struct rgb tint) {
    if (g_rendered && strcmp(g_text, text) == 0) return;
    strncpy(g_text, text, sizeof(g_text) - 1);
    bake(text, tint);
}

void title_draw(float cx, float cy, float t, struct rgb tint) {
    if (!g_rendered) return;
    struct gfx_card card;
    /* Fixed in place: a word that rocks looks cheap. The light moves. */
    card.cx = cx;
    card.cy = cy;
    card.w = BAKE_W;
    card.h = TARGET_H;
    card.yaw = 0.0f;
    card.pitch = 0.0f;
    card.alpha = 255;
    card.reflect_h = 26;
    card.bare = 1;
    float cycle = fmodf(t, 5.0f);
    card.gloss = cycle < 1.4f ? cycle / 1.4f : -1.0f;
    gfx_glow(card.cx, card.cy, g_width + 160, 120, rgb_pack(tint, 110));
    gfx_card_draw(&g_tex, &card);
}
