#ifndef PSPDX_CUES_H
#define PSPDX_CUES_H

/* What the interface sounds like. Any thread posts a cue; the audio thread
   plays it at its next callback. The ring between them is single-producer,
   single-consumer, so neither side locks. */

enum cue {
    CUE_MOVE,       /* the cursor moved; index says to which row */
    CUE_OPEN,       /* something was chosen */
    CUE_DONE,       /* it worked */
    CUE_FAIL        /* it did not */
};

void cues_post(enum cue cue, int index);

/* Audio thread only. */
void cues_drain(void);

#endif
