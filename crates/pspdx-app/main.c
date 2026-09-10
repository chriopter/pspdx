/*
 * PSPDX -- fetches a page over TLS and shows it on the PSP screen.
 *
 * Everything here is scaffolding for the real client; what it proves is that
 * the console can complete a modern TLS handshake and read an HTTPS response.
 */

#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <psprtc.h>
#include <psputility.h>
#include <psputility_netmodules.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pspctrl.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>

PSP_MODULE_INFO("pspdx", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
/* Only a few hundred KB are actually needed: the response buffer plus wolfSSL's
   record buffers. Leaving the rest to the system keeps a PSP-1000 comfortable. */
PSP_HEAP_SIZE_KB(4 * 1024);

#define HOST "chriopter.github.io"
#define PATH "/pspdx/"
#define PORT 443

#define COLS 60
#define ANIM_ROW 32

#define CONNECT_TIMEOUT_MS   10000
#define HANDSHAKE_TIMEOUT_MS 20000
#define TRANSFER_TIMEOUT_MS  30000

/* --------------------------------------------------------------- logging */

#define LOGLINES 26
#define LOGCOLS (COLS + 1)

static char g_log[LOGLINES][LOGCOLS];
static int g_logn = 0;

static char g_resp[24 * 1024];
static size_t g_resplen = 0;

/* Set once the response has been parsed. */
static long g_status = 0;
static const char *g_body = NULL;
static size_t g_bodylen = 0;
static int g_truncated = 0;

static void logline(const char *fmt, ...) {
    char line[LOGCOLS];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (g_logn < LOGLINES) {
        strcpy(g_log[g_logn++], line);
    }
    sceIoWrite(1, line, strlen(line));
    sceIoWrite(1, "\n", 1);
}

/* Written once and closed. PPSSPP only flushes an emulated file to the host on
   close, so incremental logging to the stick never shows up. */
static void dump_to_stick(void) {
    int fd = sceIoOpen("ms0:/PSPDX.LOG", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        for (int i = 0; i < g_logn; i++) {
            sceIoWrite(fd, g_log[i], strlen(g_log[i]));
            sceIoWrite(fd, "\n", 1);
        }
        sceIoClose(fd);
    }
    if (g_resplen) {
        fd = sceIoOpen("ms0:/PSPDX.HTTP", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, g_resp, g_resplen);
            sceIoClose(fd);
        }
    }
}

/* ------------------------------------------------------------------ time */

static unsigned now_ms(void) {
    u64 tick = 0;
    sceRtcGetCurrentTick(&tick);
    return (unsigned)(tick / 1000);
}

static int expired(unsigned start, unsigned budget_ms) {
    return (now_ms() - start) > budget_ms;
}

/* ------------------------------------------------------------------ boot */

static int exit_callback(int a, int b, void *c) {
    (void)a; (void)b; (void)c;
    sceKernelExitGame();
    return 0;
}

static int callback_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int cbid = sceKernelCreateCallback("Exit", exit_callback, NULL);
    if (cbid >= 0) {
        sceKernelRegisterExitCallback(cbid);
    }
    sceKernelSleepThreadCB();
    return 0;
}

/* Without this thread HOME does nothing and the only way out is the power
   switch, so a failure here is worth reporting rather than swallowing. */
static int setup_callbacks(void) {
    int thid = sceKernelCreateThread("update_thread", callback_thread,
                                     0x11, 0xFA0, THREAD_ATTR_USER, 0);
    if (thid < 0) return thid;
    return sceKernelStartThread(thid, 0, 0);
}

/* --------------------------------------------------------------- network */

/* Each flag records one initialisation step, so a failure part-way through can
   unwind exactly what came up. */
static struct {
    int net, inet, resolver, apctl, connected;
} g_net;

