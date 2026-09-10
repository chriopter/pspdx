#ifndef PSPDX_AUDIO_H
#define PSPDX_AUDIO_H

/* The one file in audio/ that knows it is on a PSP: it owns the hardware
   channel and the thread that feeds it. Everything it plays comes from
   music.c and cues.c, which know nothing about either. */

int audio_start(void);
void audio_stop(void);

/* Longest time one callback took to render its buffer, in microseconds,
   since the last call. */
unsigned audio_worst_us(void);

#endif
