#ifndef PSPDX_LATTICE_H
#define PSPDX_LATTICE_H

#include "gui/palette.h"

/* The backdrop: a floor of lit cells in perspective, the sweep's grid seen
   from above, running to a horizon. It breathes on its own, and every
   selection sends a ring across it. Nothing in here knows what a catalog is;
   it takes a colour, a time, and where the last touch landed. */

void lattice_init(void);

/* A ring starts under screen position x (0..1 across the floor). */
void lattice_touch(float x);

void lattice_draw(float t, struct rgb tint);

#endif
