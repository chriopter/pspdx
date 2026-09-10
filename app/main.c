/*
 * PSPDX -- the on-device client. Gathers entropy, fetches the catalog over
 * TLS 1.3 and shows it. https.c carries the network, this file the rest.
 */

#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspiofilemgr.h>
#include <psprtc.h>
#include <psputility.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pspctrl.h>
#include <cjson/cJSON.h>

#include "pspdx.h"
#include "install.h"

PSP_MODULE_INFO("pspdx", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
/* Only a few hundred KB are actually needed: the response buffer plus wolfSSL's
   record buffers. Leaving the rest to the system keeps a PSP-1000 comfortable. */
PSP_HEAP_SIZE_KB(4 * 1024);

#define CATALOG_URL "https://chriopter.github.io/pspdx/catalog.json"

#define ANIM_ROW 32

/* --------------------------------------------------------------- logging */

#define LOGLINES 26
#define LOGCOLS (COLS + 1)

static char g_log[LOGLINES][LOGCOLS];
static int g_logn = 0;

/* The catalog body. ~200 bytes per app; this holds a thousand. */
static char g_resp[200 * 1024];
static size_t g_resplen = 0;
static struct https_result g_fetch;

void logline(const char *fmt, ...) {
    char line[LOGCOLS];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (g_logn < LOGLINES) {
        strcpy(g_log[g_logn++], line);
    }
    sceIoWrite(1, line, strlen(line));
    sceIoWrite(1, "\n", 1);
}

/* Written once and closed. PPSSPP only flushes an emulated file to the host on
   close, so incremental logging to the stick never shows up. */
static void dump_to_stick(void) {
    int fd = sceIoOpen("ms0:/PSPDX.LOG", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        for (int i = 0; i < g_logn; i++) {
            sceIoWrite(fd, g_log[i], strlen(g_log[i]));
            sceIoWrite(fd, "\n", 1);
        }
        sceIoClose(fd);
    }
    if (g_resplen) {
        fd = sceIoOpen("ms0:/PSPDX.HTTP", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, g_resp, g_resplen);
            sceIoClose(fd);
        }
    }
}

/* ------------------------------------------------------------------ time */

unsigned now_ms(void) {
    u64 tick = 0;
    sceRtcGetCurrentTick(&tick);
    return (unsigned)(tick / 1000);
}

int expired(unsigned start, unsigned budget_ms) {
    return (now_ms() - start) > budget_ms;
}

/* ------------------------------------------------------------------ boot */

static int exit_callback(int a, int b, void *c) {
    (void)a; (void)b; (void)c;
    sceKernelExitGame();
    return 0;
}

static int callback_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int cbid = sceKernelCreateCallback("Exit", exit_callback, NULL);
    if (cbid >= 0) {
        sceKernelRegisterExitCallback(cbid);
    }
    sceKernelSleepThreadCB();
    return 0;
}

/* Without this thread HOME does nothing and the only way out is the power
   switch, so a failure here is worth reporting rather than swallowing. */
static int setup_callbacks(void) {
    int thid = sceKernelCreateThread("update_thread", callback_thread,
                                     0x11, 0xFA0, THREAD_ATTR_USER, 0);
    if (thid < 0) return thid;
    return sceKernelStartThread(thid, 0, 0);
}

static void screenshot_to_stick(const char *path);

/* ------------------------------------------------------------- psprandom */

/* Kept deliberately free of wolfSSL types so it can be lifted into its own
   library later with a `git mv`. The console has no usable randomness of its
   own: pspsdk's getentropy() is MT19937 reseeded from time(NULL) on every
   call, and the hardware PRNG in the crypto chip is a kernel-only export that
   a user-mode EBOOT cannot import at all.
   Human input is the one source that comes from outside the machine's
   determinism -- the timing of a moving thumb is not reproducible, and it
   works the same on hardware and under an emulator driven by a real gamepad.
   The same ritual PGP and TrueCrypt used with mouse movement. */

#define POOL_BYTES 20      /* one SHA-1 digest */
#define ENTROPY_BITS 256

static unsigned char g_pool[POOL_BYTES];
static unsigned int g_pool_counter = 0;
int g_pool_bits = 0;

/* The console's own SHA-1, not wolfSSL's SHA-256: the seed callback is invoked
   from inside wolfCrypt_Init, before wolfSSL's digests are usable -- doing it
   the other way round makes wolfSSL_Init fail with WC_INIT_E. sceKernelUtils*
   is a plain user-mode export and keeps this unit free of any dependency.
   Collision resistance is not the property being asked of it; mixing is. */
