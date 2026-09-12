#include <string.h>

#include "video/psmf.h"

/* Presentation starts at one second, as every PSMF Sony shipped does; the
   emulator warns when it does not. */
#define PTS_BASE 90000ULL

#define PACK_HEADER 14
#define PES_MIN 9                   /* start code, length, three flag bytes */
#define PES_PTS 5

struct writer {
    unsigned char *out;
    size_t cap, pos;
    int overflow;
};

static void put(struct writer *w, const void *data, size_t len) {
    if (w->pos + len > w->cap) { w->overflow = 1; return; }
    memcpy(w->out + w->pos, data, len);
    w->pos += len;
}

static void put8(struct writer *w, unsigned v) { unsigned char b = (unsigned char)v; put(w, &b, 1); }
static void put16(struct writer *w, unsigned v) { put8(w, v >> 8); put8(w, v); }
static void put32(struct writer *w, unsigned v) { put16(w, v >> 16); put16(w, v); }

static void put_ts48(struct writer *w, unsigned long long ts) {
    put16(w, (unsigned)(ts >> 32)); put32(w, (unsigned)ts);
}

/* MPEG-2 pack header: marker bits the demuxers check, a clock, a rate. */
static void pack_header(struct writer *w, unsigned long long scr) {
    unsigned char h[PACK_HEADER];
    h[0] = 0; h[1] = 0; h[2] = 1; h[3] = 0xBA;
    h[4] = (unsigned char)(0x44 | (((scr >> 30) & 7) << 3) | (((scr >> 28) & 3)));
    h[5] = (unsigned char)((scr >> 20) & 0xFF);
    h[6] = (unsigned char)(0x04 | (((scr >> 15) & 0x1F) << 3) | ((scr >> 13) & 3));
    h[7] = (unsigned char)((scr >> 5) & 0xFF);
    h[8] = (unsigned char)(0x04 | ((scr & 0x1F) << 3));   /* SCR_ext = 0 */
    h[9] = 0x01;
    unsigned rate = 1500;                                  /* 75 KB/s in 50-byte units */
    h[10] = (unsigned char)(rate >> 14);
    h[11] = (unsigned char)(rate >> 6);
    h[12] = (unsigned char)(((rate & 0x3F) << 2) | 3);
    h[13] = 0xF8;                                          /* reserved, no stuffing */
    put(w, h, sizeof(h));
}

static void pes_pts(struct writer *w, unsigned long long pts) {
    put8(w, 0x21 | (unsigned)(((pts >> 30) & 7) << 1));
    put16(w, 0x0001 | (unsigned)(((pts >> 15) & 0x7FFF) << 1));
    put16(w, 0x0001 | (unsigned)((pts & 0x7FFF) << 1));
}

/* Packs are filled continuously: a PES packet starts wherever the last one
   ended, and only when a pack has no room left for another header and a
   few bytes of payload is the rest padded and a new pack begun. Otherwise
   half a pack per frame is lost to padding, which for 300 frames is more
   than the film. */
struct packer {
    struct writer w;
    size_t pack_start;          /* w.pos of the current pack, or 0 if none */
    unsigned long long scr;
};

static size_t pack_left(const struct packer *p) {
    return p->pack_start ? p->pack_start + PSMF_PACK - p->w.pos : 0;
}

static void pack_close(struct packer *p) {
    size_t left = pack_left(p);
    if (!p->pack_start) return;
    if (left >= 6) {
        put8(&p->w, 0); put8(&p->w, 0); put8(&p->w, 1); put8(&p->w, 0xBE);
        put16(&p->w, (unsigned)(left - 6));
        for (size_t i = 0; i < left - 6; i++) put8(&p->w, 0xFF);
    } else {
        /* Too small even for a padding packet: the previous PES header
           would have absorbed it as stuffing. Should not happen. */
        for (size_t i = 0; i < left; i++) put8(&p->w, 0xFF);
    }
    p->pack_start = 0;
}

static void pack_open(struct packer *p) {
    p->pack_start = p->w.pos;
    pack_header(&p->w, p->scr);
}

