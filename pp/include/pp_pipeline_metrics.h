#ifndef PP_PIPELINE_METRICS_H
#define PP_PIPELINE_METRICS_H

#include <stdint.h>

typedef struct pp_pipeline_metrics {
    uint64_t frames_decoded;
    uint64_t frames_converted;
    uint64_t frames_presented;
    uint64_t frames_dropped;
    uint64_t decode_us_total;
    uint64_t convert_us_total;
    uint64_t present_us_total;
    uint64_t late_frames;
    uint64_t max_frame_us;
} pp_pipeline_metrics;

static inline void pp_metrics_note_frame(pp_pipeline_metrics *m, uint64_t frame_us)
{
    if (!m)
        return;
    if (frame_us > m->max_frame_us)
        m->max_frame_us = frame_us;
    if (frame_us > 16670ull) /* ~60 Hz budget */
        m->late_frames++;
}

#endif
