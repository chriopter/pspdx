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
    unsigned start = now_ms();
    int rc = install(entry->manifest, entry->id, &report,
                     shell_install_phase, shell_install_progress, NULL);
    unsigned seconds = (now_ms() - start) / 1000;
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
        if (strcmp(catalog.apps[i].manifest, id) == 0) return i;
    for (int i = 0; i < catalog.count; i++)
        if (strstr(catalog.apps[i].manifest, id)) return i;
    logline("PSPDX.INSTALL: no app matches %s", id);
    return -1;
}

/* Scripted input for the test rig: PSPDX.KEYS on the stick holds lines
   of "<ms> <button>", pressed at that many milliseconds after the catalog
   arrived. How the browser gets driven hard without a hand on it. */
static struct { unsigned at; unsigned button; } g_keys[256];
static int g_key_count, g_key_next;
static unsigned g_keys_since;

static unsigned button_named(const char *name) {
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

static unsigned keys_pressed(void) {
    unsigned pressed = 0;
    while (g_key_next < g_key_count && now_ms() - g_keys_since >= g_keys[g_key_next].at)
        pressed |= g_keys[g_key_next++].button;
    return pressed;
}

/* The sweep belongs on the debug screen: a grid of text cells. The shell
   takes the display right after, before any network. */
static void run_entropy(void) {
    gui_init();
    gui_header("gathering entropy");
    install_recover();
    entropy_init();
    entropy_screen_prepare();
    if (entropy_screen_is_replay() || !entropy_load()) entropy_screen_run();
    entropy_save(entropy_screen_is_replay());
}

int main(void) {
    /* Full speed: the film decodes and the piano plays on the same CPU
       the interface draws with. The default is two thirds of it. */
    scePowerSetClockFrequency(333, 333, 166);
    if (setup_callbacks() < 0) {
        gui_init();
        pspDebugScreenPrintf("exit callback failed; HOME will not work\n");
    }

    run_entropy();

    if (!shell_init()) {
        /* No system font to browse with. The log says so, and the log is
           what gets shown. */
        gui_clear();
        gui_failure();
        sceDisplayWaitVblankStart();
        gfx_screenshot("ms0:/PSPDX.BMP");
        dump_diagnostics();
        for (;;) sceDisplayWaitVblankStart();
    }

    /* The tune starts with the shell and keeps going through installs and
       sweeps; it lives on its own thread and never waits for a frame. */
    audio_start();
    sync_start(&catalog);

    /* Connect, fetch and check in the background while the first frames
       go up; the status line follows along. */
    int cursor = 0;
    int synced = 0;
    int automatic = -1;
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
            char phases[80];
            shell_profile(phases, sizeof(phases));
            logline("%s", phases);
            logline("outside draw: tick %u us, audio callback %u us", g_worst_tick,
                    audio_worst_us());
            g_worst_tick = 0;
            frames = worst = late = total = 0;
            bucket[0] = bucket[1] = bucket[2] = bucket[3] = 0;
            log_dump_later();
        }

        if (!synced) {
            shell_status(sync_message());
            if (sync_done()) {
                synced = 1;
                if (sync_state() == SYNC_DONE) shell_status("");
                dump_diagnostics();
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
            }
        }

        SceCtrlData pad;
        sceCtrlReadBufferPositive(&pad, 1);
        unsigned pressed = pad.Buttons & ~last_buttons;
        last_buttons = pad.Buttons;
        if (synced) pressed |= keys_pressed();
        int count = shown()->count;

        if ((pressed & PSP_CTRL_DOWN) && cursor + 1 < count)
            cues_post(CUE_MOVE, ++cursor);
        if ((pressed & PSP_CTRL_UP) && cursor > 0)
            cues_post(CUE_MOVE, --cursor);
        if ((pressed & PSP_CTRL_CROSS) && count > 0) {
            install_app(cursor, 0);
            dump_diagnostics();
        }
        if (pressed & PSP_CTRL_SELECT) {
            /* Back to the debug screen for the sweep, then hand the display
               to the shell again. */
            shell_shutdown();
            gui_init();
            gui_clear();
            gui_header("entropy discarded");
            entropy_forget();
            entropy_init();
            entropy_screen_run();
            entropy_save(entropy_screen_is_replay());
            entropy_screen_reset_cache();
            if (!shell_init()) return 0;
        }

        unsigned tick0 = now_us();
        shell_shot_sync(shown(), cursor);
        unsigned tick = now_us() - tick0;
        if (tick > g_worst_tick) g_worst_tick = tick;
        shell_draw(shown(), cursor);
    }
    return 0;
}
