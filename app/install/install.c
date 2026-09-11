/*
 * Installing a package: manifest, download, hash, unpack, one rename.
 *
 * Nothing is created in place. FAT32 has no transactions and PSP users pull
 * the battery, so the archive is downloaded and unpacked under
 * PSP/PSPDX/tmp/ and only a finished directory is renamed into
 * PSP/GAME/. The rename is the commit.
 *
 * Only the PSP/GAME/<dir>/ subtree of the archive is installed. Everything
 * beside it -- PSP/SYSTEM configs, LICENSES/, a build.json -- is the user's or
 * nobody's, and is never written.
 */

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <cjson/cJSON.h>

#include "util/runtime.h"
#include "install/install.h"
#include "install/zipread.h"

#define ROOT "ms0:"
#define TMP_DIR   ROOT "/PSP/PSPDX/tmp"
#define DB_DIR    ROOT "/PSP/PSPDX/db"
#define GAME_DIR  ROOT "/PSP/GAME"
#define ARCHIVE   TMP_DIR "/download.zip"
/* The staging directory is a sibling of the destination, not a child of
   PSP/PSPDX/tmp: sceIoRename cannot move anything between directories. It
   takes the basename of its second argument and renames within the first
   one's directory, silently -- a rename from tmp/stage to PSP/GAME/Foo
   reports success and leaves tmp/Foo behind. */
#define STAGE     GAME_DIR "/.pspdx-stage"

/* A Memory Stick tops out at 32 GB and no homebrew is anywhere near this.
   The number exists so that a size field cannot ask for something absurd. */
/* ------------------------------------------------------------- manifest */

static char g_manifest[8 * 1024];
static size_t g_manifest_len;

static int mem_sink(void *ctx, const void *data, size_t len) {
    (void)ctx;
    if (g_manifest_len + len >= sizeof(g_manifest)) return -1;
    memcpy(g_manifest + g_manifest_len, data, len);
    g_manifest_len += len;
    return 0;
}

/* An id becomes a file name, so it may not carry a path. Reverse-DNS letters,
   digits, dot, dash and underscore only. */
