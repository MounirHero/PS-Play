/*
 * V8: YUV420P → BGRA written directly in PS5 tiled VideoOut layout.
 * Same color math as pp_converter_parallel; same tilemap as tile_copy.c.
 */
#ifndef PP_CONVERTER_FUSED_H
#define PP_CONVERTER_FUSED_H

#include "pp_frame.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Parallel YUV420P → tiled BGRA (R in low byte), 1:1, no scale.
 * destination is the registered VideoOut plane (tiled).
 * workers typically 4 (match V3/V6B).
 */
int pp_converter_yuv420p_to_tiled_bgra_parallel(const pp_frame *source,
                                                uint32_t *tiled_destination,
                                                uint32_t frame_width,
                                                uint32_t frame_height,
                                                int workers);

/**
 * Linear pixel (x,y) → offset in tiled plane (pixels).
 * Identical formula to pp_draw_pixels_as_tiles.
 */
size_t pp_tiled_pixel_offset(int x, int y, int frame_width);

#ifdef __cplusplus
}
#endif

#endif
