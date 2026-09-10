/*
 * HTTPS on the PSP: sceNetInet sockets under wolfSSL, and just enough HTTP/1.1
 * to stream one body of any size into a sink. Every request is its own
 * connection; keep-alive is a later optimisation, redirects are followed by
 * reconnecting.
 *
 * The four traps in this file each cost an afternoon and none is documented:
 * the BSD socket wrappers return garbage, sceNetInetSelect hangs, SO_NONBLOCK
 * and SO_ERROR do not exist in the headers, and retrying EINTR inside an IO
 * callback spins forever inside the handshake.
 */

#include <pspkernel.h>
#include <psputility.h>
#include <psputility_netmodules.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

#include "pspdx.h"

#define PORT 443
#define MAX_REDIRECTS 5
#define HEAD_MAX (8 * 1024)

#define CONNECT_TIMEOUT_MS   10000
#define HANDSHAKE_TIMEOUT_MS 20000
/* Per read, not per body: a 42 MB download over 802.11b takes minutes and
   must not be cut off for being slow, only for being stuck. */
#define STALL_TIMEOUT_MS     30000

/* ------------------------------------------------------------------- net */

static struct {
    int net, inet, resolver, apctl, connected;
} g_net;

void net_down(void) {
    if (g_net.connected) { sceNetApctlDisconnect(); g_net.connected = 0; }
    if (g_net.apctl)     { sceNetApctlTerm();       g_net.apctl = 0; }
    if (g_net.resolver)  { sceNetResolverTerm();    g_net.resolver = 0; }
    if (g_net.inet)      { sceNetInetTerm();        g_net.inet = 0; }
    if (g_net.net)       { sceNetTerm();            g_net.net = 0; }
    sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
    sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
}

int net_up(void) {
    if (sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON) < 0) return -1;
    if (sceUtilityLoadNetModule(PSP_NET_MODULE_INET) < 0)   return -2;

    if (sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024) < 0) goto fail;
    g_net.net = 1;
    if (sceNetInetInit() < 0) goto fail;
    g_net.inet = 1;
    if (sceNetResolverInit() < 0) goto fail;
    g_net.resolver = 1;
    if (sceNetApctlInit(0x1600, 42) < 0) goto fail;
    g_net.apctl = 1;

    /* Connection profile 1, the first one configured on the console. */
    if (sceNetApctlConnect(1) < 0) goto fail;
    g_net.connected = 1;

    unsigned start = now_ms();
    for (;;) {
        int state = 0;
        if (sceNetApctlGetState(&state) < 0) goto fail;
        if (state == 4) return 0;                    /* got an IP */
        if (expired(start, CONNECT_TIMEOUT_MS)) goto fail;
        sceKernelDelayThread(50 * 1000);
    }

fail:
    net_down();
    return -3;
}

/* The PSP resolver rather than getaddrinfo: newlib's lookup path yields
   "Trying 0.0.0.0" here, so it is not to be trusted. */
static int resolve(const char *host, struct in_addr *out) {
    static char buf[1024];
    int rid = -1;
    if (sceNetResolverCreate(&rid, buf, sizeof(buf)) < 0) return -1;
    int rc = sceNetResolverStartNtoA(rid, host, out, 2 * 1000 * 1000, 5);
    sceNetResolverDelete(rid);
    return rc < 0 ? -2 : 0;
}

/* PSPSDK declares these as returning size_t even though they report failure as
   a negative value, so the cast back to int is deliberate and load-bearing. */
static int psp_recv(int fd, void *buf, int len) {
    return (int)sceNetInetRecv(fd, buf, (size_t)len, 0);
}

static int psp_send(int fd, const void *buf, int len) {
    return (int)sceNetInetSend(fd, buf, (size_t)len, 0);
}

/* sceNetInetSelect hangs on this stack, so waiting is a short sleep. */
static void wait_socket(int ms) {
    sceKernelDelayThread((unsigned)ms * 1000);
}

/* ------------------------------------------------------------------- tls */

