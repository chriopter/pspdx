#include <math.h>

#include "gui/lattice.h"

#define NX 19                   /* lines across */
#define NZ 13                   /* lines into the distance */
#define Z_NEAR 1.0f
#define Z_FAR 9.0f
#define HORIZON 116.0f
#define STARS 22
#define SPECKS 12

/* The ring a touch sends across the floor. No two alike: where it starts,
   how fast it runs, how high it stands, how wide it is, and whether a
   second one follows. */
static struct {
    float u, z, age, speed, height, width, second;
} g_ripple = { 0, 0.25f, 100.0f, 1.5f, 1.4f, 5.0f, 0 };

/* Deterministic and nothing to do with the entropy pool. */
static unsigned g_lcg = 0x9E3779B9;
static float frand(void) {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return (g_lcg >> 8) / 16777216.0f;
}

static struct { float x, y, size, phase; } g_stars[STARS];
static struct { float x, y, vy, size, phase; } g_specks[SPECKS];

/* A sine and a falling exponential cost a few hundred cycles each out of
   libm, and the floor wants a thousand of them a frame. Both come out of a
   table read with a linear step between entries, which is a hundredth of a
   level of alpha off -- nothing an eye or an 8-bit channel can hold. */
#define SIN_N 256
#define GAUSS_N 128
#define GAUSS_MAX 8.0f          /* exp(-8) is under a thousandth of a cell */

static float g_sin[SIN_N + 1];
static float g_gauss[GAUSS_N + 1];

static float fsin(float a) {
    float p = a * (SIN_N / 6.2831853f);
    int i = (int)p;
    if (p < 0) i--;                     /* the cast truncates toward zero */
    float f = p - i;
    i &= SIN_N - 1;
    return g_sin[i] + (g_sin[i + 1] - g_sin[i]) * f;
}

/* exp(-x) for x at or above zero. */
static float fexp(float x) {
    if (x >= GAUSS_MAX) return 0.0f;
    float p = x * (GAUSS_N / GAUSS_MAX);
    int i = (int)p;
    return g_gauss[i] + (g_gauss[i + 1] - g_gauss[i]) * (p - i);
}

/* Rows sit a constant factor apart in depth, which reads as an even floor
   once projected, and they never move. Everything the projection can work
   out from the row alone is worked out once, at startup:

       sx = SCR_W/2 + u * xu + sway * xs
       sy = y0 - h * yh
       lit = near2 * (0.55 + 0.45 * h)  */
static struct {
    float z8;               /* z * 0.8, the swell's argument */
    float gz;               /* 0..2 into the distance, for the ring */
    float y0, yh, xu, xs, near2;
} g_row[NZ];

static float g_u[NX];       /* -1..1 across */

/* Where every crossing landed this frame. The three passes below -- lines
   across, lines away, and the lights at the crossings -- all want the same
   247 points, so the floor is projected once and read three times. */
static float g_x[NZ][NX], g_y[NZ][NX], g_lit[NZ][NX];

static void speck_reset(int i, int anywhere) {
    g_specks[i].x = 20 + frand() * (SCR_W - 40);
    g_specks[i].y = anywhere ? HORIZON + frand() * (SCR_H - HORIZON) : SCR_H + 6;
    g_specks[i].vy = 0.10f + frand() * 0.25f;
    g_specks[i].size = 4 + frand() * 8;
    g_specks[i].phase = frand() * 6.283f;
}

void lattice_init(void) {
    for (int i = 0; i <= SIN_N; i++) g_sin[i] = sinf(i * (6.2831853f / SIN_N));
    for (int i = 0; i <= GAUSS_N; i++)
        g_gauss[i] = expf(-i * (GAUSS_MAX / GAUSS_N));

    for (int i = 0; i < NZ; i++) {
        float z = Z_NEAR * powf(Z_FAR / Z_NEAR, (float)i / (NZ - 1));
        float inv = 1.0f / z;
        float depth = (z - Z_NEAR) / (Z_FAR - Z_NEAR);
        g_row[i].z8 = z * 0.8f;
        g_row[i].gz = depth * 2.0f;
        g_row[i].y0 = HORIZON + 150.0f * inv;
        g_row[i].yh = 52.0f * inv;
        g_row[i].xu = 720.0f * inv;
        g_row[i].xs = 720.0f * inv * inv;
        g_row[i].near2 = (1.0f - depth) * (1.0f - depth);
    }
    for (int j = 0; j < NX; j++) g_u[j] = (float)j / (NX - 1) * 2.0f - 1.0f;

    for (int i = 0; i < STARS; i++) {
        g_stars[i].x = frand() * SCR_W;
        g_stars[i].y = 6 + frand() * (HORIZON - 30);
        g_stars[i].size = 3 + frand() * 6;
        g_stars[i].phase = frand() * 6.283f;
    }
    for (int i = 0; i < SPECKS; i++) speck_reset(i, 1);
}

void lattice_touch(float x) {
    g_ripple.age = 0.0f;
    g_ripple.u = x * 2.0f - 1.0f + (frand() - 0.5f) * 0.6f;
    g_ripple.z = 0.1f + frand() * 0.8f;
    g_ripple.speed = 1.1f + frand() * 1.3f;
    g_ripple.height = 0.9f + frand() * 1.1f;
    g_ripple.width = 3.5f + frand() * 4.0f;
    g_ripple.second = frand() < 0.5f ? 0.35f + frand() * 0.3f : 0.0f;
}

static unsigned tinted(unsigned rgb, int alpha) {
    if (alpha < 0) alpha = 0;
    else if (alpha > 255) alpha = 255;
    return rgb | (unsigned)alpha << 24;
}

