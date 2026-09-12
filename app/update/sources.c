#include <pspiofilemgr.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "update/sources.h"
#include "util/runtime.h"

#define SOURCES_DIR  "ms0:/PSP/PSPDX"
#define SOURCES_PATH SOURCES_DIR "/sources.txt"

static char g_text[8 * 1024];

static int read_file(void) {
    int fd = sceIoOpen(SOURCES_PATH, PSP_O_RDONLY, 0777);
    if (fd < 0) return -1;
    int n = sceIoRead(fd, g_text, sizeof(g_text) - 1);
    sceIoClose(fd);
    if (n < 0) n = 0;
    g_text[n] = '\0';
    return n;
}

static int write_default(void) {
    sceIoMkdir(SOURCES_DIR, 0777);
    int fd = sceIoOpen(SOURCES_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) { logline("sources: cannot write %s", SOURCES_PATH); return -1; }
    static const char head[] =
        "# sources.txt -- one URL a line: a list, a GitHub repository, or a "
        "catalog.json\n" SOURCES_DEFAULT "\n";
    int w = sceIoWrite(fd, head, sizeof(head) - 1);
    sceIoClose(fd);
    return w == (int)sizeof(head) - 1 ? 0 : -1;
}

/* One line, its ends trimmed and a comment cut off. Returns the length. */
static int trim(char *line) {
    char *hash = strchr(line, '#');
    if (hash) *hash = '\0';
    size_t n = strlen(line);
    while (n && isspace((unsigned char)line[n - 1])) line[--n] = '\0';
    size_t lead = 0;
    while (line[lead] && isspace((unsigned char)line[lead])) lead++;
    if (lead) memmove(line, line + lead, n - lead + 1);
    return (int)(n - lead);
}

/* Two URLs that differ only by case or a trailing slash name the same
   place, and one of them in the file is enough. */
