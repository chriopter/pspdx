#ifndef PSPDX_TITLE_H
#define PSPDX_TITLE_H

#include "gui/palette.h"

/* A word standing in the room, the way a console of 2004 would set it: the
   system font at display size rendered once into a texture, a highlight
   across its upper half, then the texture on a card in perspective with a
   sweep of light through the letters and a reflection under them. Not the
   block cells of the floor -- those are for the floor. */

/* Bakes the texture for text if it is not the one baked last; call between
   frames, before the frame that will draw it. */
void title_prepare(const char *text, struct rgb tint);

/* Draws the baked text centred on (cx, cy), inside a frame. t is the clock
   for the drift and the sweep. */
void title_draw(float cx, float cy, float t, struct rgb tint);

#endif