/* One access unit as PES packets, the first carrying the timestamp. */
static void put_unit(struct packer *p, const unsigned char *au, size_t len,
                     unsigned long long pts) {
    size_t done = 0;
    int first = 1;
    p->scr = pts > 900 ? pts - 900 : 0;
    while (done < len) {
        size_t header = PES_MIN + (first ? PES_PTS : 0);
        if (pack_left(p) < header + 16) {
            pack_close(p);
            pack_open(p);
        }
        size_t space = pack_left(p) - header;
        size_t take = len - done < space ? len - done : space;
        size_t spare = space - take;
        /* Under a padding packet's worth of spare room goes into this
           header as stuffing bytes, so the pack still comes out whole. */
        size_t stuff = spare < 6 ? spare : 0;
        put8(&p->w, 0); put8(&p->w, 0); put8(&p->w, 1); put8(&p->w, 0xE0);
        put16(&p->w, (unsigned)(3 + (first ? PES_PTS : 0) + stuff + take));
        put8(&p->w, 0x80);
        put8(&p->w, first ? 0x80 : 0x00);
        put8(&p->w, (unsigned)((first ? PES_PTS : 0) + stuff));
        if (first) pes_pts(&p->w, pts);
        for (size_t i = 0; i < stuff; i++) put8(&p->w, 0xFF);
        put(&p->w, au + done, take);
        done += take;
        first = 0;
    }
}

/* The largest access unit we expect: a keyframe at the catalog's bitrate
   is a few tens of kilobytes. Assembled at the end of the output buffer,
   which the stream may never reach. */
#define SCRATCH (128 * 1024)

size_t psmf_capacity(size_t mp4_len) {
    /* Pack and PES headers are under 2% of the payload; parameter sets
       before every keyframe and the final padding are bounded. */
    return PSMF_HEADER + mp4_len + mp4_len / 32 + 64 * 1024 + SCRATCH;
}

/* A sample's NAL units, length-prefixed in the file, become a byte stream
   with start codes; keyframes get the parameter sets in front. Assembled in
   a scratch area at the end of out, which the stream then overwrites. */
static size_t assemble(const unsigned char *mp4, const struct mp4 *t, int i,
                       unsigned char *scratch, size_t cap) {
    const struct mp4_sample *s = &t->sample[i];
    static const unsigned char start[4] = { 0, 0, 0, 1 };
    size_t n = 0;
    if (s->key) {
        if (8 + (size_t)t->sps_len + t->pps_len > cap) return 0;
        memcpy(scratch + n, start, 4); n += 4; memcpy(scratch + n, t->sps, t->sps_len); n += t->sps_len;
        memcpy(scratch + n, start, 4); n += 4; memcpy(scratch + n, t->pps, t->pps_len); n += t->pps_len;
    }
    const unsigned char *p = mp4 + s->offset, *end = p + s->size;
    while (p + t->nal_length_size <= end) {
        unsigned len = 0;
        for (int k = 0; k < t->nal_length_size; k++) len = (len << 8) | *p++;
        if (len == 0 || p + len > end) break;
        if (n + 4 + len > cap) return 0;
        memcpy(scratch + n, start, 4); n += 4;
        memcpy(scratch + n, p, len); n += len;
        p += len;
    }
    return n;
}

size_t psmf_build(const unsigned char *mp4, const struct mp4 *t,
                  unsigned char *out, size_t cap) {
    if (t->count <= 0 || !t->sps_len || !t->pps_len || cap < PSMF_HEADER + PSMF_PACK)
        return 0;
    if (t->width > 720 || t->height > 480 || (t->width & 15) || (t->height & 15))
        return 0;

    struct packer pk = { { out, cap - SCRATCH, PSMF_HEADER, 0 }, 0, 0 };
    /* The largest unit we assemble has to fit behind the stream it will
       be written into; the stream is at most the input plus packaging. */
    size_t scratch_cap = SCRATCH;
    if (cap < PSMF_HEADER + PSMF_PACK + scratch_cap) return 0;
    unsigned char *scratch = out + cap - scratch_cap;

    unsigned key_packs[180];
    unsigned key_pts[180];
    int keys = 0;

    for (int i = 0; i < t->count; i++) {
        size_t n = assemble(mp4, t, i, scratch, scratch_cap);
        if (!n) return 0;
        unsigned long long pts = PTS_BASE + t->sample[i].pts;
        if (t->sample[i].key && keys < 180) {
            size_t at = pk.pack_start ? pk.pack_start : pk.w.pos;
            key_packs[keys] = (unsigned)((at - PSMF_HEADER) / PSMF_PACK);
            key_pts[keys] = (unsigned)pts;
            keys++;
        }
        put_unit(&pk, scratch, n, pts);
        if (pk.w.overflow) return 0;
    }
    pack_close(&pk);
    if (pk.w.overflow) return 0;
    size_t stream = pk.w.pos - PSMF_HEADER;
    unsigned long long last = PTS_BASE + t->duration;

    /* The header: what sceMpeg reads at fixed offsets, and the one stream
       entry with its keyframe map. Unknown fields stay zero. */
    memset(out, 0, PSMF_HEADER);
    struct writer h = { out, PSMF_HEADER, 0, 0 };
    put(&h, "PSMF", 4);
    put(&h, "0015", 4);
    put32(&h, PSMF_HEADER);
    put32(&h, (unsigned)stream);
    h.pos = 0x50; put32(&h, (unsigned)stream);
    h.pos = 0x54; put_ts48(&h, PTS_BASE);
    h.pos = 0x5A; put_ts48(&h, last);
    h.pos = 0x6A; put32(&h, PSMF_PACK);
    h.pos = 0x7C; put32(&h, PSMF_PACK);
    h.pos = 0x80; put16(&h, 1);
    h.pos = 0x82;
    put8(&h, 0xE0); put8(&h, 0x00); put16(&h, 0);
    put32(&h, 0x100);                       /* keyframe map, inside the header */
    put32(&h, (unsigned)keys);
    put8(&h, (unsigned)(t->width / 16)); put8(&h, (unsigned)(t->height / 16));
    h.pos = 0x100;
    for (int k = 0; k < keys; k++) {
        put8(&h, 0); put8(&h, 0);
        put32(&h, key_pts[k]);
        put32(&h, key_packs[k]);
    }
    return pk.w.pos;
}

