#include <math.h>

#include "gui/lattice.h"

/* The surface runs to the horizon in every direction: rows a constant factor
   apart in depth until they are a few pixels under it, and beyond the
   nineteen columns in front of the viewer twelve more far out to either
   side, which only come into view where the rows have narrowed enough. The
   nineteen are the field the sweep fills; the twelve are open sea. */
#define NX_INNER 27
#define NX_OUTER 16
#define NX (NX_INNER + NX_OUTER)
#define J0 (NX_OUTER / 2)       /* first inner column */
#define J1 (J0 + NX_INNER - 1)  /* last inner column */
#define NZ 30                   /* lines into the distance */
/* The first rows are under the bottom edge of the screen, so the water runs
   off it rather than stopping on it; ROW0 is where the screen starts, and
   the sweep's field and the light both measure depth from there. */
#define ROW0 3
#define Z_NEAR 0.6f
#define Z_FAR 40.0f
#define STARS 22
#define SPECKS 12

/* The water in the world the GE draws it in: how high the eye sits over the
   surface, what one unit of the height field is worth there, and how many
   ripple tiles go to a unit of width. The height a near crest may reach is
   capped in pixels, so a tall swell does not climb over the footer. */
#define EYE_Y (150.0f / GFX_FOCAL)
#define H_SCALE (52.0f / GFX_FOCAL)
#define TILES 1.6f
#define RIPPLE_TEXELS 256.0f    /* one tile, as gfx builds it */
#define DY_CAP 42.0f
/* Past this much width per unit of depth a row is far off screen, and its
   corner can sit there rather than thousands of pixels out. */
#define XLIM 0.9167f

/* The wave equation on the cells. C is c^2 dt^2 / dx^2 and has to stay well
   under a half or the surface explodes; it also sets the speed, and a long
   ocean swell is slow. The damping is what stops a ring from ringing for
   ever. */
#define WAVE_C 0.09f
#define WAVE_DAMP 0.995f
#define WAVE_MAX 2.5f

/* What the source covers in cells, and how many frames of standing over a
   cell it takes to fill it. Sized so a sweep of the field at stick speed
   leaves no dry cells behind between passes. */
#define POUR_RX 2.5f
#define POUR_RZ 2.3f
#define POUR_RATE 0.34f
#define POUR_DENT 0.16f

/* What water is where no light reaches it. */
static const struct rgb DEEP = { 3, 8, 24 };

/* Deterministic and nothing to do with the entropy pool. */
static unsigned g_lcg = 0x9E3779B9;
static float frand(void) {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return (g_lcg >> 8) / 16777216.0f;
}

static struct { float x, y, vx, vy, size, phase; } g_stars[STARS];
static struct { float x, y, vy, size, phase; } g_specks[SPECKS];

/* A sine and a falling exponential cost a few hundred cycles each out of
   libm, and the surface wants thousands of them a frame. Both come out of a
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

static float fcos(float a) { return fsin(a + 1.5707963f); }

/* exp(-x) for x at or above zero. */
static float fexp(float x) {
    if (x >= GAUSS_MAX) return 0.0f;
    float p = x * (GAUSS_N / GAUSS_MAX);
    int i = (int)p;
    return g_gauss[i] + (g_gauss[i + 1] - g_gauss[i]) * (p - i);
}

/* Rows sit a constant factor apart in depth, which reads as an even surface
   once projected, and they never move: the depth, what a unit of height is
   worth there in pixels, and how much of the light a row still carries. */
static struct {
    float z, y0, yh, near, near2;
    float v;                    /* where the row sits in the ripple tile */
    float lod;                  /* the mip level a texel per pixel wants */
    float thin;                 /* 0 where the row is under a pixel tall */
} g_row[NZ];

static float g_u[NX];       /* across, sorted; -1..1 in front, wider outside */

/* The height field, its velocity, and how much of each cell is water at all.
   Everything else on screen is derived from these three a frame at a time. */
static float g_h[NZ][NX], g_v[NZ][NX], g_wet[NZ][NX];

/* Where every crossing landed this frame, and how the light found it. The
   four passes below -- the surface itself, the lines across, the lines away,
   and the crossings -- all want the same 682 points, so the surface is
   projected once and read four times. */
