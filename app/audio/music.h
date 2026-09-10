#ifndef PSPDX_MUSIC_H
#define PSPDX_MUSIC_H

/* The tune under the browser: sixteen bars of piano that loop, played by
   the sequencer in here on the audio thread. Plain C, no platform. */

void music_init(int sample_rate);

/* Renders frames of the tune into out (stereo 16-bit interleaved), firing
   notes at their exact sample, not at the callback boundary. Anything
   posted through cues.h is played first. */
void music_render(short *out, int frames);

#endif
