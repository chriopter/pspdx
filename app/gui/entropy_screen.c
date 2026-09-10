#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>

#include "logic/entropy.h"
#include "gui/entropy_screen.h"
#include "gui/gfx.h"
#include "gui/screen.h"

#define GRID_W 60
#define GRID_H 28
#define GRID_X 0
#define GRID_Y 2
#define SETTLE 14

#define TRACE_MAX 5000
#define TRACE_FILE  "ms0:/PSPDX.TRACE"
#define REPLAY_FILE "ms0:/PSPDX.REPLAY"
#define REC_DIR "ms0:/PSPDX_REC"
#define REC_EVERY 4

struct trace_sample { unsigned char lx, ly; unsigned short buttons; };

static struct trace_sample trace[TRACE_MAX];
static int trace_len;
static int trace_pos;
static int replaying;
static int recording;

static unsigned char cell_phase[GRID_H][GRID_W];
static unsigned char cell_done[GRID_H][GRID_W];
static char shadow_ch[GRID_H][GRID_W];
static unsigned shadow_col[GRID_H][GRID_W];

static const char IDLE_GLYPHS[] = ".,'`:;\"~";
static const char SPIN_GLYPHS[] = "|/-\\";
static const char BLOOM[] = "oO0@";
static const char LOGO_GLYPHS[] = "@#%*+";

/* Letters are 7 wide and the extrusion reaches 4 cells, so the pitch is 11:
   a shadow then lands in the gap and never inside the next letter. */
#define LOGO_X 5
#define LOGO_Y 8
static const unsigned char LOGO_BITS[5][7] = {
    { 124, 102, 102, 124,  96,  96,  96 },
    {  62,  96,  96,  60,   6,   6, 124 },
    { 124, 102, 102, 124,  96,  96,  96 },
    { 124, 102,  99,  99,  99, 102, 124 },
    {  99,  54,  28,   8,  28,  54,  99 },
};
static const unsigned char LOGO_LETTER_X[5] = { 0, 11, 22, 33, 44 };
static int logo_depth_x = 1;
static int logo_depth_y = 1;

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

static int logo_letter(int x, int y) {
    int lx = x - LOGO_X;
    int ly = y - LOGO_Y;
    if (lx < 0 || ly < 0 || ly >= 7) return 0;
    for (int letter = 0; letter < 5; letter++) {
        int within = lx - LOGO_LETTER_X[letter];
        if (within < 0 || within >= 7) continue;
        if (LOGO_BITS[letter][ly] & (1u << (6 - within))) return letter + 1;
    }
    return 0;
}

static unsigned logo_color(int x, int y, int frame) {
    static const unsigned colors[] = {
        0xFF3030FF, 0xFF20A0FF, 0xFF20E8FF, 0xFF40E040,
        0xFFFFD040, 0xFFFF7040, 0xFFE050E0
    };
    int band = (x + y + frame / 3) % 30;
    return band < 7 ? colors[band] : 0xFF40E040;
}

static char logo_face_glyph(int x, int y, int frame) {
    int band = (x + y + frame / 3) % 30;
    return band < 7 ? LOGO_GLYPHS[band % 3] : '#';
}

static int logo_extrusion(int x, int y, int *depth) {
    for (int d = 1; d <= 4; d++) {
        int letter = logo_letter(x - d * logo_depth_x, y - d * logo_depth_y);
        if (letter) {
            *depth = d;
            return letter;
        }
    }
    return 0;
}

static unsigned extrusion_color(int depth, int frame) {
    static const unsigned colors[] = {
        0xFF50D050, 0xFF309030, 0xFF206020, 0xFF103018
    };
    int lit = (frame / 10) % 4;
    int shade = depth - 1;
    if (shade == lit && shade > 0) shade--;
    return colors[shade];
}

static char idle_glyph(int x, int y) {
    unsigned h = (unsigned)(x * 73856093) ^ (unsigned)(y * 19349663);
    return IDLE_GLYPHS[(h >> 5) % (sizeof(IDLE_GLYPHS) - 1)];
}

static char tunnel_glyph(int x, int y, int frame, unsigned *color) {
    int dx = x - GRID_W / 2;
    int dy = (y - GRID_H / 2) * 2;
    int ax = dx < 0 ? -dx : dx;
    int ay = dy < 0 ? -dy : dy;
    int wave = (ax + ay + frame / 2) % 16;
    if (wave == 0) {
        *color = 0xFF30A030;
        if (ax > ay * 2) return '-';
        if (ay > ax * 2) return '|';
        return (dx < 0) == (dy < 0) ? '\\' : '/';
    }
    *color = COL_IDLE;
    return idle_glyph(x, y);
}

static void draw_cell(int x, int y, char ch, unsigned color) {
    if (shadow_ch[y][x] == ch && shadow_col[y][x] == color) return;
    shadow_ch[y][x] = ch;
    shadow_col[y][x] = color;
    pspDebugScreenSetTextColor(color);
    pspDebugScreenSetXY(GRID_X + x, GRID_Y + y);
    pspDebugScreenPrintf("%c", ch);
}

