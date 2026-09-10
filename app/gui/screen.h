#ifndef PSPDX_SCREEN_H
#define PSPDX_SCREEN_H

#include <stddef.h>
#include "update/catalog.h"

/* pspDebugScreen is an 8x8 font on a 480x272 panel: 60 columns, 34 rows. */
#define SCREEN_COLS 60
#define ANIM_ROW 32
#define STATUS_ROW (ANIM_ROW - 2)

#define COL_IDLE   0xFF103818
#define COL_SPIN   0xFFF0F0F0
#define COL_WARM   0xFFC0FFC0
#define COL_DONE   0xFF20C020
#define COL_CURSOR 0xFFFFFF40
#define COL_TEXT   0xFFFFFFFF
#define COL_DIM    0xFF909090

struct gui_progress {
    char phase[16];
    unsigned last_draw;
    int row;
};

void gui_init(void);
void gui_clear(void);
void gui_header(const char *right);
void gui_catalog(const struct catalog *catalog, int cursor);
void gui_status(const char *text);
void gui_failure(void);
void gui_install_begin(struct gui_progress *progress, const char *name);
void gui_progress_phase(void *ctx, const char *phase);
void gui_progress_update(void *ctx, size_t done, size_t total);
void gui_install_end(const char *message);
void gui_screenshot(const char *path);

#endif