/* Project the whole floor: a slow swell, and the ring. The swell is one
   sine of u times one of z plus one more of u, so it is 51 sines a frame
   rather than three per cell, and the ring's fade and reach are the same
   number everywhere and are taken once. */
static void project_all(float t, float sway) {
    float su[NX], sv[NX], du2[NX];
    for (int j = 0; j < NX; j++) {
        su[j] = fsin(g_u[j] * 2.4f + t * 0.7f);
        sv[j] = 0.12f * fsin(g_u[j] * 5.0f - t * 1.1f);
        float du = g_u[j] - g_ripple.u;
        du2[j] = du * du;
    }
    float fade = fexp(g_ripple.age * 1.1f) * g_ripple.height;
    float run = g_ripple.age * g_ripple.speed;
    float run2 = (g_ripple.age - g_ripple.second) * g_ripple.speed;
    /* Faded this far the ring can no longer move a pixel or a level of
       alpha, so it drops out of the loop entirely -- which is the state the
       floor is in most of the time. */
    int ringing = fade > 0.002f;
    int twice = ringing && g_ripple.second > 0.0f && g_ripple.age > g_ripple.second;

    for (int i = 0; i < NZ; i++) {
        float sz = 0.30f * fsin(g_row[i].z8 - t * 0.5f);
        float dz = g_row[i].gz - g_ripple.z, dz2 = dz * dz;
        float xs = sway * g_row[i].xs;
        for (int j = 0; j < NX; j++) {
            float h = sz * su[j] + sv[j];
            if (ringing) {
                float r = sqrtf(du2[j] + dz2);
                float d = (r - run) * g_ripple.width;
                h += fexp(d * d) * fade;
                if (twice) {
                    float d2 = (r - run2) * g_ripple.width;
                    h += fexp(d2 * d2) * fade * 0.6f;
                }
            }
            g_x[i][j] = SCR_W / 2 + g_u[j] * g_row[i].xu + xs;
            g_y[i][j] = g_row[i].y0 - h * g_row[i].yh;
            g_lit[i][j] = g_row[i].near2 * (0.55f + 0.45f * h);
        }
    }
}

void lattice_draw(float t, struct rgb tint) {
    g_ripple.age += 1.0f / 60.0f;
    float sway = fsin(t * 0.23f) * 0.06f;

    gfx_batch_begin();

    /* Sky: a few points of light, and the horizon burning under them. */
    unsigned white = rgb_pack(RGB_WHITE, 0);
    for (int i = 0; i < STARS; i++) {
        float tw = 0.5f + 0.5f * fsin(t * 1.3f + g_stars[i].phase);
        gfx_glow(g_stars[i].x, g_stars[i].y, g_stars[i].size, g_stars[i].size,
                 tinted(white, (int)(30 + 70 * tw)));
    }
    gfx_glow(SCR_W / 2 + sway * 200, HORIZON + 6, 760, 110, rgb_pack(tint, 110));
    gfx_glow(SCR_W / 2 + sway * 200, HORIZON + 2, 420, 30,
             rgb_pack(rgb_mix(tint, RGB_WHITE, 0.6f), 120));

    project_all(t, sway);

    float x[NX > NZ ? NX : NZ], y[NX > NZ ? NX : NZ];
    unsigned c[NX > NZ ? NX : NZ];
    /* The lines and the cells each keep one colour all frame and vary only
       in alpha, so the channels are packed once and the alpha byte is laid
       in over them. */
    unsigned line = rgb_pack(rgb_mix(tint, RGB_WHITE, 0.15f), 0);

    /* Lines across, near to far. */
    for (int i = 0; i < NZ; i++) {
        for (int j = 0; j < NX; j++) {
            x[j] = g_x[i][j];
            y[j] = g_y[i][j];
            c[j] = tinted(line, (int)(150 * g_lit[i][j]));
        }
        gfx_ribbon(x, y, c, NX, 0.9f);
    }
    /* Lines into the distance. */
    for (int j = 0; j < NX; j++) {
        for (int i = 0; i < NZ; i++) {
            x[i] = g_x[i][j];
            y[i] = g_y[i][j];
            c[i] = tinted(line, (int)(120 * g_lit[i][j]));
        }
        gfx_ribbon(x, y, c, NZ, 0.9f);
    }
    /* The cells themselves: a light at every crossing, brightest where the
       floor is highest, so the ring reads as a wave of lit cells. */
    unsigned cell = rgb_pack(rgb_mix(tint, RGB_WHITE, 0.45f), 0);
    for (int i = 0; i < NZ; i++) {
        for (int j = 0; j < NX; j++) {
            float sx = g_x[i][j];
            if (sx < -10 || sx > SCR_W + 10) continue;
            float lit = g_lit[i][j];
            float size = 3.0f + 16.0f * lit;
            gfx_glow(sx, g_y[i][j], size, size,
                     tinted(cell, (int)(60 + 190 * lit)));
        }
    }

    /* Sparks lifting off the floor. */
    unsigned spark = rgb_pack(rgb_mix(tint, RGB_WHITE, 0.7f), 0);
    for (int i = 0; i < SPECKS; i++) {
        g_specks[i].y -= g_specks[i].vy;
        g_specks[i].x += fsin(t * 1.7f + g_specks[i].phase) * 0.2f;
        if (g_specks[i].y < HORIZON - 10) speck_reset(i, 0);
        float life = (g_specks[i].y - HORIZON) / (SCR_H - HORIZON);
        gfx_glow(g_specks[i].x, g_specks[i].y, g_specks[i].size, g_specks[i].size,
                 tinted(spark, (int)(140 * life)));
    }

    gfx_batch_end();
}