static void net_down(void) {
    if (g_net.connected) { sceNetApctlDisconnect(); g_net.connected = 0; }
    if (g_net.apctl)     { sceNetApctlTerm();       g_net.apctl = 0; }
    if (g_net.resolver)  { sceNetResolverTerm();    g_net.resolver = 0; }
    if (g_net.inet)      { sceNetInetTerm();        g_net.inet = 0; }
    if (g_net.net)       { sceNetTerm();            g_net.net = 0; }
    sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
    sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
}

static int net_up(void) {
    int rc;

    if ((rc = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON)) < 0) return -1;
    if ((rc = sceUtilityLoadNetModule(PSP_NET_MODULE_INET)) < 0)   return -2;

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

/* sceNetInetSelect hangs on this stack, so waiting is a short sleep. The
   interval is the polling granularity of every retry loop below. */
static void wait_socket(int fd, int for_write, int ms) {
    (void)fd; (void)for_write;
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

/* ------------------------------------------------------------------ http */

/* Just enough HTTP to know whether the response is complete. Chunked transfer
   is rejected rather than mis-parsed; this server does not use it. */
static int parse_response(void) {
    static const char sep[] = "\r\n\r\n";
    const char *head_end = mem_find(g_resp, g_resplen, sep, 4);
    if (!head_end) {
        logline("http: no header terminator");
        return -1;
    }

    if (sscanf(g_resp, "HTTP/%*d.%*d %ld", &g_status) != 1) {
        logline("http: bad status line");
        return -2;
    }

    size_t headlen = (size_t)(head_end - g_resp) + 4;
    g_body = g_resp + headlen;
    g_bodylen = g_resplen - headlen;

    /* Header names are case-insensitive, but this server is predictable and a
       full parser is not what this program is for. */
    if (mem_find(g_resp, headlen, "Transfer-Encoding: chunked", 26)) {
        logline("http: chunked not supported");
        return -3;
    }

    const char *cl = mem_find(g_resp, headlen, "Content-Length:", 15);
    if (cl) {
        long want = strtol(cl + 15, NULL, 10);
        if (want >= 0 && (size_t)want != g_bodylen) {
            logline("http: %lu of %ld body bytes", (unsigned long)g_bodylen, want);
            g_truncated = 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------- psprandom */

/* Kept deliberately free of wolfSSL types so it can be lifted into its own
   library later with a `git mv`. The console has no usable randomness of its
   own: pspsdk's getentropy() is MT19937 reseeded from time(NULL) on every
   call, and the hardware PRNG in the crypto chip is a kernel-only export that
   a user-mode EBOOT cannot import at all.
   Human input is the one source that comes from outside the machine's
   determinism -- the timing of a moving thumb is not reproducible, and it
   works the same on hardware and under an emulator driven by a real gamepad.
   The same ritual PGP and TrueCrypt used with mouse movement. */

#define POOL_BYTES 20      /* one SHA-1 digest */
#define ENTROPY_BITS 256

static unsigned char g_pool[POOL_BYTES];
static unsigned int g_pool_counter = 0;
static int g_pool_bits = 0;

/* The console's own SHA-1, not wolfSSL's SHA-256: the seed callback is invoked
   from inside wolfCrypt_Init, before wolfSSL's digests are usable -- doing it
   the other way round makes wolfSSL_Init fail with WC_INIT_E. sceKernelUtils*
   is a plain user-mode export and keeps this unit free of any dependency.
   Collision resistance is not the property being asked of it; mixing is. */
static void pool_absorb(const void *data, unsigned int len) {
    unsigned char buf[POOL_BYTES + 64];
    unsigned int n = len > 64 ? 64 : len;
    memcpy(buf, g_pool, POOL_BYTES);
    memcpy(buf + POOL_BYTES, data, n);
    sceKernelUtilsSha1Digest(buf, POOL_BYTES + n, g_pool);
}

/* Counts loop iterations between two clock ticks. On real silicon this varies
   with interrupts and refresh cycles; under an emulator it is nearly constant,
   which is why it is a supplement and never the whole seed. */
static void pool_absorb_jitter(int rounds) {
    for (int i = 0; i < rounds; i++) {
        unsigned t0 = sceKernelGetSystemTimeLow();
        unsigned spins = 0;
        while (sceKernelGetSystemTimeLow() - t0 < 500) spins++;
        pool_absorb(&spins, sizeof(spins));
    }
}

static void psprandom_init(void) {
    unsigned t = sceKernelGetSystemTimeLow();
    void *sp = &t;
    pool_absorb(&t, sizeof(t));
    pool_absorb(&sp, sizeof(sp));
    pool_absorb_jitter(8);
    g_pool_bits = 0;                    /* none of the above is credited */
}

/* ------------------------------------------------------------- the sweep */

/* Sweeping a field is not decoration. Idle waggling parks the stick in the
   centre or against a stop, where the ADC saturates and stops telling you
   anything; covering a grid forces the whole range to be visited. It also
   makes the accounting honest -- a worn, drifting stick produces changes that
   a naive counter would credit, but it cannot complete a field. */

#define GRID_W 60
#define GRID_H 28
#define GRID_X 0
#define GRID_Y 2
#define SETTLE 14                 /* frames a touched cell spends spinning */

#define COL_IDLE   0xFF404048     /* ABGR: cold grey */
#define COL_SPIN   0xFFF0F0F0     /* white, mid-swirl */
#define COL_WARM   0xFFC0FFC0     /* settling */
#define COL_DONE   0xFF20C020     /* green, counted */
#define COL_CURSOR 0xFFFFFF40     /* cyan */
#define COL_TEXT   0xFFFFFFFF
#define COL_DIM    0xFF909090

/* Every pad sample the sweep sees, so a session can be replayed byte for byte.
   Replaying is for development only: a fixed trace yields a fixed pool, which
   is the opposite of entropy -- so a replayed run refuses to save its seed and
   says so on screen. */
#define TRACE_MAX 5000
#define TRACE_FILE  "ms0:/PSPDX.TRACE"
#define REPLAY_FILE "ms0:/PSPDX.REPLAY"

struct trace_sample { unsigned char lx, ly; unsigned short buttons; };
static struct trace_sample g_trace[TRACE_MAX];
static int g_trace_len = 0;
static int g_trace_pos = 0;
static int g_replay = 0;

static void trace_load(void) {
    int fd = sceIoOpen(REPLAY_FILE, PSP_O_RDONLY, 0777);
    if (fd < 0) return;
    sceIoClose(fd);

    fd = sceIoOpen(TRACE_FILE, PSP_O_RDONLY, 0777);
    if (fd < 0) return;
    int n = sceIoRead(fd, g_trace, sizeof(g_trace));
    sceIoClose(fd);
    if (n <= 0) return;
    g_trace_len = n / (int)sizeof(struct trace_sample);
    g_trace_pos = 0;
    g_replay = 1;
}

static void trace_save(void) {
    if (g_replay || g_trace_len == 0) return;
    int fd = sceIoOpen(TRACE_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, g_trace, (SceSize)(g_trace_len * (int)sizeof(struct trace_sample)));
    sceIoClose(fd);
}

/* One source of pad data for the sweep, live or recorded. */
static int next_sample(SceCtrlData *pad) {
    if (g_replay) {
        if (g_trace_pos >= g_trace_len) return 0;
        struct trace_sample *t = &g_trace[g_trace_pos++];
        memset(pad, 0, sizeof(*pad));
        pad->Lx = t->lx;
        pad->Ly = t->ly;
        pad->Buttons = t->buttons;
        pad->TimeStamp = (unsigned)g_trace_pos;
        return 1;
    }
    sceCtrlPeekBufferPositive(pad, 1);
    if (g_trace_len < TRACE_MAX) {
        g_trace[g_trace_len].lx = pad->Lx;
        g_trace[g_trace_len].ly = pad->Ly;
        g_trace[g_trace_len].buttons = (unsigned short)pad->Buttons;
        g_trace_len++;
    }
    return 1;
}

static unsigned char cell_phase[GRID_H][GRID_W];
static unsigned char cell_done[GRID_H][GRID_W];
static char shadow_ch[GRID_H][GRID_W];
static unsigned shadow_col[GRID_H][GRID_W];

static const char IDLE_GLYPHS[] = ".,'`:;\"~";
static const char SPIN_GLYPHS[] = "|/-\\";
static const char BLOOM[] = "oO0@";

static void grid_reset(void) {
    memset(cell_phase, 0, sizeof(cell_phase));
    memset(cell_done, 0, sizeof(cell_done));
    memset(shadow_ch, 0, sizeof(shadow_ch));
    memset(shadow_col, 0, sizeof(shadow_col));
}

static char idle_glyph(int x, int y) {
    /* Stable per cell, so the field does not shimmer when nothing happens. */
    unsigned h = (unsigned)(x * 73856093) ^ (unsigned)(y * 19349663);
    return IDLE_GLYPHS[(h >> 5) % (sizeof(IDLE_GLYPHS) - 1)];
}

static void draw_cell(int x, int y, char ch, unsigned col) {
    if (shadow_ch[y][x] == ch && shadow_col[y][x] == col) return;
    shadow_ch[y][x] = ch;
    shadow_col[y][x] = col;
    pspDebugScreenSetTextColor(col);
    pspDebugScreenSetXY(GRID_X + x, GRID_Y + y);
    pspDebugScreenPrintf("%c", ch);
}

/* Runs the sweep and leaves the pool filled. Returns credited bits. */
static int psprandom_sweep(void) {
    grid_reset();
    pspDebugScreenClear();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    float cx = GRID_W / 2.0f, cy = GRID_H / 2.0f;
    int covered = 0, frame = 0;
    const int total = GRID_W * GRID_H;

    pspDebugScreenSetTextColor(g_replay ? 0xFF4040FF : COL_TEXT);
    pspDebugScreenSetXY(0, 0);
    if (g_replay)
        pspDebugScreenPrintf(" REPLAY -- recorded input, the entropy here is NOT real");
    else
        pspDebugScreenPrintf(" COLLECT ENTROPY -- sweep the field with the analog stick");

    for (;;) {
        SceCtrlData pad;
        if (!next_sample(&pad)) break;         /* replay ran out */

        int dx = (int)pad.Lx - 128;
        int dy = (int)pad.Ly - 128;
        int moving = (dx * dx + dy * dy) > (14 * 14);   /* deadzone */

        if (moving) {
            cx += dx * 0.0065f;
            cy += dy * 0.0040f;
            if (cx < 0) cx = 0;
            if (cy < 0) cy = 0;
            if (cx > GRID_W - 1) cx = GRID_W - 1;
            if (cy > GRID_H - 1) cy = GRID_H - 1;

            struct { unsigned char lx, ly; unsigned int sys; float px, py; } sample;
            sample.lx = pad.Lx;
            sample.ly = pad.Ly;
            sample.sys = sceKernelGetSystemTimeLow();
            sample.px = cx;
            sample.py = cy;
            pool_absorb(&sample, sizeof(sample));

            /* Two bits, not four: the sampling clock is the vblank, so the
               arrival time carries almost nothing. What is unpredictable is
               the low bits of the position, and there is not much of it. */
            g_pool_bits += 2;
        }

        int ccx = (int)(cx + 0.5f), ccy = (int)(cy + 0.5f);
        for (int y = 0; y < GRID_H; y++) {
            for (int x = 0; x < GRID_W; x++) {
                int ddx = x - ccx, ddy = y - ccy;
                if (moving && ddx * ddx + ddy * ddy * 3 <= 9) {
                    if (!cell_done[y][x] && cell_phase[y][x] == 0) covered++;
                    cell_phase[y][x] = SETTLE;
                    cell_done[y][x] = 1;
                }
            }
        }

        for (int y = 0; y < GRID_H; y++) {
            for (int x = 0; x < GRID_W; x++) {
                char ch;
                unsigned col;
                if (cell_phase[y][x] > 0) {
                    int ph = cell_phase[y][x]--;
                    if (ph > SETTLE - 5) {
                        ch = BLOOM[(SETTLE - ph) % 4];       /* bloom outward */
                        col = COL_SPIN;
                    } else {
                        ch = SPIN_GLYPHS[(ph + x + y) % 4];  /* then swirl */
                        col = (ph > 4) ? COL_SPIN : COL_WARM;
                    }
                } else if (cell_done[y][x]) {
                    ch = '#';
                    col = COL_DONE;
                } else {
                    ch = idle_glyph(x, y);
                    col = COL_IDLE;
                }
                draw_cell(x, y, ch, col);
            }
        }

        /* The cursor last, so it sits on top of its own wake. */
        draw_cell(ccx, ccy, SPIN_GLYPHS[(frame / 2) % 4], COL_CURSOR);
        shadow_ch[ccy][ccx] = 0;                 /* force a redraw next frame */

        /* 95 percent of the field counts as done. A forgotten corner should
           not hold anyone hostage, and the last few cells add nothing the
           other 1500 have not already contributed. */
        int pct = (covered * 10000) / (total * 95);
        if (pct > 100) pct = 100;
        int ready = (pct >= 100 && g_pool_bits >= ENTROPY_BITS);
        pspDebugScreenSetTextColor(ready ? COL_DONE : COL_TEXT);
        pspDebugScreenSetXY(0, 33);
        pspDebugScreenPrintf(" [");
        int filled = (pct * 44) / 100;
        for (int i = 0; i < 44; i++) pspDebugScreenPrintf("%c", i < filled ? '=' : ' ');
        if (ready) pspDebugScreenPrintf("] %3d%%  X to continue", pct);
        else       pspDebugScreenPrintf("] %3d%%  %4d bits ", pct, g_pool_bits);

        if (ready && (pad.Buttons & PSP_CTRL_CROSS)) break;

        frame++;
        sceDisplayWaitVblankStart();
    }

    trace_save();
    pspDebugScreenSetTextColor(COL_TEXT);
    return g_pool_bits;
}

/* --------------------------------------------------------- seed on disk */

#define SEED_FILE "ms0:/PSPDX.SEED"

/* A seed file alone is not enough: a copied memory stick would hand two
   consoles the same stream. Fresh jitter and the console id go in before a
   single byte comes out, and the file is rewritten immediately. */
static int psprandom_load(void) {
    int fd = sceIoOpen(SEED_FILE, PSP_O_RDONLY, 0777);
    if (fd < 0) return 0;
    unsigned char stored[POOL_BYTES];
    int n = sceIoRead(fd, stored, sizeof(stored));
    sceIoClose(fd);
    if (n != (int)sizeof(stored)) return 0;

    pool_absorb(stored, sizeof(stored));
    pool_absorb_jitter(4);
    return 1;
}

static void psprandom_save(void) {
    if (g_replay) return;
    unsigned char next[POOL_BYTES];
    unsigned int tag = 0x50535058;                /* domain separation */
    unsigned char buf[POOL_BYTES + sizeof(tag)];
    memcpy(buf, g_pool, POOL_BYTES);
    memcpy(buf + POOL_BYTES, &tag, sizeof(tag));
    sceKernelUtilsSha1Digest(buf, sizeof(buf), next);

    int fd = sceIoOpen(SEED_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, next, sizeof(next));
        sceIoClose(fd);
    }
}

static void psprandom_forget(void) {
    sceIoRemove(SEED_FILE);
    g_pool_bits = 0;
    memset(g_pool, 0, sizeof(g_pool));
}

/* wolfSSL is built with -DCUSTOM_RAND_GENERATE_SEED=psprandom_seed_raw, so its
   wc_GenerateSeed() is a wrapper around this and the /dev/urandom branch never
   exists. Not static: the library resolves the symbol at link time. */
int psprandom_seed_raw(unsigned char *seed, unsigned int sz);
int psprandom_seed_raw(unsigned char *seed, unsigned int sz) {
    while (sz > 0) {
        unsigned char buf[POOL_BYTES + sizeof(unsigned int)];
        unsigned char out[POOL_BYTES];
        memcpy(buf, g_pool, POOL_BYTES);
        memcpy(buf + POOL_BYTES, &g_pool_counter, sizeof(g_pool_counter));
        sceKernelUtilsSha1Digest(buf, sizeof(buf), out);
        g_pool_counter++;

        word32 n = sz < sizeof(out) ? sz : (word32)sizeof(out);
        memcpy(seed, out, n);
        seed += n;
        sz -= n;

        /* Ratchet, so a later leak of the pool does not expose earlier seeds. */
        pool_absorb(out, sizeof(out));
    }
    return 0;
}

/* ----------------------------------------------------------------- fetch */

static int fetch(void) {
    int sock = -1, rc = -1, ret = -1;
    WOLFSSL_CTX *ctx = NULL;
    WOLFSSL *ssl = NULL;
    int wolf_up = 0;
    unsigned t_connect = 0, t_handshake = 0;

    g_resplen = 0;

    struct in_addr ip;
    if (resolve(HOST, &ip) < 0) { logline("dns failed"); return -1; }
    {
        unsigned char *o = (unsigned char *)&ip.s_addr;
        logline("dns %u.%u.%u.%u", o[0], o[1], o[2], o[3]);
    }

    sock = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { logline("socket failed"); return -2; }

    /* PSPSDK exposes no portable way to set O_NONBLOCK, and SO_NONBLOCK /
       SO_ERROR are not defined by its headers at all -- setting them picks up
       constants from elsewhere and quietly configures the wrong option. The
       stack behaves as non-blocking here (recv reports EAGAIN), which is what
       the IO callbacks are written for; the deadlines below bound the rest. */

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PORT);
    sa.sin_addr = ip;

    /* This stack completes the connect synchronously; the in-progress case is
       handled by waiting in fixed steps rather than by selecting for
       writability, which does not behave reliably here. */
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
    t_connect = now_ms() - start;
    logline("tcp connected in %u ms", t_connect);

    int irc = wolfSSL_Init();
    logline("wolfssl %s init=%d pool=%d bits",
            wolfSSL_lib_version(), irc, g_pool_bits);
    if (irc != WOLFSSL_SUCCESS) goto out;
    wolf_up = 1;

    ctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
    if (!ctx) { logline("no TLS 1.3 in this build"); goto out; }

    /* No CA bundle on the stick yet. The handshake is real, the chain is not
       checked; pinning our own issuer is the next step. */
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_NONE, NULL);
    wolfSSL_CTX_SetIORecv(ctx, io_recv);
    wolfSSL_CTX_SetIOSend(ctx, io_send);

    /* X25519 costs far less than P-256 on a 32-bit core with no crypto
       hardware, and offering its key share up front avoids a
       HelloRetryRequest, which would be an entire extra round trip. Neither
       call is fatal: a library built without curve25519 still hands back a
       working handshake on the default group. */
    static int groups[] = { WOLFSSL_ECC_X25519, WOLFSSL_ECC_SECP256R1 };
    if (wolfSSL_CTX_set_groups(ctx, groups, 2) != WOLFSSL_SUCCESS)
        logline("x25519 unavailable, using default groups");

    ssl = wolfSSL_new(ctx);
    if (!ssl) { logline("wolfSSL_new failed"); goto out; }

    wolfSSL_SetIOReadCtx(ssl, &sock);
    wolfSSL_SetIOWriteCtx(ssl, &sock);
    if (wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, HOST,
                       (unsigned short)strlen(HOST)) != WOLFSSL_SUCCESS)
        logline("SNI rejected");
    if (wolfSSL_UseKeyShare(ssl, WOLFSSL_ECC_X25519) != WOLFSSL_SUCCESS)
        logline("x25519 key share unavailable");

    start = now_ms();
    while ((rc = wolfSSL_connect(ssl)) != WOLFSSL_SUCCESS) {
        int e = wolfSSL_get_error(ssl, rc);
        if (e != WOLFSSL_ERROR_WANT_READ && e != WOLFSSL_ERROR_WANT_WRITE) {
            char msg[80];
            wolfSSL_ERR_error_string((unsigned long)e, msg);
            logline("handshake failed %d", e);
            logline("%s", msg);
            goto out;
        }
        if (expired(start, HANDSHAKE_TIMEOUT_MS)) { logline("handshake timeout"); goto out; }
        wait_socket(sock, e == WOLFSSL_ERROR_WANT_WRITE, 1);
    }
    t_handshake = now_ms() - start;

    {
        const char *group = wolfSSL_get_curve_name(ssl);
        logline("%s  %s", wolfSSL_get_version(ssl), wolfSSL_get_cipher(ssl));
        logline("%s, handshake %u ms", group ? group : "?", t_handshake);
    }

    char req[256];
    int reqlen = snprintf(req, sizeof(req),
                          "GET " PATH " HTTP/1.1\r\n"
                          "Host: " HOST "\r\n"
                          "User-Agent: pspdx/0.0\r\n"
                          "Connection: close\r\n\r\n");
    if (reqlen <= 0 || reqlen >= (int)sizeof(req)) { logline("request too long"); goto out; }

    start = now_ms();
    for (int sent = 0; sent < reqlen; ) {
        rc = wolfSSL_write(ssl, req + sent, reqlen - sent);
        if (rc > 0) { sent += rc; continue; }
        int e = wolfSSL_get_error(ssl, rc);
        if (e != WOLFSSL_ERROR_WANT_READ && e != WOLFSSL_ERROR_WANT_WRITE) {
            logline("write failed %d", e);
            goto out;
        }
        if (expired(start, TRANSFER_TIMEOUT_MS)) { logline("write timeout"); goto out; }
        wait_socket(sock, e == WOLFSSL_ERROR_WANT_WRITE, 1);
    }

    start = now_ms();
    for (;;) {
        if (g_resplen >= sizeof(g_resp) - 1) { g_truncated = 1; break; }

        rc = wolfSSL_read(ssl, g_resp + g_resplen,
                          (int)(sizeof(g_resp) - 1 - g_resplen));
        if (rc > 0) { g_resplen += (size_t)rc; continue; }

        int e = wolfSSL_get_error(ssl, rc);
        if (e == WOLFSSL_ERROR_NONE || e == WOLFSSL_ERROR_ZERO_RETURN)
            break;                                   /* clean close_notify */
        if (e != WOLFSSL_ERROR_WANT_READ && e != WOLFSSL_ERROR_WANT_WRITE) {
            /* A reset after the body has arrived is common enough to tolerate,
               but it must not be reported as a clean read. */
            logline("read error %d after %lu bytes", e, (unsigned long)g_resplen);
            g_truncated = 1;
            break;
        }
        if (expired(start, TRANSFER_TIMEOUT_MS)) { logline("read timeout"); g_truncated = 1; break; }
        wait_socket(sock, 0, 1);
    }
    g_resp[g_resplen] = '\0';
    logline("read %lu bytes in %u ms", (unsigned long)g_resplen, now_ms() - start);

    if (parse_response() == 0) {
        logline("HTTP %ld, body %lu bytes%s", g_status, (unsigned long)g_bodylen,
                g_truncated ? " (truncated)" : "");
        ret = g_truncated ? 1 : 0;
    }

out:
    if (ssl) {
        if (ret >= 0) wolfSSL_shutdown(ssl);
        wolfSSL_free(ssl);
    }
    if (ctx) wolfSSL_CTX_free(ctx);
    if (wolf_up) wolfSSL_Cleanup();
    if (sock >= 0) sceNetInetClose(sock);
    return ret;
}

