#include <string.h>

#include "gui/letters.h"
#include "gui/gfx.h"

/* Seven rows of five bits, top row first, high bit on the left. */
struct glyph { char c; unsigned char rows[7]; };

static const struct glyph GLYPHS[] = {
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
    { 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } },
    { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } },
    { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } },
    { 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } },
    { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
    { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } },
    { 'X', { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 } },
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04 } },
};

static const unsigned char *rows_of(char c) {
    for (unsigned i = 0; i < sizeof(GLYPHS) / sizeof(*GLYPHS); i++)
        if (GLYPHS[i].c == c) return GLYPHS[i].rows;
    return 0;
}

float letters_width(const char *text, float cell) {
    size_t n = strlen(text);
    return n ? (n * 6 - 1) * cell : 0.0f;
}

static unsigned darker(unsigned color, int by) {
    unsigned r = (color & 0xFF) * (256 - by) >> 8;
    unsigned g = ((color >> 8) & 0xFF) * (256 - by) >> 8;
    unsigned b = ((color >> 16) & 0xFF) * (256 - by) >> 8;
    return (color & 0xFF000000u) | (b << 16) | (g << 8) | r;
}

void letters_draw(const char *text, float cx, float cy, float cell,
                  float yaw, float pitch, unsigned color, int depth) {
    float width = letters_width(text, cell);
    float left = -width / 2, top = -3.5f * cell;
    gfx_plane_begin(cx, cy, yaw, pitch);
    /* Back to front: the extrusion first, each layer a little darker and a
       little further back, then the face on top. */
    for (int layer = depth; layer >= 0; layer--) {
        unsigned c = layer ? darker(color, 60 + layer * (140 / (depth + 1))) : color;
        float z = -layer * cell * 0.6f;
        float x = left;
        for (const char *p = text; *p; p++, x += 6 * cell) {
            const unsigned char *rows = rows_of(*p);
            if (!rows) continue;
            for (int r = 0; r < 7; r++)
                for (int b = 0; b < 5; b++)
                    if (rows[r] & (0x10 >> b))
                        gfx_plane_quad(x + b * cell, top + r * cell, cell, cell, z, c);
        }
    }
    gfx_plane_end();
}
