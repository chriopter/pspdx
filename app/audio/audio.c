#include <pspaudio.h>
#include <pspatrac3.h>
#include <pspkernel.h>
#include <psputility.h>
#include <psputility_avmodules.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "audio/audio.h"
#include "audio/music.h"
#include "audio/synth.h"
#include "util/runtime.h"
#include "video/player.h"

#define RATE 44100

/* A thousand frames: twenty-three milliseconds of sound a chunk, rendered
   in four or five, so the thread is off the CPU four parts in five. It sat
   at half that until the performance runs showed the interface, held at
   full scroll with the water and a film, keeping this thread waiting for
   eight of a twelve-millisecond chunk; a chunk twice as long has twice
   the slack, and nobody hears twenty-three milliseconds of latency on a
   tune.

   The thread sits one step under the interface on purpose. pspaudiolib puts
   its thread far above everything, and then a chunk being rendered as the
   vblank fires holds the frame back by the length of the render -- the
   buffers swap late, the top of the picture tears. Under the interface it
   can hold back nothing; the interface sleeps most of every frame, and
   that is when the sound is made. */
#define CHUNK 1024
#define AUDIO_PRIORITY 0x21
#define AUDIO_STACK (16 * 1024)

static int g_up;
static int g_channel = -1;
static SceUID g_thread = -1;
static volatile int g_quit;
static volatile unsigned g_worst_us;
static short __attribute__((aligned(64))) g_buf[2][CHUNK * 2];

/* ------------------------------------------------------ the card's sound */

/* Under the notes the list plays and well under the tune's own peaks: the
   SND0 is atmosphere for the card, not a track. */
#define SOUND_LEVEL 0.30f
#define SOUND_IN_S 0.3f
#define SOUND_OUT_S 0.2f

/* One decode of ATRAC3 is 1024 samples a channel and of ATRAC3plus 2048;
   the ring holds that plus two chunks with room over, so the thread never
   decodes more than it needs for the chunk in hand. */
#define SOUND_FRAME 2048
#define SOUND_RING 8192

/* The hand-over. Play and stop leave the bytes here under the lock and
   move seq on; the audio thread notices at its next chunk, takes them
   under the same lock, and does everything else on its own, so neither
   caller ever waits on a decode. The lock is held for a pointer swap. */
static SceUID g_snd_lock = -1;
static void *g_snd_next;
static size_t g_snd_next_len;
static volatile unsigned g_snd_seq, g_snd_taken;

/* Audio thread only from here on. */
static void *g_snd_buf;                 /* the AT3; sceAtrac reads it in place */
static size_t g_snd_len;
static int g_snd_id = -1;
static int g_snd_modules;               /* AVCODEC and ATRAC3PLUS are loaded */
static short __attribute__((aligned(64))) g_snd_frame[SOUND_FRAME * 2];
static short g_snd_ring[SOUND_RING * 2];
static int g_snd_head, g_snd_fill;      /* read position, frames queued */
static float g_snd_gain, g_snd_target;
static int g_snd_loops, g_snd_decoded;  /* times round, and samples this time */
static volatile int g_snd_on;           /* audio_duck reads this from the interface */

static void sound_close(void) {
    if (g_snd_id >= 0) {
        sceAtracReleaseAtracID(g_snd_id);
        logline("sound: off after %d times round", g_snd_loops);
    }
    g_snd_id = -1;
    free(g_snd_buf);
    g_snd_buf = 0;
    g_snd_len = 0;
    g_snd_head = g_snd_fill = 0;
    g_snd_gain = g_snd_target = 0.0f;
    g_snd_on = 0;
}

static unsigned le16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned le32(const unsigned char *p) { return le16(p) | (le16(p + 2) << 16); }

/* Opens the AT3 the thread has just taken. What sceAtrac cannot say is
   read off the RIFF header first: the sample rate, which has to be the
   channel's, and the channel count, which has to be two -- the stub set
   here has no way to ask what a mono file comes out as, and no SND0 has
   ever been mono. */
static int sound_open(void) {
    const unsigned char *h = g_snd_buf;
    if (g_snd_len < 36 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0 ||
        memcmp(h + 12, "fmt ", 4) != 0) {
        logline("sound: not a RIFF");
        return -1;
    }
    unsigned tag = le16(h + 20), channels = le16(h + 22), rate = le32(h + 24);
    if (rate != RATE || channels != 2) {
        logline("sound: %u Hz, %u channels; wants %d Hz stereo", rate, channels, RATE);
        return -1;
    }
    int id = sceAtracSetDataAndGetID(g_snd_buf, (SceSize)g_snd_len);
    if (id < 0) {
        logline("sound: set data %08x (tag %04x)", (unsigned)id, tag);
        return -1;
    }
    g_snd_id = id;
    int max = 0;
    if (sceAtracGetMaxSample(id, &max) < 0 || max <= 0 || max > SOUND_FRAME) {
        logline("sound: %d samples a decode, at most %d", max, SOUND_FRAME);
        return -1;
    }
    /* Looping is the decoder's when the file carries loop points, and
       ours when it does not: it refuses the request then, and the end of
       the data is where this rewinds it. */
    int rc = sceAtracSetLoopNum(id, -1);
    logline("sound: tag %04x, %u Hz, %d samples a decode, %lu KB, loop %s", tag, rate, max,
            (unsigned long)(g_snd_len / 1024), rc < 0 ? "by hand" : "by the decoder");
    g_snd_loops = g_snd_decoded = 0;
    return 0;
}

