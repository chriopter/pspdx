#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <string.h>

#include "logic/entropy.h"

#define POOL_BYTES 20
#define SEED_FILE "ms0:/PSPDX.SEED"

/* The PSP leaves through sceKernelExitGame, so there is no reliable moment at
   the end to write the seed: it has to be written along the way. Sixty-four
   stirs is a few seconds of traffic, cheap against a 20-byte write. */
#define STIR_PER_SAVE 64

static unsigned char pool[POOL_BYTES];
static unsigned int pool_counter;
static int pool_bits;
static unsigned char field_seen[(ENTROPY_FIELD_COUNT + 7) / 8];
static unsigned int stir_count;
static int replay_lock;

/* sync runs the network on its own thread while the shell keeps the main one,
   and SELECT starts a fresh sweep from there: two threads reach the pool, and
   since entropy_stir writes the seed as it goes, two could reach the file. The
   lock is taken per operation, never across a sweep, so the handshake is never
   left waiting on a hand at the stick. */
static SceUID pool_sema = -1;

static void pool_lock(void) {
    if (pool_sema >= 0) sceKernelWaitSema(pool_sema, 1, NULL);
}

static void pool_unlock(void) {
    if (pool_sema >= 0) sceKernelSignalSema(pool_sema, 1);
}

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
    if (pool_sema < 0) pool_sema = sceKernelCreateSema("entropy", 0, 1, 1, NULL);
    unsigned t = sceKernelGetSystemTimeLow();
    void *sp = &t;
    pool_lock();
    pool_absorb(&t, sizeof(t));
    pool_absorb(&sp, sizeof(sp));
    pool_absorb_jitter(8);
    pool_bits = 0;
    stir_count = 0;
    memset(field_seen, 0, sizeof(field_seen));
    pool_unlock();
}

int entropy_absorb_field(unsigned int field) {
    if (field >= ENTROPY_FIELD_COUNT) return 0;
    unsigned int byte = field >> 3;
    unsigned char bit = (unsigned char)(1u << (field & 7));
    pool_lock();
    if (field_seen[byte] & bit) {
        pool_unlock();
        return 0;
    }
    field_seen[byte] |= bit;

    /* The field number is what the bar is counting. The timestamp rides along
       because the moment the point is reached is unpredictable too, and the
       pool is happy to take it; it is not counted, so the tally stays honest. */
    struct {
        unsigned int field;
        unsigned int sys;
    } sample;
    sample.field = field;
    sample.sys = sceKernelGetSystemTimeLow();
    pool_absorb(&sample, sizeof(sample));
    pool_bits += ENTROPY_BITS_PER_FIELD;
    pool_unlock();
    return 1;
}

void entropy_stir(const void *data, unsigned int len) {
    unsigned int sys = sceKernelGetSystemTimeLow();
    pool_lock();
    pool_absorb(data, len);
    pool_absorb(&sys, sizeof(sys));
    /* A pool that was never credited full has no business on the stick: a
       stored seed skips the sweep on the next run and is taken for the whole
       128 bits. nettest reaches here with a pool that has only ever seen boot
       jitter, and writing that would quietly downgrade the next start. */
    int due = ++stir_count >= STIR_PER_SAVE && pool_bits >= ENTROPY_BITS;
    if (due) stir_count = 0;
    pool_unlock();
    if (due) entropy_save(replay_lock);
}

int entropy_bits(void) { return pool_bits; }

int entropy_load(void) {
    int fd = sceIoOpen(SEED_FILE, PSP_O_RDONLY, 0777);
    if (fd < 0) return 0;
    unsigned char stored[POOL_BYTES];
    int n = sceIoRead(fd, stored, sizeof(stored));
    sceIoClose(fd);
    if (n != (int)sizeof(stored)) return 0;
    pool_lock();
    pool_absorb(stored, sizeof(stored));
    pool_absorb_jitter(4);
    pool_bits = ENTROPY_BITS;
    pool_unlock();
    return 1;
}

/* The first call comes from startup and settles the question for the run: a
   replayed sweep is public input, so nothing from that session may ever reach
   the stick, including the automatic saves that entropy_stir makes later. */
void entropy_save(int replaying) {
    if (replaying) {
        replay_lock = 1;
        return;
    }
    unsigned char next[POOL_BYTES];
    unsigned int tag = 0x50535058;
    unsigned char buf[POOL_BYTES + sizeof(tag)];
    pool_lock();
    memcpy(buf, pool, POOL_BYTES);
    memcpy(buf + POOL_BYTES, &tag, sizeof(tag));
    sceKernelUtilsSha1Digest(buf, sizeof(buf), next);
    /* The write stays inside the lock: released here, two threads could be in
       this file at once, and a half-written seed still reads back as twenty
       bytes and is taken for a full pool on the next run. A short wait on the
       stick is the cheaper end of that trade. */
    int fd = sceIoOpen(SEED_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, next, sizeof(next));
        sceIoClose(fd);
    }
    pool_unlock();
}

void entropy_forget(void) {
    pool_lock();
    /* Inside the lock, or an automatic save from the network thread lands
       between the removal and the clearing and puts the file straight back. */
    sceIoRemove(SEED_FILE);
    pool_bits = 0;
    memset(pool, 0, sizeof(pool));
    memset(field_seen, 0, sizeof(field_seen));
    stir_count = 0;
    pool_unlock();
}

int psprandom_seed_raw(unsigned char *seed, unsigned int size) {
    pool_lock();
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
    pool_unlock();
    return 0;
}
