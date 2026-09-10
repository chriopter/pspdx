#ifndef PSPDX_ENTROPY_SCREEN_H
#define PSPDX_ENTROPY_SCREEN_H

void entropy_screen_prepare(void);
int entropy_screen_is_replay(void);
int entropy_screen_run(void);
void entropy_screen_reset_cache(void);

#endif
