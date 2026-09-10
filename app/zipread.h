#ifndef PSPDX_ZIPREAD_H
#define PSPDX_ZIPREAD_H

#include <stddef.h>
#include <stdint.h>

struct zipread {
    int fd;
    uint32_t entries, cd_off, cd_size;
    uint32_t cd_pos, index;      /* iteration state */
};

struct zipentry {
    char name[256];
    uint16_t method;             /* 0 stored, 8 deflate */
    uint32_t csize, usize, local_off;
    int encrypted, name_truncated;
};

int zip_open(struct zipread *z, const char *path);
void zip_close(struct zipread *z);
int zip_first(struct zipread *z, struct zipentry *e);
int zip_next(struct zipread *z, struct zipentry *e);
int zip_extract(struct zipread *z, const struct zipentry *e,
                int (*sink)(void *ctx, const void *data, size_t len), void *ctx);

#endif
