#include <math.h>
#include <string.h>

#include "audio/synth.h"

#define TABLE_BITS 11
#define TABLE_SIZE (1 << TABLE_BITS)
#define PARTIALS 4
#define BLOCK 32                /* samples between envelope updates */

/* The room the instruments stand in: two feedback delays that are not
   multiples of each other, so their echoes do not line up into a flutter. */
#define ECHO_A 13107            /* 0.297 s at 44.1 kHz */
#define ECHO_B 16361            /* 0.371 s */

static float g_table[TABLE_SIZE];
static int g_rate;

struct timbre {
    unsigned char ratio[PARTIALS];  /* partial frequency multiples */
    float level[PARTIALS];          /* relative loudness */
    unsigned char power[PARTIALS];  /* envelope raised to this: over sooner */
    float attack_s, decay_s;        /* 0 attack = struck; decay per pitch below */
    int decay_by_pitch;
};

static const struct timbre TIMBRES[] = {
    [SYNTH_PIANO] = { { 1, 2, 3, 4 }, { 1.00f, 0.42f, 0.18f, 0.08f }, { 1, 2, 2, 3 }, 0.0f,  2.4f, 1 },
    [SYNTH_GLASS] = { { 1, 2, 3, 5 }, { 1.00f, 0.22f, 0.10f, 0.04f }, { 1, 1, 1, 1 }, 0.04f, 2.6f, 0 },
    [SYNTH_PAD]   = { { 1, 2, 3, 4 }, { 1.00f, 0.35f, 0.12f, 0.05f }, { 1, 1, 2, 2 }, 0.9f,  5.5f, 0 },
};

struct voice {
    /* One phase per partial, 32-bit fixed point with the table index on
       top. A partial's own step is the fundamental's times its ratio, which
       is the same number the multiply used to make every sample. */
    unsigned phase[PARTIALS], inc[PARTIALS];
    float env;                  /* fundamental's envelope, 1 at the strike */
    float peak;                 /* where the attack is heading */
    float attack;               /* per-block rise while env < peak, 0 = struck */
    float decay;                /* per-block multiplier once past the peak */
    float gain_l, gain_r;
    float gain[PARTIALS];       /* this block's level per partial */
    int pair;                   /* the upper two are under hearing this block */
    const struct timbre *t;
    int active;
};

static struct voice g_voices[SYNTH_VOICES];
static float g_echo_a[ECHO_A], g_echo_b[ECHO_B];
static int g_echo_pos_a, g_echo_pos_b;
static float g_echo_lp;

void synth_init(int sample_rate) {
    g_rate = sample_rate;
    for (int i = 0; i < TABLE_SIZE; i++)
        g_table[i] = sinf((float)i / TABLE_SIZE * 6.28318530f);
    memset(g_voices, 0, sizeof(g_voices));
    memset(g_echo_a, 0, sizeof(g_echo_a));
    memset(g_echo_b, 0, sizeof(g_echo_b));
    g_echo_pos_a = g_echo_pos_b = 0;
    g_echo_lp = 0.0f;
}

static float midi_hz(int note) {
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}

void synth_strike(int note, float velocity, enum synth_timbre timbre, float pan) {
    struct voice *v = 0;
    for (int i = 0; i < SYNTH_VOICES; i++)
        if (!g_voices[i].active) { v = &g_voices[i]; break; }
    if (!v) {
        v = &g_voices[0];
        for (int i = 1; i < SYNTH_VOICES; i++)
            if (g_voices[i].env < v->env) v = &g_voices[i];
    }
    const struct timbre *t = &TIMBRES[timbre];
    float blocks_per_s = (float)g_rate / BLOCK;
    v->t = t;
    unsigned inc = (unsigned)(midi_hz(note) / g_rate * 4294967296.0f);
    for (int p = 0; p < PARTIALS; p++) {
        v->phase[p] = 0;
        v->inc[p] = inc * t->ratio[p];
    }
    v->peak = velocity;
    if (t->attack_s > 0.0f) {
        v->env = 0.0f;
        v->attack = velocity / (t->attack_s * blocks_per_s);
    } else {
        v->env = velocity;
        v->attack = 0.0f;
    }
    float seconds = t->decay_s;
    if (t->decay_by_pitch) {
        /* Low strings ring for seconds, high ones for a fraction of one. */
        seconds = t->decay_s * powf(0.5f, (note - 48) / 24.0f);
        if (seconds < 0.35f) seconds = 0.35f;
    }
    v->decay = expf(-1.0f / (seconds * blocks_per_s));
    if (pan < -1) pan = -1;
    if (pan > 1) pan = 1;
    v->gain_l = 0.5f + 0.5f * (1.0f - pan) * 0.5f + 0.25f * (pan < 0 ? -pan : 0);
    v->gain_r = 0.5f + 0.5f * (1.0f + pan) * 0.5f + 0.25f * (pan > 0 ? pan : 0);
    v->active = 1;
}