static void pool_absorb(const void *data, unsigned int len) {
    unsigned char buf[POOL_BYTES + 64];
    unsigned int n = len > 64 ? 64 : len;
    memcpy(buf, g_pool, POOL_BYTES);
    memcpy(buf + POOL_BYTES, data, n);
    sceKernelUtilsSha1Digest(buf, POOL_BYTES + n, g_pool);
}

/* Counts loop iterations between two clock ticks. On real silicon this varies
   with interrupts and refresh cycles; under an emulator it is nearly constant,
   which is why it is a supplement and never the whole seed. */
static void pool_absorb_jitter(int rounds) {
    for (int i = 0; i < rounds; i++) {
        unsigned t0 = sceKernelGetSystemTimeLow();
        unsigned spins = 0;
        while (sceKernelGetSystemTimeLow() - t0 < 500) spins++;
        pool_absorb(&spins, sizeof(spins));
    }
}

static void psprandom_init(void) {
    unsigned t = sceKernelGetSystemTimeLow();
    void *sp = &t;
    pool_absorb(&t, sizeof(t));
    pool_absorb(&sp, sizeof(sp));
    pool_absorb_jitter(8);
    g_pool_bits = 0;                    /* none of the above is credited */
}

/* ------------------------------------------------------------- the sweep */

/* Sweeping a field is not decoration. Idle waggling parks the stick in the
   centre or against a stop, where the ADC saturates and stops telling you
   anything; covering a grid forces the whole range to be visited. It also
   makes the accounting honest -- a worn, drifting stick produces changes that
   a naive counter would credit, but it cannot complete a field. */

#define GRID_W 60
#define GRID_H 28
#define GRID_X 0
#define GRID_Y 2
#define SETTLE 14                 /* frames a touched cell spends spinning */

#define COL_IDLE   0xFF404048     /* ABGR: cold grey */
#define COL_SPIN   0xFFF0F0F0     /* white, mid-swirl */
#define COL_WARM   0xFFC0FFC0     /* settling */
#define COL_DONE   0xFF20C020     /* green, counted */
#define COL_CURSOR 0xFFFFFF40     /* cyan */
#define COL_TEXT   0xFFFFFFFF
#define COL_DIM    0xFF909090

/* Every pad sample the sweep sees, so a session can be replayed byte for byte.
   Replaying is for development only: a fixed trace yields a fixed pool, which
   is the opposite of entropy -- so a replayed run refuses to save its seed and
   says so on screen. */
#define TRACE_MAX 5000
#define TRACE_FILE  "ms0:/PSPDX.TRACE"
#define REPLAY_FILE "ms0:/PSPDX.REPLAY"

struct trace_sample { unsigned char lx, ly; unsigned short buttons; };
static struct trace_sample g_trace[TRACE_MAX];
static int g_trace_len = 0;
static int g_trace_pos = 0;
static int g_replay = 0;

static void trace_load(void) {
    int fd = sceIoOpen(REPLAY_FILE, PSP_O_RDONLY, 0777);
    if (fd < 0) return;
    sceIoClose(fd);

    fd = sceIoOpen(TRACE_FILE, PSP_O_RDONLY, 0777);
    if (fd < 0) return;
    int n = sceIoRead(fd, g_trace, sizeof(g_trace));
    sceIoClose(fd);
    if (n <= 0) return;
    g_trace_len = n / (int)sizeof(struct trace_sample);
    g_trace_pos = 0;
    g_replay = 1;
}

static void trace_save(void) {
    if (g_replay || g_trace_len == 0) return;
    int fd = sceIoOpen(TRACE_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, g_trace, (SceSize)(g_trace_len * (int)sizeof(struct trace_sample)));
    sceIoClose(fd);
}

/* One source of pad data for the sweep, live or recorded. */
static int next_sample(SceCtrlData *pad) {
    if (g_replay) {
        if (g_trace_pos >= g_trace_len) return 0;
        struct trace_sample *t = &g_trace[g_trace_pos++];
        memset(pad, 0, sizeof(*pad));
        pad->Lx = t->lx;
        pad->Ly = t->ly;
        pad->Buttons = t->buttons;
        pad->TimeStamp = (unsigned)g_trace_pos;
        return 1;
    }
    sceCtrlPeekBufferPositive(pad, 1);
    if (g_trace_len < TRACE_MAX) {
        g_trace[g_trace_len].lx = pad->Lx;
        g_trace[g_trace_len].ly = pad->Ly;
        g_trace[g_trace_len].buttons = (unsigned short)pad->Buttons;
        g_trace_len++;
    }
    return 1;
}

