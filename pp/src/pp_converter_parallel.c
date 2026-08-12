#include "pp_converter_parallel.h"
#include "pp_converter.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

/*
 * Soft-UHD freeze root cause was pthread_create/join *every frame* (8 workers).
 * That stalls every few frames under scheduler pressure. Use a persistent pool
 * and a single-thread fast path for 1080 downscale (thread spawn cost > gain).
 */

#define PP_POOL_MAX 8

typedef struct {
    const pp_frame *src;
    uint32_t *dst;
    uint32_t pitch_px;
    uint32_t y0, y1;
} band_job;

typedef struct {
    const pp_frame *src;
    uint32_t *dst;
    uint32_t pitch_px;
    /* Destination paint rect (within full frame buffer) */
    uint32_t ox, oy, ow, oh;
    /* Source crop region */
    uint32_t sx0, sy0, sw_use, sh_use;
    /* Band within paint rect: relative y in [0, oh) */
    uint32_t y0, y1;
} scale_band_job;

typedef enum {
    PP_JOB_NONE = 0,
    PP_JOB_1TO1,
    PP_JOB_SCALE_NN
} pp_job_kind;

static struct {
    pthread_t th[PP_POOL_MAX];
    pthread_mutex_t m;
    pthread_cond_t work_cv;
    pthread_cond_t done_cv;
    int started;
    int shutdown;
    int n_workers;
    int generation;
    int done_count;
    pp_job_kind kind;
    band_job jobs_1[PP_POOL_MAX];
    scale_band_job jobs_s[PP_POOL_MAX];
} g_pool;

static uint32_t yuv_to_bgra_r_low(int y, int u, int v)
{
    int c = y - 16, d = u - 128, e = v - 128;
    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;
    if (r < 0)
        r = 0;
    if (r > 255)
        r = 255;
    if (g < 0)
        g = 0;
    if (g > 255)
        g = 255;
    if (b < 0)
        b = 0;
    if (b > 255)
        b = 255;
    return 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
}

static void run_1to1_band(const band_job *j)
{
    uint32_t y, x;
    const pp_frame *s = j->src;
    for (y = j->y0; y < j->y1; y++) {
        uint32_t *row = j->dst + y * j->pitch_px;
        const uint8_t *Y = s->planes[0] + y * (uint32_t)s->strides[0];
        const uint8_t *U = s->planes[1] + (y / 2u) * (uint32_t)s->strides[1];
        const uint8_t *V = s->planes[2] + (y / 2u) * (uint32_t)s->strides[2];
        /* 2-wide: reuse U/V for chroma pair */
        for (x = 0; x + 1u < s->width; x += 2u) {
            int uu = U[x / 2u];
            int vv = V[x / 2u];
            row[x] = yuv_to_bgra_r_low(Y[x], uu, vv);
            row[x + 1u] = yuv_to_bgra_r_low(Y[x + 1u], uu, vv);
        }
        if (x < s->width)
            row[x] = yuv_to_bgra_r_low(Y[x], U[x / 2u], V[x / 2u]);
    }
}

static void run_scale_band(const scale_band_job *j)
{
    const pp_frame *s = j->src;
    uint32_t y, x;
    uint32_t x_step, y_step;

    if (!j->ow || !j->oh || !j->sw_use || !j->sh_use)
        return;

    x_step = (j->sw_use << 16) / j->ow;
    y_step = (j->sh_use << 16) / j->oh;

    for (y = j->y0; y < j->y1; y++) {
        uint32_t sy = j->sy0 + ((y * y_step) >> 16);
        uint32_t *row = j->dst + (j->oy + y) * j->pitch_px + j->ox;
        uint32_t sx_acc = 0;
        const uint8_t *Y;
        const uint8_t *U;
        const uint8_t *V;
        if (sy >= s->height)
            sy = s->height - 1u;
        Y = s->planes[0] + sy * (uint32_t)s->strides[0];
        U = s->planes[1] + (sy / 2u) * (uint32_t)s->strides[1];
        V = s->planes[2] + (sy / 2u) * (uint32_t)s->strides[2];
        for (x = 0; x < j->ow; x++) {
            uint32_t sx = j->sx0 + (sx_acc >> 16);
            if (sx >= s->width)
                sx = s->width - 1u;
            row[x] = yuv_to_bgra_r_low(Y[sx], U[sx / 2u], V[sx / 2u]);
            sx_acc += x_step;
        }
    }
}

