#include <pspdebug.h>
#include <pspge.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>

#include "util/runtime.h"
#include "gui/screen.h"

#define LIST_ROW 3
#define TITLE "PSPDX  "

/* pspDebugScreen has its own printf; format through newlib first so the
   width can come from SCREEN_COLS instead of a literal in every call. */
static void print_padded(const char *text, int width) {
    char line[SCREEN_COLS + 1];
    snprintf(line, sizeof(line), "%-*.*s", width, width, text ? text : "");
    pspDebugScreenPrintf("%s", line);
}

void gui_init(void) { pspDebugScreenInit(); }
void gui_clear(void) { pspDebugScreenClear(); }

void gui_header(const char *right) {
    pspDebugScreenSetXY(0, 0);
    pspDebugScreenSetTextColor(COL_TEXT);
    pspDebugScreenPrintf("%s", TITLE);
    pspDebugScreenSetTextColor(COL_DIM);
    print_padded(right, SCREEN_COLS - (int)strlen(TITLE));
    pspDebugScreenSetTextColor(COL_TEXT);
}

static void state_text(const struct app_entry *entry, char *out, size_t size) {
    switch (entry->state) {
    case APP_NOT_INSTALLED: snprintf(out, size, "%s", entry->license); break;
    case APP_UNKNOWN:       snprintf(out, size, "installed %s", entry->local_version); break;
    case APP_CURRENT:       snprintf(out, size, "up to date"); break;
    case APP_UPDATE:        snprintf(out, size, "update %s", entry->remote_version); break;
    }
}

void gui_catalog(const struct catalog *catalog, int cursor) {
    for (int i = 0; i < catalog->count && LIST_ROW + 2 * i + 1 < STATUS_ROW - 1; i++) {
        const struct app_entry *entry = &catalog->apps[i];
        int selected = i == cursor;
        char state[20];
        state_text(entry, state, sizeof(state));
        pspDebugScreenSetXY(0, LIST_ROW + 2 * i);
        pspDebugScreenSetTextColor(selected ? COL_CURSOR :
                                   entry->state == APP_UPDATE ? COL_DONE : COL_TEXT);
        pspDebugScreenPrintf("%c %-34.34s %-10.10s %-13.13s", selected ? '>' : ' ',
                             entry->name, entry->category, state);
        pspDebugScreenSetXY(0, LIST_ROW + 2 * i + 1);
        pspDebugScreenSetTextColor(COL_DIM);
        pspDebugScreenPrintf("    %-56.56s", entry->summary);
    }
    pspDebugScreenSetTextColor(COL_TEXT);
}

void gui_status(const char *text) {
    pspDebugScreenSetXY(0, STATUS_ROW);
    pspDebugScreenSetTextColor(COL_DIM);
    print_padded(text, SCREEN_COLS);
    pspDebugScreenSetTextColor(COL_TEXT);
}

void gui_failure(void) {
    gui_header("failed");
    for (int i = 0; i < log_count() && i + 2 < STATUS_ROW; i++) {
        pspDebugScreenSetXY(0, 2 + i);
        pspDebugScreenPrintf("%s", log_at(i));
    }
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

void gui_install_begin(struct gui_progress *progress, const char *name) {
    memset(progress, 0, sizeof(*progress));
    progress->row = STATUS_ROW - 3;
    pspDebugScreenSetXY(0, progress->row - 1);
    pspDebugScreenSetTextColor(COL_TEXT);
    pspDebugScreenPrintf("Installing %s", name);
    gui_status("");
}

void gui_progress_phase(void *ctx, const char *phase) {
    struct gui_progress *progress = ctx;
    strncpy(progress->phase, phase, sizeof(progress->phase) - 1);
    progress->phase[sizeof(progress->phase) - 1] = '\0';
    progress->last_draw = 0;
    draw_bar(progress->row, 0, 0, progress->phase);
}

void gui_progress_update(void *ctx, size_t done, size_t total) {
    struct gui_progress *progress = ctx;
    unsigned now = now_ms();
    if (done != total && now - progress->last_draw < 250) return;
    progress->last_draw = now;
    draw_bar(progress->row, done, total, progress->phase);
}

void gui_install_end(const char *message) {
    pspDebugScreenSetXY(0, STATUS_ROW - 3);
    print_padded("", SCREEN_COLS);
    gui_status(message);
}

void gui_screenshot(const char *path) {
    enum { W = 480, H = 272, STRIDE = 512 };
    const unsigned *vram = (const unsigned *)(0x40000000 | (unsigned)sceGeEdramGetAddr());
    int fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    unsigned rowbytes = W * 3;
    unsigned datasize = rowbytes * H;
    unsigned char hdr[54] = { 'B', 'M' };
    unsigned value;
    value = 54 + datasize; memcpy(hdr + 2, &value, 4);
    value = 54;            memcpy(hdr + 10, &value, 4);
    value = 40;            memcpy(hdr + 14, &value, 4);
    value = W;             memcpy(hdr + 18, &value, 4);
    value = H;             memcpy(hdr + 22, &value, 4);
    hdr[26] = 1; hdr[28] = 24;
    value = datasize;      memcpy(hdr + 34, &value, 4);
    sceIoWrite(fd, hdr, sizeof(hdr));
    static unsigned char row[W * 3];
    for (int y = H - 1; y >= 0; y--) {
        const unsigned *src = vram + y * STRIDE;
        for (int x = 0; x < W; x++) {
            unsigned px = src[x];
            row[x * 3 + 0] = (px >> 16) & 0xff;
            row[x * 3 + 1] = (px >> 8) & 0xff;
            row[x * 3 + 2] = px & 0xff;
        }
        sceIoWrite(fd, row, sizeof(row));
    }
    sceIoClose(fd);
}