static float g_hh[NZ][NX];              /* the swell plus the simulation */
static float g_wx[NZ][NX], g_wy[NZ][NX];        /* where it is in the world */
static float g_x[NZ][NX], g_y[NZ][NX];          /* and where that lands */
static float g_lit[NZ][NX], g_spec[NZ][NX];
static unsigned g_fill[NZ][NX];         /* what the swell does to its colour */

/* The sweep's source: where it is over the field, and whether to draw it. */
static struct { float fx, fz; int on; } g_source;

/* Motes in the air over the horizon, rising slowly through the sky and
   never in front of the water: something drifting across the surface
   reads as dirt on it. */
static void speck_reset(int i, int anywhere) {
    g_specks[i].x = 20 + frand() * (SCR_W - 40);
    g_specks[i].y = anywhere ? 8 + frand() * (GFX_HORIZON - 16) : GFX_HORIZON - 4;
    g_specks[i].vy = 0.05f + frand() * 0.12f;
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
        /* Depth by row rather than by z: the rows are spaced by eye, so
           the light fades by eye too, evenly down to the horizon. */
        float depth = (float)(i - ROW0) / (NZ - 1 - ROW0);
        if (depth < 0.0f) depth = 0.0f;
        g_row[i].z = z;
        g_row[i].y0 = GFX_HORIZON + EYE_Y * GFX_FOCAL * inv;
        g_row[i].yh = H_SCALE * GFX_FOCAL * inv;
        g_row[i].near = 1.0f - depth;
        g_row[i].near2 = (1.0f - depth) * (1.0f - depth);
        /* The tile goes into the distance by the log of the depth, not the
           depth: laid down by depth alone, a far row a few pixels tall
           would cross hundreds of texel rows, and one bright texel row is
           a streak across the screen. This keeps the front row's density
           and lets the tile grow with distance, which the perspective
           takes back out. */
        g_row[i].v = TILES * Z_NEAR * logf(z);
        /* Texels per pixel, across and away, both grow with depth: across
           it is the tile's density over the pixels a unit of width gets,
           away it is the tile's density over the pixels a unit of depth
           gets, and the coarser of the two picks the level. */
        float across = TILES * RIPPLE_TEXELS * z / GFX_FOCAL;
        float away = TILES * Z_NEAR * RIPPLE_TEXELS / z * z * z / (EYE_Y * GFX_FOCAL);
        float dense = across > away ? across : away;
        g_row[i].lod = dense > 1.0f ? logf(dense) * (1.0f / 0.6931472f) : 0.0f;
    }
    /* The last rows before the horizon are under a pixel apart, and where
       two strips share an edge the rasteriser may paint it from both;
       added on, that is a line of bright dots. Rows that thin go out, and
       the horizon's own light stands in for them. */
    for (int i = 0; i < NZ; i++) {
        float tall = i + 1 < NZ ? g_row[i].y0 - g_row[i + 1].y0 : 0.0f;
        float thin = (tall - 1.0f) / 2.5f;
        g_row[i].thin = thin < 0.0f ? 0.0f : thin > 1.0f ? 1.0f : thin;
    }
    /* Out to fourteen: the far rows are forty deep, and the screen's edge
       is a third of the depth out, so anything less leaves a corner of
       the horizon bare. */
    static const float OUTER[NX_OUTER / 2] = { 1.25f, 1.6f, 2.1f, 2.8f, 4.0f, 6.0f, 9.0f, 14.0f };
    int j = 0;
    for (int k = NX_OUTER / 2 - 1; k >= 0; k--) g_u[j++] = -OUTER[k];
    for (int k = 0; k < NX_INNER; k++) g_u[j++] = (float)k / (NX_INNER - 1) * 2.0f - 1.0f;
    for (int k = 0; k < NX_OUTER / 2; k++) g_u[j++] = OUTER[k];

    for (int i = 0; i < STARS; i++) {
        g_stars[i].x = frand() * SCR_W;
        g_stars[i].y = 6 + frand() * (GFX_HORIZON - 30);
        g_stars[i].size = 3 + frand() * 6;
        g_stars[i].phase = frand() * 6.283f;
        /* Adrift, each its own way, a few pixels a second. */
        g_stars[i].vx = (frand() - 0.5f) * 0.12f;
        g_stars[i].vy = (frand() - 0.5f) * 0.05f;
    }
    for (int i = 0; i < SPECKS; i++) speck_reset(i, 1);

    /* Open water unless somebody asks for the sweep's empty field. */
    for (int i = 0; i < NZ; i++)
        for (int k = 0; k < NX; k++) {
            g_h[i][k] = g_v[i][k] = 0.0f;
            g_wet[i][k] = 1.0f;
        }
    g_source.on = 0;
}

