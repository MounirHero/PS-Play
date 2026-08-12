/*
 * Product 4K progressive stages (G–H) after isolation A–F PASS.
 *
 * Isolation probe proved: 4K VO + decode + V8 convert + present (silent).
 * Product crash was after 4K VO + decoder path with audio/UI.
 *
 * Stage ladder (compile -DPP_4K_PRODUCT_STAGE=N):
 *   0 SAFE    — never reconfigure to 4K VO (1080 scale path)
 *   1 G_VO    — 4K VO + V3 linear convert, NO audio, NO player UI
 *   2 G_V8    — 4K VO + V8 fused present,  NO audio, NO player UI
 *   3 G_AUDIO — stage 2 + audio threads
 *   4 H_FULL  — stage 3 + full player UI overlays
 *
 * Default 4 = H_FULL after G-H ladder PASS (2026-07-21).
 * Emergency safe: -DPP_4K_PRODUCT_STAGE=0
 */
#ifndef PP_4K_PRODUCT_STAGE_H
#define PP_4K_PRODUCT_STAGE_H

#ifndef PP_4K_PRODUCT_STAGE
#define PP_4K_PRODUCT_STAGE 4
#endif

#define PP_4K_STAGE_SAFE   0
#define PP_4K_STAGE_G_VO   1
#define PP_4K_STAGE_G_V8   2
#define PP_4K_STAGE_G_AUDIO 3
#define PP_4K_STAGE_H_FULL 4

/* Convenience */
#define PP_4K_STAGE_WANTS_4K_VO   (PP_4K_PRODUCT_STAGE >= PP_4K_STAGE_G_VO)
#define PP_4K_STAGE_WANTS_V8      (PP_4K_PRODUCT_STAGE >= PP_4K_STAGE_G_V8)
#define PP_4K_STAGE_WANTS_AUDIO   (PP_4K_PRODUCT_STAGE >= PP_4K_STAGE_G_AUDIO)
#define PP_4K_STAGE_WANTS_UI      (PP_4K_PRODUCT_STAGE >= PP_4K_STAGE_H_FULL)

static inline const char *pp_4k_stage_name(int stage)
{
    switch (stage) {
    case 0: return "SAFE_1080";
    case 1: return "G_VO_V3_noA_noUI";
    case 2: return "G_V8_noA_noUI";
    case 3: return "G_V8_AUDIO_noUI";
    case 4: return "H_FULL";
    default: return "UNKNOWN";
    }
}

#endif /* PP_4K_PRODUCT_STAGE_H */
