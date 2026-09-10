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

/* Install progress, drawn over the browser. The two middle ones match the
   callback types install() expects. */
void shell_install_begin(const char *name);
void shell_install_phase(void *ctx, const char *phase);
void shell_install_progress(void *ctx, size_t done, size_t total);
void shell_install_end(const char *message);

#endif