void lattice_dry(void) {
    for (int i = 0; i < NZ; i++)
        for (int j = 0; j < NX; j++) g_h[i][j] = g_v[i][j] = g_wet[i][j] = 0.0f;
}

void lattice_settle(void) {
    g_source.on = 0;
    for (int i = 0; i < NZ; i++)
        for (int j = 0; j < NX; j++) g_wet[i][j] = 1.0f;
}

/* A dent in the surface, which the wave equation then turns into a ring. */
static void dent(float jf, float rf, float depth, float radius) {
    float inv = 1.0f / (radius * radius);
    int i0 = (int)(rf - radius), i1 = (int)(rf + radius) + 1;
    int j0 = (int)(jf - radius), j1 = (int)(jf + radius) + 1;
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > NZ - 1) i1 = NZ - 1;
    if (j1 > NX - 1) j1 = NX - 1;
    for (int i = i0; i <= i1; i++) {
        float di = i - rf;
        for (int j = j0; j <= j1; j++) {
            float dj = j - jf;
            float d = (di * di + dj * dj) * inv;
            if (d >= 1.0f) continue;
            g_h[i][j] -= depth * (1.0f - d) * (1.0f - d) * g_wet[i][j];
        }
    }
}

void lattice_touch(float x) {
    float jf = J0 + x * (NX_INNER - 1) + (frand() - 0.5f) * 3.0f;
    float rf = ROW0 + (0.12f + frand() * 0.5f) * (NZ - 1 - ROW0);
    dent(jf, rf, 1.5f + frand() * 1.1f, 2.2f + frand() * 1.8f);
}

void lattice_stir(float x, float y) {
    float push = x * x + y * y;
    if (push < 0.04f) return;                   /* the dead zone */
    if (push > 1.0f) push = 1.0f;
    /* Across the field with the stick, and stick forward is out toward
       the horizon. Kept off the very front rows, which are under the
       screen's bottom edge. */
    float fx = 0.5f + x * 0.42f;
    float fz = 0.42f - y * 0.34f;
    float jf = J0 + fx * (NX_INNER - 1) + (frand() - 0.5f) * 0.8f;
    float rf = ROW0 + fz * (NZ - 1 - ROW0) + (frand() - 0.5f) * 0.6f;
    /* A light press each frame rather than a drop: the wave equation adds
       them up into a wake, and the cap on the height keeps a stick held
       against its stop from digging a hole. */
    dent(jf, rf, 0.18f + 0.45f * push, 2.0f + 1.2f * push);
}

/* Velocity from the curvature, height from the velocity: the plain damped
   wave equation, with the edges reflecting because their neighbour is
   themselves. Dry ground is a shore -- what runs into it stops there. */
static void step_water(void) {
    for (int i = 0; i < NZ; i++) {
        int im = i ? i - 1 : 0, ip = i + 1 < NZ ? i + 1 : NZ - 1;
        for (int j = 0; j < NX; j++) {
            int jm = j ? j - 1 : 0, jp = j + 1 < NX ? j + 1 : NX - 1;
            float lap = g_h[im][j] + g_h[ip][j] + g_h[i][jm] + g_h[i][jp]
                      - 4.0f * g_h[i][j];
            g_v[i][j] = (g_v[i][j] + lap * WAVE_C) * WAVE_DAMP;
        }
    }
    for (int i = 0; i < NZ; i++) {
        for (int j = 0; j < NX; j++) {
            float w = g_wet[i][j];
            float h = g_h[i][j] + g_v[i][j];
            if (w < 1.0f) { h *= w; g_v[i][j] *= w; }
            g_h[i][j] = h > WAVE_MAX ? WAVE_MAX : h < -WAVE_MAX ? -WAVE_MAX : h;
        }
    }
}