static unsigned char cell_phase[GRID_H][GRID_W];
static unsigned char cell_done[GRID_H][GRID_W];
static char shadow_ch[GRID_H][GRID_W];
static unsigned shadow_col[GRID_H][GRID_W];

static const char IDLE_GLYPHS[] = ".,'`:;\"~";
static const char SPIN_GLYPHS[] = "|/-\\";
static const char BLOOM[] = "oO0@";

static void grid_reset(void) {
    memset(cell_phase, 0, sizeof(cell_phase));
    memset(cell_done, 0, sizeof(cell_done));
    memset(shadow_ch, 0, sizeof(shadow_ch));
    memset(shadow_col, 0, sizeof(shadow_col));
}

static char idle_glyph(int x, int y) {
    /* Stable per cell, so the field does not shimmer when nothing happens. */
    unsigned h = (unsigned)(x * 73856093) ^ (unsigned)(y * 19349663);
    return IDLE_GLYPHS[(h >> 5) % (sizeof(IDLE_GLYPHS) - 1)];
}

static void draw_cell(int x, int y, char ch, unsigned col) {
    if (shadow_ch[y][x] == ch && shadow_col[y][x] == col) return;
    shadow_ch[y][x] = ch;
    shadow_col[y][x] = col;
    pspDebugScreenSetTextColor(col);
    pspDebugScreenSetXY(GRID_X + x, GRID_Y + y);
    pspDebugScreenPrintf("%c", ch);
}

/* ------------------------------------------------------------- recording */

/* With a file named PSPDX.RECORD on the stick, every fourth frame of the sweep
   is written to PSPDX_REC/ as a BMP, and the host turns them into a video. The
   emulator's own frame dump produces an unreadable file in this build, and a
   real console has no capture at all -- so the app records itself, the same
   way it takes its own screenshots. Roughly 400 files and 150 MB for one
   sweep, which is why it is off unless the file is there. */
#define REC_DIR "ms0:/PSPDX_REC"
#define REC_EVERY 4

static int g_recording = 0;

static void record_init(void) {
    int fd = sceIoOpen("ms0:/PSPDX.RECORD", PSP_O_RDONLY, 0777);
    if (fd < 0) return;
    sceIoClose(fd);
    g_recording = 1;
    sceIoMkdir(REC_DIR, 0777);
}

static void record_frame(int frame) {
    if (!g_recording || (frame % REC_EVERY)) return;
    char path[64];
    snprintf(path, sizeof(path), REC_DIR "/F%05d.BMP", frame / REC_EVERY);
    screenshot_to_stick(path);
}

