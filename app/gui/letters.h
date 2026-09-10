#ifndef PSPDX_LETTERS_H
#define PSPDX_LETTERS_H

/* Block capitals, five cells by seven, raised off a plane in space: the
   lettering that stands in the room while something is being waited for.
   Cells, not glyphs -- it is the same stuff the floor is made of. */

/* Draws text centred on (cx, cy) at cell pixels per cell, leaning by yaw
   and pitch, in color with depth layers of extrusion behind the face.
   Letters not in the set draw as a gap. */
void letters_draw(const char *text, float cx, float cy, float cell,
                  float yaw, float pitch, unsigned color, int depth);

/* How wide text comes out at that cell size, for placing things under it. */
float letters_width(const char *text, float cell);

#endif
