#include <pspaudio.h>
#include <pspkernel.h>

#include "audio/audio.h"
#include "audio/music.h"
#include "audio/synth.h"
#include "util/runtime.h"

#define RATE 44100

/* Half a thousand frames: twelve milliseconds of sound a chunk, rendered
   in two or three, so the thread is off the CPU nine parts in ten and no
   one burst of the interface's own work is long enough to starve it.

   The thread sits one step under the interface on purpose. pspaudiolib puts
   its thread far above everything, and then a chunk being rendered as the
   vblank fires holds the frame back by the length of the render -- the
   buffers swap late, the top of the picture tears. Under the interface it
   can hold back nothing; the interface sleeps most of every frame, and
   that is when the sound is made. */
#define CHUNK 512
#define AUDIO_PRIORITY 0x21
#define AUDIO_STACK (16 * 1024)

static int g_up;
static int g_channel = -1;
static SceUID g_thread = -1;
static volatile int g_quit;
static volatile unsigned g_worst_us;
static short __attribute__((aligned(64))) g_buf[2][CHUNK * 2];

static int run(SceSize args, void *argp) {
    (void)args; (void)argp;
    int b = 0;
    while (!g_quit) {
        unsigned t0 = now_us();
        music_render(g_buf[b], CHUNK);
        unsigned took = now_us() - t0;
        if (took > g_worst_us) g_worst_us = took;
        /* Blocks until the chunk before this one has played out, which is
           what paces the loop. */
        sceAudioOutputPannedBlocking(g_channel, PSP_AUDIO_VOLUME_MAX,
                                     PSP_AUDIO_VOLUME_MAX, g_buf[b]);
        b ^= 1;
    }
    return 0;
}

void audio_duck(int film_on) {
    synth_set_music_level(film_on ? 0.0f : 1.0f);
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
    g_up = 0;
}
