#ifndef PSPDX_PSPDX_H
#define PSPDX_PSPDX_H

#include <stddef.h>
#include "update/catalog.h"     /* MAX_SUMMARY: the card shows no more */

/* The .pspdx in a repository's root: the author's consent and their words.
   A repository without one is not an app, by any list, so reading it is the
   first thing the origin path does and a refusal ends the entry there.

   The console reads what it needs and lets the rest be. The cache refuses
   an unknown key, which is where a misspelt field is meant to be noticed;
   a console that did the same would lose an app to every field added after
   its own build, on a machine nobody can update in a hurry. */

struct pspdx_file {
    char name[40];              /* required */
    char category[12];          /* required, one of the five */
    char author[40];
    char summary[MAX_SUMMARY];
    char license[16];
    char media[128];            /* the directory, no slashes at its ends */
    char asset[64];             /* a glob naming the zip */
    char release[40];           /* a tag, to pin */
    char root[200];             /* install.root */
    char dir[64];               /* install.dir */
};

/* One .pspdx parsed and checked. Returns 0 when the repository is an app;
   -1 when it is not, with why written into reason, which is what the status
   line shows and what the log says. */
int pspdx_parse(const char *text, size_t len, struct pspdx_file *out,
                char *reason, size_t reason_size);

#endif
