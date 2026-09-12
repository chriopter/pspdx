#ifndef PSPDX_CATALOG_H
#define PSPDX_CATALOG_H

#include "network/https.h"
#include "install/install.h"

#define MAX_APPS 64
#define MAX_SUMMARY 60

/* PSPDX is an app in its own catalog, and a few things have to know which row
   is the client itself: the one that cannot be removed while it is running,
   and the one whose record was written by a first start rather than by an
   install. The id is written once, here. */
#define PSPDX_SELF_ID "io.github.chriopter.pspdx"

enum app_state { APP_UNKNOWN, APP_NOT_INSTALLED, APP_CURRENT, APP_UPDATE };

struct app_entry {
    char id[96];
    char name[40];
    char author[40];
    char summary[MAX_SUMMARY];
    char category[12];
    char license[16];
    /* The catalog folds each app's manifest in as "release", so what is
       current is known without a fetch per app. An entry without one names
       a manifest to fetch instead. manifest is the raw app.pspdx URL the
       entry was read from, or that the cache read it from: it goes into
       the record on the stick, so a package installed from a cache can
       later update from its source. */
    struct manifest release;
    int has_release;
    char manifest[256];
    /* Absolute already: the catalog serves these relative to itself, and
       resolving them once at parse time keeps the base URL in this file. */
    char icon[256];
    char screenshot[256];
    char video[256];
    char sound[256];            /* SND0.AT3 out of the EBOOT, the card's own loop */
    enum app_state state;
    unsigned local_rev, remote_rev;
    char local_version[32], remote_version[32];
};

struct catalog {
    struct app_entry apps[MAX_APPS];
    int count;
    int total;
    size_t response_len;
    struct https_result fetch;
};

/* Every source in PSP/PSPDX/sources.txt, in order, merged into one catalog
   by id, the first to name an id winning. A list's cache is taken when it
   answers and the list walked when it does not. Returns the number of apps,
   or -1 when no source answered at all. */
int catalog_fetch(struct catalog *catalog);

/* The cache last taken, or the built-in one before any was: what the info
   band names as the catalog and what the bench fetches. */
const char *catalog_url(void);

/* Where a fetch has got to, for the status line -- "list: 3 of 12" while a
   list is walked -- and empty when there is nothing more to say than what
   the network stack says. Written by the fetching thread. */
const char *catalog_progress(void);

/* Why a walked repository's app.pspdx, named by its URL, did not make it
   into the catalog: 0 if it did or was never asked for, -1 if the file
   could not be fetched, below that if it was fetched and refused. Only the
   last one is kept, which is the one the gear tab just asked for. */
int catalog_refused(const char *url);

int catalog_check_updates(struct catalog *catalog);
void catalog_dump_http(void);

#endif
