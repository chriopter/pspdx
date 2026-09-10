#ifndef PSPDX_INSTALL_H
#define PSPDX_INSTALL_H

#include <stddef.h>
#include "pspdx.h"

/* The fields the client acts on. Everything under "display" is shown, never
   compared -- version is carried along only to be printed. */
struct manifest {
    char id[96];
    unsigned rev;               /* unix seconds set by the publish step */
    char url[512];
    unsigned char sha256[32];
    size_t size;
    char version[32];
    char manifest_url[512];
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

typedef void (*install_phase_cb)(void *ctx, const char *phase);

int manifest_fetch(const char *url, struct manifest *m);

/* Fetch manifest, download, verify, unpack, rename into place. Returns 0 on
   success; negative on the phase that failed. Nothing is left half-written. */
int install(const char *manifest_url, struct install_report *rep,
            install_phase_cb phase, https_progress progress, void *pctx);

#endif
