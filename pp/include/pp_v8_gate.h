/*
 * V8 / native-4K source gate — 8-bit SDR yuv420p UHD ≤30.
 *
 * Accepts full 3840x2160 and common cinema UHD (e.g. 3840x1920) which was
 * previously forced onto the slow soft@1080 scale path (choppy).
 * Never feed 10-bit / HDR / NV12 into the fused 8-bit converter.
 */
#ifndef PP_V8_GATE_H
#define PP_V8_GATE_H

#include "pp_frame.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum pp_v8_gate_result {
    PP_V8_GATE_OK = 0,
    PP_V8_SOURCE_UNSUPPORTED = 1
} pp_v8_gate_result;

typedef struct pp_v8_source_info {
    uint32_t width;
    uint32_t height;
    double fps;
    pp_frame_format format; /* must be PP_FRAME_YUV420P */
    int bits_per_component; /* must be 8 */
    int is_pq;              /* transfer PQ */
    int is_hlg;             /* transfer HLG */
    int has_hdr_metadata;
    int has_dolby_vision;
} pp_v8_source_info;

/**
 * True for product "UHD soft/native" sizes: at least ~3K wide cinema/full UHD.
 * Height 1600–2160 covers 3840x1920 (2.0:1) and 3840x2160 (16:9).
 */
static inline int pp_v8_is_uhd_size(uint32_t w, uint32_t h)
{
    if (w < 3200u || w > 3840u)
        return 0;
    if (h < 1600u || h > 2160u)
        return 0;
    /* Must be even for 4:2:0 and tile math */
    if ((w & 1u) || (h & 1u))
        return 0;
    return 1;
}

/**
 * Returns PP_V8_GATE_OK when all of:
 *   UHD size, fps<=30, YUV420P, 8-bit, not PQ/HLG, no HDR/DV metadata.
 */
static inline pp_v8_gate_result pp_v8_source_gate(const pp_v8_source_info *s)
{
    if (!s)
        return PP_V8_SOURCE_UNSUPPORTED;
    if (!pp_v8_is_uhd_size(s->width, s->height))
        return PP_V8_SOURCE_UNSUPPORTED;
    if (s->fps <= 0.0 || s->fps > 30.01)
        return PP_V8_SOURCE_UNSUPPORTED;
    if (s->format != PP_FRAME_YUV420P)
        return PP_V8_SOURCE_UNSUPPORTED;
    if (s->bits_per_component != 8)
        return PP_V8_SOURCE_UNSUPPORTED;
    if (s->is_pq || s->is_hlg)
        return PP_V8_SOURCE_UNSUPPORTED;
    if (s->has_hdr_metadata || s->has_dolby_vision)
        return PP_V8_SOURCE_UNSUPPORTED;
    return PP_V8_GATE_OK;
}

/** Gate a live pp_frame (runtime decode). */
static inline pp_v8_gate_result pp_v8_frame_gate(const pp_frame *f, double fps)
{
    pp_v8_source_info s;
    if (!f)
        return PP_V8_SOURCE_UNSUPPORTED;
    s.width = f->width;
    s.height = f->height;
    s.fps = fps > 0.0 ? fps : 30.0;
    s.format = f->format;
    s.bits_per_component = 8; /* pp_frame has no bpp; caller must not map 10-bit */
    s.is_pq = 0;
    s.is_hlg = 0;
    s.has_hdr_metadata = 0;
    s.has_dolby_vision = 0;
    return pp_v8_source_gate(&s);
}

#ifdef __cplusplus
}
#endif

#endif /* PP_V8_GATE_H */
