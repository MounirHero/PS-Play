#include "pp_audio_clock.h"

#include <string.h>
#include <unistd.h>
#include <sys/time.h>

static uint64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

static void recompute_played(pp_audio_clock *c)
{
    /*
     * Latency-corrected playhead:
     *   played = submitted - device_latency
     * (sceAudioOutOutput typically blocks until a slot is free; residual
     *  in the device is ~device_latency_samples.)
     */
    if (c->samples_submitted > c->device_latency_samples)
        c->samples_played = c->samples_submitted - c->device_latency_samples;
    else
        c->samples_played = 0;
    c->samples_queued = c->samples_submitted - c->samples_played;
    c->stats.samples_submitted = c->samples_submitted;
    c->stats.samples_queued = c->samples_queued;
    c->stats.samples_played = c->samples_played;
    c->stats.device_latency_us =
        (int64_t)((c->device_latency_samples * 1000000ull) / (uint64_t)c->sample_rate);
}

void pp_audio_clock_init(pp_audio_clock *c, int sample_rate, int channels)
{
    if (!c)
        return;
    memset(c, 0, sizeof(*c));
    c->state = PP_AV_CLOCK_UNPRIMED;
    c->sample_rate = sample_rate > 0 ? sample_rate : 48000;
    c->channels = channels > 0 ? channels : 2;
    /* Default: 2 × 1024-sample blocks ≈ 42.7 ms @ 48 kHz */
    c->device_latency_samples = 2048;
    c->on_time_us = 10000;
    c->late_drop_us = 45000;
    c->max_early_us = 250000;
    c->hard_resync_us = 500000;
    c->generation = 1;
}

void pp_audio_clock_set_device_latency_samples(pp_audio_clock *c, uint64_t samples)
{
    if (!c)
        return;
    c->device_latency_samples = samples;
    recompute_played(c);
}

void pp_audio_clock_set_thresholds(pp_audio_clock *c, int64_t on_time_us, int64_t late_drop_us,
                                   int64_t max_early_us, int64_t hard_resync_us)
{
    if (!c)
        return;
    if (on_time_us > 0)
        c->on_time_us = on_time_us;
    if (late_drop_us > 0)
        c->late_drop_us = late_drop_us;
    if (max_early_us > 0)
        c->max_early_us = max_early_us;
    if (hard_resync_us > 0)
        c->hard_resync_us = hard_resync_us;
}

void pp_audio_clock_set_from_fps(pp_audio_clock *c, double fps)
{
    int64_t frame_us;
    if (!c || fps < 1.0)
        return;
    frame_us = (int64_t)(1000000.0 / fps);
    c->on_time_us = frame_us / 3;
    if (c->on_time_us < 8000)
        c->on_time_us = 8000;
    c->late_drop_us = (frame_us * 13) / 10;
    if (c->late_drop_us < 40000)
        c->late_drop_us = 40000;
    c->max_early_us = frame_us * 7;
    if (c->max_early_us < 100000)
        c->max_early_us = 100000;
}

pp_av_clock_state pp_audio_clock_state(const pp_audio_clock *c)
{
    return c ? c->state : PP_AV_CLOCK_UNPRIMED;
}

int pp_audio_clock_is_running(const pp_audio_clock *c)
{
    return c && c->state == PP_AV_CLOCK_RUNNING;
}

uint64_t pp_audio_clock_generation(const pp_audio_clock *c)
{
    return c ? c->generation : 0;
}

void pp_audio_clock_begin_seek(pp_audio_clock *c)
{
    if (!c)
        return;
    c->state = PP_AV_CLOCK_SEEKING;
    c->generation++;
    c->samples_submitted = 0;
    c->samples_queued = 0;
    c->samples_played = 0;
    c->first_pts_us = 0;
    c->pause_shift_us = 0;
    c->media_at_pause_us = 0;
    c->stats.resets++;
    c->stats.reanchors++;
    recompute_played(c);
}

void pp_audio_clock_prime(pp_audio_clock *c, int64_t first_audio_pts_us)
{
    if (!c)
        return;
    c->first_pts_us = first_audio_pts_us;
    c->samples_submitted = 0;
    c->samples_queued = 0;
    c->samples_played = 0;
    c->pause_shift_us = 0;
    c->host_start_us = now_us();
    c->state = PP_AV_CLOCK_PRIMING;
    c->stats.reanchors++;
    c->stats.last_media_us = first_audio_pts_us;
    recompute_played(c);
}

void pp_audio_clock_mark_running(pp_audio_clock *c)
{
    if (!c)
        return;
    if (c->state == PP_AV_CLOCK_PRIMING || c->state == PP_AV_CLOCK_SEEKING ||
        c->state == PP_AV_CLOCK_UNPRIMED)
        c->state = PP_AV_CLOCK_RUNNING;
}

