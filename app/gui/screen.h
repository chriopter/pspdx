#ifndef PSPDX_SCREEN_H
#define PSPDX_SCREEN_H


/* What is left of the text UI: the entropy sweep draws itself on the debug
   screen, and a failed run dumps the log there. Everything else moved to the
   GE shell in gui/shell.h. */

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

void gui_init(void);
void gui_clear(void);
void gui_header(const char *right);
void gui_status(const char *text);
void gui_failure(void);

#endif
