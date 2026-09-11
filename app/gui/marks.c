/*
 * Drawing a mark. Every glyph goes through the same three steps, so the set
 * reads as one family whatever it is spelling: a wide soft light behind it
 * when it is the one being looked at, a dark copy of itself one pixel down
 * and right, then the glyph in white.
 *
 * The dark copy is the whole trick, and it is the system's own: the XMB ships
 * a blurred shadow texture beside every foreground glyph it has, because a
 * white shape on a photograph loses its edge wherever the photograph is pale.
 * A real blur costs a texture per glyph; the same bitmap offset by one at
 * alpha 150 costs nothing and does the same job at eleven pixels.
 *
 * The highlight is the second thing taken from the originals: the lit state
 * is not a brighter glyph, it is the same glyph with more light behind it.
 * Brightening the strokes would make a selected mark read as a different,
 * bolder mark; putting the light behind leaves the shape alone.
 */

#include <math.h>
#include <string.h>

#include "gui/gfx.h"
#include "gui/marks.h"
#include "gui/marks_data.h"

/* The order of the table is the order of the enum and nothing enforces that
   but the list at the top of tools/marks/embed.py. The count at least can be
   checked, and a negative array size is how C says no at compile time: if
   this line is what failed, a mark was added to one list and not the other. */
typedef char mark_table_matches_enum[
    (int)(sizeof(mark_glyphs) / sizeof(mark_glyphs[0])) == MARK_COUNT ? 1 : -1];

static const struct mark_glyph *glyph(enum mark m) {
    if (m < 0 || m >= MARK_COUNT) return &mark_glyphs[MARK_CROSS];
    return &mark_glyphs[m];
}

int mark_width(enum mark m) { return glyph(m)->w; }
int mark_height(enum mark m) { return glyph(m)->h; }

/* One pass over a bitmap, as few quads as it can be said in.
 *
 * gfx_rect is a quad and a quad breaks whatever glow batch is open, so a
 * glyph drawn a pixel at a time would cost two hundred draws and the shell
 * asks for fifteen of these a frame. Instead each run of equal alpha along a
 * row is one quad, grown downwards as far as the same run repeats. No glyph
 * in the set costs more than thirty-four quads that way, so the worst mark
 * on screen is sixty-eight draws with its shadow, and most are half that. */
static void blit(const unsigned char *alpha, int w, int h, int x0, int y0,
                 unsigned color, int scale) {
    unsigned char done[MARK_MAX_W * MARK_MAX_H];
    memset(done, 0, (size_t)(w * h));
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int at = y * w + x;
            if (done[at] || !alpha[at]) continue;
            unsigned char v = alpha[at];
            int run = 1;
            while (x + run < w && !done[at + run] && alpha[at + run] == v) run++;
            int rows = 1;
            while (y + rows < h) {
                const unsigned char *a = alpha + (y + rows) * w + x;
                const unsigned char *d = done + (y + rows) * w + x;
                int k = 0;
                while (k < run && a[k] == v && !d[k]) k++;
                if (k < run) break;
                rows++;
            }
            for (int ry = 0; ry < rows; ry++)
                memset(done + (y + ry) * w + x, 1, (size_t)run);
            int a = v * scale / 255;
            if (a <= 0) continue;
            gfx_rect(x0 + x, y0 + y, run, rows,
                     (color & 0x00FFFFFFu) | ((unsigned)a << 24));
        }
    }
}

void mark_draw(enum mark m, float cx, float cy, unsigned color, int state,
               unsigned tint, float t) {
    const struct mark_glyph *g = glyph(m);
    int w = g->w, h = g->h;
    /* Centred on whole pixels: a bitmap landing on a half pixel would be
       resampled by the GE and every two-pixel stroke would go soft. */
    int x0 = (int)(cx - w / 2.0f + 0.5f);
    int y0 = (int)(cy - h / 2.0f + 0.5f);
    int scale = (int)(color >> 24);
    if (scale <= 0) return;

    if (state == MARK_LIT) {
        /* Three times the glyph across, so the light is a halo around it and
           not a lamp inside it, and breathing just enough to be noticed only
           when the mark is looked at. */
        float pulse = 0.88f + 0.12f * sinf(t * 2.2f);
        int a = (int)(110.0f * pulse * scale / 255.0f);
        gfx_glow(cx, cy, w * 3.0f, h * 3.0f,
                 (tint & 0x00FFFFFFu) | ((unsigned)a << 24));
    }
    if (state != MARK_DIM)
        blit(g->alpha, w, h, x0 + 1, y0 + 1, 0, 150 * scale / 255);
    blit(g->alpha, w, h, x0, y0, color, scale);
}