float lattice_pour(float fx, float fz, int pouring) {
    g_source.fx = fx;
    g_source.fz = fz;
    g_source.on = 1;

    float jf = J0 + fx * (NX_INNER - 1);
    float rf = ROW0 + fz * (NZ - 1 - ROW0);
    if (pouring) {
        for (int i = ROW0; i < NZ; i++) {
            float di = (i - rf) / POUR_RZ;
            if (di * di >= 1.0f) continue;
            for (int j = J0; j <= J1; j++) {
                float dj = (j - jf) / POUR_RX;
                float d = di * di + dj * dj;
                if (d >= 1.0f) continue;
                float w = g_wet[i][j] + POUR_RATE * (1.0f - d * 0.5f);
                g_wet[i][j] = w > 1.0f ? 1.0f : w;
            }
        }
        dent(jf, rf, POUR_DENT, 2.8f);
    }
    /* Past the sides of the field, and under the bottom edge of the screen,
       the sea simply carries on. */
    for (int i = ROW0; i < NZ; i++) {
        for (int j = 0; j < J0; j++) g_wet[i][j] = g_wet[i][J0];
        for (int j = J1 + 1; j < NX; j++) g_wet[i][j] = g_wet[i][J1];
    }
    for (int i = 0; i < ROW0; i++)
        for (int j = 0; j < NX; j++) g_wet[i][j] = g_wet[ROW0][j];

    int wet = 0;
    for (int i = ROW0; i < NZ; i++)
        for (int j = J0; j <= J1; j++) if (g_wet[i][j] >= 0.5f) wet++;
    return (float)wet / ((NZ - ROW0) * NX_INNER);
}

static unsigned tinted(unsigned rgb, int alpha) {
    if (alpha < 0) alpha = 0;
    else if (alpha > 255) alpha = 255;
    return rgb | (unsigned)alpha << 24;
}

/* Two long swells crossing at an angle, each one sine of row and column. A
   sine of a sum is two products of the ends, so the whole surface costs
   four sines a row and four a column rather than two per cell. */
static void swell(float t) {
    float sa1[NZ], ca1[NZ], sa2[NZ], ca2[NZ];
    float sb1[NX], cb1[NX], sb2[NX], cb2[NX];
    for (int i = 0; i < NZ; i++) {
        float a1 = i * (0.55f * 21.0f / (NZ - 1)) - t * 0.80f, a2 = i * (0.95f * 21.0f / (NZ - 1)) + t * 0.55f;
        sa1[i] = fsin(a1); ca1[i] = fcos(a1);
        sa2[i] = fsin(a2); ca2[i] = fcos(a2);
    }
    for (int j = 0; j < NX; j++) {
        float b1 = g_u[j] * 1.2f, b2 = g_u[j] * -2.4f;
        sb1[j] = fsin(b1); cb1[j] = fcos(b1);
        sb2[j] = fsin(b2); cb2[j] = fcos(b2);
    }
    for (int i = 0; i < NZ; i++)
        for (int j = 0; j < NX; j++) {
            float s1 = sa1[i] * cb1[j] + ca1[i] * sb1[j];
            float s2 = sa2[i] * cb2[j] + ca2[i] * sb2[j];
            float s = 0.58f * s1 + 0.30f * s2 + g_h[i][j];
            /* Water is not a sine: crests stand up and troughs lie flat. */
            g_hh[i][j] = (s + 0.20f * s * (s < 0 ? -s : s)) * g_wet[i][j];
        }
}

/* Place the whole surface in the world and light it. Every corner gets its
   world position, the pixel that position projects to -- so the flat things
   drawn over the water agree with the mesh -- and the colour the big swell
   gives it, which the ripple's own palette is then multiplied by.

   The light the water glints back sits on the horizon, so the glints lie in
   a path that runs from under it down to the viewer, narrow at the far end
   and broad at the near one. Which facet inside the path catches it drifts,
   so the path never holds still. */
