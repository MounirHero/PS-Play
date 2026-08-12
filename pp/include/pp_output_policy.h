/*
 * Output mode selection for ProsperoPlayer product path.
 * After FOUR_K_SDR_PLAYBACK_PASS — honest advertising only for 4K SDR ≤30 FPS.
 */
#ifndef PP_OUTPUT_POLICY_H
#define PP_OUTPUT_POLICY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum pp_output_mode {
    PP_OUT_1080P_SDR = 0,     /* production default */
    PP_OUT_4K_SDR = 1,        /* native 4K SDR when source is 4K SDR */
    PP_OUT_HDR_RESEARCH = 2   /* never claim product HDR on BGRA path */
} pp_output_mode;

typedef struct pp_source_caps {
    uint32_t width;
    uint32_t height;
    int is_hdr_trc;   /* PQ/HLG */
    int is_10bit;
    double fps;
} pp_source_caps;

/**
 * Select output mode from source + display capability.
 * display_max_w/h: reported display (e.g. 3840x2160 if 4K mode available).
 */
static inline pp_output_mode pp_select_output_mode(const pp_source_caps *src,
                                                   uint32_t display_max_w,
                                                   uint32_t display_max_h)
{
    if (!src)
        return PP_OUT_1080P_SDR;
    /* HDR sources: do not auto-select "HDR product" on current BGRA stack */
    if (src->is_hdr_trc || src->is_10bit)
        return PP_OUT_1080P_SDR; /* or explicit SDR fallback later with tonemap */
    if (src->width >= 3840u && src->height >= 2160u && display_max_w >= 3840u &&
        display_max_h >= 2160u && src->fps > 0.0 && src->fps <= 30.01)
        return PP_OUT_4K_SDR;
    return PP_OUT_1080P_SDR;
}

static inline void pp_output_dims(pp_output_mode m, uint32_t *w, uint32_t *h)
{
    if (m == PP_OUT_4K_SDR) {
        *w = 3840;
        *h = 2160;
    } else {
        *w = 1920;
        *h = 1080;
    }
}

#ifdef __cplusplus
}
#endif

#endif
