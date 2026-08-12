#include "pp_playback.h"
#include "pp_converter_fused.h"
#include "pp_converter_parallel.h"
#include "pp_output_policy.h"
#include "pp_4k_sdr_policy.h"
#include "pp_product_path.h"
#include "pp_v8_gate.h"
#include "pp_stage_breadcrumb.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static uint64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

static uint64_t now_ms(void)
{
    return now_us() / 1000ull;
}

static pthread_mutex_t *mtx(pp_playback *pb)
{
    return (pthread_mutex_t *)pb->lock;
}

void pp_playback_init(pp_playback *pb)
{
    pthread_mutex_t *m;
    if (!pb)
        return;
    memset(pb, 0, sizeof(*pb));
    m = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    if (m) {
        pthread_mutex_init(m, NULL);
        pb->lock = m;
    }
    /*
     * Soft UHD: very wide late window (never freeze on late-drop).
     * Early sleep capped short so convert+decode can stay realtime.
     */
    pp_clock_init(&pb->clock, 800000, 25000);
    pb->cfg.aspect = PP_ASPECT_FIT;
    pb->cfg.clear_color_bgra = 0xFF000000u;
    pb->out_w = 1920;
    pb->out_h = 1080;
    pb->display = NULL;
    pb->display_back = NULL;
    pb->backend = PP_BACKEND_1080_STANDARD;
    pb->force_v3_fallback = 0;
    pb->pending_present = 0;
    pb->pending_vo_idx = 0;
    pb->pending_frame_id = 0;
    pb->present_seq = 0;
    pb->stats.output_w = 1920;
    pb->stats.output_h = 1080;
    pb->stats.aspect = (int)PP_ASPECT_FIT;
    pb->stats.backend = (int)PP_BACKEND_1080_STANDARD;
}

void pp_playback_set_backend(pp_playback *pb, pp_video_backend backend)
{
    if (!pb)
        return;
    if (pb->lock)
        pthread_mutex_lock(mtx(pb));
    pb->backend = backend;
    pb->stats.backend = (int)backend;
    pb->pending_present = 0;
    if (pb->lock)
        pthread_mutex_unlock(mtx(pb));
}

void pp_playback_force_v3_fallback(pp_playback *pb, int enable)
{
    if (!pb)
        return;
    if (pb->lock)
        pthread_mutex_lock(mtx(pb));
    pb->force_v3_fallback = enable ? 1 : 0;
    if (enable && pb->backend == PP_BACKEND_4K_V8_FUSED)
        pb->backend = PP_BACKEND_4K_V3_FALLBACK;
    pb->stats.backend = (int)pb->backend;
    if (pb->lock)
        pthread_mutex_unlock(mtx(pb));
}

void pp_playback_shutdown(pp_playback *pb)
{
    if (!pb)
        return;
    pp_playback_on_file_close(pb);
    free(pb->display);
    pb->display = NULL;
    free(pb->display_back);
    pb->display_back = NULL;
    if (pb->lock) {
        pthread_mutex_destroy(mtx(pb));
        free(pb->lock);
        pb->lock = NULL;
    }
    memset(pb, 0, sizeof(*pb));
}

void pp_playback_attach_videoout(pp_playback *pb, pp_videoout *vo)
{
    if (!pb)
        return;
    pb->vo = vo;
}

int pp_playback_set_output(pp_playback *pb, uint32_t w, uint32_t h,
                           pp_aspect_mode aspect)
{
    uint32_t *front, *back;
    size_t bytes;
    if (!pb || w < 64 || h < 64)
        return -1;
    bytes = (size_t)w * (size_t)h * 4u;
    front = (uint32_t *)malloc(bytes);
    back = (uint32_t *)malloc(bytes);
    if (!front || !back) {
        free(front);
        free(back);
        return -2;
    }
    if (pb->lock)
        pthread_mutex_lock(mtx(pb));
    free(pb->display);
    free(pb->display_back);
    pb->display = front;
    pb->display_back = back;
    pb->out_w = w;
    pb->out_h = h;
    pb->cfg.aspect = aspect;
    pb->display_ready = 0;
    pb->stats.output_w = (int)w;
    pb->stats.output_h = (int)h;
    pb->stats.aspect = (int)aspect;
    if (pb->lock)
        pthread_mutex_unlock(mtx(pb));
    return 0;
}