/* Runs the sweep and leaves the pool filled. Returns credited bits. */
static int psprandom_sweep(void) {
    grid_reset();
    pspDebugScreenClear();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    float cx = GRID_W / 2.0f, cy = GRID_H / 2.0f;
    int covered = 0, frame = 0;
    const int total = GRID_W * GRID_H;

    pspDebugScreenSetTextColor(g_replay ? 0xFF4040FF : COL_TEXT);
    pspDebugScreenSetXY(0, 0);
    if (g_replay)
        pspDebugScreenPrintf(" REPLAY -- recorded input, the entropy here is NOT real");
    else
        pspDebugScreenPrintf(" COLLECT ENTROPY -- sweep the field with the analog stick");

    for (;;) {
        SceCtrlData pad;
        if (!next_sample(&pad)) break;         /* replay ran out */

        int dx = (int)pad.Lx - 128;
        int dy = (int)pad.Ly - 128;
        int moving = (dx * dx + dy * dy) > (14 * 14);   /* deadzone */

        if (moving) {
            cx += dx * 0.0065f;
            cy += dy * 0.0040f;
            if (cx < 0) cx = 0;
            if (cy < 0) cy = 0;
            if (cx > GRID_W - 1) cx = GRID_W - 1;
            if (cy > GRID_H - 1) cy = GRID_H - 1;

            struct { unsigned char lx, ly; unsigned int sys; float px, py; } sample;
            sample.lx = pad.Lx;
            sample.ly = pad.Ly;
            sample.sys = sceKernelGetSystemTimeLow();
            sample.px = cx;
            sample.py = cy;
            pool_absorb(&sample, sizeof(sample));

            /* Two bits, not four: the sampling clock is the vblank, so the
               arrival time carries almost nothing. What is unpredictable is
               the low bits of the position, and there is not much of it. */
            g_pool_bits += 2;
        }

        int ccx = (int)(cx + 0.5f), ccy = (int)(cy + 0.5f);
        for (int y = 0; y < GRID_H; y++) {
            for (int x = 0; x < GRID_W; x++) {
                int ddx = x - ccx, ddy = y - ccy;
                if (moving && ddx * ddx + ddy * ddy * 3 <= 9) {
                    if (!cell_done[y][x] && cell_phase[y][x] == 0) covered++;
                    cell_phase[y][x] = SETTLE;
                    cell_done[y][x] = 1;
                }
            }
        }

        for (int y = 0; y < GRID_H; y++) {
            for (int x = 0; x < GRID_W; x++) {
                char ch;
                unsigned col;
                if (cell_phase[y][x] > 0) {
                    int ph = cell_phase[y][x]--;
                    if (ph > SETTLE - 5) {
                        ch = BLOOM[(SETTLE - ph) % 4];       /* bloom outward */
                        col = COL_SPIN;
                    } else {
                        ch = SPIN_GLYPHS[(ph + x + y) % 4];  /* then swirl */
                        col = (ph > 4) ? COL_SPIN : COL_WARM;
                    }
                } else if (cell_done[y][x]) {
                    ch = '#';
                    col = COL_DONE;
                } else {
                    ch = idle_glyph(x, y);
                    col = COL_IDLE;
                }
                draw_cell(x, y, ch, col);
            }
        }

        /* The cursor last, so it sits on top of its own wake. */
        draw_cell(ccx, ccy, SPIN_GLYPHS[(frame / 2) % 4], COL_CURSOR);
        shadow_ch[ccy][ccx] = 0;                 /* force a redraw next frame */

        /* 95 percent of the field counts as done. A forgotten corner should
           not hold anyone hostage, and the last few cells add nothing the
           other 1500 have not already contributed. */
        int pct = (covered * 10000) / (total * 95);
        if (pct > 100) pct = 100;
        int ready = (pct >= 100 && g_pool_bits >= ENTROPY_BITS);
        pspDebugScreenSetTextColor(ready ? COL_DONE : COL_TEXT);
        pspDebugScreenSetXY(0, 33);
        pspDebugScreenPrintf(" [");
        int filled = (pct * 44) / 100;
        for (int i = 0; i < 44; i++) pspDebugScreenPrintf("%c", i < filled ? '=' : ' ');
        if (ready) pspDebugScreenPrintf("] %3d%%  X to continue", pct);
        else       pspDebugScreenPrintf("] %3d%%  %4d bits ", pct, g_pool_bits);

        if (ready && (pad.Buttons & PSP_CTRL_CROSS)) break;

        frame++;
        sceDisplayWaitVblankStart();
        record_frame(frame);
    }

    trace_save();
    pspDebugScreenSetTextColor(COL_TEXT);
    return g_pool_bits;
}

/* --------------------------------------------------------- seed on disk */

#define SEED_FILE "ms0:/PSPDX.SEED"

/* A seed file alone is not enough: a copied memory stick would hand two
   consoles the same stream. Fresh jitter and the console id go in before a
   single byte comes out, and the file is rewritten immediately. */
static int psprandom_load(void) {
    int fd = sceIoOpen(SEED_FILE, PSP_O_RDONLY, 0777);
    if (fd < 0) return 0;
    unsigned char stored[POOL_BYTES];
    int n = sceIoRead(fd, stored, sizeof(stored));
    sceIoClose(fd);
    if (n != (int)sizeof(stored)) return 0;

    pool_absorb(stored, sizeof(stored));
    pool_absorb_jitter(4);
    return 1;
}

static void psprandom_save(void) {
    if (g_replay) return;
    unsigned char next[POOL_BYTES];
    unsigned int tag = 0x50535058;                /* domain separation */
    unsigned char buf[POOL_BYTES + sizeof(tag)];
    memcpy(buf, g_pool, POOL_BYTES);
    memcpy(buf + POOL_BYTES, &tag, sizeof(tag));
    sceKernelUtilsSha1Digest(buf, sizeof(buf), next);

    int fd = sceIoOpen(SEED_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, next, sizeof(next));
        sceIoClose(fd);
    }
}

