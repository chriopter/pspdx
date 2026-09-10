/*
 * The tune: a slow room. Eight chords, two bars each, on pads that take a
 * second to arrive and seven to leave, so each chord is still ringing when
 * the next one comes in. Over them a glass picks through the chord tones,
 * a piano says one or two things a bar, and now and then a very high, very
 * quiet note goes by like a satellite. Sixty-four to the minute; a loop of
 * a minute that does not sound like one.
 */

#include "audio/music.h"
#include "audio/cues.h"
#include "audio/synth.h"

#define BARS 16
#define BPM 64
#define CHORD_BARS 2
#define CHORDS (BARS / CHORD_BARS)

/* Bass, then four upper voices low to high. */
struct chord { unsigned char note[5]; };

static const struct chord CHORD[CHORDS] = {
    /* Dmaj9      */ { { 38, 57, 61, 64, 66 } },
    /* Bm11       */ { { 35, 54, 57, 62, 64 } },
    /* Gmaj7 add9 */ { { 43, 59, 62, 66, 69 } },
    /* Asus2 add9 */ { { 45, 57, 59, 64, 71 } },
    /* F#m9       */ { { 42, 57, 61, 64, 68 } },
    /* Gmaj9      */ { { 43, 59, 62, 66, 69 } },
    /* Em9        */ { { 40, 55, 59, 62, 66 } },
    /* A add9     */ { { 45, 57, 62, 64, 71 } },
};

/* Where in a two-bar chord (sixteen eighths) the glass picks, and which
   chord tone: 1..4 are the upper voices, +12 an octave up. */
static const struct { unsigned char pos, tone; signed char octave; unsigned char vel; } PICK[] = {
    { 0, 4, 0, 16 }, { 3, 2, 1, 12 }, { 5, 3, 0, 14 }, { 8, 1, 1, 12 },
    { 10, 4, 1, 10 }, { 13, 3, 1, 12 }, { 14, 2, 0, 11 },
};

/* The piano, per two bars: a few notes, mostly on the chord. */
static const struct { unsigned char pos, tone; signed char octave; unsigned char vel; } SAY[] = {
    { 0, 4, 0, 28 }, { 6, 3, 0, 20 }, { 11, 4, 1, 18 },
};

static int g_rate;
static long g_pos;              /* sample position inside the loop */
static long g_loop;             /* loop length in samples */
static float g_eighth;          /* samples per eighth */

struct event { long at; unsigned char note, vel, timbre; signed char pan; };
#define MAX_EVENTS 1024
static struct event g_events[MAX_EVENTS];
static int g_event_count;
static int g_next;

static void add(long at, int note, int vel, enum synth_timbre timbre, float pan) {
    if (g_event_count >= MAX_EVENTS) return;
    struct event *e = &g_events[g_event_count++];
    e->at = at;
    e->note = (unsigned char)note;
    e->vel = (unsigned char)vel;
    e->timbre = (unsigned char)timbre;
    e->pan = (signed char)(pan * 100);
}

static void sort_events(void) {
    for (int i = 1; i < g_event_count; i++) {
        struct event e = g_events[i];
        int j = i - 1;
        while (j >= 0 && g_events[j].at > e.at) { g_events[j + 1] = g_events[j]; j--; }
        g_events[j + 1] = e;
    }
}

void music_init(int sample_rate) {
    g_rate = sample_rate;
    g_eighth = 60.0f / BPM / 2.0f * sample_rate;
    g_loop = (long)(BARS * 8 * g_eighth);
    g_pos = 0;
    g_next = 0;
    g_event_count = 0;

    for (int c = 0; c < CHORDS; c++) {
        long start = (long)(c * CHORD_BARS * 8 * g_eighth);
        const struct chord *ch = &CHORD[c];

        /* The pad: bass in the middle, the four voices spread across the
           room, the highest a touch later so the chord blooms. */
        add(start, ch->note[0], 30, SYNTH_PAD, 0.0f);
        add(start, ch->note[1], 20, SYNTH_PAD, -0.7f);
        add(start, ch->note[2], 20, SYNTH_PAD, 0.4f);
        add(start + (long)(0.5f * g_eighth), ch->note[3], 18, SYNTH_PAD, -0.3f);
        add(start + (long)(1.0f * g_eighth), ch->note[4], 16, SYNTH_PAD, 0.7f);

        for (unsigned i = 0; i < sizeof(PICK) / sizeof(*PICK); i++) {
            int note = ch->note[PICK[i].tone] + 12 * PICK[i].octave;
            float pan = (i & 1) ? 0.5f : -0.5f;
            add(start + (long)(PICK[i].pos * g_eighth), note, PICK[i].vel, SYNTH_GLASS, pan);
        }
        for (unsigned i = 0; i < sizeof(SAY) / sizeof(*SAY); i++) {
            int note = ch->note[SAY[i].tone] + 12 * SAY[i].octave;
            /* Every other chord the piano waits a beat, so it is not a
               pattern you can count. */
            long late = (c & 1) ? (long)(2 * g_eighth) : 0;
            add(start + late + (long)(SAY[i].pos * g_eighth), note, SAY[i].vel, SYNTH_PIANO, 0.1f);
        }
        /* The satellite: once per chord, somewhere in the second bar. */
        add(start + (long)((9 + (c * 5) % 6) * g_eighth), 91 + (c * 7) % 5, 7, SYNTH_GLASS,
            (c & 1) ? 0.9f : -0.9f);
    }
    sort_events();
}

void music_render(short *out, int frames) {
    cues_drain();
    while (frames > 0) {
        long until = g_next < g_event_count ? g_events[g_next].at - g_pos
                                             : g_loop - g_pos;
        if (until <= 0) {
            if (g_next < g_event_count) {
                const struct event *e = &g_events[g_next++];
                synth_strike(e->note, e->vel / 100.0f, (enum synth_timbre)e->timbre,
                             e->pan / 100.0f);
            } else {
                g_pos = 0;
                g_next = 0;
            }
            continue;
        }
        int n = until < frames ? (int)until : frames;
        synth_render(out, n);
        out += n * 2;
        frames -= n;
        g_pos += n;
    }
}