/* Compute FIT / FILL / STRETCH geometry (same rules as pp_converter.c). */
static void compute_aspect_rect(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh,
                                pp_aspect_mode mode,
                                uint32_t *ox, uint32_t *oy, uint32_t *ow, uint32_t *oh,
                                uint32_t *sx0, uint32_t *sy0,
                                uint32_t *sw_use, uint32_t *sh_use)
{
    double src_ar, dst_ar;

    *sx0 = 0;
    *sy0 = 0;
    *sw_use = sw;
    *sh_use = sh;

    if (mode == PP_ASPECT_STRETCH) {
        *ox = 0;
        *oy = 0;
        *ow = dw;
        *oh = dh;
        return;
    }

    src_ar = (double)sw / (double)sh;
    dst_ar = (double)dw / (double)dh;

    if (mode == PP_ASPECT_FIT) {
        if (src_ar > dst_ar) {
            *ow = dw;
            *oh = (uint32_t)((double)dw / src_ar + 0.5);
            if (*oh > dh)
                *oh = dh;
            *ox = 0;
            *oy = (dh - *oh) / 2u;
        } else {
            *oh = dh;
            *ow = (uint32_t)((double)dh * src_ar + 0.5);
            if (*ow > dw)
                *ow = dw;
            *oy = 0;
            *ox = (dw - *ow) / 2u;
        }
        return;
    }

    /* FILL: crop source so scaled rect covers dest fully */
    *ox = 0;
    *oy = 0;
    *ow = dw;
    *oh = dh;
    if (src_ar > dst_ar) {
        *sw_use = (uint32_t)((double)sh * dst_ar + 0.5);
        if (*sw_use > sw)
            *sw_use = sw;
        *sx0 = (sw - *sw_use) / 2u;
        *sh_use = sh;
        *sy0 = 0;
    } else {
        *sh_use = (uint32_t)((double)sw / dst_ar + 0.5);
        if (*sh_use > sh)
            *sh_use = sh;
        *sy0 = (sh - *sh_use) / 2u;
        *sw_use = sw;
        *sx0 = 0;
    }
}

static void clear_bgra(uint32_t *dst, uint32_t pitch_px, uint32_t w, uint32_t h,
                       uint32_t color)
{
    uint32_t y;
    for (y = 0; y < h; y++) {
        uint32_t *row = dst + y * pitch_px;
        uint32_t x;
        for (x = 0; x < w; x++)
            row[x] = color;
    }
}

static void *pool_worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    int seen_gen = 0;

    for (;;) {
        pthread_mutex_lock(&g_pool.m);
        while (!g_pool.shutdown && g_pool.generation == seen_gen)
            pthread_cond_wait(&g_pool.work_cv, &g_pool.m);
        if (g_pool.shutdown) {
            pthread_mutex_unlock(&g_pool.m);
            break;
        }
        seen_gen = g_pool.generation;
        pp_job_kind kind = g_pool.kind;
        band_job j1;
        scale_band_job js;
        if (kind == PP_JOB_1TO1)
            j1 = g_pool.jobs_1[id];
        else
            js = g_pool.jobs_s[id];
        pthread_mutex_unlock(&g_pool.m);

        if (kind == PP_JOB_1TO1) {
            if (j1.y0 < j1.y1)
                run_1to1_band(&j1);
        } else if (kind == PP_JOB_SCALE_NN) {
            if (js.y0 < js.y1)
                run_scale_band(&js);
        }

        pthread_mutex_lock(&g_pool.m);
        g_pool.done_count++;
        if (g_pool.done_count >= g_pool.n_workers)
            pthread_cond_signal(&g_pool.done_cv);
        pthread_mutex_unlock(&g_pool.m);
    }
    return NULL;
}

static int pool_ensure(int n)
{
    int i;
    if (n < 1)
        n = 1;
    if (n > PP_POOL_MAX)
        n = PP_POOL_MAX;
    if (g_pool.started && g_pool.n_workers == n)
        return 0;
    if (g_pool.started) {
        /* Resize not supported mid-run; keep existing size */
        return 0;
    }
    memset(&g_pool, 0, sizeof(g_pool));
    pthread_mutex_init(&g_pool.m, NULL);
    pthread_cond_init(&g_pool.work_cv, NULL);
    pthread_cond_init(&g_pool.done_cv, NULL);
    g_pool.n_workers = n;
    g_pool.started = 1;
    for (i = 0; i < n; i++) {
        if (pthread_create(&g_pool.th[i], NULL, pool_worker,
                           (void *)(intptr_t)i) != 0) {
            g_pool.n_workers = i;
            break;
        }
    }
    return g_pool.n_workers > 0 ? 0 : -1;
}

