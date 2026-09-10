#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <string.h>

#include "logic/entropy.h"

#define POOL_BYTES 20
#define SEED_FILE "ms0:/PSPDX.SEED"

static unsigned char pool[POOL_BYTES];
static unsigned int pool_counter;
static int pool_bits;

static void pool_absorb(const void *data, unsigned int len) {
    unsigned char buf[POOL_BYTES + 64];
    unsigned int n = len > 64 ? 64 : len;
    memcpy(buf, pool, POOL_BYTES);
    memcpy(buf + POOL_BYTES, data, n);
    sceKernelUtilsSha1Digest(buf, POOL_BYTES + n, pool);
}

static void pool_absorb_jitter(int rounds) {
    for (int i = 0; i < rounds; i++) {
        unsigned t0 = sceKernelGetSystemTimeLow();
        unsigned spins = 0;
        while (sceKernelGetSystemTimeLow() - t0 < 500) spins++;
        pool_absorb(&spins, sizeof(spins));
    }
}

void entropy_init(void) {
    unsigned t = sceKernelGetSystemTimeLow();
    void *sp = &t;
    pool_absorb(&t, sizeof(t));
    pool_absorb(&sp, sizeof(sp));
    pool_absorb_jitter(8);
    pool_bits = 0;
}

void entropy_absorb_motion(unsigned char lx, unsigned char ly, float x, float y) {
    struct {
        unsigned char lx, ly;
        unsigned int sys;
        float x, y;
    } sample;
    sample.lx = lx;
    sample.ly = ly;
    sample.sys = sceKernelGetSystemTimeLow();
    sample.x = x;
    sample.y = y;
    pool_absorb(&sample, sizeof(sample));
    pool_bits += 2;
}

int entropy_bits(void) { return pool_bits; }

int entropy_load(void) {
    int fd = sceIoOpen(SEED_FILE, PSP_O_RDONLY, 0777);
    if (fd < 0) return 0;
    unsigned char stored[POOL_BYTES];
    int n = sceIoRead(fd, stored, sizeof(stored));
    sceIoClose(fd);
    if (n != (int)sizeof(stored)) return 0;
    pool_absorb(stored, sizeof(stored));
    pool_absorb_jitter(4);
    pool_bits = ENTROPY_BITS;
    return 1;
}

void entropy_save(int replaying) {
    if (replaying) return;
    unsigned char next[POOL_BYTES];
    unsigned int tag = 0x50535058;
    unsigned char buf[POOL_BYTES + sizeof(tag)];
    memcpy(buf, pool, POOL_BYTES);
    memcpy(buf + POOL_BYTES, &tag, sizeof(tag));
    sceKernelUtilsSha1Digest(buf, sizeof(buf), next);
    int fd = sceIoOpen(SEED_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, next, sizeof(next));
        sceIoClose(fd);
    }
}

void entropy_forget(void) {
    sceIoRemove(SEED_FILE);
    pool_bits = 0;
    memset(pool, 0, sizeof(pool));
}

int psprandom_seed_raw(unsigned char *seed, unsigned int size) {
    while (size > 0) {
        unsigned char buf[POOL_BYTES + sizeof(unsigned int)];
        unsigned char out[POOL_BYTES];
        memcpy(buf, pool, POOL_BYTES);
        memcpy(buf + POOL_BYTES, &pool_counter, sizeof(pool_counter));
        sceKernelUtilsSha1Digest(buf, sizeof(buf), out);
        pool_counter++;
        unsigned n = size < sizeof(out) ? size : (unsigned)sizeof(out);
        memcpy(seed, out, n);
        seed += n;
        size -= n;
        pool_absorb(out, sizeof(out));
    }
    return 0;
}
