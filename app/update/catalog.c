#include <pspiofilemgr.h>
#include <cjson/cJSON.h>
#include <stdio.h>
#include <string.h>

#include "update/catalog.h"
#include "update/sources.h"
#include "install/install.h"
#include "util/runtime.h"

#ifndef CATALOG_URL   /* a test build may point at a catalog on the host */
#define CATALOG_URL "https://chriopter.github.io/pspdx-catalog/catalog.json"
#endif

static char response[200 * 1024];
static size_t response_len;

/* The cache last taken: the info band names its host, and the bench fetches
   it. Before any source has answered it is the built-in one. */
static char g_catalog_url[SOURCE_URL] = CATALOG_URL;
static char g_progress[48];
static char g_refused_url[SOURCE_URL];
static int g_refused_rc;

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
static void asset_url(const char *base, const char *rel, char *out, size_t size) {
    if (!rel || !rel[0]) { out[0] = '\0'; return; }
    if (strncmp(rel, "http://", 7) == 0 || strncmp(rel, "https://", 8) == 0) {
        snprintf(out, size, "%s", rel);
        return;
    }
    const char *slash = strrchr(base, '/');
    snprintf(out, size, "%.*s%s", (int)(slash - base) + 1, base, rel);
}

/* Which state the stick puts an entry in: unknown until the update check
   has compared revs, or not installed when there is no record at all. */
static void settle_state(struct app_entry *entry) {
    struct installed installed;
    if (db_read(entry->id, &installed) == 0) {
        entry->state = APP_UNKNOWN;
        entry->local_rev = installed.rev;
        snprintf(entry->local_version, sizeof(entry->local_version), "%s", installed.version);
    } else {
        entry->state = APP_NOT_INSTALLED;
    }
}

/* The first source to name an id wins: an entry already there is left
   alone, whatever a later source says about it. */
static int has_id(const struct catalog *catalog, const char *id) {
    for (int i = 0; i < catalog->count; i++)
        if (strcmp(catalog->apps[i].id, id) == 0) return 1;
    return 0;
}

/* A catalog.json in the response buffer, merged into the catalog. base is
   the URL it came from, for the assets it names relative to itself.
   Returns the entries taken, or -1 for something that is not a catalog. */
static int parse(struct catalog *catalog, const char *base) {
    cJSON *root = cJSON_ParseWithLength(response, response_len);
    if (!root) { logline("catalog: not json"); return -1; }
    cJSON *apps = cJSON_GetObjectItemCaseSensitive(root, "apps");
    if (!cJSON_IsArray(apps)) {
        logline("catalog: no apps array");
        cJSON_Delete(root);
        return -1;
    }

    int taken = 0;
    catalog->total += cJSON_GetArraySize(apps);
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

        cJSON *release = cJSON_GetObjectItemCaseSensitive(app, "release");
        if (cJSON_IsObject(release)) {
            struct manifest *m = &entry->release;
            memset(m, 0, sizeof(*m));
            strncpy(m->id, entry->id, sizeof(m->id) - 1);
            cJSON *rev = cJSON_GetObjectItemCaseSensitive(release, "rev");
            cJSON *size = cJSON_GetObjectItemCaseSensitive(release, "size");
            cJSON *sha = cJSON_GetObjectItemCaseSensitive(release, "sha256");
            /* The same rules the manifest is held to: these fields go
               straight into a download and an unpack. */
            int ok = 1;
            if (cJSON_IsNumber(rev) && manifest_rev_in_range(rev->valuedouble))
                m->rev = (unsigned)rev->valuedouble;
            else ok = 0;
            if (cJSON_IsNumber(size) && manifest_size_in_range(size->valuedouble))
                m->size = (size_t)size->valuedouble;
            else ok = 0;
            copy_str(m->url, sizeof(m->url), cJSON_GetObjectItemCaseSensitive(release, "url"));
            copy_str(m->version, sizeof(m->version), cJSON_GetObjectItemCaseSensitive(release, "version"));
            ok = ok && m->rev && m->url[0] && cJSON_IsString(sha) && strlen(sha->valuestring) == 64;
            for (int k = 0; ok && k < 32; k++) {
                unsigned byte;
                if (sscanf(sha->valuestring + 2 * k, "%2x", &byte) != 1) ok = 0;
                m->sha256[k] = (unsigned char)byte;
            }
            entry->has_release = ok;
            /* The release is installed from as it stands, and the record
               it writes has to say where the file behind it lives, or the
               package could never update from its source once the cache
               is gone. */
            memcpy(m->manifest_url, entry->manifest, sizeof(entry->manifest));
        }

        char shot[256];
        copy_str(shot, sizeof(shot), cJSON_GetObjectItemCaseSensitive(app, "icon"));
        asset_url(base, shot, entry->icon, sizeof(entry->icon));
        copy_str(shot, sizeof(shot), cJSON_GetObjectItemCaseSensitive(app, "screenshot"));
        asset_url(base, shot, entry->screenshot, sizeof(entry->screenshot));
        copy_str(shot, sizeof(shot), cJSON_GetObjectItemCaseSensitive(app, "video"));
        asset_url(base, shot, entry->video, sizeof(entry->video));
        copy_str(shot, sizeof(shot), cJSON_GetObjectItemCaseSensitive(app, "sound"));
        asset_url(base, shot, entry->sound, sizeof(entry->sound));

        /* An id names a directory on the stick and a file in the cache: one
           that cannot be a path component is not an entry. */
        if (!manifest_id_is_safe(entry->id) || !entry->name[0]) continue;
        if (!entry->has_release && !entry->manifest[0]) continue;
        if (has_id(catalog, entry->id)) continue;

        settle_state(entry);
        catalog->count++;
        taken++;
    }
    cJSON_Delete(root);
    return taken;
}

