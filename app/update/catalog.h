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
       a manifest to fetch instead. */
    struct manifest release;
    int has_release;
    char manifest[256];
    /* Absolute already: the catalog serves these relative to itself, and
       resolving them once at parse time keeps the base URL in this file. */
    char icon[256];
    char screenshot[256];
    char video[256];
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

int catalog_fetch(struct catalog *catalog);
const char *catalog_url(void);
int catalog_check_updates(struct catalog *catalog);
void catalog_dump_http(void);

#endif
