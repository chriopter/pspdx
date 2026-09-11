#ifndef PSPDX_LATTICE_H
#define PSPDX_LATTICE_H

#include "gui/palette.h"

/* The backdrop: a water surface in perspective, rows running to a horizon.
   A height field lives on the crossings -- crests catch the light, troughs
   go dark, and a glint wanders across whatever slope happens to face the
   horizon. Nothing in here knows what a catalog is; it takes a colour, a
   time, and where something landed.

   The same surface is the entropy sweep's field. There it starts dry and a
   source fills it, so the browser stands on the water the sweep built. */

void lattice_init(void);

/* A drop falls under screen position x (0..1 across the floor), and the ring
   it leaves runs outward on its own. */
void lattice_touch(float x);

/* A hand in the water: the stick's position, both -1..1, pressed into
   the surface every frame it is held off centre. The point wanders with
   the stick and every frame leaves a dent, so a stick swung about makes a
   wake of rings crossing rings. Harder the further the stick is pushed. */
void lattice_stir(float x, float y);

void lattice_draw(float t, struct rgb tint);

/* The sweep. dry() empties the field. pour() puts the source at (fx, fz) --
   across, and into the distance, both 0..1 -- and while it is pouring wets
   what is under it and dents the surface; it returns how much of the field
   is water. settle() ends the sweep: the source goes and the last of the
   field fills, so the browser never stands on dry ground. */
void lattice_dry(void);
float lattice_pour(float fx, float fz, int pouring);
void lattice_settle(void);

#endif
