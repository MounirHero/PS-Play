/*
 * ProsperoPlayer product video path selection — crash isolation defaults.
 */
#ifndef PP_PRODUCT_PATH_H
#define PP_PRODUCT_PATH_H

#include "pp_4k_sdr_policy.h"
#include "pp_output_policy.h"
#include "pp_v8_gate.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum pp_video_backend {
    PP_BACKEND_1080_STANDARD = 0, /* stable default: convert → linear → tile */
    PP_BACKEND_4K_V8_FUSED = 1,   /* gated 4K SDR ≤30 8-bit (probe-qualified) */
    PP_BACKEND_4K_V3_FALLBACK = 2 /* 4K via V3 parallel + tile if ever enabled */
} pp_video_backend;

/**
 * Product path selection for gated 4K SDR sources.
 * Stage 0 → 1080. Stage 1 → V3 on 4K VO. Stage 2+ → V8 if enable+gate.
 */
static inline pp_video_backend pp_select_video_backend(pp_output_mode mode, int v8_gate_ok)
{
    if (mode != PP_OUT_4K_SDR)
        return PP_BACKEND_1080_STANDARD;
#if !PP_4K_STAGE_WANTS_4K_VO
    (void)v8_gate_ok;
    return PP_BACKEND_1080_STANDARD;
#else
    if (!v8_gate_ok)
        return PP_BACKEND_1080_STANDARD;
#if PP_4K_V8_PRODUCT_ENABLE
    if (PP_4K_STAGE_WANTS_V8 && PP_4K_SDR_BACKEND == PP_4K_BACKEND_V8_FUSED)
        return PP_BACKEND_4K_V8_FUSED;
#endif
    /* G_VO: native 4K surface with V3 linear convert */
    return PP_BACKEND_4K_V3_FALLBACK;
#endif
}

#ifdef __cplusplus
}
#endif

#endif