void pp_playback_on_file_open(pp_playback *pb)
{
    if (!pb)
        return;
    if (!pb->display && pb->backend != PP_BACKEND_4K_V8_FUSED)
        (void)pp_playback_set_output(pb, pb->out_w ? pb->out_w : 1920,
                                     pb->out_h ? pb->out_h : 1080, pb->cfg.aspect);
    pp_clock_reset(&pb->clock);
    pb->stats.clock_resets++;
    pb->active = 1;
    pb->seek_discarding = 0;
    pb->display_ready = 0;
    pb->display_pts_us = 0;
    if (pb->pending_present && pb->vo)
        pp_videoout_release(pb->vo, pb->pending_vo_idx);
    pb->pending_present = 0;
    pb->stats.frames_in = 0;
    pb->stats.frames_converted = 0;
    pb->stats.frames_published = 0;
    pb->stats.frames_late_dropped = 0;
    pb->stats.frames_discarded_seek = 0;
    pb->stats.convert_us_total = 0;
    pb->stats.convert_us_max = 0;
    pb->stats.convert_ring_count = 0;
    pb->stats.sample_bgra = 0;
}

void pp_playback_on_file_close(pp_playback *pb)
{
    if (!pb)
        return;
    pb->active = 0;
    pb->seek_discarding = 0;
    pp_clock_reset(&pb->clock);
    pb->stats.clock_resets++;
    if (pb->lock)
        pthread_mutex_lock(mtx(pb));
    if (pb->pending_present && pb->vo)
        pp_videoout_release(pb->vo, pb->pending_vo_idx);
    pb->pending_present = 0;
    pb->display_ready = 0;
    if (pb->lock)
        pthread_mutex_unlock(mtx(pb));
}

void pp_playback_pause(pp_playback *pb)
{
    if (!pb)
        return;
    pp_clock_pause(&pb->clock);
}

void pp_playback_resume(pp_playback *pb)
{
    if (!pb)
        return;
    pp_clock_resume(&pb->clock);
}

static void note_convert(pp_playback *pb, uint64_t us)
{
    uint32_t i;
    pb->stats.convert_us_total += us;
    if (us > pb->stats.convert_us_max)
        pb->stats.convert_us_max = us;
    /* Rolling ring of last 256 convert times for p95. */
    if (pb->stats.convert_ring_count < 256u) {
        i = pb->stats.convert_ring_count;
        pb->stats.convert_ring[i] = us;
        pb->stats.convert_ring_count++;
    } else {
        i = (uint32_t)(pb->stats.frames_converted % 256u);
        pb->stats.convert_ring[i] = us;
    }
}