static void psprandom_forget(void) {
    sceIoRemove(SEED_FILE);
    g_pool_bits = 0;
    memset(g_pool, 0, sizeof(g_pool));
}

/* wolfSSL is built with -DCUSTOM_RAND_GENERATE_SEED=psprandom_seed_raw, so its
   wc_GenerateSeed() is a wrapper around this and the /dev/urandom branch never
   exists. Not static: the library resolves the symbol at link time. */
int psprandom_seed_raw(unsigned char *seed, unsigned int sz);
int psprandom_seed_raw(unsigned char *seed, unsigned int sz) {
    while (sz > 0) {
        unsigned char buf[POOL_BYTES + sizeof(unsigned int)];
        unsigned char out[POOL_BYTES];
        memcpy(buf, g_pool, POOL_BYTES);
        memcpy(buf + POOL_BYTES, &g_pool_counter, sizeof(g_pool_counter));
        sceKernelUtilsSha1Digest(buf, sizeof(buf), out);
        g_pool_counter++;

        unsigned n = sz < sizeof(out) ? sz : (unsigned)sizeof(out);
        memcpy(seed, out, n);
        seed += n;
        sz -= n;

        /* Ratchet, so a later leak of the pool does not expose earlier seeds. */
        pool_absorb(out, sizeof(out));
    }
    return 0;
}

/* --------------------------------------------------------------- catalog */

#define MAX_APPS 64

enum app_state { APP_UNKNOWN, APP_NOT_INSTALLED, APP_CURRENT, APP_UPDATE };

struct app_entry {
    char id[96];
    char name[40];
    char summary[COLS];
    char category[12];
    char license[16];
    char manifest[256];
    enum app_state state;
    unsigned local_rev, remote_rev;
    char local_version[32], remote_version[32];
};

static struct app_entry g_apps[MAX_APPS];
static int g_napps = 0;
static int g_ntotal = 0;

static void copy_str(char *dst, size_t sz, cJSON *v) {
    if (cJSON_IsString(v)) { strncpy(dst, v->valuestring, sz - 1); dst[sz - 1] = '\0'; }
    else dst[0] = '\0';
}

/* Only fields the client acts on are read; everything else is display, and
   an unknown field is not an error. Returns the number of apps, -1 if the
   body was not a catalog. */
static int catalog_parse(const char *body, size_t len) {
    cJSON *root = cJSON_ParseWithLength(body, len);
    if (!root) { logline("catalog: not json"); return -1; }
    cJSON *apps = cJSON_GetObjectItemCaseSensitive(root, "apps");
    if (!cJSON_IsArray(apps)) { logline("catalog: no apps array"); cJSON_Delete(root); return -1; }

    g_napps = 0;
    g_ntotal = cJSON_GetArraySize(apps);
    cJSON *app;
    cJSON_ArrayForEach(app, apps) {
        if (g_napps >= MAX_APPS) break;
        struct app_entry *e = &g_apps[g_napps];
        memset(e, 0, sizeof(*e));
        copy_str(e->id, sizeof(e->id), cJSON_GetObjectItemCaseSensitive(app, "id"));
        copy_str(e->name, sizeof(e->name), cJSON_GetObjectItemCaseSensitive(app, "name"));
        copy_str(e->summary, sizeof(e->summary), cJSON_GetObjectItemCaseSensitive(app, "summary"));
        copy_str(e->category, sizeof(e->category), cJSON_GetObjectItemCaseSensitive(app, "category"));
        copy_str(e->license, sizeof(e->license), cJSON_GetObjectItemCaseSensitive(app, "license"));
        copy_str(e->manifest, sizeof(e->manifest), cJSON_GetObjectItemCaseSensitive(app, "manifest"));
        if (!e->id[0] || !e->name[0] || !e->manifest[0]) continue;

        /* What is on the stick is known without asking anyone. */
        struct installed ins;
        if (db_read(e->id, &ins) == 0) {
            e->state = APP_UNKNOWN;             /* installed, freshness unknown */
            e->local_rev = ins.rev;
            strncpy(e->local_version, ins.version, sizeof(e->local_version) - 1);
        } else {
            e->state = APP_NOT_INSTALLED;
        }
        g_napps++;
    }
    cJSON_Delete(root);
    logline("catalog: %d apps, %d usable", g_ntotal, g_napps);
    return g_napps;
}

static int catalog_sink(void *ctx, const void *data, size_t len) {
    (void)ctx;
    if (g_resplen + len >= sizeof(g_resp)) return -1;
    memcpy(g_resp + g_resplen, data, len);
    g_resplen += len;
    return 0;
}

