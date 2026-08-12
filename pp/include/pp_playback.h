/*
 * pp_playback — product session façade over converter + clock.
 * VideoOut is owned by the app (main); session publishes display frames
 * or (V8) prepares pre-tiled VO presents.
 * No FFmpeg types in this header.
 */
#ifndef PP_PLAYBACK_H
#define PP_PLAYBACK_H

#include "pp_clock.h"
#include "pp_converter.h"
#include "pp_frame.h"
#include "pp_output_policy.h"
#include "pp_product_path.h"
#include "pp_videoout.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pp_playback_stats {
    uint64_t frames_in;
    uint64_t frames_converted;
    uint64_t frames_published;
    uint64_t frames_late_dropped;
    uint64_t frames_discarded_seek;
    uint64_t convert_us_total;
    uint64_t convert_us_max;
    uint32_t convert_ring_count;
    uint64_t convert_ring[256];
    uint64_t seek_requests;
    uint64_t seek_successes;
    uint64_t seek_failures;
    uint64_t clock_resets;
    uint64_t seek_to_first_frame_ms;
    uint32_t sample_bgra;
    int output_w;
    int output_h;
    int aspect;
    int backend; /* pp_video_backend */
} pp_playback_stats;

typedef struct pp_playback {
    pp_videoout *vo; /* non-owning; may be NULL if only convert/clock used */
    pp_clock clock;
    pp_converter_config cfg;

    uint32_t out_w;
    uint32_t out_h;
    uint32_t *display;      /* linear BGRA front (published) */
    uint32_t *display_back; /* convert target; swapped under lock */
    int display_ready;
    int64_t display_pts_us;

    int active;
    int seek_discarding;
    int64_t seek_target_us;
    uint64_t seek_begin_ms;

    /* Product path */
    pp_video_backend backend;
    int force_v3_fallback; /* runtime emergency switch */

    /* V8: convert wrote tiled GPU plane; main should present_pre_tiled */
    int pending_present;
    uint32_t pending_vo_idx;
    uint64_t pending_frame_id;
    uint64_t present_seq;

    void *lock;
    pp_playback_stats stats;
} pp_playback;

void pp_playback_init(pp_playback *pb);
void pp_playback_shutdown(pp_playback *pb);

void pp_playback_attach_videoout(pp_playback *pb, pp_videoout *vo);

int pp_playback_set_output(pp_playback *pb, uint32_t w, uint32_t h,
                           pp_aspect_mode aspect);

/** Set product backend (1080 standard / 4K V8 / 4K V3 fallback). */
void pp_playback_set_backend(pp_playback *pb, pp_video_backend backend);

/** Runtime emergency: force V3 fallback for 4K. */
void pp_playback_force_v3_fallback(pp_playback *pb, int enable);

void pp_playback_on_file_open(pp_playback *pb);
void pp_playback_on_file_close(pp_playback *pb);

void pp_playback_pause(pp_playback *pb);
void pp_playback_resume(pp_playback *pb);

/**
 * Convert + pace.
 * V8 4K with VO: fused write to GPU plane, sets pending present.
 * Else: linear display buffer (V3 / 1080).
 * Returns 0 published, 1 late-drop/discard, <0 error.
 */
int pp_playback_push_frame(pp_playback *pb, const pp_frame *src);

/**
 * If V8 left a pre-tiled present ready, fill *idx / *frame_id and clear pending.
 * Returns 1 if caller should pp_videoout_present_pre_tiled.
 */
int pp_playback_take_pending_present(pp_playback *pb, uint32_t *idx, uint64_t *frame_id);

int pp_playback_copy_display(pp_playback *pb, uint32_t *dst, uint32_t pitch_bytes,
                             uint32_t dst_w, uint32_t dst_h);

int pp_playback_has_display(const pp_playback *pb);

void pp_playback_notify_seek_begin(pp_playback *pb, int64_t target_pts_us);
void pp_playback_notify_seek_end(pp_playback *pb, int success,
                                 uint64_t discarded, uint64_t elapsed_ms);

void pp_playback_get_stats(const pp_playback *pb, pp_playback_stats *out);
uint64_t pp_playback_convert_p95_us(const pp_playback *pb);
void pp_playback_write_stats_file(const pp_playback *pb, const char *path);

int pp_playback_choose_output_mode(const pp_source_caps *src,
                                   uint32_t display_max_w,
                                   uint32_t display_max_h);

#ifdef __cplusplus
}
#endif

#endif /* PP_PLAYBACK_H */