int pp_playback_push_frame(pp_playback *pb, const pp_frame *src)
{
    uint64_t t0, t1, cus;
    int rc;
    size_t need;
    int use_v8;

    if (!pb || !src || !pb->active)
        return -1;

    pb->stats.frames_in++;

    if (pb->seek_discarding) {
        if (src->pts_us < pb->seek_target_us) {
            pb->stats.frames_discarded_seek++;
            return 1;
        }
        pp_clock_reset(&pb->clock);
        pb->stats.clock_resets++;
        pp_clock_start(&pb->clock, src->pts_us);
        pb->seek_discarding = 0;
        if (pb->seek_begin_ms) {
            pb->stats.seek_to_first_frame_ms = now_ms() - pb->seek_begin_ms;
            pb->seek_begin_ms = 0;
        }
    }

    if (pp_clock_is_paused(&pb->clock))
        return 1;

    need = (size_t)pb->out_w * (size_t)pb->out_h;
    if (need == 0 || need > (size_t)3840u * 2160u)
        return -2;

    /*
     * V8 fused: 1:1 yuv420p → tiled BGRA at live VO size.
     * Supports full 3840x2160 and cinema UHD (e.g. 3840x1920).
     */
    use_v8 = (pb->backend == PP_BACKEND_4K_V8_FUSED && !pb->force_v3_fallback &&
              pb->vo && pb->vo->inited &&
              pb->out_w == pb->vo->width && pb->out_h == pb->vo->height &&
              src->width == pb->out_w && src->height == pb->out_h &&
              pp_v8_frame_gate(src, 30.0) == PP_V8_GATE_OK);

    /*
     * V8 is strict 1:1 tile (no FIT/FILL/STRETCH). If the user forced V3 via
     * view-mode apply (force_v3_fallback), use_v8 stays 0 and aspect convert runs.
     */

#if !PP_4K_V8_PRODUCT_ENABLE
    use_v8 = 0; /* crash isolation: product never uses V8 path */
#endif

    /* ---- V8 fused: write tiled GPU plane; main present_pre_tiled ---- */
    if (use_v8) {
        uint32_t idx = 0, pitch = 0;
        uint32_t *lin;
        uint32_t *gpu;
        int v8_workers = 8;

        /* VO must match output — drop if mid-reconfig */
        if (!pb->vo->inited ||
            pb->vo->width != pb->out_w || pb->vo->height != pb->out_h)
            return 1;

        if (pp_clock_wait_or_drop(&pb->clock, src->pts_us) == PP_CLOCK_DROP) {
            pb->stats.frames_late_dropped++;
            return 1;
        }

        lin = (uint32_t *)pp_videoout_acquire(pb->vo, &idx, &pitch);
        gpu = (uint32_t *)pp_videoout_gpu_plane(pb->vo, idx);
        if (!lin || !gpu)
            return -5;

        if (pb->stats.frames_converted == 0)
            pp_stage_bc_checkpoint("010_FIRST_FRAME_DECODED", "entering V8 convert");

        /* More workers on wide UHD frames (was 4 — under-used CPU) */
        if (pb->out_w * pb->out_h >= 3840u * 1600u)
            v8_workers = 12;

        t0 = now_us();
        rc = pp_converter_yuv420p_to_tiled_bgra_parallel(
            src, gpu, pb->out_w, pb->out_h, v8_workers);
        t1 = now_us();
        if (rc != 0) {
            pp_videoout_release(pb->vo, idx);
            pp_stage_bc_checkpoint("011_FIRST_FRAME_CONVERT_FAIL", "fused rc");
            return -4;
        }
        cus = t1 - t0;
        pb->stats.frames_converted++;
        note_convert(pb, cus);
        if (pb->stats.frames_converted == 1)
            pp_stage_bc_checkpoint("011_FIRST_FRAME_CONVERTED", "fused ok");

        if (pb->lock)
            pthread_mutex_lock(mtx(pb));
        /* Drop previous unpresented V8 frame if any */
        if (pb->pending_present)
            pp_videoout_release(pb->vo, pb->pending_vo_idx);
        pb->present_seq++;
        pb->pending_vo_idx = idx;
        pb->pending_frame_id = pb->present_seq;
        pb->pending_present = 1;
        pb->display_ready = 1;
        pb->display_pts_us = src->pts_us;
        pb->stats.frames_published++;
        if (pb->lock)
            pthread_mutex_unlock(mtx(pb));
        return 0;
    }

    /* ---- 1080 / V3 fallback: linear display then main tiles ---- */
    if (!pb->display || !pb->display_back) {
        if (pp_playback_set_output(pb, pb->out_w, pb->out_h, pb->cfg.aspect) != 0)
            return -3;
    }

    {
        /*
         * Soft UHD: audio-master pacing lives in decode_next_video_frame.
         * Do not host-clock sleep or late-drop here (double-pace / freezes).
         * Non-soft paths still use the normal PTS clock.
         */
        int soft_uhd = (src->width >= 2560u && pb->out_w <= 1920u);
        if (soft_uhd) {
            if (!pb->clock.started)
                pp_clock_start(&pb->clock, src->pts_us);
        } else if (pp_clock_wait_or_drop(&pb->clock, src->pts_us) ==
                   PP_CLOCK_DROP) {
            pb->stats.frames_late_dropped++;
            return 1;
        }
    }

    /* Convert into back buffer WITHOUT holding display lock (UI can blit). */
    t0 = now_us();
    rc = pp_converter_to_display(src, pb->display_back, pb->out_w, pb->out_h,
                                 pb->out_w * 4u, pb->cfg.aspect);
    t1 = now_us();
    if (rc != 0)
        return -4;
    cus = t1 - t0;
    pb->stats.frames_converted++;
    note_convert(pb, cus);

    /* Swap back → front under lock (pointer swap, no memcpy). */
    if (pb->lock)
        pthread_mutex_lock(mtx(pb));
    {
        uint32_t *tmp = pb->display;
        pb->display = pb->display_back;
        pb->display_back = tmp;
    }

    /*
     * Only re-anchor on large host lag (not every soft-UHD frame).
     * Re-anchoring every frame made residual early-sleep tiny → free-run.
     */
    {
        int soft_uhd = (src->width >= 2560u && pb->out_w <= 1920u);
        if (!soft_uhd) {
            int64_t media = pp_clock_media_us(&pb->clock);
            int64_t lag = media - src->pts_us;
            if (lag > 120000 || lag < -120000)
                pp_clock_reanchor(&pb->clock, src->pts_us);
        }
    }

    pb->display_ready = 1;
    pb->display_pts_us = src->pts_us;
    pb->stats.frames_published++;
    if (pb->stats.sample_bgra == 0 && need > 0)
        pb->stats.sample_bgra = pb->display[need / 2u];
    if (pb->lock)
        pthread_mutex_unlock(mtx(pb));
    return 0;
}