int manifest_id_is_safe(const char *id) {
    if (!id || !*id || strlen(id) > 80) return 0;
    for (const char *p = id; *p; p++) {
        int ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                 (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_';
        if (!ok) return 0;
    }
    if (strstr(id, "..")) return 0;
    return 1;
}

/* Attacker-controlled doubles: out of range, the conversion to an integer
   is undefined rather than merely wrong. */
int manifest_rev_in_range(double rev) {
    return rev >= 0 && rev <= 4294967295.0;
}

int manifest_size_in_range(double size) {
    return size > 0 && size <= MAX_PACKAGE_BYTES;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int manifest_fetch(const char *url, const char *expect_id, struct manifest *m) {
    struct https_result r;
    g_manifest_len = 0;
    memset(m, 0, sizeof(*m));

    int rc = https_get(url, mem_sink, NULL, NULL, NULL, &r);
    if (rc != 0 || r.status != 200) {
        logline("manifest: fetch rc=%d status=%ld", rc, r.status);
        return -1;
    }

    cJSON *root = cJSON_ParseWithLength(g_manifest, g_manifest_len);
    if (!root) { logline("manifest: not json"); return -2; }

    cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    cJSON *rev    = cJSON_GetObjectItemCaseSensitive(root, "rev");
    cJSON *u      = cJSON_GetObjectItemCaseSensitive(root, "url");
    cJSON *sha    = cJSON_GetObjectItemCaseSensitive(root, "sha256");
    cJSON *size   = cJSON_GetObjectItemCaseSensitive(root, "size");
    cJSON *id     = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *disp   = cJSON_GetObjectItemCaseSensitive(root, "display");

    rc = -3;
    if (!cJSON_IsString(schema) || strcmp(schema->valuestring, PSPDX_SCHEMA) != 0) {
        logline("manifest: schema");
        goto out;
    }
    if (!cJSON_IsNumber(rev) || !cJSON_IsString(u) || !cJSON_IsString(sha) ||
        !cJSON_IsNumber(size) || !cJSON_IsString(id)) {
        logline("manifest: missing field");
        goto out;
    }
    if (strlen(sha->valuestring) != 64) { logline("manifest: sha256 length"); goto out; }
    for (int i = 0; i < 32; i++) {
        int hi = hexval(sha->valuestring[2 * i]), lo = hexval(sha->valuestring[2 * i + 1]);
        if (hi < 0 || lo < 0) { logline("manifest: sha256 hex"); goto out; }
        m->sha256[i] = (unsigned char)(hi * 16 + lo);
    }
    if (!manifest_rev_in_range(rev->valuedouble)) {
        logline("manifest: rev out of range");
        goto out;
    }
    if (!manifest_size_in_range(size->valuedouble)) {
        logline("manifest: size out of range");
        goto out;
    }
    if (!manifest_id_is_safe(id->valuestring)) { logline("manifest: unusable id"); goto out; }
    if (expect_id && strcmp(expect_id, id->valuestring) != 0) {
        /* The catalog said which package this is. A manifest that renames
           itself would otherwise overwrite another package's record. */
        logline("manifest: id is not %s", expect_id);
        goto out;
    }
    m->rev = (unsigned)rev->valuedouble;
    m->size = (size_t)size->valuedouble;
    strncpy(m->id, id->valuestring, sizeof(m->id) - 1);
    strncpy(m->url, u->valuestring, sizeof(m->url) - 1);
    if (cJSON_IsObject(disp)) {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(disp, "version");
        if (cJSON_IsString(v)) strncpy(m->version, v->valuestring, sizeof(m->version) - 1);
    }
    logline("manifest: %s rev %u, %lu bytes", m->id, m->rev, (unsigned long)m->size);
    rc = 0;
out:
    cJSON_Delete(root);
    return rc;
}

/* ------------------------------------------------------------- download */

struct dl {
    int fd;
    wc_Sha256 sha;
    size_t written;
};

static int file_sink(void *ctx, const void *data, size_t len) {
    struct dl *d = ctx;
    if (wc_Sha256Update(&d->sha, data, (word32)len) != 0) return -1;
    while (len) {
        int n = sceIoWrite(d->fd, data, len);
        if (n <= 0) { logline("write failed %d", n); return -1; }
        data = (const char *)data + n;
        len -= (size_t)n;
        d->written += (size_t)n;
    }
    return 0;
}

static int download(const struct manifest *m, https_progress progress, void *pctx) {
    struct dl d;
    struct https_result r;
    d.written = 0;
    if (wc_InitSha256(&d.sha) != 0) return -1;

    d.fd = sceIoOpen(ARCHIVE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (d.fd < 0) { logline("cannot create %s", ARCHIVE); return -2; }

    unsigned start = now_ms();
    int rc = https_get(m->url, file_sink, &d, progress, pctx, &r);
    sceIoClose(d.fd);
    unsigned ms = now_ms() - start;
    logline("download: rc=%d status=%ld %lu bytes in %u.%us", rc, r.status,
            (unsigned long)d.written, ms / 1000, (ms % 1000) / 100);
    if (rc != 0 || r.status != 200) return -3;
    if (d.written != m->size) {
        logline("download: size %lu, manifest says %lu",
                (unsigned long)d.written, (unsigned long)m->size);
        return -4;
    }

    unsigned char digest[32];
    wc_Sha256Final(&d.sha, digest);
    if (memcmp(digest, m->sha256, 32) != 0) { logline("download: sha256 MISMATCH"); return -5; }
    logline("download: sha256 ok");
    return 0;
}

/* -------------------------------------------------------------- unpack */

static int mkdir_p(const char *path) {
    char tmp[256];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 5; *p; p++) {       /* skip "ms0:/" */
        if (*p == '/') { *p = '\0'; sceIoMkdir(tmp, 0777); *p = '/'; }
    }
    sceIoMkdir(tmp, 0777);
    return 0;
}

static int rm_rf(const char *path) {
    SceUID d = sceIoDopen(path);
    if (d < 0) return sceIoRemove(path) < 0 ? -1 : 0;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0) {
        if (strcmp(e.d_name, ".") == 0 || strcmp(e.d_name, "..") == 0) continue;
        char sub[256];
        if (snprintf(sub, sizeof(sub), "%s/%s", path, e.d_name) >= (int)sizeof(sub)) continue;
        if (FIO_S_ISDIR(e.d_stat.st_mode)) rm_rf(sub);
        else sceIoRemove(sub);
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
    return sceIoRmdir(path) < 0 ? -1 : 0;
}

static int safe_relative(const char *rel);

/* Where the package sits inside the archive: the directory of the
   shallowest EBOOT.PBP, which is the rule the catalog's scanner applies
   too. Of sixteen surveyed release archives, ten put the EBOOT one
   directory down, three at the root and two under PSP/GAME/; the root case
   has no directory name of its own and takes the last part of the id.
   root comes back with its trailing slash, or empty; dir is what the
   directory under PSP/GAME will be called. */
static void slashes(char *name) {
    for (char *p = name; *p; p++) if (*p == '\\') *p = '/';
}

static int ends_with_eboot(const char *name) {
    size_t n = strlen(name);
    if (n < 9) return 0;
    const char *tail = name + n - 9;
    static const char want[] = "EBOOT.PBP";
    for (int i = 0; i < 9; i++) {
        char c = tail[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c != want[i]) return 0;
    }
    return n == 9 || name[n - 10] == '/';
}

static int find_package(struct zipread *z, const char *id, char *root, size_t rootsz,
                        char *dir, size_t dirsz) {
    struct zipentry e;
    int rc, depth = -1, tied = 0;
    root[0] = dir[0] = '\0';
    for (rc = zip_first(z, &e); rc > 0; rc = zip_next(z, &e)) {
        if (e.name_truncated) continue;
        slashes(e.name);
        if (!ends_with_eboot(e.name)) continue;
        int d = 0;
        for (const char *p = e.name; *p; p++) d += *p == '/';
        if (depth < 0 || d < depth) {
            depth = d;
            tied = 0;
            const char *slash = strrchr(e.name, '/');
            size_t len = slash ? (size_t)(slash - e.name) + 1 : 0;
            if (len >= rootsz) { logline("unpack: package path too long"); return -1; }
            memcpy(root, e.name, len);
            root[len] = '\0';
        } else if (d == depth) {
            tied = 1;
        }
    }
    if (rc < 0) return -1;
    if (depth < 0) { logline("unpack: no EBOOT.PBP in archive"); return -1; }
    if (tied) { logline("unpack: two EBOOT.PBP at the same depth"); return -1; }
    if (!safe_relative(root)) { logline("unpack: refusing package at %s", root); return -1; }

    /* The directory's own name: the root's last part, or the id's. */
    const char *name;
    size_t len;
    if (root[0]) {
        const char *end = root + strlen(root) - 1;       /* the trailing slash */
        const char *start = end;
        while (start > root && start[-1] != '/') start--;
        name = start;
        len = (size_t)(end - start);
    } else {
        const char *dot = strrchr(id, '.');
        name = dot ? dot + 1 : id;
        len = strlen(name);
    }
    if (len == 0 || len >= dirsz) { logline("unpack: unusable directory name"); return -1; }
    memcpy(dir, name, len);
    dir[len] = '\0';
    logline("unpack: package at %s%s -> PSP/GAME/%s", root[0] ? root : "", root[0] ? "" : "(root)", dir);
    return 0;
}

/* A path component that walks anywhere but down is refused. */
static int safe_relative(const char *rel) {
    if (rel[0] == '/' || strstr(rel, "..")) return 0;
    if (strchr(rel, ':')) return 0;
    return 1;
}

struct out_file { int fd; size_t *done; };

static int out_sink(void *ctx, const void *data, size_t len) {
    struct out_file *o = ctx;
    int n = sceIoWrite(o->fd, data, len);
    if (n != (int)len) return -1;
    *o->done += len;
    return 0;
}

/* Everything under the package root goes into the staging directory;
   what the archive holds beside the package -- a readme at the top, a
   source tree -- stays in the archive. */
static int unpack(struct zipread *z, const char *root, struct install_report *rep,
                  https_progress progress, void *pctx) {
    size_t plen = strlen(root);
    struct zipentry e;
    int rc, files = 0;
    size_t total = 0, done = 0;

    for (rc = zip_first(z, &e); rc > 0; rc = zip_next(z, &e)) {
        slashes(e.name);
        if (strncmp(e.name, root, plen) == 0) total += e.usize;
    }
    if (rc < 0) return -1;
    if (progress) progress(pctx, 0, total);

    for (rc = zip_first(z, &e); rc > 0; rc = zip_next(z, &e)) {
        slashes(e.name);
        if (strncmp(e.name, root, plen) != 0) continue;   /* not ours */
        const char *rel = e.name + plen;
        if (*rel == '\0') continue;
        if (e.name_truncated || !safe_relative(rel)) { logline("unpack: refusing %s", e.name); return -1; }

        char path[256];
        if (snprintf(path, sizeof(path), STAGE "/%s", rel) >= (int)sizeof(path)) {
            logline("unpack: path too long: %s", rel);
            return -1;
        }
        size_t L = strlen(path);
        if (path[L - 1] == '/') { path[L - 1] = '\0'; mkdir_p(path); continue; }

        char *slash = strrchr(path, '/');
        if (slash) { *slash = '\0'; mkdir_p(path); *slash = '/'; }

        struct out_file o;
        o.done = &done;
        o.fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (o.fd < 0) { logline("unpack: cannot create %s", rel); return -1; }
        int xrc = zip_extract(z, &e, out_sink, &o);
        /* On FAT32 the write that fails is often the close: the stick fills up
           while earlier writes were still buffered. */
        if (sceIoClose(o.fd) < 0) { logline("unpack: close failed on %s", rel); return -1; }
        if (xrc < 0) { logline("unpack: failed on %s", rel); return -1; }
        files++;
        if (progress) progress(pctx, done, total);
    }
    if (rc < 0) return -1;

    rep->files = files;
    rep->bytes = done;
    logline("unpack: %d files, %lu bytes", files, (unsigned long)done);
    return 0;
}

/* ------------------------------------------------------------------- db */

/* What the client will need to uninstall or update later: which directory it
   actually wrote, and which manifest to ask. */
/* Written to a temporary name and renamed over the old record, so a stick that
   fills up or a battery that dies leaves the previous record intact rather
   than an empty file where the package's identity used to be. */
static int db_write(const struct manifest *m, const char *dir) {
    char path[256], tmpname[128];
    mkdir_p(DB_DIR);
    snprintf(path, sizeof(path), DB_DIR "/%s.json.new", m->id);
    snprintf(tmpname, sizeof(tmpname), "%s.json", m->id);

    int fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) { logline("db: cannot write %s", m->id); return -1; }
    char line[1024];
    int n = snprintf(line, sizeof(line),
                     "{\"id\":\"%s\",\"rev\":%u,\"dir\":\"%s\",\"manifest\":\"%s\",\"version\":\"%s\"}\n",
                     m->id, m->rev, dir, m->manifest_url, m->version);
    if (n <= 0 || n >= (int)sizeof(line)) { sceIoClose(fd); return -1; }
    int w = sceIoWrite(fd, line, (SceSize)n);
    if (sceIoClose(fd) < 0 || w != n) { logline("db: %s not persisted", m->id); return -1; }
    if (sceIoRename(path, tmpname) < 0) { logline("db: cannot commit %s", m->id); return -1; }
    return 0;
}

/* The same file from fields rather than from a manifest: PSPDX's own record,
   which no install ever wrote. The manifest URL is left empty -- the catalog
   carries the client's release like everyone else's, so nothing would ever
   read it. */
int db_write_record(const struct installed *record) {
    struct manifest m;
    if (!manifest_id_is_safe(record->id)) { logline("db: unusable id"); return -1; }
    memset(&m, 0, sizeof(m));
    snprintf(m.id, sizeof(m.id), "%s", record->id);
    snprintf(m.version, sizeof(m.version), "%s", record->version);
    m.rev = record->rev;
    return db_write(&m, record->dir);
}

/* Reads what is installed for one id. Returns 0 if a record exists. */
int db_read(const char *id, struct installed *out) {
    char path[256];
    snprintf(path, sizeof(path), DB_DIR "/%s.json", id);
    int fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0) return -1;
    char buf[640];
    int n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) return -1;

    cJSON *root = cJSON_ParseWithLength(buf, (size_t)n);
    if (!root) { logline("db: %s is not json", id); return -1; }
    memset(out, 0, sizeof(*out));
    cJSON *rev = cJSON_GetObjectItemCaseSensitive(root, "rev");
    cJSON *dir = cJSON_GetObjectItemCaseSensitive(root, "dir");
    cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "version");
    int ok = cJSON_IsNumber(rev);
    if (ok) {
        out->rev = (unsigned)rev->valuedouble;
        strncpy(out->id, id, sizeof(out->id) - 1);
        if (cJSON_IsString(dir)) strncpy(out->dir, dir->valuestring, sizeof(out->dir) - 1);
        if (cJSON_IsString(ver)) strncpy(out->version, ver->valuestring, sizeof(out->version) - 1);
    }
    cJSON_Delete(root);
    return ok ? 0 : -1;
}

