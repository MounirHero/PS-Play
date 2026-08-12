#include "pp_clock.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

static uint64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

void pp_clock_init(pp_clock *c, int64_t max_late_us, int64_t max_early_us)
{
    if (!c)
        return;
    memset(c, 0, sizeof(*c));
    c->max_late_us = max_late_us > 0 ? max_late_us : 80000;
    c->max_early_us = max_early_us > 0 ? max_early_us : 250000;
    c->stats.min_lag_us = INT64_MAX;
}

void pp_clock_start(pp_clock *c, int64_t first_pts_us)
{
    if (!c)
        return;
    c->host_start_us = now_us();
    c->media_start_pts_us = first_pts_us;
    c->started = 1;
    c->paused = 0;
    c->pause_host_us = 0;
    c->media_at_pause_us = 0;
}

int64_t pp_clock_media_us(const pp_clock *c)
{
    uint64_t elapsed;
    if (!c || !c->started)
        return 0;
    if (c->paused)
        return c->media_at_pause_us;
    elapsed = now_us() - c->host_start_us;
    return c->media_start_pts_us + (int64_t)elapsed;
}

static void note_lag(pp_clock *c, int64_t lag)
{
    c->stats.last_lag_us = lag;
    c->stats.sum_lag_us += lag;
    c->stats.lag_samples++;
    if (lag > c->stats.max_lag_us)
        c->stats.max_lag_us = lag;
    if (lag < c->stats.min_lag_us)
        c->stats.min_lag_us = lag;
}

void pp_clock_pause(pp_clock *c)
{
    if (!c || !c->started || c->paused)
        return;
    c->media_at_pause_us = pp_clock_media_us(c);
    c->pause_host_us = now_us();
    c->paused = 1;
    c->stats.pause_count++;
}

void pp_clock_resume(pp_clock *c)
{
    uint64_t paused_dur;
    if (!c || !c->started || !c->paused)
        return;
    paused_dur = now_us() - c->pause_host_us;
    /* Shift anchor forward so media timeline does not jump during pause. */
    c->host_start_us += paused_dur;
    c->paused = 0;
    c->pause_host_us = 0;
    c->stats.resume_count++;
}

void pp_clock_reanchor(pp_clock *c, int64_t pts_us)
{
    if (!c)
        return;
    c->host_start_us = now_us();
    c->media_start_pts_us = pts_us;
    c->started = 1;
    c->paused = 0;
    c->pause_host_us = 0;
    c->media_at_pause_us = 0;
    c->stats.reset_count++;
}

void pp_clock_reset(pp_clock *c)
{
    int64_t max_late, max_early;
    pp_clock_stats kept;
    if (!c)
        return;
    max_late = c->max_late_us;
    max_early = c->max_early_us;
    kept = c->stats;
    memset(c, 0, sizeof(*c));
    c->max_late_us = max_late;
    c->max_early_us = max_early;
    c->stats = kept;
    c->stats.reset_count++;
    c->stats.min_lag_us = INT64_MAX;
    c->stats.max_lag_us = 0;
    c->stats.sum_lag_us = 0;
    c->stats.lag_samples = 0;
    c->stats.last_lag_us = 0;
}

int pp_clock_is_paused(const pp_clock *c)
{
    return c && c->paused;
}

int pp_clock_wait_or_drop(pp_clock *c, int64_t pts_us)
{
    int64_t media, lag, early;

    if (!c)
        return PP_CLOCK_PRESENT;

    /* While paused, do not sleep or drop — caller should not push frames. */
    if (c->paused)
        return PP_CLOCK_DROP;

    if (!c->started)
        pp_clock_start(c, pts_us);

    media = pp_clock_media_us(c);
    lag = media - pts_us;

    if (lag > c->max_late_us) {
        note_lag(c, lag);
        c->stats.late_drops++;
        return PP_CLOCK_DROP;
    }

    if (lag < 0) {
        early = -lag;
        if (early > c->max_early_us)
            early = c->max_early_us;
        if (early > 500) {
            c->stats.wait_count++;
            c->stats.early_sleeps++;
            c->stats.wait_us_total += (uint64_t)early;
            usleep((useconds_t)early);
        }
        if (c->paused)
            return PP_CLOCK_DROP;
        media = pp_clock_media_us(c);
        lag = media - pts_us;
        if (lag > c->max_late_us) {
            note_lag(c, lag);
            c->stats.late_drops++;
            return PP_CLOCK_DROP;
        }
    }

    note_lag(c, lag);
    c->stats.on_time_presents++;
    return PP_CLOCK_PRESENT;
}

void pp_clock_get_stats(const pp_clock *c, pp_clock_stats *out)
{
    if (!c || !out)
        return;
    *out = c->stats;
}
