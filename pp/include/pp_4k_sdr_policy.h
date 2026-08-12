/*
 * Product 4K SDR policy — progressive G–H stages after isolation A–F PASS.
 *
 * Isolation (2026-07-21): ISOLATION_PASS through 014 (silent V8).
 * Product crash: after 006 VO OK / 007 with audio; no first-frame BC.
 *
 * PP_4K_PRODUCT_STAGE (see pp_4k_product_stage.h) controls re-enable.
 * PP_4K_V8_PRODUCT_ENABLE follows stage >= G_V8 unless forced.
 */
#ifndef PP_4K_SDR_POLICY_H
#define PP_4K_SDR_POLICY_H

#include "pp_4k_product_stage.h"

#define PP_4K_SDR_ENABLED 1
#define PP_4K_SDR_MAX_FPS 30

/*
 * Native 4K VideoOut (3840x2160). Re-enabled after RC1 tile formula restore
 * (integer ceil had stacked 1080 UI). Soft@1080 for gated 4K was choppy
 * (full 4K decode + scale). Override: -DPP_4K_NATIVE_VO=0
 */
#ifndef PP_4K_NATIVE_VO
#define PP_4K_NATIVE_VO 1
#endif

/* Product convert backend identifiers */
#define PP_4K_BACKEND_V3_FALLBACK 3
#define PP_4K_BACKEND_V8_FUSED    8

/*
 * V8 product path active only at stage G_V8+.
 * Override: -DPP_4K_V8_PRODUCT_ENABLE=0/1
 */
#ifndef PP_4K_V8_PRODUCT_ENABLE
#define PP_4K_V8_PRODUCT_ENABLE (PP_4K_STAGE_WANTS_V8 ? 1 : 0)
#endif

#ifndef PP_4K_SDR_BACKEND
#if PP_4K_V8_PRODUCT_ENABLE
#define PP_4K_SDR_BACKEND PP_4K_BACKEND_V8_FUSED
#else
#define PP_4K_SDR_BACKEND PP_4K_BACKEND_V3_FALLBACK
#endif
#endif

#define PP_4K_SDR_VARIANT PP_4K_SDR_BACKEND
#define PP_4K_SDR_VO_BUFFERS 3
#define PP_4K_SDR_MAX_QUEUE_DEPTH 2

#define PP_4K_SDR_PROBE_ELF "PP_GPU_PROBE_PP_4K_SDR_AV_V8.elf"
#define PP_4K_SDR_PROBE_SHA256 \
    "2f340fde386b57eaf9875dc80614e00fb05e691b167ce58606c6fa7c76d61e66"

/*
 * Advertise only after PROSPEROPLAYER_V8_APP_PASS (stage 4 + matrix).
 */

#define PP_OUTPUT_POLICY_1080 0
#define PP_OUTPUT_POLICY_4K_SDR 1
#define PP_OUTPUT_POLICY_HDR_RESEARCH_ONLY 2

#endif