/* Back to the top, in place; when that is refused the same bytes are
   opened again, which is what a fresh ID costs. */
static int sound_rewind(void) {
    if (sceAtracResetPlayPosition(g_snd_id, 0, 0, 0) >= 0) return 0;
    sceAtracReleaseAtracID(g_snd_id);
    g_snd_id = sceAtracSetDataAndGetID(g_snd_buf, (SceSize)g_snd_len);
    if (g_snd_id < 0) { logline("sound: rewind %08x", (unsigned)g_snd_id); return -1; }
    return 0;
}

/* The end of the data, seen either way the decoder reports it. Logged the
   first time round, so the log says how long the loop is. */
static void sound_end(void) {
    if (g_snd_loops++ == 0) logline("sound: %d samples, again", g_snd_decoded);
    g_snd_decoded = 0;
    if (sound_rewind() != 0) sound_close();
}

/* One decode into the ring. Returns 1 while there is more to come, 0 when
   the sound is over or the ring has no room. */
static int sound_decode(void) {
    if (g_snd_id < 0 || g_snd_fill + SOUND_FRAME > SOUND_RING) return 0;
    int n = 0, end = 0, remain = 0;
    int rc = sceAtracDecodeData(g_snd_id, (u16 *)g_snd_frame, &n, &end, &remain);
    if (rc == (int)PSP_ATRAC_ERROR_ALLDATA_WAS_DECODED) {
        /* Nothing came with it; the ring is filled by the next call. */
        sound_end();
        return g_snd_id >= 0;
    }
    if (rc < 0) {
        logline("sound: decode %08x", (unsigned)rc);
        sound_close();
        return 0;
    }
    if (n > SOUND_FRAME) n = SOUND_FRAME;
    if (g_snd_loops == 0 && g_snd_decoded == 0) {
        /* Once, so a rig run can see that the samples are a signal and
           not the silence a decoder that failed quietly would give. */
        int peak = 0;
        for (int i = 0; i < n * 2; i++) {
            int v = g_snd_frame[i] < 0 ? -g_snd_frame[i] : g_snd_frame[i];
            if (v > peak) peak = v;
        }
        logline("sound: first decode %d samples, peak %d", n, peak);
    }
    int tail = (g_snd_head + g_snd_fill) % SOUND_RING;
    int first = n < SOUND_RING - tail ? n : SOUND_RING - tail;
    memcpy(g_snd_ring + tail * 2, g_snd_frame, (size_t)first * 4);
    if (n > first) memcpy(g_snd_ring, g_snd_frame + first * 2, (size_t)(n - first) * 4);
    g_snd_fill += n;
    g_snd_decoded += n;
    if (end) sound_end();
    return g_snd_id >= 0;
}

/* Takes what play or stop left, once the sound before it has faded. */
static void sound_take(void) {
    sceKernelWaitSema(g_snd_lock, 1, 0);
    void *buf = g_snd_next;
    size_t len = g_snd_next_len;
    g_snd_next = 0;
    g_snd_next_len = 0;
    g_snd_taken = g_snd_seq;
    sceKernelSignalSema(g_snd_lock, 1);
    sound_close();
    if (!buf) return;
    g_snd_buf = buf;
    g_snd_len = len;
    if (!g_snd_modules || sound_open() != 0) { sound_close(); return; }
    g_snd_target = 1.0f;
    g_snd_on = 1;
}

