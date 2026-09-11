/* PSPDX application controller. Feature code lives behind module APIs. */

#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <psppower.h>
#include <stdio.h>
#include <string.h>

#include "audio/audio.h"
#include "audio/cues.h"
#include "gui/entropy_screen.h"
#include "gui/gfx.h"
#include "gui/preview.h"
#include "gui/screen.h"
#include "gui/shell.h"
#include "install/install.h"
#include "logic/entropy.h"
#include "network/bench.h"
#include "update/catalog.h"
#include "update/sync.h"
#include "util/runtime.h"

PSP_MODULE_INFO("pspdx", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
/* One screenshot decodes into a megabyte of texture and wolfSSL wants its own
   working set; four megabytes no longer covers both. */
PSP_HEAP_SIZE_KB(12 * 1024);

static struct catalog catalog;
static unsigned g_worst_tick;       /* worst preview tick (fetch, decode) in us */

static int exit_callback(int a, int b, void *c) {
    (void)a; (void)b; (void)c;
    /* The one orderly moment in a run. Everything the session stirred into the
       pool has been sitting in RAM until here, so this is where it reaches the
       stick -- twenty bytes, once, instead of the same sector every few
       seconds. */
    entropy_save(entropy_screen_is_replay());
    sceKernelExitGame();
    return 0;
}

static int callback_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int callback = sceKernelCreateCallback("Exit", exit_callback, NULL);
    if (callback >= 0) sceKernelRegisterExitCallback(callback);
    sceKernelSleepThreadCB();
    return 0;
}

static int setup_callbacks(void) {
    int thread = sceKernelCreateThread("exit_thread", callback_thread,
                                       0x11, 0xFA0, THREAD_ATTR_USER, 0);
    if (thread < 0) return thread;
    return sceKernelStartThread(thread, 0, 0);
}

/* The log every time; the catalog's raw response once, after it arrived
   -- it is 200 KB and does not change, and writing it every ten seconds
   was a visible hitch. */
static int g_http_dumped;

static void dump_diagnostics(void) {
    log_dump();
    if (!g_http_dumped && sync_done()) {
        catalog_dump_http();
        g_http_dumped = 1;
    }
}

/* Until the catalog is here the browser has nothing to browse; it is on
   screen anyway, saying what it waits for. */
static struct catalog empty;

static const struct catalog *shown(void) {
    return sync_done() ? &catalog : &empty;
}

/* Let the transitions finish before photographing the screen, but not
   forever: six seconds covers a film being fetched and started. */
static void screenshot_settled(int cursor, const char *path) {
    /* At least one frame, so the shell has seen the catalog it is about
       to be judged on. */
    for (int i = 0; i < 360; i++) {
        shell_shot_sync(shown(), cursor);
        shell_draw(shown(), cursor);
        if (shell_settled()) break;
    }
    gfx_screenshot(path);
}

static int install_app(int index, int screenshot) {
    struct app_entry *entry = &catalog.apps[index];
    struct install_report report;

    shell_install_begin(entry->name);
    cues_post(CUE_OPEN, 0);
    /* The installer and the media thread share one HTTPS stack and one
       asset buffer; only one of them talks to the network at a time. */
    preview_quiesce();
    /* The handshake and the checksum run flat out on this thread, and the
       audio thread sits one step under it by design -- see audio.c -- so
       for the length of the install this thread steps under the audio
       thread instead. The tune keeps playing; the progress bar, drawn from
       the installer's callbacks, gets what is left, which is nearly all. */
    SceUID self = sceKernelGetThreadId();
    sceKernelChangeThreadPriority(self, 0x22);
    unsigned start = now_ms();
    int rc = entry->has_release
        ? install_release(&entry->release, &report, shell_install_phase,
                          shell_install_progress, NULL)
        : install(entry->manifest, entry->id, &report, shell_install_phase,
                  shell_install_progress, NULL);
    unsigned seconds = (now_ms() - start) / 1000;
    sceKernelChangeThreadPriority(self, 0x20);
    preview_resume();

    char message[96];
    if (rc == 0) {
        entry->state = APP_CURRENT;
        entry->local_rev = report.rev;
        strncpy(entry->local_version, report.version, sizeof(entry->local_version) - 1);
        entry->local_version[sizeof(entry->local_version) - 1] = '\0';
        snprintf(message, sizeof(message), "Installed %s %s: %d files, %luK, %us",
                 entry->name, report.version, report.files,
                 (unsigned long)(report.bytes / 1024), seconds);
    } else {
        snprintf(message, sizeof(message), "Install failed (%d): %s",
                 rc, log_at(log_count() - 1));
    }
    shell_install_end(message);
    cues_post(rc == 0 ? CUE_DONE : CUE_FAIL, 0);
    shell_draw(&catalog, index);
    if (screenshot) screenshot_settled(index, "ms0:/PSPDX2.BMP");
    return rc;
}