static int pool_run_1to1(const pp_frame *source, uint32_t *dst, uint32_t pitch_px,
                         int workers)
{
    int i, n;
    uint32_t band;

    if (pool_ensure(workers) != 0)
        return -1;
    n = g_pool.n_workers;
    band = (source->height + (uint32_t)n - 1u) / (uint32_t)n;
    if (band & 1u)
        band++;

    pthread_mutex_lock(&g_pool.m);
    g_pool.kind = PP_JOB_1TO1;
    g_pool.done_count = 0;
    for (i = 0; i < n; i++) {
        g_pool.jobs_1[i].src = source;
        g_pool.jobs_1[i].dst = dst;
        g_pool.jobs_1[i].pitch_px = pitch_px;
        g_pool.jobs_1[i].y0 = (uint32_t)i * band;
        g_pool.jobs_1[i].y1 = g_pool.jobs_1[i].y0 + band;
        if (g_pool.jobs_1[i].y1 > source->height)
            g_pool.jobs_1[i].y1 = source->height;
        if (g_pool.jobs_1[i].y0 >= source->height)
            g_pool.jobs_1[i].y0 = g_pool.jobs_1[i].y1 = source->height;
    }
    g_pool.generation++;
    pthread_cond_broadcast(&g_pool.work_cv);
    while (g_pool.done_count < n)
        pthread_cond_wait(&g_pool.done_cv, &g_pool.m);
    pthread_mutex_unlock(&g_pool.m);
    return 0;
}

static int pool_run_scale(const pp_frame *source, uint32_t *dst, uint32_t pitch_px,
                          uint32_t ox, uint32_t oy, uint32_t ow, uint32_t oh,
                          uint32_t sx0, uint32_t sy0, uint32_t sw_use, uint32_t sh_use,
                          int workers)
{
    int i, n;
    uint32_t band;

    if (ow < 2 || oh < 2)
        return -1;
    if (pool_ensure(workers) != 0)
        return -1;
    n = g_pool.n_workers;
    band = (oh + (uint32_t)n - 1u) / (uint32_t)n;

    pthread_mutex_lock(&g_pool.m);
    g_pool.kind = PP_JOB_SCALE_NN;
    g_pool.done_count = 0;
    for (i = 0; i < n; i++) {
        g_pool.jobs_s[i].src = source;
        g_pool.jobs_s[i].dst = dst;
        g_pool.jobs_s[i].pitch_px = pitch_px;
        g_pool.jobs_s[i].ox = ox;
        g_pool.jobs_s[i].oy = oy;
        g_pool.jobs_s[i].ow = ow;
        g_pool.jobs_s[i].oh = oh;
        g_pool.jobs_s[i].sx0 = sx0;
        g_pool.jobs_s[i].sy0 = sy0;
        g_pool.jobs_s[i].sw_use = sw_use;
        g_pool.jobs_s[i].sh_use = sh_use;
        g_pool.jobs_s[i].y0 = (uint32_t)i * band;
        g_pool.jobs_s[i].y1 = g_pool.jobs_s[i].y0 + band;
        if (g_pool.jobs_s[i].y1 > oh)
            g_pool.jobs_s[i].y1 = oh;
        if (g_pool.jobs_s[i].y0 >= oh)
            g_pool.jobs_s[i].y0 = g_pool.jobs_s[i].y1 = oh;
    }
    g_pool.generation++;
    pthread_cond_broadcast(&g_pool.work_cv);
    while (g_pool.done_count < n)
        pthread_cond_wait(&g_pool.done_cv, &g_pool.m);
    pthread_mutex_unlock(&g_pool.m);
    return 0;
}

