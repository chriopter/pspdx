#include <pspkernel.h>
#include <stdio.h>

#include "update/sync.h"
#include "network/https.h"
#include "util/runtime.h"

/* Below the main thread's priority: the browser keeps its frame rate and
   the network runs in the time the browser spends waiting for vblank,
   which is most of every frame. A handshake takes a moment longer and
   nothing on screen stutters for it. */
#define SYNC_PRIORITY 0x21
#define SYNC_STACK (128 * 1024)

static struct catalog *g_catalog;
static volatile enum sync_state g_state = SYNC_IDLE;
static char g_message[64];

static int run(SceSize args, void *argp) {
    (void)args; (void)argp;
    g_state = SYNC_CONNECTING;
    if (net_up() < 0) {
        logline("network failed");
        snprintf(g_message, sizeof(g_message), "no network");
        g_state = SYNC_FAILED;
        return 0;
    }
    logline("net up");
    g_state = SYNC_FETCHING;
    int count = catalog_fetch(g_catalog);
    if (count < 0) {
        snprintf(g_message, sizeof(g_message), "catalog unreachable");
        g_catalog->count = 0;
        g_state = SYNC_FAILED;
        return 0;
    }
    g_catalog->count = count;
    g_state = SYNC_CHECKING;
    if (count > 0) catalog_check_updates(g_catalog);
    g_message[0] = '\0';
    g_state = SYNC_DONE;
    return 0;
}

int sync_start(struct catalog *catalog) {
    g_catalog = catalog;
    g_state = SYNC_IDLE;
    SceUID thread = sceKernelCreateThread("sync", run, SYNC_PRIORITY, SYNC_STACK,
                                          PSP_THREAD_ATTR_USER, 0);
    if (thread < 0) {
        logline("sync: no thread %08x", thread);
        return -1;
    }
    sceKernelStartThread(thread, 0, 0);
    return 0;
}

enum sync_state sync_state(void) { return g_state; }

int sync_done(void) { return g_state == SYNC_DONE || g_state == SYNC_FAILED; }

const char *sync_message(void) {
    switch (g_state) {
    case SYNC_IDLE:
    case SYNC_CONNECTING: return "connecting";
    case SYNC_FETCHING:   return "fetching the catalog";
    case SYNC_CHECKING:   return "checking for updates";
    case SYNC_FAILED:     return g_message;
    default:              return "";
    }
}