static int auto_install_index(void) {
    char id[96];
    int fd = sceIoOpen("ms0:/PSPDX.INSTALL", PSP_O_RDONLY, 0777);
    if (fd < 0) return -1;
    int n = sceIoRead(fd, id, sizeof(id) - 1);
    sceIoClose(fd);
    if (n <= 0) return -1;
    id[n] = '\0';
    char *newline = strpbrk(id, "\r\n");
    if (newline) *newline = '\0';
    for (int i = 0; i < catalog.count; i++)
        if (strcmp(catalog.apps[i].id, id) == 0 || strcmp(catalog.apps[i].manifest, id) == 0) return i;
    for (int i = 0; i < catalog.count; i++)
        if (strstr(catalog.apps[i].id, id) || strstr(catalog.apps[i].manifest, id)) return i;
    logline("PSPDX.INSTALL: no app matches %s", id);
    return -1;
}

/* Scripted input for the test rig: PSPDX.KEYS on the stick holds lines
   of "<ms> <button>", pressed at that many milliseconds after the catalog
   arrived. How the browser gets driven hard without a hand on it. */
static struct { unsigned at; unsigned button; } g_keys[256];
static int g_key_count, g_key_next;
static unsigned g_keys_since;

/* Not a PSP button: a scripted "shot" takes a settled screenshot of what
   the earlier keys led to, into PSPDX1.BMP. */
#define KEY_SHOT 0x80000000u

static unsigned button_named(const char *name) {
    if (strcmp(name, "shot") == 0) return KEY_SHOT;
    if (strcmp(name, "up") == 0) return PSP_CTRL_UP;
    if (strcmp(name, "down") == 0) return PSP_CTRL_DOWN;
    if (strcmp(name, "cross") == 0) return PSP_CTRL_CROSS;
    if (strcmp(name, "select") == 0) return PSP_CTRL_SELECT;
    return 0;
}