int uninstall(const char *id) {
    struct installed rec;
    if (!manifest_id_is_safe(id)) { logline("uninstall: unusable id"); return -1; }
    if (db_read(id, &rec) < 0) { logline("uninstall: no record for %s", id); return -2; }
    /* The record is a file on a stick anyone can edit, and what follows is a
       recursive delete: only a plain directory name under PSP/GAME is ever
       acted on, never a path. */
    if (!rec.dir[0] || strpbrk(rec.dir, "/\\:") || strstr(rec.dir, "..") ||
        strcmp(rec.dir, ".") == 0) {
        logline("uninstall: %s names no directory of its own", id);
        return -3;
    }

    char dest[128], record[256];
    snprintf(dest, sizeof(dest), GAME_DIR "/%s", rec.dir);
    snprintf(record, sizeof(record), DB_DIR "/%s.json", id);
    /* A directory that was already gone by hand is not a failure: the record
       is what makes the client think the package is there. */
    SceUID probe = sceIoDopen(dest);
    if (probe >= 0) {
        sceIoDclose(probe);
        if (rm_rf(dest) < 0) { logline("uninstall: %s did not go", dest); return -4; }
    }
    if (sceIoRemove(record) < 0) { logline("uninstall: record %s stayed", id); return -5; }
    logline("uninstalled %s -> PSP/GAME/%s gone", id, rec.dir);
    return 0;
}

