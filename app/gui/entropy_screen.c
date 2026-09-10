/*
 * The entropy sweep. The field the browser stands on starts dry; the stick
 * carries a source of water over it, and where the source goes water is
 * born and spreads. When the field is water and the pool is full the key
 * can be made, and the surface that was just built stays as the backdrop.
 */

#include <math.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>

#include "gui/entropy_screen.h"
#include "gui/font.h"
#include "gui/gfx.h"
#include "gui/lattice.h"
#include "gui/palette.h"
#include "logic/entropy.h"

/* The stick moves the source across the field at the speed it moved the old
   text cursor across its 60 by 28 cells, so a sweep takes as long as it
   ever did. The pool is still fed the position it landed on. */
#define FIELD_W 60.0f
#define FIELD_H 28.0f
#define STEP_X (0.0065f / FIELD_W)
#define STEP_Z (0.0040f / FIELD_H)


#define TRACE_MAX 5000
#define TRACE_FILE  "ms0:/PSPDX.TRACE"
#define REPLAY_FILE "ms0:/PSPDX.REPLAY"
#define REC_DIR "ms0:/PSPDX_REC"
#define REC_EVERY 4

/* The room before there is a catalog to colour it: the shell's own default. */
static const struct rgb NIGHT_TOP = { 2, 3, 9 };
static const struct rgb NIGHT_BOTTOM = { 6, 8, 22 };
static const struct rgb TINT = { 80, 140, 255 };
static const struct rgb ALARM = { 255, 90, 80 };

struct trace_sample { unsigned char lx, ly; unsigned short buttons; };

static struct trace_sample trace[TRACE_MAX];
static int trace_len;
static int trace_pos;
static int replaying;
static int recording;

static float g_fx = 0.5f, g_fz = 0.5f;

static void trace_load(void) {
    int fd = sceIoOpen(REPLAY_FILE, PSP_O_RDONLY, 0777);
    if (fd < 0) return;
    sceIoClose(fd);
    fd = sceIoOpen(TRACE_FILE, PSP_O_RDONLY, 0777);
    if (fd < 0) return;
    int n = sceIoRead(fd, trace, sizeof(trace));
    sceIoClose(fd);
    if (n <= 0) return;
    trace_len = n / (int)sizeof(struct trace_sample);
    trace_pos = 0;
    replaying = 1;
}

static void trace_save(void) {
    if (replaying || trace_len == 0) return;
    int fd = sceIoOpen(TRACE_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, trace, (SceSize)(trace_len * (int)sizeof(struct trace_sample)));
    sceIoClose(fd);
}

static int next_sample(SceCtrlData *pad) {
    if (replaying) {
        if (trace_pos >= trace_len) return 0;
        struct trace_sample *sample = &trace[trace_pos++];
        memset(pad, 0, sizeof(*pad));
        pad->Lx = sample->lx;
        pad->Ly = sample->ly;
        pad->Buttons = sample->buttons;
        pad->TimeStamp = (unsigned)trace_pos;
        return 1;
    }
    sceCtrlPeekBufferPositive(pad, 1);
    if (trace_len < TRACE_MAX) {
        trace[trace_len].lx = pad->Lx;
        trace[trace_len].ly = pad->Ly;
        trace[trace_len].buttons = (unsigned short)pad->Buttons;
        trace_len++;
    }
    return 1;
}

void entropy_screen_reset_cache(void) {
    lattice_dry();
    g_fx = g_fz = 0.5f;
}

void entropy_screen_prepare(void) {
    trace_load();
    int fd = sceIoOpen("ms0:/PSPDX.RECORD", PSP_O_RDONLY, 0777);
    if (fd < 0) return;
    sceIoClose(fd);
    recording = 1;
    sceIoMkdir(REC_DIR, 0777);
}

int entropy_screen_is_replay(void) { return replaying; }

static void record_frame(int frame) {
    if (!recording || frame % REC_EVERY) return;
    char path[64];
    snprintf(path, sizeof(path), REC_DIR "/F%05d.BMP", frame / REC_EVERY);
    gfx_screenshot(path);
}

/* The name, glossy rather than stamped: a soft light behind it, the face
   printed a few times at a low alpha so its edge bleeds, then the face
   itself on top. */