/* ---------------------------------------------------------------- screen */

static int draw_wrapped(const char *text, size_t len, int row, int maxrows) {
    size_t i = 0;
    while (row < maxrows && i < len) {
        char line[COLS + 1];
        int c = 0;
        while (c < COLS && i < len && text[i] != '\n') {
            char ch = text[i++];
            if (ch == '\r') continue;
            line[c++] = (ch >= 32 && ch < 127) ? ch : '.';
        }
        line[c] = '\0';
        if (i < len && text[i] == '\n') i++;         /* consume the newline */
        pspDebugScreenSetXY(0, row++);
        pspDebugScreenPrintf("%s", line);
    }
    return row;
}

static void animate_forever(void) {
    static const char spin[] = "|/-\\";
    int frame = 0;
    int hinted = 0;

    for (;;) {
        /* Once the connection stands, the entropy can be thrown away and
           gathered again -- useful after moving the stick to another console,
           or simply to see the field once more. */
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_SELECT) {
            psprandom_forget();
            pspDebugScreenClear();
            pspDebugScreenSetXY(0, 0);
            pspDebugScreenPrintf("PSPDX  entropy discarded\n");
            psprandom_sweep();
            psprandom_save();
            pspDebugScreenClear();
            pspDebugScreenSetTextColor(COL_TEXT);
            pspDebugScreenSetXY(0, 0);
            pspDebugScreenPrintf("PSPDX  entropy regenerated. %d bits.\n", g_pool_bits);
            memset(shadow_ch, 0, sizeof(shadow_ch));
            hinted = 0;
        }
        if (!hinted) {
            pspDebugScreenSetTextColor(COL_DIM);
            pspDebugScreenSetXY(0, ANIM_ROW - 2);
            pspDebugScreenPrintf("SELECT: discard entropy and sweep again");
            pspDebugScreenSetTextColor(COL_TEXT);
            hinted = 1;
        }
        int pos = frame % 40;
        if ((frame / 40) % 2) pos = 39 - pos;

        pspDebugScreenSetXY(0, ANIM_ROW);
        pspDebugScreenPrintf("%c ", spin[frame % 4]);
        for (int x = 0; x < 40; x++) pspDebugScreenPrintf("%c", x == pos ? '#' : '-');

        frame++;
        sceDisplayWaitVblankStart();
        sceDisplayWaitVblankStart();
        sceDisplayWaitVblankStart();
    }
}

