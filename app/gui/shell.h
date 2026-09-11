#ifndef PSPDX_SHELL_H
#define PSPDX_SHELL_H

#include <stddef.h>

#include "update/catalog.h"

/* The catalog browser, drawn with the GE and the system font. Everything the
   user sees after the entropy sweep goes through here; the debug screen stays
   for the failure dump, where a wall of log text is the right answer. */

/* 0 if the firmware font is missing, in which case nothing was initialised
   and the caller should stay on the debug screen. */
int shell_init(void);
void shell_shutdown(void);

/* One frame, paced at 60 Hz by the vblank wait inside. */
void shell_draw(const struct catalog *catalog, int cursor);

/* ------------------------------------------------------------------ tabs */

/* The browser shows one category at a time, chosen by the tabs across the
   top. The view is the catalog filtered to the active tab, and a cursor
   anywhere in this program is a row of the view rather than a catalog
   index -- so there is one filter and no second copy of the entries.

   Called whenever the catalog has been rewritten: it works out which tabs
   have anything in them, keeps the active category if it survived, and
   builds the view. */
void shell_view_rebuild(const struct catalog *catalog);

int shell_view_count(void);
int shell_view_index(int row);      /* view row -> catalog index, -1 if none */
int shell_view_row(int index);      /* catalog index -> view row, -1 if hidden */

/* How many tabs are on screen: one means there is nothing to switch. */
int shell_tab_count(void);

/* L and R: one tab along, wrapping. The view follows; the caller resets
   its cursor. */
void shell_tab_move(int step);

/* True once nothing is mid-transition: the start fade is over, the selection
   bar has arrived, the screenshot has faded in. What a screenshot of the
   screen should wait for. */
int shell_settled(void);

/* Worst frame phases since the last call, as a line for the log. */
void shell_profile(char *out, int size);

/* Fetches and decodes the selected entry's screenshot once the cursor has
   stopped moving, so holding a direction does not start a download per row.
   Blocks for the length of the fetch. */
void shell_shot_sync(const struct catalog *catalog, int cursor);

/* The line at the bottom: what the client is doing right now. Empty
   returns to the key hints. Cleared by a cursor move, like the install
   result it also carries. */
void shell_status(const char *text);

/* The word that stands in the room while there is no catalog: Connecting
   by default, or what the caller says the wait has become. */
void shell_word(const char *word);

/* A question standing over the browser until it is answered: a title, a line
   under it, and a footer saying which button means what. The shell only draws
   it -- the pad belongs to the main loop, and so does the answer. A null or
   empty title takes the question away again. */
void shell_ask(const char *title, const char *line);

/* A short menu in the same band: a title, up to four choices, and the cursor
   on one of them. takeable[i] zero draws that row grey -- the choice exists
   and cannot be taken, which is how a package that has no update says so.
   count 0 closes it. Like the question, it is drawn here and driven there. */
void shell_menu(const char *title, const char *const *items,
                const unsigned char *takeable, int count, int cursor);

/* The info band over the dimmed browser: what this session is connected to
   and what it is standing on. Drawn while open, and nothing more -- the
   button that closes it and the action row's button are read in the main
   loop. */
void shell_info(int open);

/* Install progress, drawn over the browser. The two middle ones match the
   callback types install() expects. */
void shell_install_begin(const char *name);
void shell_install_phase(void *ctx, const char *phase);
void shell_install_progress(void *ctx, size_t done, size_t total);
void shell_install_end(const char *message);

#endif