static void title(const char *text, float cx, float y, float t) {
    static const float ox[4] = { -1.0f, 1.0f, 0.0f, 0.0f };
    static const float oy[4] = { 0.0f, 0.0f, -1.0f, 1.0f };
    float w = font_width(FONT_H1, text);
    float x = cx - w / 2;
    float pulse = 0.85f + 0.15f * sinf(t * 1.1f);
    gfx_glow(cx, y - 7, w + 90, 54, rgb_pack(TINT, (int)(90 * pulse)));
    unsigned halo = rgb_pack(rgb_mix(TINT, RGB_WHITE, 0.5f), 60);
    for (int i = 0; i < 4; i++) font_print(FONT_H1, x + ox[i], y + oy[i], halo, text);
    font_print(FONT_H1, x, y, rgb_pack(rgb_mix(RGB_WHITE, TINT, 0.12f), 255), text);
}

/* What the user has to know, over the water: what this is for, how far it
   has got, and when it can stop. */
static void draw_chrome(float t, int percent, int ready) {
    unsigned text = rgb_pack(rgb_mix(RGB_WHITE, TINT, 0.05f), 255);
    unsigned accent = rgb_pack(rgb_mix(TINT, RGB_WHITE, 0.45f), 255);
    unsigned dim = rgb_pack(rgb_mix(TINT, RGB_WHITE, 0.35f), 200);

    title("PSPDX", SCR_W / 2.0f, 58.0f, t);

    const char *head = replaying
        ? "REPLAY -- recorded input, the entropy here is NOT real"
        : "COLLECT ENTROPY -- sweep the field with the analog stick";
    font_print(FONT_META, 16, 96, replaying ? rgb_pack(ALARM, 255) : text, head);

    /* The bar is the shore drawn straight: how much of the field is water. */
    int bar_x = 16, bar_y = 238, bar_w = SCR_W - 32;
    gfx_rect(bar_x, bar_y, bar_w, 5, RGBA(255, 255, 255, 36));
    int filled = bar_w * percent / 100;
    if (filled > 0) gfx_hgrad(bar_x, bar_y, filled, 5, rgb_pack(TINT, 255), accent);

    char right[48];
    if (ready) snprintf(right, sizeof(right), "%d bits   X to continue", entropy_bits());
    else snprintf(right, sizeof(right), "%d%%   %d bits", percent, entropy_bits());
    float w = font_width(FONT_META, right);
    font_print(FONT_META, SCR_W - 16 - w, 232, ready ? accent : dim, right);
}

int entropy_screen_run(void) {
    entropy_screen_reset_cache();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    int frame = 0;
    for (;;) {
        SceCtrlData pad;
        if (!next_sample(&pad)) break;
        int dx = (int)pad.Lx - 128;
        int dy = (int)pad.Ly - 128;
        int moving = dx * dx + dy * dy > 14 * 14;

        if (moving) {
            g_fx += dx * STEP_X;
            /* Stick down runs the source at the viewer, not at the horizon. */
            g_fz -= dy * STEP_Z;
            if (g_fx < 0) g_fx = 0;
            if (g_fx > 1) g_fx = 1;
            if (g_fz < 0) g_fz = 0;
            if (g_fz > 1) g_fz = 1;
            /* The source pays for new ground: one field of the invisible
               250x250 grid, credited once, so holding the stick against its
               stop earns nothing. */
            int fx = (int)(g_fx * (ENTROPY_FIELD_SIDE - 1) + 0.5f);
            int fz = (int)(g_fz * (ENTROPY_FIELD_SIDE - 1) + 0.5f);
            entropy_absorb_field((unsigned)fz * ENTROPY_FIELD_SIDE + (unsigned)fx);
        }

        /* The water is the picture of the sweep, not its measure: the bar
           tracks the bits alone, and past the mark it stays full. */
        lattice_pour(g_fx, g_fz, moving);
        int bits = entropy_bits();
        int ready = bits >= ENTROPY_BITS;
        int percent = ready ? 100 : bits * 100 / ENTROPY_BITS;

        float t = gfx_frames() * (1.0f / 60.0f);
        gfx_frame_begin(0xFF000000);
        gfx_vgrad(0, 0, SCR_W, SCR_H,
                  rgb_pack(rgb_mix(NIGHT_TOP, TINT, 0.05f), 255),
                  rgb_pack(rgb_mix(NIGHT_BOTTOM, TINT, 0.18f), 255));
        lattice_draw(t, TINT);
        draw_chrome(t, percent, ready);
        gfx_frame_end();

        frame++;
        record_frame(frame);
        if (ready && (pad.Buttons & PSP_CTRL_CROSS)) break;
    }

    lattice_settle();
    trace_save();
    return entropy_bits();
}
