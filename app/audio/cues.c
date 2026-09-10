#include "audio/cues.h"
#include "audio/synth.h"
#include "util/runtime.h"

#define RING 16

static volatile struct { unsigned char cue; signed char index; } g_ring[RING];
static volatile unsigned g_head, g_tail;

void cues_post(enum cue cue, int index) {
    unsigned next = (g_head + 1) % RING;
    if (next == g_tail) return;         /* full: drop, a sound is not data */
    g_ring[g_head].cue = (unsigned char)cue;
    g_ring[g_head].index = (signed char)(index > 60 ? 60 : index < 0 ? 0 : index);
    g_head = next;
}

/* Rows step down a pentatonic scale from E5, so scrolling down a list
   plays it down; low enough to sit inside the tune rather than over it. */
static const unsigned char SCALE[5] = { 0, 2, 4, 7, 9 };

static void play(enum cue cue, int index) {
    switch (cue) {
    case CUE_MOVE: {
        /* A soft chime: the note, a fifth above it quieter and to one
           side, the octave below as body. Nothing sharp in it. A held
           direction runs through rows many times a second, and then it
           is the note alone and quieter: a run, not a pile-up, and a
           third of the voices. */
        static unsigned last_ms;
        unsigned now = now_ms();
        int run = now - last_ms < 90;
        last_ms = now;
        int note = 74 - SCALE[index % 5] - 12 * (index / 5);
        if (note < 55) note = 55;
        synth_strike(note, run ? 0.09f : 0.16f, SYNTH_GLASS, 0.2f, 0);
        if (run) break;
        synth_strike(note + 7, 0.06f, SYNTH_GLASS, 0.6f, 0);
        synth_strike(note - 12, 0.07f, SYNTH_PAD, -0.4f, 0);
        break;
    }
    case CUE_OPEN:
        synth_strike(62, 0.26f, SYNTH_GLASS, -0.4f, 0);
        synth_strike(69, 0.22f, SYNTH_GLASS, 0.4f, 0);
        break;
    case CUE_DONE:
        synth_strike(62, 0.24f, SYNTH_GLASS, -0.5f, 0);
        synth_strike(66, 0.24f, SYNTH_GLASS, -0.2f, 0);
        synth_strike(69, 0.26f, SYNTH_GLASS, 0.2f, 0);
        synth_strike(74, 0.20f, SYNTH_GLASS, 0.5f, 0);
        break;
    case CUE_FAIL:
        synth_strike(50, 0.28f, SYNTH_GLASS, -0.3f, 0);
        synth_strike(53, 0.20f, SYNTH_GLASS, 0.3f, 0);
        break;
    }
}

void cues_drain(void) {
    while (g_tail != g_head) {
        play((enum cue)g_ring[g_tail].cue, g_ring[g_tail].index);
        g_tail = (g_tail + 1) % RING;
    }
}