int pp_converter_yuv420p_to_bgra_parallel(const pp_frame *source,
                                          void *destination,
                                          uint32_t destination_pitch_bytes,
                                          int workers)
{
    uint32_t pitch_px;

    if (!source || !destination || !source->planes[0] || !source->planes[1] ||
        !source->planes[2])
        return -1;
    if (source->format != PP_FRAME_YUV420P)
        return -2;
    if (destination_pitch_bytes < source->width * 4u)
        return -3;
    if (workers < 1)
        workers = 1;
    if (workers > PP_POOL_MAX)
        workers = PP_POOL_MAX;

    pitch_px = destination_pitch_bytes / 4u;
    return pool_run_1to1(source, (uint32_t *)destination, pitch_px, workers);
}

static int pp_converter_yuv420p_scale_nn_aspect(
    const pp_frame *source,
    void *destination,
    uint32_t dst_w,
    uint32_t dst_h,
    uint32_t destination_pitch_bytes,
    pp_aspect_mode aspect,
    int workers)
{
    uint32_t pitch_px;
    uint32_t ox, oy, ow, oh, sx0, sy0, sw_use, sh_use;
    scale_band_job j;

    if (!source || !destination || source->format != PP_FRAME_YUV420P)
        return -1;
    if (!source->planes[0] || !source->planes[1] || !source->planes[2])
        return -2;
    if (destination_pitch_bytes < dst_w * 4u || dst_w < 2 || dst_h < 2)
        return -3;

    pitch_px = destination_pitch_bytes / 4u;

    compute_aspect_rect(source->width, source->height, dst_w, dst_h, aspect,
                        &ox, &oy, &ow, &oh, &sx0, &sy0, &sw_use, &sh_use);
    if (ow < 2 || oh < 2)
        return -4;

    /* Letterbox / pillarbox bars for FIT */
    if (aspect == PP_ASPECT_FIT)
        clear_bgra((uint32_t *)destination, pitch_px, dst_w, dst_h, 0xFF000000u);

    if (workers < 1)
        workers = 4;
    if (workers > 4)
        workers = 4;

    if (pool_ensure(workers) == 0 && g_pool.n_workers >= 2)
        return pool_run_scale(source, (uint32_t *)destination, pitch_px,
                              ox, oy, ow, oh, sx0, sy0, sw_use, sh_use,
                              g_pool.n_workers);

    j.src = source;
    j.dst = (uint32_t *)destination;
    j.pitch_px = pitch_px;
    j.ox = ox;
    j.oy = oy;
    j.ow = ow;
    j.oh = oh;
    j.sx0 = sx0;
    j.sy0 = sy0;
    j.sw_use = sw_use;
    j.sh_use = sh_use;
    j.y0 = 0;
    j.y1 = oh;
    run_scale_band(&j);
    return 0;
}

int pp_converter_to_display(const pp_frame *source,
                            void *destination,
                            uint32_t destination_width,
                            uint32_t destination_height,
                            uint32_t destination_pitch_bytes,
                            pp_aspect_mode aspect)
{
    pp_converter_config cfg;

    if (!source || !destination)
        return -1;

    if (aspect > PP_ASPECT_STRETCH)
        aspect = PP_ASPECT_FIT;

    /*
     * Parallel 1:1 only when letterbox/crop cannot change the picture:
     * same pixel size AND (stretch OR identical aspect ratio).
     * Otherwise honor FIT/FILL so HEVC/4K view modes work.
     */
    if (source->format == PP_FRAME_YUV420P && source->width >= 2560u &&
        source->width == destination_width &&
        source->height == destination_height) {
        double sar = (double)source->width / (double)source->height;
        double dar =
            (double)destination_width / (double)destination_height;
        if (aspect == PP_ASPECT_STRETCH ||
            (sar > dar - 0.002 && sar < dar + 0.002)) {
            return pp_converter_yuv420p_to_bgra_parallel(
                source, destination, destination_pitch_bytes, 6);
        }
    }

    /*
     * Soft UHD → 1080: nearest-neighbor scale via persistent pool.
     * Honors FIT (letterbox), FILL (crop), STRETCH.
     */
    if (source->format == PP_FRAME_YUV420P && source->width >= 2560u &&
        destination_width <= 1920u && destination_height <= 1080u) {
        return pp_converter_yuv420p_scale_nn_aspect(
            source, destination, destination_width, destination_height,
            destination_pitch_bytes, aspect, 4);
    }

    cfg.aspect = aspect;
    cfg.clear_color_bgra = 0xFF000000u;
    return pp_converter_convert_ex(source, destination, destination_width,
                                   destination_height, destination_pitch_bytes, &cfg);
}