static inline float soft_clip(float x) {
    /* Gentle above +-0.7, hard wall at +-1. */
    if (x > 0.7f) x = 0.7f + (x - 0.7f) / (1.0f + (x - 0.7f) * 3.0f);
    else if (x < -0.7f) x = -0.7f + (x + 0.7f) / (1.0f - (x + 0.7f) * 3.0f);
    return x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
}

/* Envelopes move once per block; inside a block a voice is two or four
   table lookups and as many multiply-adds per sample, which is what the CPU
   can afford at 44.1 kHz. */
static void voice_block(struct voice *v) {
    if (v->attack > 0.0f) {
        v->env += v->attack;
        if (v->env >= v->peak) { v->env = v->peak; v->attack = 0.0f; }
    } else {
        v->env *= v->decay;
        if (v->env < 0.003f) { v->active = 0; return; }
    }
    float e = v->env, e2 = e * e, e3 = e2 * e;
    for (int p = 0; p < PARTIALS; p++) {
        unsigned char pw = v->t->power[p];
        v->gain[p] = v->t->level[p] * (pw >= 3 ? e3 : pw == 2 ? e2 : e);
    }
    /* The upper partials are quieter and die sooner, so for most of a
       note's life the third is already under the last bit of the output --
       and the fourth with it. Then the block is two partials wide. */
    v->pair = v->gain[2] < 1e-5f;
}

void synth_render(short *out, int frames) {
    while (frames > 0) {
        int n = frames < BLOCK ? frames : BLOCK;
        float mix_l[BLOCK], mix_r[BLOCK];
        memset(mix_l, 0, sizeof(mix_l));
        memset(mix_r, 0, sizeof(mix_r));

        for (int i = 0; i < SYNTH_VOICES; i++) {
            struct voice *v = &g_voices[i];
            if (!v->active) continue;
            voice_block(v);
            if (!v->active) continue;
            unsigned p0 = v->phase[0], p1 = v->phase[1];
            unsigned i0 = v->inc[0], i1 = v->inc[1];
            float g0 = v->gain[0], g1 = v->gain[1];
            float gl = v->gain_l, gr = v->gain_r;
            if (v->pair) {
                for (int f = 0; f < n; f++) {
                    float s = g_table[p0 >> (32 - TABLE_BITS)] * g0
                            + g_table[p1 >> (32 - TABLE_BITS)] * g1;
                    p0 += i0; p1 += i1;
                    mix_l[f] += s * gl;
                    mix_r[f] += s * gr;
                }
                /* The two that were left out still have to arrive where
                   they would have, for when the envelope brings them back. */
                v->phase[2] += (unsigned)n * v->inc[2];
                v->phase[3] += (unsigned)n * v->inc[3];
            } else {
                unsigned p2 = v->phase[2], p3 = v->phase[3];
                unsigned i2 = v->inc[2], i3 = v->inc[3];
                float g2 = v->gain[2], g3 = v->gain[3];
                for (int f = 0; f < n; f++) {
                    float s = g_table[p0 >> (32 - TABLE_BITS)] * g0
                            + g_table[p1 >> (32 - TABLE_BITS)] * g1
                            + g_table[p2 >> (32 - TABLE_BITS)] * g2
                            + g_table[p3 >> (32 - TABLE_BITS)] * g3;
                    p0 += i0; p1 += i1; p2 += i2; p3 += i3;
                    mix_l[f] += s * gl;
                    mix_r[f] += s * gr;
                }
                v->phase[2] = p2; v->phase[3] = p3;
            }
            v->phase[0] = p0; v->phase[1] = p1;
        }

        /* Two echoes fed with a mono sum, darkened a little each pass, and
           laid back in: a room with some depth to it. Neither delay wraps
           often, so the run up to the nearer wrap is taken in one go and
           the two tests come out of the inner loop. */
        for (int f = 0; f < n; ) {
            int m = n - f;
            if (m > ECHO_A - g_echo_pos_a) m = ECHO_A - g_echo_pos_a;
            if (m > ECHO_B - g_echo_pos_b) m = ECHO_B - g_echo_pos_b;
            float *da = &g_echo_a[g_echo_pos_a], *db = &g_echo_b[g_echo_pos_b];
            float lp = g_echo_lp;
            for (int k = 0; k < m; k++, f++) {
                float l = mix_l[f] * 0.60f, r = mix_r[f] * 0.60f;
                float mono = (l + r) * 0.5f;
                float ea = da[k], eb = db[k];
                lp += ((ea + eb) * 0.5f - lp) * 0.30f;
                da[k] = mono + lp * 0.50f;
                db[k] = mono + lp * 0.46f;
                l += ea * 0.30f + eb * 0.20f;
                r += eb * 0.30f + ea * 0.20f;
                out[f * 2 + 0] = (short)(soft_clip(l) * 32000.0f);
                out[f * 2 + 1] = (short)(soft_clip(r) * 32000.0f);
            }
            g_echo_lp = lp;
            if ((g_echo_pos_a += m) >= ECHO_A) g_echo_pos_a = 0;
            if ((g_echo_pos_b += m) >= ECHO_B) g_echo_pos_b = 0;
        }
        out += n * 2;
        frames -= n;
    }
}
