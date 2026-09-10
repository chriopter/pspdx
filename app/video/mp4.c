#include <string.h>

#include "video/mp4.h"

static unsigned be32(const unsigned char *p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}
static unsigned be16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }

/* One box: type at *pos, payload span returned, *pos moved past it. */
struct box { unsigned type; const unsigned char *body; size_t len; };

static int next_box(const unsigned char *data, size_t end, size_t *pos, struct box *b) {
    if (*pos + 8 > end) return 0;
    const unsigned char *p = data + *pos;
    unsigned long long size = be32(p);
    size_t header = 8;
    b->type = be32(p + 4);
    if (size == 1) {
        if (*pos + 16 > end) return 0;
        size = ((unsigned long long)be32(p + 8) << 32) | be32(p + 12);
        header = 16;
    } else if (size == 0) {
        size = end - *pos;
    }
    if (size < header || *pos + size > end) return 0;
    b->body = p + header;
    b->len = (size_t)size - header;
    *pos += (size_t)size;
    return 1;
}

#define TYPE(a, b, c, d) (((unsigned)(a) << 24) | ((unsigned)(b) << 16) | ((unsigned)(c) << 8) | (d))

/* The tables under stbl, kept as pointers into the file until we have all
   of them and can walk chunks. */
struct tables {
    const unsigned char *stts, *stss, *stsc, *stsz, *stco, *ctts;
    size_t stts_len, stss_len, stsc_len, stsz_len, stco_len, ctts_len;
    int co64;
};

static int parse_avcc(const unsigned char *p, size_t len, struct mp4 *out) {
    if (len < 7) return -1;
    out->nal_length_size = (p[4] & 3) + 1;
    int nsps = p[5] & 0x1F;
    size_t i = 6;
    for (int n = 0; n < nsps; n++) {
        if (i + 2 > len) return -1;
        unsigned l = be16(p + i); i += 2;
        if (i + l > len) return -1;
        if (n == 0 && l <= MP4_MAX_PARAMS) { memcpy(out->sps, p + i, l); out->sps_len = (int)l; }
        i += l;
    }
    if (i >= len) return -1;
    int npps = p[i++];
    for (int n = 0; n < npps; n++) {
        if (i + 2 > len) return -1;
        unsigned l = be16(p + i); i += 2;
        if (i + l > len) return -1;
        if (n == 0 && l <= MP4_MAX_PARAMS) { memcpy(out->pps, p + i, l); out->pps_len = (int)l; }
        i += l;
    }
    return out->sps_len && out->pps_len ? 0 : -1;
}

static int parse_stsd(const unsigned char *p, size_t len, struct mp4 *out) {
    if (len < 16) return -1;
    /* version/flags, entry count, then the first sample entry: a box whose
       body is 78 bytes of visual sample entry before its own children. */
    size_t pos = 8;
    struct box entry;
    if (!next_box(p, len, &pos, &entry)) return -1;
    if (entry.type != TYPE('a', 'v', 'c', '1') && entry.type != TYPE('a', 'v', 'c', '3')) return -1;
    if (entry.len < 78) return -1;
    out->width = (int)be16(entry.body + 24);
    out->height = (int)be16(entry.body + 26);
    size_t cpos = 78;
    struct box child;
    while (next_box(entry.body, entry.len, &cpos, &child)) {
        if (child.type == TYPE('a', 'v', 'c', 'C'))
            return parse_avcc(child.body, child.len, out);
    }
    return -1;
}

static int build_samples(const struct tables *t, struct mp4 *out) {
    if (!t->stts || !t->stsc || !t->stsz || !t->stco) return -1;

    /* Sizes. */
    unsigned fixed = be32(t->stsz + 4);
    unsigned count = be32(t->stsz + 8);
    if (count == 0 || count > MP4_MAX_SAMPLES) return -1;
    if (!fixed && t->stsz_len < 12 + 4 * (size_t)count) return -1;
    out->count = (int)count;
    for (unsigned i = 0; i < count; i++)
        out->sample[i].size = fixed ? fixed : be32(t->stsz + 12 + 4 * i);

    /* Offsets, by walking chunks. stsc says how many samples each run of
       chunks holds; stco says where each chunk starts. */
    unsigned chunks = be32(t->stco + 4);
    unsigned runs = be32(t->stsc + 4);
    if (chunks == 0 || runs == 0) return -1;
    unsigned s = 0;
    for (unsigned c = 0; c < chunks && s < count; c++) {
        unsigned per = 0;
        for (unsigned r = 0; r < runs; r++) {
            unsigned first = be32(t->stsc + 8 + r * 12);
            if (first - 1 <= c) per = be32(t->stsc + 8 + r * 12 + 4);
        }
        unsigned long long off = t->co64
            ? (((unsigned long long)be32(t->stco + 8 + c * 8) << 32) | be32(t->stco + 8 + c * 8 + 4))
            : be32(t->stco + 8 + c * 4);
        for (unsigned k = 0; k < per && s < count; k++) {
            out->sample[s].offset = (unsigned)off;
            off += out->sample[s].size;
            s++;
        }
    }
    if (s < count) return -1;

    /* Times: stts runs of (count, delta), summed; ctts shifts presentation
       when it exists (baseline has no reordering, so usually not). */
    unsigned entries = be32(t->stts + 4);
    unsigned long long dts = 0;
    s = 0;
    for (unsigned e = 0; e < entries && s < count; e++) {
        unsigned n = be32(t->stts + 8 + e * 8), delta = be32(t->stts + 8 + e * 8 + 4);
        for (unsigned k = 0; k < n && s < count; k++) {
            out->sample[s].pts = (unsigned)(dts * 90000 / out->timescale);
            dts += delta;
            s++;
        }
    }
    out->duration = (unsigned)(dts * 90000 / out->timescale);
    if (t->ctts) {
        unsigned ce = be32(t->ctts + 4);
        s = 0;
        for (unsigned e = 0; e < ce && s < count; e++) {
            unsigned n = be32(t->ctts + 8 + e * 8);
            int off = (int)be32(t->ctts + 8 + e * 8 + 4);
            for (unsigned k = 0; k < n && s < count; k++, s++)
                out->sample[s].pts += (unsigned)((long long)off * 90000 / out->timescale);
        }
    }

    /* Keyframes: every one unless stss says otherwise. */
    if (t->stss) {
        unsigned n = be32(t->stss + 4);
        for (unsigned i = 0; i < n; i++) {
            unsigned idx = be32(t->stss + 8 + i * 4);
            if (idx >= 1 && idx <= count) out->sample[idx - 1].key = 1;
        }
    } else {
        for (unsigned i = 0; i < count; i++) out->sample[i].key = 1;
    }
    return 0;
}

