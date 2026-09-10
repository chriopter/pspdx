#ifndef PSPDX_CATALOG_H
#define PSPDX_CATALOG_H

#include "network/https.h"

#define MAX_APPS 64
#define MAX_SUMMARY 60

enum app_state { APP_UNKNOWN, APP_NOT_INSTALLED, APP_CURRENT, APP_UPDATE };

struct app_entry {
    char id[96];
    char name[40];
    char author[40];
    char summary[MAX_SUMMARY];
    char category[12];
    char license[16];
    char manifest[256];
    /* Absolute already: the catalog serves these relative to itself, and
       resolving them once at parse time keeps the base URL in this file. */
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
int catalog_check_updates(struct catalog *catalog);
void catalog_dump_http(void);

#endif
