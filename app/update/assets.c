#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>

#include "update/assets.h"
#include "network/https.h"
#include "util/runtime.h"

#define CACHE_DIR "ms0:/PSP/PSPDX/cache"

/* A screen-sized PNG is under 100 KB; ten seconds of video at 600 kbit land
   around 750. One and a half megabytes is well above both and still refuses
   a runaway body. */
#define ASSET_MAX (1536 * 1024)

static unsigned char g_buf[ASSET_MAX];
static size_t g_len;

static const char *EXT[] = { [ASSET_ICON] = "icon.png", [ASSET_SHOT] = "png",
                             [ASSET_VIDEO] = "mp4" };

static int sink(void *ctx, const void *data, size_t len) {
    (void)ctx;
    if (g_len + len > sizeof(g_buf)) return -1;
    memcpy(g_buf + g_len, data, len);
    g_len += len;
    return 0;
}

static void cache_path(enum asset_kind kind, const char *id, char *out, size_t size) {
    snprintf(out, size, CACHE_DIR "/%s.%s", id, EXT[kind]);
}

static size_t cache_read(enum asset_kind kind, const char *id) {
    char path[256];
    cache_path(kind, id, path, sizeof(path));
    int fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0) return 0;
    int n = sceIoRead(fd, g_buf, sizeof(g_buf));
    sceIoClose(fd);
    return n > 0 ? (size_t)n : 0;
}

static void cache_write(enum asset_kind kind, const char *id) {
    /* The parents belong to the installer and usually exist already; making
       them again is cheaper than asking. */
    sceIoMkdir("ms0:/PSP", 0777);
    sceIoMkdir("ms0:/PSP/PSPDX", 0777);
    sceIoMkdir(CACHE_DIR, 0777);
    char path[256];
    cache_path(kind, id, path, sizeof(path));
    int fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, g_buf, (SceSize)g_len);
    sceIoClose(fd);
}

const void *asset_fetch(enum asset_kind kind, const char *id, const char *url,
                        size_t *len) {
    if (!id) return 0;

    g_len = cache_read(kind, id);
    if (g_len) {
        logline("%s: %lu bytes cached, %s", EXT[kind], (unsigned long)g_len, id);
        *len = g_len;
        return g_buf;
    }
    if (!url || !url[0]) return 0;

    g_len = 0;
    struct https_result result;
    int rc = https_get(url, sink, 0, 0, 0, &result);
    if (rc != 0 || result.status != 200 || g_len == 0) {
        logline("%s: rc=%d status=%ld, %s", EXT[kind], rc, result.status, id);
        return 0;
    }
    cache_write(kind, id);
    logline("%s: %lu bytes fetched, %s", EXT[kind], (unsigned long)g_len, id);
    *len = g_len;
    return g_buf;
}