static int io_recv(WOLFSSL *ssl, char *buf, int sz, void *ctx) {
    (void)ssl;
    if (sz <= 0) return 0;
    int fd = *(int *)ctx;

    int n = psp_recv(fd, buf, sz);
    if (n > 0) return n;
    if (n == 0) return WOLFSSL_CBIO_ERR_CONN_CLOSE;

    int e = sceNetInetGetErrno();
    if (e == ECONNRESET) return WOLFSSL_CBIO_ERR_CONN_RST;
    if (e == ETIMEDOUT) return WOLFSSL_CBIO_ERR_TIMEOUT;
    /* EINTR is documented to map to CBIO_ERR_ISR, but returning WANT_READ hands
       control back to the caller, where a deadline governs the retry. Retrying
       inside the callback has no bound and hangs the handshake. */
    if (e == EAGAIN || e == EWOULDBLOCK || e == EINTR)
        return WOLFSSL_CBIO_ERR_WANT_READ;
    return WOLFSSL_CBIO_ERR_GENERAL;
}

static int io_send(WOLFSSL *ssl, char *buf, int sz, void *ctx) {
    (void)ssl;
    if (sz <= 0) return 0;
    int fd = *(int *)ctx;

    int n = psp_send(fd, buf, sz);
    if (n >= 0) return n;

    int e = sceNetInetGetErrno();
    if (e == EPIPE || e == ECONNRESET) return WOLFSSL_CBIO_ERR_CONN_RST;
    if (e == EAGAIN || e == EWOULDBLOCK || e == EINTR)
        return WOLFSSL_CBIO_ERR_WANT_WRITE;
    return WOLFSSL_CBIO_ERR_GENERAL;
}

/* PSP newlib has no memmem. */
static const char *mem_find(const char *hay, size_t hlen,
                            const char *needle, size_t nlen) {
    if (nlen == 0 || hlen < nlen) return NULL;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nlen) == 0)
            return hay + i;
    }
    return NULL;
}

/* Case-insensitive header lookup within the head. Returns the value start. */
static const char *header(const char *head, size_t len, const char *name) {
    size_t nlen = strlen(name);
    const char *p = head, *end = head + len;
    while (p < end) {
        const char *eol = mem_find(p, (size_t)(end - p), "\r\n", 2);
        if (!eol) eol = end;
        if ((size_t)(eol - p) > nlen && p[nlen] == ':' &&
            strncasecmp(p, name, nlen) == 0) {
            const char *v = p + nlen + 1;
            while (v < eol && *v == ' ') v++;
            return v;
        }
        p = eol + 2;
    }
    return NULL;
}

/* ------------------------------------------------------------------- url */

struct url { char host[128]; char path[512]; };

static int url_parse(const char *s, struct url *u) {
    if (strncmp(s, "https://", 8) != 0) { logline("url: not https: %.40s", s); return -1; }
    s += 8;
    const char *slash = strchr(s, '/');
    size_t hl = slash ? (size_t)(slash - s) : strlen(s);
    if (hl == 0 || hl >= sizeof(u->host)) { logline("url: bad host"); return -1; }
    memcpy(u->host, s, hl);
    u->host[hl] = '\0';
    if (!slash) { strcpy(u->path, "/"); return 0; }
    if (strlen(slash) >= sizeof(u->path)) { logline("url: path too long"); return -1; }
    strcpy(u->path, slash);
    return 0;
}

/* Location may be absolute or a path on the same host. */
static int url_resolve(const struct url *base, const char *loc, size_t loclen,
                       struct url *out) {
    char tmp[640];
    if (loclen >= sizeof(tmp)) return -1;
    memcpy(tmp, loc, loclen);
    tmp[loclen] = '\0';
    if (tmp[0] == '/') {
        *out = *base;
        if (strlen(tmp) >= sizeof(out->path)) return -1;
        strcpy(out->path, tmp);
        return 0;
    }
    return url_parse(tmp, out);
}

/* ---------------------------------------------------------------- request */

/* One connection, one request. Fills head[] with the response head, streams
   the body. Returns: 0 complete, 1 truncated, <0 failed before the body.
   On a 3xx with Location, *redirect is filled and 2 is returned. */
