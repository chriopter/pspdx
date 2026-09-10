/*
 * What is on the card, and the thread that fetches it. The main thread
 * only ever hands over an entry and picks up finished pictures; every
 * blocking step -- reading the stick, a TLS fetch, decoding the PNG,
 * wrapping the film, starting the decoder -- runs on the media thread
 * below the interface, so a frame never waits for any of it. The decoder
 * itself has its own thread under video/player.c; this one starts and stops
 * it.
 */

#include <pspkernel.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui/preview.h"
#include "gui/icons.h"
#include "gui/image.h"
#include "update/assets.h"
#include "util/runtime.h"
#include "video/mp4.h"
#include "video/player.h"
#include "video/psmf.h"

#define SETTLE_MS 250
#define FILM_W 480
#define FILM_H 272
#define FILM_STRIDE 512

/* Below the decoder's 0x23: the wrap is a big memcpy and can wait for the
   pictures already in flight. */
#define MEDIA_PRIORITY 0x24
#define MEDIA_STACK (64 * 1024)

enum still_state { STILL_NONE, STILL_LOADING, STILL_READY, STILL_FAILED };
enum film_state { FILM_NONE, FILM_LOADING, FILM_PLAYING, FILM_FAILED };

/* What the main thread asked for. gen goes up with every new entry; work
   for an older gen is dropped on the floor when it finishes. */
static struct {
    char id[96], shot_url[256], video_url[256];
    volatile unsigned gen;
} g_want;
static unsigned g_shown_gen;            /* gen of what the pictures belong to */
static unsigned g_shown_ms;
static int g_immediate;

/* Two still slots: the thread fills the one not on screen, then the
   pointer moves. The GE may still be reading the old one this frame; it
   is not written again before the next request, frames later. */
static struct gfx_texture g_stills[2];
static int g_still_slot;
static const struct gfx_texture *volatile g_still_pub;
static volatile enum still_state g_still_state;
static float g_still_alpha;

static struct gfx_texture g_film;
static void *g_film_buf[2];             /* one is drawn while the other fills */
static volatile enum film_state g_film_state;
static float g_film_alpha;
static unsigned char *g_psmf;
static struct mp4 g_track;
static int g_decoded;                   /* pictures since the film started */

static SceUID g_thread = -1, g_wake = -1, g_idle = -1;
static volatile int g_quit, g_hold;
static volatile unsigned g_done_gen;    /* gen the thread has finished with */

/* ------------------------------------------------------------ the thread */

static int stale(unsigned gen) { return gen != g_want.gen || g_quit || g_hold; }

static void load_still(unsigned gen, struct gfx_texture *into) {
    size_t len = 0;
    const void *png = asset_fetch(ASSET_SHOT, g_want.id, g_want.shot_url, &len);
    if (stale(gen)) return;
    if (!png || image_decode_png(png, len, into) != 0) {
        g_still_state = STILL_FAILED;
        return;
    }
    g_still_pub = into;
    g_still_state = STILL_READY;
}

static void load_film(unsigned gen) {
    size_t len = 0;
    const unsigned char *mp4 = asset_fetch(ASSET_VIDEO, g_want.id, g_want.video_url, &len);
    if (stale(gen)) return;
    if (!mp4) { g_film_state = FILM_FAILED; return; }
    if (mp4_parse(mp4, len, &g_track) != 0) {
        logline("film: not a video track the PSP can play");
        g_film_state = FILM_FAILED;
        return;
    }
    if (g_track.width != FILM_W || g_track.height != FILM_H) {
        logline("film: %dx%d, wanted %dx%d", g_track.width, g_track.height, FILM_W, FILM_H);
        g_film_state = FILM_FAILED;
        return;
    }
    size_t cap = psmf_capacity(len);
    unsigned char *psmf = malloc(cap);
    if (!psmf) { logline("film: no room for %lu", (unsigned long)cap); g_film_state = FILM_FAILED; return; }
    size_t n = psmf_build(mp4, &g_track, psmf, cap);
    if (!n || stale(gen)) { free(psmf); if (!n) { logline("film: could not wrap"); g_film_state = FILM_FAILED; } return; }
    logline("film: %d pictures, %lu KB wrapped", g_track.count, (unsigned long)(n / 1024));
    g_psmf = psmf;
    if (player_start(g_psmf, n, g_film_buf[0], g_film_buf[1], FILM_STRIDE) != 0) {
        free(g_psmf);
        g_psmf = 0;
        g_film_state = FILM_FAILED;
        return;
    }
    g_film_state = FILM_PLAYING;
}

