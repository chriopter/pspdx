#ifndef PSPDX_HTTPS_H
#define PSPDX_HTTPS_H

#include <stddef.h>

int net_up(void);
void net_down(void);

typedef int (*https_sink)(void *ctx, const void *data, size_t len);
typedef void (*https_progress)(void *ctx, size_t done, size_t total);

struct https_result {
    long status;
    size_t body_len;
    size_t content_length;
    int truncated;
    unsigned handshake_ms;
    int redirects;
    char host[128];
};

/* The cipher suites to offer first, in wolfSSL's spelling, or NULL for the
   library's own order. Applies to connections made after the call. */
void https_prefer(const char *suites);

/* What the stack is doing at this moment -- "dns", "tls handshake",
   "download" -- for a status line. */
const char *https_phase(void);

int https_get(const char *url, https_sink sink, void *sink_ctx,
              https_progress progress, void *progress_ctx,
              struct https_result *out);

#endif