/* Mixes the sound under a chunk the tune has already rendered. */
static void sound_render(short *out, int frames) {
    if (g_snd_seq != g_snd_taken) {
        /* A sound still sounding goes out first, over its fade, and only
           then is the next one opened; a sweep down the list hands over
           several times inside one fade and only the last one is taken. */
        if (g_snd_id >= 0 && g_snd_gain > 0.001f) g_snd_target = 0.0f;
        else sound_take();
    }
    if (g_snd_id < 0) return;
    /* A chunk is one or two decodes; the cap is for a rewind that keeps
       coming up empty, which would otherwise hold the chunk for ever. */
    for (int tries = 0; g_snd_fill < frames && tries < 8; tries++)
        if (!sound_decode()) break;
    if (g_snd_id < 0) return;

    float g = g_snd_gain, target = g_snd_target;
    float step = target > g ? 1.0f / (RATE * SOUND_IN_S) : -1.0f / (RATE * SOUND_OUT_S);
    int have = g_snd_fill < frames ? g_snd_fill : frames;
    for (int f = 0; f < have; f++) {
        if (step > 0.0f ? g < target : g > target) g += step;
        else g = target;
        float k = g * SOUND_LEVEL;
        const short *s = g_snd_ring + ((g_snd_head + f) % SOUND_RING) * 2;
        int l = out[f * 2] + (int)(s[0] * k), r = out[f * 2 + 1] + (int)(s[1] * k);
        out[f * 2] = (short)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[f * 2 + 1] = (short)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
    g_snd_head = (g_snd_head + have) % SOUND_RING;
    g_snd_fill -= have;
    g_snd_gain = g;
}

/* Whichever thread calls, the bytes are its own again on return; the
   copy is the audio side's until it lets go. */
static void sound_post(void *copy, size_t len) {
    if (g_snd_lock < 0) { free(copy); return; }
    sceKernelWaitSema(g_snd_lock, 1, 0);
    free(g_snd_next);
    g_snd_next = copy;
    g_snd_next_len = len;
    g_snd_seq++;
    sceKernelSignalSema(g_snd_lock, 1);
}

void audio_sound_play(const void *at3, size_t len) {
    if (!at3 || !len) { audio_sound_stop(); return; }
    /* sceAtrac wants its buffer four-byte aligned and reads it for as long
       as the ID lives, which is why it is copied rather than borrowed. */
    void *copy = memalign(64, len);
    if (!copy) { logline("sound: no room for %lu", (unsigned long)len); return; }
    memcpy(copy, at3, len);
    sound_post(copy, len);
}

void audio_sound_stop(void) {
    sound_post(0, 0);
}

/* The AV modules, once: the film's decoder loads AVCODEC through the
   player and this asks it to, then adds ATRAC3PLUS, which the sceAtrac
   calls resolve against. Done before the thread starts, so the first
   sound does not spend its chunk on a module load. */
static void sound_modules(void) {
    if (!player_avcodec_up()) return;
    int rc = sceUtilityLoadAvModule(PSP_AV_MODULE_ATRAC3PLUS);
    if (rc < 0) { logline("sound: atrac3plus module %08x", (unsigned)rc); return; }
    g_snd_modules = 1;
}

/* ------------------------------------------------------------ the thread */

static int run(SceSize args, void *argp) {
    (void)args; (void)argp;
    int b = 0;
    while (!g_quit) {
        unsigned t0 = now_us();
        music_render(g_buf[b], CHUNK);
        sound_render(g_buf[b], CHUNK);
        unsigned took = now_us() - t0;
        if (took > g_worst_us) g_worst_us = took;
        /* Blocks until the chunk before this one has played out, which is
           what paces the loop. */
        sceAudioOutputPannedBlocking(g_channel, PSP_AUDIO_VOLUME_MAX,
                                     PSP_AUDIO_VOLUME_MAX, g_buf[b]);
        b ^= 1;
    }
    sound_close();
    return 0;
}

void audio_duck(int film_on) {
    synth_set_music_level(film_on || g_snd_on ? 0.0f : 1.0f);
}

unsigned audio_worst_us(void) {
    unsigned w = g_worst_us;
    g_worst_us = 0;
    return w;
}

int audio_start(void) {
    if (g_up) return 1;
    synth_init(RATE);
    music_init(RATE);
    g_channel = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, CHUNK, PSP_AUDIO_FORMAT_STEREO);
    if (g_channel < 0) {
        logline("audio: no channel %08x", (unsigned)g_channel);
        return 0;
    }
    g_snd_lock = sceKernelCreateSema("sound", 0, 1, 1, 0);
    if (g_snd_lock < 0) logline("audio: no sound lock %08x", (unsigned)g_snd_lock);
    sound_modules();
    g_quit = 0;
    g_thread = sceKernelCreateThread("audio", run, AUDIO_PRIORITY, AUDIO_STACK,
                                     PSP_THREAD_ATTR_USER, 0);
    if (g_thread < 0) {
        logline("audio: no thread %08x", (unsigned)g_thread);
        sceAudioChRelease(g_channel);
        g_channel = -1;
        return 0;
    }
    sceKernelStartThread(g_thread, 0, 0);
    g_up = 1;
    logline("audio: up");
    return 1;
}

void audio_stop(void) {
    if (!g_up) return;
    g_quit = 1;
    sceKernelWaitThreadEnd(g_thread, 0);
    sceKernelDeleteThread(g_thread);
    g_thread = -1;
    sceAudioChRelease(g_channel);
    g_channel = -1;
    if (g_snd_lock >= 0) {
        sceKernelDeleteSema(g_snd_lock);
        g_snd_lock = -1;
    }
    free(g_snd_next);
    g_snd_next = 0;
    g_up = 0;
}