static void project_all(float t, float swayx, float lightx) {
    float lx = 0.50f * fsin(t * 0.19f);
    float lz = -0.28f + 0.26f * fsin(t * 0.12f + 1.0f);

    for (int i = 0; i < NZ; i++) {
        float z = g_row[i].z, xlim = XLIM * z;
        float path = 1.0f / (22.0f + 120.0f * g_row[i].near2);
        int im = i ? i - 1 : 0, ip = i + 1 < NZ ? i + 1 : NZ - 1;
        for (int j = 0; j < NX; j++) {
            int jm = j ? j - 1 : 0, jp = j + 1 < NX ? j + 1 : NX - 1;
            float h = g_hh[i][j];
            float dhx = g_hh[i][jp] - g_hh[i][jm];
            float dhz = g_hh[ip][j] - g_hh[im][j];
            float w = g_wet[i][j];

            float wx = g_u[j] + swayx;
            if (wx < -xlim) wx = -xlim;
            else if (wx > xlim) wx = xlim;
            /* Perspective would give the front row a crest half the screen
               tall; it is allowed this much and no more. */
            float dy = h * g_row[i].yh;
            if (dy > DY_CAP) dy = DY_CAP;
            else if (dy < -DY_CAP) dy = -DY_CAP;
            g_wx[i][j] = wx;
            g_wy[i][j] = dy * z / GFX_FOCAL - EYE_Y;
            gfx_water_project(wx, g_wy[i][j], z, &g_x[i][j], &g_y[i][j]);

            /* A crest stands in the light and a trough hides from it, and a
               face leaning back toward the horizon catches more than a flat
               one. Dry ground keeps only the little it is drawn with. */
            float raw = 0.05f + w * (0.16f + 0.52f * h - 0.60f * dhz)
                      + (1.0f - w) * 0.55f;      /* dry ground: the grid, lit */
            if (raw < 0.0f) raw = 0.0f;
            g_lit[i][j] = g_row[i].near2 * raw;
            float a = dhx - lx, b = dhz - lz;
            float d = (g_x[i][j] - lightx) * path;
            float glint = fexp((a * a + b * b) * 7.0f) * fexp(d * d);
            /* Squared with distance: a far row is a few pixels tall, and a
               glint on it is a dash, not a glint. */
            g_spec[i][j] = w * g_row[i].near2 * glint;

            /* The swell's own shading, which the palette's colour is read
               through: dark in a trough, white on a crest that has turned
               into the light, and broad where the light's path crosses. The
               water keeps most of it into the distance -- water does not
               stop being water halfway to the horizon -- and it is the alpha
               that carries the shore and the fade. */
            /* The swell's own shading. A far row is a pixel or two tall
               and its crossings a few pixels apart, so there it is
               dropped: lit crossing by crossing, a row that thin reads as
               a dashed line along the horizon. */
            float s = (raw - 0.16f) * 2.2f * g_row[i].near2;
            if (s < 0.0f) s = 0.0f;
            else if (s > 1.0f) s = 1.0f;
            float dr = d * 0.8f;
            float level = 0.14f + 1.25f * s + 0.45f * fexp(dr * dr)
                        + 0.90f * g_spec[i][j];
            if (level > 1.0f) level = 1.0f;
            int lit = (int)(level * 255.0f);
            int alpha = (int)(w * 200.0f * (0.35f + 0.65f * g_row[i].near) * g_row[i].thin);
            g_fill[i][j] = RGBA(lit, lit, lit, alpha);
        }
    }

    /* No row may climb over the one behind it. Where a crest would, the
       strip between the two folds and lands on itself, and added on
       twice it is a bright line straight across the screen. So from the
       horizon forward each row is held a pixel and a half below the last
       -- a whole pixel put the two edges on the same pixel rows, and the
       rasteriser painted a dot from each --
       and the world position is taken back from the pixel so the mesh and
       the flat things over it still agree. */
    for (int j = 0; j < NX; j++) {
        for (int i = NZ - 2; i >= 0; i--) {
            /* Not a sliver: a row pressed against the one behind it keeps
               a third of its natural height, so a crest is compressed
               into the rows ahead of it rather than drawn as a thin bright
               strip a pixel and a half tall. */
            float natural = g_row[i].y0 - g_row[i + 1].y0;
            float least = natural * 0.35f;
            if (least < 1.5f) least = 1.5f;
            float floor = g_y[i + 1][j] + least;
            if (g_y[i][j] < floor) {
                g_y[i][j] = floor;
                g_wy[i][j] = (GFX_HORIZON - floor) * g_row[i].z / GFX_FOCAL;
            }
        }
    }
}

/* The surface itself: one strip of quads per pair of rows, in real space, so
   the GE lays the tile down in perspective and the front row comes out even
   instead of hatched. The tile is anchored to the world and creeps toward
   the viewer, which is the movement between the crossings that the swell is
   too coarse to carry. */
static struct gfx_water_vertex *build_surface(float t) {
    struct gfx_water_vertex *mesh = gfx_water_mesh((NZ - 1) * NX * 2);
    if (!mesh) return 0;
    float du = t * 0.05f, dv = -t * 0.33f;
    struct gfx_water_vertex *p = mesh;
    for (int i = 0; i < NZ - 1; i++) {
        for (int j = 0; j < NX; j++) {
            for (int k = 0; k < 2; k++) {
                int r = i + k;
                p->u = g_wx[r][j] * TILES + du;
                p->v = g_row[r].v + dv;
                p->color = g_fill[r][j];
                p->x = g_wx[r][j];
                p->y = g_wy[r][j];
                p->z = -g_row[r].z;
                p++;
            }
        }
    }
    return mesh;
}

