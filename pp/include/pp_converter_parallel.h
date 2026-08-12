/*
 * 4K30 winning convert path (004K V3): multi-thread YUV420P → BGRA 1:1.
 * Prefer for source >= 2560 wide; keep pp_converter_convert for 1080p.
 */
#ifndef PP_CONVERTER_PARALLEL_H
#define PP_CONVERTER_PARALLEL_H

#include "pp_frame.h"
#include "pp_converter.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 1:1 YUV420P → BGRA (R in low byte). workers typically 4. */
int pp_converter_yuv420p_to_bgra_parallel(const pp_frame *source,
                                          void *destination,
                                          uint32_t destination_pitch_bytes,
                                          int workers);

/**
 * Product helper: pick convert strategy.
 * - Soft UHD → 1080: parallel NN scale honoring aspect (FIT/FILL/STRETCH)
 * - 1:1 large YUV420P → parallel convert
 * - Else → pp_converter_convert_ex with aspect
 */
int pp_converter_to_display(const pp_frame *source,
                            void *destination,
                            uint32_t destination_width,
                            uint32_t destination_height,
                            uint32_t destination_pitch_bytes,
                            pp_aspect_mode aspect);

#ifdef __cplusplus
}
#endif

#endif
