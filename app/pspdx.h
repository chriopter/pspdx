/* Shared between the translation units of the client. */
#ifndef PSPDX_H
#define PSPDX_H

#include <stddef.h>

#define COLS 60

/* One line into the in-memory log that ends up in PSPDX.LOG. main.c owns it. */
void logline(const char *fmt, ...);

unsigned now_ms(void);
int expired(unsigned start, unsigned budget_ms);

/* Bits of entropy the pool holds; wolfSSL seeds from it. psprandom in main.c. */
extern int g_pool_bits;

/* ------------------------------------------------------------------ https */

/* Bring the WLAN up on connection profile 1. */
int net_up(void);
void net_down(void);

/* Receives body bytes as they arrive. Return 0 to continue, nonzero to abort. */
typedef int (*https_sink)(void *ctx, const void *data, size_t len);

/* Called with progress; total is 0 when the server sent no Content-Length. */
typedef void (*https_progress)(void *ctx, size_t done, size_t total);

struct https_result {
    long status;            /* HTTP status of the final response */
    size_t body_len;        /* bytes handed to the sink */
    size_t content_length;  /* what the server announced, 0 if nothing */
    int truncated;          /* body_len < content_length, or reset mid-body */
    unsigned handshake_ms;  /* of the last connection */
    int redirects;
    char host[128];         /* host of the final response */
};

/* GET an https:// URL, following up to 5 redirects, streaming the body into
   the sink. Returns 0 on a complete body, 1 on a truncated one, <0 on failure
   before any body arrived. */
int https_get(const char *url, https_sink sink, void *sink_ctx,
              https_progress progress, void *progress_ctx,
              struct https_result *out);

#endif