void pp_audio_clock_note_submitted(pp_audio_clock *c, uint64_t samples_per_channel)
{
    int64_t m;
    if (!c)
        return;
    if (c->state != PP_AV_CLOCK_PRIMING && c->state != PP_AV_CLOCK_RUNNING)
        return;
    c->samples_submitted += samples_per_channel;
    recompute_played(c);
    m = pp_audio_clock_media_us(c);
    c->stats.last_media_us = m;
    if (m > c->stats.max_media_us)
        c->stats.max_media_us = m;
}

int64_t pp_audio_clock_media_us(const pp_audio_clock *c)
{
    double sec;
    if (!c)
        return 0;
    if (c->state == PP_AV_CLOCK_UNPRIMED || c->state == PP_AV_CLOCK_SEEKING ||
        c->state == PP_AV_CLOCK_EOF)
        return 0;
    if (c->state == PP_AV_CLOCK_PAUSED)
        return c->media_at_pause_us;
    /* PRIMING or RUNNING: use played samples */
    sec = (double)c->samples_played / (double)c->sample_rate;
    return c->first_pts_us + (int64_t)(sec * 1000000.0);
}

void pp_audio_clock_pause(pp_audio_clock *c)
{
    if (!c)
        return;
    if (c->state != PP_AV_CLOCK_RUNNING && c->state != PP_AV_CLOCK_PRIMING)
        return;
    c->media_at_pause_us = pp_audio_clock_media_us(c);
    c->pause_host_us = now_us();
    c->state = PP_AV_CLOCK_PAUSED;
    c->stats.pause_count++;
}

void pp_audio_clock_resume(pp_audio_clock *c)
{
    uint64_t now;
    if (!c || c->state != PP_AV_CLOCK_PAUSED)
        return;
    now = now_us();
    /*
     * If the audio device cannot truly pause, host time still advances.
     * Played-sample clock does not advance while PAUSED, so no shift is
     * required when resume continues from the same sample counters.
     * Record pause duration for diagnostics only.
     */
    (void)now;
    c->state = PP_AV_CLOCK_RUNNING;
    c->stats.resume_count++;
}

void pp_audio_clock_reset(pp_audio_clock *c)
{
    int sr, ch;
    int64_t ot, ld, me, hr;
    uint64_t dlat, gen;
    if (!c)
        return;
    sr = c->sample_rate;
    ch = c->channels;
    ot = c->on_time_us;
    ld = c->late_drop_us;
    me = c->max_early_us;
    hr = c->hard_resync_us;
    dlat = c->device_latency_samples;
    gen = c->generation + 1;
    memset(c, 0, sizeof(*c));
    c->sample_rate = sr;
    c->channels = ch;
    c->on_time_us = ot;
    c->late_drop_us = ld;
    c->max_early_us = me;
    c->hard_resync_us = hr;
    c->device_latency_samples = dlat;
    c->generation = gen;
    c->state = PP_AV_CLOCK_UNPRIMED;
    c->stats.resets++;
}

void pp_audio_clock_mark_eof(pp_audio_clock *c)
{
    if (c)
        c->state = PP_AV_CLOCK_EOF;
}

int pp_audio_clock_schedule_video(pp_audio_clock *c, int64_t video_pts_us, int64_t *out_delta_us)
{
    int64_t audio_us, delta, early;

    if (!c)
        return PP_AV_HOLD;
    if (c->state != PP_AV_CLOCK_RUNNING) {
        if (out_delta_us)
            *out_delta_us = 0;
        return PP_AV_HOLD;
    }

    audio_us = pp_audio_clock_media_us(c);
    /*
     * delta = video_pts − audio_clock
     *   delta < 0  → video behind audio (late) → drop if past threshold
     *   delta > 0  → video ahead of audio (early) → wait
     */
    delta = video_pts_us - audio_us;
    if (out_delta_us)
        *out_delta_us = delta;

    if (delta < -c->late_drop_us)
        return PP_AV_DROP;

    if (delta > c->on_time_us) {
        early = delta;
        if (early > c->max_early_us)
            early = c->max_early_us;
        if (early > 500)
            usleep((useconds_t)early);
        return PP_AV_PRESENT;
    }

    return PP_AV_PRESENT;
}

void pp_audio_clock_note_underrun(pp_audio_clock *c)
{
    if (c)
        c->stats.underruns++;
}

void pp_audio_clock_get_stats(const pp_audio_clock *c, pp_audio_clock_stats *out)
{
    if (!c || !out)
        return;
    *out = c->stats;
    out->samples_submitted = c->samples_submitted;
    out->samples_queued = c->samples_queued;
    out->samples_played = c->samples_played;
    out->last_media_us = pp_audio_clock_media_us(c);
    out->device_latency_us =
        (int64_t)((c->device_latency_samples * 1000000ull) / (uint64_t)c->sample_rate);
}