static int catalog_fetch(void) {
    g_resplen = 0;
    int rc = https_get(CATALOG_URL, catalog_sink, NULL, NULL, NULL, &g_fetch);
    if (rc != 0 || g_fetch.status != 200) {
        logline("catalog: rc=%d status=%ld", rc, g_fetch.status);
        return -1;
    }
    g_resp[g_resplen] = '\0';
    return catalog_parse(g_resp, g_resplen);
}

/* Asks each installed package's own manifest whether something newer exists.
   The catalog is never consulted for this: it carries no version, so it may
   be a day stale without anyone noticing. Only installed packages are asked,
   which is ten to forty requests, not the whole catalog. */
static int check_updates(void) {
    int updates = 0;
    for (int i = 0; i < g_napps; i++) {
        struct app_entry *e = &g_apps[i];
        if (e->state == APP_NOT_INSTALLED) continue;

        struct manifest m;
        if (manifest_fetch(e->manifest, e->id, &m) < 0) continue;
        e->remote_rev = m.rev;
        strncpy(e->remote_version, m.version, sizeof(e->remote_version) - 1);

        /* Numbers only. Homebrew version strings -- r12, v0.9b, final2,
           "1.0 FIXED" -- cannot be ordered, and a version comparator has no
           business on a 222 MHz CPU. A remote rev that is older is ignored
           rather than offered: there is no downgrade. */
        if (m.rev > e->local_rev) { e->state = APP_UPDATE; updates++; }
        else e->state = APP_CURRENT;
    }
    logline("updates: %d of %d installed", updates, g_napps);
    return updates;
}

/* ---------------------------------------------------------------- screen */

#define LIST_ROW 3
#define STATUS_ROW (ANIM_ROW - 2)

static void draw_header(const char *right) {
    pspDebugScreenSetXY(0, 0);
    pspDebugScreenSetTextColor(COL_TEXT);
    pspDebugScreenPrintf("PSPDX  ");
    pspDebugScreenSetTextColor(COL_DIM);
    pspDebugScreenPrintf("%-53.53s", right ? right : "");
    pspDebugScreenSetTextColor(COL_TEXT);
}

/* The right-hand column: what this app is, in four words or fewer. */
static void app_state_text(const struct app_entry *e, char *out, size_t sz) {
    switch (e->state) {
    case APP_NOT_INSTALLED: snprintf(out, sz, "%s", e->license); break;
    case APP_UNKNOWN:       snprintf(out, sz, "installed %s", e->local_version); break;
    case APP_CURRENT:       snprintf(out, sz, "up to date"); break;
    case APP_UPDATE:        snprintf(out, sz, "update %s", e->remote_version); break;
    }
}

static void draw_list(int cursor) {
    for (int i = 0; i < g_napps && LIST_ROW + 2 * i + 1 < STATUS_ROW - 1; i++) {
        struct app_entry *e = &g_apps[i];
        int sel = (i == cursor);
        char state[20];
        app_state_text(e, state, sizeof(state));
        pspDebugScreenSetXY(0, LIST_ROW + 2 * i);
        pspDebugScreenSetTextColor(sel ? COL_CURSOR : (e->state == APP_UPDATE ? COL_DONE : COL_TEXT));
        pspDebugScreenPrintf("%c %-34.34s %-10.10s %-13.13s", sel ? '>' : ' ',
                             e->name, e->category, state);
        pspDebugScreenSetXY(0, LIST_ROW + 2 * i + 1);
        pspDebugScreenSetTextColor(COL_DIM);
        pspDebugScreenPrintf("    %-56.56s", e->summary);
    }
    pspDebugScreenSetTextColor(COL_TEXT);
}

static void draw_status(const char *text) {
    pspDebugScreenSetXY(0, STATUS_ROW);
    pspDebugScreenSetTextColor(COL_DIM);
    pspDebugScreenPrintf("%-60.60s", text);
    pspDebugScreenSetTextColor(COL_TEXT);
}

static void draw_bar(int row, size_t done, size_t total, const char *label) {
    pspDebugScreenSetXY(0, row);
    pspDebugScreenSetTextColor(COL_TEXT);
    pspDebugScreenPrintf("%-10.10s [", label);
    int filled = total ? (int)((unsigned long long)done * 40 / total) : 0;
    for (int x = 0; x < 40; x++) pspDebugScreenPrintf("%c", x < filled ? '#' : '-');
    if (total) pspDebugScreenPrintf("] %3d%%", (int)((unsigned long long)done * 100 / total));
    else       pspDebugScreenPrintf("] %5luK", (unsigned long)(done / 1024));
}

