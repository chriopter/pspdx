#ifndef PSPDX_INSTALL_H
#define PSPDX_INSTALL_H

#include <stddef.h>
#include "network/https.h"

/* One release, as the console installs it: the fields that go into a
   download and an unpack, and the repository it came from, which goes
   into the record on the stick so the package can be found at its source
   again. A sha256 of all zeros means nobody has hashed the zip: the origin
   path gives none, and the size is what is checked then. */
struct manifest {
    char id[96];
    unsigned rev;               /* the release's published_at, unix seconds */
    char url[512];
    unsigned char sha256[32];
    size_t size;
    char version[32];
    char repo[256];
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
    char repo[256];
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

/* The rules a release's fields are held to by whoever reads them out of
   JSON -- a cache's or GitHub's -- since they go straight into a download
   and an unpack. An id is a path component on the stick: letters, digits,
   dot, dash and underscore, at most eighty of them, no "..". A package is
   at most a gigabyte, and a revision fits an unsigned. */
#define MAX_PACKAGE_BYTES (1024u * 1024u * 1024u)
int manifest_id_is_safe(const char *id);
int manifest_rev_in_range(double rev);
int manifest_size_in_range(double size);
int manifest_has_sha256(const struct manifest *m);

/* Finishes an install interrupted between its two renames. Call once at
   startup, before anything reads the database. */
void install_recover(void);

/* Download, verify, unpack, rename into place. Returns 0 on success;
   negative on the phase that failed. Nothing is left half-written. */
int install_release(const struct manifest *release, struct install_report *rep,
                    install_phase_cb phase, https_progress progress, void *pctx);

#endif