static int parse_stbl(const unsigned char *p, size_t len, struct mp4 *out) {
    struct tables t;
    memset(&t, 0, sizeof(t));
    size_t pos = 0;
    struct box b;
    int have_stsd = 0;
    while (next_box(p, len, &pos, &b)) {
        switch (b.type) {
        case TYPE('s', 't', 's', 'd'): have_stsd = parse_stsd(b.body, b.len, out) == 0; break;
        case TYPE('s', 't', 't', 's'): t.stts = b.body; t.stts_len = b.len; break;
        case TYPE('s', 't', 's', 's'): t.stss = b.body; t.stss_len = b.len; break;
        case TYPE('s', 't', 's', 'c'): t.stsc = b.body; t.stsc_len = b.len; break;
        case TYPE('s', 't', 's', 'z'): t.stsz = b.body; t.stsz_len = b.len; break;
        case TYPE('c', 't', 't', 's'): t.ctts = b.body; t.ctts_len = b.len; break;
        case TYPE('s', 't', 'c', 'o'): t.stco = b.body; t.stco_len = b.len; t.co64 = 0; break;
        case TYPE('c', 'o', '6', '4'): t.stco = b.body; t.stco_len = b.len; t.co64 = 1; break;
        }
    }
    if (!have_stsd) return -1;
    return build_samples(&t, out);
}

/* Descends moov/trak/mdia/minf/stbl for the first track whose handler is
   video. */
static int parse_trak(const unsigned char *p, size_t len, struct mp4 *out) {
    size_t pos = 0;
    struct box b;
    while (next_box(p, len, &pos, &b)) {
        if (b.type != TYPE('m', 'd', 'i', 'a')) continue;
        size_t mpos = 0;
        struct box m;
        int video = 0;
        const unsigned char *minf = 0;
        size_t minf_len = 0;
        unsigned timescale = 0;
        while (next_box(b.body, b.len, &mpos, &m)) {
            if (m.type == TYPE('h', 'd', 'l', 'r') && m.len >= 12)
                video = be32(m.body + 8) == TYPE('v', 'i', 'd', 'e');
            else if (m.type == TYPE('m', 'd', 'h', 'd') && m.len >= 20)
                timescale = m.body[0] == 1 ? be32(m.body + 20) : be32(m.body + 12);
            else if (m.type == TYPE('m', 'i', 'n', 'f')) { minf = m.body; minf_len = m.len; }
        }
        if (!video || !minf || !timescale) continue;
        out->timescale = timescale;
        size_t ipos = 0;
        struct box i;
        while (next_box(minf, minf_len, &ipos, &i))
            if (i.type == TYPE('s', 't', 'b', 'l'))
                return parse_stbl(i.body, i.len, out);
    }
    return -1;
}

int mp4_parse(const unsigned char *data, size_t len, struct mp4 *out) {
    memset(out, 0, sizeof(*out));
    size_t pos = 0;
    struct box b;
    while (next_box(data, len, &pos, &b)) {
        if (b.type != TYPE('m', 'o', 'o', 'v')) continue;
        size_t tpos = 0;
        struct box t;
        while (next_box(b.body, b.len, &tpos, &t))
            if (t.type == TYPE('t', 'r', 'a', 'k') && parse_trak(t.body, t.len, out) == 0) {
                /* Every sample must lie inside the file we were handed. */
                for (int i = 0; i < out->count; i++)
                    if ((size_t)out->sample[i].offset + out->sample[i].size > len) return -1;
                return 0;
            }
    }
    return -1;
}