/* The same vertices are drawn once per ripple step; building them is the
   CPU's part and happens once. */
static void draw_surface(const struct gfx_water_vertex *mesh) {
    if (!mesh) return;
    for (int i = 0; i < NZ - 1; i++)
        gfx_water_strip(mesh + i * NX * 2, NX * 2,
                        0.5f * (g_row[i].lod + g_row[i + 1].lod));
}

/* The source hangs over the cell it is filling, so it has to be placed
   between four crossings that have already been projected. */
static void source_at(float *sx, float *sy) {
    float rf = ROW0 + g_source.fz * (NZ - 1 - ROW0);
    float jf = J0 + g_source.fx * (NX_INNER - 1);
    int i = (int)rf, j = (int)jf;
    if (i > NZ - 2) i = NZ - 2;
    if (j > NX - 2) j = NX - 2;
    float fi = rf - i, fj = jf - j;
    float x0 = g_x[i][j] + (g_x[i][j + 1] - g_x[i][j]) * fj;
    float x1 = g_x[i + 1][j] + (g_x[i + 1][j + 1] - g_x[i + 1][j]) * fj;
    float y0 = g_y[i][j] + (g_y[i][j + 1] - g_y[i][j]) * fj;
    float y1 = g_y[i + 1][j] + (g_y[i + 1][j + 1] - g_y[i + 1][j]) * fj;
    *sx = x0 + (x1 - x0) * fi;
    *sy = y0 + (y1 - y0) * fi;
}