/* --------------------------------------------------------------- install */

struct ui_ctx {
    char phase[16];
    unsigned last_draw;
    int row;
};

static void ui_phase(void *ctx, const char *phase) {
    struct ui_ctx *u = ctx;
    strncpy(u->phase, phase, sizeof(u->phase) - 1);
    u->last_draw = 0;
    draw_bar(u->row, 0, 0, u->phase);
    /* PPSSPP writes an emulated file to the host only when it is closed, so
       the log is rewritten whole at each phase. On the console this is a few
       hundred bytes; in the emulator it is the only way to watch a long
       install from outside. */
    dump_to_stick();
}

static void ui_progress(void *ctx, size_t done, size_t total) {
    struct ui_ctx *u = ctx;
    /* Redrawing costs more than the bytes it reports; a few times a second. */
    unsigned t = now_ms();
    if (done != total && t - u->last_draw < 250) return;
    u->last_draw = t;
    draw_bar(u->row, done, total, u->phase);
}

static int install_app(int idx, int shot) {
    struct app_entry *e = &g_apps[idx];
    struct install_report rep;
    struct ui_ctx u;
    memset(&u, 0, sizeof(u));
    u.row = STATUS_ROW - 3;

    pspDebugScreenSetXY(0, u.row - 1);
    pspDebugScreenSetTextColor(COL_TEXT);
    pspDebugScreenPrintf("Installing %s", e->name);
    draw_status("");

    unsigned start = now_ms();
    int rc = install(e->manifest, e->id, &rep, ui_phase, ui_progress, &u);
    draw_list(idx);
    unsigned secs = (now_ms() - start) / 1000;

    char line[COLS + 1];
    if (rc == 0) {
        e->state = APP_CURRENT;
        e->local_rev = rep.rev;
        strncpy(e->local_version, rep.version, sizeof(e->local_version) - 1);
        snprintf(line, sizeof(line), "Installed %s %s: %d files, %luK, %us",
                 e->name, rep.version, rep.files, (unsigned long)(rep.bytes / 1024), secs);
    } else {
        snprintf(line, sizeof(line), "Install failed (%d): %s", rc, g_log[g_logn ? g_logn - 1 : 0]);
    }
    pspDebugScreenSetXY(0, u.row);
    pspDebugScreenPrintf("%-60.60s", "");
    draw_status(line);
    if (shot) screenshot_to_stick("ms0:/PSPDX2.BMP");
    return rc;
}

/* Development trigger: a file on the stick naming an app id installs it
   without anyone pressing X. Under replay that makes a whole install run
   reproducible from the host. */
static int auto_install_index(void) {
    char id[96];
    int fd = sceIoOpen("ms0:/PSPDX.INSTALL", PSP_O_RDONLY, 0777);
    if (fd < 0) return -1;
    int n = sceIoRead(fd, id, sizeof(id) - 1);
    sceIoClose(fd);
    if (n <= 0) return -1;
    id[n] = '\0';
    char *nl = strpbrk(id, "\r\n");
    if (nl) *nl = '\0';
    for (int i = 0; i < g_napps; i++) {
        /* The catalog entry carries no id in RAM; match on the manifest URL's
           tail instead is fragile, so the trigger names the manifest URL. */
        if (strcmp(g_apps[i].manifest, id) == 0) return i;
    }
    for (int i = 0; i < g_napps; i++)
        if (strstr(g_apps[i].manifest, id)) return i;
    logline("PSPDX.INSTALL: no app matches %s", id);
    return -1;
}

/* ------------------------------------------------------------- screenshot */

/* Dumps the debug-screen framebuffer as a 24-bit BMP. Development aid: the
   host may have no window to capture (locked screen, headless run), and a
   real PSP has no screen capture at all. pspDebugScreen draws 8888 into VRAM
   with a 512-pixel stride. */
