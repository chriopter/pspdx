/*
 * PNG in, texture out. libpng does the decoding; the work here is getting the
 * result into the shape the GE insists on: power-of-two dimensions, RGBA in
 * that byte order, 16-byte aligned, and out of the CPU's cache.
 */

#include <pspkernel.h>
#include <malloc.h>
#include <png.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "gui/image.h"
#include "util/runtime.h"

#define MAX_DIM 512

struct source {
    const unsigned char *data;
    size_t len, pos;
};

static void read_from_memory(png_structp png, png_bytep out, png_size_t need) {
    struct source *src = png_get_io_ptr(png);
    if (src->pos + need > src->len) {
        png_error(png, "truncated");
        return;
    }
    memcpy(out, src->data + src->pos, need);
    src->pos += need;
}

static void on_error(png_structp png, png_const_charp msg) {
    logline("png: %s", msg);
    longjmp(png_jmpbuf(png), 1);
}

static void on_warning(png_structp png, png_const_charp msg) {
    (void)png;
    (void)msg;
}

static unsigned next_pow2(unsigned v) {
    unsigned p = 1;
    while (p < v) p <<= 1;
    return p;
}

int image_decode_png(const void *data, size_t len, struct gfx_texture *out) {
    struct source src = { data, len, 0 };
    png_bytep *rows = 0;

    memset(out, 0, sizeof(*out));
    if (len < 8 || png_sig_cmp((png_const_bytep)data, 0, 8)) {
        logline("png: not a png");
        return -1;
    }

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, 0,
                                             on_error, on_warning);
    if (!png) return -1;
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, 0, 0);
        return -1;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, 0);
        free(rows);
        gfx_texture_free(out);
        return -1;
    }

    png_set_read_fn(png, &src, read_from_memory);
    png_read_info(png, info);

    png_uint_32 w = png_get_image_width(png, info);
    png_uint_32 h = png_get_image_height(png, info);
    if (w > MAX_DIM || h > MAX_DIM || w == 0 || h == 0) {
        logline("png: %ux%u does not fit a %dx%d texture", (unsigned)w,
                (unsigned)h, MAX_DIM, MAX_DIM);
        png_destroy_read_struct(&png, &info, 0);
        return -1;
    }

    /* Whatever the file says, hand back 8-bit RGBA: palettes expanded, low
       bit depths widened, 16-bit narrowed, and an opaque alpha added when
       the image has none. */
    int type = png_get_color_type(png, info);
    int depth = png_get_bit_depth(png, info);
    if (type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (type == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (type == PNG_COLOR_TYPE_GRAY || type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    int alpha = (type & PNG_COLOR_MASK_ALPHA) ||
                png_get_valid(png, info, PNG_INFO_tRNS);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (depth == 16) png_set_strip_16(png);
    if (!alpha) png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);

    if (png_get_channels(png, info) != 4) {
        logline("png: %d channels after expansion, expected 4",
                png_get_channels(png, info));
        png_destroy_read_struct(&png, &info, 0);
        return -1;
    }

    out->w = (int)w;
    out->h = (int)h;
    out->tw = (int)next_pow2(w);
    out->th = (int)next_pow2(h);
    /* The GE reads this behind the cache, so it has to be aligned and later
       written back by hand. */
    out->pixels = memalign(16, (size_t)out->tw * out->th * 4);
    if (!out->pixels) {
        logline("png: no room for a %dx%d texture", out->tw, out->th);
        png_destroy_read_struct(&png, &info, 0);
        return -1;
    }
    memset(out->pixels, 0, (size_t)out->tw * out->th * 4);

    rows = malloc(sizeof(png_bytep) * h);
    if (!rows) {
        png_destroy_read_struct(&png, &info, 0);
        gfx_texture_free(out);
        return -1;
    }
    for (png_uint_32 y = 0; y < h; y++)
        rows[y] = (png_bytep)out->pixels + (size_t)y * out->tw * 4;

    png_read_image(png, rows);
    png_read_end(png, 0);
    png_destroy_read_struct(&png, &info, 0);
    free(rows);

    sceKernelDcacheWritebackRange(out->pixels, (unsigned)out->tw * out->th * 4);
    logline("png: %dx%d in a %dx%d texture, %d KB", out->w, out->h, out->tw,
            out->th, out->tw * out->th * 4 / 1024);
    return 0;
}
