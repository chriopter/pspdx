#ifndef PSPDX_OSK_H
#define PSPDX_OSK_H

#include <stddef.h>

/* The firmware's on-screen keyboard, over the browser. It draws itself into
   each frame after the shell has, so the loop keeps drawing while it is up:
   the frame callback below is called once per frame for as long as the
   keyboard stands, and is expected to draw one. Set once, before the first
   read. */
void osk_frame(void (*draw)(void *ctx), void *ctx);

/* Blocks until the keyboard is put away. Returns 1 with the text in out,
   0 when it was cancelled, below zero when the firmware would not open it.
   The keyboard speaks UCS-2; what comes back is the ASCII of it, anything
   else dropped, which is all a URL needs. */
int osk_read(const char *title, const char *initial, char *out, size_t size);

#endif
