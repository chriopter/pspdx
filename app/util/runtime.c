#include <pspiofilemgr.h>
#include <psprtc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "util/runtime.h"

#define LOGLINES 26
#define LOGCOLS 61            /* one 60-column debug-screen row plus NUL */

static char lines[LOGLINES][LOGCOLS];
static int line_count;

void logline(const char *fmt, ...) {
    char line[LOGCOLS];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (line_count < LOGLINES) strcpy(lines[line_count++], line);
    sceIoWrite(1, line, strlen(line));
    sceIoWrite(1, "\n", 1);
}

void log_dump(void) {
    int fd = sceIoOpen("ms0:/PSPDX.LOG", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    for (int i = 0; i < line_count; i++) {
        sceIoWrite(fd, lines[i], strlen(lines[i]));
        sceIoWrite(fd, "\n", 1);
    }
    sceIoClose(fd);
}

int log_count(void) { return line_count; }

const char *log_at(int index) {
    return index >= 0 && index < line_count ? lines[index] : "";
}

unsigned now_ms(void) {
    u64 tick = 0;
    sceRtcGetCurrentTick(&tick);
    return (unsigned)(tick / 1000);
}

int expired(unsigned start, unsigned budget_ms) {
    return (now_ms() - start) > budget_ms;
}
