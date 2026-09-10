/* PSPDX application controller. Feature code lives behind module APIs. */

#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>

#include "gui/entropy_screen.h"
#include "gui/screen.h"
#include "install/install.h"
#include "logic/entropy.h"
#include "network/https.h"
#include "update/catalog.h"
#include "util/runtime.h"

PSP_MODULE_INFO("pspdx", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(4 * 1024);

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

struct install_ui {
    struct gui_progress progress;
};

static void install_phase(void *ctx, const char *phase) {
    struct install_ui *ui = ctx;
    gui_progress_phase(&ui->progress, phase);
    log_dump();
}

static void install_progress(void *ctx, size_t done, size_t total) {
    struct install_ui *ui = ctx;
    gui_progress_update(&ui->progress, done, total);
}

static int install_app(int index, int screenshot) {
    struct app_entry *entry = &catalog.apps[index];
    struct install_report report;
    struct install_ui ui;
    gui_install_begin(&ui.progress, entry->name);

    unsigned start = now_ms();
    int rc = install(entry->manifest, entry->id, &report,
                     install_phase, install_progress, &ui);
    gui_catalog(&catalog, index);
    unsigned seconds = (now_ms() - start) / 1000;

    char message[SCREEN_COLS + 1];
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
    gui_install_end(message);
    if (screenshot) gui_screenshot("ms0:/PSPDX2.BMP");
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

static void show_catalog(int updates) {
    gui_clear();
    if (catalog.count >= 0) {
        char header[64];
        if (updates) {
            snprintf(header, sizeof(header), "%d apps, %d update%s available",
                     catalog.total, updates, updates == 1 ? "" : "s");
        } else {
            snprintf(header, sizeof(header), "%d apps  %lu bytes  %u ms handshake",
                     catalog.total, (unsigned long)catalog.response_len,
                     catalog.fetch.handshake_ms);
        }
        gui_header(header);
        gui_catalog(&catalog, 0);
        gui_status("X: install or update   SELECT: discard entropy and sweep again");
    } else {
        gui_failure();
    }
}

int main(void) {
    if (setup_callbacks() < 0) {
        gui_init();
        pspDebugScreenPrintf("exit callback failed; HOME will not work\n");
    }
    gui_init();
    gui_header("gathering entropy");

    install_recover();
    entropy_init();
    entropy_screen_prepare();
    if (entropy_screen_is_replay() || !entropy_load()) entropy_screen_run();
    entropy_save(entropy_screen_is_replay());

    gui_clear();
    gui_header("connecting");
    int count = -1;
    if (net_up() < 0) {
        logline("network failed");
    } else {
        logline("net up");
        count = catalog_fetch(&catalog);
    }
    catalog.count = count;
    int updates = count > 0 ? catalog_check_updates(&catalog) : 0;
    show_catalog(updates);
    sceDisplayWaitVblankStart();
    gui_screenshot("ms0:/PSPDX.BMP");
    dump_diagnostics();

    int cursor = 0;
    int automatic = count > 0 ? auto_install_index() : -1;
    if (automatic >= 0) {
        cursor = automatic;
        gui_catalog(&catalog, cursor);
        install_app(cursor, 1);
        dump_diagnostics();
    }

    unsigned last_buttons = 0;
    for (;;) {
        SceCtrlData pad;
        sceCtrlReadBufferPositive(&pad, 1);
        unsigned pressed = pad.Buttons & ~last_buttons;
        last_buttons = pad.Buttons;
        if (count > 0 && (pressed & PSP_CTRL_DOWN) && cursor + 1 < catalog.count)
            gui_catalog(&catalog, ++cursor);
        if (count > 0 && (pressed & PSP_CTRL_UP) && cursor > 0)
            gui_catalog(&catalog, --cursor);
        if (count > 0 && (pressed & PSP_CTRL_CROSS)) {
            install_app(cursor, 0);
            dump_diagnostics();
        }
        if (pressed & PSP_CTRL_SELECT) {
            entropy_forget();
            entropy_init();
            gui_clear();
            gui_header("entropy discarded");
            entropy_screen_run();
            entropy_save(entropy_screen_is_replay());
            gui_clear();
            gui_header("entropy regenerated");
            if (count >= 0) gui_catalog(&catalog, cursor);
            entropy_screen_reset_cache();
        }
        sceDisplayWaitVblankStart();
    }
    return 0;
}