static void keys_load(void) {
    static char text[4096];
    int fd = sceIoOpen("ms0:/PSPDX.KEYS", PSP_O_RDONLY, 0777);
    if (fd < 0) return;
    int n = sceIoRead(fd, text, sizeof(text) - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    text[n] = '\0';
    char *line = text;
    while (line && *line && g_key_count < 256) {
        char *end = strpbrk(line, "\r\n");
        if (end) *end++ = '\0';
        unsigned at = 0;
        char name[16] = "";
        if (sscanf(line, "%u %15s", &at, name) == 2) {
            g_keys[g_key_count].at = at;
            g_keys[g_key_count].button = button_named(name);
            g_key_count++;
        }
        line = end;
        while (line && (*line == '\r' || *line == '\n')) line++;
    }
    logline("keys: %d scripted", g_key_count);
}

/* A direction held down scrolls: after a third of a second it repeats,
   slowly at first and then, as it is held, at a rate that gets through a
   long list -- twenty-five rows a second -- without ever skipping one. */
static unsigned repeat(unsigned held) {
    static unsigned was, since, fired;
    unsigned now = now_ms();
    if (held != was) { was = held; since = now; fired = 0; return 0; }
    if (!held || now - since < 330) return 0;
    unsigned along = now - since - 330;
    unsigned interval = along < 800 ? 110 : along < 2000 ? 65 : 40;
    if (now - fired < interval) return 0;
    fired = now;
    return held;
}

static unsigned keys_pressed(void) {
    unsigned pressed = 0;
    while (g_key_next < g_key_count && now_ms() - g_keys_since >= g_keys[g_key_next].at)
        pressed |= g_keys[g_key_next++].button;
    return pressed;
}

int main(void) {
    /* Full speed: the film decodes and the piano plays on the same CPU
       the interface draws with. The default is two thirds of it. */
    scePowerSetClockFrequency(333, 333, 166);
    if (setup_callbacks() < 0) {
        gui_init();
        pspDebugScreenPrintf("exit callback failed; HOME will not work\n");
    }

    install_recover();
    entropy_init();
    entropy_screen_prepare();
    int sweep = entropy_screen_is_replay() || !entropy_load();

    /* The sweep is drawn on the water the browser then stands on, so the GE
       and the font come up before it rather than after. */
    if (!shell_init()) {
        /* No system font to browse with. The log says so, and the log is
           what gets shown. */
        gui_init();
        gui_clear();
        gui_failure();
        sceDisplayWaitVblankStart();
        gfx_screenshot("ms0:/PSPDX.BMP");
        dump_diagnostics();
        for (;;) sceDisplayWaitVblankStart();
    }

    if (sweep) {
        unsigned since = now_ms();
        int bits = entropy_screen_run();
        logline("entropy: %d bits swept in %u ms%s", bits, now_ms() - since,
                entropy_screen_is_replay() ? " (replay)" : "");
    }
    entropy_save(entropy_screen_is_replay());

    /* The tune starts with the shell and keeps going through installs and
       sweeps; it lives on its own thread and never waits for a frame. */
    audio_start();
    sync_start(&catalog);

    /* Connect, fetch and check in the background while the first frames
       go up; the status line follows along. */
    int cursor = 0;
    int synced = 0;
    int rounds = 0;                     /* times the sync has come back */
    int automatic = -1;
    unsigned shell_since = now_ms();
    int shot_connecting = 0;
    unsigned dumped_ms = now_ms();
    unsigned last_buttons = 0;
    /* Frame times, so a slow frame is a number and not a feeling: every
       ten seconds the average, the worst, and how many missed 60 Hz. */
    unsigned frame_us = now_us(), frames = 0, worst = 0, late = 0, total = 0;
    unsigned bucket[4] = { 0, 0, 0, 0 };    /* 17-20, 20-25, 25-35, >35 ms */
    for (;;) {
        unsigned now = now_us(), took = now - frame_us;
        frame_us = now;
        frames++; total += took / 1000;
        if (took > worst) worst = took;
        if (took > 100000)
            logline("frame %u: %u ms, %u ms since the shell came up",
                    gfx_frames(), took / 1000, now_ms() - shell_since);
        if (took > 17000) late++;
        if (took > 35000) bucket[3]++;
        else if (took > 25000) bucket[2]++;
        else if (took > 20000) bucket[1]++;
        else if (took > 17000) bucket[0]++;
        /* The emulator only flushes a file on close, and a long session
           should still leave a log behind: once every ten seconds. */
        if (expired(dumped_ms, 10000)) {
            dumped_ms = now_ms();
            logline("frames: %u in 10 s, avg %u ms, worst %u ms, %u late "
                    "(17-20 %u, 20-25 %u, 25-35 %u, 35+ %u)",
                    frames, frames ? total / frames : 0, worst / 1000, late,
                    bucket[0], bucket[1], bucket[2], bucket[3]);
            char phases[120];
            shell_profile(phases, sizeof(phases));
            logline("%s", phases);
            logline("outside draw: tick %u us, audio callback %u us, free %u KB",
                    g_worst_tick, audio_worst_us(),
                    (unsigned)sceKernelTotalFreeMemSize() / 1024);
            g_worst_tick = 0;
            frames = worst = late = total = 0;
            bucket[0] = bucket[1] = bucket[2] = bucket[3] = 0;
            log_dump_later();
        }

        if (!synced) {
            shell_status(sync_message());
            /* The connecting screen, for the rig: a second and a half in,
               while there is still something to connect to. */
            if (!shot_connecting && expired(shell_since, 1500)) {
                shot_connecting = 1;
                gfx_screenshot("ms0:/PSPDX0.BMP");
            }
            if (sync_done()) {
                synced = 1;
                if (sync_state() == SYNC_DONE) shell_status("");
                else {
                    /* Nothing came: say so, and offer the one thing that
                       can be done about it. */
                    char again[96];
                    snprintf(again, sizeof(again), "%s   X to try again", sync_message());
                    shell_word("Offline");
                    shell_status(again);
                }
                dump_diagnostics();
                /* The rig's hooks, once: a retry that comes through does
                   not get to install or replay the keys a second time. */
                if (rounds++ > 0) continue;
                screenshot_settled(cursor, "ms0:/PSPDX.BMP");
                dump_diagnostics();
                automatic = catalog.count > 0 ? auto_install_index() : -1;
                if (automatic >= 0) {
                    cursor = automatic;
                    install_app(cursor, 1);
                    dump_diagnostics();
                }
                keys_load();
                g_keys_since = now_ms();
                int bench = sceIoOpen("ms0:/PSPDX.BENCH", PSP_O_RDONLY, 0777);
                if (bench >= 0) {
                    sceIoClose(bench);
                    shell_status("benchmarking ciphers");
                    shell_draw(shown(), cursor);
                    preview_quiesce();
                    bench_run(catalog_url());
                    preview_resume();
                    shell_status("");
                    dump_diagnostics();
                }
            }
        }

        SceCtrlData pad;
        sceCtrlReadBufferPositive(&pad, 1);
        unsigned pressed = pad.Buttons & ~last_buttons;
        last_buttons = pad.Buttons;
        pressed |= repeat(pad.Buttons & (PSP_CTRL_UP | PSP_CTRL_DOWN));
        if (synced) pressed |= keys_pressed();
        int count = shown()->count;

        /* The list is a ring: past the last entry comes the first. */
        if ((pressed & PSP_CTRL_DOWN) && count > 0)
            cues_post(CUE_MOVE, cursor = (cursor + 1) % count);
        if ((pressed & PSP_CTRL_UP) && count > 0)
            cues_post(CUE_MOVE, cursor = (cursor + count - 1) % count);
        if (pressed & KEY_SHOT) {
            screenshot_settled(cursor, "ms0:/PSPDX1.BMP");
            logline("shot: PSPDX1.BMP at cursor %d", cursor);
        }
        if ((pressed & PSP_CTRL_CROSS) && count > 0) {
            install_app(cursor, 0);
            dump_diagnostics();
        } else if ((pressed & PSP_CTRL_CROSS) && sync_state() == SYNC_FAILED) {
            /* Once more from the top: the wait comes back with its word,
               and the frames below carry on as they did the first time. */
            shell_word("Connecting");
            if (sync_start(&catalog) == 0) synced = 0;
        }
        if (pressed & PSP_CTRL_SELECT) {
            /* The field drains and is swept again, in the room the browser
               was already standing in. */
            entropy_forget();
            entropy_init();
            entropy_screen_run();
            entropy_save(entropy_screen_is_replay());
        }

        unsigned tick0 = now_us();
        shell_shot_sync(shown(), cursor);
        audio_duck(preview_playing());
        unsigned tick = now_us() - tick0;
        if (tick > g_worst_tick) g_worst_tick = tick;
        shell_draw(shown(), cursor);
    }
    return 0;
}