/* ------------------------------------------------------------------ main */

int main(void) {
    if (setup_callbacks() < 0) {
        pspDebugScreenInit();
        pspDebugScreenPrintf("exit callback failed; HOME will not work\n");
    }
    pspDebugScreenInit();
    pspDebugScreenPrintf("PSPDX  https://" HOST PATH "\nconnecting...\n");

    /* Before anything touches the network: fill the entropy pool, then hand
       wolfSSL the source. Without this every key it derives is guessable. */
    psprandom_init();
    trace_load();
    if (!g_replay && psprandom_load()) {
        g_pool_bits = ENTROPY_BITS;              /* carried over from last run */
    } else {
        psprandom_sweep();
    }
    psprandom_save();
    pspDebugScreenClear();

    int rc;
    if (net_up() < 0) {
        logline("network failed");
        rc = -1;
    } else {
        logline("net up");
        rc = fetch();
        net_down();
    }
    dump_to_stick();

    pspDebugScreenClear();
    pspDebugScreenSetXY(0, 0);
    pspDebugScreenPrintf("PSPDX  https://" HOST PATH "\n");

    if (rc >= 0 && g_body) {
        pspDebugScreenPrintf("HTTP %ld  %s  %lu bytes\n", g_status,
                             g_truncated ? "truncated" : "complete",
                             (unsigned long)g_bodylen);
        draw_wrapped(g_body, g_bodylen, 4, ANIM_ROW - 1);
    } else {
        draw_wrapped(g_log[0], 0, 2, 2);
        for (int i = 0; i < g_logn && i + 2 < ANIM_ROW - 1; i++) {
            pspDebugScreenSetXY(0, 2 + i);
            pspDebugScreenPrintf("%s", g_log[i]);
        }
    }

    animate_forever();
    return 0;
}
