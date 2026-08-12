#include "pp_converter_fused.h"

#include <pthread.h>
#include <string.h>

/* Same constants as reference/SDL_ps5tilemap.inc — data lives in tile_copy.c only */
#define PS5_TILE_WIDTH 512
#define PS5_TILE_HEIGHT 128
#define PS5_TILE_SIZE (PS5_TILE_WIDTH * PS5_TILE_HEIGHT)
extern unsigned short PS5_tilemap[PS5_TILE_HEIGHT][PS5_TILE_WIDTH];

typedef struct {
    const pp_frame *src;
    uint32_t *dst;
    uint32_t frame_w;
    uint32_t y0, y1;
} fused_band_job;

size_t pp_tiled_pixel_offset(int x, int y, int frame_width)
{
    int ty = y / PS5_TILE_HEIGHT;
    int tx = x / PS5_TILE_WIDTH;
    int t = (int)(PS5_TILE_SIZE *
                  (tx + ty * ((double)frame_width / (double)PS5_TILE_WIDTH)));
    int i = (int)PS5_tilemap[y % PS5_TILE_HEIGHT][x % PS5_TILE_WIDTH];
    return (size_t)(t + i);
}

static uint32_t yuv_to_bgra_r_low(int y, int u, int v)
{
    int c = y - 16, d = u - 128, e = v - 128;
    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;
    if (r < 0)
        r = 0;
    if (r > 255)
        r = 255;
    if (g < 0)
        g = 0;
    if (g > 255)
        g = 255;
    if (b < 0)
        b = 0;
    if (b > 255)
        b = 255;
    return 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

static void *fused_band_worker(void *arg)
{
    fused_band_job *j = (fused_band_job *)arg;
    const pp_frame *s = j->src;
    uint32_t y, x;
    uint32_t w = s->width;
    uint32_t fw = j->frame_w;

    /* Process 2×2 blocks: load U/V once per block */
    for (y = j->y0; y < j->y1; y += 2u) {
        const uint8_t *Y0 = s->planes[0] + y * (uint32_t)s->strides[0];
        const uint8_t *Y1 =
            (y + 1u < s->height) ? s->planes[0] + (y + 1u) * (uint32_t)s->strides[0] : Y0;
        const uint8_t *U = s->planes[1] + (y / 2u) * (uint32_t)s->strides[1];
        const uint8_t *V = s->planes[2] + (y / 2u) * (uint32_t)s->strides[2];

        for (x = 0; x < w; x += 2u) {
            int uu = U[x / 2u];
            int vv = V[x / 2u];
            uint32_t p00 = yuv_to_bgra_r_low(Y0[x], uu, vv);
            j->dst[pp_tiled_pixel_offset((int)x, (int)y, (int)fw)] = p00;

            if (x + 1u < w) {
                uint32_t p10 = yuv_to_bgra_r_low(Y0[x + 1u], uu, vv);
                j->dst[pp_tiled_pixel_offset((int)(x + 1u), (int)y, (int)fw)] = p10;
            }
            if (y + 1u < j->y1 && y + 1u < s->height) {
                uint32_t p01 = yuv_to_bgra_r_low(Y1[x], uu, vv);
                j->dst[pp_tiled_pixel_offset((int)x, (int)(y + 1u), (int)fw)] = p01;
                if (x + 1u < w) {
                    uint32_t p11 = yuv_to_bgra_r_low(Y1[x + 1u], uu, vv);
                    j->dst[pp_tiled_pixel_offset((int)(x + 1u), (int)(y + 1u), (int)fw)] = p11;
                }
            }
        }
    }
    return NULL;
}

int pp_converter_yuv420p_to_tiled_bgra_parallel(const pp_frame *source,
                                                uint32_t *tiled_destination,
                                                uint32_t frame_width,
                                                uint32_t frame_height,
                                                int workers)
{
    pthread_t th[8];
    fused_band_job jobs[8];
    int i, n = workers;
    uint32_t band;

    if (!source || !tiled_destination || !source->planes[0] || !source->planes[1] ||
        !source->planes[2])
        return -1;
    if (source->format != PP_FRAME_YUV420P)
        return -2;
    if (source->width != frame_width || source->height != frame_height)
        return -3;
    if (n < 1)
        n = 1;
    if (n > 8)
        n = 8;

    band = (frame_height + (uint32_t)n - 1u) / (uint32_t)n;
    /* even rows for 420 + 2×2 block processing */
    if (band & 1u)
        band++;

    for (i = 0; i < n; i++) {
        jobs[i].src = source;
        jobs[i].dst = tiled_destination;
        jobs[i].frame_w = frame_width;
        jobs[i].y0 = (uint32_t)i * band;
        jobs[i].y1 = jobs[i].y0 + band;
        if (jobs[i].y1 > frame_height)
            jobs[i].y1 = frame_height;
        if (jobs[i].y0 >= frame_height)
            jobs[i].y0 = jobs[i].y1 = frame_height;
        pthread_create(&th[i], NULL, fused_band_worker, &jobs[i]);
    }
    for (i = 0; i < n; i++)
        pthread_join(th[i], NULL);
    return 0;
}