/* The list's icons come after the card: one at a time, and only while no
   newer selection is waiting, so a scroll through the list is never held
   up by the icons of the rows it left. */
static void load_icons(unsigned gen) {
    int index;
    while (!stale(gen) && (index = icons_pending()) >= 0) icons_load(index);
}

/* One request at a time, the newest. Between requests the decoder is
   stopped and the last film let go of, so nothing here ever runs two
   films or two fetches at once. */
static int media_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    unsigned served = 0;
    for (;;) {
        sceKernelWaitSema(g_wake, 1, 0);
        if (g_quit) break;
        if (g_hold) { sceKernelSignalSema(g_idle, 1); continue; }
        unsigned gen = g_want.gen;
        if (gen == served) { load_icons(gen); continue; }
        served = gen;

        player_stop();
        free(g_psmf);
        g_psmf = 0;

        int slot = g_still_slot ^ 1;
        gfx_texture_free(&g_stills[slot]);
        g_still_state = STILL_LOADING;
        load_still(gen, &g_stills[slot]);
        if (stale(gen)) { gfx_texture_free(&g_stills[slot]); continue; }
        g_still_slot = slot;

        /* The cache is tried even without a link, so this is asked of
           every entry; it comes back at once when there is nothing. */
        if (g_film_buf[0]) {
            g_film_state = FILM_LOADING;
            load_film(gen);
        } else {
            g_film_state = FILM_FAILED;
        }
        if (gen == g_want.gen) g_done_gen = gen;
        else if (g_film_state == FILM_PLAYING) { player_stop(); free(g_psmf); g_psmf = 0; g_film_state = FILM_NONE; }
        load_icons(gen);
    }
    player_stop();
    free(g_psmf);
    g_psmf = 0;
    sceKernelSignalSema(g_idle, 1);
    return 0;
}

static void wake(void) { if (g_wake >= 0) sceKernelSignalSema(g_wake, 1); }

/* ----------------------------------------------------------- the interface */

void preview_init(void) {
    memset(g_stills, 0, sizeof(g_stills));
    g_still_pub = 0;
    g_still_state = STILL_NONE;
    g_want.id[0] = '\0';
    g_want.gen = 0;
    g_shown_gen = 0;
    g_done_gen = 0;
    g_quit = g_hold = 0;
    /* The film decodes into two textures that live for the whole run: the
       decoder writes one while the GE reads the other, and a picture
       changes hands as a pointer. The CPU never touches either. */
    g_film.w = FILM_W; g_film.h = FILM_H;
    g_film.tw = FILM_STRIDE; g_film.th = 512;
    g_film.opaque = 1;
    for (int i = 0; i < 2; i++) {
        g_film_buf[i] = memalign(16, (size_t)FILM_STRIDE * 512 * 4);
        if (g_film_buf[i]) memset(g_film_buf[i], 0, (size_t)FILM_STRIDE * 512 * 4);
    }
    if (g_film_buf[0] && g_film_buf[1]) {
        g_film.pixels = g_film_buf[0];
        sceKernelDcacheWritebackInvalidateAll();
    } else {
        free(g_film_buf[0]); free(g_film_buf[1]);
        g_film_buf[0] = g_film_buf[1] = 0;
    }
    g_wake = sceKernelCreateSema("media_wake", 0, 0, 64, 0);
    g_idle = sceKernelCreateSema("media_idle", 0, 0, 64, 0);
    g_thread = sceKernelCreateThread("media", media_thread, MEDIA_PRIORITY, MEDIA_STACK,
                                     PSP_THREAD_ATTR_USER, 0);
    if (g_thread >= 0) sceKernelStartThread(g_thread, 0, 0);
    else logline("media: no thread %08x", (unsigned)g_thread);
}