int pp_playback_take_pending_present(pp_playback *pb, uint32_t *idx, uint64_t *frame_id)
{
    int ok = 0;
    if (!pb || !idx || !frame_id)
        return 0;
    if (pb->lock)
        pthread_mutex_lock(mtx(pb));
    if (pb->pending_present) {
        *idx = pb->pending_vo_idx;
        *frame_id = pb->pending_frame_id;
        pb->pending_present = 0;
        ok = 1;
    }
    if (pb->lock)
        pthread_mutex_unlock(mtx(pb));
    return ok;
}

/**
 * Apply V3 product output policy for a newly opened source.
 * Does not reconfigure VideoOut — caller must pp_videoout_reconfigure + set_output.
 * Returns selected pp_output_mode.
 */
int pp_playback_choose_output_mode(const pp_source_caps *src,
                                   uint32_t display_max_w,
                                   uint32_t display_max_h)
{
    return (int)pp_select_output_mode(src, display_max_w, display_max_h);
}

int pp_playback_copy_display(pp_playback *pb, uint32_t *dst, uint32_t pitch_bytes,
                             uint32_t dst_w, uint32_t dst_h)
{
    uint32_t y, copy_w, copy_h, p;
    if (!pb || !dst || !pb->display)
        return 0;
    if (pb->lock)
        pthread_mutex_lock(mtx(pb));
    if (!pb->display_ready) {
        if (pb->lock)
            pthread_mutex_unlock(mtx(pb));
        return 0;
    }
    copy_w = pb->out_w < dst_w ? pb->out_w : dst_w;
    copy_h = pb->out_h < dst_h ? pb->out_h : dst_h;
    p = pitch_bytes / 4u;
    for (y = 0; y < copy_h; y++)
        memcpy(dst + y * p, pb->display + y * pb->out_w, copy_w * 4u);
    if (pb->lock)
        pthread_mutex_unlock(mtx(pb));
    return 1;
}

int pp_playback_has_display(const pp_playback *pb)
{
    return pb && pb->display_ready;
}

