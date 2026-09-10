/* PSPDX application controller. Feature code lives behind module APIs. */

#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>

#include "gui/entropy_screen.h"
#include "gui/gfx.h"
#include "gui/screen.h"
#include "gui/shell.h"
#include "install/install.h"
#include "logic/entropy.h"
#include "network/https.h"
#include "update/catalog.h"
#include "util/runtime.h"

PSP_MODULE_INFO("pspdx", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
/* One screenshot decodes into a megabyte of texture and wolfSSL wants its own
   working set; four megabytes no longer covers both. */
PSP_HEAP_SIZE_KB(12 * 1024);

static struct catalog catalog;

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

static void dump_diagnostics(void) {
    log_dump();
    catalog_dump_http();
}

/* Let the transitions finish before photographing the screen, but not
   forever: two seconds is more than any of them take. */
static void screenshot_settled(int cursor, const char *path) {
    for (int i = 0; i < 120 && !shell_settled(); i++) {
        shell_shot_sync(&catalog, cursor);
        shell_draw(&catalog, cursor);
    }
    gfx_screenshot(path);
}

static int install_app(int index, int screenshot) {
    struct app_entry *entry = &catalog.apps[index];
    struct install_report report;

    shell_install_begin(entry->name);
    unsigned start = now_ms();
    int rc = install(entry->manifest, entry->id, &report,
                     shell_install_phase, shell_install_progress, NULL);
    unsigned seconds = (now_ms() - start) / 1000;

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

/* The sweep and the failure dump both belong on the debug screen: one is a
   grid of text cells, the other a wall of log lines. The shell takes the
   display only once there is a catalog to put on it. */
static void run_entropy(void) {
    gui_init();
    gui_header("gathering entropy");
    install_recover();
    entropy_init();
    entropy_screen_prepare();
    if (entropy_screen_is_replay() || !entropy_load()) entropy_screen_run();
    entropy_save(entropy_screen_is_replay());
}

static int fetch_catalog(void) {
    gui_clear();
    gui_header("connecting");
    if (net_up() < 0) {
        logline("network failed");
        return -1;
    }
    logline("net up");
    return catalog_fetch(&catalog);
}

int main(void) {
    if (setup_callbacks() < 0) {
        gui_init();
        pspDebugScreenPrintf("exit callback failed; HOME will not work\n");
    }

    run_entropy();

    int count = fetch_catalog();
    catalog.count = count;
    if (count > 0) catalog_check_updates(&catalog);
    dump_diagnostics();

    if (count <= 0 || !shell_init()) {
        /* Nothing to browse, or no system font to browse it with. Either way
           the log says which, and the log is what gets shown. */
        gui_clear();
        gui_failure();
        sceDisplayWaitVblankStart();
        gfx_screenshot("ms0:/PSPDX.BMP");
        dump_diagnostics();
        for (;;) sceDisplayWaitVblankStart();
    }

    int cursor = 0;
    shell_shot_sync(&catalog, cursor);
    shell_draw(&catalog, cursor);
    screenshot_settled(cursor, "ms0:/PSPDX.BMP");
    dump_diagnostics();                 /* now with the shell's own lines */

    int automatic = auto_install_index();
    if (automatic >= 0) {
        cursor = automatic;
        install_app(cursor, 1);
        dump_diagnostics();
    }

    unsigned last_buttons = 0;
    for (;;) {
        SceCtrlData pad;
        sceCtrlReadBufferPositive(&pad, 1);
        unsigned pressed = pad.Buttons & ~last_buttons;
        last_buttons = pad.Buttons;

        if ((pressed & PSP_CTRL_DOWN) && cursor + 1 < catalog.count) cursor++;
        if ((pressed & PSP_CTRL_UP) && cursor > 0) cursor--;
        if (pressed & PSP_CTRL_CROSS) {
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

        shell_shot_sync(&catalog, cursor);
        shell_draw(&catalog, cursor);
    }
    return 0;
}