void preview_shutdown(void) {
    if (g_thread >= 0) {
        g_quit = 1;
        wake();
        sceKernelWaitSema(g_idle, 1, 0);
        sceKernelWaitThreadEnd(g_thread, 0);
        sceKernelDeleteThread(g_thread);
        g_thread = -1;
    }
    if (g_wake >= 0) { sceKernelDeleteSema(g_wake); g_wake = -1; }
    if (g_idle >= 0) { sceKernelDeleteSema(g_idle); g_idle = -1; }
    gfx_texture_free(&g_stills[0]);
    gfx_texture_free(&g_stills[1]);
    g_still_pub = 0;
    free(g_film_buf[0]);
    free(g_film_buf[1]);
    g_film_buf[0] = g_film_buf[1] = 0;
    memset(&g_film, 0, sizeof(g_film));
    g_still_state = STILL_NONE;
    g_film_state = FILM_NONE;
    g_want.id[0] = '\0';
}

void preview_quiesce(void) {
    if (g_thread < 0) return;
    g_hold = 1;
    wake();
    sceKernelWaitSema(g_idle, 1, 0);
}

void preview_resume(void) {
    g_hold = 0;
    wake();
}

void preview_show(const struct app_entry *entry, int immediately) {
    if (strcmp(g_want.id, entry->id) == 0) return;
    snprintf(g_want.id, sizeof(g_want.id), "%s", entry->id);
    snprintf(g_want.shot_url, sizeof(g_want.shot_url), "%s", entry->screenshot);
    snprintf(g_want.video_url, sizeof(g_want.video_url), "%s", entry->video);
    /* Nothing of the last entry stays on the card: the pictures may still
       exist, they are just not drawn until the thread has this one. */
    g_still_pub = 0;
    g_still_state = STILL_NONE;
    g_still_alpha = 0.0f;
    g_film_state = FILM_NONE;
    g_film_alpha = 0.0f;
    g_decoded = 0;
    g_shown_ms = now_ms();
    g_immediate = immediately;
    g_shown_gen = 0;
}

int preview_tick(void) {
    /* The request goes out once the cursor has rested; the thread does
       the rest and this only picks up what it has finished. */
    if (g_shown_gen == 0) {
        if (!g_immediate && !expired(g_shown_ms, SETTLE_MS)) return 0;
        g_shown_gen = ++g_want.gen;
        wake();
        return 0;
    }
    if (g_still_state == STILL_READY && g_still_pub)
        g_still_alpha += (1.0f - g_still_alpha) * 0.12f;

    if (g_film_state == FILM_PLAYING) {
        void *picture = player_take();
        if (picture) { g_film.pixels = picture; g_decoded++; }
        if (g_decoded > 0) g_film_alpha += (1.0f - g_film_alpha) * 0.08f;
        if (player_failed()) g_film_state = FILM_FAILED;
    }
    return 0;
}

void preview_load(void) {}

void preview_poke(void) { wake(); }

const struct gfx_texture *preview_still(int *alpha) {
    const struct gfx_texture *t = g_still_pub;
    if (g_still_state != STILL_READY || !t) { *alpha = 0; return 0; }
    *alpha = (int)(g_still_alpha * 255.0f);
    return t;
}

const struct gfx_texture *preview_film(int *alpha) {
    if (g_film_state != FILM_PLAYING || g_decoded == 0) { *alpha = 0; return 0; }
    *alpha = (int)(g_film_alpha * 255.0f);
    return &g_film;
}

int preview_playing(void) {
    return g_film_state == FILM_PLAYING && g_decoded > 0;
}

enum preview_state preview_state(void) {
    if (g_still_state == STILL_READY || g_film_state == FILM_PLAYING) return PREVIEW_SHOWING;
    if (g_still_state == STILL_FAILED) return PREVIEW_MISSING;
    if (g_shown_gen != 0) return PREVIEW_LOADING;
    return PREVIEW_EMPTY;
}

int preview_settled(void) {
    if (g_shown_gen == 0 || g_done_gen != g_shown_gen) return 0;
    int still = g_still_state == STILL_FAILED ||
                (g_still_state == STILL_READY && g_still_alpha > 0.98f);
    int film = g_film_state == FILM_FAILED || g_film_state == FILM_NONE ||
               (g_film_state == FILM_PLAYING && g_decoded > 0 && g_film_alpha > 0.98f);
    return still && film;
}