void pp_playback_notify_seek_begin(pp_playback *pb, int64_t target_pts_us)
{
    if (!pb)
        return;
    pb->stats.seek_requests++;
    pb->seek_discarding = 1;
    pb->seek_target_us = target_pts_us;
    pb->seek_begin_ms = now_ms();
    pp_clock_pause(&pb->clock);
    if (pb->lock)
        pthread_mutex_lock(mtx(pb));
    pb->display_ready = 0;
    if (pb->lock)
        pthread_mutex_unlock(mtx(pb));
}

void pp_playback_notify_seek_end(pp_playback *pb, int success,
                                 uint64_t discarded, uint64_t elapsed_ms)
{
    if (!pb)
        return;
    (void)discarded;
    (void)elapsed_ms;
    if (success)
        pb->stats.seek_successes++;
    else {
        pb->stats.seek_failures++;
        pb->seek_discarding = 0;
    }
    /* clock re-start happens on first post-seek push_frame */
}

void pp_playback_get_stats(const pp_playback *pb, pp_playback_stats *out)
{
    if (!pb || !out)
        return;
    *out = pb->stats;
}

uint64_t pp_playback_convert_p95_us(const pp_playback *pb)
{
    uint64_t tmp[256];
    uint32_t n, i, j, idx;
    if (!pb)
        return 0;
    n = pb->stats.convert_ring_count;
    if (n == 0)
        return 0;
    if (n > 256)
        n = 256;
    memcpy(tmp, pb->stats.convert_ring, n * sizeof(uint64_t));
    /* insertion sort small */
    for (i = 1; i < n; i++) {
        uint64_t v = tmp[i];
        j = i;
        while (j > 0 && tmp[j - 1] > v) {
            tmp[j] = tmp[j - 1];
            j--;
        }
        tmp[j] = v;
    }
    idx = (n * 95u) / 100u;
    if (idx >= n)
        idx = n - 1;
    return tmp[idx];
}

void pp_playback_write_stats_file(const pp_playback *pb, const char *path)
{
    FILE *f;
    pp_clock_stats cs;
    uint64_t avg = 0;
    if (!pb || !path)
        return;
    f = fopen(path, "w");
    if (!f)
        return;
    pp_clock_get_stats(&pb->clock, &cs);
    if (pb->stats.frames_converted)
        avg = pb->stats.convert_us_total / pb->stats.frames_converted;
    fprintf(f, "pp_playback_stats\n");
    fprintf(f, "output=%ux%u aspect=%d\n", pb->out_w, pb->out_h, (int)pb->cfg.aspect);
    fprintf(f, "frames_in=%llu converted=%llu published=%llu late_drop=%llu seek_disc=%llu\n",
            (unsigned long long)pb->stats.frames_in,
            (unsigned long long)pb->stats.frames_converted,
            (unsigned long long)pb->stats.frames_published,
            (unsigned long long)pb->stats.frames_late_dropped,
            (unsigned long long)pb->stats.frames_discarded_seek);
    fprintf(f, "convert_us_avg=%llu convert_us_p95=%llu convert_us_max=%llu\n",
            (unsigned long long)avg,
            (unsigned long long)pp_playback_convert_p95_us(pb),
            (unsigned long long)pb->stats.convert_us_max);
    fprintf(f, "seek_requests=%llu seek_ok=%llu seek_fail=%llu clock_resets=%llu seek_to_first_ms=%llu\n",
            (unsigned long long)pb->stats.seek_requests,
            (unsigned long long)pb->stats.seek_successes,
            (unsigned long long)pb->stats.seek_failures,
            (unsigned long long)pb->stats.clock_resets,
            (unsigned long long)pb->stats.seek_to_first_frame_ms);
    fprintf(f, "clock_late_drops=%llu early_sleeps=%llu pause=%llu resume=%llu\n",
            (unsigned long long)cs.late_drops,
            (unsigned long long)cs.early_sleeps,
            (unsigned long long)cs.pause_count,
            (unsigned long long)cs.resume_count);
    fprintf(f, "sample_bgra=0x%08X\n", (unsigned)pb->stats.sample_bgra);
    fclose(f);
}
