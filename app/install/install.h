#ifndef PSPDX_INSTALL_H
#define PSPDX_INSTALL_H

#include <stddef.h>
#include "network/https.h"

/* What a manifest must say it is. The value is a page describing the format,
   so a file found years from now points at its own documentation. */
#define PSPDX_SCHEMA "https://github.com/chriopter/pspdx/blob/master/manifest.md"

/* The fields the client acts on, and under them the author's half, which is
   shown and never compared -- version included, which is carried along only
   to be printed. The text fields are optional in the file and empty when
   absent; their sizes are the catalog entry's, since that is where they
   end up. */
struct manifest {
    char id[96];
    unsigned rev;               /* unix seconds set by the publish step */
    char url[512];
    unsigned char sha256[32];
    size_t size;
    char version[32];
    char manifest_url[512];
    char name[40];
    char author[40];
    char summary[60];
    char category[12];
    char license[16];
};

struct install_report {
    char id[96];
    char dir[64];               /* PSP/GAME/<dir> actually written */
    char version[32];
    unsigned rev;
    int files;
    size_t bytes;
};

/* What PSP/PSPDX/db/<id>.json remembers about an installed package. */
struct installed {
    char id[96];
    char dir[64];
    char version[32];
    unsigned rev;
};

int db_read(const char *id, struct installed *out);

/* A record for a package this client did not unpack. There is exactly one --
   PSPDX's own, which is on the stick because somebody copied it there, and
   which still needs a record to be an app like the others. Written the same
   way an install writes one, through a rename, so it is never half a file. */
int db_write_record(const struct installed *record);

/* Removes PSP/GAME/<dir> and forgets the record, in that order: a directory
   left behind with no record would be offered as uninstalled and written over,
   while a record with no directory only costs one line in the database. The
   directory comes from the record and is refused unless it is a plain name --
   nothing here may be talked into deleting a path of someone else's choosing.
   Returns 0 when the package is gone. */
int uninstall(const char *id);

typedef void (*install_phase_cb)(void *ctx, const char *phase);

/* expect_id, when given, is the id the catalog promised: a manifest that
   claims a different one is refused rather than allowed to overwrite another
   package's record. A list promises less -- whose app it is, not which --
   so an expect_id ending in a dot, "io.github.<owner>.", is a prefix the
   file's id has to begin with, the rest being the author's to choose. */
int manifest_fetch(const char *url, const char *expect_id, struct manifest *m);

/* The rules a manifest's fields are held to, for whoever else reads the
   same fields -- the catalog folds a release in and has to be as strict. An
   id is a path component on the stick: letters, digits, dot, dash and
   underscore, at most eighty of them, no "..". A package is at most a
   gigabyte, and a revision fits an unsigned. */
#define MAX_PACKAGE_BYTES (1024u * 1024u * 1024u)
int manifest_id_is_safe(const char *id);
int manifest_rev_in_range(double rev);
int manifest_size_in_range(double size);

/* Finishes an install interrupted between its two renames. Call once at
   startup, before anything reads the database. */
void install_recover(void);

/* Fetch manifest, download, verify, unpack, rename into place. Returns 0 on
   success; negative on the phase that failed. Nothing is left half-written. */
int install(const char *manifest_url, const char *expect_id,
            struct install_report *rep,
            install_phase_cb phase, https_progress progress, void *pctx);

/* The same from a manifest already in hand -- the catalog carries each
   entry's release, so there is nothing to fetch first. */
int install_release(const struct manifest *release, struct install_report *rep,
                    install_phase_cb phase, https_progress progress, void *pctx);

#endif
