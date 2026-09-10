#include <pspiofilemgr.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>

#include "update/catalog.h"
#include "install/install.h"
#include "util/runtime.h"

#define CATALOG_URL "https://chriopter.github.io/pspdx-catalog/catalog.json"

static char response[200 * 1024];
static size_t response_len;

static void copy_str(char *dst, size_t size, cJSON *value) {
    if (cJSON_IsString(value)) {
        strncpy(dst, value->valuestring, size - 1);
        dst[size - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

/* Entries point at their assets relative to the catalog, so that moving the
   whole thing to another host stays a one-line change. */
static void asset_url(const char *rel, char *out, size_t size) {
    if (!rel || !rel[0]) { out[0] = '\0'; return; }
    if (strncmp(rel, "http://", 7) == 0 || strncmp(rel, "https://", 8) == 0) {
        snprintf(out, size, "%s", rel);
        return;
    }
    const char *slash = strrchr(CATALOG_URL, '/');
    snprintf(out, size, "%.*s%s", (int)(slash - CATALOG_URL) + 1, CATALOG_URL,
             rel);
}

static int parse(struct catalog *catalog) {
    cJSON *root = cJSON_ParseWithLength(response, response_len);
    if (!root) { logline("catalog: not json"); return -1; }
    cJSON *apps = cJSON_GetObjectItemCaseSensitive(root, "apps");
    if (!cJSON_IsArray(apps)) {
        logline("catalog: no apps array");
        cJSON_Delete(root);
        return -1;
    }

    catalog->count = 0;
    catalog->total = cJSON_GetArraySize(apps);
    cJSON *app;
    cJSON_ArrayForEach(app, apps) {
        if (catalog->count >= MAX_APPS) break;
        struct app_entry *entry = &catalog->apps[catalog->count];
        memset(entry, 0, sizeof(*entry));
        copy_str(entry->id, sizeof(entry->id), cJSON_GetObjectItemCaseSensitive(app, "id"));
        copy_str(entry->name, sizeof(entry->name), cJSON_GetObjectItemCaseSensitive(app, "name"));
        copy_str(entry->author, sizeof(entry->author), cJSON_GetObjectItemCaseSensitive(app, "author"));
        copy_str(entry->summary, sizeof(entry->summary), cJSON_GetObjectItemCaseSensitive(app, "summary"));
        copy_str(entry->category, sizeof(entry->category), cJSON_GetObjectItemCaseSensitive(app, "category"));
        copy_str(entry->license, sizeof(entry->license), cJSON_GetObjectItemCaseSensitive(app, "license"));
        copy_str(entry->manifest, sizeof(entry->manifest), cJSON_GetObjectItemCaseSensitive(app, "manifest"));

        char shot[256];
        copy_str(shot, sizeof(shot), cJSON_GetObjectItemCaseSensitive(app, "screenshot"));
        asset_url(shot, entry->screenshot, sizeof(entry->screenshot));
        copy_str(shot, sizeof(shot), cJSON_GetObjectItemCaseSensitive(app, "video"));
        asset_url(shot, entry->video, sizeof(entry->video));

        if (!entry->id[0] || !entry->name[0] || !entry->manifest[0]) continue;

        struct installed installed;
        if (db_read(entry->id, &installed) == 0) {
            entry->state = APP_UNKNOWN;
            entry->local_rev = installed.rev;
            strncpy(entry->local_version, installed.version, sizeof(entry->local_version) - 1);
        } else {
            entry->state = APP_NOT_INSTALLED;
        }
        catalog->count++;
    }
    cJSON_Delete(root);
    logline("catalog: %d apps, %d usable", catalog->total, catalog->count);
    return catalog->count;
}

static int response_sink(void *ctx, const void *data, size_t len) {
    (void)ctx;
    if (response_len + len >= sizeof(response)) return -1;
    memcpy(response + response_len, data, len);
    response_len += len;
    return 0;
}

int catalog_fetch(struct catalog *catalog) {
    memset(catalog, 0, sizeof(*catalog));
    response_len = 0;
    int rc = https_get(CATALOG_URL, response_sink, NULL, NULL, NULL, &catalog->fetch);
    if (rc != 0 || catalog->fetch.status != 200) {
        logline("catalog: rc=%d status=%ld", rc, catalog->fetch.status);
        return -1;
    }
    response[response_len] = '\0';
    catalog->response_len = response_len;
    return parse(catalog);
}

int catalog_check_updates(struct catalog *catalog) {
    int updates = 0;
    for (int i = 0; i < catalog->count; i++) {
        struct app_entry *entry = &catalog->apps[i];
        if (entry->state == APP_NOT_INSTALLED) continue;
        struct manifest manifest;
        if (manifest_fetch(entry->manifest, entry->id, &manifest) < 0) continue;
        entry->remote_rev = manifest.rev;
        strncpy(entry->remote_version, manifest.version, sizeof(entry->remote_version) - 1);
        if (manifest.rev > entry->local_rev) {
            entry->state = APP_UPDATE;
            updates++;
        } else {
            entry->state = APP_CURRENT;
        }
    }
    logline("updates: %d of %d installed", updates, catalog->count);
    return updates;
}

void catalog_dump_http(void) {
    if (!response_len) return;
    int fd = sceIoOpen("ms0:/PSPDX.HTTP", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, response, response_len);
    sceIoClose(fd);
}