static int response_sink(void *ctx, const void *data, size_t len) {
    (void)ctx;
    if (response_len + len >= sizeof(response)) return -1;
    memcpy(response + response_len, data, len);
    response_len += len;
    return 0;
}

/* One text file into the response buffer. Returns 0 when it arrived. */
static int fetch_text(const char *url, struct https_result *out) {
    struct https_result r;
    response_len = 0;
    int rc = https_get(url, response_sink, NULL, NULL, NULL, &r);
    if (out) *out = r;
    if (rc != 0 || r.status != 200) {
        logline("fetch: rc=%d status=%ld %s", rc, r.status, url);
        return -1;
    }
    response[response_len] = '\0';
    return 0;
}

/* A cache fetched and merged in. Returns the entries taken, or -1 when
   the cache did not answer or was not a catalog. */
static int take_cache(struct catalog *catalog, const char *url) {
    struct https_result r;
    if (fetch_text(url, &r) < 0) return -1;
    int taken = parse(catalog, url);
    if (taken < 0) return -1;
    catalog->fetch = r;
    catalog->response_len = response_len;
    snprintf(g_catalog_url, sizeof(g_catalog_url), "%s", url);
    return taken;
}

/* One repository's app.pspdx, fetched and made into an entry. Nothing in
   the file is a picture; the console shows nothing there rather than spend
   a request finding out. Returns 1 taken, 0 already there, -1 refused. */
static int take_manifest(struct catalog *catalog, const struct source_repo *repo) {
    char expect[96], url[SOURCE_URL];
    sources_repo_expect(repo, expect, sizeof(expect));
    sources_repo_manifest(repo, url, sizeof(url));
    if (catalog->count >= MAX_APPS) return -1;

    /* The list says whose the app is and where the file is; which app it
       is, the file says. So the id is only known once the file is here,
       and the first source to name it wins from then on. */
    struct manifest m;
    int rc = manifest_fetch(url, expect, &m);
    if (rc < 0) {
        snprintf(g_refused_url, sizeof(g_refused_url), "%s", url);
        g_refused_rc = rc;
        return -1;
    }
    if (has_id(catalog, m.id)) return 0;
    struct app_entry *entry = &catalog->apps[catalog->count];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->id, sizeof(entry->id), "%s", m.id);
    snprintf(entry->name, sizeof(entry->name), "%s", m.name);
    snprintf(entry->author, sizeof(entry->author), "%s", m.author);
    snprintf(entry->summary, sizeof(entry->summary), "%s", m.summary);
    snprintf(entry->category, sizeof(entry->category), "%s", m.category);
    snprintf(entry->license, sizeof(entry->license), "%s", m.license);
    snprintf(entry->manifest, sizeof(entry->manifest), "%s", url);
    if (!entry->name[0]) {
        /* A file from before the author's half existed has no name; the
           last piece of the id -- "extremetuxracer" -- is what it has. */
        const char *dot = strrchr(entry->id, '.');
        snprintf(entry->name, sizeof(entry->name), "%s", dot ? dot + 1 : entry->id);
    }
    entry->release = m;
    snprintf(entry->release.manifest_url, sizeof(entry->release.manifest_url), "%s", url);
    entry->has_release = 1;
    settle_state(entry);
    catalog->count++;
    catalog->total++;
    return 1;
}

/* Every repository of a list, one fetch each, the status line counting
   along. Returns the entries taken. */
static int walk_list(struct catalog *catalog, const struct source_list *list,
                     int *refused) {
    int taken = 0;
    *refused = 0;
    for (int i = 0; i < list->count; i++) {
        snprintf(g_progress, sizeof(g_progress), "list: %d of %d", i + 1, list->count);
        int rc = take_manifest(catalog, &list->repo[i]);
        if (rc > 0) taken++;
        else if (rc < 0) (*refused)++;
    }
    g_progress[0] = '\0';
    return taken;
}

/* One source, whatever kind it is. Returns the entries taken, or -1 when
   nothing behind it answered. */