int sources_same_url(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    while (na && a[na - 1] == '/') na--;
    while (nb && b[nb - 1] == '/') nb--;
    if (na != nb) return 0;
    for (size_t i = 0; i < na; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

int sources_load(struct sources *s) {
    memset(s, 0, sizeof(*s));
    if (read_file() < 0) {
        if (write_default() < 0 || read_file() < 0) {
            /* No file and no way to make one: the built-in list still
               stands, so the browser is not empty for want of a stick. */
            snprintf(s->url[0], SOURCE_URL, "%s", SOURCES_DEFAULT);
            s->count = 1;
            return 1;
        }
        logline("sources: wrote the built-in list");
    }
    char *line = g_text;
    while (line && *line && s->count < SOURCES_MAX) {
        char *end = strpbrk(line, "\r\n");
        if (end) *end++ = '\0';
        if (trim(line) > 0 && strncmp(line, "https://", 8) == 0) {
            int dup = 0;
            for (int i = 0; i < s->count; i++) dup = dup || sources_same_url(s->url[i], line);
            /* A line longer than a URL slot is not a URL anyone typed. */
            if (!dup && strlen(line) < SOURCE_URL) {
                memcpy(s->url[s->count], line, strlen(line) + 1);
                s->count++;
            }
        }
        line = end;
        while (line && (*line == '\r' || *line == '\n')) line++;
    }
    return s->count;
}

int sources_add(const char *text, char *url, size_t size) {
    char line[SOURCE_URL];
    snprintf(line, sizeof(line), "%s", text ? text : "");
    if (trim(line) <= 0 || strchr(line, ' ')) return -1;
    if (strncmp(line, "https://", 8) == 0) snprintf(url, size, "%s", line);
    else if (strncmp(line, "http://", 7) == 0) snprintf(url, size, "https://%s", line + 7);
    else if (strncmp(line, "github.com/", 11) == 0) snprintf(url, size, "https://%s", line);
    else {
        /* "owner/repo" and nothing else: one slash, both halves there. */
        const char *slash = strchr(line, '/');
        if (!slash || slash == line || !slash[1] || strchr(slash + 1, '/')) return -1;
        snprintf(url, size, "https://github.com/%s", line);
    }
    struct sources have;
    sources_load(&have);
    for (int i = 0; i < have.count; i++)
        if (sources_same_url(have.url[i], url)) return 0;
    if (have.count >= SOURCES_MAX) { logline("sources: the file is full"); return -1; }

    int fd = sceIoOpen(SOURCES_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd < 0) { logline("sources: cannot append to %s", SOURCES_PATH); return -1; }
    snprintf(line, sizeof(line), "%s\n", url);
    int w = sceIoWrite(fd, line, strlen(line));
    sceIoClose(fd);
    if (w != (int)strlen(line)) return -1;
    logline("sources: added %s", url);
    return 1;
}

enum source_kind sources_kind(const char *url) {
    size_t n = strlen(url);
    while (n && url[n - 1] == '/') n--;
    if (n > 5 && strncmp(url + n - 5, ".json", 5) == 0) return SOURCE_CATALOG;
    struct source_repo r;
    if (sources_parse_repo(url, &r)) return SOURCE_REPO;
    return SOURCE_LIST;
}

int sources_parse_repo(const char *url, struct source_repo *out) {
    static const char host[] = "https://github.com/";
    memset(out, 0, sizeof(*out));
    if (strncmp(url, host, sizeof(host) - 1) != 0) return 0;
    const char *p = url + sizeof(host) - 1;
    const char *slash = strchr(p, '/');
    if (!slash || slash == p) return 0;
    size_t n = (size_t)(slash - p);
    if (n >= sizeof(out->owner)) return 0;
    memcpy(out->owner, p, n);
    p = slash + 1;
    /* The name runs to an "@tag", a slash, or the end; a trailing ".git"
       is how git spells the same repository. */
    size_t len = strcspn(p, "@/");
    if (!len || len >= sizeof(out->name)) return 0;
    memcpy(out->name, p, len);
    if (len > 4 && strcmp(out->name + len - 4, ".git") == 0) out->name[len - 4] = '\0';
    if (p[len] == '/' && p[len + 1] && p[len + 1] != '@') return 0;
    const char *at = strchr(p + len, '@');
    if (at && at[1]) snprintf(out->ref, sizeof(out->ref), "%s", at + 1);
    else snprintf(out->ref, sizeof(out->ref), "HEAD");
    return 1;
}

int sources_parse_list(const char *text, struct source_list *out) {
    memset(out, 0, sizeof(*out));
    static char copy[64 * 1024];
    snprintf(copy, sizeof(copy), "%s", text);
    char *line = copy;
    while (line && *line && out->count < LIST_REPOS) {
        char *end = strpbrk(line, "\r\n");
        if (end) *end++ = '\0';
        if (trim(line) > 0) {
            if (strncmp(line, "cache ", 6) == 0) {
                char *url = line + 6;
                trim(url);
                if (!out->cache[0]) snprintf(out->cache, sizeof(out->cache), "%s", url);
            } else {
                /* The URL is the whole line. Anything after it is from a
                   list written for the older shape, where the category and
                   the overrides stood there; it is passed over rather than
                   made to refuse the repository, since the .pspdx says all
                   of it now. */
                line[strcspn(line, " \t")] = '\0';
                if (sources_parse_repo(line, &out->repo[out->count]))
                    out->count++;
            }
        }
        line = end;
        while (line && (*line == '\r' || *line == '\n')) line++;
    }
    return out->count;
}

/* The id names a directory on the stick, so only [a-z0-9] of the owner
   and the repository survive in it: "Chris-Opter/PSP-Thing" becomes
   io.github.chrisopter.pspthing. */
static size_t append_plain(char *id, size_t n, size_t size, const char *text) {
    for (const char *p = text; *p && n + 1 < size; p++) {
        int c = tolower((unsigned char)*p);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) id[n++] = (char)c;
    }
    id[n] = '\0';
    return n;
}

void sources_repo_id(const struct source_repo *r, char *id, size_t size) {
    size_t n = (size_t)snprintf(id, size, "io.github.");
    n = append_plain(id, n, size, r->owner);
    if (n + 1 < size) id[n++] = '.';
    append_plain(id, n, size, r->name);
}

void sources_repo_url(const struct source_repo *r, char *url, size_t size) {
    snprintf(url, size, "https://github.com/%s/%s", r->owner, r->name);
}
