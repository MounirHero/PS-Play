/*
 * CPU tiling helper — shares PS5_tilemap defined by the player (main.c includes
 * SDL_ps5tilemap.inc once). Do not include the .inc here (duplicate symbol).
 */
#include "pp_platform.h"

#include <pthread.h>

#ifndef PS5_TILE_WIDTH
#define PS5_TILE_WIDTH  512
#define PS5_TILE_HEIGHT 128
#define PS5_TILE_SIZE   (PS5_TILE_WIDTH * PS5_TILE_HEIGHT)
#endif

extern unsigned short PS5_tilemap[PS5_TILE_HEIGHT][PS5_TILE_WIDTH];

#define PP_TILE_THREADS 12

typedef struct {
    uint32_t *src;
    uint32_t *dst;
    uint16_t frame_width;
    uint16_t frame_height;
    size_t src_start;
    size_t src_end;
} PP_DrawChunk;

static void *pp_tile_thread(void *arg)
{
    const PP_DrawChunk *chunk = (const PP_DrawChunk *)arg;
    /*
     * MUST match RC1 / working 1080 path:
     *   t = TILE_SIZE * (tx + ty * (frame_width / 512.0))
     * Integer ceil(width/512) was tried for 4K and breaks 1080 UI into a
     * staircase (see TV photo IMG_8304) — do not use it here.
     */
    for (int ind = (int)chunk->src_start; ind < (int)chunk->src_end; ind++) {
        int x = ind % chunk->frame_width;
        int y = ind / chunk->frame_width;
        int ty = y / PS5_TILE_HEIGHT;
        int tx = x / PS5_TILE_WIDTH;
        int t = (int)(PS5_TILE_SIZE *
                      (tx + ty * ((double)chunk->frame_width / PS5_TILE_WIDTH)));
        int i = PS5_tilemap[y % PS5_TILE_HEIGHT][x % PS5_TILE_WIDTH];
        chunk->dst[t + i] = chunk->src[ind];
    }
    return NULL;
}

void pp_draw_pixels_as_tiles(uint32_t *src, uint32_t *dst, int frame_width,
                             int frame_height)
{
    PP_DrawChunk chunks[PP_TILE_THREADS];
    pthread_t threads[PP_TILE_THREADS];
    size_t total = (size_t)frame_width * (size_t)frame_height;
    size_t chunk_size = total / PP_TILE_THREADS;
    int i;

    for (i = 0; i < PP_TILE_THREADS; i++) {
        chunks[i].src = src;
        chunks[i].dst = dst;
        chunks[i].frame_width = (uint16_t)frame_width;
        chunks[i].frame_height = (uint16_t)frame_height;
        chunks[i].src_start = (size_t)i * chunk_size;
        chunks[i].src_end = (i == PP_TILE_THREADS - 1)
                                ? total
                                : (size_t)(i + 1) * chunk_size;
        pthread_create(&threads[i], 0, pp_tile_thread, &chunks[i]);
    }
    for (i = 0; i < PP_TILE_THREADS; i++)
        pthread_join(threads[i], 0);
}