/* ---------------------------------------------------------- reading one */

static unsigned get16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }
static unsigned get32(const unsigned char *p) { return (get16(p) << 16) | get16(p + 2); }

/* The 48-bit timestamps at 0x54 and 0x5A: the low 32 bits are all a film of
   a few seconds ever uses, and all the pacing arithmetic wants. */
static unsigned get_ts48(const unsigned char *p) { return get32(p + 2); }

int psmf_is(const unsigned char *data, size_t len) {
    return len >= 4 && memcmp(data, "PSMF", 4) == 0;
}

/* Counts the video PES packets that carry a timestamp, one per access
   unit: the muxers Sony shipped put one on the first PES of every picture,
   and so does psmf_build. The packs are walked in place; a pack that does
   not start with a pack header ends the count, which is where the stream
   ends too. */
static int count_frames(const unsigned char *s, size_t len) {
    int frames = 0;
    for (size_t at = 0; at + PSMF_PACK <= len; at += PSMF_PACK) {
        const unsigned char *p = s + at, *end = p + PSMF_PACK;
        if (p[0] != 0 || p[1] != 0 || p[2] != 1 || p[3] != 0xBA) break;
        p += PACK_HEADER + (p[13] & 7);
        /* PES packets, the system header and padding all share the shape
           start code, id, 16-bit length; anything else is stuffing to the
           end of the pack. */
        while (p + 6 <= end && p[0] == 0 && p[1] == 0 && p[2] == 1) {
            unsigned id = p[3], size = get16(p + 4);
            const unsigned char *next = p + 6 + size;
            if ((id & 0xF0) == 0xE0 && p + 8 <= end && (p[7] & 0x80)) frames++;
            if (next > end) break;
            p = next;
        }
    }
    return frames;
}

int psmf_parse(const unsigned char *data, size_t len, struct psmf_info *out) {
    if (!psmf_is(data, len) || len < 0x100) return -1;
    memset(out, 0, sizeof(*out));
    out->stream_offset = get32(data + 0x8);
    out->stream_size = get32(data + 0xC);
    if (out->stream_offset < 0x100 || out->stream_offset >= len) return -1;
    /* A file cut short still plays as far as it goes. */
    if (out->stream_size > len - out->stream_offset) out->stream_size = len - out->stream_offset;
    out->start_pts = get_ts48(data + 0x54);
    out->end_pts = get_ts48(data + 0x5A);

    /* The stream table at 0x80: a count, then sixteen bytes an entry, the
       video's carrying its size in macroblocks at the end. An ICON1.PMF
       lists an audio stream too, in PES 0xBD, which the player leaves in
       the ring unread. */
    unsigned streams = get16(data + 0x80);
    for (unsigned i = 0; i < streams && 0x82 + (i + 1) * 16 <= out->stream_offset; i++) {
        const unsigned char *e = data + 0x82 + i * 16;
        if ((e[0] & 0xF0) != 0xE0) continue;
        out->width = e[0xC] * 16;
        out->height = e[0xD] * 16;
        break;
    }
    if (!out->width || !out->height) return -1;
    out->frames = count_frames(data + out->stream_offset, out->stream_size);
    return 0;
}

unsigned psmf_frame_ticks(const struct psmf_info *info) {
    unsigned ticks = 3000;
    if (info->frames > 0 && info->end_pts > info->start_pts)
        ticks = (info->end_pts - info->start_pts) / (unsigned)info->frames;
    /* Between ten and sixty a second: outside that the header is lying,
       and thirty is what every film so far has been. */
    if (ticks < 1500 || ticks > 9000) ticks = 3000;
    return ticks;
}