static int fetch_source(struct catalog *catalog, int at, const char *url) {
    struct source_list list;
    int refused = 0, taken;

    switch (sources_kind(url)) {
    case SOURCE_CATALOG:
        taken = take_cache(catalog, url);
        if (taken < 0) logline("source %d: catalog unreachable", at);
        else logline("source %d: catalog, %d apps", at, taken);
        return taken;

    case SOURCE_REPO:
        sources_parse_repo(url, &list.repo[0]);
        list.count = 1;
        taken = walk_list(catalog, &list, &refused);
        logline("source %d: repository %s/%s, %d app%s", at,
                list.repo[0].owner, list.repo[0].name, taken,
                refused ? ", refused" : "");
        return refused && !taken ? -1 : taken;

    default:
        break;
    }

    if (fetch_text(url, NULL) < 0) {
        /* The built-in list names the built-in cache, and a console that
           cannot read the one still knows where the other is. */
        if (strcmp(url, SOURCES_DEFAULT) != 0) {
            logline("source %d: list unreachable", at);
            return -1;
        }
        taken = take_cache(catalog, CATALOG_URL);
        logline("source %d: list unreachable, built-in cache %s", at,
                taken < 0 ? "unreachable too" : "taken");
        return taken;
    }
    sources_parse_list(response, &list);
    if (list.cache[0]) {
        taken = take_cache(catalog, list.cache);
        if (taken >= 0) {
            logline("source %d: list of %d, cache taken, %d apps", at, list.count, taken);
            return taken;
        }
    }
    taken = walk_list(catalog, &list, &refused);
    logline("source %d: list of %d walked, %d apps, %d refused%s", at, list.count,
            taken, refused, list.cache[0] ? ", cache unreachable" : "");
    return taken;
}

const char *catalog_url(void) { return g_catalog_url; }
const char *catalog_progress(void) { return g_progress; }

int catalog_refused(const char *url) {
    return strcmp(g_refused_url, url) == 0 ? g_refused_rc : 0;
}

int catalog_fetch(struct catalog *catalog) {
    struct sources sources;
    memset(catalog, 0, sizeof(*catalog));
    g_progress[0] = '\0';
    g_refused_url[0] = '\0';
    sources_load(&sources);
    int answered = 0;
    for (int i = 0; i < sources.count; i++)
        if (fetch_source(catalog, i + 1, sources.url[i]) >= 0) answered++;
    logline("catalog: %d of %d sources, %d apps, %d usable", answered,
            sources.count, catalog->total, catalog->count);
    return answered ? catalog->count : -1;
}

int catalog_check_updates(struct catalog *catalog) {
    int updates = 0;
    for (int i = 0; i < catalog->count; i++) {
        struct app_entry *entry = &catalog->apps[i];
        if (entry->state == APP_NOT_INSTALLED) continue;
        struct manifest manifest;
        if (entry->has_release) manifest = entry->release;
        else if (manifest_fetch(entry->manifest, entry->id, &manifest) < 0) continue;
        entry->remote_rev = manifest.rev;
        snprintf(entry->remote_version, sizeof(entry->remote_version), "%s", manifest.version);
        /* Every other record carries the rev of the release it came from,
           because an install had the catalog in front of it. PSPDX's own was
           written by its first start out of nothing but the build, and a rev
           is the moment GitHub published the release -- which a build cannot
           know. So that record holds rev 0 and the version string the build
           was made from, and this once the comparison is made on the version
           instead. The same version means the stick is running the published
           release: the catalog's rev goes into the record, and from the next
           run on it is an ordinary record compared like any other. A
           different version is an update, whichever way the strings sort. */
        if (strcmp(entry->id, PSPDX_SELF_ID) == 0 && entry->local_rev == 0) {
            /* A build made past the tag -- "0.1.0-5-gabc", as git describes
               it -- is the release and then some, not an older one: it
               counts as current, or every desk build would offer itself
               the release it was built after. */
            size_t n = strlen(manifest.version);
            int same = strncmp(entry->local_version, manifest.version, n) == 0 &&
                       (entry->local_version[n] == '\0' || entry->local_version[n] == '-');
            if (same) {
                struct installed self;
                if (db_read(entry->id, &self) == 0) {
                    self.rev = manifest.rev;
                    if (db_write_record(&self) == 0) entry->local_rev = manifest.rev;
                }
                entry->state = APP_CURRENT;
                logline("self: %s is the published release, rev %u noted",
                        entry->local_version, manifest.rev);
            } else {
                entry->state = APP_UPDATE;
                updates++;
                logline("self: %s installed, %s published",
                        entry->local_version, manifest.version);
            }
            continue;
        }
        if (manifest.rev > entry->local_rev) {
            entry->state = APP_UPDATE;
            updates++;
        } else {
            entry->state = APP_CURRENT;
        }
    }
    logline("updates: %d waiting, %d apps", updates, catalog->count);
    return updates;
}

void catalog_dump_http(void) {
    if (!response_len) return;
    int fd = sceIoOpen("ms0:/PSPDX.HTTP", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, response, response_len);
    sceIoClose(fd);
}