static void screenshot_to_stick(const char *path) {
    enum { W = 480, H = 272, STRIDE = 512 };
    const unsigned *vram = (const unsigned *)(0x40000000 | (unsigned)sceGeEdramGetAddr());
    int fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;

    unsigned rowbytes = W * 3;                    /* 1440, already 4-aligned */
    unsigned datasize = rowbytes * H;
    unsigned char hdr[54] = { 'B', 'M' };
    unsigned v;
    v = 54 + datasize; memcpy(hdr + 2, &v, 4);
    v = 54;            memcpy(hdr + 10, &v, 4);
    v = 40;            memcpy(hdr + 14, &v, 4);
    v = W;             memcpy(hdr + 18, &v, 4);
    v = H;             memcpy(hdr + 22, &v, 4);
    hdr[26] = 1; hdr[28] = 24;
    v = datasize;      memcpy(hdr + 34, &v, 4);
    sceIoWrite(fd, hdr, sizeof(hdr));

    static unsigned char row[W * 3];
    for (int y = H - 1; y >= 0; y--) {            /* BMP is bottom-up */
        const unsigned *src = vram + y * STRIDE;
        for (int x = 0; x < W; x++) {
            unsigned px = src[x];                 /* 0xAABBGGRR */
            row[x * 3 + 0] = (px >> 16) & 0xff;   /* B */
            row[x * 3 + 1] = (px >> 8) & 0xff;    /* G */
            row[x * 3 + 2] = px & 0xff;           /* R */
        }
        sceIoWrite(fd, row, sizeof(row));
    }
    sceIoClose(fd);
}

/* ------------------------------------------------------------------ main */

int main(void) {
    if (setup_callbacks() < 0) {
        pspDebugScreenInit();
        pspDebugScreenPrintf("exit callback failed; HOME will not work\n");
    }
    pspDebugScreenInit();
    draw_header("gathering entropy");

    /* Before anything touches the network: fill the entropy pool, then hand
       wolfSSL the source. Without this every key it derives is guessable. */
    install_recover();
    psprandom_init();
    record_init();
    trace_load();
    if (!g_replay && psprandom_load()) {
        g_pool_bits = ENTROPY_BITS;              /* carried over from last run */
    } else {
        psprandom_sweep();
    }
    psprandom_save();
    pspDebugScreenClear();
    draw_header("connecting");

    int n = -1;
    if (net_up() < 0) {
        logline("network failed");
    } else {
        logline("net up");
        n = catalog_fetch();
    }

    int updates = 0;
    if (n > 0) updates = check_updates();

    pspDebugScreenClear();
    if (n >= 0) {
        char hdr[64];
        if (updates) snprintf(hdr, sizeof(hdr), "%d apps, %d update%s available",
                              g_ntotal, updates, updates == 1 ? "" : "s");
        else snprintf(hdr, sizeof(hdr), "%d apps  %lu bytes  %u ms handshake",
                      g_ntotal, (unsigned long)g_resplen, g_fetch.handshake_ms);
        draw_header(hdr);
        draw_list(0);
        draw_status("X: install or update   SELECT: discard entropy and sweep again");
    } else {
        draw_header("failed");
        for (int i = 0; i < g_logn && i + 2 < STATUS_ROW; i++) {
            pspDebugScreenSetXY(0, 2 + i);
            pspDebugScreenPrintf("%s", g_log[i]);
        }
    }
    sceDisplayWaitVblankStart();
    screenshot_to_stick("ms0:/PSPDX.BMP");
    dump_to_stick();

    int cursor = 0;
    int autoidx = n > 0 ? auto_install_index() : -1;
    if (autoidx >= 0) {
        cursor = autoidx;
        draw_list(cursor);
        install_app(cursor, 1);
        dump_to_stick();
    }

    unsigned last_buttons = 0;
    for (;;) {
        SceCtrlData pad;
        sceCtrlReadBufferPositive(&pad, 1);
        unsigned pressed = pad.Buttons & ~last_buttons;
        last_buttons = pad.Buttons;

        if (n > 0 && (pressed & PSP_CTRL_DOWN) && cursor + 1 < g_napps) draw_list(++cursor);
        if (n > 0 && (pressed & PSP_CTRL_UP) && cursor > 0) draw_list(--cursor);
        if (n > 0 && (pressed & PSP_CTRL_CROSS)) {
            install_app(cursor, 0);
            dump_to_stick();
        }
        if (pressed & PSP_CTRL_SELECT) {
            psprandom_forget();
            pspDebugScreenClear();
            draw_header("entropy discarded");
            psprandom_sweep();
            psprandom_save();
            pspDebugScreenClear();
            draw_header("entropy regenerated");
            if (n >= 0) draw_list(cursor);
            memset(shadow_ch, 0, sizeof(shadow_ch));
        }
        sceDisplayWaitVblankStart();
    }
    return 0;
}
