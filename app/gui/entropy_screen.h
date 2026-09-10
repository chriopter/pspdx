#ifndef PSPDX_ENTROPY_SCREEN_H
#define PSPDX_ENTROPY_SCREEN_H

/* The sweep, drawn in the browser's own world: the field starts dry and the
   stick carries a source of water over it. Needs gfx and the font up first.
   Returns the bits in the pool when it is done. */

void entropy_screen_prepare(void);
int entropy_screen_is_replay(void);
int entropy_screen_run(void);
void entropy_screen_reset_cache(void);

#endif
