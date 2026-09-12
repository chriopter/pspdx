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

/* One line of a list: https://github.com/<owner>/<repo>[@tag]. */
struct source_repo {
    char owner[40];
    char name[100];
    char ref[40];               /* the tag, or HEAD when none is pinned */
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

/* A repository URL into its parts. Returns 0 when it is not one. */
int sources_parse_repo(const char *url, struct source_repo *out);

/* What a repository's app.pspdx has to say it is: an id beginning with
   io.github.<owner>. -- lower case, anything outside [a-z0-9] dropped --
   the rest being the author's choice. In the form manifest_fetch takes as a
   prefix. */
void sources_repo_expect(const struct source_repo *r, char *id, size_t size);

/* Where the file is: the tree's copy at HEAD, or the copy the action
   attached to the release when the line pins a tag -- the tag was cut
   before the action ran, so the tree at the tag still holds the release
   before. */
void sources_repo_manifest(const struct source_repo *r, char *url, size_t size);

#endif