void entropy_screen_reset_cache(void) {
    memset(cell_phase, 0, sizeof(cell_phase));
    memset(cell_done, 0, sizeof(cell_done));
    memset(shadow_ch, 0, sizeof(shadow_ch));
    memset(shadow_col, 0, sizeof(shadow_col));
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

int entropy_screen_run(void) {
    entropy_screen_reset_cache();
    gui_clear();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    float cx = GRID_W / 2.0f;
    float cy = GRID_H / 2.0f;
    int covered = 0;
    int frame = 0;
    const int total = GRID_W * GRID_H;

    pspDebugScreenSetTextColor(replaying ? 0xFF4040FF : COL_TEXT);
    pspDebugScreenSetXY(0, 0);
    if (replaying)
        pspDebugScreenPrintf(" REPLAY -- recorded input, the entropy here is NOT real");
    else
        pspDebugScreenPrintf(" COLLECT ENTROPY -- sweep the field with the analog stick");

    for (;;) {
        SceCtrlData pad;
        if (!next_sample(&pad)) break;
        int dx = (int)pad.Lx - 128;
        int dy = (int)pad.Ly - 128;
        int moving = dx * dx + dy * dy > 14 * 14;

        if (moving) {
            cx += dx * 0.0065f;
            cy += dy * 0.0040f;
            if (cx < 0) cx = 0;
            if (cy < 0) cy = 0;
            if (cx > GRID_W - 1) cx = GRID_W - 1;
            if (cy > GRID_H - 1) cy = GRID_H - 1;
            entropy_absorb_motion(pad.Lx, pad.Ly, cx, cy);
        }

        int ccx = (int)(cx + 0.5f);
        int ccy = (int)(cy + 0.5f);
        logo_depth_x = ccx < 20 ? 1 : ccx > 39 ? -1 : 1;
        logo_depth_y = ccy < 9 ? 1 : ccy > 18 ? -1 : 1;

        for (int y = 0; y < GRID_H; y++) {
            for (int x = 0; x < GRID_W; x++) {
                int px = x - ccx;
                int py = y - ccy;
                if (moving && px * px + py * py * 3 <= 9) {
                    if (!cell_done[y][x] && cell_phase[y][x] == 0) covered++;
                    cell_phase[y][x] = SETTLE;
                    cell_done[y][x] = 1;
                }
            }
        }

        for (int y = 0; y < GRID_H; y++) {
            for (int x = 0; x < GRID_W; x++) {
                char ch;
                unsigned color;
                int letter = logo_letter(x, y);
                int depth = 0;
                int extrusion = logo_extrusion(x, y, &depth);
                int phase = cell_phase[y][x];
                if (phase > 0) cell_phase[y][x]--;
                if (letter) {
                    ch = logo_face_glyph(x, y, frame);
                    color = logo_color(x, y, frame);
                } else if (phase > 0) {
                    if (phase > SETTLE - 5) {
                        ch = BLOOM[(SETTLE - phase) % 4];
                        color = COL_SPIN;
                    } else {
                        ch = SPIN_GLYPHS[(phase + x + y) % 4];
                        color = phase > 4 ? COL_SPIN : COL_WARM;
                    }
                } else if (extrusion) {
                    ch = depth == 1 ? '/' : depth == 2 ? ':' : '.';
                    color = extrusion_color(depth, frame);
                } else if (cell_done[y][x]) {
                    ch = '#';
                    color = COL_DONE;
                } else {
                    ch = tunnel_glyph(x, y, frame, &color);
                }
                draw_cell(x, y, ch, color);
            }
        }

        draw_cell(ccx, ccy, SPIN_GLYPHS[(frame / 2) % 4], COL_CURSOR);
        shadow_ch[ccy][ccx] = 0;

        int percent = covered * 10000 / (total * 95);
        if (percent > 100) percent = 100;
        int ready = percent >= 100 && entropy_bits() >= ENTROPY_BITS;
        pspDebugScreenSetTextColor(ready ? COL_DONE : COL_TEXT);
        pspDebugScreenSetXY(0, 33);
        pspDebugScreenPrintf(" [");
        int filled = percent * 44 / 100;
        for (int i = 0; i < 44; i++) pspDebugScreenPrintf("%c", i < filled ? '=' : ' ');
        if (ready) pspDebugScreenPrintf("] %3d%%  X to continue", percent);
        else pspDebugScreenPrintf("] %3d%%  %4d bits ", percent, entropy_bits());
        if (ready && (pad.Buttons & PSP_CTRL_CROSS)) break;
        frame++;
        sceDisplayWaitVblankStart();
        record_frame(frame);
    }

    trace_save();
    pspDebugScreenSetTextColor(COL_TEXT);
    return entropy_bits();
}
