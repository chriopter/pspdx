#ifndef PSPDX_SOURCES_H
#define PSPDX_SOURCES_H

#include <stddef.h>

/* Where the console finds apps: PSP/PSPDX/sources.txt, one URL a line, the
   built-in list first. A line is one of three things -- a list (text with an
   optional "cache <url>" line and one GitHub repository a line), a single
   repository, which is a list of one, or a catalog.json, which is a cache
   with no list behind it. The file is read at every catalog fetch and written
   to by the gear tab; nothing else touches it. */

#define SOURCES_MAX 16
#define SOURCE_URL 256
#define SOURCES_DEFAULT "https://raw.githubusercontent.com/chriopter/pspdx-catalog/HEAD/repos.txt"

struct sources {
    char url[SOURCES_MAX][SOURCE_URL];
    int count;
};

/* Reads the file, writing it first with the built-in list when it is
   missing. Returns how many sources there are. */
int sources_load(struct sources *s);

/* What the user typed, made into a source: "owner/repo" becomes the
   repository's URL, a bare github.com address gets its scheme, a full URL
   is taken as it is. Appends it to the file unless it is there already.
   Returns 1 added, 0 already present, -1 for something that is not a URL.
   url receives the normalised form either way. */
int sources_add(const char *text, char *url, size_t size);

enum source_kind { SOURCE_LIST, SOURCE_REPO, SOURCE_CATALOG };
enum source_kind sources_kind(const char *url);

/* One line of a list: https://github.com/<owner>/<repo>[@tag] category
   [key=value ...]. The category is the word after the URL and "apps" when
   there is none; the words after it override what would be derived, for
   the title that is an abbreviation in the SFO or the licence GitHub
   cannot see. A single repository typed by the user is a line with nothing
   after the URL. */
struct source_override {
    char name[40];
    char author[40];
    char summary[60];
    char license[16];
    char asset[64];             /* a glob naming the zip when there is more than one */
};

struct source_repo {
    char owner[40];
    char name[100];
    char ref[40];               /* the tag, or HEAD when none is pinned */
    char category[12];
    struct source_override over;
};

#define LIST_REPOS 64
struct source_list {
    char cache[SOURCE_URL];     /* the "cache" line, empty when there is none */
    struct source_repo repo[LIST_REPOS];
    int count;
};

/* The list's text into its parts; lines that name nothing this understands
   are passed over. Returns the number of repositories. */
int sources_parse_list(const char *text, struct source_list *out);

/* A repository URL into its parts, category "apps" and no overrides.
   Returns 0 when it is not one. */
int sources_parse_repo(const char *url, struct source_repo *out);

/* Two URLs that differ only by case or a trailing slash name the same
   place. */
int sources_same_url(const char *a, const char *b);

/* The id the repository derives to: io.github.<owner>.<repo>, lower case,
   [a-z0-9] only, so that "Chris-Opter/PSP-Thing" owns io.github.chrisopter.pspthing.
   It names a directory on the stick and never changes. */
void sources_repo_id(const struct source_repo *r, char *id, size_t size);

/* The repository's canonical URL, https://github.com/<owner>/<repo>, with
   no tag on it: what the record on the stick and the cache both call the
   repository, so the two can be compared. */
void sources_repo_url(const struct source_repo *r, char *url, size_t size);

#endif
