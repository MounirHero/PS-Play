/*
 * Audio-master media clock — latency-corrected played samples + lifecycle states.
 * Video presentation only while RUNNING.
 */
#ifndef PP_AUDIO_CLOCK_H
#define PP_AUDIO_CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum pp_av_clock_state {
    PP_AV_CLOCK_UNPRIMED = 0,
    PP_AV_CLOCK_PRIMING = 1,
    PP_AV_CLOCK_RUNNING = 2,
    PP_AV_CLOCK_PAUSED = 3,
    PP_AV_CLOCK_SEEKING = 4,
    PP_AV_CLOCK_EOF = 5
} pp_av_clock_state;

typedef struct pp_audio_clock_stats {
    uint64_t samples_submitted;
    uint64_t samples_queued;   /* estimate of still-in-device buffer */
    uint64_t samples_played;   /* submitted - queued (latency-corrected) */
    uint64_t underruns;
    uint64_t resets;
    uint64_t reanchors;
    uint64_t pause_count;
    uint64_t resume_count;
    int64_t last_media_us;
    int64_t max_media_us;
    int64_t device_latency_us;
} pp_audio_clock_stats;

typedef struct pp_audio_clock {
    pp_av_clock_state state;
    int sample_rate;
    int channels;
    int64_t first_pts_us; /* first *played* audio PTS after prime/seek */
    uint64_t samples_submitted;
    uint64_t samples_queued;  /* device-side residual estimate */
    uint64_t samples_played;  /* master position in samples */
    uint64_t device_latency_samples; /* fixed HW estimate (e.g. 2 blocks) */
    uint64_t pause_host_us;
    int64_t media_at_pause_us;
    uint64_t host_start_us;
    int64_t pause_shift_us; /* if HW cannot pause: total paused host time to ignore */
    /* thresholds */
    int64_t on_time_us;
    int64_t late_drop_us;
    int64_t max_early_us;
    int64_t hard_resync_us;
    uint64_t generation; /* playback generation (seek/replay bumps) */
    pp_audio_clock_stats stats;
} pp_audio_clock;

#define PP_AV_PRESENT 0
#define PP_AV_WAIT    1
#define PP_AV_DROP    2
#define PP_AV_HOLD    3 /* unprimed / paused / seeking — do not present */

void pp_audio_clock_init(pp_audio_clock *c, int sample_rate, int channels);
void pp_audio_clock_set_device_latency_samples(pp_audio_clock *c, uint64_t samples);
void pp_audio_clock_set_thresholds(pp_audio_clock *c, int64_t on_time_us, int64_t late_drop_us,
                                   int64_t max_early_us, int64_t hard_resync_us);
void pp_audio_clock_set_from_fps(pp_audio_clock *c, double fps);

pp_av_clock_state pp_audio_clock_state(const pp_audio_clock *c);
int pp_audio_clock_is_running(const pp_audio_clock *c);
uint64_t pp_audio_clock_generation(const pp_audio_clock *c);

/** Begin seek: invalidate clock, bump generation. */
void pp_audio_clock_begin_seek(pp_audio_clock *c);

/**
 * After first valid post-seek audio PTS: set anchor and enter PRIMING.
 * Call note_submitted for prime blocks, then mark_running.
 */
void pp_audio_clock_prime(pp_audio_clock *c, int64_t first_audio_pts_us);

/** Transition PRIMING → RUNNING after prime audio queued. */
void pp_audio_clock_mark_running(pp_audio_clock *c);

/**
 * Record samples just submitted to audio HW (per channel).
 * Updates samples_played = submitted - device_latency (latency-corrected).
 */
void pp_audio_clock_note_submitted(pp_audio_clock *c, uint64_t samples_per_channel);

/** Current media time from played samples (not submitted). */
int64_t pp_audio_clock_media_us(const pp_audio_clock *c);

void pp_audio_clock_pause(pp_audio_clock *c);
void pp_audio_clock_resume(pp_audio_clock *c);

/** Full invalidate (stop/replay). Keeps thresholds + sample rate. Bumps generation. */
void pp_audio_clock_reset(pp_audio_clock *c);

void pp_audio_clock_mark_eof(pp_audio_clock *c);

/**
 * Schedule video against played audio clock.
 * Only PRESENT while RUNNING; HOLD if not running.
 * delta = video_pts - audio_clock (positive ⇒ video late).
 */
int pp_audio_clock_schedule_video(pp_audio_clock *c, int64_t video_pts_us, int64_t *out_delta_us);

void pp_audio_clock_note_underrun(pp_audio_clock *c);
void pp_audio_clock_get_stats(const pp_audio_clock *c, pp_audio_clock_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* PP_AUDIO_CLOCK_H */
