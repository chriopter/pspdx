#ifndef PSPDX_PALETTE_H
#define PSPDX_PALETTE_H

#include "gui/gfx.h"

/* Colour arithmetic shared by the shell and the backdrop. Floats so that a
   crossfade can sit between two colours without rounding its way there. */

struct rgb { float r, g, b; };

static inline struct rgb rgb_mix(struct rgb a, struct rgb b, float t) {
    struct rgb c = { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                     a.b + (b.b - a.b) * t };
    return c;
}

static inline int rgb_clamp(float v) {
    return v < 0 ? 0 : v > 255 ? 255 : (int)v;
}

static inline unsigned rgb_pack(struct rgb c, int alpha) {
    return RGBA(rgb_clamp(c.r), rgb_clamp(c.g), rgb_clamp(c.b),
                rgb_clamp((float)alpha));
}

static const struct rgb RGB_WHITE = { 255, 255, 255 };

#endif
