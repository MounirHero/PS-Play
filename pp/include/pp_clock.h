/*
 * pp_clock — host-time media clock for silent PTS pacing.
 * Video master. Late frames can be dropped.
 * Supports pause/resume (anchor shift) and reset (seek/stop).
 */
#ifndef PP_CLOCK_H
#define PP_CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pp_clock_stats {
    uint64_t wait_count;
    uint64_t wait_us_total;
    uint64_t late_drops;
    uint64_t on_time_presents;
    uint64_t early_sleeps;
    uint64_t pause_count;
    uint64_t resume_count;
    uint64_t reset_count;
    int64_t last_lag_us;   /* host_media - pts; >0 = late */
    int64_t max_lag_us;
    int64_t min_lag_us;
    int64_t sum_lag_us;
    uint64_t lag_samples;
} pp_clock_stats;

typedef struct pp_clock {
    int started;
    int paused;
    uint64_t host_start_us;
    uint64_t pause_host_us;      /* host time when pause began */
    int64_t media_at_pause_us;   /* frozen media time while paused */
    int64_t media_start_pts_us;
    int64_t max_late_us;
    int64_t max_early_us;
    pp_clock_stats stats;
} pp_clock;

#define PP_CLOCK_PRESENT 0
#define PP_CLOCK_DROP    1

void pp_clock_init(pp_clock *c, int64_t max_late_us, int64_t max_early_us);
void pp_clock_start(pp_clock *c, int64_t first_pts_us);
/** Re-sync host timeline to a media PTS (soft-decode catch-up). */
void pp_clock_reanchor(pp_clock *c, int64_t pts_us);
int64_t pp_clock_media_us(const pp_clock *c);
int pp_clock_wait_or_drop(pp_clock *c, int64_t pts_us);

/** Freeze media timeline. Safe if already paused. */
void pp_clock_pause(pp_clock *c);

/**
 * Resume: shift host_start_us by paused duration so PTS lag is unchanged
 * across the pause gap (no mass late-drops).
 */
void pp_clock_resume(pp_clock *c);

/** Full reset for stop/seek — not started; keeps max_late/max_early. */
void pp_clock_reset(pp_clock *c);

int pp_clock_is_paused(const pp_clock *c);
void pp_clock_get_stats(const pp_clock *c, pp_clock_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* PP_CLOCK_H */