void lattice_draw(float t, struct rgb tint) {
    float sway = fsin(t * 0.23f) * 0.06f;

    step_water();
    swell(t);

    gfx_batch_begin();

    /* Sky: a few points of light, and the horizon burning under them. */
    unsigned white = rgb_pack(RGB_WHITE, 0);
    for (int i = 0; i < STARS; i++) {
        g_stars[i].x += g_stars[i].vx + fsin(t * 0.3f + g_stars[i].phase) * 0.03f;
        g_stars[i].y += g_stars[i].vy;
        if (g_stars[i].x < -8) g_stars[i].x += SCR_W + 16;
        else if (g_stars[i].x > SCR_W + 8) g_stars[i].x -= SCR_W + 16;
        if (g_stars[i].y < 4) { g_stars[i].y = 4; g_stars[i].vy = -g_stars[i].vy; }
        else if (g_stars[i].y > GFX_HORIZON - 24) { g_stars[i].y = GFX_HORIZON - 24; g_stars[i].vy = -g_stars[i].vy; }
        float tw = 0.5f + 0.5f * fsin(t * 1.3f + g_stars[i].phase);
        gfx_glow(g_stars[i].x, g_stars[i].y, g_stars[i].size, g_stars[i].size,
                 tinted(white, (int)(30 + 70 * tw)));
    }
    /* The light is out past the far row, so the room sliding under it barely
       moves it. */
    float lightx = SCR_W / 2 + sway * (GFX_FOCAL / Z_FAR);
    gfx_glow(lightx, GFX_HORIZON + 6, 760, 110, rgb_pack(tint, 110));
    gfx_glow(lightx, GFX_HORIZON + 2, 420, 30,
             rgb_pack(rgb_mix(tint, RGB_WHITE, 0.6f), 120));

    /* What the water is made of, for the palette: its own dark, the sky it
       mirrors, and what a facet turned square into the light sends back. */
    gfx_water_light(0.42f * fsin(t * 0.13f), 0.86f, 0.30f,
                    rgb_pack(rgb_mix(tint, DEEP, 0.86f), 0),
                    rgb_pack(rgb_mix(tint, DEEP, 0.38f), 0),
                    rgb_pack(rgb_mix(rgb_mix(tint, RGB_WHITE, 0.9f), DEEP, 0.62f), 0));
    project_all(t, sway, lightx);
    /* Seven steps a second through the ripple's baked frames, each one
       crossfaded into the next so nothing jumps. */
    float phase = t * 7.0f;
    int step = (int)phase;
    float f = phase - step;
    gfx_water_begin(step);
    const struct gfx_water_vertex *mesh = build_surface(t);
    gfx_water_step(0, step, 1.0f - f);
    draw_surface(mesh);
    gfx_water_step(1, step + 1, f);
    draw_surface(mesh);
    gfx_water_end();

    float x[NX > NZ ? NX : NZ], y[NX > NZ ? NX : NZ];
    unsigned c[NX > NZ ? NX : NZ];
    /* The lines and the crossings each keep one colour all frame and vary
       only in alpha, so the channels are packed once and the alpha byte is
       laid in over them. */
    unsigned line = rgb_pack(rgb_mix(tint, RGB_WHITE, 0.15f), 0);

    /* The grid belongs to the ground, not to the water: it goes out under a
       cell as the cell fills, and what is left where the field is full is
       the surface and nothing else. */
    for (int i = ROW0 - 1; i < NZ; i++) {
        for (int j = 0; j < NX; j++) {
            x[j] = g_x[i][j];
            y[j] = g_y[i][j];
            c[j] = tinted(line, (int)(190 * g_lit[i][j] * (1.0f - g_wet[i][j])));
        }
        gfx_ribbon(x, y, c, NX, 0.9f);
    }
    for (int j = 0; j < NX; j++) {
        for (int i = ROW0 - 1; i < NZ; i++) {
            x[i] = g_x[i][j];
            y[i] = g_y[i][j];
            c[i] = tinted(line, (int)(140 * g_lit[i][j] * (1.0f - g_wet[i][j])));
        }
        gfx_ribbon(x + ROW0 - 1, y + ROW0 - 1, c + ROW0 - 1, NZ - ROW0 + 1, 0.9f);
    }
    /* Dry ground keeps a light at every crossing. Water keeps only the
       foam along the shore where a cell is filling but not yet full; its
       glints are in the palette, texel by texel, and a glow laid on a
       crossing over a row a few pixels tall came out as a dash. */
    unsigned cell = rgb_pack(rgb_mix(tint, RGB_WHITE, 0.45f), 0);
    unsigned foam = rgb_pack(rgb_mix(tint, RGB_WHITE, 0.85f), 0);
    for (int i = ROW0 - 1; i < NZ; i++) {
        for (int j = 0; j < NX; j++) {
            float sx = g_x[i][j];
            if (sx < -10 || sx > SCR_W + 10) continue;
            float sy = g_y[i][j], lit = g_lit[i][j], w = g_wet[i][j];
            if (w < 0.98f) {
                float size = 3.0f + 16.0f * lit;
                gfx_glow(sx, sy, size, size,
                         tinted(cell, (int)((50 + 200 * lit) * (1.0f - w))));
                if (w > 0.05f)
                    gfx_glow(sx, sy, 14, 7,
                             tinted(foam, (int)(200 * g_row[i].near2)));
            }
        }
    }

    /* The source: a light standing over the water it is making, the column
       under it, and what it throws up where the two meet. */
    if (g_source.on) {
        float sx, sy;
        source_at(&sx, &sy);
        unsigned core = rgb_pack(rgb_mix(tint, RGB_WHITE, 0.8f), 0);
        gfx_glow(sx, sy - 15, 7, 34, tinted(core, 150));
        gfx_glow(sx, sy, 54, 22, tinted(core, 120));
        gfx_glow(sx, sy, 26, 12, tinted(white, 200));
        float bob = 2.0f * fsin(t * 5.0f);
        gfx_glow(sx, sy - 30 + bob, 26, 26, tinted(core, 210));
        gfx_glow(sx, sy - 30 + bob, 11, 11, tinted(white, 255));
    }

    /* Spray lifting off the water. */
    unsigned spark = rgb_pack(rgb_mix(tint, RGB_WHITE, 0.7f), 0);
    for (int i = 0; i < SPECKS; i++) {
        g_specks[i].y -= g_specks[i].vy;
        g_specks[i].x += fsin(t * 1.7f + g_specks[i].phase) * 0.2f;
        if (g_specks[i].y < 6) speck_reset(i, 0);
        /* Brightest just over the horizon, gone by the top. */
        float life = (g_specks[i].y - 6) / (GFX_HORIZON - 10);
        gfx_glow(g_specks[i].x, g_specks[i].y, g_specks[i].size, g_specks[i].size,
                 tinted(spark, (int)(140 * life)));
    }

    gfx_batch_end();
}
