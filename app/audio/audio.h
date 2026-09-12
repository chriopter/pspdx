#ifndef PSPDX_AUDIO_H
#define PSPDX_AUDIO_H

#include <stddef.h>

/* The one file in audio/ that knows it is on a PSP: it owns the hardware
   channel and the thread that feeds it. Everything it plays comes from
   music.c and cues.c, which know nothing about either. */

int audio_start(void);
void audio_stop(void);

/* Longest time one callback took to render its buffer, in microseconds,
   since the last call. */
unsigned audio_worst_us(void);

/* A film is on: the tune steps aside for it, and comes back when it is
   over. Safe to call every frame. The tune steps aside for the card's
   sound the same way, which this knows about on its own. */
void audio_duck(int film_on);

/* The card's sound: an SND0.AT3 out of the EBOOT, ATRAC3 in a RIFF, played
   in a loop under the card for as long as the selection stands, and faded
   out when it moves. The bytes are copied at the call, so the caller's
   buffer is its own again when this returns. Either may be called from
   any thread; the decoding is all on the audio thread. Playing while one
   is on fades it out first; stopping with nothing on does nothing. */
void audio_sound_play(const void *at3, size_t len);
void audio_sound_stop(void);

#endif
