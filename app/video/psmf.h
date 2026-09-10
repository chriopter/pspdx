#ifndef PSPDX_PSMF_H
#define PSPDX_PSMF_H

#include <stddef.h>

#include "video/mp4.h"

/* Wraps the H.264 out of an MP4 the way the PSP's own movie player wants
   it: a PSMF header, then an MPEG-2 program stream in 2048-byte packs with
   the video in PES 0xE0. Same pictures, different envelope. Plain C; the
   result is what sceMpeg reads, and what ffmpeg reads too, which is how it
   gets checked on a desk. */

#define PSMF_HEADER 0x800
#define PSMF_PACK 2048

/* How much room to give psmf_build for a given MP4: the stream plus its
   packaging, rounded up. */
size_t psmf_capacity(size_t mp4_len);

/* Returns the number of bytes written, 0 if out is too small or the track
   is not something the PSP can play. */
size_t psmf_build(const unsigned char *mp4, const struct mp4 *track,
                  unsigned char *out, size_t cap);

#endif
