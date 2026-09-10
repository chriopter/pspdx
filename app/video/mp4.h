#ifndef PSPDX_MP4_H
#define PSPDX_MP4_H

#include <stddef.h>

/* Just enough of ISO base media to find the one H.264 track in the videos
   the catalog serves: the parameter sets, and where every sample is, how
   big it is, when it shows, and whether it is a keyframe. Plain C, no
   platform. Nothing here decodes anything. */

#define MP4_MAX_SAMPLES 2048
#define MP4_MAX_PARAMS 256

struct mp4_sample {
    unsigned offset, size;
    unsigned pts;               /* in 90 kHz ticks from the start */
    unsigned char key;
};

struct mp4 {
    int width, height;
    int nal_length_size;        /* 1, 2 or 4: how NAL units are prefixed */
    unsigned char sps[MP4_MAX_PARAMS], pps[MP4_MAX_PARAMS];
    int sps_len, pps_len;
    unsigned timescale;
    unsigned duration;          /* in 90 kHz ticks */
    int count;
    struct mp4_sample sample[MP4_MAX_SAMPLES];
};

/* 0 on success. Logs nothing itself: the caller knows the file's name. */
int mp4_parse(const unsigned char *data, size_t len, struct mp4 *out);

#endif