static int one_request(const struct url *u, https_sink sink, void *sink_ctx,
                       https_progress progress, void *progress_ctx,
                       struct https_result *res, struct url *redirect) {
    int sock = -1, rc, ret = -1, wolf_up = 0;
    WOLFSSL_CTX *ctx = NULL;
    WOLFSSL *ssl = NULL;
    static char buf[16 * 1024];
    static char head[HEAD_MAX];
    size_t headlen = 0;

    struct in_addr ip;
    if (resolve(u->host, &ip) < 0) { logline("dns failed: %s", u->host); return -1; }

    sock = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { logline("socket failed"); return -2; }

    /* No portable O_NONBLOCK here, and SO_NONBLOCK / SO_ERROR are not in the
       headers -- using them picks up constants from elsewhere and configures
       the wrong option. The stack behaves as non-blocking (recv reports
       EAGAIN), which is what the IO callbacks are written for. */
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PORT);
    sa.sin_addr = ip;

    unsigned start = now_ms();
    if (sceNetInetConnect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        int e = sceNetInetGetErrno();
        if (e != EINPROGRESS && e != EALREADY && e != EWOULDBLOCK) {
            logline("connect failed errno=%d", e);
            goto out;
        }
        while (!expired(start, CONNECT_TIMEOUT_MS))
            sceKernelDelayThread(50 * 1000);
    }

    int irc = wolfSSL_Init();
    if (irc != WOLFSSL_SUCCESS) {
        logline("wolfssl %s init=%d pool=%d bits", wolfSSL_lib_version(), irc, g_pool_bits);
        goto out;
    }
    wolf_up = 1;

    ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
    if (!ctx) { logline("no TLS 1.3 in this build"); goto out; }

    /* No CA bundle on the stick yet. The handshake is real, the chain is not
       checked; pinning our own issuer is the next step. */
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_NONE, NULL);
    wolfSSL_CTX_SetIORecv(ctx, io_recv);
    wolfSSL_CTX_SetIOSend(ctx, io_send);

    /* X25519 costs a fraction of P-256 on a core with no crypto hardware, and
       offering its key share up front avoids a HelloRetryRequest, which would
       be an entire extra round trip. */
    static int groups[] = { WOLFSSL_ECC_X25519, WOLFSSL_ECC_SECP256R1 };
    if (wolfSSL_CTX_set_groups(ctx, groups, 2) != WOLFSSL_SUCCESS)
        logline("x25519 unavailable, using default groups");

    ssl = wolfSSL_new(ctx);
    if (!ssl) { logline("wolfSSL_new failed"); goto out; }
    wolfSSL_SetIOReadCtx(ssl, &sock);
    wolfSSL_SetIOWriteCtx(ssl, &sock);
    if (wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, u->host,
                       (unsigned short)strlen(u->host)) != WOLFSSL_SUCCESS)
        logline("SNI rejected");
    if (wolfSSL_UseKeyShare(ssl, WOLFSSL_ECC_X25519) != WOLFSSL_SUCCESS)
        logline("x25519 key share unavailable");

    start = now_ms();
    while ((rc = wolfSSL_connect(ssl)) != WOLFSSL_SUCCESS) {
        int e = wolfSSL_get_error(ssl, rc);
        if (e != WOLFSSL_ERROR_WANT_READ && e != WOLFSSL_ERROR_WANT_WRITE) {
            char msg[80];
            wolfSSL_ERR_error_string((unsigned long)e, msg);
            logline("handshake failed %d: %s", e, msg);
            goto out;
        }
        if (expired(start, HANDSHAKE_TIMEOUT_MS)) { logline("handshake timeout"); goto out; }
        wait_socket(1);
    }
    res->handshake_ms = now_ms() - start;
    {
        const char *group = wolfSSL_get_curve_name(ssl);
        logline("%s %s %s %u ms", u->host, wolfSSL_get_cipher(ssl),
                group ? group : "?", res->handshake_ms);
    }

    int reqlen = snprintf(buf, sizeof(buf),
                          "GET %s HTTP/1.1\r\n"
                          "Host: %s\r\n"
                          "User-Agent: pspdx/0.0\r\n"
                          "Connection: close\r\n\r\n", u->path, u->host);
    if (reqlen <= 0 || reqlen >= (int)sizeof(buf)) { logline("request too long"); goto out; }

    start = now_ms();
    for (int sent = 0; sent < reqlen; ) {
        rc = wolfSSL_write(ssl, buf + sent, reqlen - sent);
        if (rc > 0) { sent += rc; continue; }
        int e = wolfSSL_get_error(ssl, rc);
        if (e != WOLFSSL_ERROR_WANT_READ && e != WOLFSSL_ERROR_WANT_WRITE) {
            logline("write failed %d", e);
            goto out;
        }
        if (expired(start, STALL_TIMEOUT_MS)) { logline("write timeout"); goto out; }
        wait_socket(1);
    }

    /* Read until the head is complete, then hand the rest to the sink. */
    const char *body_start = NULL;
    size_t want = 0;
    int have_length = 0, chunked = 0;
    res->body_len = 0;
    res->content_length = 0;
    res->truncated = 0;

    start = now_ms();
    for (;;) {
        rc = wolfSSL_read(ssl, buf, (int)sizeof(buf));
        if (rc > 0) {
            start = now_ms();
            const char *data = buf;
            size_t len = (size_t)rc;

            if (!body_start) {
                if (headlen + len > sizeof(head)) { logline("http: head too large"); goto out; }
                memcpy(head + headlen, data, len);
                headlen += len;
                const char *sep = mem_find(head, headlen, "\r\n\r\n", 4);
                if (!sep) continue;

                size_t hl = (size_t)(sep - head) + 4;
                if (sscanf(head, "HTTP/%*d.%*d %ld", &res->status) != 1) {
                    logline("http: bad status line");
                    goto out;
                }
                const char *cl = header(head, hl, "Content-Length");
                if (cl) { want = (size_t)strtoul(cl, NULL, 10); have_length = 1; }
                res->content_length = want;
                const char *te = header(head, hl, "Transfer-Encoding");
                if (te && strncasecmp(te, "chunked", 7) == 0) chunked = 1;

                if (res->status >= 300 && res->status < 400) {
                    const char *loc = header(head, hl, "Location");
                    if (loc) {
                        const char *eol = mem_find(loc, (size_t)(head + hl - loc), "\r\n", 2);
                        if (eol && url_resolve(u, loc, (size_t)(eol - loc), redirect) == 0) {
                            logline("http %ld -> %s", res->status, redirect->host);
                            ret = 2;
                            goto out;
                        }
                    }
                }
                if (chunked) {
                    /* GitHub serves everything we ask for with a length;
                       a chunk decoder is not worth its bytes until it is not. */
                    logline("http: chunked not supported");
                    goto out;
                }
                logline("http %ld, %lu bytes announced", res->status, (unsigned long)want);
                if (progress) progress(progress_ctx, 0, want);

                /* Whatever followed the head in this read is body. */
                body_start = head + hl;
                data = body_start;
                len = headlen - hl;
                ret = 1;                                 /* body has begun */
                if (len == 0) {
                    if (have_length && want == 0) { ret = 0; goto out; }
                    continue;
                }
            }

            if (sink && sink(sink_ctx, data, len) != 0) { logline("sink aborted"); goto out; }
            res->body_len += len;
            if (progress) progress(progress_ctx, res->body_len, want);
            if (have_length && res->body_len >= want) { ret = 0; goto out; }
            continue;
        }

        int e = wolfSSL_get_error(ssl, rc);
        if (e == WOLFSSL_ERROR_NONE || e == WOLFSSL_ERROR_ZERO_RETURN) {
            /* Clean close: complete unless a length says otherwise. */
            if (body_start && (!have_length || res->body_len >= want)) ret = 0;
            else if (!body_start) logline("http: closed before head");
            goto out;
        }
        if (e != WOLFSSL_ERROR_WANT_READ && e != WOLFSSL_ERROR_WANT_WRITE) {
            /* A reset after the body has arrived is common enough to tolerate,
               but it must not be reported as a clean read. */
            logline("read error %d after %lu bytes", e, (unsigned long)res->body_len);
            if (body_start && have_length && res->body_len >= want) ret = 0;
            goto out;
        }
        if (expired(start, STALL_TIMEOUT_MS)) { logline("read stalled"); goto out; }
        wait_socket(1);
    }

out:
    if (ret == 1) res->truncated = 1;
    if (ssl) {
        if (ret == 0 || ret == 2) wolfSSL_shutdown(ssl);
        wolfSSL_free(ssl);
    }
    if (ctx) wolfSSL_CTX_free(ctx);
    if (wolf_up) wolfSSL_Cleanup();
    if (sock >= 0) sceNetInetClose(sock);
    return ret;
}

int https_get(const char *url, https_sink sink, void *sink_ctx,
              https_progress progress, void *progress_ctx,
              struct https_result *out) {
    struct url u, next;
    struct https_result res;
    memset(&res, 0, sizeof(res));
    if (url_parse(url, &u) < 0) return -1;

    for (res.redirects = 0; ; res.redirects++) {
        int rc = one_request(&u, sink, sink_ctx, progress, progress_ctx, &res, &next);
        if (rc != 2) {
            strncpy(res.host, u.host, sizeof(res.host) - 1);
            if (out) *out = res;
            return rc;
        }
        if (res.redirects >= MAX_REDIRECTS) { logline("too many redirects"); return -3; }
        u = next;
    }
}
