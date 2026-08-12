/*
 * CPU converter: YUV420P / NV12 / RGBA / BGRA → BGRA32 linear display buffer.
 */
#include "pp_converter.h"

#include <string.h>

static uint8_t clamp_u8(int v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

/*
 * BT.601 → display dword matching known-good test_pattern.c / Probe 002:
 *   COL_R = 0x000000FF  (R in bits 0-7)
 *   COL_G = 0x0000FF00
 *   COL_B = 0x00FF0000
 * With opaque alpha in bits 24-31 (same as pp_videoout_pattern solids).
 */
static uint32_t yuv_to_bgra(int y, int u, int v)
{
    int c = y - 16;
    int d = u - 128;
    int e = v - 128;
    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;
    return ((uint32_t)255u << 24) |
           ((uint32_t)clamp_u8(b) << 16) |
           ((uint32_t)clamp_u8(g) << 8) |
           ((uint32_t)clamp_u8(r));
}

static void clear_rect(uint32_t *dst, uint32_t pitch_px, uint32_t x0, uint32_t y0,
                       uint32_t x1, uint32_t y1, uint32_t color)
{
    uint32_t y, x;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++)
            dst[y * pitch_px + x] = color;
}

static void compute_dest_rect(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh,
                              pp_aspect_mode mode,
                              uint32_t *ox, uint32_t *oy, uint32_t *ow, uint32_t *oh,
                              uint32_t *sx0, uint32_t *sy0, uint32_t *sw_use, uint32_t *sh_use)
{
    *sx0 = 0;
    *sy0 = 0;
    *sw_use = sw;
    *sh_use = sh;

    if (mode == PP_ASPECT_STRETCH) {
        *ox = 0;
        *oy = 0;
        *ow = dw;
        *oh = dh;
        return;
    }

    /* FIT: entire source visible, letterbox */
    /* FILL: cover dest, crop source */
    double src_ar = (double)sw / (double)sh;
    double dst_ar = (double)dw / (double)dh;

    if (mode == PP_ASPECT_FIT) {
        if (src_ar > dst_ar) {
            *ow = dw;
            *oh = (uint32_t)((double)dw / src_ar + 0.5);
            if (*oh > dh)
                *oh = dh;
            *ox = 0;
            *oy = (dh - *oh) / 2u;
        } else {
            *oh = dh;
            *ow = (uint32_t)((double)dh * src_ar + 0.5);
            if (*ow > dw)
                *ow = dw;
            *oy = 0;
            *ox = (dw - *ow) / 2u;
        }
        return;
    }

    /* FILL: crop source so scaled rect covers dest fully */
    *ox = 0;
    *oy = 0;
    *ow = dw;
    *oh = dh;
    if (src_ar > dst_ar) {
        /* source wider — crop sides */
        *sw_use = (uint32_t)((double)sh * dst_ar + 0.5);
        if (*sw_use > sw)
            *sw_use = sw;
        *sx0 = (sw - *sw_use) / 2u;
        *sh_use = sh;
        *sy0 = 0;
    } else {
        *sh_use = (uint32_t)((double)sw / dst_ar + 0.5);
        if (*sh_use > sh)
            *sh_use = sh;
        *sy0 = (sh - *sh_use) / 2u;
        *sw_use = sw;
        *sx0 = 0;
    }
}

static void sample_yuv420p(const pp_frame *s, uint32_t sx, uint32_t sy,
                           int *y, int *u, int *v)
{
    uint32_t uvx = sx / 2u;
    uint32_t uvy = sy / 2u;
    *y = s->planes[0][sy * (uint32_t)s->strides[0] + sx];
    *u = s->planes[1][uvy * (uint32_t)s->strides[1] + uvx];
    *v = s->planes[2][uvy * (uint32_t)s->strides[2] + uvx];
}

static void sample_nv12(const pp_frame *s, uint32_t sx, uint32_t sy,
                        int *y, int *u, int *v)
{
    uint32_t uvx = (sx / 2u) * 2u;
    uint32_t uvy = sy / 2u;
    const uint8_t *uv = s->planes[1] + uvy * (uint32_t)s->strides[1] + uvx;
    *y = s->planes[0][sy * (uint32_t)s->strides[0] + sx];
    *u = uv[0];
    *v = uv[1];
}

int pp_converter_convert_ex(const pp_frame *source,
                            void *destination,
                            uint32_t destination_width,
                            uint32_t destination_height,
                            uint32_t destination_pitch,
                            const pp_converter_config *cfg)
{
    uint32_t *dst;
    uint32_t pitch_px;
    uint32_t ox, oy, ow, oh, sx0, sy0, sw_use, sh_use;
    uint32_t dy, dx;
    pp_aspect_mode aspect;
    uint32_t clear;

    if (!source || !destination || !source->width || !source->height)
        return -1;
    if (!destination_width || !destination_height || destination_pitch < destination_width * 4u)
        return -2;
    if (!source->planes[0])
        return -3;

    aspect = cfg ? cfg->aspect : PP_ASPECT_FIT;
    clear = cfg ? cfg->clear_color_bgra : 0xFF000000u;

    dst = (uint32_t *)destination;
    pitch_px = destination_pitch / 4u;

    /* Clear full dest (letterbox). */
    clear_rect(dst, pitch_px, 0, 0, destination_width, destination_height, clear);

    compute_dest_rect(source->width, source->height, destination_width, destination_height,
                      aspect, &ox, &oy, &ow, &oh, &sx0, &sy0, &sw_use, &sh_use);
    if (ow == 0 || oh == 0)
        return -4;

    for (dy = 0; dy < oh; dy++) {
        uint32_t sy = sy0 + (dy * sh_use) / oh;
        if (sy >= source->height)
            sy = source->height - 1;
        for (dx = 0; dx < ow; dx++) {
            uint32_t sx = sx0 + (dx * sw_use) / ow;
            uint32_t out;
            if (sx >= source->width)
                sx = source->width - 1;

            if (source->format == PP_FRAME_YUV420P) {
                int y, u, v;
                if (!source->planes[1] || !source->planes[2])
                    return -5;
                sample_yuv420p(source, sx, sy, &y, &u, &v);
                out = yuv_to_bgra(y, u, v);
            } else if (source->format == PP_FRAME_NV12) {
                int y, u, v;
                if (!source->planes[1])
                    return -5;
                sample_nv12(source, sx, sy, &y, &u, &v);
                out = yuv_to_bgra(y, u, v);
            } else if (source->format == PP_FRAME_BGRA) {
                const uint32_t *row =
                    (const uint32_t *)(source->planes[0] + sy * (uint32_t)source->strides[0]);
                out = row[sx];
            } else if (source->format == PP_FRAME_RGBA) {
                const uint8_t *p =
                    source->planes[0] + sy * (uint32_t)source->strides[0] + sx * 4u;
                /* byte RGBA → dword with R in low byte (match Probe 002) */
                out = ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
                      ((uint32_t)p[1] << 8) | (uint32_t)p[0];
            } else {
                return -6;
            }
            dst[(oy + dy) * pitch_px + (ox + dx)] = out;
        }
    }
    return 0;
}

int pp_converter_convert(const pp_frame *source,
                         void *destination,
                         uint32_t destination_width,
                         uint32_t destination_height,
                         uint32_t destination_pitch)
{
    pp_converter_config cfg;
    cfg.aspect = PP_ASPECT_FIT;
    cfg.clear_color_bgra = 0xFF000000u;
    return pp_converter_convert_ex(source, destination, destination_width,
                                   destination_height, destination_pitch, &cfg);
}
