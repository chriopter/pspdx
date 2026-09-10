#include <pspaudio.h>
#include <pspaudiolib.h>

#include "audio/audio.h"
#include "audio/music.h"
#include "audio/synth.h"
#include "util/runtime.h"

#define RATE 44100

static int g_up;
static volatile unsigned g_worst_us;

static void fill(void *buf, unsigned int frames, void *userdata) {
    (void)userdata;
    unsigned t0 = now_us();
    music_render(buf, (int)frames);
    unsigned took = now_us() - t0;
    if (took > g_worst_us) g_worst_us = took;
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
    if (pspAudioInit() < 0) {
        logline("audio: init failed");
        return 0;
    }
    pspAudioSetChannelCallback(0, fill, 0);
    g_up = 1;
    logline("audio: up");
    return 1;
}

void audio_stop(void) {
    if (!g_up) return;
    pspAudioSetChannelCallback(0, 0, 0);
    pspAudioEnd();
    g_up = 0;
}