/* --------------------------------------------------------------- install */

/* Finishes an install that the battery interrupted between the two renames.
   The window is one rename wide: the old copy is called <dir>.old and the new
   one is not in place yet, so a package can look uninstalled while its files
   are still there. Called once at startup. */
void install_recover(void) {
    SceUID d = sceIoDopen(GAME_DIR);
    if (d < 0) return;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0) {
        size_t n = strlen(e.d_name);
        if (n > 4 && strcmp(e.d_name + n - 4, ".old") == 0) {
            char live[128], old[192];
            snprintf(live, sizeof(live), "%.*s", (int)(n - 4), e.d_name);
            snprintf(old, sizeof(old), GAME_DIR "/%s", e.d_name);
            char livepath[192];
            snprintf(livepath, sizeof(livepath), GAME_DIR "/%s", live);
            SceUID probe = sceIoDopen(livepath);
            if (probe >= 0) {
                sceIoDclose(probe);           /* the new copy made it; drop the old */
                rm_rf(old);
                logline("recovered: dropped %s", e.d_name);
            } else if (sceIoRename(old, live) >= 0) {
                logline("recovered: restored %s", live);
            }
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
    rm_rf(STAGE);
}

int install(const char *manifest_url, const char *expect_id,
            struct install_report *rep,
            install_phase_cb phase, https_progress progress, void *pctx) {
    struct manifest m;
    memset(rep, 0, sizeof(*rep));

    if (phase) phase(pctx, "manifest");
    if (manifest_fetch(manifest_url, expect_id, &m) < 0) return -1;
    strncpy(m.manifest_url, manifest_url, sizeof(m.manifest_url) - 1);
    return install_release(&m, rep, phase, progress, pctx);
}

int install_release(const struct manifest *release, struct install_report *rep,
                    install_phase_cb phase, https_progress progress, void *pctx) {
    struct manifest m = *release;
    memset(rep, 0, sizeof(*rep));
    strncpy(rep->id, m.id, sizeof(rep->id) - 1);
    strncpy(rep->version, m.version, sizeof(rep->version) - 1);
    rep->rev = m.rev;

    mkdir_p(TMP_DIR);
    mkdir_p(GAME_DIR);
    /* A staging tree left by an earlier failure must be gone, not merged into:
       its files would be committed as part of this package. */
    rm_rf(STAGE);
    SceUID leftover = sceIoDopen(STAGE);
    if (leftover >= 0) {
        sceIoDclose(leftover);
        logline("install: cannot clear the staging directory");
        return -6;
    }

    if (phase) phase(pctx, "download");
    if (download(&m, progress, pctx) < 0) { sceIoRemove(ARCHIVE); return -2; }

    if (phase) phase(pctx, "unpack");
    char dir[64], root[200];
    struct zipread z;
    if (zip_open(&z, ARCHIVE) < 0) {
        logline("unpack: cannot open archive");
        sceIoRemove(ARCHIVE);
        return -3;
    }
    int rc = find_package(&z, m.id, root, sizeof(root), dir, sizeof(dir));
    if (rc == 0) {
        strncpy(rep->dir, dir, sizeof(rep->dir) - 1);
        mkdir_p(STAGE);
        rc = unpack(&z, root, rep, progress, pctx);
    }
    zip_close(&z);
    sceIoRemove(ARCHIVE);
    if (rc < 0) { rm_rf(STAGE); return -4; }

    /* The commit. A previous copy steps aside as .old until the new one is in
       place; without a mirror that is the only rollback there is. */
    if (phase) phase(pctx, "commit");
    char dest[128], old[128], oldname[80];
    snprintf(dest, sizeof(dest), GAME_DIR "/%s", dir);
    snprintf(old, sizeof(old), GAME_DIR "/%s.old", dir);
    snprintf(oldname, sizeof(oldname), "%s.old", dir);
    rm_rf(old);
    sceIoRename(dest, oldname);               /* fails harmlessly if absent */
    if (sceIoRename(STAGE, dir) < 0) {
        logline("commit: rename failed, restoring");
        sceIoRename(old, dir);
        rm_rf(STAGE);
        return -5;
    }
    rm_rf(old);
    if (db_write(&m, dir) < 0) {
        /* The files are in place but nothing remembers them, so the next run
           would offer the package as uninstalled and write over it. */
        logline("installed, but the record did not persist");
        return -7;
    }
    logline("installed %s -> PSP/GAME/%s", m.id, dir);
    return 0;
}
