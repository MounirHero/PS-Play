
#define UI_PANEL_FILL      0xDD331A07
#define UI_PANEL_SEL       0xEE662A06
#define UI_LINE_DIM        0xFF4B8FE8
#define UI_LINE_BRIGHT     0xFFFFD819
#define UI_WHITE           0xFFFFFFFF
#define UI_CYAN            0xFF24C8FF

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include "pp_videoout.h"
#include "pp_playback.h"
#include "pp_output_policy.h"
#include "pp_product_path.h"
#include "pp_4k_sdr_policy.h"
#include "pp_4k_product_stage.h"
#include "pp_v8_gate.h"
#include "pp_stage_breadcrumb.h"
#include "pp_frame.h"
#include "pp_converter.h"
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "SDL_ps5tilemap.inc"


#include "assets/renderer_reset_assets.h"
#include <sys/stat.h>
#include "assets/browser_icon_assets.h"
#include "assets/pp_net_icons.h"
#include "net/pp_dlna.h"
#include "net/pp_iptv.h"
#include "net/pp_dmr.h"

#ifndef RR_BGRA
#define RR_BGRA(r,g,b,a) \
    ((((uint32_t)(a)) << 24) | \
     (((uint32_t)(b)) << 16) | \
     (((uint32_t)(g)) << 8)  | \
     ((uint32_t)(r)))
#endif

static void ui_round_asset(uint32_t *fb, int x, int y, int w, int h, int r, uint32_t fill, uint32_t border, uint32_t glow);

int sceAudioOutInit(void);
int sceAudioOutOpen(int userId, int type, int index, unsigned int len, unsigned int freq, unsigned int param);
int sceAudioOutClose(int handle);
int sceAudioOutOutput(int handle, const void *ptr);


#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE
#define WIDTH 1920
#define HEIGHT 1080

/* Product video backend (pp_videoout + converter + clock). */
#define PP_BACKEND_ENABLED 1
static pp_videoout g_pp_vo;
static pp_playback g_pp_pb;
/* VO dimensions may be 1080 (UI) or 4K (V8 playback). */
static uint32_t g_vo_w = 1920;
static uint32_t g_vo_h = 1080;
static pp_video_backend g_pp_backend = PP_BACKEND_1080_STANDARD;
static int g_pp_vo_ready = 0;
/* Progressive 4K product diag (G-H). Set when gated 4K session opens. */
static int g_4k_diag_active = 0;
static int g_4k_suppress_audio = 0; /* stage < G_AUDIO */
static int g_4k_suppress_ui = 0;   /* stage < H_FULL */
/* Defer VO reconfig to frame-loop top (never while a buffer is held). */
static int g_pending_vo_reconfig = 0;
static uint32_t g_pending_vo_w = 1920;
static uint32_t g_pending_vo_h = 1080;
static uint32_t g_pending_vo_buffers = 2;
static pp_video_backend g_pending_backend = PP_BACKEND_1080_STANDARD;
static uint32_t g_pending_out_w = 1920;
static uint32_t g_pending_out_h = 1080;
/* 0 while VO reconfig in flight — video thread must not touch VO */
static volatile int g_vo_decode_gate = 1;
static int g_first_frame_bc_done = 0;

// Prospero Player 1.0 UI state
#define SCREEN_MAIN_MENU 0
#define SCREEN_USB_BROWSER 1
#define SCREEN_PLAYER 2
#define SCREEN_RESUME_PROMPT 3
#define SCREEN_SETTINGS 10
#define SCREEN_PROFILE_SELECT 11
#define SCREEN_RECENT_FILES 12
#define SCREEN_FAVORITES 13
#define SCREEN_ABOUT_SUPPORT 14
#define SCREEN_DEVELOPER_TOOLS 15
#define SCREEN_MEDIA_INFO 16
#define SCREEN_PLAYBACK_FINISHED 17
#define SCREEN_DLNA_SERVERS 20
#define SCREEN_DLNA_BROWSE  21
#define SCREEN_IPTV         22
#define SCREEN_URL_ENTRY    23
#define SCREEN_DMR          24

typedef enum {
    PROFILE_BALANCED = 0,
    PROFILE_PERFORMANCE,
    PROFILE_COMPATIBILITY,
    PROFILE_DEBUG
} PlaybackProfile;

/* Media clock: audio when live, else video (silent / E-AC3 files). */
static double prospero_media_clock_seconds(void);

static PlaybackProfile current_profile = PROFILE_BALANCED;
static int settings_selected = 0;
static int profile_selected = 0;
static int show_debug_overlay = 0;

/* PROSPERO_SETTINGS_EARLY_GLOBALS_START */
static int prospero_resume_playback_enabled = 1;
static int prospero_default_view_mode = 0;
static int prospero_auto_subtitles_enabled = 1;
/* PROSPERO_SETTINGS_EARLY_GLOBALS_END */



typedef struct {
    char path[512];
    char title[128];
    char container[32];
    char video_codec[64];
    char audio_codec[64];
    int width;
    int height;
    double duration_sec;
    long long file_size_bytes;
    int has_video;
    int has_audio;
    int has_subtitles;
} MediaMetadata;

static MediaMetadata current_metadata;

// Forward declarations for Prospero helper blocks placed before original globals.
extern int screen;
extern char current_media_path[512];
extern double resume_base_offset_seconds;
extern double media_duration_sec;
static volatile double audio_clock_seconds;

void toast(const char* title, const char* msg);
void clean_media_title(const char *path, char *title, size_t title_sz, char *meta, size_t meta_sz);
void format_time_mmss(char *out, size_t out_size, double sec);
long long now_ms(void);
static void recent_save(void);
static void format_file_size(char *out, size_t outsz, long long bytes);
const char* profile_name(PlaybackProfile profile);

#if PP_BACKEND_ENABLED
/**
 * Transactional VO reconfiguration (004Q crash isolation).
 * Stops pending presents / purges before destroying old framebuffers.
 */
static int pp_product_reconfigure_vo(uint32_t w, uint32_t h, uint32_t buffers)
{
    char detail[96];
    if (g_pp_vo_ready && g_pp_vo.inited && g_vo_w == w && g_vo_h == h &&
        g_pp_vo.buffer_count == buffers)
        return 0;

    /* Block decode/present against this VO until re-init completes */
    g_vo_decode_gate = 0;
    pp_stage_bc_checkpoint("003_OLD_WORKERS_STOPPED", "no dedicated V8 workers in app");
    /* Block new V8 presents; release any pending acquired buffer */
    if (g_pp_vo_ready) {
        uint32_t pidx = 0;
        uint64_t pfid = 0;
        while (pp_playback_take_pending_present(&g_pp_pb, &pidx, &pfid)) {
            pp_videoout_release(&g_pp_vo, pidx);
        }
        pp_playback_set_backend(&g_pp_pb, PP_BACKEND_1080_STANDARD);
        g_pp_backend = PP_BACKEND_1080_STANDARD;
    }
    pp_stage_bc_checkpoint("004_OLD_QUEUES_PURGED", "pending present cleared");

    if (g_pp_vo_ready) {
        pp_stage_bc_checkpoint("005_OLD_VIDEOOUT_SHUTDOWN", "before destroy");
        /* Ensure no buffer left acquired/in-flight; video thread may still spin */
        usleep(80000);
        pp_videoout_shutdown(&g_pp_vo);
        usleep(20000);
        g_pp_vo_ready = 0;
    }
    memset(&g_pp_vo, 0, sizeof(g_pp_vo));

    snprintf(detail, sizeof(detail), "w=%u h=%u buf=%u", w, h, buffers);
    if (pp_videoout_init(&g_pp_vo, w, h, PP_PIXEL_BGRA32_TILED, buffers) != 0) {
        g_pp_vo_ready = 0;
        pp_stage_bc_checkpoint("006_4K_VIDEOOUT_INIT_FAIL", detail);
        /*
         * PS5 VideoOut only likes a few modes. Nonstandard sizes like
         * 3840x1920 often fail — try standard 3840x2160, then 1080.
         */
        if ((w != 3840u || h != 2160u) && (w > 1920u || h > 1080u)) {
            if (pp_videoout_init(&g_pp_vo, 3840, 2160, PP_PIXEL_BGRA32_TILED,
                                 buffers >= 2 ? buffers : 3) == 0) {
                g_vo_w = 3840;
                g_vo_h = 2160;
                g_pp_vo_ready = 1;
                pp_playback_attach_videoout(&g_pp_pb, &g_pp_vo);
                g_vo_decode_gate = 1;
                toast("PLAYBACK", "4K VO @ 3840x2160");
                pp_stage_bc_checkpoint("006_4K_VIDEOOUT_INIT_OK",
                                      "fallback 3840x2160");
                return 0;
            }
        }
        if (w != 1920u || h != 1080u) {
            if (pp_videoout_init(&g_pp_vo, 1920, 1080, PP_PIXEL_BGRA32_TILED, 2) == 0) {
                g_vo_w = 1920;
                g_vo_h = 1080;
                g_pp_vo_ready = 1;
                pp_playback_attach_videoout(&g_pp_pb, &g_pp_vo);
                pp_playback_set_backend(&g_pp_pb, PP_BACKEND_1080_STANDARD);
                g_pp_backend = PP_BACKEND_1080_STANDARD;
                g_vo_decode_gate = 1;
                /* Friendly — console rejected UHD mode, not a hard crash */
                toast("PLAYBACK", "Present @ 1080 (VO mode)");
                return -1;
            }
        }
        toast("VIDEOOUT", "display init failed");
        return -2;
    }
    g_vo_w = w;
    g_vo_h = h;
    g_pp_vo_ready = 1;
    pp_playback_attach_videoout(&g_pp_pb, &g_pp_vo);
    /* Brief settle after 4K register — first-open race was here */
    if (w >= 3840u)
        usleep(30000);
    g_vo_decode_gate = 1;
    pp_stage_bc_checkpoint("006_4K_VIDEOOUT_INIT_OK", detail);
    return 0;
}

/**
 * Queue VO reconfig for next frame start (safe: no buffer held).
 *
 * CRITICAL: do NOT arm V8/4K backend or 4K output dims until apply succeeds.
 * Arming early made the convert/present path write 4K tiles into a 1080 plane
 * (or draw 1080 UI into a 4K plane) → stacked/split layout on every page.
 */
static void pp_product_request_vo(uint32_t w, uint32_t h, uint32_t buffers,
                                  pp_video_backend backend,
                                  uint32_t out_w, uint32_t out_h)
{
    int size_change;

    g_pending_vo_w = w;
    g_pending_vo_h = h;
    g_pending_vo_buffers = buffers;
    g_pending_backend = backend;
    g_pending_out_w = out_w;
    g_pending_out_h = out_h;
    g_pending_vo_reconfig = 1;

    size_change =
        (w != g_vo_w || h != g_vo_h ||
         buffers != (g_pp_vo_ready ? g_pp_vo.buffer_count : 0));

    if (size_change) {
        /* Transition lock: stay on current safe backend until VO matches. */
        g_vo_decode_gate = 0;
        g_pp_backend = PP_BACKEND_1080_STANDARD;
        pp_playback_set_backend(&g_pp_pb, PP_BACKEND_1080_STANDARD);
        /* Keep output dims matching live VO so convert never 4K→1080 plane. */
        pp_playback_set_output(&g_pp_pb,
                               g_vo_w ? g_vo_w : (uint32_t)WIDTH,
                               g_vo_h ? g_vo_h : (uint32_t)HEIGHT,
                               PP_ASPECT_FIT);
    } else {
        g_pp_backend = backend;
        pp_playback_set_backend(&g_pp_pb, backend);
        pp_playback_set_output(&g_pp_pb, out_w, out_h, PP_ASPECT_FIT);
    }
    {
        char d[96];
        snprintf(d, sizeof(d), "req %ux%u buf=%u be=%d chg=%d",
                 w, h, buffers, (int)backend, size_change);
        pp_stage_bc_checkpoint("005B_VO_RECONFIG_QUEUED", d);
    }
}

static void pp_product_apply_pending_vo(void)
{
    if (!g_pending_vo_reconfig)
        return;
    g_pending_vo_reconfig = 0;
    if (pp_product_reconfigure_vo(g_pending_vo_w, g_pending_vo_h,
                                  g_pending_vo_buffers) != 0) {
        g_pp_backend = PP_BACKEND_1080_STANDARD;
        pp_playback_set_backend(&g_pp_pb, g_pp_backend);
        pp_playback_set_output(&g_pp_pb, WIDTH, HEIGHT, PP_ASPECT_FIT);
        g_4k_diag_active = 0;
        g_4k_suppress_audio = 0;
        g_4k_suppress_ui = 0;
        g_vo_decode_gate = 1;
        toast("PLAYBACK", "Present @ 1080 (VO mode)");
        pp_stage_bc_checkpoint("006_4K_VIDEOOUT_INIT_FAIL", "pending apply");
        return;
    }
    /*
     * Arm backend after VO is live. If we requested a nonstandard size but
     * init fell back to 3840x2160, keep V8 only when source is full UHD
     * 1:1; otherwise V3 letterbox/fit into the live VO size.
     */
    if (g_vo_w == 3840u && g_vo_h == 2160u &&
        g_pending_out_w == 3840u && g_pending_out_h == 2160u &&
        g_pending_backend == PP_BACKEND_4K_V8_FUSED) {
        g_pp_backend = PP_BACKEND_4K_V8_FUSED;
        pp_playback_set_backend(&g_pp_pb, g_pp_backend);
        pp_playback_set_output(&g_pp_pb, 3840, 2160, PP_ASPECT_FIT);
    } else if (g_vo_w >= 3200u && g_vo_h >= 1600u &&
               g_pending_backend != PP_BACKEND_1080_STANDARD) {
        /* Live UHD VO but source not 1:1 — V3 fit/letterbox */
        g_pp_backend = PP_BACKEND_4K_V3_FALLBACK;
        pp_playback_set_backend(&g_pp_pb, g_pp_backend);
        pp_playback_set_output(&g_pp_pb, g_vo_w, g_vo_h, PP_ASPECT_FIT);
    } else {
        g_pp_backend = g_pending_backend;
        pp_playback_set_backend(&g_pp_pb, g_pp_backend);
        pp_playback_set_output(&g_pp_pb, g_pending_out_w, g_pending_out_h,
                               PP_ASPECT_FIT);
    }
    g_vo_decode_gate = 1;
    pp_stage_bc_checkpoint("006B_VO_RECONFIG_APPLIED",
                          pp_4k_stage_name(PP_4K_PRODUCT_STAGE));
}

/** True only when player is allowed to keep a native UHD surface. */
static int pp_product_k4_live(int scr)
{
    return (scr == SCREEN_PLAYER &&
            (g_pp_backend == PP_BACKEND_4K_V8_FUSED ||
             g_pp_backend == PP_BACKEND_4K_V3_FALLBACK) &&
            g_pp_vo_ready && g_pp_vo.inited &&
            g_vo_w == g_pp_vo.width && g_vo_h == g_pp_vo.height &&
            pp_v8_is_uhd_size(g_vo_w, g_vo_h));
}

/**
 * Force 1080 UI surface. Call only with no buffer held.
 * Returns 1 if VO is safe for 1080 UI draw, 0 if still wrong (skip frame).
 */
static int pp_product_ensure_ui_1080(void)
{
    if (g_vo_w == 1920u && g_vo_h == 1080u &&
        g_pp_backend == PP_BACKEND_1080_STANDARD &&
        g_pp_vo_ready && g_pp_vo.inited &&
        g_pp_vo.width == 1920u && g_pp_vo.height == 1080u &&
        !g_pending_vo_reconfig)
        return 1;

    g_pp_backend = PP_BACKEND_1080_STANDARD;
    pp_playback_set_backend(&g_pp_pb, PP_BACKEND_1080_STANDARD);
    if (g_vo_w != 1920u || g_vo_h != 1080u ||
        !g_pp_vo_ready || g_pp_vo.buffer_count != 2 ||
        g_pp_vo.width != 1920u || g_pp_vo.height != 1080u) {
        pp_product_request_vo(1920, 1080, 2, PP_BACKEND_1080_STANDARD,
                              WIDTH, HEIGHT);
        pp_product_apply_pending_vo();
    } else if (g_pending_vo_reconfig) {
        pp_product_apply_pending_vo();
    }

    return (g_vo_w == 1920u && g_vo_h == 1080u &&
            g_pp_vo_ready && g_pp_vo.inited &&
            g_pp_vo.width == 1920u && g_pp_vo.height == 1080u);
}
#endif



#define RECENT_FILE_DB "/mnt/usb0/prospero_recent.txt"
#define MAX_RECENT_FILES 25

typedef struct {
    char path[512];
    char title[128];
    double last_pos;
    double duration;
    long long last_played;
} RecentFileEntry;

static RecentFileEntry recent_files[MAX_RECENT_FILES];
static int recent_file_count = 0;
static int recent_selected = 0;


#define FAVORITES_FILE_DB "/mnt/usb0/prospero_favorites.txt"
#define MAX_FAVORITES 100

typedef struct {
    char path[512];
    char title[128];
    double duration;
} FavoriteEntry;

static FavoriteEntry favorite_files[MAX_FAVORITES];
static int favorite_count = 0;
static int favorite_selected = 0;

static void favorites_clear(void) {
    memset(favorite_files, 0, sizeof(favorite_files));
    favorite_count = 0;
    favorite_selected = 0;
}

static int favorites_find(const char *path) {
    if (!path || !path[0]) return -1;

    for (int i = 0; i < favorite_count; i++) {
        if (strcmp(favorite_files[i].path, path) == 0) return i;
    }

    return -1;
}

static int favorites_is_favorite(const char *path) {
    return favorites_find(path) >= 0;
}

static void favorites_add(const char *path, const char *title, double duration) {
    if (!path || !path[0]) return;
    if (favorites_find(path) >= 0) return;
    if (favorite_count >= MAX_FAVORITES) return;

    FavoriteEntry *e = &favorite_files[favorite_count];
    memset(e, 0, sizeof(*e));

    snprintf(e->path, sizeof(e->path), "%s", path);
    snprintf(e->title, sizeof(e->title), "%s", title && title[0] ? title : path);
    e->duration = duration;

    favorite_count++;
}

static void favorites_remove(const char *path) {
    int idx = favorites_find(path);
    if (idx < 0) return;

    for (int i = idx; i < favorite_count - 1; i++) {
        favorite_files[i] = favorite_files[i + 1];
    }

    if (favorite_count > 0) favorite_count--;

    if (favorite_selected >= favorite_count) {
        favorite_selected = favorite_count > 0 ? favorite_count - 1 : 0;
    }
}

static void favorites_save(void) {
    FILE *fp = fopen(FAVORITES_FILE_DB, "w");
    if (!fp) return;

    for (int i = 0; i < favorite_count; i++) {
        fprintf(fp, "%s\t%s\t%.3f\n",
            favorite_files[i].path,
            favorite_files[i].title,
            favorite_files[i].duration
        );
    }

    fclose(fp);
}

static void favorites_load(void) {
    favorites_clear();

    FILE *fp = fopen(FAVORITES_FILE_DB, "r");
    if (!fp) return;

    char line[1024];

    while (fgets(line, sizeof(line), fp) && favorite_count < MAX_FAVORITES) {
        FavoriteEntry *e = &favorite_files[favorite_count];
        memset(e, 0, sizeof(*e));

        char *path = strtok(line, "\t");
        char *title = strtok(NULL, "\t");
        char *duration = strtok(NULL, "\t\r\n");

        if (!path || !title) continue;

        snprintf(e->path, sizeof(e->path), "%s", path);
        snprintf(e->title, sizeof(e->title), "%s", title);
        e->duration = duration ? atof(duration) : 0.0;

        favorite_count++;
    }

    fclose(fp);
}


/* ==========================================================================
 * SMART RANKING FEATURES
 * 1) Cover art (sidecar images) for browser / recent / favorites
 * 2) Subtitle delay (L2 / R3)
 * 3) Chapters (L1 / R1 jump when present)
 * 4) Progress bars on recent / favorites
 * ========================================================================== */

/* Small cache kept for optional use; list rows use stock icons again. */
#define PROSPERO_COVER_W 80
#define PROSPERO_COVER_H 80
#define PROSPERO_COVER_CACHE 48

/* Large selection preview (right panel) — Infuse/BFplayer-style */
#define PROSPERO_BROWSER_PREVIEW_W 400
#define PROSPERO_BROWSER_PREVIEW_H 225

typedef struct {
    char path_key[512];
    uint32_t pixels[PROSPERO_COVER_W * PROSPERO_COVER_H];
    int valid;
    int tried; /* 1 if we already looked (hit or miss) */
} ProsperoCoverEntry;

static ProsperoCoverEntry prospero_cover_cache[PROSPERO_COVER_CACHE];
static int prospero_cover_clock = 0;

static uint32_t prospero_browser_preview_pixels[
    PROSPERO_BROWSER_PREVIEW_W * PROSPERO_BROWSER_PREVIEW_H];
static char prospero_browser_preview_path[768];
static int prospero_browser_preview_valid = 0;
static int prospero_browser_preview_failed = 0;

static int prospero_subtitle_delay_ms = 0;

#define PROSPERO_CHAPTER_LIMIT 128
static int prospero_chapter_count = 0;
static double prospero_chapter_start[PROSPERO_CHAPTER_LIMIT];
static char prospero_chapter_title[PROSPERO_CHAPTER_LIMIT][48];

static int prospero_path_is_readable(const char *path)
{
    if (!path || !path[0])
        return 0;
    return access(path, R_OK) == 0;
}

static void prospero_cover_scale_rgba(
    const unsigned char *src,
    int sw,
    int sh,
    uint32_t *dst,
    int dw,
    int dh
) {
    int y, x;
    if (!src || !dst || sw < 1 || sh < 1 || dw < 1 || dh < 1)
        return;
    for (y = 0; y < dh; y++) {
        int sy = y * sh / dh;
        if (sy >= sh)
            sy = sh - 1;
        for (x = 0; x < dw; x++) {
            int sx = x * sw / dw;
            const unsigned char *p;
            unsigned char r, g, b;
            if (sx >= sw)
                sx = sw - 1;
            p = src + ((size_t)sy * (size_t)sw + (size_t)sx) * 4u;
            /* stbi/sws RGBA → same packing as RR_BGRA / load_stb_image */
            r = p[0];
            g = p[1];
            b = p[2];
            dst[y * dw + x] = RR_BGRA(r, g, b, 255);
        }
    }
}

/*
 * Resolve cover image for a media file or folder.
 * Order: basename.jpg/png, cover.jpg, poster.jpg, folder.jpg (dir).
 */
static int prospero_cover_resolve_path(
    const char *media_path,
    int is_directory,
    char *out,
    size_t out_size
) {
    char dir[512];
    char base[256];
    const char *slash;
    const char *dot;
    size_t dir_len;
    size_t base_len;
    int i;
    static const char *sidecar_ext[] = {
        ".jpg", ".jpeg", ".png", ".JPG", ".JPEG", ".PNG", NULL
    };
    static const char *folder_names[] = {
        "folder.jpg", "folder.png", "poster.jpg", "poster.png",
        "cover.jpg", "cover.png", "Folder.jpg", "Poster.jpg", NULL
    };

    if (!media_path || !media_path[0] || !out || out_size < 8)
        return 0;
    out[0] = 0;

    slash = strrchr(media_path, '/');
    if (!slash || slash == media_path)
        return 0;

    dir_len = (size_t)(slash - media_path);
    if (dir_len >= sizeof(dir))
        return 0;
    memcpy(dir, media_path, dir_len);
    dir[dir_len] = 0;

    if (is_directory) {
        for (i = 0; folder_names[i]; i++) {
            snprintf(out, out_size, "%s/%s", media_path, folder_names[i]);
            if (prospero_path_is_readable(out))
                return 1;
        }
        /* parent-style names inside the folder already tried */
        return 0;
    }

    snprintf(base, sizeof(base), "%s", slash + 1);
    dot = strrchr(base, '.');
    if (dot && dot != base)
        base[dot - base] = 0;
    base_len = strlen(base);
    if (base_len == 0)
        return 0;

    for (i = 0; sidecar_ext[i]; i++) {
        snprintf(out, out_size, "%s/%s%s", dir, base, sidecar_ext[i]);
        if (prospero_path_is_readable(out))
            return 1;
    }
    for (i = 0; folder_names[i]; i++) {
        snprintf(out, out_size, "%s/%s", dir, folder_names[i]);
        if (prospero_path_is_readable(out))
            return 1;
    }
    return 0;
}

static ProsperoCoverEntry *prospero_cover_find_slot(const char *key)
{
    int i;
    int free_i = -1;
    int oldest_i = 0;
    int oldest_clk = 0x7fffffff;

    if (!key || !key[0])
        return NULL;

    for (i = 0; i < PROSPERO_COVER_CACHE; i++) {
        if (prospero_cover_cache[i].tried &&
            strcmp(prospero_cover_cache[i].path_key, key) == 0)
            return &prospero_cover_cache[i];
        if (!prospero_cover_cache[i].tried && free_i < 0)
            free_i = i;
    }
    if (free_i >= 0)
        return &prospero_cover_cache[free_i];

    /* Evict oldest simple slot */
    for (i = 0; i < PROSPERO_COVER_CACHE; i++) {
        /* reuse first invalid tried entry as LRU-ish */
        if (!prospero_cover_cache[i].valid) {
            free_i = i;
            break;
        }
        if (i < oldest_clk) {
            oldest_clk = i;
            oldest_i = i;
        }
    }
    return &prospero_cover_cache[free_i >= 0 ? free_i : oldest_i];
}

static int prospero_cover_path_is_video(const char *path)
{
    const char *dot;
    if (!path || !path[0])
        return 0;
    dot = strrchr(path, '.');
    if (!dot || !dot[1])
        return 0;
    return (
        strcasecmp(dot, ".mkv") == 0 ||
        strcasecmp(dot, ".mp4") == 0 ||
        strcasecmp(dot, ".mov") == 0 ||
        strcasecmp(dot, ".m4v") == 0 ||
        strcasecmp(dot, ".avi") == 0 ||
        strcasecmp(dot, ".webm") == 0 ||
        strcasecmp(dot, ".ts") == 0 ||
        strcasecmp(dot, ".m2ts") == 0 ||
        strcasecmp(dot, ".mpg") == 0 ||
        strcasecmp(dot, ".mpeg") == 0
    );
}

/*
 * Decode one video frame near ~10% / 5s and scale to out_w x out_h.
 * Used for large right-panel preview (and small cover fallback).
 * Returns 0 on success.
 */
static int prospero_cover_extract_video_frame_size(
    const char *media_path,
    uint32_t *out_pixels,
    int out_w,
    int out_h
) {
    AVFormatContext *fmt = NULL;
    AVCodecContext *ctx = NULL;
    const AVCodec *dec = NULL;
    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    struct SwsContext *sws = NULL;
    AVDictionary *opts = NULL;
    int vstream = -1;
    unsigned int i;
    int got = 0;
    int packets = 0;
    double duration = 0.0;
    double seek_sec = 5.0;
    int64_t seek_ts;
    uint8_t *rgba = NULL;
    int rgba_stride;

    if (!media_path || !out_pixels || out_w < 2 || out_h < 2)
        return -1;

    /* Fast probe — avoid multi-second analyze on huge WEB-DLs */
    av_dict_set(&opts, "probesize", "1048576", 0);
    av_dict_set(&opts, "analyzeduration", "1000000", 0);

    if (avformat_open_input(&fmt, media_path, NULL, &opts) < 0) {
        av_dict_free(&opts);
        return -2;
    }
    av_dict_free(&opts);

    if (avformat_find_stream_info(fmt, NULL) < 0)
        goto fail;

    for (i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar &&
            fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vstream = (int)i;
            break;
        }
    }
    if (vstream < 0)
        goto fail;

    if (fmt->duration > 0)
        duration = (double)fmt->duration / (double)AV_TIME_BASE;
    if (duration > 1.0) {
        seek_sec = duration * 0.10;
        if (seek_sec < 2.0)
            seek_sec = 2.0;
        if (seek_sec > 30.0)
            seek_sec = 30.0;
        if (seek_sec > duration - 1.0)
            seek_sec = duration * 0.5;
    } else {
        seek_sec = 1.0;
    }

    {
        AVStream *st = fmt->streams[vstream];
        AVCodecParameters *par = st->codecpar;

        dec = avcodec_find_decoder(par->codec_id);
        if (!dec)
            goto fail;
        ctx = avcodec_alloc_context3(dec);
        if (!ctx)
            goto fail;
        if (avcodec_parameters_to_context(ctx, par) < 0)
            goto fail;
        ctx->thread_count = 1;
#ifdef AV_CODEC_FLAG2_FAST
        ctx->flags2 |= AV_CODEC_FLAG2_FAST;
#endif
        if (avcodec_open2(ctx, dec, NULL) < 0)
            goto fail;

        seek_ts = (int64_t)(seek_sec / av_q2d(st->time_base));
        if (seek_ts < 0)
            seek_ts = 0;
        (void)av_seek_frame(fmt, vstream, seek_ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(ctx);
    }

    frame = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!frame || !pkt)
        goto fail;

    while (packets < 48 && av_read_frame(fmt, pkt) >= 0) {
        packets++;
        if (pkt->stream_index != vstream) {
            av_packet_unref(pkt);
            continue;
        }
        if (avcodec_send_packet(ctx, pkt) == 0) {
            while (avcodec_receive_frame(ctx, frame) == 0) {
                got = 1;
                break;
            }
        }
        av_packet_unref(pkt);
        if (got)
            break;
    }

    if (!got) {
        avcodec_send_packet(ctx, NULL);
        if (avcodec_receive_frame(ctx, frame) == 0)
            got = 1;
    }

    if (!got || !frame->width || !frame->height)
        goto fail;

    /*
     * Match scrub-thumbnail path: sws straight into pixel buffer as RGBA
     * bytes. On LE this is the same layout as RR_BGRA (R in low byte).
     * Do NOT re-swizzle — that was producing a yellow cast on previews.
     */
    sws = sws_getContext(
        frame->width,
        frame->height,
        (enum AVPixelFormat)frame->format,
        out_w,
        out_h,
        AV_PIX_FMT_RGBA,
        SWS_FAST_BILINEAR,
        NULL,
        NULL,
        NULL);
    if (!sws)
        goto fail;

    {
        uint8_t *dst[4] = { (uint8_t *)out_pixels, NULL, NULL, NULL };
        int dst_ls[4] = { out_w * 4, 0, 0, 0 };
        sws_scale(
            sws,
            (const uint8_t * const *)frame->data,
            frame->linesize,
            0,
            frame->height,
            dst,
            dst_ls);
    }

    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return 0;

fail:
    free(rgba);
    if (sws)
        sws_freeContext(sws);
    if (frame)
        av_frame_free(&frame);
    if (pkt)
        av_packet_free(&pkt);
    if (ctx)
        avcodec_free_context(&ctx);
    if (fmt)
        avformat_close_input(&fmt);
    return -3;
}

static int prospero_cover_extract_video_frame(
    const char *media_path,
    uint32_t *out_pixels
) {
    return prospero_cover_extract_video_frame_size(
        media_path, out_pixels, PROSPERO_COVER_W, PROSPERO_COVER_H);
}

/*
 * Load large preview for the currently highlighted row.
 * Prefer sidecar image; else video frame. Cached per path.
 */
static void prospero_browser_preview_ensure(
    const char *media_path,
    int is_directory
) {
    char image_path[768];
    int w = 0, h = 0, ch = 0;
    unsigned char *data;

    if (!media_path || !media_path[0]) {
        prospero_browser_preview_valid = 0;
        prospero_browser_preview_path[0] = 0;
        return;
    }

    if (prospero_browser_preview_valid &&
        strcmp(prospero_browser_preview_path, media_path) == 0)
        return;

    if (prospero_browser_preview_failed &&
        strcmp(prospero_browser_preview_path, media_path) == 0 &&
        !prospero_browser_preview_valid)
        return;

    prospero_browser_preview_valid = 0;
    prospero_browser_preview_failed = 0;
    snprintf(
        prospero_browser_preview_path,
        sizeof(prospero_browser_preview_path),
        "%s",
        media_path);

    if (prospero_cover_resolve_path(
            media_path, is_directory, image_path, sizeof(image_path))) {
        data = stbi_load(image_path, &w, &h, &ch, 4);
        if (data && w >= 2 && h >= 2) {
            prospero_cover_scale_rgba(
                data,
                w,
                h,
                prospero_browser_preview_pixels,
                PROSPERO_BROWSER_PREVIEW_W,
                PROSPERO_BROWSER_PREVIEW_H);
            stbi_image_free(data);
            prospero_browser_preview_valid = 1;
            return;
        }
        if (data)
            stbi_image_free(data);
    }

    if (!is_directory &&
        prospero_cover_path_is_video(media_path) &&
        prospero_cover_extract_video_frame_size(
            media_path,
            prospero_browser_preview_pixels,
            PROSPERO_BROWSER_PREVIEW_W,
            PROSPERO_BROWSER_PREVIEW_H) == 0) {
        prospero_browser_preview_valid = 1;
        return;
    }

    /* Image files: load directly */
    if (!is_directory) {
        const char *dot = strrchr(media_path, '.');
        if (dot &&
            (strcasecmp(dot, ".jpg") == 0 ||
             strcasecmp(dot, ".jpeg") == 0 ||
             strcasecmp(dot, ".png") == 0 ||
             strcasecmp(dot, ".bmp") == 0 ||
             strcasecmp(dot, ".webp") == 0)) {
            data = stbi_load(media_path, &w, &h, &ch, 4);
            if (data && w >= 2 && h >= 2) {
                prospero_cover_scale_rgba(
                    data,
                    w,
                    h,
                    prospero_browser_preview_pixels,
                    PROSPERO_BROWSER_PREVIEW_W,
                    PROSPERO_BROWSER_PREVIEW_H);
                stbi_image_free(data);
                prospero_browser_preview_valid = 1;
                return;
            }
            if (data)
                stbi_image_free(data);
        }
    }

    prospero_browser_preview_failed = 1;
}

/* Right-side preview panel (highlighted file only). */
static void prospero_browser_preview_draw_panel(
    uint32_t *fb,
    int panel_x,
    int panel_y
) {
    const int fb_w = 1920;
    const int fb_h = 1080;
    const int pad = 14;
    const int box_w = PROSPERO_BROWSER_PREVIEW_W + pad * 2;
    const int box_h = PROSPERO_BROWSER_PREVIEW_H + pad * 2 + 48;
    int row, col;
    int img_x, img_y;

    if (!fb)
        return;

    {
        /* Must use RR_BGRA — raw 0xFF00D7FF is YELLOW (R high, B low) on this FB */
        const uint32_t col_bg = RR_BGRA(35,35,35,224);
        const uint32_t col_border = RR_BGRA(205,205,205,255);
        const uint32_t col_empty = RR_BGRA(34,34,34,255);

        for (row = 0; row < box_h; row++) {
            int py = panel_y + row;
            if (py < 0 || py >= fb_h)
                continue;
            for (col = 0; col < box_w; col++) {
                int px = panel_x + col;
                if (px < 0 || px >= fb_w)
                    continue;
                fb[py * fb_w + px] = col_bg;
            }
        }

        for (col = 0; col < box_w; col++) {
            int px = panel_x + col;
            int py0 = panel_y;
            int py1 = panel_y + box_h - 1;
            if (px >= 0 && px < fb_w) {
                if (py0 >= 0 && py0 < fb_h)
                    fb[py0 * fb_w + px] = col_border;
                if (py1 >= 0 && py1 < fb_h)
                    fb[py1 * fb_w + px] = col_border;
            }
        }
        for (row = 0; row < box_h; row++) {
            int py = panel_y + row;
            int px0 = panel_x;
            int px1 = panel_x + box_w - 1;
            if (py >= 0 && py < fb_h) {
                if (px0 >= 0 && px0 < fb_w)
                    fb[py * fb_w + px0] = col_border;
                if (px1 >= 0 && px1 < fb_w)
                    fb[py * fb_w + px1] = col_border;
            }
        }

        img_x = panel_x + pad;
        img_y = panel_y + pad;

        if (prospero_browser_preview_valid) {
            for (row = 0; row < PROSPERO_BROWSER_PREVIEW_H; row++) {
                int py = img_y + row;
                if (py < 0 || py >= fb_h)
                    continue;
                for (col = 0; col < PROSPERO_BROWSER_PREVIEW_W; col++) {
                    int px = img_x + col;
                    if (px < 0 || px >= fb_w)
                        continue;
                    fb[py * fb_w + px] =
                        prospero_browser_preview_pixels[
                            row * PROSPERO_BROWSER_PREVIEW_W + col];
                }
            }
        } else {
            for (row = 0; row < PROSPERO_BROWSER_PREVIEW_H; row++) {
                int py = img_y + row;
                if (py < 0 || py >= fb_h)
                    continue;
                for (col = 0; col < PROSPERO_BROWSER_PREVIEW_W; col++) {
                    int px = img_x + col;
                    if (px < 0 || px >= fb_w)
                        continue;
                    fb[py * fb_w + px] = col_empty;
                }
            }
        }
    }
    /* Caption drawn by caller via rr_text (defined later). */
}

static const ProsperoCoverEntry *prospero_cover_get(
    const char *media_path,
    int is_directory
) {
    ProsperoCoverEntry *slot;
    char image_path[768];
    int w = 0, h = 0, ch = 0;
    unsigned char *data;

    if (!media_path || !media_path[0])
        return NULL;

    slot = prospero_cover_find_slot(media_path);
    if (!slot)
        return NULL;

    if (slot->tried && strcmp(slot->path_key, media_path) == 0)
        return slot->valid ? slot : NULL;

    memset(slot, 0, sizeof(*slot));
    snprintf(slot->path_key, sizeof(slot->path_key), "%s", media_path);
    slot->tried = 1;
    prospero_cover_clock++;

    /* 1) Sidecar image: Movie.jpg / poster.jpg / folder.jpg */
    if (prospero_cover_resolve_path(
            media_path, is_directory, image_path, sizeof(image_path))) {
        data = stbi_load(image_path, &w, &h, &ch, 4);
        if (data && w >= 2 && h >= 2) {
            prospero_cover_scale_rgba(
                data, w, h, slot->pixels, PROSPERO_COVER_W, PROSPERO_COVER_H);
            stbi_image_free(data);
            slot->valid = 1;
            return slot;
        }
        if (data)
            stbi_image_free(data);
    }

    /* 2) No image file: grab a frame from the video itself */
    if (!is_directory &&
        prospero_cover_path_is_video(media_path) &&
        prospero_cover_extract_video_frame(media_path, slot->pixels) == 0) {
        slot->valid = 1;
        return slot;
    }

    return NULL;
}

static int prospero_cover_blit(
    uint32_t *fb,
    int x,
    int y,
    const char *media_path,
    int is_directory
) {
    const ProsperoCoverEntry *cover;
    int row, col;
    const int fb_w = 1920;
    const int fb_h = 1080;

    if (!fb || !media_path)
        return 0;

    cover = prospero_cover_get(media_path, is_directory);
    if (!cover || !cover->valid)
        return 0;

    /* Thin cyan frame (RR_BGRA — not raw 0xFF00D7FF which reads as yellow) */
    for (row = -2; row < PROSPERO_COVER_H + 2; row++) {
        for (col = -2; col < PROSPERO_COVER_W + 2; col++) {
            int px = x + col;
            int py = y + row;
            int edge = (row < 0 || row >= PROSPERO_COVER_H ||
                        col < 0 || col >= PROSPERO_COVER_W);
            if (!edge || px < 0 || py < 0 || px >= fb_w || py >= fb_h)
                continue;
            fb[py * fb_w + px] = RR_BGRA(205,205,205,255);
        }
    }

    for (row = 0; row < PROSPERO_COVER_H; row++) {
        int py = y + row;
        if (py < 0 || py >= fb_h)
            continue;
        for (col = 0; col < PROSPERO_COVER_W; col++) {
            int px = x + col;
            if (px < 0 || px >= fb_w)
                continue;
            fb[py * fb_w + px] =
                cover->pixels[row * PROSPERO_COVER_W + col];
        }
    }
    return 1;
}

static void prospero_draw_progress_bar(
    uint32_t *fb,
    int x,
    int y,
    int w,
    int h,
    double pos,
    double dur
) {
    int filled;
    int i, j;
    const int fb_w = 1920;
    const int fb_h = 1080;
    double t;

    if (!fb || w < 4 || h < 2 || dur < 1.0)
        return;
    t = pos / dur;
    if (t < 0.0)
        t = 0.0;
    if (t > 1.0)
        t = 1.0;
    filled = (int)(t * (double)w + 0.5);
    if (filled < 0)
        filled = 0;
    if (filled > w)
        filled = w;

    for (j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= fb_h)
            continue;
        for (i = 0; i < w; i++) {
            int px = x + i;
            uint32_t c;
            if (px < 0 || px >= fb_w)
                continue;
            c = (i < filled)
                    ? RR_BGRA(205,205,205,255)
                    : RR_BGRA(48,48,48,176);
            fb[py * fb_w + px] = c;
        }
    }
}

static double prospero_recent_lookup(
    const char *path,
    double *out_duration
) {
    int i;
    if (out_duration)
        *out_duration = 0.0;
    if (!path || !path[0])
        return 0.0;
    for (i = 0; i < recent_file_count; i++) {
        if (strcmp(recent_files[i].path, path) == 0) {
            if (out_duration)
                *out_duration = recent_files[i].duration;
            return recent_files[i].last_pos;
        }
    }
    return 0.0;
}

static void prospero_chapters_clear(void)
{
    prospero_chapter_count = 0;
    memset(prospero_chapter_start, 0, sizeof(prospero_chapter_start));
    memset(prospero_chapter_title, 0, sizeof(prospero_chapter_title));
}

static void prospero_chapters_load(AVFormatContext *fmt)
{
    unsigned int i;

    prospero_chapters_clear();
    if (!fmt || fmt->nb_chapters <= 0)
        return;

    for (i = 0;
         i < fmt->nb_chapters &&
         prospero_chapter_count < PROSPERO_CHAPTER_LIMIT;
         i++) {
        AVChapter *ch = fmt->chapters[i];
        const AVDictionaryEntry *title;
        double start;
        int idx;

        if (!ch)
            continue;
        start = ch->start * av_q2d(ch->time_base);
        if (start < 0.0)
            start = 0.0;

        idx = prospero_chapter_count;
        prospero_chapter_start[idx] = start;

        title = av_dict_get(ch->metadata, "title", NULL, 0);
        if (title && title->value && title->value[0]) {
            snprintf(
                prospero_chapter_title[idx],
                sizeof(prospero_chapter_title[idx]),
                "%s",
                title->value);
        } else {
            snprintf(
                prospero_chapter_title[idx],
                sizeof(prospero_chapter_title[idx]),
                "Chapter %d",
                idx + 1);
        }
        prospero_chapter_count++;
    }
}

static int prospero_chapter_index_at(double position)
{
    int i;
    int best = -1;
    for (i = 0; i < prospero_chapter_count; i++) {
        if (prospero_chapter_start[i] <= position + 0.05)
            best = i;
        else
            break;
    }
    return best;
}

/* chapter_jump + subtitle delay nudge live near scrub (need player state) */

#define COMPAT_REPORT_PATH "/mnt/usb0/prospero_compat_report.txt"

static void write_compatibility_report(void) {
    FILE *fp = fopen(COMPAT_REPORT_PATH, "w");
    if (!fp) {
        toast("REPORT", "Save failed");
        return;
    }

    char sizebuf[64];
    char dur[32];
    char posbuf[32];

    double pos =
        resume_base_offset_seconds +
        prospero_media_clock_seconds();
    if (pos < 0.0) pos = 0.0;

    format_file_size(sizebuf, sizeof(sizebuf), current_metadata.file_size_bytes);
    format_time_mmss(dur, sizeof(dur), current_metadata.duration_sec);
    format_time_mmss(posbuf, sizeof(posbuf), pos);

    fprintf(fp, "PS Play Compatibility Report\n");
    fprintf(fp, "====================================\n\n");

    fprintf(fp, "Build\n");
    fprintf(fp, "VERSION 1.0\n\n");

    fprintf(fp, "Playback Profile\n");
    fprintf(fp, "%s\n\n", profile_name(current_profile));

    fprintf(fp, "File\n");
    fprintf(fp, "%s\n\n", current_metadata.path);

    fprintf(fp, "Title\n");
    fprintf(fp, "%s\n\n", current_metadata.title);

    fprintf(fp, "Container\n");
    fprintf(fp, "%s\n\n", current_metadata.container);

    fprintf(fp, "Video Codec\n");
    fprintf(fp, "%s\n\n", current_metadata.video_codec);

    fprintf(fp, "Resolution\n");
    fprintf(fp, "%d x %d\n\n", current_metadata.width, current_metadata.height);

    fprintf(fp, "Audio Codec\n");
    fprintf(fp, "%s\n\n", current_metadata.audio_codec);

    fprintf(fp, "Subtitles\n");
    fprintf(fp, "%s\n\n", current_metadata.has_subtitles ? "Yes" : "No");

    fprintf(fp, "Duration\n");
    fprintf(fp, "%s\n\n", dur);

    fprintf(fp, "Current Position\n");
    fprintf(fp, "%s\n\n", posbuf);

    fprintf(fp, "File Size\n");
    fprintf(fp, "%s\n\n", sizebuf);

    fprintf(fp, "Recent DB Entries\n");
    fprintf(fp, "%d\n\n", recent_file_count);

    fprintf(fp, "Favorites Entries\n");
    fprintf(fp, "%d\n\n", favorite_count);

    fprintf(fp, "Playback State\n");
    fprintf(fp, "%s\n\n", screen == 2 ? "Playing Screen" : "Not On Player Screen");

    fprintf(fp, "Notes\n");
    fprintf(fp, "Generated locally from PS Play metadata and playback state.\n");

    fclose(fp);
    toast("REPORT", "Saved to USB");
}


static void favorites_toggle_current_media(void) {
    if (!current_media_path[0]) return;

    if (favorites_is_favorite(current_media_path)) {
        favorites_remove(current_media_path);
        favorites_save();
        toast("FAVORITES", "Removed");
    } else {
        char title[128];
        char meta[96];
        clean_media_title(current_media_path, title, sizeof(title), meta, sizeof(meta));

        favorites_add(current_media_path, title, media_duration_sec);
        favorites_save();
        toast("FAVORITES", "Added");
    }
}



static void recent_clear(void) {
    memset(recent_files, 0, sizeof(recent_files));
    recent_file_count = 0;
    recent_selected = 0;
}

static void recent_add_or_update(const char *path, const char *title, double last_pos, double duration) {
    if (!path || !path[0]) return;

    int existing = -1;
    for (int i = 0; i < recent_file_count; i++) {
        if (strcmp(recent_files[i].path, path) == 0) {
            existing = i;
            break;
        }
    }

    RecentFileEntry entry;
    memset(&entry, 0, sizeof(entry));
    snprintf(entry.path, sizeof(entry.path), "%s", path);
    snprintf(entry.title, sizeof(entry.title), "%s", title && title[0] ? title : path);
    entry.last_pos = last_pos;
    entry.duration = duration;
    entry.last_played = now_ms();

    if (existing >= 0) {
        for (int i = existing; i > 0; i--) {
            recent_files[i] = recent_files[i - 1];
        }
        recent_files[0] = entry;
    } else {
        int limit = recent_file_count;
        if (limit >= MAX_RECENT_FILES) limit = MAX_RECENT_FILES - 1;

        for (int i = limit; i > 0; i--) {
            recent_files[i] = recent_files[i - 1];
        }

        recent_files[0] = entry;

        if (recent_file_count < MAX_RECENT_FILES) {
            recent_file_count++;
        }
    }

    recent_selected = 0;
}


static void recent_update_current_position(void) {
    if (!current_media_path[0]) return;

    double pos =
        resume_base_offset_seconds +
        prospero_media_clock_seconds();
    if (pos < 0.0) pos = 0.0;

    char title[128];
    char meta[96];
    clean_media_title(current_media_path, title, sizeof(title), meta, sizeof(meta));

    recent_add_or_update(current_media_path, title, pos, media_duration_sec);
    recent_save();
}

static void recent_save(void) {
    FILE *fp = fopen(RECENT_FILE_DB, "w");
    if (!fp) return;

    for (int i = 0; i < recent_file_count; i++) {
        fprintf(fp, "%s\t%s\t%.3f\t%.3f\t%lld\n",
            recent_files[i].path,
            recent_files[i].title,
            recent_files[i].last_pos,
            recent_files[i].duration,
            recent_files[i].last_played
        );
    }

    fclose(fp);
}

static void recent_load(void) {
    recent_clear();

    FILE *fp = fopen(RECENT_FILE_DB, "r");
    if (!fp) return;

    char line[1024];

    while (fgets(line, sizeof(line), fp) && recent_file_count < MAX_RECENT_FILES) {
        RecentFileEntry *e = &recent_files[recent_file_count];
        memset(e, 0, sizeof(*e));

        char *path = strtok(line, "\t");
        char *title = strtok(NULL, "\t");
        char *last_pos = strtok(NULL, "\t");
        char *duration = strtok(NULL, "\t");
        char *last_played = strtok(NULL, "\t\r\n");

        if (!path || !title) continue;

        snprintf(e->path, sizeof(e->path), "%s", path);
        snprintf(e->title, sizeof(e->title), "%s", title);
        e->last_pos = last_pos ? atof(last_pos) : 0.0;
        e->duration = duration ? atof(duration) : 0.0;
        e->last_played = last_played ? atoll(last_played) : 0;

        recent_file_count++;
    }

    fclose(fp);
}


static void clear_media_metadata(MediaMetadata *m) {
    if (!m) return;
    memset(m, 0, sizeof(MediaMetadata));
    snprintf(m->container, sizeof(m->container), "Unknown");
    snprintf(m->video_codec, sizeof(m->video_codec), "Unknown");
    snprintf(m->audio_codec, sizeof(m->audio_codec), "Unknown");
}

static const char* safe_codec_name(enum AVCodecID id) {
    const char *name = avcodec_get_name(id);
    return name ? name : "Unknown";
}

static void load_media_metadata_basic(const char *path, MediaMetadata *m) {
    if (!m) return;

    clear_media_metadata(m);

    if (path) {
        snprintf(m->path, sizeof(m->path), "%s", path);
        clean_media_title(path, m->title, sizeof(m->title), m->container, sizeof(m->container));
    }

    FILE *fp = path ? fopen(path, "rb") : NULL;
    if (fp) {
        fseek(fp, 0, SEEK_END);
        m->file_size_bytes = ftell(fp);
        fclose(fp);
    }
}

static void load_media_metadata_from_format(AVFormatContext *fmt, const char *path, MediaMetadata *m) {
    if (!m) return;

    load_media_metadata_basic(path, m);

    if (!fmt) return;

    if (fmt->duration > 0) {
        m->duration_sec = (double)fmt->duration / (double)AV_TIME_BASE;
    }

    if (fmt->iformat && fmt->iformat->name) {
        snprintf(m->container, sizeof(m->container), "%s", fmt->iformat->name);
    }

    for (unsigned int i = 0; i < fmt->nb_streams; i++) {
        AVStream *st = fmt->streams[i];
        if (!st || !st->codecpar) continue;

        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !m->has_video) {
            m->has_video = 1;
            m->width = st->codecpar->width;
            m->height = st->codecpar->height;
            snprintf(m->video_codec, sizeof(m->video_codec), "%s", safe_codec_name(st->codecpar->codec_id));
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && !m->has_audio) {
            m->has_audio = 1;
            snprintf(m->audio_codec, sizeof(m->audio_codec), "%s", safe_codec_name(st->codecpar->codec_id));
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            m->has_subtitles = 1;
        }
    }
}

static void format_file_size(char *out, size_t outsz, long long bytes) {
    if (!out || outsz == 0) return;

    double v = (double)bytes;
    const char *unit = "B";

    if (v >= 1024.0) { v /= 1024.0; unit = "KB"; }
    if (v >= 1024.0) { v /= 1024.0; unit = "MB"; }
    if (v >= 1024.0) { v /= 1024.0; unit = "GB"; }

    snprintf(out, outsz, "%.2f %s", v, unit);
}


const char* profile_name(PlaybackProfile profile) {
    switch (profile) {
        case PROFILE_BALANCED: return "Balanced";
        case PROFILE_PERFORMANCE: return "Performance";
        case PROFILE_COMPATIBILITY: return "Compatibility";
        case PROFILE_DEBUG: return "Debug";
        default: return "Balanced";
    }
}

#define PS5_THREAD_COUNT 12

int screen = 0;
int file_count = 0;
int file_selected = 0;
int file_scroll = 0;
char usb_files[256][128];
int player_start_frame = 0;
int player_paused = 0;
int player_elapsed = 0;
int show_stats_for_nerds = 0;
int playback_profile = 1;
int video_packet_cap = 96;
int audio_packet_cap = 96;
int decoder_thread_count = 4;
/* 1 when the current media is an http/https stream (DLNA, IPTV, cast) */
int g_media_is_network = 0;
int resume_prompt_active = 0;
double pending_resume_pos = 0.0;
double requested_resume_seek_pos = 0.0;
double resume_base_offset_seconds = 0.0;
char pending_resume_path[512] = {0};
int video_view_mode = 1; // 0=FIT 1=FILL 2=STRETCH
long long controls_last_used_ms = 0;

#if PP_BACKEND_ENABLED
/* Map UI view mode → product convert aspect (HEVC soft + 4K paths). */
static pp_aspect_mode prospero_view_mode_to_aspect(void)
{
    if (video_view_mode == 1)
        return PP_ASPECT_FILL;
    if (video_view_mode == 2)
        return PP_ASPECT_STRETCH;
    return PP_ASPECT_FIT;
}

/*
 * Apply Fit/Fill/Stretch to the live convert path.
 * H.264 software blit used video_view_mode directly; HEVC/4K used g_pp_pb
 * and previously ignored Triangle unless aspect was pushed each frame — and
 * native UHD V8 is 1:1 only, so we fall back to V3 convert with aspect.
 */
static void prospero_apply_view_mode(void)
{
    pp_aspect_mode am = prospero_view_mode_to_aspect();

    g_pp_pb.cfg.aspect = am;
    g_pp_pb.stats.aspect = (int)am;

    if (g_pp_backend == PP_BACKEND_4K_V8_FUSED ||
        g_pp_backend == PP_BACKEND_4K_V3_FALLBACK) {
        /*
         * V8 fused ignores aspect (1:1 tile). Switch to V3 so FIT/FILL/STRETCH
         * scale into the live UHD (or 1080) plane.
         */
        pp_playback_force_v3_fallback(&g_pp_pb, 1);
        g_pp_backend = PP_BACKEND_4K_V3_FALLBACK;
        pp_playback_set_backend(&g_pp_pb, PP_BACKEND_4K_V3_FALLBACK);
        if (g_vo_w >= 1920u && g_vo_h >= 1080u)
            (void)pp_playback_set_output(&g_pp_pb, g_vo_w, g_vo_h, am);
        else
            (void)pp_playback_set_output(&g_pp_pb, (uint32_t)WIDTH,
                                         (uint32_t)HEIGHT, am);
    } else {
        /* Soft 1080 path: keep buffer size, only aspect changes. */
        g_pp_pb.cfg.aspect = am;
        g_pp_pb.stats.aspect = (int)am;
        if (g_pp_pb.out_w == 0 || g_pp_pb.out_h == 0)
            (void)pp_playback_set_output(&g_pp_pb, (uint32_t)WIDTH,
                                         (uint32_t)HEIGHT, am);
    }
}
#endif
uint32_t *video_frame_pixels = NULL;
uint32_t *video_frame_back_pixels = NULL;
uint32_t *video_frame_third_pixels = NULL;

#define VIDEO_ROTATE_BUFFERS 8
uint32_t *video_rotate_pixels[VIDEO_ROTATE_BUFFERS] = {0};
int video_rotate_index = 0;
int video_frame_w = 0;
int video_frame_h = 0;
int video_frame_loaded = 0;
int video_decode_ready = 0;
int video_decode_done = 0;

static int prospero_playback_finished = 0;
static long long prospero_finish_candidate_ms = 0;

double video_fps = 60.0;
double media_duration_sec = 0.0;
int perf_render_frames = 0;
int perf_decode_frames = 0;
int perf_render_fps = 0;
int perf_decode_fps = 0;
int dbg_video_packets = 0;
int dbg_video_frames = 0;
int dbg_read_fail = 0;
int dbg_video_thread_alive = 0;
int dbg_swaps = 0;
long long dbg_last_pts = 0;
long long perf_last_ms = 0;
long long playback_start_ms = 0;
int video_stream_index = -1;
int audio_stream_index = -1;
/* 1 = audio-only file (music); no video track / video thread */
static int prospero_music_mode = 0;
AVCodecContext *audio_ctx = NULL;

/* PROSPERO_AUDIO_RESAMPLER_STATE_START */

static SwrContext *prospero_audio_swr = NULL;

static AVChannelLayout prospero_audio_input_layout;
static int prospero_audio_input_layout_valid = 0;

static enum AVSampleFormat prospero_audio_input_format =
    AV_SAMPLE_FMT_NONE;

static int prospero_audio_input_rate = 0;
static const int prospero_audio_output_rate = 48000;


static void prospero_audio_resampler_destroy(void) {
    if (prospero_audio_swr) {
        swr_free(&prospero_audio_swr);
    }

    if (prospero_audio_input_layout_valid) {
        av_channel_layout_uninit(
            &prospero_audio_input_layout
        );

        prospero_audio_input_layout_valid = 0;
    }

    prospero_audio_input_format =
        AV_SAMPLE_FMT_NONE;

    prospero_audio_input_rate = 0;
}


static void prospero_audio_resampler_reset(void) {
    if (!prospero_audio_swr) {
        return;
    }

    swr_close(prospero_audio_swr);

    if (swr_init(prospero_audio_swr) < 0) {
        prospero_audio_resampler_destroy();
    }
}


static int prospero_audio_resampler_configure(
    const AVFrame *frame
) {
    if (!frame) {
        return 0;
    }

    int input_rate =
        frame->sample_rate > 0
            ? frame->sample_rate
            : (
                audio_ctx &&
                audio_ctx->sample_rate > 0
                    ? audio_ctx->sample_rate
                    : 48000
            );

    AVChannelLayout input_layout;

    memset(
        &input_layout,
        0,
        sizeof(input_layout)
    );

    if (frame->ch_layout.nb_channels > 0) {
        if (
            av_channel_layout_copy(
                &input_layout,
                &frame->ch_layout
            ) < 0
        ) {
            return 0;
        }
    } else if (
        audio_ctx &&
        audio_ctx->ch_layout.nb_channels > 0
    ) {
        if (
            av_channel_layout_copy(
                &input_layout,
                &audio_ctx->ch_layout
            ) < 0
        ) {
            return 0;
        }
    } else {
        av_channel_layout_default(
            &input_layout,
            2
        );
    }

    int unchanged =
        prospero_audio_swr &&
        prospero_audio_input_layout_valid &&
        prospero_audio_input_format ==
            (enum AVSampleFormat)frame->format &&
        prospero_audio_input_rate ==
            input_rate &&
        av_channel_layout_compare(
            &prospero_audio_input_layout,
            &input_layout
        ) == 0;

    if (unchanged) {
        av_channel_layout_uninit(
            &input_layout
        );

        return 1;
    }

    prospero_audio_resampler_destroy();

    AVChannelLayout output_layout =
        AV_CHANNEL_LAYOUT_STEREO;

    if (
        swr_alloc_set_opts2(
            &prospero_audio_swr,
            &output_layout,
            AV_SAMPLE_FMT_S16,
            prospero_audio_output_rate,
            &input_layout,
            (enum AVSampleFormat)frame->format,
            input_rate,
            0,
            NULL
        ) < 0
    ) {
        av_channel_layout_uninit(
            &input_layout
        );

        prospero_audio_resampler_destroy();
        return 0;
    }

    av_opt_set_double(
        prospero_audio_swr,
        "rematrix_maxval",
        1.0,
        0
    );

    av_opt_set_double(
        prospero_audio_swr,
        "center_mix_level",
        0.70710678,
        0
    );

    av_opt_set_double(
        prospero_audio_swr,
        "surround_mix_level",
        0.70710678,
        0
    );

    av_opt_set_double(
        prospero_audio_swr,
        "lfe_mix_level",
        0.5,
        0
    );

    if (swr_init(prospero_audio_swr) < 0) {
        av_channel_layout_uninit(
            &input_layout
        );

        prospero_audio_resampler_destroy();
        return 0;
    }

    if (
        av_channel_layout_copy(
            &prospero_audio_input_layout,
            &input_layout
        ) < 0
    ) {
        av_channel_layout_uninit(
            &input_layout
        );

        prospero_audio_resampler_destroy();
        return 0;
    }

    prospero_audio_input_layout_valid = 1;

    prospero_audio_input_format =
        (enum AVSampleFormat)frame->format;

    prospero_audio_input_rate =
        input_rate;

    av_channel_layout_uninit(
        &input_layout
    );

    return 1;
}

/* PROSPERO_AUDIO_RESAMPLER_STATE_END */


int audio_handle = -1;
static int16_t audio_accum[2048 * 2];
int audio_accum_pos = 0;

/* ~2.7s of stereo 48kHz at 2048-sample blocks — UHD soft decode needs headroom */
#define AUDIO_QUEUE_BLOCKS 64
#define AUDIO_BLOCK_SAMPLES 2048

static int16_t audio_queue[AUDIO_QUEUE_BLOCKS][AUDIO_BLOCK_SAMPLES * 2];
static volatile int audio_queue_read = 0;
static volatile int audio_queue_write = 0;
static volatile int audio_queue_count = 0;
static volatile long long audio_samples_played = 0;
static volatile long long audio_samples_decoded = 0;
static volatile double audio_clock_seconds = 0.0;
static volatile double audio_pts_seconds = 0.0;
static double first_video_pts_seconds = -1.0;
static double first_audio_pts_seconds = -1.0;
double video_clock_seconds = 0.0;

static double prospero_media_clock_seconds(void)
{
    double audio_rel = audio_clock_seconds;
    double video_rel = 0.0;

    if (first_video_pts_seconds >= 0.0)
        video_rel = video_clock_seconds - first_video_pts_seconds;
    else if (video_clock_seconds > 0.0)
        video_rel = video_clock_seconds;
    if (video_rel < 0.0)
        video_rel = 0.0;

    /*
     * Prefer audio when a playable track is open and the out clock has
     * started. Otherwise drive UI/resume from video so silent E-AC3
     * files still move the progress bar.
     */
    if (audio_stream_index >= 0 && audio_handle >= 1 && audio_rel >= 0.05)
        return audio_rel;
    return video_rel;
}
static int detected_audio_rate = 48000;
static volatile int audio_thread_running = 0;
static pthread_t audio_thread;

static volatile int video_thread_running = 0;
static volatile int audio_decode_thread_running = 0;
static pthread_t video_thread;
static pthread_t audio_decode_thread;
static AVPacket *video_pending_pkt = NULL;
static AVPacket *video_video_pending_pkt = NULL;
static pthread_mutex_t video_frame_mutex = PTHREAD_MUTEX_INITIALIZER;

void audio_queue_push(int16_t *buf) {
    int spins = 0;
    /* Wait for space instead of dropping — drops cause choppy audio on UHD */
    while (audio_queue_count >= AUDIO_QUEUE_BLOCKS &&
           audio_decode_thread_running && spins < 200) {
        usleep(500);
        spins++;
    }
    if (audio_queue_count >= AUDIO_QUEUE_BLOCKS)
        return;

    memcpy(audio_queue[audio_queue_write], buf, AUDIO_BLOCK_SAMPLES * 2 * sizeof(int16_t));
    audio_queue_write = (audio_queue_write + 1) % AUDIO_QUEUE_BLOCKS;
    audio_queue_count++;
}

void *audio_output_thread(void *arg) {
    static int16_t silence[AUDIO_BLOCK_SAMPLES * 2];
    while (audio_thread_running) {
        if (screen == 2 && !player_paused && audio_handle >= 1) {
            /*
             * Compare like-for-like: audio_clock is from t=0 of this session,
             * video must be relative to first video PTS (not absolute PTS).
             * Absolute compare freezes easy 720p/YouTube when clocks diverge.
             */
            double video_rel = video_clock_seconds;
            if (first_video_pts_seconds >= 0.0)
                video_rel = video_clock_seconds - first_video_pts_seconds;
            if (video_rel < 0.0)
                video_rel = 0.0;

            if (video_rel > 0.1 &&
                audio_clock_seconds > video_rel + 0.50) {
                usleep(2000);
                continue;
            }
            if (audio_queue_count > 0) {
                sceAudioOutOutput(audio_handle, audio_queue[audio_queue_read]);
                audio_samples_played += AUDIO_BLOCK_SAMPLES;
                audio_clock_seconds = (double)audio_samples_played / 48000.0;
                audio_queue_read = (audio_queue_read + 1) % AUDIO_QUEUE_BLOCKS;
                audio_queue_count--;
            } else {
                /*
                 * Soft underrun: silence without advancing media clock.
                 * Video wait loop must break if audio stays stuck (see decode).
                 */
                sceAudioOutOutput(audio_handle, silence);
            }
        } else {
            usleep(200);
        }
    }
    return NULL;
}


#define PACKET_QUEUE_SIZE 512

typedef struct {
    AVPacket *packets[PACKET_QUEUE_SIZE];
    int read;
    int write;
    int count;
    pthread_mutex_t mutex;
} PacketQueue;

static PacketQueue video_packet_queue = { .mutex = PTHREAD_MUTEX_INITIALIZER };
static PacketQueue audio_packet_queue = { .mutex = PTHREAD_MUTEX_INITIALIZER };

static volatile int demux_thread_running = 0;
static pthread_t demux_thread;

static void packet_queue_clear(PacketQueue *q) {
    pthread_mutex_lock(&q->mutex);

    for (int i = 0; i < PACKET_QUEUE_SIZE; i++) {
        if (q->packets[i]) {
            av_packet_free(&q->packets[i]);
            q->packets[i] = NULL;
        }
    }

    q->read = 0;
    q->write = 0;
    q->count = 0;

    pthread_mutex_unlock(&q->mutex);
}

static int packet_queue_push(PacketQueue *q, AVPacket *pkt) {
    pthread_mutex_lock(&q->mutex);

    if (q->count >= PACKET_QUEUE_SIZE) {
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }

    q->packets[q->write] = av_packet_clone(pkt);
    q->write = (q->write + 1) % PACKET_QUEUE_SIZE;
    q->count++;

    pthread_mutex_unlock(&q->mutex);
    return 1;
}

static AVPacket *packet_queue_pop(PacketQueue *q) {
    pthread_mutex_lock(&q->mutex);

    if (q->count <= 0) {
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }

    AVPacket *pkt = q->packets[q->read];
    q->packets[q->read] = NULL;
    q->read = (q->read + 1) % PACKET_QUEUE_SIZE;
    q->count--;

    pthread_mutex_unlock(&q->mutex);
    return pkt;
}

static int packet_queue_count(PacketQueue *q) {
    pthread_mutex_lock(&q->mutex);
    int c = q->count;
    pthread_mutex_unlock(&q->mutex);
    return c;
}

AVFormatContext *play_fmt = NULL;
AVCodecContext *play_ctx = NULL;
AVPacket *play_pkt = NULL;
AVFrame *play_frame = NULL;
struct SwsContext *play_sws = NULL;
char current_media[128];
char current_path[256] = "/mnt/usb0";
char current_media_path[512];
char current_image[256];
char current_image_path[512];
uint32_t *bmp_pixels = NULL;
int bmp_w = 0;
int bmp_h = 0;
int bmp_loaded = 0;
unsigned char usb_types[256];

#define PS5_PAD_BUTTON_OPTIONS 0x0008
#define PS5_PAD_BUTTON_UP      0x0010
#define PS5_PAD_BUTTON_DOWN    0x0040
#define PS5_PAD_BUTTON_LEFT     0x0080
#define PS5_PAD_BUTTON_RIGHT    0x0020
#define PS5_PAD_BUTTON_CIRCLE  0x2000
#define PS5_PAD_BUTTON_TRIANGLE 0x1000
#define PS5_PAD_BUTTON_L1       0x0400
#define PS5_PAD_BUTTON_R1       0x0800
#define PS5_PAD_BUTTON_L2       0x0100
#define PS5_PAD_BUTTON_R2       0x0200
#define PS5_PAD_BUTTON_SQUARE   0x8000
#define PS5_PAD_BUTTON_CROSS   0x4000
#define PS5_PAD_BUTTON_R3      0x00040000
#define PS5_PAD_BUTTON_L3      0x00020000

typedef struct PS5_PadData {
    uint32_t buttons;
    struct { uint8_t x; uint8_t y; } leftStick;
    struct { uint8_t x; uint8_t y; } rightStick;
    struct { uint8_t l2; uint8_t r2; } analogButtons;
    uint8_t rest[128];
} PS5_PadData;

int scePadReadState(int, PS5_PadData *);


typedef struct PS5_VideoBuf {
    void *data;
    uint64_t junk0[3];
} PS5_VideoBuf;

typedef struct PS5_VideoAttr {
    uint8_t junk0[80];
} PS5_VideoAttr;

typedef struct PS5_DrawChunk {
    uint32_t *src;
    uint32_t *dst;
    uint16_t frame_width;
    uint16_t frame_height;
    size_t src_start;
    size_t src_end;
} PS5_DrawChunk;

int sceNotificationSend(int userId, bool isLogged, const char* payload);
int sceKernelSendNotificationRequest(int userId, void* req, size_t size, int flags);
int sceKernelAllocateMainDirectMemory(size_t, size_t, int, intptr_t*);
int sceKernelMapDirectMemory(void**, size_t, int, int, intptr_t, size_t);
int sceKernelCreateEqueue(struct kevent **, const char *);
int sceVideoOutOpen(int, int, int, const void*);
int sceVideoOutAddFlipEvent(struct kevent*, int, void*);
int sceVideoOutSetFlipRate(int, int);
int sceVideoOutSubmitFlip(int, int, uint32_t, int64_t);
void sceVideoOutSetBufferAttribute2(PS5_VideoAttr*, uint64_t, uint32_t, uint32_t, uint32_t, uint64_t, uint32_t, uint64_t);
int sceVideoOutRegisterBuffers2(int, int, int, PS5_VideoBuf*, int, PS5_VideoAttr*, int, void*);

void toast(const char* title, const char* msg);

void ffmpeg_mkv_test(void);


int start_video_playback(const char *path);

/* PROSPERO_SRT_FORWARD_START */

static void prospero_subtitle_clear(void);

/* PROSPERO_SUBTITLE_EARLY_DECLS_START */

/*
 * These are implemented later by the external SRT module but are
 * also used by the earlier embedded-subtitle module.
 */
static int prospero_subtitle_count;
static int prospero_subtitle_enabled;

static void prospero_subtitle_clean_line(
    const char *input,
    char *output,
    size_t output_size
);

static void prospero_subtitle_append_text(
    char *destination,
    size_t destination_size,
    const char *line
);

/* PROSPERO_SUBTITLE_EARLY_DECLS_END */



static int prospero_subtitle_load_for_media(
    const char *media_path
);


/* PROSPERO_EMBEDDED_SUBTITLE_MODULE_START */

#define PROSPERO_EMBEDDED_SUBTITLE_MAX_CUES 256
#define PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE 768

typedef struct {
    double start_seconds;
    double end_seconds;

    char text[
        PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE
    ];
} ProsperoEmbeddedSubtitleCue;


static AVCodecContext *
prospero_embedded_subtitle_ctx = NULL;

static int
prospero_embedded_subtitle_stream_index = -1;

/* PROSPERO_DIRECT_SUBRIP_START */

static enum AVCodecID
prospero_embedded_subtitle_codec_id =
    AV_CODEC_ID_NONE;

/* PROSPERO_DIRECT_SUBRIP_END */



/* PROSPERO_SUBTITLE_SELECTION_STATE_START */

/*
 * -2 = automatic
 * -1 = external matching SRT
 * >=0 = exact embedded MKV subtitle stream
 */
static int prospero_subtitle_requested_stream = -2;

/*
 * Zero renders the active embedded stream.
 * One renders the matching external SRT.
 */
static int prospero_subtitle_use_external = 0;

/* PROSPERO_SUBTITLE_SELECTION_STATE_END */




static ProsperoEmbeddedSubtitleCue
prospero_embedded_subtitle_cues[
    PROSPERO_EMBEDDED_SUBTITLE_MAX_CUES
];

static int
prospero_embedded_subtitle_head = 0;

static int
prospero_embedded_subtitle_count = 0;

static pthread_mutex_t
prospero_embedded_subtitle_mutex =
    PTHREAD_MUTEX_INITIALIZER;


static int prospero_embedded_subtitle_supported(
    enum AVCodecID codec_id
) {
    return (
        codec_id == AV_CODEC_ID_SUBRIP ||
        codec_id == AV_CODEC_ID_ASS ||
        codec_id == AV_CODEC_ID_SSA ||
        codec_id == AV_CODEC_ID_WEBVTT
    );
}


static void prospero_embedded_subtitle_reset(void) {
    pthread_mutex_lock(
        &prospero_embedded_subtitle_mutex
    );

    prospero_embedded_subtitle_head = 0;
    prospero_embedded_subtitle_count = 0;

    pthread_mutex_unlock(
        &prospero_embedded_subtitle_mutex
    );
}


static void prospero_embedded_subtitle_close(void) {
    prospero_embedded_subtitle_reset();

    if (prospero_embedded_subtitle_ctx) {
        avcodec_free_context(
            &prospero_embedded_subtitle_ctx
        );
    }

    prospero_embedded_subtitle_stream_index =
        -1;

    prospero_embedded_subtitle_codec_id =
        AV_CODEC_ID_NONE;
}


static int prospero_embedded_subtitle_score_stream(
    AVStream *stream
) {
    if (
        !stream ||
        !stream->codecpar ||
        !prospero_embedded_subtitle_supported(
            stream->codecpar->codec_id
        )
    ) {
        return -100000;
    }

    int score = 10;

    AVDictionaryEntry *language =
        av_dict_get(
            stream->metadata,
            "language",
            NULL,
            0
        );

    if (language && language->value) {
        if (
            strcasecmp(
                language->value,
                "eng"
            ) == 0 ||
            strcasecmp(
                language->value,
                "en"
            ) == 0 ||
            strcasecmp(
                language->value,
                "english"
            ) == 0
        ) {
            score += 100;
        }
    }

    if (
        stream->disposition &
        AV_DISPOSITION_DEFAULT
    ) {
        score += 40;
    }

    if (
        stream->disposition &
        AV_DISPOSITION_FORCED
    ) {
        score += 25;
    }

    return score;
}


static int prospero_embedded_subtitle_open(
    AVFormatContext *format
) {
    prospero_embedded_subtitle_close();

    /*
     * A matching external SRT is treated as the selected track.
     */
    if (!format) {
        return 0;
    }

    int best_stream = -1;
    int best_score = -100000;
    int found_pgs = 0;

    for (
        unsigned int index = 0;
        index < format->nb_streams;
        index++
    ) {
        AVStream *stream =
            format->streams[index];

        if (
            !stream ||
            !stream->codecpar ||
            stream->codecpar->codec_type !=
                AVMEDIA_TYPE_SUBTITLE
        ) {
            continue;
        }

        if (
            stream->codecpar->codec_id ==
            AV_CODEC_ID_HDMV_PGS_SUBTITLE
        ) {
            found_pgs = 1;
            continue;
        }

        int score =
            prospero_embedded_subtitle_score_stream(
                stream
            );

        if (score > best_score) {
            best_score = score;
            best_stream = (int)index;
        }
    }

    
    /*
     * D-pad track cycling may request a specific embedded stream.
     */
    if (
        prospero_subtitle_requested_stream >= 0
    ) {
        int requested =
            prospero_subtitle_requested_stream;

        if (
            requested <
            (int)format->nb_streams
        ) {
            AVStream *requested_stream =
                format->streams[requested];

            if (
                requested_stream &&
                requested_stream->codecpar &&
                requested_stream->codecpar->codec_type ==
                    AVMEDIA_TYPE_SUBTITLE &&
                prospero_embedded_subtitle_supported(
                    requested_stream->codecpar->codec_id
                )
            ) {
                best_stream = requested;
            }
        }
    }

if (best_stream < 0) {
        if (found_pgs) {
            toast(
                "SUBTITLES",
                "PGS NOT SUPPORTED YET"
            );
        }

        return 0;
    }

    AVStream *stream =
        format->streams[best_stream];

    enum AVCodecID codec_id =
        stream->codecpar->codec_id;

    prospero_embedded_subtitle_stream_index =
        best_stream;

    prospero_embedded_subtitle_codec_id =
        codec_id;

    prospero_embedded_subtitle_reset();

    /*
     * Matroska SubRip packets already contain plain subtitle text.
     * Decode them directly when the PS5 FFmpeg build does not include
     * a registered SubRip decoder.
     */
    if (codec_id != AV_CODEC_ID_SUBRIP) {
        const AVCodec *decoder =
            avcodec_find_decoder(
                codec_id
            );

        if (!decoder) {
            prospero_embedded_subtitle_stream_index =
                -1;

            prospero_embedded_subtitle_codec_id =
                AV_CODEC_ID_NONE;

            toast(
                "SUBTITLES",
                "TEXT DECODER NOT FOUND"
            );

            return 0;
        }

        AVCodecContext *context =
            avcodec_alloc_context3(
                decoder
            );

        if (!context) {
            prospero_embedded_subtitle_stream_index =
                -1;

            prospero_embedded_subtitle_codec_id =
                AV_CODEC_ID_NONE;

            return 0;
        }

        if (
            avcodec_parameters_to_context(
                context,
                stream->codecpar
            ) < 0
        ) {
            avcodec_free_context(
                &context
            );

            prospero_embedded_subtitle_stream_index =
                -1;

            prospero_embedded_subtitle_codec_id =
                AV_CODEC_ID_NONE;

            return 0;
        }

        context->pkt_timebase =
            stream->time_base;

        if (
            avcodec_open2(
                context,
                decoder,
                NULL
            ) < 0
        ) {
            avcodec_free_context(
                &context
            );

            prospero_embedded_subtitle_stream_index =
                -1;

            prospero_embedded_subtitle_codec_id =
                AV_CODEC_ID_NONE;

            toast(
                "SUBTITLES",
                "TEXT DECODER FAILED"
            );

            return 0;
        }

        prospero_embedded_subtitle_ctx =
            context;
    }

    const char *codec_name =
        avcodec_get_name(
            codec_id
        );

    const char *language_name =
        "UNSPECIFIED";

    AVDictionaryEntry *language =
        av_dict_get(
            stream->metadata,
            "language",
            NULL,
            0
        );

    if (
        language &&
        language->value &&
        language->value[0]
    ) {
        language_name =
            language->value;
    }

    char message[128];

    snprintf(
        message,
        sizeof(message),
        "%s / %s",
        language_name,
        codec_name
            ? codec_name
            : "TEXT"
    );

    toast(
        "EMBEDDED SUBTITLES",
        message
    );

    return 1;
}


static const char *
prospero_embedded_subtitle_ass_payload(
    const char *ass
) {
    if (!ass) {
        return NULL;
    }

    int fields_to_skip =
        strncmp(
            ass,
            "Dialogue:",
            9
        ) == 0
            ? 9
            : 8;

    const char *cursor = ass;
    int comma_count = 0;

    while (*cursor) {
        if (*cursor == ',') {
            comma_count++;

            if (
                comma_count >=
                fields_to_skip
            ) {
                return cursor + 1;
            }
        }

        cursor++;
    }

    return ass;
}


static void prospero_embedded_subtitle_extract_text(
    AVSubtitle *subtitle,
    char *output,
    size_t output_size
) {
    if (
        !subtitle ||
        !output ||
        output_size == 0
    ) {
        return;
    }

    output[0] = 0;

    for (
        unsigned int index = 0;
        index < subtitle->num_rects;
        index++
    ) {
        AVSubtitleRect *rectangle =
            subtitle->rects[index];

        if (!rectangle) {
            continue;
        }

        const char *raw_text = NULL;

        if (
            rectangle->text &&
            rectangle->text[0]
        ) {
            raw_text =
                rectangle->text;
        } else if (
            rectangle->ass &&
            rectangle->ass[0]
        ) {
            raw_text =
                prospero_embedded_subtitle_ass_payload(
                    rectangle->ass
                );
        }

        if (!raw_text || !raw_text[0]) {
            continue;
        }

        char cleaned[
            PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE
        ];

        prospero_subtitle_clean_line(
            raw_text,
            cleaned,
            sizeof(cleaned)
        );

        if (cleaned[0]) {
            prospero_subtitle_append_text(
                output,
                output_size,
                cleaned
            );
        }
    }
}


static void prospero_embedded_subtitle_add_cue(
    double start_seconds,
    double end_seconds,
    const char *text
) {
    if (
        !text ||
        !text[0] ||
        end_seconds <= start_seconds
    ) {
        return;
    }

    pthread_mutex_lock(
        &prospero_embedded_subtitle_mutex
    );

    if (
        prospero_embedded_subtitle_count >=
        PROSPERO_EMBEDDED_SUBTITLE_MAX_CUES
    ) {
        prospero_embedded_subtitle_head =
            (
                prospero_embedded_subtitle_head +
                1
            ) %
            PROSPERO_EMBEDDED_SUBTITLE_MAX_CUES;

        prospero_embedded_subtitle_count--;
    }

    int write_index =
        (
            prospero_embedded_subtitle_head +
            prospero_embedded_subtitle_count
        ) %
        PROSPERO_EMBEDDED_SUBTITLE_MAX_CUES;

    ProsperoEmbeddedSubtitleCue *cue =
        &prospero_embedded_subtitle_cues[
            write_index
        ];

    cue->start_seconds =
        start_seconds;

    cue->end_seconds =
        end_seconds;

    snprintf(
        cue->text,
        sizeof(cue->text),
        "%s",
        text
    );

    prospero_embedded_subtitle_count++;

    pthread_mutex_unlock(
        &prospero_embedded_subtitle_mutex
    );
}


static void prospero_embedded_subtitle_decode_packet(
    AVPacket *packet
) {
    if (
        !packet ||
        packet->stream_index !=
            prospero_embedded_subtitle_stream_index ||
        !play_fmt ||
        prospero_embedded_subtitle_stream_index < 0
    ) {
        return;
    }

    AVStream *stream =
        play_fmt->streams[
            prospero_embedded_subtitle_stream_index
        ];

    int64_t timestamp =
        packet->pts != AV_NOPTS_VALUE
            ? packet->pts
            : packet->dts;

    double base_seconds = 0.0;

    if (timestamp != AV_NOPTS_VALUE) {
        base_seconds =
            timestamp *
            av_q2d(
                stream->time_base
            );
    }

    double packet_duration = 0.0;

    if (packet->duration > 0) {
        packet_duration =
            packet->duration *
            av_q2d(
                stream->time_base
            );
    }

    if (packet_duration <= 0.0) {
        packet_duration = 5.0;
    }

    /*
     * Direct Matroska SubRip path.
     */
    if (
        prospero_embedded_subtitle_codec_id ==
        AV_CODEC_ID_SUBRIP
    ) {
        if (
            !packet->data ||
            packet->size <= 0
        ) {
            return;
        }

        size_t copy_size =
            (size_t)packet->size;

        if (
            copy_size >=
            PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE
        ) {
            copy_size =
                PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE - 1;
        }

        char raw_text[
            PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE
        ];

        memcpy(
            raw_text,
            packet->data,
            copy_size
        );

        raw_text[copy_size] = 0;

        char cleaned[
            PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE
        ];

        prospero_subtitle_clean_line(
            raw_text,
            cleaned,
            sizeof(cleaned)
        );

        if (cleaned[0]) {
            prospero_embedded_subtitle_add_cue(
                base_seconds,
                base_seconds + packet_duration,
                cleaned
            );
        }

        return;
    }

    if (!prospero_embedded_subtitle_ctx) {
        return;
    }

    AVSubtitle subtitle;

    memset(
        &subtitle,
        0,
        sizeof(subtitle)
    );

    subtitle.pts =
        AV_NOPTS_VALUE;

    int got_subtitle = 0;

    int result =
        avcodec_decode_subtitle2(
            prospero_embedded_subtitle_ctx,
            &subtitle,
            &got_subtitle,
            packet
        );

    if (
        result < 0 ||
        !got_subtitle
    ) {
        avsubtitle_free(
            &subtitle
        );

        return;
    }

    if (
        subtitle.pts !=
        AV_NOPTS_VALUE
    ) {
        base_seconds =
            (double)subtitle.pts /
            (double)AV_TIME_BASE;
    }

    double start_seconds =
        base_seconds +
        subtitle.start_display_time /
            1000.0;

    double end_seconds =
        base_seconds +
        subtitle.end_display_time /
            1000.0;

    if (
        end_seconds <= start_seconds ||
        end_seconds - start_seconds >
            120.0
    ) {
        end_seconds =
            start_seconds +
            packet_duration;
    }

    if (end_seconds <= start_seconds) {
        end_seconds =
            start_seconds + 5.0;
    }

    char text[
        PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE
    ];

    prospero_embedded_subtitle_extract_text(
        &subtitle,
        text,
        sizeof(text)
    );

    if (text[0]) {
        prospero_embedded_subtitle_add_cue(
            start_seconds,
            end_seconds,
            text
        );
    }

    avsubtitle_free(
        &subtitle
    );
}


static int prospero_embedded_subtitle_text_at(
    double position,
    char *output,
    size_t output_size
) {
    if (
        !output ||
        output_size == 0
    ) {
        return 0;
    }

    output[0] = 0;

    pthread_mutex_lock(
        &prospero_embedded_subtitle_mutex
    );

    const ProsperoEmbeddedSubtitleCue *
        selected = NULL;

    for (
        int offset = 0;
        offset <
            prospero_embedded_subtitle_count;
        offset++
    ) {
        int index =
            (
                prospero_embedded_subtitle_head +
                offset
            ) %
            PROSPERO_EMBEDDED_SUBTITLE_MAX_CUES;

        const ProsperoEmbeddedSubtitleCue *cue =
            &prospero_embedded_subtitle_cues[
                index
            ];

        if (
            position >= cue->start_seconds &&
            position <= cue->end_seconds
        ) {
            if (
                !selected ||
                cue->start_seconds >
                    selected->start_seconds
            ) {
                selected = cue;
            }
        }
    }

    if (selected) {
        snprintf(
            output,
            output_size,
            "%s",
            selected->text
        );
    }

    pthread_mutex_unlock(
        &prospero_embedded_subtitle_mutex
    );

    return output[0] != 0;
}

/* PROSPERO_EMBEDDED_SUBTITLE_MODULE_END */

static void prospero_subtitle_draw(
    uint32_t *framebuffer,
    double position,
    int osd_visibility
);

/* PROSPERO_SRT_FORWARD_END */

/* PROSPERO_EMBEDDED_SUBTITLE_FORWARD_START */

/*
 * External SRT state/helpers are defined later in this translation
 * unit, after the embedded subtitle module.
 */
static int prospero_subtitle_count;

static void prospero_subtitle_clean_line(
    const char *input,
    char *output,
    size_t output_size
);

static void prospero_subtitle_append_text(
    char *destination,
    size_t destination_size,
    const char *line
);



static int prospero_embedded_subtitle_open(
    AVFormatContext *format
);

static void prospero_embedded_subtitle_decode_packet(
    AVPacket *packet
);

static void prospero_embedded_subtitle_reset(void);
static void prospero_embedded_subtitle_close(void);

static int prospero_embedded_subtitle_text_at(
    double position,
    char *output,
    size_t output_size
);

/* PROSPERO_EMBEDDED_SUBTITLE_FORWARD_END */




void save_resume_position(void);
double load_resume_position(const char *path);

void stop_video_playback(void);
static void prospero_thumbnail_close_context(void);
int decode_next_video_frame(void);
long long now_ms(void);

void ffmpeg_test(void) {
    char msg[128];

    snprintf(msg, sizeof(msg),
        "FFmpeg %u",
        avformat_version());

}


/* PROSPERO_TOAST_STATE_START */

static char prospero_toast_title[96] = {0};
static char prospero_toast_message[256] = {0};
static long long prospero_toast_started_ms = 0;
static int prospero_toast_active = 0;
static int prospero_toast_kind = 0;

static int prospero_text_contains_ci(
    const char *text,
    const char *needle
) {
    if (!text || !needle || !needle[0]) {
        return 0;
    }

    size_t needle_length = strlen(needle);

    for (const char *cursor = text; *cursor; cursor++) {
        if (
            strncasecmp(
                cursor,
                needle,
                needle_length
            ) == 0
        ) {
            return 1;
        }
    }

    return 0;
}

static int prospero_toast_is_technical(
    const char *title,
    const char *message
) {
    const char *technical_titles[] = {
        "STREAM INFO",
        "AUDIO STREAM",
        "FIRST FRAME",
        "MKV OPEN",
        "MKV INFO",
        "DECODER"
    };

    for (
        size_t i = 0;
        i <
        sizeof(technical_titles) /
        sizeof(technical_titles[0]);
        i++
    ) {
        if (
            title &&
            strcasecmp(
                title,
                technical_titles[i]
            ) == 0
        ) {
            return 1;
        }
    }

    if (
        title &&
        strcasecmp(title, "AUDIO") == 0
    ) {
        if (
            prospero_text_contains_ci(message, "OK") ||
            prospero_text_contains_ci(message, "THREAD") ||
            prospero_text_contains_ci(message, "DECODER")
        ) {
            return 1;
        }
    }

    if (
        title &&
        strcasecmp(title, "PROFILE") == 0
    ) {
        return 1;
    }

    if (
        title &&
        (
            prospero_text_contains_ci(title, "SELECTED") ||
            prospero_text_contains_ci(message, "SELECTED")
        )
    ) {
        return 1;
    }

    return 0;
}

static int prospero_toast_is_error(
    const char *title,
    const char *message
) {
    const char *error_terms[] = {
        "FAIL",
        "FAILED",
        "ERROR",
        "UNSUPPORTED",
        "NOT FOUND",
        "NO VIDEO",
        "NO AUDIO",
        "INVALID"
    };

    for (
        size_t i = 0;
        i <
        sizeof(error_terms) /
        sizeof(error_terms[0]);
        i++
    ) {
        if (
            prospero_text_contains_ci(
                title,
                error_terms[i]
            ) ||
            prospero_text_contains_ci(
                message,
                error_terms[i]
            )
        ) {
            return 1;
        }
    }

    return 0;
}

/* PROSPERO_TOAST_STATE_END */

void toast(
    const char *title,
    const char *msg
) {
    if (!title) {
        title = "PS PLAY";
    }

    if (!msg) {
        msg = "";
    }

    int technical =
        prospero_toast_is_technical(
            title,
            msg
        );

    int error =
        prospero_toast_is_error(
            title,
            msg
        );

    int debug_enabled =
        show_debug_overlay ||
        current_profile == PROFILE_DEBUG;

    /*
     * Normal users do not need decoder initialization,
     * stream-count or successful-thread messages.
     */
    if (technical && !error && !debug_enabled) {
        return;
    }

    snprintf(
        prospero_toast_title,
        sizeof(prospero_toast_title),
        "%s",
        title
    );

    snprintf(
        prospero_toast_message,
        sizeof(prospero_toast_message),
        "%s",
        msg
    );

    prospero_toast_started_ms =
        now_ms();

    prospero_toast_active = 1;

    prospero_toast_kind =
        error ? 2 :
        technical ? 1 :
        0;

    /*
     * Native notifications: the kernel notification path
     * (sceKernelSendNotificationRequest) is the only one reliably visible
     * from a payload/BigApp context on all firmwares — the sprx service
     * (sceNotificationSend) renders nothing there.
     * Errors and user-facing messages always notify; technical spam only
     * while the Debug profile or overlay is active.
     */
    if (error || !technical || debug_enabled) {
        typedef struct {
            char reserved[45];
            char message[3075];
        } ProsperoKernelNotify;

        ProsperoKernelNotify req;

        memset(&req, 0, sizeof(req));
        snprintf(
            req.message,
            sizeof(req.message),
            "%s\n%s",
            title,
            msg
        );

        sceKernelSendNotificationRequest(
            0,
            &req,
            sizeof(req),
            0
        );
    }
}


#define PROSPERO_USER_SERVICE_USER_ID_SYSTEM 0xFF
#define PROSPERO_AUDIO_OUT_PORT_TYPE_MAIN 0
#define PROSPERO_AUDIO_OUT_PARAM_FORMAT_S16_STEREO 1


void audio_beep_test(void) {
    sceAudioOutInit();

    int samples = 1024;
    int h = sceAudioOutOpen(PROSPERO_USER_SERVICE_USER_ID_SYSTEM,
                            PROSPERO_AUDIO_OUT_PORT_TYPE_MAIN,
                            0, samples, 48000,
                            PROSPERO_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);

    if (h < 1) {
        toast("AUDIO", "OPEN FAIL");
        return;
    }

    toast("AUDIO", "OPEN OK");

    static int16_t buffer[1024 * 2];

    for (int block = 0; block < 48; block++) {
        for (int i = 0; i < samples; i++) {
            int16_t sample = ((i + block * samples) % 54) < 27 ? 12000 : -12000;
            buffer[i * 2 + 0] = sample;
            buffer[i * 2 + 1] = sample;
        }
        sceAudioOutOutput(h, buffer);
    }

    sceAudioOutClose(h);
    toast("AUDIO", "BEEP DONE");
}

static void* PS5_DrawTileThread(void* arg) {
    const PS5_DrawChunk* chunk = (PS5_DrawChunk*)arg;
    /* RC1-proven float pitch (not integer ceil) — integer ceil stacks 1080 UI */
    for (int ind = chunk->src_start; ind < chunk->src_end; ind++) {
        int x = ind % chunk->frame_width;
        int y = ind / chunk->frame_width;
        int ty = y / PS5_TILE_HEIGHT;
        int tx = x / PS5_TILE_WIDTH;

        int t = (int)(PS5_TILE_SIZE *
                      (tx + ty * ((double)chunk->frame_width / PS5_TILE_WIDTH)));
        int i = PS5_tilemap[y % PS5_TILE_HEIGHT][x % PS5_TILE_WIDTH];

        chunk->dst[t + i] = chunk->src[ind];
    }

    return 0;
}

static void PS5_DrawPixelsAsTiles(uint32_t *src, uint32_t *dst, int frame_width, int frame_height)
{
    int chunk_size = frame_width * frame_height / PS5_THREAD_COUNT;
    PS5_DrawChunk chunks[PS5_THREAD_COUNT];
    pthread_t threads[PS5_THREAD_COUNT];

    for (int i = 0; i < PS5_THREAD_COUNT; i++) {
        chunks[i].src = src;
        chunks[i].dst = dst;
        chunks[i].src_start = i * chunk_size;
        chunks[i].src_end = (i + 1) * chunk_size;
        chunks[i].frame_width = frame_width;
        chunks[i].frame_height = frame_height;

        if (i == PS5_THREAD_COUNT - 1) {
            chunks[i].src_end = frame_width * frame_height;
        }

        pthread_create(&threads[i], 0, &PS5_DrawTileThread, &chunks[i]);
    }

    for (int i = 0; i < PS5_THREAD_COUNT; i++) {
        pthread_join(threads[i], 0);
    }
}

void rect(uint32_t *fb, int x, int y, int w, int h, uint32_t color) {
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            if (xx >= 0 && xx < WIDTH && yy >= 0 && yy < HEIGHT) {
                fb[yy * WIDTH + xx] = color;
            }
        }
    }
}



int count_files_in_path(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return -1;

    int count = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (!strcmp(entry->d_name, "$RECYCLE.BIN")) continue;
        if (!strcmp(entry->d_name, "System Volume Information")) continue;
        count++;
    }

    closedir(dir);
    return count;
}



int load_stb_image_file(const char *path) {
    int w, h, channels;
    unsigned char *data = stbi_load(path, &w, &h, &channels, 4);

    if (!data) {
        toast("IMAGE FAIL", "stbi_load failed");
        bmp_loaded = 0;
        return 0;
    }

    if (bmp_pixels) {
        free(bmp_pixels);
        bmp_pixels = NULL;
    }

    bmp_pixels = malloc(w * h * sizeof(uint32_t));
    if (!bmp_pixels) {
        stbi_image_free(data);
        toast("IMAGE FAIL", "Out of memory");
        bmp_loaded = 0;
        return 0;
    }

    for (int i = 0; i < w * h; i++) {
        unsigned char r = data[i * 4 + 0];
        unsigned char g = data[i * 4 + 1];
        unsigned char b = data[i * 4 + 2];
        bmp_pixels[i] = 0xFF000000 | (b << 16) | (g << 8) | r;
    }

    stbi_image_free(data);

    bmp_w = w;
    bmp_h = h;
    bmp_loaded = 1;

    toast("IMAGE LOADED", path);
    return 1;
}

int load_bmp_file(const char *path);
void draw_bmp_to_fb(uint32_t *fb, int x, int y, int max_w, int max_h);


/* PROSPERO_LAST_FOLDER_V3_START */

#define PROSPERO_LAST_FOLDER_FILE \
    "/mnt/usb0/.prospero_last_folder.cfg"


static int prospero_last_folder_is_safe(
    const char *folder
) {
    if (!folder) {
        return 0;
    }

    if (
        strncmp(
            folder,
            "/mnt/usb0",
            9
        ) != 0
    ) {
        return 0;
    }

    return (
        folder[9] == 0 ||
        folder[9] == '/'
    );
}


static void prospero_last_folder_save(void)
{
    if (
        !prospero_last_folder_is_safe(
            current_path
        )
    ) {
        return;
    }

    FILE *file =
        fopen(
            PROSPERO_LAST_FOLDER_FILE,
            "w"
        );

    if (!file) {
        return;
    }

    fprintf(
        file,
        "%s\n",
        current_path
    );

    fclose(file);
}


static void prospero_last_folder_load(void)
{
    snprintf(
        current_path,
        sizeof(current_path),
        "/mnt/usb0"
    );

    FILE *file =
        fopen(
            PROSPERO_LAST_FOLDER_FILE,
            "r"
        );

    if (!file) {
        return;
    }

    char saved_folder[
        sizeof(current_path)
    ];

    saved_folder[0] = 0;

    if (
        fgets(
            saved_folder,
            sizeof(saved_folder),
            file
        )
    ) {
        size_t length =
            strlen(saved_folder);

        while (
            length > 0 &&
            (
                saved_folder[length - 1] == '\n' ||
                saved_folder[length - 1] == '\r'
            )
        ) {
            saved_folder[length - 1] = 0;
            length--;
        }
    }

    fclose(file);

    if (
        !prospero_last_folder_is_safe(
            saved_folder
        )
    ) {
        return;
    }

    DIR *directory =
        opendir(saved_folder);

    if (!directory) {
        return;
    }

    closedir(directory);

    snprintf(
        current_path,
        sizeof(current_path),
        "%s",
        saved_folder
    );
}

/* PROSPERO_LAST_FOLDER_V3_END */


void load_usb_files(void) {
    DIR *dir =
        opendir(current_path);

    file_count = 0;
    file_selected = 0;

    /*
     * A remembered folder may have been deleted or renamed.
     */
    if (
        !dir &&
        strcmp(
            current_path,
            "/mnt/usb0"
        ) != 0
    ) {
        snprintf(
            current_path,
            sizeof(current_path),
            "/mnt/usb0"
        );

        dir =
            opendir(current_path);
    }

    if (!dir) {
        snprintf(
            usb_files[0],
            sizeof(usb_files[0]),
            "USB NOT FOUND"
        );

        file_count = 1;
        return;
    }


    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && file_count < 256) {
        if (entry->d_name[0] == '.') continue;
        if (!strcmp(entry->d_name, "$RECYCLE.BIN")) continue;
        if (!strcmp(entry->d_name, "System Volume Information")) continue;
        snprintf(usb_files[file_count], sizeof(usb_files[file_count]), "%s", entry->d_name);
        usb_types[file_count] = entry->d_type;
        file_count++;
    }

    closedir(dir);

    if (file_count == 0) {
        snprintf(usb_files[0], sizeof(usb_files[0]), "NO FILES FOUND");
        file_count = 1;
    }
}


int has_ext(const char *name, const char *ext) {
    size_t n = strlen(name);
    size_t e = strlen(ext);
    if (n < e) return 0;
    return strcasecmp(name + n - e, ext) == 0;
}


void ffmpeg_mkv_test(void)
{
    AVFormatContext *fmt = NULL;

    if (avformat_open_input(&fmt, current_media_path, NULL, NULL) == 0)
    {
        toast("MKV OPEN", "SUCCESS");

        char msg[128];
        snprintf(msg, sizeof(msg), "streams:%d", fmt->nb_streams);
        toast("MKV INFO", msg);


        for (unsigned int ai = 0; ai < fmt->nb_streams; ai++) {
            AVCodecParameters *apar = fmt->streams[ai]->codecpar;
            if (apar->codec_type == AVMEDIA_TYPE_AUDIO) {
                snprintf(msg, sizeof(msg), "audio codec:%d ch:%d rate:%d",
                         apar->codec_id, apar->ch_layout.nb_channels, apar->sample_rate);
                toast("AUDIO STREAM", msg);
                break;
            }
        }

        for (unsigned int i = 0; i < fmt->nb_streams; i++) {
            AVCodecParameters *par = fmt->streams[i]->codecpar;
            if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
                snprintf(msg, sizeof(msg), "video:%dx%d codec:%d", par->width, par->height, par->codec_id);
                toast("VIDEO STREAM", msg);

                const AVCodec *dec = avcodec_find_decoder(par->codec_id);
                if (!dec) {
                    toast("DECODER", "NOT FOUND");
                    break;
                }

                AVCodecContext *ctx = avcodec_alloc_context3(dec);
                if (!ctx) {
                    toast("DECODER", "ALLOC FAIL");
                    break;
                }

                if (avcodec_parameters_to_context(ctx, par) < 0) {
                    toast("DECODER", "PARAM FAIL");
                    avcodec_free_context(&ctx);
                    break;
                }

                ctx->thread_count = 4;
                ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

                if (avcodec_open2(ctx, dec, NULL) < 0) {
                    toast("DECODER", "OPEN FAIL");
                    avcodec_free_context(&ctx);
                    break;
                }

                toast("DECODER", "OPEN OK");

                AVPacket *pkt = av_packet_alloc();
                AVFrame *frame = av_frame_alloc();
                int got_frame = 0;

                if (!pkt || !frame) {
                    toast("FRAME", "ALLOC FAIL");
                } else {
                    while (av_read_frame(fmt, pkt) >= 0) {
                        if (pkt->stream_index == (int)i) {
                            int send_ret = avcodec_send_packet(ctx, pkt);

                            if (send_ret == 0) {
                                int recv_ret = avcodec_receive_frame(ctx, frame);

                                if (recv_ret == 0) {
                                    snprintf(msg, sizeof(msg), "frame:%dx%d", frame->width, frame->height);
                                    toast("FIRST FRAME OK", msg);

                                    if (video_frame_pixels) {
                                        free(video_frame_pixels);
                                        video_frame_pixels = NULL;
                                    }

                                    video_frame_w = frame->width;
                                    video_frame_h = frame->height;
                                    video_frame_pixels = malloc(video_frame_w * video_frame_h * 4);

                                    if (video_frame_pixels) {
                                        struct SwsContext *sws = sws_getContext(
                                            frame->width, frame->height, frame->format,
                                            frame->width, frame->height, AV_PIX_FMT_RGBA,
                                            SWS_BILINEAR, NULL, NULL, NULL
                                        );

                                        if (sws) {
                                            uint8_t *dst_data[4] = {(uint8_t*)video_frame_pixels, NULL, NULL, NULL};
                                            int dst_linesize[4] = {video_frame_w * 4, 0, 0, 0};

                                            sws_scale(sws, (const uint8_t * const*)frame->data,
                                                      frame->linesize, 0, frame->height,
                                                      dst_data, dst_linesize);

                                            sws_freeContext(sws);
                                            video_frame_loaded = 1;
                                            toast("FRAME RGB", "CONVERT OK");
                                        } else {
                                            toast("FRAME RGB", "SWS FAIL");
                                        }
                                    } else {
                                        toast("FRAME RGB", "ALLOC FAIL");
                                    }

                                    got_frame = 1;
                                    av_packet_unref(pkt);
                                    break;
                                }
                            }
                        }

                        av_packet_unref(pkt);
                    }

                    if (!got_frame) {
                        toast("FIRST FRAME", "NOT DECODED");
                    }
                }

                if (frame) av_frame_free(&frame);
                if (pkt) av_packet_free(&pkt);

                avcodec_free_context(&ctx);
                break;
            }
        }

        avformat_close_input(&fmt);
    }
    else
    {
        toast("MKV OPEN", "FAILED");
    }
}

/* PROSPERO_LAST_FOLDER_SEMANTICS_V4 */
static void prospero_last_folder_save(void);

void file_action(const char *name) {
    /* Remember the folder containing the selected media. */
    prospero_last_folder_save();
    if (has_ext(name, ".mkv")) {
        snprintf(current_media, sizeof(current_media), "%s", name);
        snprintf(current_media_path, sizeof(current_media_path), "%s/%s", current_path, name);
        double rp =
            prospero_resume_playback_enabled
                ? load_resume_position(
                    current_media_path
                )
                : 0.0;
        if (rp > 0.0) {
            pending_resume_pos = rp;
            snprintf(pending_resume_path, sizeof(pending_resume_path), "%s", current_media_path);
            resume_prompt_active = 1;
            screen = 4;
        } else {
            screen = 2;
            toast("MKV SELECTED", name);
            start_video_playback(current_media_path);
        }
    } else if (has_ext(name, ".mp4")) {
        snprintf(current_media, sizeof(current_media), "%s", name);
        snprintf(current_media_path, sizeof(current_media_path), "%s/%s", current_path, name);
        double rp =
            prospero_resume_playback_enabled
                ? load_resume_position(
                    current_media_path
                )
                : 0.0;
        if (rp > 0.0) {
            pending_resume_pos = rp;
            snprintf(pending_resume_path, sizeof(pending_resume_path), "%s", current_media_path);
            resume_prompt_active = 1;
            screen = 4;
        } else {
            screen = 2;
            toast("MP4 SELECTED", name);
            start_video_playback(current_media_path);
        }
    } else if (has_ext(name, ".avi")) {
        snprintf(current_media, sizeof(current_media), "%s", name);
        snprintf(current_media_path, sizeof(current_media_path), "%s/%s", current_path, name);
        screen = 2;
        toast("AVI SELECTED", name);
    } else if (has_ext(name, ".mov")) {
        snprintf(current_media, sizeof(current_media), "%s", name);
        snprintf(current_media_path, sizeof(current_media_path), "%s/%s", current_path, name);
        double rp =
            prospero_resume_playback_enabled
                ? load_resume_position(
                    current_media_path
                )
                : 0.0;
        if (rp > 0.0) {
            pending_resume_pos = rp;
            snprintf(pending_resume_path, sizeof(pending_resume_path), "%s", current_media_path);
            resume_prompt_active = 1;
            screen = 4;
        } else {
            screen = 2;
            toast("MOV SELECTED", name);
            start_video_playback(current_media_path);
        }
    } else if (
        has_ext(name, ".mp3") ||
        has_ext(name, ".m4a") ||
        has_ext(name, ".aac") ||
        has_ext(name, ".wav") ||
        has_ext(name, ".flac") ||
        has_ext(name, ".ogg")
    ) {
        /* Music / audio-only — same player path, music UI if no video track */
        snprintf(current_media, sizeof(current_media), "%s", name);
        snprintf(current_media_path, sizeof(current_media_path), "%s/%s", current_path, name);
        double rp =
            prospero_resume_playback_enabled
                ? load_resume_position(
                    current_media_path
                )
                : 0.0;
        if (rp > 0.0) {
            pending_resume_pos = rp;
            snprintf(pending_resume_path, sizeof(pending_resume_path), "%s", current_media_path);
            resume_prompt_active = 1;
            screen = 4;
        } else {
            screen = 2;
            toast("MUSIC", name);
            start_video_playback(current_media_path);
        }
    } else if (has_ext(name, ".bmp")) {
        snprintf(current_image, sizeof(current_image), "%s", name);
        snprintf(current_image_path, sizeof(current_image_path), "%s/%s", current_path, name);
        load_bmp_file(current_image_path);
        screen = 3;
        toast("BMP SELECTED", name);
    } else if (has_ext(name, ".jpg") || has_ext(name, ".jpeg") || has_ext(name, ".png")) {
        snprintf(current_image, sizeof(current_image), "%s", name);
        snprintf(current_image_path, sizeof(current_image_path), "%s/%s", current_path, name);
        load_stb_image_file(current_image_path);
        screen = 3;
        toast("IMAGE SELECTED", name);
    } else if (has_ext(name, ".elf")) {
        toast("NOT MEDIA", "ELF file selected");
    } else {
        toast("FILE SELECTED", name);
    }
}

void usb_scan_test(void) {
    char msg[128];

    int c0 = count_files_in_path("/mnt/usb0");
    int c1 = count_files_in_path("/mnt/usb1");
    int cm = count_files_in_path("/mnt");

    snprintf(msg, sizeof(msg), "mnt:%d usb0:%d usb1:%d", cm, c0, c1);
    toast("USB SCAN", msg);
}

void draw_char(uint32_t *fb, int x, int y, char c, uint32_t color, int scale) {
    if (c >= 'a' && c <= 'z') c = c - 32;

    const char *rows[7] = {
        "00000","00000","00000","00000","00000","00000","00000"
    };

    switch (c) {
        case 'A': { static const char*r[]={"01110","10001","10001","11111","10001","10001","10001"}; memcpy(rows,r,sizeof(r)); break; }
        case '0': { static const char*r[]={"01110","10001","10011","10101","11001","10001","01110"}; memcpy(rows,r,sizeof(r)); break; }
        case '1': { static const char*r[]={"00100","01100","00100","00100","00100","00100","01110"}; memcpy(rows,r,sizeof(r)); break; }
        case '2': { static const char*r[]={"01110","10001","00001","00010","00100","01000","11111"}; memcpy(rows,r,sizeof(r)); break; }
        case '3': { static const char*r[]={"11110","00001","00001","01110","00001","00001","11110"}; memcpy(rows,r,sizeof(r)); break; }
        case '4': { static const char*r[]={"00010","00110","01010","10010","11111","00010","00010"}; memcpy(rows,r,sizeof(r)); break; }
        case '5': { static const char*r[]={"11111","10000","10000","11110","00001","00001","11110"}; memcpy(rows,r,sizeof(r)); break; }
        case '6': { static const char*r[]={"01110","10000","10000","11110","10001","10001","01110"}; memcpy(rows,r,sizeof(r)); break; }
        case '7': { static const char*r[]={"11111","00001","00010","00100","01000","01000","01000"}; memcpy(rows,r,sizeof(r)); break; }
        case '8': { static const char*r[]={"01110","10001","10001","01110","10001","10001","01110"}; memcpy(rows,r,sizeof(r)); break; }
        case '9': { static const char*r[]={"01110","10001","10001","01111","00001","00001","01110"}; memcpy(rows,r,sizeof(r)); break; }
        case 'B': { static const char*r[]={"11110","10001","10001","11110","10001","10001","11110"}; memcpy(rows,r,sizeof(r)); break; }

        case 'C': { static const char*r[]={"01111","10000","10000","10000","10000","10000","01111"}; memcpy(rows,r,sizeof(r)); break; }
        case 'F': { static const char*r[]={"11111","10000","10000","11110","10000","10000","10000"}; memcpy(rows,r,sizeof(r)); break; }
        case 'H': { static const char*r[]={"10001","10001","10001","11111","10001","10001","10001"}; memcpy(rows,r,sizeof(r)); break; }
        case 'J': { static const char*r[]={"00111","00010","00010","00010","00010","10010","01100"}; memcpy(rows,r,sizeof(r)); break; }
        case 'K': { static const char*r[]={"10001","10010","10100","11000","10100","10010","10001"}; memcpy(rows,r,sizeof(r)); break; }
        case 'O': { static const char*r[]={"01110","10001","10001","10001","10001","10001","01110"}; memcpy(rows,r,sizeof(r)); break; }
        case 'Q': { static const char*r[]={"01110","10001","10001","10001","10101","10010","01101"}; memcpy(rows,r,sizeof(r)); break; }
        case 'W': { static const char*r[]={"10001","10001","10001","10101","10101","10101","01010"}; memcpy(rows,r,sizeof(r)); break; }
        case 'X': { static const char*r[]={"10001","10001","01010","00100","01010","10001","10001"}; memcpy(rows,r,sizeof(r)); break; }
        case 'Z': { static const char*r[]={"11111","00001","00010","00100","01000","10000","11111"}; memcpy(rows,r,sizeof(r)); break; }
        case '.': { static const char*r[]={"00000","00000","00000","00000","00000","01100","01100"}; memcpy(rows,r,sizeof(r)); break; }
        case '-': { static const char*r[]={"00000","00000","00000","11111","00000","00000","00000"}; memcpy(rows,r,sizeof(r)); break; }
        case '_': { static const char*r[]={"00000","00000","00000","00000","00000","00000","11111"}; memcpy(rows,r,sizeof(r)); break; }

        case 'D': { static const char*r[]={"11110","10001","10001","10001","10001","10001","11110"}; memcpy(rows,r,sizeof(r)); break; }
        case 'E': { static const char*r[]={"11111","10000","10000","11110","10000","10000","11111"}; memcpy(rows,r,sizeof(r)); break; }
        case 'G': { static const char*r[]={"01111","10000","10000","10111","10001","10001","01111"}; memcpy(rows,r,sizeof(r)); break; }
        case 'I': { static const char*r[]={"11111","00100","00100","00100","00100","00100","11111"}; memcpy(rows,r,sizeof(r)); break; }
        case 'L': { static const char*r[]={"10000","10000","10000","10000","10000","10000","11111"}; memcpy(rows,r,sizeof(r)); break; }
        case 'M': { static const char*r[]={"10001","11011","10101","10101","10001","10001","10001"}; memcpy(rows,r,sizeof(r)); break; }
        case 'N': { static const char*r[]={"10001","11001","10101","10011","10001","10001","10001"}; memcpy(rows,r,sizeof(r)); break; }
        case 'P': { static const char*r[]={"11110","10001","10001","11110","10000","10000","10000"}; memcpy(rows,r,sizeof(r)); break; }
        case 'R': { static const char*r[]={"11110","10001","10001","11110","10100","10010","10001"}; memcpy(rows,r,sizeof(r)); break; }
        case 'S': { static const char*r[]={"01111","10000","10000","01110","00001","00001","11110"}; memcpy(rows,r,sizeof(r)); break; }
        case 'T': { static const char*r[]={"11111","00100","00100","00100","00100","00100","00100"}; memcpy(rows,r,sizeof(r)); break; }
        case 'U': { static const char*r[]={"10001","10001","10001","10001","10001","10001","01110"}; memcpy(rows,r,sizeof(r)); break; }
        case 'V': { static const char*r[]={"10001","10001","10001","10001","10001","01010","00100"}; memcpy(rows,r,sizeof(r)); break; }
        case 'Y': { static const char*r[]={"10001","10001","01010","00100","00100","00100","00100"}; memcpy(rows,r,sizeof(r)); break; }
        case '/': { static const char*r[]={"00001","00010","00100","01000","10000","00000","00000"}; memcpy(rows,r,sizeof(r)); break; }
        case '$': { static const char*r[]={"00100","01111","10100","01110","00101","11110","00100"}; memcpy(rows,r,sizeof(r)); break; }
        case '(': { static const char*r[]={"00010","00100","01000","01000","01000","00100","00010"}; memcpy(rows,r,sizeof(r)); break; }
        case ')': { static const char*r[]={"01000","00100","00010","00010","00010","00100","01000"}; memcpy(rows,r,sizeof(r)); break; }
        case ':': { static const char*r[]={"00000","01100","01100","00000","01100","01100","00000"}; memcpy(rows,r,sizeof(r)); break; }
        case ' ': default: break;
    }

    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            if (rows[row][col] == '1') {
                rect(fb, x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

void draw_text(uint32_t *fb, int x, int y, const char *text, uint32_t color, int scale) {
    int cx = x;
    for (int i = 0; text[i]; i++) {
        draw_char(fb, cx, y, text[i], color, scale);
        cx += 6 * scale;
    }
}



void enter_selected_usb(void) {
    if (file_count <= 0) return;

    if (usb_types[file_selected] == 4) {
        char next[256];

        if (strcmp(current_path, "/mnt/usb0") == 0)
            snprintf(next, sizeof(next), "/mnt/usb0/%s", usb_files[file_selected]);
        else
            snprintf(next, sizeof(next), "%s/%s", current_path, usb_files[file_selected]);

        snprintf(current_path, sizeof(current_path), "%s", next);
        load_usb_files();
        prospero_last_folder_save();
        toast("OPEN FOLDER", current_path);
    } else {
        file_action(usb_files[file_selected]);
    }
}

void usb_go_back(void) {
    if (strcmp(current_path, "/mnt/usb0") == 0) {
        screen = 0;
        return;
    }

    char *last = strrchr(current_path, '/');
    if (last && last > current_path) {
        *last = 0;
    }

    load_usb_files();
}


void format_time(int seconds, char *out, int size) {
    int m = seconds / 60;
    int s = seconds % 60;
    snprintf(out, size, "%02d:%02d", m, s);
}


static uint16_t le16(unsigned char *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(unsigned char *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

static int32_t le32s(unsigned char *p) {
    return (int32_t)le32(p);
}


int load_bmp_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        toast("BMP FAIL", "Could not open file");
        bmp_loaded = 0;
        return 0;
    }

    unsigned char h[54];
    if (fread(h, 1, 54, f) != 54) {
        fclose(f);
        toast("BMP FAIL", "Bad header");
        bmp_loaded = 0;
        return 0;
    }

    if (h[0] != 'B' || h[1] != 'M') {
        fclose(f);
        toast("BMP FAIL", "Not BMP");
        bmp_loaded = 0;
        return 0;
    }

    uint32_t data_offset = le32(h + 10);
    int32_t width = le32s(h + 18);
    int32_t height = le32s(h + 22);
    uint16_t bpp = le16(h + 28);
    uint32_t compression = le32(h + 30);

    if (width <= 0 || height == 0 || compression != 0 || (bpp != 24 && bpp != 32)) {
        fclose(f);
        toast("BMP FAIL", "Need 24/32-bit BMP");
        bmp_loaded = 0;
        return 0;
    }

    int abs_h = height < 0 ? -height : height;
    int bottom_up = height > 0;
    int row_size = ((bpp * width + 31) / 32) * 4;

    if (bmp_pixels) {
        free(bmp_pixels);
        bmp_pixels = NULL;
    }

    bmp_pixels = malloc(width * abs_h * sizeof(uint32_t));
    if (!bmp_pixels) {
        fclose(f);
        toast("BMP FAIL", "Out of memory");
        bmp_loaded = 0;
        return 0;
    }

    unsigned char *row = malloc(row_size);
    if (!row) {
        fclose(f);
        free(bmp_pixels);
        bmp_pixels = NULL;
        toast("BMP FAIL", "Row alloc failed");
        bmp_loaded = 0;
        return 0;
    }

    fseek(f, data_offset, SEEK_SET);

    for (int src_y = 0; src_y < abs_h; src_y++) {
        if (fread(row, 1, row_size, f) != (size_t)row_size) break;

        int dst_y = bottom_up ? (abs_h - 1 - src_y) : src_y;

        for (int x = 0; x < width; x++) {
            unsigned char b = row[x * (bpp / 8) + 0];
            unsigned char g = row[x * (bpp / 8) + 1];
            unsigned char r = row[x * (bpp / 8) + 2];

            bmp_pixels[dst_y * width + x] =
                0xFF000000 | (b << 16) | (g << 8) | r;
        }
    }

    free(row);
    fclose(f);

    bmp_w = width;
    bmp_h = abs_h;
    bmp_loaded = 1;

    toast("BMP LOADED", path);
    return 1;
}

void draw_bmp_to_fb(uint32_t *fb, int x, int y, int max_w, int max_h) {
    if (!bmp_loaded || !bmp_pixels || bmp_w <= 0 || bmp_h <= 0) return;

    int draw_w = max_w;
    int draw_h = (bmp_h * draw_w) / bmp_w;

    if (draw_h > max_h) {
        draw_h = max_h;
        draw_w = (bmp_w * draw_h) / bmp_h;
    }

    int ox = x + (max_w - draw_w) / 2;
    int oy = y + (max_h - draw_h) / 2;

    for (int dy = 0; dy < draw_h; dy++) {
        int sy = (dy * bmp_h) / draw_h;
        for (int dx = 0; dx < draw_w; dx++) {
            int sx = (dx * bmp_w) / draw_w;
            int px = ox + dx;
            int py = oy + dy;

            if (px >= 0 && px < WIDTH && py >= 0 && py < HEIGHT) {
                fb[py * WIDTH + px] = bmp_pixels[sy * bmp_w + sx];
            }
        }
    }
}


/* IMAGE_VIEWER_FORWARD_DECLARATIONS_START */
static void rr_bg(uint32_t *fb);

static void rr_fill(
    uint32_t *fb,
    int x,
    int y,
    int w,
    int h,
    uint32_t color
);

static void rr_text(
    uint32_t *fb,
    int x,
    int y,
    const char *text,
    uint32_t color,
    int face
);

static void rr_control(
    uint32_t *fb,
    int x,
    int y,
    int index
);

static void rr_browser_icon(
    uint32_t *fb,
    int x,
    int y,
    int type
);
/* IMAGE_VIEWER_FORWARD_DECLARATIONS_END */

void draw_image_screen(uint32_t *fb) {
    static char last_image_path[512] = {0};
    static int image_intro_frame = 0;

    if (
        strcmp(last_image_path, current_image_path) != 0
    ) {
        snprintf(
            last_image_path,
            sizeof(last_image_path),
            "%s",
            current_image_path
        );

        image_intro_frame = 0;
    }

    if (image_intro_frame < 14) {
        image_intro_frame++;
    }


    rr_bg(fb);

    /*
     * Slightly darken the approved background so the image remains
     * the visual focus.
     */
    rr_fill(
        fb,
        0,
        0,
        1920,
        1080,
        RR_BGRA(8,8,8,105)
    );

    /*
     * Header.
     */
    rr_fill(
        fb,
        82,
        74,
        5,
        76,
        RR_BGRA(205,205,205,225)
    );

    rr_fill(
        fb,
        82,
        146,
        31,
        2,
        RR_BGRA(205,205,205,195)
    );

    rr_text(
        fb,
        122,
        70,
        "IMAGE VIEWER",
        RR_BGRA(232, 244, 255, 240),
        3
    );

    char title[160];

    snprintf(
        title,
        sizeof(title),
        "%s",
        current_image[0]
            ? current_image
            : "IMAGE"
    );

    if (strlen(title) > 58) {
        title[55] = '.';
        title[56] = '.';
        title[57] = '.';
        title[58] = 0;
    }

    rr_text(
        fb,
        124,
        132,
        title,
        RR_BGRA(199,199,199,225),
        1
    );

    /*
     * Entrance animation: the glass frame settles upward slightly.
     */
    int remaining =
        14 - image_intro_frame;

    if (remaining < 0) {
        remaining = 0;
    }

    int frame_offset_y =
        (remaining * remaining) / 4;

    int frame_x = 150;
    int frame_y = 192 + frame_offset_y;
    int frame_w = 1620;
    int frame_h = 690;

    ui_round_asset(
        fb,
        frame_x,
        frame_y,
        frame_w,
        frame_h,
        30,
        RR_BGRA(14,14,14,230),
        RR_BGRA(192,192,192,175),
        RR_BGRA(182,182,182,52)
    );

    /*
     * Inner viewing area.
     */
    int view_x = frame_x + 38;
    int view_y = frame_y + 38;
    int view_w = frame_w - 76;
    int view_h = frame_h - 76;

    rr_fill(
        fb,
        view_x,
        view_y,
        view_w,
        view_h,
        RR_BGRA(5,5,5,235)
    );

    rr_fill(
        fb,
        view_x,
        view_y,
        view_w,
        1,
        RR_BGRA(216,216,216,75)
    );

    rr_fill(
        fb,
        view_x,
        view_y + view_h - 1,
        view_w,
        1,
        RR_BGRA(216,216,216,45)
    );

    if (bmp_loaded && bmp_w > 0 && bmp_h > 0) {
        /*
         * Existing draw_bmp_to_fb() already preserves the image and
         * scales it into the supplied destination rectangle.
         */
        draw_bmp_to_fb(
            fb,
            view_x,
            view_y,
            view_w,
            view_h
        );
    } else {
        rr_browser_icon(
            fb,
            frame_x + frame_w / 2 - 36,
            frame_y + 220,
            3
        );

        rr_text(
            fb,
            frame_x + 570,
            frame_y + 340,
            "IMAGE COULD NOT BE DISPLAYED",
            RR_BGRA(232, 244, 255, 230),
            2
        );

        rr_text(
            fb,
            frame_x + 650,
            frame_y + 392,
            "UNSUPPORTED OR INVALID FILE",
            RR_BGRA(204,204,204,215),
            1
        );
    }

    /*
     * Bottom metadata strip.
     */
    char dimensions[96];

    if (bmp_loaded && bmp_w > 0 && bmp_h > 0) {
        snprintf(
            dimensions,
            sizeof(dimensions),
            "%d X %d",
            bmp_w,
            bmp_h
        );
    } else {
        snprintf(
            dimensions,
            sizeof(dimensions),
            "IMAGE FILE"
        );
    }

    rr_text(
        fb,
        1540,
        132,
        dimensions,
        RR_BGRA(203,203,203,210),
        0
    );

    /*
     * Minimal footer.
     */
    rr_fill(
        fb,
        0,
        948,
        1920,
        1,
        RR_BGRA(186,186,186,48)
    );

    rr_fill(
        fb,
        0,
        949,
        1920,
        79,
        RR_BGRA(12,12,12,138)
    );

    rr_control(
        fb,
        92,
        982,
        4
    );

    rr_text(
        fb,
        148,
        998,
        "BACK",
        RR_BGRA(209,209,209,205),
        0
    );

    rr_browser_icon(
        fb,
        1512,
        965,
        3
    );

    rr_text(
        fb,
        1585,
        997,
        "IMAGE MEDIA",
        RR_BGRA(203,203,203,205),
        0
    );
}




#define RESUME_FILE "/data/ps5_media_resume.txt"

void save_resume_position(void) {
    double pos;

    if (!current_media_path[0]) return;

    pos = resume_base_offset_seconds + prospero_media_clock_seconds();
    if (pos < 10.0) return;
    if (media_duration_sec > 30.0 && pos > media_duration_sec - 20.0) return;

    FILE *f = fopen(RESUME_FILE, "w");
    if (!f) return;

    fprintf(f, "%.3f\n%s\n", pos, current_media_path);
    fclose(f);
}

double load_resume_position(const char *path) {
    FILE *f = fopen(RESUME_FILE, "r");
    if (!f) return 0.0;

    double pos = 0.0;
    char saved_path[512];

    if (fscanf(f, "%lf\n", &pos) != 1) {
        fclose(f);
        return 0.0;
    }

    if (!fgets(saved_path, sizeof(saved_path), f)) {
        fclose(f);
        return 0.0;
    }

    fclose(f);

    saved_path[strcspn(saved_path, "\r\n")] = 0;

    if (strcmp(saved_path, path) != 0)
        return 0.0;

    if (pos < 10.0)
        return 0.0;

    return pos;
}


#if PP_BACKEND_ENABLED
static void pp_p10_scratch_free(void);
#endif

void stop_video_playback(void) {
    save_resume_position();
    player_paused = 0;

    prospero_subtitle_clear();

    if (demux_thread_running) {
        demux_thread_running = 0;
        pthread_join(demux_thread, NULL);
    }

    prospero_embedded_subtitle_close();


    if (video_thread_running) {
        video_thread_running = 0;
        pthread_join(video_thread, NULL);
    }

    if (audio_decode_thread_running) {
        audio_decode_thread_running = 0;
        pthread_join(audio_decode_thread, NULL);
    }

    if (audio_thread_running) {
        audio_thread_running = 0;
        pthread_join(audio_thread, NULL);
    }

    packet_queue_clear(&video_packet_queue);
    packet_queue_clear(&audio_packet_queue);

    audio_queue_count = 0;
    audio_queue_read = 0;
    audio_queue_write = 0;
    audio_accum_pos = 0;

    if (video_pending_pkt) {
        av_packet_free(&video_pending_pkt);
        video_pending_pkt = NULL;
    }

    if (play_sws) {
        sws_freeContext(play_sws);
        play_sws = NULL;
    }

    if (play_frame) {
        av_frame_free(&play_frame);
        play_frame = NULL;
    }

    if (play_pkt) {
        av_packet_free(&play_pkt);
        play_pkt = NULL;
    }

    if (audio_handle >= 1) {
        sceAudioOutClose(audio_handle);
        audio_handle = -1;
    }

    prospero_audio_resampler_destroy();

    if (audio_ctx) {
        avcodec_free_context(&audio_ctx);
        audio_ctx = NULL;
    }

    if (play_ctx) {
        avcodec_free_context(&play_ctx);
        play_ctx = NULL;
    }

    if (play_fmt) {
        avformat_close_input(&play_fmt);
        play_fmt = NULL;
    }

    video_decode_ready = 0;
    video_decode_done = 0;
    video_stream_index = -1;
    audio_stream_index = -1;
    prospero_music_mode = 0;

    video_frame_loaded = 0;
    audio_samples_played = 0;
    audio_samples_decoded = 0;
    audio_clock_seconds = 0.0;
    audio_pts_seconds = 0.0;
    video_clock_seconds = 0.0;
    first_audio_pts_seconds = -1.0;
    first_video_pts_seconds = -1.0;

    prospero_playback_finished = 0;
    prospero_finish_candidate_ms = 0;
    prospero_chapters_clear();

    /* Drop scrub demux so the next file does not reuse a stale context */
    prospero_thumbnail_close_context();

#if PP_BACKEND_ENABLED
    pp_playback_on_file_close(&g_pp_pb);
    pp_playback_write_stats_file(&g_pp_pb, "/mnt/usb0/pp_playback_stats.txt");
    /*
     * Always return to 1080 UI surface after any session.
     * Main loop applies this on the next frame (no buffer held here).
     * Also clear V8 backend so menus never tile at 4K.
     */
    g_pp_backend = PP_BACKEND_1080_STANDARD;
    pp_playback_set_backend(&g_pp_pb, PP_BACKEND_1080_STANDARD);
    if (g_vo_w != 1920u || g_vo_h != 1080u || g_pending_vo_reconfig) {
        pp_product_request_vo(1920, 1080, 2, PP_BACKEND_1080_STANDARD,
                              WIDTH, HEIGHT);
    }
    pp_p10_scratch_free();
    g_4k_diag_active = 0;
    g_4k_suppress_audio = 0;
    g_4k_suppress_ui = 0;
    g_first_frame_bc_done = 0;
#endif

}


#if PP_BACKEND_ENABLED
/*
 * Fast path for 10-bit planar 4:2:0: right-shift to 8-bit yuv420p, then use
 * the normal convert (not full swscale YUV->RGBA). Main10 soft-decode is still
 * heavy; this removes the second bottleneck.
 */
static uint8_t *s_p10_y = NULL;
static uint8_t *s_p10_u = NULL;
static uint8_t *s_p10_v = NULL;
static int s_p10_w = 0;
static int s_p10_h = 0;
static int s_p10_pack_toasted = 0;

static void pp_p10_scratch_free(void)
{
    free(s_p10_y); s_p10_y = NULL;
    free(s_p10_u); s_p10_u = NULL;
    free(s_p10_v); s_p10_v = NULL;
    s_p10_w = 0;
    s_p10_h = 0;
    s_p10_pack_toasted = 0;
}

static int pp_p10_scratch_ensure(int w, int h)
{
    size_t ysz, csz;
    if (w <= 0 || h <= 0 || (w & 1) || (h & 1))
        return -1;
    if (s_p10_y && s_p10_w == w && s_p10_h == h)
        return 0;
    pp_p10_scratch_free();
    ysz = (size_t)w * (size_t)h;
    csz = (size_t)(w / 2) * (size_t)(h / 2);
    s_p10_y = (uint8_t *)malloc(ysz);
    s_p10_u = (uint8_t *)malloc(csz);
    s_p10_v = (uint8_t *)malloc(csz);
    if (!s_p10_y || !s_p10_u || !s_p10_v) {
        pp_p10_scratch_free();
        return -2;
    }
    s_p10_w = w;
    s_p10_h = h;
    return 0;
}

static void pp_pack_plane_u16_to_u8(const uint8_t *src, int src_stride,
                                    uint8_t *dst, int dst_stride,
                                    int width, int height, int shift)
{
    int y, x;
    for (y = 0; y < height; y++) {
        const uint16_t *s = (const uint16_t *)(src + (size_t)y * (size_t)src_stride);
        uint8_t *d = dst + (size_t)y * (size_t)dst_stride;
        /* Unroll 8-wide — 10-bit pack was a multi-ms stall before convert */
        x = 0;
        for (; x + 8 <= width; x += 8) {
            d[x + 0] = (uint8_t)(s[x + 0] >> shift);
            d[x + 1] = (uint8_t)(s[x + 1] >> shift);
            d[x + 2] = (uint8_t)(s[x + 2] >> shift);
            d[x + 3] = (uint8_t)(s[x + 3] >> shift);
            d[x + 4] = (uint8_t)(s[x + 4] >> shift);
            d[x + 5] = (uint8_t)(s[x + 5] >> shift);
            d[x + 6] = (uint8_t)(s[x + 6] >> shift);
            d[x + 7] = (uint8_t)(s[x + 7] >> shift);
        }
        for (; x < width; x++)
            d[x] = (uint8_t)(s[x] >> shift);
    }
}

static int pp_map_yuv420p10_to_8(const AVFrame *frame, pp_frame *out, int64_t pts_us)
{
    int w = frame->width;
    int h = frame->height;
    int cw = w / 2;
    int ch = h / 2;
    const int shift = 2; /* 10-bit in low bits of uint16 */

    if (pp_p10_scratch_ensure(w, h) != 0)
        return -3;
    if (!frame->data[0] || !frame->data[1] || !frame->data[2])
        return -4;

    pp_pack_plane_u16_to_u8(frame->data[0], frame->linesize[0],
                            s_p10_y, w, w, h, shift);
    pp_pack_plane_u16_to_u8(frame->data[1], frame->linesize[1],
                            s_p10_u, cw, cw, ch, shift);
    pp_pack_plane_u16_to_u8(frame->data[2], frame->linesize[2],
                            s_p10_v, cw, cw, ch, shift);

    memset(out, 0, sizeof(*out));
    out->format = PP_FRAME_YUV420P;
    out->width = (uint32_t)w;
    out->height = (uint32_t)h;
    out->pts_us = pts_us;
    out->planes[0] = s_p10_y;
    out->planes[1] = s_p10_u;
    out->planes[2] = s_p10_v;
    out->strides[0] = w;
    out->strides[1] = cw;
    out->strides[2] = cw;

    if (!s_p10_pack_toasted) {
        toast("CONVERT", "10-bit pack -> fast path");
        s_p10_pack_toasted = 1;
    }
    return 0;
}

static int pp_map_avframe(const AVFrame *frame, pp_frame *out, int64_t pts_us)
{
    if (!frame || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->width = (uint32_t)frame->width;
    out->height = (uint32_t)frame->height;
    out->pts_us = pts_us;
    if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P) {
        out->format = PP_FRAME_YUV420P;
        out->planes[0] = frame->data[0];
        out->planes[1] = frame->data[1];
        out->planes[2] = frame->data[2];
        out->strides[0] = frame->linesize[0];
        out->strides[1] = frame->linesize[1];
        out->strides[2] = frame->linesize[2];
        return 0;
    }
    if (frame->format == AV_PIX_FMT_NV12) {
        out->format = PP_FRAME_NV12;
        out->planes[0] = frame->data[0];
        out->planes[1] = frame->data[1];
        out->strides[0] = frame->linesize[0];
        out->strides[1] = frame->linesize[1];
        return 0;
    }
    /* 10-bit 4:2:0 planar -> pack to 8-bit, keep fast convert */
    if (frame->format == AV_PIX_FMT_YUV420P10LE ||
        frame->format == AV_PIX_FMT_YUV420P10BE) {
        return pp_map_yuv420p10_to_8(frame, out, pts_us);
    }
    return -2;
}

static int64_t pp_frame_pts_us(const AVFrame *frame)
{
    int64_t pts;
    if (!frame || !play_fmt || video_stream_index < 0)
        return 0;
    if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
        pts = frame->best_effort_timestamp;
    else if (frame->pts != AV_NOPTS_VALUE)
        pts = frame->pts;
    else
        return (int64_t)(video_clock_seconds * 1000000.0);
    return av_rescale_q(pts, play_fmt->streams[video_stream_index]->time_base,
                        (AVRational){1, 1000000});
}
#endif

int convert_frame_to_rgb(AVFrame *frame) {
    if (!frame) return 0;

#if PP_BACKEND_ENABLED
    if (g_pp_pb.active) {
        pp_frame pf;
        int64_t pts_us = pp_frame_pts_us(frame);
        /* Wait briefly for deferred 4K VO — do not convert into dying buffers */
        if (!g_vo_decode_gate) {
            int spins = 0;
            while (!g_vo_decode_gate && spins < 200) {
                usleep(1000);
                spins++;
            }
            if (!g_vo_decode_gate)
                return 0; /* drop frame; VO not ready */
        }
        {
            if (!g_first_frame_bc_done && g_vo_decode_gate) {
                char d[80];
                const char *pn = av_get_pix_fmt_name((enum AVPixelFormat)frame->format);
                snprintf(d, sizeof(d), "fmt=%s %dx%d be=%d",
                         pn ? pn : "?", frame->width, frame->height,
                         (int)g_pp_backend);
                pp_stage_bc_checkpoint("009_FIRST_FRAME_ENTER", d);
                g_first_frame_bc_done = 1;
            }
        }
        if (pp_map_avframe(frame, &pf, pts_us) == 0) {
            g_pp_pb.cfg.aspect = prospero_view_mode_to_aspect();
            g_pp_pb.stats.aspect = (int)g_pp_pb.cfg.aspect;
            (void)pp_playback_push_frame(&g_pp_pb, &pf);
            video_frame_loaded = pp_playback_has_display(&g_pp_pb);
            return 1;
        }
        /* 10-bit / odd formats → sws (much slower). One-shot toast. */
        {
            static int s_sws_warned;
            if (!s_sws_warned) {
                const char *pn = av_get_pix_fmt_name((enum AVPixelFormat)frame->format);
                char msg[80];
                snprintf(msg, sizeof(msg), "slow path fmt=%s", pn ? pn : "?");
                toast("CONVERT", msg);
                s_sws_warned = 1;
            }
        }
    }
#endif

    if (video_frame_w != frame->width || video_frame_h != frame->height || video_rotate_pixels[0] == NULL) {
        pthread_mutex_lock(&video_frame_mutex);

        for (int i = 0; i < VIDEO_ROTATE_BUFFERS; i++) {
            if (video_rotate_pixels[i]) {
                free(video_rotate_pixels[i]);
                video_rotate_pixels[i] = NULL;
            }
        }

        video_frame_w = frame->width;
        video_frame_h = frame->height;
        video_rotate_index = 0;
        video_frame_loaded = 0;

        for (int i = 0; i < VIDEO_ROTATE_BUFFERS; i++) {
            video_rotate_pixels[i] = malloc(video_frame_w * video_frame_h * 4);
            if (!video_rotate_pixels[i]) {
                pthread_mutex_unlock(&video_frame_mutex);
                return 0;
            }
        }

        video_frame_pixels = video_rotate_pixels[0];
        pthread_mutex_unlock(&video_frame_mutex);
    }

    if (!play_sws) {
        play_sws = sws_getContext(
            frame->width, frame->height, frame->format,
            frame->width, frame->height, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, NULL, NULL, NULL
        );

        if (!play_sws) return 0;
    }

    int write_index = (video_rotate_index + 1) % VIDEO_ROTATE_BUFFERS;

    uint8_t *dst_data[4] = {(uint8_t*)video_rotate_pixels[write_index], NULL, NULL, NULL};
    int dst_linesize[4] = {video_frame_w * 4, 0, 0, 0};

    sws_scale(play_sws, (const uint8_t * const*)frame->data,
              frame->linesize, 0, frame->height,
              dst_data, dst_linesize);

    pthread_mutex_lock(&video_frame_mutex);
    video_rotate_index = write_index;
    video_frame_pixels = video_rotate_pixels[video_rotate_index];
    video_frame_loaded = 1;
    dbg_swaps++;
    pthread_mutex_unlock(&video_frame_mutex);

    return 1;
}



/* PROSPERO_TRUE_AV_SEEK_START */

static pthread_mutex_t prospero_seek_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static volatile int prospero_seek_pending = 0;
static volatile int prospero_seek_in_progress = 0;

static double prospero_seek_target_seconds = 0.0;
static int prospero_seek_restore_paused = 0;


static int prospero_request_inplace_seek(
    double target_seconds,
    int restore_paused
) {
    if (
        !play_fmt ||
        (video_stream_index < 0 && audio_stream_index < 0) ||
        !current_media_path[0]
    ) {
        return 0;
    }

    if (target_seconds < 0.0) {
        target_seconds = 0.0;
    }

    if (media_duration_sec > 1.0) {
        double maximum =
            media_duration_sec - 1.0;

        if (maximum < 0.0) {
            maximum = 0.0;
        }

        if (target_seconds > maximum) {
            target_seconds = maximum;
        }
    }

    player_paused = 1;
    controls_last_used_ms = now_ms();

    pthread_mutex_lock(
        &prospero_seek_mutex
    );

    prospero_seek_target_seconds =
        target_seconds;

    prospero_seek_restore_paused =
        restore_paused;

    prospero_seek_pending = 1;

    pthread_mutex_unlock(
        &prospero_seek_mutex
    );

    return 1;
}



/* AUDIO_DECODE_THREAD_FORWARD_START */
void *audio_decode_thread_func(void *arg);
/* AUDIO_DECODE_THREAD_FORWARD_END */

static int prospero_process_seek_request(void) {
    double target_seconds;
    int restore_paused;

    pthread_mutex_lock(
        &prospero_seek_mutex
    );

    if (!prospero_seek_pending) {
        pthread_mutex_unlock(
            &prospero_seek_mutex
        );

        return 0;
    }

    target_seconds =
        prospero_seek_target_seconds;

    restore_paused =
        prospero_seek_restore_paused;

    prospero_seek_pending = 0;
    prospero_seek_in_progress = 1;

    pthread_mutex_unlock(
        &prospero_seek_mutex
    );

#if PP_BACKEND_ENABLED
    pp_playback_notify_seek_begin(
        &g_pp_pb,
        (int64_t)(target_seconds * 1000000.0)
    );
#endif

    /*
     * Let decoder/output threads observe player_paused.
     */
    usleep(5000);

    
    /*
     * Clear EOF immediately. This wakes the video and audio decoder
     * loops while the seek and queue reset are being completed.
     */
    video_decode_done = 0;
    video_decode_ready = 1;
    dbg_read_fail = 0;

packet_queue_clear(
        &video_packet_queue
    );

    packet_queue_clear(
        &audio_packet_queue
    );

    if (video_pending_pkt) {
        av_packet_free(
            &video_pending_pkt
        );

        video_pending_pkt = NULL;
    }

    if (video_video_pending_pkt) {
        av_packet_free(
            &video_video_pending_pkt
        );

        video_video_pending_pkt = NULL;
    }

    audio_queue_count = 0;
    audio_queue_read = 0;
    audio_queue_write = 0;
    audio_accum_pos = 0;

    double decoder_seek_seconds =
        target_seconds;

    /*
     * Move slightly backward so inter-frame codecs can begin from
     * a nearby keyframe.
     */
    /* Video: back up slightly for keyframe. Audio-only: seek near target. */
    if (video_stream_index >= 0 && decoder_seek_seconds > 0.50) {
        decoder_seek_seconds -= 0.50;
    }

    int seek_stream =
        video_stream_index >= 0
            ? video_stream_index
            : audio_stream_index;

    AVRational time_base =
        play_fmt->streams[
            seek_stream
        ]->time_base;

    int64_t seek_timestamp =
        (int64_t)(
            decoder_seek_seconds /
            av_q2d(time_base)
        );

    int result =
        av_seek_frame(
            play_fmt,
            seek_stream,
            seek_timestamp,
            AVSEEK_FLAG_BACKWARD
        );

    if (result >= 0) {
        avformat_flush(play_fmt);

        if (play_ctx) {
            avcodec_flush_buffers(
                play_ctx
            );
        }

        if (play_frame) {
            av_frame_unref(play_frame);
        }

        if (play_pkt) {
            av_packet_unref(play_pkt);
        }


        if (audio_ctx) {
            avcodec_flush_buffers(
                audio_ctx
            );
        }

        prospero_audio_resampler_reset();

        if (
            prospero_embedded_subtitle_ctx
        ) {
            avcodec_flush_buffers(
                prospero_embedded_subtitle_ctx
            );
        }

        prospero_embedded_subtitle_reset();

        /*
         * The UI position is base offset plus the new audio clock.
         */
        resume_base_offset_seconds =
            target_seconds;

        audio_samples_played = 0;
        audio_samples_decoded = 0;

        audio_clock_seconds = 0.0;
        audio_pts_seconds = 0.0;
        video_clock_seconds = 0.0;

        first_audio_pts_seconds = -1.0;
        first_video_pts_seconds = -1.0;

        video_decode_done = 0;
        dbg_read_fail = 0;

        /*
         * Older builds allowed the audio decoder thread to exit at
         * EOF. Restart it if this session reached EOF before seeking.
         */
        if (
            audio_ctx &&
            !audio_decode_thread_running
        ) {
            audio_decode_thread_running = 1;

            pthread_create(
                &audio_decode_thread,
                NULL,
                audio_decode_thread_func,
                NULL
            );
        }

    } else {
        toast(
            "SEEK",
            "Decoder seek failed"
        );
    }

    prospero_seek_in_progress = 0;

#if PP_BACKEND_ENABLED
    pp_playback_notify_seek_end(
        &g_pp_pb,
        result >= 0,
        0,
        0
    );
    if (result >= 0 && !restore_paused)
        pp_playback_resume(&g_pp_pb);
#endif

    player_paused =
        restore_paused ? 1 : 0;

    controls_last_used_ms =
        now_ms();

    return result >= 0;
}


void *demux_thread_func(void *arg) {
    (void)arg;

    AVPacket *pkt =
        av_packet_alloc();

    if (!pkt) {
        return NULL;
    }

    while (demux_thread_running) {
        /*
         * Process seek requests before checking the paused state.
         */
        if (prospero_process_seek_request()) {
            av_packet_unref(pkt);
            continue;
        }

        if (player_paused) {
            usleep(1000);
            continue;
        }

        int read_result =
            av_read_frame(
                play_fmt,
                pkt
            );

        if (read_result < 0) {
            /*
             * Keep the demux thread alive so seeking backward from EOF
             * does not require reopening the file.
             */
            video_decode_done = 1;
            usleep(5000);
            continue;
        }

        video_decode_done = 0;

        if (
            pkt->stream_index ==
            video_stream_index
        ) {
            int vq = packet_queue_count(&video_packet_queue);
            /*
             * Do not drop non-keyframes in demux — that freezes for a full GOP
             * (often every 1–2s). Cap queue by waiting only.
             */
            while (
                demux_thread_running &&
                !player_paused &&
                packet_queue_count(
                    &video_packet_queue
                ) >= video_packet_cap
            ) {
                usleep(playback_profile >= 3 ? 300 : 500);
            }

            if (
                demux_thread_running &&
                !player_paused
            ) {
                packet_queue_push(
                    &video_packet_queue,
                    pkt
                );

                dbg_video_packets++;
            }
        } else if (
            pkt->stream_index ==
            audio_stream_index
        ) {
            while (
                demux_thread_running &&
                !player_paused &&
                packet_queue_count(
                    &audio_packet_queue
                ) >= audio_packet_cap
            ) {
                usleep(1000);
            }

            if (
                demux_thread_running &&
                !player_paused
            ) {
                packet_queue_push(
                    &audio_packet_queue,
                    pkt
                );
            }
        }

        if (
            pkt->stream_index ==
            prospero_embedded_subtitle_stream_index
        ) {
            prospero_embedded_subtitle_decode_packet(
                pkt
            );
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    return NULL;
}

/* PROSPERO_TRUE_AV_SEEK_END */



void *video_decode_thread_func(void *arg) {
    long long next_ms = 0;
    double frame_ms = 33.333;

    while (video_thread_running) {
        if (
            player_paused ||
            screen != SCREEN_PLAYER ||
            !video_decode_ready
        ) {
            usleep(1000);
            next_ms = 0;
            continue;
        }

        dbg_video_thread_alive++;

#if PP_BACKEND_ENABLED
        /* Audio-master wait is inside decode_next_video_frame. */
        decode_next_video_frame();
        usleep(playback_profile >= 2 ? 100 : 200);
#else
        if (video_fps > 1.0) frame_ms = 1000.0 / video_fps;

        long long now = now_ms();

        if (next_ms == 0) {
            next_ms = now;
        }

        if (now >= next_ms) {
            decode_next_video_frame();
            next_ms += (long long)(frame_ms);

            static double frac = 0.0;
            frac += frame_ms - (long long)frame_ms;
            if (frac >= 1.0) {
                next_ms += 1;
                frac -= 1.0;
            }
        } else {
            usleep(500);
        }
#endif
    }

    return NULL;
}

static float audio_get_sample(AVFrame *af, int ch, int i) {
    int channels = af->ch_layout.nb_channels;
    if (channels <= 0) channels = 2;

    if (ch >= channels) ch = channels - 1;
    if (ch < 0) ch = 0;

    if (af->format == AV_SAMPLE_FMT_FLTP) {
        float *d = (float*)af->data[ch];
        return d ? d[i] : 0.0f;
    }

    if (af->format == AV_SAMPLE_FMT_FLT) {
        float *d = (float*)af->data[0];
        return d ? d[i * channels + ch] : 0.0f;
    }

    if (af->format == AV_SAMPLE_FMT_S16P) {
        int16_t *d = (int16_t*)af->data[ch];
        return d ? ((float)d[i] / 32768.0f) : 0.0f;
    }

    if (af->format == AV_SAMPLE_FMT_S16) {
        int16_t *d = (int16_t*)af->data[0];
        return d ? ((float)d[i * channels + ch] / 32768.0f) : 0.0f;
    }

    return 0.0f;
}

static int16_t audio_float_to_s16(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)(v * 30000.0f);
}

static void mix_audio_frame_to_queue(
    AVFrame *frame
) {
    if (
        !frame ||
        frame->nb_samples <= 0
    ) {
        return;
    }

    if (
        !prospero_audio_resampler_configure(
            frame
        )
    ) {
        return;
    }

    int input_rate =
        frame->sample_rate > 0
            ? frame->sample_rate
            : prospero_audio_input_rate;

    int64_t delay =
        swr_get_delay(
            prospero_audio_swr,
            input_rate
        );

    int output_capacity =
        (int)av_rescale_rnd(
            delay + frame->nb_samples,
            prospero_audio_output_rate,
            input_rate,
            AV_ROUND_UP
        );

    if (output_capacity <= 0) {
        return;
    }

    uint8_t *output_buffer = NULL;
    int output_linesize = 0;

    if (
        av_samples_alloc(
            &output_buffer,
            &output_linesize,
            2,
            output_capacity,
            AV_SAMPLE_FMT_S16,
            0
        ) < 0
    ) {
        return;
    }

    uint8_t *output_planes[1] = {
        output_buffer
    };

    int converted =
        swr_convert(
            prospero_audio_swr,
            output_planes,
            output_capacity,
            (const uint8_t * const *)
                frame->extended_data,
            frame->nb_samples
        );

    if (converted > 0) {
        int16_t *samples =
            (int16_t *)output_buffer;

        for (
            int index = 0;
            index < converted &&
            audio_decode_thread_running;
            index++
        ) {
            audio_accum[
                audio_accum_pos * 2
            ] =
                samples[index * 2];

            audio_accum[
                audio_accum_pos * 2 + 1
            ] =
                samples[index * 2 + 1];

            audio_accum_pos++;
            audio_samples_decoded++;

            if (
                audio_accum_pos >=
                AUDIO_BLOCK_SAMPLES
            ) {
                while (
                    audio_decode_thread_running &&
                    audio_queue_count >=
                        AUDIO_QUEUE_BLOCKS - 2
                ) {
                    usleep(1000);
                }

                if (!audio_decode_thread_running) {
                    break;
                }

                audio_queue_push(audio_accum);
                audio_accum_pos = 0;
            }
        }
    }

    av_freep(&output_buffer);
}


void *audio_decode_thread_func(void *arg) {
    AVFrame *af = av_frame_alloc();
    if (!af) return NULL;

    while (audio_decode_thread_running) {
        if (player_paused || screen != 2) {
            usleep(1000);
            continue;
        }

        while (audio_decode_thread_running && audio_queue_count > 10) {
            usleep(1000);
        }

        AVPacket *pkt = packet_queue_pop(&audio_packet_queue);
        if (!pkt) {
            /*
             * Do not terminate at EOF. The demux thread stays alive
             * so an in-place backward seek can refill this queue.
             */
            usleep(
                video_decode_done
                    ? 5000
                    : 1000
            );

            continue;
        }

        if (avcodec_send_packet(audio_ctx, pkt) == 0) {
            while (audio_decode_thread_running && avcodec_receive_frame(audio_ctx, af) == 0) {
                if (af->pts != AV_NOPTS_VALUE && play_fmt && audio_stream_index >= 0) {
                    audio_pts_seconds = af->pts * av_q2d(play_fmt->streams[audio_stream_index]->time_base);
                    if (first_audio_pts_seconds < 0.0)
                        first_audio_pts_seconds = audio_pts_seconds;
                }

                mix_audio_frame_to_queue(af);
                av_frame_unref(af);
            }
        }

        av_packet_free(&pkt);
    }

    av_frame_free(&af);
    return NULL;
}


/* PROSPERO_AUDIO_TRACK_SWITCH_START */

#define PROSPERO_AUDIO_TRACK_LIMIT 32

static int prospero_audio_requested_stream = -1;
static char prospero_audio_active_label[128] = {0};

int start_video_playback(const char *path);


static int prospero_audio_collect_streams(
    AVFormatContext *format,
    int *indexes,
    int maximum
) {
    if (!format || !indexes || maximum <= 0) {
        return 0;
    }

    int count = 0;

    for (
        unsigned int index = 0;
        index < format->nb_streams &&
        count < maximum;
        index++
    ) {
        AVStream *stream =
            format->streams[index];

        if (
            !stream ||
            !stream->codecpar ||
            stream->codecpar->codec_type !=
                AVMEDIA_TYPE_AUDIO
        ) {
            continue;
        }

        if (
            !avcodec_find_decoder(
                stream->codecpar->codec_id
            )
        ) {
            continue;
        }

        indexes[count++] =
            (int)index;
    }

    return count;
}


static void prospero_audio_build_label(
    AVFormatContext *format,
    int selected_stream,
    char *output,
    size_t output_size
) {
    if (!output || output_size == 0) {
        return;
    }

    snprintf(
        output,
        output_size,
        "UNKNOWN AUDIO"
    );

    if (
        !format ||
        selected_stream < 0 ||
        selected_stream >=
            (int)format->nb_streams
    ) {
        return;
    }

    int indexes[
        PROSPERO_AUDIO_TRACK_LIMIT
    ];

    int count =
        prospero_audio_collect_streams(
            format,
            indexes,
            PROSPERO_AUDIO_TRACK_LIMIT
        );

    int ordinal = 0;

    for (int index = 0; index < count; index++) {
        if (indexes[index] == selected_stream) {
            ordinal = index + 1;
            break;
        }
    }

    AVStream *stream =
        format->streams[selected_stream];

    AVCodecParameters *parameters =
        stream->codecpar;

    const AVDictionaryEntry *language =
        av_dict_get(
            stream->metadata,
            "language",
            NULL,
            0
        );

    const char *language_text =
        (
            language &&
            language->value &&
            language->value[0]
        )
            ? language->value
            : "und";

    const char *codec_name =
        avcodec_get_name(
            parameters->codec_id
        );

    int channels =
        parameters->ch_layout.nb_channels;

    snprintf(
        output,
        output_size,
        "%d/%d  %s  %s  %dCH",
        ordinal,
        count,
        language_text,
        codec_name
            ? codec_name
            : "unknown",
        channels
    );
}


static void prospero_audio_cycle_track(void)
{
    if (
        screen != SCREEN_PLAYER ||
        !play_fmt
    ) {
        return;
    }

    int indexes[
        PROSPERO_AUDIO_TRACK_LIMIT
    ];

    int count =
        prospero_audio_collect_streams(
            play_fmt,
            indexes,
            PROSPERO_AUDIO_TRACK_LIMIT
        );

    if (count <= 0) {
        toast(
            "AUDIO TRACK",
            "NO SUPPORTED AUDIO"
        );

        return;
    }

    if (count == 1) {
        prospero_audio_build_label(
            play_fmt,
            indexes[0],
            prospero_audio_active_label,
            sizeof(prospero_audio_active_label)
        );

        toast(
            "AUDIO TRACK",
            "ONLY ONE TRACK"
        );

        return;
    }

    int current_slot = 0;

    for (int index = 0; index < count; index++) {
        if (
            indexes[index] ==
            audio_stream_index
        ) {
            current_slot = index;
            break;
        }
    }

    int next_slot =
        (current_slot + 1) % count;

    int next_stream =
        indexes[next_slot];

    int restore_paused =
        player_paused;

    int restore_subtitles =
        prospero_subtitle_enabled;

    int subtitle_request = -2;

    if (
        prospero_subtitle_use_external &&
        prospero_subtitle_count > 0
    ) {
        subtitle_request = -1;
    } else if (
        !prospero_subtitle_use_external &&
        prospero_embedded_subtitle_stream_index >= 0
    ) {
        subtitle_request =
            prospero_embedded_subtitle_stream_index;
    }

    double position =
        resume_base_offset_seconds +
        (
            audio_stream_index >= 0
                ? audio_clock_seconds
                : video_clock_seconds
        );

    if (position < 0.0) {
        position = 0.0;
    }

    if (
        media_duration_sec > 0.0 &&
        position > media_duration_sec
    ) {
        position = media_duration_sec;
    }

    char playback_path[1024];

    snprintf(
        playback_path,
        sizeof(playback_path),
        "%s",
        current_media_path
    );

    prospero_audio_requested_stream =
        next_stream;

    prospero_subtitle_requested_stream =
        subtitle_request;

    /*
     * Playback startup subtracts one second for keyframe preroll.
     */
    requested_resume_seek_pos =
        position > 3.0
            ? position + 1.0
            : position;

    resume_base_offset_seconds =
        position;

    if (
        !start_video_playback(
            playback_path
        )
    ) {
        prospero_audio_requested_stream = -1;

        toast(
            "AUDIO TRACK",
            "SWITCH FAILED"
        );

        return;
    }

    player_paused =
        restore_paused;

    prospero_subtitle_enabled =
        restore_subtitles;

    prospero_audio_build_label(
        play_fmt,
        audio_stream_index,
        prospero_audio_active_label,
        sizeof(prospero_audio_active_label)
    );

    controls_last_used_ms =
        now_ms();

    toast(
        "AUDIO TRACK",
        prospero_audio_active_label
    );
}

/* PROSPERO_AUDIO_TRACK_SWITCH_END */



/* PROSPERO_ERROR_MESSAGES_V4 */

static void prospero_ffmpeg_error(
    const char *stage,
    int error_code
) {
    char reason[64];
    char message[112];

    reason[0] = 0;

    if (
        av_strerror(
            error_code,
            reason,
            sizeof(reason)
        ) < 0 ||
        reason[0] == 0
    ) {
        snprintf(
            reason,
            sizeof(reason),
            "FFmpeg error %d",
            error_code
        );
    }

    snprintf(
        message,
        sizeof(message),
        "%s: %.76s",
        stage ? stage : "Playback",
        reason
    );

    toast(
        "PLAYBACK ERROR",
        message
    );
}


static void prospero_codec_error(
    const char *title,
    enum AVCodecID codec_id,
    const char *detail
) {
    const char *codec =
        avcodec_get_name(codec_id);

    char message[112];

    if (!codec || !codec[0]) {
        codec = "unknown codec";
    }

    snprintf(
        message,
        sizeof(message),
        "%s: %.78s",
        codec,
        detail ? detail : "unsupported"
    );

    toast(
        title,
        message
    );
}


int start_video_playback(const char *path) {
    stop_video_playback();

    prospero_playback_finished = 0;
    prospero_finish_candidate_ms = 0;

    video_view_mode =
        prospero_default_view_mode;

#if PP_BACKEND_ENABLED
    pp_playback_on_file_open(&g_pp_pb);
    {
        pp_aspect_mode am = prospero_view_mode_to_aspect();
        (void)pp_playback_set_output(&g_pp_pb, WIDTH, HEIGHT, am);
    }
#endif


    if (requested_resume_seek_pos <= 0.0)
        resume_base_offset_seconds = 0.0;

    if (video_pending_pkt) {
        av_packet_free(&video_pending_pkt);
        video_pending_pkt = NULL;
    }

    video_clock_seconds = 0.0;
    audio_clock_seconds = 0.0;
    audio_pts_seconds = 0.0;
    first_video_pts_seconds = -1.0;
    first_audio_pts_seconds = -1.0;
    audio_samples_played = 0;
    audio_samples_decoded = 0;
    video_decode_done = 0;

    /*
     * Network streams (DLNA / IPTV): socket timeouts so a dead server
     * cannot hang the player, plus aggressive buffering so Wi-Fi
     * jitter never reaches the decoder:
     *  - 4 MB kernel receive buffer absorbs bitrate bursts
     *  - 16 MB short-seek window: +-10 s seeks are served from the
     *    already-buffered data instead of reopening the HTTP stream
     *  - small probe/analyze limits so playback starts fast
     * Values in microseconds unless noted.
     */
    AVDictionary *prospero_net_opts = NULL;
    g_media_is_network =
        (strncmp(path, "http://", 7) == 0 ||
         strncmp(path, "https://", 8) == 0)
            ? 1
            : 0;
    if (g_media_is_network) {
        av_dict_set(&prospero_net_opts, "timeout", "10000000", 0);
        av_dict_set(&prospero_net_opts, "rw_timeout", "10000000", 0);
        av_dict_set(&prospero_net_opts, "reconnect", "1", 0);
        av_dict_set(&prospero_net_opts, "reconnect_streamed", "1", 0);
        av_dict_set(&prospero_net_opts, "reconnect_on_network_error", "1", 0);
        av_dict_set(&prospero_net_opts, "reconnect_delay_max", "4", 0);
        av_dict_set(&prospero_net_opts, "recv_buffer_size", "4194304", 0);
        av_dict_set(&prospero_net_opts, "tcp_nodelay", "1", 0);
        av_dict_set(&prospero_net_opts, "short_seek_size", "16777216", 0);
        av_dict_set(&prospero_net_opts, "probesize", "2097152", 0);
        av_dict_set(&prospero_net_opts, "analyzeduration", "2000000", 0);
    }

    int prospero_open_result =
        avformat_open_input(
            &play_fmt,
            path,
            NULL,
            prospero_net_opts ? &prospero_net_opts : NULL
        );

    if (prospero_net_opts) {
        av_dict_free(&prospero_net_opts);
    }

    if (prospero_open_result < 0) {
        prospero_ffmpeg_error(
            "Cannot open file",
            prospero_open_result
        );

        return 0;
    }

    int prospero_stream_result =
        avformat_find_stream_info(
            play_fmt,
            NULL
        );

    if (prospero_stream_result < 0) {
        prospero_ffmpeg_error(
            "Cannot read media streams",
            prospero_stream_result
        );

        stop_video_playback();
        return 0;
    }

    prospero_subtitle_load_for_media(
        path
    );

    prospero_embedded_subtitle_open(
        play_fmt
    );

    /* Chapters for L1/R1 jump + timeline marks */
    prospero_chapters_load(play_fmt);
    if (prospero_chapter_count > 0) {
        char chmsg[48];
        snprintf(
            chmsg,
            sizeof(chmsg),
            "%d chapters",
            prospero_chapter_count);
        /* quiet — only toast if many; avoids dual-toast spam on open */
        if (prospero_chapter_count >= 2) {
            /* keep silent on open; user discovers via L1/R1 */
            (void)chmsg;
        }
    }

    /* PROSPERO_SUBTITLE_SOURCE_APPLY_START */

    if (
        prospero_subtitle_requested_stream == -1 &&
        prospero_subtitle_count > 0
    ) {
        prospero_subtitle_use_external = 1;
    } else if (
        prospero_subtitle_requested_stream >= 0 &&
        prospero_embedded_subtitle_stream_index ==
            prospero_subtitle_requested_stream
    ) {
        prospero_subtitle_use_external = 0;
    } else {
        prospero_subtitle_use_external =
            prospero_subtitle_count > 0;
    }

    prospero_subtitle_enabled =
        prospero_auto_subtitles_enabled;
    prospero_subtitle_requested_stream = -2;

    /* PROSPERO_SUBTITLE_SOURCE_APPLY_END */

    load_media_metadata_from_format(play_fmt, path, &current_metadata);
    recent_add_or_update(path, current_metadata.title, 0.0, current_metadata.duration_sec);
    recent_save();

    char stream_msg[64];
    snprintf(stream_msg, sizeof(stream_msg), "streams:%u", play_fmt->nb_streams);

    if (play_fmt->duration > 0)
        media_duration_sec = (double)play_fmt->duration / (double)AV_TIME_BASE;
    else
        media_duration_sec = 0.0;
    /*
     * Pre-scan: decide 4K stage / audio suppress BEFORE opening audio.
     * (Audio streams often appear before video in the container.)
     */
#if PP_BACKEND_ENABLED
    {
        int pre_gate = 0;
        double pre_fps = 30.0;
        for (unsigned int pi = 0; pi < play_fmt->nb_streams; pi++) {
            AVCodecParameters *ppar = play_fmt->streams[pi]->codecpar;
            if (ppar->codec_type != AVMEDIA_TYPE_VIDEO)
                continue;
            if (!pp_v8_is_uhd_size((uint32_t)ppar->width,
                                  (uint32_t)ppar->height))
                continue;
            {
                pp_v8_source_info v8i;
                AVStream *vs = play_fmt->streams[pi];
                int bpp = 8, is_pq = 0, is_hlg = 0;
                if (vs->avg_frame_rate.num > 0)
                    pre_fps = av_q2d(vs->avg_frame_rate);
                else if (vs->r_frame_rate.num > 0)
                    pre_fps = av_q2d(vs->r_frame_rate);
                if (ppar->bits_per_raw_sample > 8)
                    bpp = ppar->bits_per_raw_sample;
                else if (ppar->format == AV_PIX_FMT_YUV420P10LE ||
                         ppar->format == AV_PIX_FMT_YUV420P10BE)
                    bpp = 10;
                if (ppar->color_trc == AVCOL_TRC_SMPTE2084)
                    is_pq = 1;
                if (ppar->color_trc == AVCOL_TRC_ARIB_STD_B67)
                    is_hlg = 1;
                memset(&v8i, 0, sizeof(v8i));
                v8i.width = (uint32_t)ppar->width;
                v8i.height = (uint32_t)ppar->height;
                v8i.fps = pre_fps;
                v8i.format = PP_FRAME_YUV420P;
                v8i.bits_per_component = bpp;
                v8i.is_pq = is_pq;
                v8i.is_hlg = is_hlg;
                v8i.has_hdr_metadata = is_pq || is_hlg;
                pre_gate = (pp_v8_source_gate(&v8i) == PP_V8_GATE_OK);
            }
            if (pre_gate && PP_4K_STAGE_WANTS_4K_VO) {
                g_4k_diag_active = 1;
                g_4k_suppress_audio = !PP_4K_STAGE_WANTS_AUDIO;
                g_4k_suppress_ui = !PP_4K_STAGE_WANTS_UI;
                {
                    char d[80];
                    snprintf(d, sizeof(d), "pre stage=%d suppressA=%d",
                             PP_4K_PRODUCT_STAGE, g_4k_suppress_audio);
                    pp_stage_bc_checkpoint("000_4K_PRESCAN", d);
                }
            }
            break;
        }
    }
#endif

    for (unsigned int i = 0; i < play_fmt->nb_streams; i++) {
        AVCodecParameters *par = play_fmt->streams[i]->codecpar;


        if (
            par->codec_type ==
            AVMEDIA_TYPE_AUDIO
        ) {
            /*
             * Open only the first audio stream that FFmpeg and the
             * PS5 output path can initialize successfully.
             */
            if (g_4k_suppress_audio) {
                /* G-H stage < G_AUDIO: video-only isolation */
                continue;
            }
            if (audio_stream_index >= 0) {
                continue;
            }

            if (
                prospero_audio_requested_stream >= 0 &&
                (int)i != prospero_audio_requested_stream
            ) {
                continue;
            }

            int audio_ch =
                par->ch_layout.nb_channels;

            int audio_rate =
                par->sample_rate;

            const AVCodec *adec =
                avcodec_find_decoder(
                    par->codec_id
                );

            if (!adec) {
                prospero_codec_error(
                    "AUDIO UNSUPPORTED",
                    par->codec_id,
                    "decoder unavailable; video may continue"
                );

                continue;
            }

            AVCodecContext *candidate =
                avcodec_alloc_context3(
                    adec
                );

            if (!candidate) {
                continue;
            }

            if (
                avcodec_parameters_to_context(
                    candidate,
                    par
                ) < 0
            ) {
                avcodec_free_context(
                    &candidate
                );

                continue;
            }

            if (
                avcodec_open2(
                    candidate,
                    adec,
                    NULL
                ) < 0
            ) {
                avcodec_free_context(
                    &candidate
                );

                prospero_codec_error(
                    "AUDIO ERROR",
                    par->codec_id,
                    "decoder could not initialize"
                );

                continue;
            }

            sceAudioOutInit();

            int candidate_handle =
                sceAudioOutOpen(
                    0xFF,
                    0,
                    0,
                    AUDIO_BLOCK_SAMPLES,
                    48000,
                    1
                );

            if (candidate_handle < 1) {
                avcodec_free_context(
                    &candidate
                );

                toast(
                    "AUDIO OUTPUT ERROR",
                    "PS5 stereo output could not start"
                );

                continue;
            }

            audio_ctx =
                candidate;

            audio_handle =
                candidate_handle;

            audio_stream_index =
                (int)i;

            prospero_audio_build_label(
                play_fmt,
                audio_stream_index,
                prospero_audio_active_label,
                sizeof(prospero_audio_active_label)
            );

            detected_audio_rate =
                audio_rate > 0
                    ? audio_rate
                    : 48000;

            audio_queue_read = 0;
            audio_queue_write = 0;
            audio_queue_count = 0;
            audio_accum_pos = 0;

            audio_samples_played = 0;
            audio_samples_decoded = 0;

            audio_clock_seconds = 0.0;
            audio_pts_seconds = 0.0;

            first_audio_pts_seconds = -1.0;
            first_video_pts_seconds = -1.0;

            /* No AUDIO STREAM toast — stacks with PLAYBACK on 4K open */

            audio_thread_running = 1;

            pthread_create(
                &audio_thread,
                NULL,
                audio_output_thread,
                NULL
            );
        }

        if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            /*
             * UHD soft = wide cinema 4K (e.g. 3840x1920) or full UHD when not
             * on the gated H.264 V8 path. Soft HEVC UHD is the choppy case.
             */
            int is_uhd = (par->width >= 3200) ||
                         ((int64_t)par->width * (int64_t)par->height >=
                          (int64_t)3000 * 1600);
            int is_hevc = (par->codec_id == AV_CODEC_ID_HEVC);
            int is_h264 = (par->codec_id == AV_CODEC_ID_H264);
            int is10 = (par->bits_per_raw_sample > 8) ||
                       (par->format == AV_PIX_FMT_YUV420P10LE) ||
                       (par->format == AV_PIX_FMT_YUV420P10BE);
#ifdef FF_PROFILE_HEVC_MAIN_10
            if (is_hevc && par->profile == FF_PROFILE_HEVC_MAIN_10)
                is10 = 1;
#endif

            if (is_h264 && par->width <= 1920 && par->height <= 1080) {
                playback_profile = 0;
                /*
                 * Was 8/8 — too shallow for YouTube-style H.264 with B-frames
                 * (e.g. Y2JB guide). Demux stalled when the decoder needed
                 * reorder depth → freeze ~1–2s in.
                 */
                video_packet_cap = 48;
                audio_packet_cap = 64;
                decoder_thread_count = 4;
            } else if (is_hevc && par->width <= 1920 && par->height <= 1080) {
                playback_profile = 1;
                video_packet_cap = 48;
                audio_packet_cap = 64;
                decoder_thread_count = 6;
            } else if (pp_v8_is_uhd_size((uint32_t)par->width,
                                         (uint32_t)par->height) ||
                       is_uhd) {
                /*
                 * UHD: leave CPU for audio. H.264 4K soft@1080 needs fewer
                 * video threads so the audio queue does not underrun.
                 */
                playback_profile = is_hevc ? 3 : 2;
                /* Deep enough for continuous decode; not so deep we lag forever */
                video_packet_cap = is_hevc ? 48 : 48;
                audio_packet_cap = is_hevc ? 64 : 64;
                /*
                 * Leave cores for convert pool + audio. 12 HEVC frame threads
                 * + 8 convert spawns was thrashing → freezes every few seconds.
                 */
                decoder_thread_count = is_h264 ? 6 : 8;
                if (decoder_thread_count > 10)
                    decoder_thread_count = 10;
#if PP_BACKEND_ENABLED
                {
                    pp_v8_source_info v8i;
                    double fps = 30.0;
                    AVStream *vs = play_fmt->streams[i];
                    char det[160];
                    int gate_ok;
                    pp_video_backend be;
                    int bpp = 8;
                    int is_pq = 0, is_hlg = 0;
                    uint32_t src_w = (uint32_t)par->width;
                    uint32_t src_h = (uint32_t)par->height;

                    if (vs->avg_frame_rate.num > 0)
                        fps = av_q2d(vs->avg_frame_rate);
                    else if (vs->r_frame_rate.num > 0)
                        fps = av_q2d(vs->r_frame_rate);

                    if (par->bits_per_raw_sample > 8)
                        bpp = par->bits_per_raw_sample;
                    else if (par->format == AV_PIX_FMT_YUV420P10LE ||
                             par->format == AV_PIX_FMT_YUV420P10BE)
                        bpp = 10;
#ifdef FF_PROFILE_HEVC_MAIN_10
                    else if (par->codec_id == AV_CODEC_ID_HEVC &&
                             par->profile == FF_PROFILE_HEVC_MAIN_10)
                        bpp = 10;
#endif
                    if (par->color_trc == AVCOL_TRC_SMPTE2084)
                        is_pq = 1;
                    if (par->color_trc == AVCOL_TRC_ARIB_STD_B67)
                        is_hlg = 1;

                    pp_stage_bc_checkpoint("001_MEDIA_OPEN_BEGIN", "4K source detected");
                    memset(&v8i, 0, sizeof(v8i));
                    v8i.width = src_w;
                    v8i.height = src_h;
                    v8i.fps = fps;
                    v8i.format = PP_FRAME_YUV420P;
                    v8i.bits_per_component = bpp;
                    v8i.is_pq = is_pq;
                    v8i.is_hlg = is_hlg;
                    v8i.has_hdr_metadata = is_pq || is_hlg;
                    v8i.has_dolby_vision = 0;
                    gate_ok = (pp_v8_source_gate(&v8i) == PP_V8_GATE_OK);
                    be = pp_select_video_backend(PP_OUT_4K_SDR, gate_ok);

                    snprintf(det, sizeof(det),
                             "w=%d h=%d fps=%.2f bpp=%d trc=%d gate=%d be=%d",
                             par->width, par->height, fps, bpp,
                             (int)par->color_trc, gate_ok, (int)be);
                    pp_stage_bc_checkpoint("002_MEDIA_INSPECT_OK", det);

                    /*
                     * Progressive product 4K (G-H). Stage 0 = safe 1080.
                     * Stage 1 = 4K VO + V3, no audio/UI.
                     * Stage 2 = 4K VO + V8, no audio/UI.
                     * Stage 3 = + audio. Stage 4 = + UI.
                     */
                    g_4k_diag_active = 0;
                    g_4k_suppress_audio = 0;
                    g_4k_suppress_ui = 0;
                    {
                        char st[96];
                        snprintf(st, sizeof(st), "stage=%d %s gate=%d be=%d",
                                 PP_4K_PRODUCT_STAGE,
                                 pp_4k_stage_name(PP_4K_PRODUCT_STAGE),
                                 gate_ok, (int)be);
                        pp_stage_bc_checkpoint("002B_PRODUCT_STAGE", st);
                    }

                    /*
                     * UHD HEVC (HOTD etc.): always soft@1080 — native 4K VO
                     * costs more convert CPU and steals from audio.
                     * H.264 8-bit full 2160 can try native; cinema 1920-tall
                     * uses soft@1080 for A/V smoothness.
                     */
                    if (is_hevc || bpp > 8 || is_pq || is_hlg ||
                        src_h < 2160u || !gate_ok || !PP_4K_STAGE_WANTS_4K_VO ||
                        !PP_4K_NATIVE_VO) {
                        g_4k_diag_active = 0;
                        g_4k_suppress_audio = 0;
                        g_4k_suppress_ui = 0;
                        pp_product_request_vo(1920, 1080, 2,
                                              PP_BACKEND_1080_STANDARD,
                                              WIDTH, HEIGHT);
                        if (is_hevc)
                            toast("PLAYBACK",
                                  is10 ? "4K HEVC soft@1080" : "4K HEVC soft@1080");
                        else
                            toast("PLAYBACK", "4K soft@1080");
                        pp_stage_bc_checkpoint("007_DECODER_INIT_PENDING",
                                              "UHD soft present 1080");
                    } else {
                        int exact_uhd =
                            (src_w == 3840u && src_h == 2160u);
                        if (exact_uhd && be == PP_BACKEND_4K_V8_FUSED &&
                            PP_4K_V8_PRODUCT_ENABLE)
                            be = PP_BACKEND_4K_V8_FUSED;
                        else
                            be = PP_BACKEND_4K_V3_FALLBACK;
                        g_4k_diag_active = 1;
                        g_4k_suppress_audio = !PP_4K_STAGE_WANTS_AUDIO;
                        g_4k_suppress_ui = !PP_4K_STAGE_WANTS_UI;
                        pp_product_request_vo(3840, 2160, PP_4K_SDR_VO_BUFFERS,
                                              be, 3840, 2160);
                        toast("PLAYBACK",
                              be == PP_BACKEND_4K_V8_FUSED
                                  ? "4K V8 native"
                                  : "4K fit@2160");
                        pp_stage_bc_checkpoint("007_DECODER_INIT_PENDING",
                                              pp_4k_stage_name(PP_4K_PRODUCT_STAGE));
                    }
                }
#else
                toast("PLAYBACK", is_hevc ? "4K HEVC soft" : "4K soft");
#endif
            } else if (is_uhd && is_hevc) {
                playback_profile = 3;
                video_packet_cap = 6;
                audio_packet_cap = 48;
                decoder_thread_count = 10;
                toast("PLAYBACK", "4K HEVC soft@1080");
            } else {
                playback_profile = 2;
                video_packet_cap = 128;
                audio_packet_cap = 128;
                decoder_thread_count = 8;
                if (decoder_thread_count > 16) decoder_thread_count = 16;
            }

            const AVCodec *dec = avcodec_find_decoder(par->codec_id);
            if (!dec) {
                prospero_codec_error(
                    "VIDEO UNSUPPORTED",
                    par->codec_id,
                    "decoder unavailable"
                );
                stop_video_playback();
                return 0;
            }

            play_ctx = avcodec_alloc_context3(dec);
            if (!play_ctx) {
                toast(
                    "VIDEO ERROR",
                    "Could not allocate decoder context"
                );
                stop_video_playback();
                return 0;
            }

            if (avcodec_parameters_to_context(play_ctx, par) < 0) {
                toast(
                    "VIDEO ERROR",
                    "Invalid video stream parameters"
                );
                stop_video_playback();
                return 0;
            }

            /*
             * User playback profile tweaks the auto content knobs.
             * Balanced = auto defaults. Performance = speed. Compatibility = safe.
             * Debug = technical toasts (handled in toast path).
             */
            if (current_profile == PROFILE_PERFORMANCE) {
                if (decoder_thread_count < 10)
                    decoder_thread_count += 2;
                if (video_packet_cap > 24)
                    video_packet_cap = video_packet_cap * 3 / 4;
                if (video_packet_cap < 16)
                    video_packet_cap = 16;
            } else if (current_profile == PROFILE_COMPATIBILITY) {
                if (decoder_thread_count > 4)
                    decoder_thread_count -= 2;
                if (decoder_thread_count < 2)
                    decoder_thread_count = 2;
                video_packet_cap += 16;
                if (video_packet_cap > 96)
                    video_packet_cap = 96;
                audio_packet_cap += 16;
                if (audio_packet_cap > 96)
                    audio_packet_cap = 96;
            }

            play_ctx->thread_count = decoder_thread_count;
            /* Frame threading usually wins for HEVC; slice+frame can thrash. */
            if (par->codec_id == AV_CODEC_ID_HEVC)
                play_ctx->thread_type = FF_THREAD_FRAME;
            else
                play_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

#ifdef AV_CODEC_FLAG2_FAST
            /* Compatibility: full quality decode. Else speed path for hard content. */
            if (current_profile != PROFILE_COMPATIBILITY) {
                if (playback_profile >= 2 ||
                    par->codec_id == AV_CODEC_ID_HEVC ||
                    current_profile == PROFILE_PERFORMANCE)
                    play_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
            }
#endif

            if (current_profile == PROFILE_COMPATIBILITY) {
                play_ctx->skip_loop_filter = AVDISCARD_DEFAULT;
                play_ctx->skip_frame = AVDISCARD_DEFAULT;
                play_ctx->skip_idct = AVDISCARD_DEFAULT;
            } else if (current_profile == PROFILE_PERFORMANCE &&
                       (playback_profile >= 1 ||
                        par->codec_id == AV_CODEC_ID_HEVC)) {
                play_ctx->skip_loop_filter = AVDISCARD_NONREF;
                play_ctx->skip_frame = AVDISCARD_DEFAULT;
            } else if (playback_profile == 3) {
                /* Soft UHD HEVC only: mild quality trade for realtime */
                play_ctx->skip_loop_filter = AVDISCARD_NONREF;
                play_ctx->skip_frame = AVDISCARD_DEFAULT;
            } else if (playback_profile == 2) {
                play_ctx->skip_loop_filter = AVDISCARD_DEFAULT;
                play_ctx->skip_frame = AVDISCARD_DEFAULT;
                play_ctx->skip_idct = AVDISCARD_DEFAULT;
            } else if (par->codec_id == AV_CODEC_ID_HEVC) {
                play_ctx->skip_loop_filter = AVDISCARD_NONREF;
                play_ctx->skip_frame = AVDISCARD_DEFAULT;
            }

            if (avcodec_open2(play_ctx, dec, NULL) < 0) {
                prospero_codec_error(
                    "VIDEO ERROR",
                    par->codec_id,
                    "decoder could not initialize"
                );
                stop_video_playback();
                return 0;
            }
#if PP_BACKEND_ENABLED
            {
                char d[96];
                snprintf(d, sizeof(d), "codec=%d %dx%d thr=%d",
                         (int)par->codec_id, par->width, par->height,
                         decoder_thread_count);
                pp_stage_bc_checkpoint("008_DECODER_OPEN_OK", d);
            }
#endif

            video_stream_index = (int)i;

            AVRational fr = play_fmt->streams[i]->avg_frame_rate;
            if (fr.num > 0 && fr.den > 0) {
                video_fps = (double)fr.num / (double)fr.den;
            } else {
                video_fps = 60.0;
            }

            continue;
        }
    }


    prospero_music_mode = 0;

    if (video_stream_index < 0 && audio_stream_index >= 0) {
        /* Audio-only file — music mode (no video plane required). */
        prospero_music_mode = 1;
        toast("MUSIC", "Now playing");
        pp_stage_bc_checkpoint("008M_MUSIC_MODE", "audio-only playback");
    } else if (video_stream_index < 0) {
        toast(
            "PLAY ERROR",
            "No playable video or audio track"
        );
        stop_video_playback();
        return 0;
    } else if (audio_stream_index < 0 && !g_4k_suppress_audio) {
        /*
         * Common: E-AC3/DDP/Atmos/DTS with FFmpeg built without those decoders.
         * PS5 homebrew libavcodec currently has AAC/MP3/PCM, not eac3.
         */
        toast(
            "AUDIO",
            "Silent: need AAC/MP3 (E-AC3/DDP unsupported)"
        );
        pp_stage_bc_checkpoint("008B_NO_AUDIO", "no usable audio decoder/stream");
    }

    play_pkt = av_packet_alloc();
    play_frame = av_frame_alloc();

    if (!play_pkt || !play_frame) {
        toast(
            "MEMORY ERROR",
            "Could not allocate decoder buffers"
        );
        stop_video_playback();
        return 0;
    }

    player_elapsed = 0;
    player_paused = 0;
    video_decode_ready = (video_stream_index >= 0) ? 1 : 0;
    /* demux sets video_decode_done on EOF for both video and music */
    video_decode_done = 0;

    packet_queue_clear(&video_packet_queue);
    packet_queue_clear(&audio_packet_queue);

    if (requested_resume_seek_pos > 0.0 && play_fmt &&
        (video_stream_index >= 0 || audio_stream_index >= 0)) {
        double seek_pos = requested_resume_seek_pos;
        int seek_stream =
            video_stream_index >= 0
                ? video_stream_index
                : audio_stream_index;

        if (video_stream_index >= 0 && seek_pos > 3.0)
            seek_pos -= 1.0;

        int64_t seek_ts =
            (int64_t)(seek_pos /
                      av_q2d(play_fmt->streams[seek_stream]->time_base));

        if (av_seek_frame(play_fmt, seek_stream, seek_ts,
                          AVSEEK_FLAG_BACKWARD) >= 0) {
            if (play_ctx) avcodec_flush_buffers(play_ctx);
            if (audio_ctx) {
                avcodec_flush_buffers(audio_ctx);
                prospero_audio_resampler_reset();

                if (
                    prospero_embedded_subtitle_ctx
                ) {
                    avcodec_flush_buffers(
                        prospero_embedded_subtitle_ctx
                    );
                }

                prospero_embedded_subtitle_reset();
            }

            packet_queue_clear(&video_packet_queue);
            packet_queue_clear(&audio_packet_queue);

            audio_queue_count = 0;
            audio_queue_read = 0;
            audio_queue_write = 0;
            audio_accum_pos = 0;

            audio_samples_played = 0;
            audio_samples_decoded = 0;
            audio_clock_seconds = 0.0;
            audio_pts_seconds = 0.0;
            video_clock_seconds = 0.0;
            first_audio_pts_seconds = -1.0;
            first_video_pts_seconds = -1.0;
            resume_base_offset_seconds = requested_resume_seek_pos;
        }

        requested_resume_seek_pos = 0.0;
    }

    /*
     * Network streams (DLNA / IPTV / HTTP): demux far ahead of the
     * decoder so socket jitter and slow servers never starve the
     * video thread. Local files keep the modest profile caps.
     */
    if (g_media_is_network) {
        if (video_packet_cap < 256)
            video_packet_cap = 256;
        if (audio_packet_cap < 256)
            audio_packet_cap = 256;
    }

    demux_thread_running = 1;
    video_thread_running = (video_stream_index >= 0) ? 1 : 0;
    audio_decode_thread_running =
        audio_ctx != NULL;

    pthread_create(
        &demux_thread,
        NULL,
        demux_thread_func,
        NULL
    );

    if (video_thread_running) {
        pthread_create(
            &video_thread,
            NULL,
            video_decode_thread_func,
            NULL
        );
    }

    if (audio_decode_thread_running) {
        pthread_create(
            &audio_decode_thread,
            NULL,
            audio_decode_thread_func,
            NULL
        );
    }
    
    prospero_audio_requested_stream = -1;

    return 1;
}

long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void prospero_video_queue_drain_nonkey(int max_packets)
{
    /*
     * Drop only non-keyframes so the decoder never waits on a full flush
     * (flush → freeze until next key every few seconds).
     */
    int n = 0;
    while (n < max_packets) {
        AVPacket *drop = packet_queue_pop(&video_packet_queue);
        if (!drop)
            break;
        if (drop->flags & AV_PKT_FLAG_KEY) {
            /* Put keyframe back as pending so we don't skip the GOP start */
            if (!video_video_pending_pkt)
                video_video_pending_pkt = drop;
            else
                av_packet_free(&drop);
            break;
        }
        av_packet_free(&drop);
        n++;
    }
}

int decode_next_video_frame(void) {
    if (!video_decode_ready || player_paused) return 0;

    /*
     * Audio-master pacing:
     *  - If video is AHEAD of audio: wait (full remaining gap, chunked) so
     *    picture is not "very fast".
     *  - If video is slightly late: present now (no freeze).
     *  - If video is badly late: drain non-keys, still present this frame.
     *  - Always convert (skip-convert = frozen picture).
     *  - Never avcodec_flush mid-stream.
     */
    for (int attempts = 0; attempts < 48; attempts++) {
        int recv_ret = avcodec_receive_frame(play_ctx, play_frame);
        if (recv_ret == 0) {
            double video_rel = 0.0;
            double audio_rel = 0.0;
            double behind = 0.0;
            int wait_iters = 0;

            dbg_video_frames++;
            dbg_last_pts = play_frame->pts;
            perf_decode_frames++;

            if (play_frame->pts != AV_NOPTS_VALUE && play_fmt &&
                video_stream_index >= 0) {
                video_clock_seconds =
                    play_frame->pts *
                    av_q2d(play_fmt->streams[video_stream_index]->time_base);
                if (first_video_pts_seconds < 0.0)
                    first_video_pts_seconds = video_clock_seconds;
            }

            video_rel = video_clock_seconds - first_video_pts_seconds;
            if (video_rel < 0.0)
                video_rel = 0.0;
            audio_rel = audio_clock_seconds;
            behind = audio_rel - video_rel; /* >0 => video late; <0 => video early */

            if (audio_rel > 0.05) {
                /*
                 * Video ahead of audio: wait for audio, but NEVER freeze the
                 * picture if audio clock stops (underrun / 44.1k stall).
                 * Y2JB-class clips froze ~2s in when wait never broke out.
                 */
                double audio_at_wait = audio_rel;
                int stuck_iters = 0;

                while (behind < -0.008 &&
                       !player_paused &&
                       video_decode_ready &&
                       wait_iters < 100) {
                    int sleep_us = (int)((-behind) * 1000000.0);
                    if (sleep_us > 15000)
                        sleep_us = 15000;
                    if (sleep_us < 500)
                        sleep_us = 500;
                    usleep(sleep_us);
                    audio_rel = audio_clock_seconds;
                    behind = audio_rel - video_rel;
                    wait_iters++;
                    if (audio_rel <= audio_at_wait + 0.0005)
                        stuck_iters++;
                    else {
                        audio_at_wait = audio_rel;
                        stuck_iters = 0;
                    }
                    /* ~120ms with no audio progress → present anyway */
                    if (stuck_iters >= 8)
                        break;
                }

                /* Badly late vs audio: drop queued non-keys, still show frame */
                if (behind > 0.45)
                    prospero_video_queue_drain_nonkey(24);
            } else if (video_fps > 1.0) {
                /*
                 * Audio not running yet: pace by nominal frame interval so
                 * we don't race through the open before audio primes.
                 */
                static double s_last_present_host = -1.0;
                struct timespec ts;
                double now_s, target_s, frame_s;

                /* Reset host pacer on a fresh open (no first video PTS yet) */
                if (first_video_pts_seconds < 0.0 &&
                    audio_samples_played == 0)
                    s_last_present_host = -1.0;

                clock_gettime(CLOCK_MONOTONIC, &ts);
                now_s = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
                frame_s = 1.0 / video_fps;
                if (frame_s < 0.016)
                    frame_s = 0.016;
                if (frame_s > 0.050)
                    frame_s = 0.050;
                if (s_last_present_host > 0.0) {
                    target_s = s_last_present_host + frame_s;
                    if (now_s < target_s) {
                        int sleep_us = (int)((target_s - now_s) * 1000000.0);
                        if (sleep_us > 40000)
                            sleep_us = 40000;
                        if (sleep_us > 500)
                            usleep(sleep_us);
                    }
                }
                clock_gettime(CLOCK_MONOTONIC, &ts);
                s_last_present_host =
                    (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
            }

#if !PP_BACKEND_ENABLED
            {
                double avdiff = audio_pts_seconds - video_clock_seconds;
                if (audio_pts_seconds > 0.1 && avdiff < -0.04) {
                    int sleep_us = (int)((-avdiff - 0.04) * 1000000.0);
                    if (sleep_us > 60000)
                        sleep_us = 60000;
                    if (sleep_us < 1000)
                        sleep_us = 1000;
                    usleep(sleep_us);
                }
            }
#endif

            /* Always convert+show — continuous picture */
            convert_frame_to_rgb(play_frame);
            av_frame_unref(play_frame);
            return 1;
        }

        if (!video_video_pending_pkt) {
            video_video_pending_pkt = packet_queue_pop(&video_packet_queue);
        }

        if (!video_video_pending_pkt) {
            if (video_decode_done)
                return 0;
            usleep(100);
            return 1;
        }

        int send_ret = avcodec_send_packet(play_ctx, video_video_pending_pkt);

        if (send_ret == 0) {
            av_packet_free(&video_video_pending_pkt);
            continue;
        }

        if (send_ret == AVERROR(EAGAIN)) {
            usleep(50);
            continue;
        }

        av_packet_free(&video_video_pending_pkt);
    }

    return 1;
}

void draw_video_frame_to_fb(uint32_t *fb, int x, int y, int max_w, int max_h) {
#if PP_BACKEND_ENABLED
    if (g_pp_pb.active && pp_playback_has_display(&g_pp_pb)) {
        /* Already converted + aspect-scaled to output size. */
        if (x == 0 && y == 0 && max_w == WIDTH && max_h == HEIGHT) {
            (void)pp_playback_copy_display(&g_pp_pb, fb, (uint32_t)(WIDTH * 4), WIDTH, HEIGHT);
            return;
        }
        /* Sub-rect: copy full then leave (rare). */
        (void)pp_playback_copy_display(&g_pp_pb, fb, (uint32_t)(WIDTH * 4), WIDTH, HEIGHT);
        return;
    }
#endif
    pthread_mutex_lock(&video_frame_mutex);

    if (!video_frame_loaded || !video_frame_pixels || video_frame_w <= 0 || video_frame_h <= 0) {
        pthread_mutex_unlock(&video_frame_mutex);
        return;
    }

    int src_w = video_frame_w;
    int src_h = video_frame_h;

    int dst_w = max_w;
    int dst_h = max_h;
    int dst_x = x;
    int dst_y = y;

    double scale_w = (double)dst_w / (double)src_w;
    double scale_h = (double)dst_h / (double)src_h;
    double scale = 1.0;

    if (video_view_mode == 0) {
        // FIT: preserve aspect ratio, no crop
        scale = scale_w < scale_h ? scale_w : scale_h;
        dst_w = (int)(src_w * scale);
        dst_h = (int)(src_h * scale);
        dst_x = x + (max_w - dst_w) / 2;
        dst_y = y + (max_h - dst_h) / 2;

        for (int dy = 0; dy < dst_h; dy++) {
            int sy = (int)(dy / scale);
            if (sy < 0) sy = 0;
            if (sy >= src_h) sy = src_h - 1;

            for (int dx = 0; dx < dst_w; dx++) {
                int sx = (int)(dx / scale);
                if (sx < 0) sx = 0;
                if (sx >= src_w) sx = src_w - 1;

                fb[(dst_y + dy) * WIDTH + (dst_x + dx)] = video_frame_pixels[sy * src_w + sx];
            }
        }
    } else if (video_view_mode == 1) {
        // FILL: preserve aspect ratio, crop edges
        scale = scale_w > scale_h ? scale_w : scale_h;

        int scaled_w = (int)(src_w * scale);
        int scaled_h = (int)(src_h * scale);
        int crop_x = (scaled_w - max_w) / 2;
        int crop_y = (scaled_h - max_h) / 2;

        for (int dy = 0; dy < max_h; dy++) {
            int sy = (int)((dy + crop_y) / scale);
            if (sy < 0) sy = 0;
            if (sy >= src_h) sy = src_h - 1;

            for (int dx = 0; dx < max_w; dx++) {
                int sx = (int)((dx + crop_x) / scale);
                if (sx < 0) sx = 0;
                if (sx >= src_w) sx = src_w - 1;

                fb[(y + dy) * WIDTH + (x + dx)] = video_frame_pixels[sy * src_w + sx];
            }
        }
    } else {
        // STRETCH: force full screen
        for (int dy = 0; dy < max_h; dy++) {
            int sy = (int)((double)dy * src_h / max_h);
            if (sy >= src_h) sy = src_h - 1;

            for (int dx = 0; dx < max_w; dx++) {
                int sx = (int)((double)dx * src_w / max_w);
                if (sx >= src_w) sx = src_w - 1;

                fb[(y + dy) * WIDTH + (x + dx)] = video_frame_pixels[sy * src_w + sx];
            }
        }
    }

    pthread_mutex_unlock(&video_frame_mutex);
}
long long perf_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}



int str_contains_ci(const char *s, const char *needle) {
    if (!s || !needle) return 0;
    char a[512], b[128];
    int i;
    for (i = 0; s[i] && i < 511; i++) {
        char c = s[i];
        a[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    a[i] = 0;
    for (i = 0; needle[i] && i < 127; i++) {
        char c = needle[i];
        b[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    b[i] = 0;
    return strstr(a, b) != NULL;
}

void clean_media_title(const char *path, char *line1, size_t line1_sz, char *line2, size_t line2_sz) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;

    char original[256];
    snprintf(original, sizeof(original), "%s", name);

    char *ext = strrchr(original, '.');
    if (ext) *ext = 0;

    char season[16] = "";
    int season_pos = -1;

    for (int i = 0; original[i]; i++) {
        if ((original[i] == 'S' || original[i] == 's') &&
            original[i+1] >= '0' && original[i+1] <= '9' &&
            original[i+2] >= '0' && original[i+2] <= '9' &&
            (original[i+3] == 'E' || original[i+3] == 'e') &&
            original[i+4] >= '0' && original[i+4] <= '9' &&
            original[i+5] >= '0' && original[i+5] <= '9') {
            snprintf(season, sizeof(season), "S%c%cE%c%c", original[i+1], original[i+2], original[i+4], original[i+5]);
            season_pos = i;
            break;
        }
    }

    if (season_pos > 0) {
        char title[256];
        snprintf(title, sizeof(title), "%s", original);
        title[season_pos] = 0;

        while (strlen(title) > 0 &&
              (title[strlen(title)-1] == '.' || title[strlen(title)-1] == '_' || title[strlen(title)-1] == '-' || title[strlen(title)-1] == ' '))
            title[strlen(title)-1] = 0;

        snprintf(line1, line1_sz, "%s", title);
    } else {
        snprintf(line1, line1_sz, "%s", original);
    }

    const char *quality = str_contains_ci(name, "2160") ? "2160p" :
                          str_contains_ci(name, "1080") ? "1080p" :
                          str_contains_ci(name, "720")  ? "720p"  : "";

    const char *codec = (str_contains_ci(name, "hevc") || str_contains_ci(name, "h265") || str_contains_ci(name, "x265")) ? "HEVC" :
                        (str_contains_ci(name, "h264") || str_contains_ci(name, "x264")) ? "H.264" : "";

    const char *ch = str_contains_ci(name, "6ch") ? "6CH" :
                     str_contains_ci(name, "5.1") ? "5.1" :
                     str_contains_ci(name, "2ch") ? "2CH" : "";

    snprintf(line2, line2_sz, "%s%s%s%s%s%s%s",
        season,
        season[0] && quality[0] ? "  " : "",
        quality,
        (season[0] || quality[0]) && codec[0] ? "  " : "",
        codec,
        (season[0] || quality[0] || codec[0]) && ch[0] ? "  " : "",
        ch);
}

void format_time_mmss(char *out, size_t out_size, double sec) {
    if (sec < 0) sec = 0;
    int total = (int)sec;
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int ss = total % 60;

    if (h > 0)
        snprintf(out, out_size, "%d:%02d:%02d", h, m, ss);
    else
        snprintf(out, out_size, "%d:%02d", m, ss);
}


// Prospero modern UI helpers
#define UI_BG        0xFF101010
#define UI_PANEL     0xDD331A07
#define UI_PANEL_2   0xEE242424
#define UI_BORDER    0xFF4B8FE8
#define UI_BLUE      0xFFFFD819
#define UI_BLUE_SOFT 0xFF883A00
#define UI_WHITE     0xFFFFFFFF
#define UI_MUTED     0xFF9BA8B8
#define UI_DARK_TEXT 0xFF101010

void ui_clear_modern(uint32_t *fb) {
    for (int i = 0; i < WIDTH * HEIGHT; i++) fb[i] = UI_BG;
}











static void ui_rr_panel(uint32_t *fb, int x, int y, int w, int h, uint32_t fill, uint32_t border) {
    ui_round_asset(fb, x, y, w, h, 24, fill, border, 0x00000000);
}

static void ui_rr_selected(uint32_t *fb, int x, int y, int w, int h) {
    ui_round_asset(fb, x, y, w, h, 26, 0xEE062A66, 0xFF19D8FF, 0x2219D8FF);
}


static void ui_round_asset(uint32_t *fb, int x, int y, int w, int h, int r, uint32_t fill, uint32_t border, uint32_t glow) {
    (void)r; (void)glow;
    rect(fb, x, y, w, h, fill);
    rect(fb, x, y, w, 2, border);
    rect(fb, x, y + h - 2, w, 2, border);
    rect(fb, x, y, 2, h, border);
    rect(fb, x + w - 2, y, 2, h, border);
}

void ui_panel(uint32_t *fb, int x, int y, int w, int h) {
    ui_rr_panel(fb, x, y, w, h, 0xDD331A07, 0xFFD87B1E);
}

void ui_selected_panel(uint32_t *fb, int x, int y, int w, int h) {
    ui_rr_selected(fb, x, y, w, h);
}

void ui_header_modern(uint32_t *fb, const char *title, const char *subtitle) {
    rect(fb, 70, 55, 5, 65, UI_BLUE);
    draw_text(fb, 95, 55, title, UI_WHITE, 4);
    if (subtitle) draw_text(fb, 98, 105, subtitle, UI_MUTED, 2);
}

void ui_footer_modern(uint32_t *fb, const char *text) {
    rect(fb, 0, 1010, WIDTH, 1, UI_BORDER);
    draw_text(fb, 80, 1030, text, UI_MUTED, 2);
}

void ui_row_modern(uint32_t *fb, int x, int y, int w, int h, int selected) {
    if (selected) ui_selected_panel(fb, x, y, w, h);
    else ui_panel(fb, x, y, w, h);
}



/* PLAYER_OSD_FORWARD_DECLARATIONS_START */
static void rr_fill(
    uint32_t *fb,
    int x,
    int y,
    int w,
    int h,
    uint32_t color
);

static void rr_text(
    uint32_t *fb,
    int x,
    int y,
    const char *text,
    uint32_t color,
    int face
);

static void rr_control(
    uint32_t *fb,
    int x,
    int y,
    int index
);

static int rr_text_width(
    const char *text,
    int face
);
/* PLAYER_OSD_FORWARD_DECLARATIONS_END */


/* SCRUB_FORWARD_DECLARATIONS_START */
static int prospero_scrub_active;
static double prospero_scrub_target;
/* SCRUB_FORWARD_DECLARATIONS_END */


/* PROSPERO_PLAYER_OSD_MODULE_START */

#define PROSPERO_PREVIEW_W 320
#define PROSPERO_PREVIEW_H 180

typedef struct ProsperoPlayerOsdState {
    uint32_t preview_pixels[
        PROSPERO_PREVIEW_W *
        PROSPERO_PREVIEW_H
    ];

    int preview_valid;
    int preview_alpha;

    float preview_scale;
    float marker_x;
    float target_marker_x;

    long long last_capture_ms;
    char captured_path[512];
} ProsperoPlayerOsdState;


static ProsperoPlayerOsdState prospero_player_osd = {
    .preview_valid = 0,
    .preview_alpha = 0,
    .preview_scale = 0.88f,
    .marker_x = 0.0f,
    .target_marker_x = 0.0f,
    .last_capture_ms = 0,
    .captured_path = {0}
};


static uint32_t prospero_osd_blend_pixel(
    uint32_t destination,
    uint32_t source_pixel,
    int opacity
) {
    if (opacity <= 0) {
        return destination;
    }

    if (opacity >= 255) {
        return source_pixel;
    }

    int source_r = source_pixel & 0xFF;
    int source_g = (source_pixel >> 8) & 0xFF;
    int source_b = (source_pixel >> 16) & 0xFF;

    int destination_r = destination & 0xFF;
    int destination_g = (destination >> 8) & 0xFF;
    int destination_b = (destination >> 16) & 0xFF;

    int inverse = 255 - opacity;

    int output_r =
        (
            source_r * opacity +
            destination_r * inverse
        ) / 255;

    int output_g =
        (
            source_g * opacity +
            destination_g * inverse
        ) / 255;

    int output_b =
        (
            source_b * opacity +
            destination_b * inverse
        ) / 255;

    return
        0xFF000000u |
        ((uint32_t)output_b << 16) |
        ((uint32_t)output_g << 8) |
        (uint32_t)output_r;
}


static void prospero_player_osd_capture_frame(void) {
    pthread_mutex_lock(
        &video_frame_mutex
    );

    if (
        !video_frame_loaded ||
        !video_frame_pixels ||
        video_frame_w <= 0 ||
        video_frame_h <= 0
    ) {
        pthread_mutex_unlock(
            &video_frame_mutex
        );

        return;
    }

    int source_w = video_frame_w;
    int source_h = video_frame_h;

    double source_aspect =
        (double)source_w /
        (double)source_h;

    double preview_aspect =
        (double)PROSPERO_PREVIEW_W /
        (double)PROSPERO_PREVIEW_H;

    int crop_x = 0;
    int crop_y = 0;
    int crop_w = source_w;
    int crop_h = source_h;

    if (source_aspect > preview_aspect) {
        crop_w =
            (int)(
                source_h *
                preview_aspect
            );

        crop_x =
            (source_w - crop_w) / 2;
    } else if (source_aspect < preview_aspect) {
        crop_h =
            (int)(
                source_w /
                preview_aspect
            );

        crop_y =
            (source_h - crop_h) / 2;
    }

    for (int y = 0; y < PROSPERO_PREVIEW_H; y++) {
        int source_y =
            crop_y +
            y * crop_h /
            PROSPERO_PREVIEW_H;

        if (source_y >= source_h) {
            source_y = source_h - 1;
        }

        for (int x = 0; x < PROSPERO_PREVIEW_W; x++) {
            int source_x =
                crop_x +
                x * crop_w /
                PROSPERO_PREVIEW_W;

            if (source_x >= source_w) {
                source_x = source_w - 1;
            }

            prospero_player_osd.preview_pixels[
                y * PROSPERO_PREVIEW_W + x
            ] =
                video_frame_pixels[
                    source_y * source_w +
                    source_x
                ];
        }
    }

    pthread_mutex_unlock(
        &video_frame_mutex
    );

    prospero_player_osd.preview_valid = 1;
    prospero_player_osd.last_capture_ms = now_ms();

    snprintf(
        prospero_player_osd.captured_path,
        sizeof(prospero_player_osd.captured_path),
        "%s",
        current_media_path
    );
}


static void prospero_player_osd_update(
    int scrub_active,
    double scrub_target,
    double duration,
    int bar_x,
    int bar_w
) {
    float ratio = 0.0f;

    if (duration > 0.1) {
        ratio =
            (float)(
                scrub_target /
                duration
            );

        if (ratio < 0.0f) {
            ratio = 0.0f;
        }

        if (ratio > 1.0f) {
            ratio = 1.0f;
        }
    }

    prospero_player_osd.target_marker_x =
        (float)bar_x +
        ratio * (float)bar_w;

    if (prospero_player_osd.marker_x <= 0.0f) {
        prospero_player_osd.marker_x =
            prospero_player_osd.target_marker_x;
    }

    prospero_player_osd.marker_x +=
        (
            prospero_player_osd.target_marker_x -
            prospero_player_osd.marker_x
        ) * 0.20f;

    if (scrub_active) {
        if (
            strcmp(
                prospero_player_osd.captured_path,
                current_media_path
            ) != 0
        ) {
            prospero_player_osd.preview_valid = 0;
        }

        if (
            !prospero_player_osd.preview_valid ||
            now_ms() -
            prospero_player_osd.last_capture_ms >
            300
        ) {
            prospero_player_osd_capture_frame();
        }

        prospero_player_osd.preview_alpha += 34;

        if (prospero_player_osd.preview_alpha > 255) {
            prospero_player_osd.preview_alpha = 255;
        }

        prospero_player_osd.preview_scale +=
            (
                1.0f -
                prospero_player_osd.preview_scale
            ) * 0.24f;
    } else {
        prospero_player_osd.preview_alpha -= 36;

        if (prospero_player_osd.preview_alpha < 0) {
            prospero_player_osd.preview_alpha = 0;
        }

        prospero_player_osd.preview_scale +=
            (
                0.94f -
                prospero_player_osd.preview_scale
            ) * 0.22f;
    }
}


static int prospero_player_osd_fill_width(
    int bar_x,
    int bar_w,
    int normal_fill
) {
    if (!prospero_scrub_active) {
        return normal_fill;
    }

    int fill =
        (int)(
            prospero_player_osd.marker_x -
            (float)bar_x
        );

    if (fill < 0) {
        fill = 0;
    }

    if (fill > bar_w) {
        fill = bar_w;
    }

    return fill;
}


static void prospero_player_osd_blit_preview(
    uint32_t *fb,
    int destination_x,
    int destination_y,
    int destination_w,
    int destination_h,
    int opacity
) {
    if (
        !prospero_player_osd.preview_valid ||
        opacity <= 0
    ) {
        return;
    }

    for (int y = 0; y < destination_h; y++) {
        int source_y =
            y *
            PROSPERO_PREVIEW_H /
            destination_h;

        for (int x = 0; x < destination_w; x++) {
            int screen_x = destination_x + x;
            int screen_y = destination_y + y;

            if (
                screen_x < 0 ||
                screen_x >= WIDTH ||
                screen_y < 0 ||
                screen_y >= HEIGHT
            ) {
                continue;
            }

            int source_x =
                x *
                PROSPERO_PREVIEW_W /
                destination_w;

            uint32_t source_pixel =
                prospero_player_osd.preview_pixels[
                    source_y *
                    PROSPERO_PREVIEW_W +
                    source_x
                ];

            uint32_t *destination =
                &fb[
                    screen_y *
                    WIDTH +
                    screen_x
                ];

            *destination =
                prospero_osd_blend_pixel(
                    *destination,
                    source_pixel,
                    opacity
                );
        }
    }
}



/* THUMBNAIL_WORKER_FORWARD_START */

#ifndef PROSPERO_THUMB_W
#define PROSPERO_THUMB_W 320
#endif

#ifndef PROSPERO_THUMB_H
#define PROSPERO_THUMB_H 180
#endif

static int prospero_thumbnail_is_valid(void);
static int prospero_thumbnail_is_loading(void);
static void prospero_thumbnail_close_context(void);

static void prospero_thumbnail_blit(
    uint32_t *fb,
    int destination_x,
    int destination_y,
    int destination_w,
    int destination_h,
    int opacity
);

/* THUMBNAIL_WORKER_FORWARD_END */

static void prospero_player_osd_draw_scrub_preview(
    uint32_t *fb,
    int bar_y,
    int alpha
) {
    if (
        !prospero_scrub_active ||
        prospero_player_osd.preview_alpha <= 0
    ) {
        return;
    }

    int preview_alpha =
        prospero_player_osd.preview_alpha *
        alpha /
        255;

    float scale =
        prospero_player_osd.preview_scale;

    int image_w =
        (int)(
            PROSPERO_THUMB_W *
            scale
        );

    int image_h =
        (int)(
            PROSPERO_THUMB_H *
            scale
        );

    int card_w =
        image_w + 28;

    int card_h =
        image_h + 76;

    int card_x =
        (int)prospero_player_osd.marker_x -
        card_w / 2;

    int minimum_x = 38;
    int maximum_x =
        WIDTH - card_w - 38;

    if (card_x < minimum_x) {
        card_x = minimum_x;
    }

    if (card_x > maximum_x) {
        card_x = maximum_x;
    }

    int card_y =
        bar_y - card_h - 32;

    /*
     * Shadow.
     */
    ui_round_asset(
        fb,
        card_x + 10,
        card_y + 12,
        card_w,
        card_h,
        24,
        RR_BGRA(
            0,
            0,
            0,
            preview_alpha * 90 / 255
        ),
        0,
        0
    );

    /*
     * Glass frame.
     */
    ui_round_asset(
        fb,
        card_x,
        card_y,
        card_w,
        card_h,
        24,
        RR_BGRA(
            1,
            8,
            20,
            preview_alpha * 235 / 255
        ),
        RR_BGRA(
            0,
            205,
            255,
            preview_alpha * 210 / 255
        ),
        RR_BGRA(
            0,
            145,
            255,
            preview_alpha * 72 / 255
        )
    );

    int image_x =
        card_x + 14;

    int image_y =
        card_y + 14;

    rr_fill(
        fb,
        image_x,
        image_y,
        image_w,
        image_h,
        RR_BGRA(
            0,
            2,
            8,
            preview_alpha * 245 / 255
        )
    );

    if (prospero_thumbnail_is_valid()) {
        prospero_thumbnail_blit(
            fb,
            image_x,
            image_y,
            image_w,
            image_h,
            preview_alpha
        );
    } else {
        rr_text(
            fb,
            image_x + image_w / 2 - 78,
            image_y + image_h / 2 - 14,
            prospero_thumbnail_is_loading()
                ? "LOADING PREVIEW"
                : "PREVIEW PENDING",
            RR_BGRA(
                140,
                205,
                230,
                preview_alpha * 220 / 255
            ),
            0
        );
    }

    rr_fill(
        fb,
        card_x + 18,
        card_y + card_h - 49,
        card_w - 36,
        1,
        RR_BGRA(
            0,
            165,
            225,
            preview_alpha * 90 / 255
        )
    );
    if (
        prospero_thumbnail_is_loading() &&
        prospero_thumbnail_is_valid()
    ) {
        int phase =
            (int)(now_ms() / 90) % 8;

        for (int dot = 0; dot < 3; dot++) {
            int dot_alpha =
                dot == phase % 3
                    ? 220
                    : 75;

            rr_fill(
                fb,
                image_x + image_w - 54 + dot * 15,
                image_y + image_h - 22,
                7,
                7,
                RR_BGRA(
                    125,
                    235,
                    255,
                    preview_alpha *
                    dot_alpha /
                    255
                )
            );
        }
    }



    char target_time[32];

    format_time_mmss(
        target_time,
        sizeof(target_time),
        prospero_scrub_target
    );

    rr_text(
        fb,
        card_x + card_w / 2 - 50,
        card_y + card_h - 37,
        target_time,
        RR_BGRA(
            230,
            246,
            255,
            preview_alpha * 245 / 255
        ),
        1
    );

    int marker_x =
        (int)prospero_player_osd.marker_x;

    rr_fill(
        fb,
        marker_x - 1,
        card_y + card_h,
        3,
        bar_y - (card_y + card_h),
        RR_BGRA(
            0,
            205,
            255,
            preview_alpha * 105 / 255
        )
    );
}


static void prospero_player_osd_draw_scrub_handle(
    uint32_t *fb,
    int bar_y,
    int alpha
) {
    if (!prospero_scrub_active) {
        return;
    }

    int marker_x =
        (int)prospero_player_osd.marker_x;

    int pulse_phase =
        (int)(
            now_ms() / 70
        ) % 20;

    int pulse =
        pulse_phase < 10
            ? pulse_phase
            : 20 - pulse_phase;

    int glow_size =
        22 + pulse / 2;

    rr_fill(
        fb,
        marker_x - glow_size / 2,
        bar_y - glow_size / 2 + 4,
        glow_size,
        glow_size,
        RR_BGRA(
            0,
            150,
            255,
            alpha * 38 / 255
        )
    );

    rr_fill(
        fb,
        marker_x - 7,
        bar_y - 7,
        14,
        22,
        RR_BGRA(
            145,
            245,
            255,
            alpha * 245 / 255
        )
    );

    rr_fill(
        fb,
        marker_x - 3,
        bar_y - 3,
        6,
        14,
        RR_BGRA(
            235,
            255,
            255,
            alpha
        )
    );
}

/* PROSPERO_PLAYER_OSD_MODULE_END */




/* THUMBNAIL_STATE_FORWARD_START */

/*
 * Tentative declarations for worker state used by the crossfade
 * publisher. The initialized definitions remain later in the file.
 */
static uint32_t prospero_thumbnail_pixels[
    PROSPERO_THUMB_W *
    PROSPERO_THUMB_H
];

static int prospero_thumbnail_valid;
static int prospero_thumbnail_loading;
static double prospero_thumbnail_display_time;

/* THUMBNAIL_STATE_FORWARD_END */

/* PROSPERO_THUMBNAIL_CROSSFADE_START */

#define PROSPERO_THUMBNAIL_FADE_MS 180

static uint32_t prospero_thumbnail_previous_pixels[
    PROSPERO_THUMB_W *
    PROSPERO_THUMB_H
];

static int prospero_thumbnail_previous_valid = 0;
static long long prospero_thumbnail_transition_started_ms = 0;


static void prospero_thumbnail_begin_transition_locked(
    const uint32_t *new_pixels,
    double timestamp
) {
    if (!new_pixels) {
        return;
    }

    if (prospero_thumbnail_valid) {
        memcpy(
            prospero_thumbnail_previous_pixels,
            prospero_thumbnail_pixels,
            PROSPERO_THUMB_W *
            PROSPERO_THUMB_H *
            sizeof(uint32_t)
        );

        prospero_thumbnail_previous_valid = 1;
    } else {
        prospero_thumbnail_previous_valid = 0;
    }

    memcpy(
        prospero_thumbnail_pixels,
        new_pixels,
        PROSPERO_THUMB_W *
        PROSPERO_THUMB_H *
        sizeof(uint32_t)
    );

    prospero_thumbnail_valid = 1;
    prospero_thumbnail_loading = 0;
    prospero_thumbnail_display_time = timestamp;

    prospero_thumbnail_transition_started_ms =
        now_ms();
}


static int prospero_thumbnail_transition_alpha_locked(void) {
    if (!prospero_thumbnail_previous_valid) {
        return 255;
    }

    long long elapsed =
        now_ms() -
        prospero_thumbnail_transition_started_ms;

    if (elapsed >= PROSPERO_THUMBNAIL_FADE_MS) {
        prospero_thumbnail_previous_valid = 0;
        return 255;
    }

    if (elapsed <= 0) {
        return 0;
    }

    return (int)(
        elapsed * 255 /
        PROSPERO_THUMBNAIL_FADE_MS
    );
}

/* PROSPERO_THUMBNAIL_CROSSFADE_END */

/* PROSPERO_THUMBNAIL_WORKER_START */



static pthread_mutex_t prospero_thumbnail_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static pthread_cond_t prospero_thumbnail_condition =
    PTHREAD_COND_INITIALIZER;

static pthread_t prospero_thumbnail_thread;

static int prospero_thumbnail_thread_started = 0;
static volatile int prospero_thumbnail_thread_running = 0;

static unsigned long prospero_thumbnail_request_serial = 0;
static int prospero_thumbnail_request_pending = 0;

static char prospero_thumbnail_request_path[512] = {0};
static double prospero_thumbnail_request_time = 0.0;

static char prospero_thumbnail_last_path[512] = {0};
static double prospero_thumbnail_last_requested_time = -100000.0;

static uint32_t prospero_thumbnail_pixels[
    PROSPERO_THUMB_W *
    PROSPERO_THUMB_H
];

static int prospero_thumbnail_valid = 0;
static int prospero_thumbnail_loading = 0;
static double prospero_thumbnail_display_time = 0.0;



/* PROSPERO_THUMBNAIL_CACHE_START */

#define PROSPERO_THUMB_CACHE_SIZE 32
#define PROSPERO_THUMB_CACHE_TOLERANCE 1.50

typedef struct ProsperoThumbnailCacheEntry {
    char path[512];
    double timestamp;

    unsigned long long last_used;
    int valid;

    uint32_t pixels[
        PROSPERO_THUMB_W *
        PROSPERO_THUMB_H
    ];
} ProsperoThumbnailCacheEntry;


static ProsperoThumbnailCacheEntry
    prospero_thumbnail_cache[
        PROSPERO_THUMB_CACHE_SIZE
    ];

static unsigned long long
    prospero_thumbnail_cache_clock = 1;

static unsigned long
    prospero_thumbnail_cache_hits = 0;

static unsigned long
    prospero_thumbnail_cache_misses = 0;


/*
 * The caller must already hold prospero_thumbnail_mutex.
 */
static int prospero_thumbnail_cache_find_locked(
    const char *path,
    double timestamp
) {
    if (!path || !path[0]) {
        return -1;
    }

    int best_index = -1;
    double best_difference = 1000000000.0;

    for (
        int index = 0;
        index < PROSPERO_THUMB_CACHE_SIZE;
        index++
    ) {
        ProsperoThumbnailCacheEntry *entry =
            &prospero_thumbnail_cache[index];

        if (!entry->valid) {
            continue;
        }

        if (
            strcmp(entry->path, path) != 0
        ) {
            continue;
        }

        double difference =
            entry->timestamp - timestamp;

        if (difference < 0.0) {
            difference = -difference;
        }

        if (
            difference <=
                PROSPERO_THUMB_CACHE_TOLERANCE &&
            difference < best_difference
        ) {
            best_difference = difference;
            best_index = index;
        }
    }

    if (best_index >= 0) {
        prospero_thumbnail_cache[
            best_index
        ].last_used =
            prospero_thumbnail_cache_clock++;

        prospero_thumbnail_cache_hits++;
    } else {
        prospero_thumbnail_cache_misses++;
    }

    return best_index;
}


/*
 * Copies a cached entry directly into the published display buffer.
 * The caller must already hold prospero_thumbnail_mutex.
 */
static int prospero_thumbnail_cache_publish_locked(
    const char *path,
    double timestamp
) {
    int index =
        prospero_thumbnail_cache_find_locked(
            path,
            timestamp
        );

    if (index < 0) {
        return 0;
    }

    ProsperoThumbnailCacheEntry *entry =
        &prospero_thumbnail_cache[index];

    prospero_thumbnail_begin_transition_locked(
        entry->pixels,
        entry->timestamp
    );

    return 1;
}


/*
 * The caller must already hold prospero_thumbnail_mutex.
 */
static void prospero_thumbnail_cache_store_locked(
    const char *path,
    double timestamp,
    const uint32_t *pixels
) {
    if (
        !path ||
        !path[0] ||
        !pixels
    ) {
        return;
    }

    /*
     * Update an existing nearby entry when possible.
     */
    int destination = -1;

    for (
        int index = 0;
        index < PROSPERO_THUMB_CACHE_SIZE;
        index++
    ) {
        ProsperoThumbnailCacheEntry *entry =
            &prospero_thumbnail_cache[index];

        if (
            entry->valid &&
            strcmp(entry->path, path) == 0
        ) {
            double difference =
                entry->timestamp - timestamp;

            if (difference < 0.0) {
                difference = -difference;
            }

            if (
                difference <=
                PROSPERO_THUMB_CACHE_TOLERANCE
            ) {
                destination = index;
                break;
            }
        }
    }

    /*
     * Prefer an empty slot.
     */
    if (destination < 0) {
        for (
            int index = 0;
            index < PROSPERO_THUMB_CACHE_SIZE;
            index++
        ) {
            if (
                !prospero_thumbnail_cache[
                    index
                ].valid
            ) {
                destination = index;
                break;
            }
        }
    }

    /*
     * Otherwise replace the least-recently-used entry.
     */
    if (destination < 0) {
        unsigned long long oldest =
            prospero_thumbnail_cache[0].last_used;

        destination = 0;

        for (
            int index = 1;
            index < PROSPERO_THUMB_CACHE_SIZE;
            index++
        ) {
            if (
                prospero_thumbnail_cache[
                    index
                ].last_used < oldest
            ) {
                oldest =
                    prospero_thumbnail_cache[
                        index
                    ].last_used;

                destination = index;
            }
        }
    }

    ProsperoThumbnailCacheEntry *entry =
        &prospero_thumbnail_cache[
            destination
        ];

    snprintf(
        entry->path,
        sizeof(entry->path),
        "%s",
        path
    );

    entry->timestamp = timestamp;

    entry->last_used =
        prospero_thumbnail_cache_clock++;

    entry->valid = 1;

    memcpy(
        entry->pixels,
        pixels,
        PROSPERO_THUMB_W *
        PROSPERO_THUMB_H *
        sizeof(uint32_t)
    );
}


/*
 * Useful later for the developer overlay.
 */
static int prospero_thumbnail_cache_count_locked(void) {
    int count = 0;

    for (
        int index = 0;
        index < PROSPERO_THUMB_CACHE_SIZE;
        index++
    ) {
        if (
            prospero_thumbnail_cache[
                index
            ].valid
        ) {
            count++;
        }
    }

    return count;
}

/* PROSPERO_THUMBNAIL_CACHE_END */

/*
 * Persistent FFmpeg context for scrub previews — reopening 4K HEVC every
 * scrub step was the main reason previews felt very slow.
 */
static AVFormatContext *prospero_thumb_fmt = NULL;
static AVCodecContext *prospero_thumb_codec = NULL;
static AVFrame *prospero_thumb_frame = NULL;
static AVPacket *prospero_thumb_packet = NULL;
static int prospero_thumb_stream = -1;
static char prospero_thumb_open_path[512] = {0};

static void prospero_thumbnail_close_context(void)
{
    if (prospero_thumb_packet) {
        av_packet_free(&prospero_thumb_packet);
        prospero_thumb_packet = NULL;
    }
    if (prospero_thumb_frame) {
        av_frame_free(&prospero_thumb_frame);
        prospero_thumb_frame = NULL;
    }
    if (prospero_thumb_codec) {
        avcodec_free_context(&prospero_thumb_codec);
        prospero_thumb_codec = NULL;
    }
    if (prospero_thumb_fmt) {
        avformat_close_input(&prospero_thumb_fmt);
        prospero_thumb_fmt = NULL;
    }
    prospero_thumb_stream = -1;
    prospero_thumb_open_path[0] = 0;
}

static int prospero_thumbnail_ensure_context(const char *path)
{
    unsigned int i;

    if (path && path[0] &&
        prospero_thumb_fmt && prospero_thumb_codec &&
        strcmp(prospero_thumb_open_path, path) == 0)
        return 1;

    prospero_thumbnail_close_context();

    if (!path || !path[0])
        return 0;

    if (avformat_open_input(&prospero_thumb_fmt, path, NULL, NULL) < 0)
        return 0;
    if (avformat_find_stream_info(prospero_thumb_fmt, NULL) < 0) {
        prospero_thumbnail_close_context();
        return 0;
    }

    for (i = 0; i < prospero_thumb_fmt->nb_streams; i++) {
        if (prospero_thumb_fmt->streams[i]->codecpar->codec_type ==
            AVMEDIA_TYPE_VIDEO) {
            prospero_thumb_stream = (int)i;
            break;
        }
    }
    if (prospero_thumb_stream < 0) {
        prospero_thumbnail_close_context();
        return 0;
    }

    {
        AVStream *stream =
            prospero_thumb_fmt->streams[prospero_thumb_stream];
        const AVCodec *decoder =
            avcodec_find_decoder(stream->codecpar->codec_id);
        if (!decoder) {
            prospero_thumbnail_close_context();
            return 0;
        }
        prospero_thumb_codec = avcodec_alloc_context3(decoder);
        if (!prospero_thumb_codec ||
            avcodec_parameters_to_context(
                prospero_thumb_codec, stream->codecpar) < 0) {
            prospero_thumbnail_close_context();
            return 0;
        }
        /* Light threads — previews must not starve main playback */
        prospero_thumb_codec->thread_count = 2;
        prospero_thumb_codec->thread_type = FF_THREAD_FRAME;
#ifdef AV_CODEC_FLAG2_FAST
        prospero_thumb_codec->flags2 |= AV_CODEC_FLAG2_FAST;
#endif
        prospero_thumb_codec->skip_loop_filter = AVDISCARD_ALL;
        if (avcodec_open2(prospero_thumb_codec, decoder, NULL) < 0) {
            prospero_thumbnail_close_context();
            return 0;
        }
    }

    prospero_thumb_frame = av_frame_alloc();
    prospero_thumb_packet = av_packet_alloc();
    if (!prospero_thumb_frame || !prospero_thumb_packet) {
        prospero_thumbnail_close_context();
        return 0;
    }

    snprintf(prospero_thumb_open_path,
             sizeof(prospero_thumb_open_path), "%s", path);
    return 1;
}

static int prospero_thumbnail_decode(
    const char *path,
    double target_seconds,
    uint32_t *output_pixels
) {
    AVStream *stream;
    struct SwsContext *sws = NULL;
    double seek_seconds;
    double time_base_seconds;
    int64_t seek_timestamp;
    int decoded_frames = 0;
    int packet_limit = 96;
    int success = 0;

    if (!path || !path[0] || !output_pixels)
        return 0;

    if (!prospero_thumbnail_ensure_context(path))
        return 0;

    stream = prospero_thumb_fmt->streams[prospero_thumb_stream];
    time_base_seconds = av_q2d(stream->time_base);
    if (time_base_seconds <= 0.0)
        return 0;

    /* Short pre-roll only — long -2s seeks made every scrub step crawl */
    seek_seconds = target_seconds;
    if (seek_seconds > 0.6)
        seek_seconds -= 0.6;
    else
        seek_seconds = 0.0;

    seek_timestamp =
        (int64_t)(seek_seconds / time_base_seconds);

    if (av_seek_frame(prospero_thumb_fmt, prospero_thumb_stream,
                      seek_timestamp, AVSEEK_FLAG_BACKWARD) < 0)
        return 0;

    avformat_flush(prospero_thumb_fmt);
    avcodec_flush_buffers(prospero_thumb_codec);

    while (packet_limit-- > 0 &&
           av_read_frame(prospero_thumb_fmt, prospero_thumb_packet) >= 0) {
        if (prospero_thumb_packet->stream_index != prospero_thumb_stream) {
            av_packet_unref(prospero_thumb_packet);
            continue;
        }

        if (avcodec_send_packet(prospero_thumb_codec,
                                prospero_thumb_packet) < 0) {
            av_packet_unref(prospero_thumb_packet);
            continue;
        }
        av_packet_unref(prospero_thumb_packet);

        while (1) {
            int receive_result =
                avcodec_receive_frame(prospero_thumb_codec,
                                      prospero_thumb_frame);
            if (receive_result == AVERROR(EAGAIN) ||
                receive_result == AVERROR_EOF)
                break;
            if (receive_result < 0)
                break;

            decoded_frames++;
            {
                int64_t frame_timestamp =
                    prospero_thumb_frame->best_effort_timestamp;
                double frame_seconds =
                    frame_timestamp != AV_NOPTS_VALUE
                        ? frame_timestamp * time_base_seconds
                        : seek_seconds;
                int near_target =
                    frame_seconds >= target_seconds - 0.35;
                int timestamp_fallback = decoded_frames >= 6;

                if (!near_target && !timestamp_fallback) {
                    av_frame_unref(prospero_thumb_frame);
                    continue;
                }
            }

            sws = sws_getContext(
                prospero_thumb_frame->width,
                prospero_thumb_frame->height,
                (enum AVPixelFormat)prospero_thumb_frame->format,
                PROSPERO_THUMB_W,
                PROSPERO_THUMB_H,
                AV_PIX_FMT_RGBA,
                SWS_FAST_BILINEAR,
                NULL, NULL, NULL);
            if (!sws) {
                av_frame_unref(prospero_thumb_frame);
                return 0;
            }

            {
                uint8_t *destination_data[4] = {
                    (uint8_t *)output_pixels, NULL, NULL, NULL
                };
                int destination_linesize[4] = {
                    PROSPERO_THUMB_W * 4, 0, 0, 0
                };
                sws_scale(
                    sws,
                    (const uint8_t *const *)prospero_thumb_frame->data,
                    prospero_thumb_frame->linesize,
                    0,
                    prospero_thumb_frame->height,
                    destination_data,
                    destination_linesize);
            }
            sws_freeContext(sws);
            av_frame_unref(prospero_thumb_frame);
            success = 1;
            return success;
        }
    }

    return success;
}


static void *prospero_thumbnail_worker(
    void *argument
) {
    (void)argument;

    uint32_t *local_pixels =
        malloc(
            PROSPERO_THUMB_W *
            PROSPERO_THUMB_H *
            sizeof(uint32_t)
        );

    if (!local_pixels) {
        return NULL;
    }

    while (prospero_thumbnail_thread_running) {
        char path[512];
        double target = 0.0;
        unsigned long serial = 0;

        pthread_mutex_lock(
            &prospero_thumbnail_mutex
        );

        while (
            prospero_thumbnail_thread_running &&
            !prospero_thumbnail_request_pending
        ) {
            pthread_cond_wait(
                &prospero_thumbnail_condition,
                &prospero_thumbnail_mutex
            );
        }

        if (!prospero_thumbnail_thread_running) {
            pthread_mutex_unlock(
                &prospero_thumbnail_mutex
            );

            break;
        }

        serial =
            prospero_thumbnail_request_serial;

        snprintf(
            path,
            sizeof(path),
            "%s",
            prospero_thumbnail_request_path
        );

        target =
            prospero_thumbnail_request_time;

        prospero_thumbnail_request_pending = 0;

        pthread_mutex_unlock(
            &prospero_thumbnail_mutex
        );

        /*
         * Short debounce only — was 150ms and felt very laggy with 4K opens.
         * Persistent demux context makes the decode cheap enough to react faster.
         */
        usleep(35000);

        pthread_mutex_lock(
            &prospero_thumbnail_mutex
        );

        int stale_before_decode =
            serial !=
            prospero_thumbnail_request_serial;

        if (!stale_before_decode) {
            prospero_thumbnail_loading = 1;
        }

        pthread_mutex_unlock(
            &prospero_thumbnail_mutex
        );

        if (stale_before_decode) {
            continue;
        }

        int decoded =
            prospero_thumbnail_decode(
                path,
                target,
                local_pixels
            );

        pthread_mutex_lock(
            &prospero_thumbnail_mutex
        );

        /*
         * Never publish an obsolete frame after the scrubber has moved.
         */
        if (
            serial ==
            prospero_thumbnail_request_serial
        ) {
            if (decoded) {
                prospero_thumbnail_begin_transition_locked(
                    local_pixels,
                    target
                );

                prospero_thumbnail_cache_store_locked(
                    path,
                    target,
                    local_pixels
                );



            }

            prospero_thumbnail_loading = 0;
        }

        pthread_mutex_unlock(
            &prospero_thumbnail_mutex
        );
    }

    free(local_pixels);
    return NULL;
}


static int prospero_thumbnail_start(void) {
    if (prospero_thumbnail_thread_started) {
        return 1;
    }

    prospero_thumbnail_thread_running = 1;

    if (
        pthread_create(
            &prospero_thumbnail_thread,
            NULL,
            prospero_thumbnail_worker,
            NULL
        ) != 0
    ) {
        prospero_thumbnail_thread_running = 0;
        return 0;
    }

    /*
     * This worker lives for the lifetime of the application.
     * Each decode operation opens and closes its own FFmpeg contexts.
     */
    pthread_detach(
        prospero_thumbnail_thread
    );

    prospero_thumbnail_thread_started = 1;
    return 1;
}


static void prospero_thumbnail_request(
    const char *path,
    double target_seconds,
    int scrub_active
) {
    if (!scrub_active) {
        return;
    }

    if (!path || !path[0]) {
        return;
    }

    /*
     * First try the LRU cache. A hit immediately updates the preview
     * and avoids opening a second FFmpeg context.
     */
    pthread_mutex_lock(
        &prospero_thumbnail_mutex
    );

    int cache_hit =
        prospero_thumbnail_cache_publish_locked(
            path,
            target_seconds
        );

    pthread_mutex_unlock(
        &prospero_thumbnail_mutex
    );

    if (cache_hit) {
        snprintf(
            prospero_thumbnail_last_path,
            sizeof(prospero_thumbnail_last_path),
            "%s",
            path
        );

        prospero_thumbnail_last_requested_time =
            target_seconds;

        return;
    }

    if (!prospero_thumbnail_start()) {
        return;
    }

    double difference =
        target_seconds -
        prospero_thumbnail_last_requested_time;

    if (difference < 0.0) {
        difference = -difference;
    }

    int path_changed =
        strcmp(
            prospero_thumbnail_last_path,
            path
        ) != 0;

    /*
     * Coarser scrub steps hit cache more often (tolerance 1.5s).
     * Was 0.25s → thrash decode on every tiny D-pad tick.
     */
    if (
        !path_changed &&
        difference < 0.85
    ) {
        return;
    }

    pthread_mutex_lock(
        &prospero_thumbnail_mutex
    );

    /*
     * Check once more after obtaining the mutex in case the worker
     * completed and cached this target between the first lookup and
     * this point.
     */
    cache_hit =
        prospero_thumbnail_cache_publish_locked(
            path,
            target_seconds
        );

    if (cache_hit) {
        pthread_mutex_unlock(
            &prospero_thumbnail_mutex
        );

        snprintf(
            prospero_thumbnail_last_path,
            sizeof(prospero_thumbnail_last_path),
            "%s",
            path
        );

        prospero_thumbnail_last_requested_time =
            target_seconds;

        return;
    }

    snprintf(
        prospero_thumbnail_request_path,
        sizeof(prospero_thumbnail_request_path),
        "%s",
        path
    );

    prospero_thumbnail_request_time =
        target_seconds;

    prospero_thumbnail_request_serial++;
    prospero_thumbnail_request_pending = 1;
    prospero_thumbnail_loading = 1;

    if (path_changed) {
        prospero_thumbnail_valid = 0;
    }

    pthread_cond_signal(
        &prospero_thumbnail_condition
    );

    pthread_mutex_unlock(
        &prospero_thumbnail_mutex
    );

    snprintf(
        prospero_thumbnail_last_path,
        sizeof(prospero_thumbnail_last_path),
        "%s",
        path
    );

    prospero_thumbnail_last_requested_time =
        target_seconds;
}


static int prospero_thumbnail_is_valid(void) {
    pthread_mutex_lock(
        &prospero_thumbnail_mutex
    );

    int valid =
        prospero_thumbnail_valid;

    pthread_mutex_unlock(
        &prospero_thumbnail_mutex
    );

    return valid;
}


static int prospero_thumbnail_is_loading(void) {
    pthread_mutex_lock(
        &prospero_thumbnail_mutex
    );

    int loading =
        prospero_thumbnail_loading;

    pthread_mutex_unlock(
        &prospero_thumbnail_mutex
    );

    return loading;
}


static void prospero_thumbnail_blit(
    uint32_t *fb,
    int destination_x,
    int destination_y,
    int destination_w,
    int destination_h,
    int opacity
) {
    if (!fb || opacity <= 0) {
        return;
    }

    pthread_mutex_lock(
        &prospero_thumbnail_mutex
    );

    if (!prospero_thumbnail_valid) {
        pthread_mutex_unlock(
            &prospero_thumbnail_mutex
        );

        return;
    }

    int transition_alpha =
        prospero_thumbnail_transition_alpha_locked();

    int old_alpha =
        255 - transition_alpha;

    for (int y = 0; y < destination_h; y++) {
        int source_y =
            y *
            PROSPERO_THUMB_H /
            destination_h;

        for (int x = 0; x < destination_w; x++) {
            int screen_x =
                destination_x + x;

            int screen_y =
                destination_y + y;

            if (
                screen_x < 0 ||
                screen_x >= WIDTH ||
                screen_y < 0 ||
                screen_y >= HEIGHT
            ) {
                continue;
            }

            int source_x =
                x *
                PROSPERO_THUMB_W /
                destination_w;

            int source_index =
                source_y *
                PROSPERO_THUMB_W +
                source_x;

            uint32_t new_pixel =
                prospero_thumbnail_pixels[
                    source_index
                ];

            uint32_t composed_pixel =
                new_pixel;

            if (
                prospero_thumbnail_previous_valid &&
                old_alpha > 0
            ) {
                uint32_t old_pixel =
                    prospero_thumbnail_previous_pixels[
                        source_index
                    ];

                composed_pixel =
                    prospero_osd_blend_pixel(
                        old_pixel,
                        new_pixel,
                        transition_alpha
                    );
            }

            uint32_t *destination =
                &fb[
                    screen_y *
                    WIDTH +
                    screen_x
                ];

            *destination =
                prospero_osd_blend_pixel(
                    *destination,
                    composed_pixel,
                    opacity
                );
        }
    }

    pthread_mutex_unlock(
        &prospero_thumbnail_mutex
    );
}

/* PROSPERO_THUMBNAIL_WORKER_END */


/* PROSPERO_SRT_MODULE_START */

#define PROSPERO_SUBTITLE_MAX_CUES 2048
#define PROSPERO_SUBTITLE_TEXT_SIZE 512
#define PROSPERO_SUBTITLE_MAX_LINES 3
#define PROSPERO_SUBTITLE_LINE_SIZE 160

typedef struct {
    double start_seconds;
    double end_seconds;

    char text[
        PROSPERO_SUBTITLE_TEXT_SIZE
    ];
} ProsperoSubtitleCue;


static ProsperoSubtitleCue prospero_subtitle_cues[
    PROSPERO_SUBTITLE_MAX_CUES
];

static int prospero_subtitle_count = 0;
static int prospero_subtitle_enabled = 1;

static char prospero_subtitle_path[512] = {0};


static void prospero_subtitle_clear(void) {
    prospero_subtitle_count = 0;
    prospero_subtitle_path[0] = 0;
}


static void prospero_subtitle_trim(
    char *text
) {
    if (!text) {
        return;
    }

    size_t length =
        strlen(text);

    while (
        length > 0 &&
        (
            text[length - 1] == '\r' ||
            text[length - 1] == '\n' ||
            text[length - 1] == ' ' ||
            text[length - 1] == '\t'
        )
    ) {
        text[--length] = 0;
    }

    size_t start = 0;

    while (
        text[start] == ' ' ||
        text[start] == '\t'
    ) {
        start++;
    }

    if (start > 0) {
        memmove(
            text,
            text + start,
            strlen(text + start) + 1
        );
    }
}


static int prospero_subtitle_parse_timing(
    const char *line,
    double *start_seconds,
    double *end_seconds
) {
    int start_hour = 0;
    int start_minute = 0;
    int start_second = 0;
    int start_millisecond = 0;

    int end_hour = 0;
    int end_minute = 0;
    int end_second = 0;
    int end_millisecond = 0;

    char start_separator = 0;
    char end_separator = 0;

    int matched =
        sscanf(
            line,
            "%d:%d:%d%c%d --> %d:%d:%d%c%d",
            &start_hour,
            &start_minute,
            &start_second,
            &start_separator,
            &start_millisecond,
            &end_hour,
            &end_minute,
            &end_second,
            &end_separator,
            &end_millisecond
        );

    if (matched != 10) {
        return 0;
    }

    if (
        (
            start_separator != ',' &&
            start_separator != '.'
        ) ||
        (
            end_separator != ',' &&
            end_separator != '.'
        )
    ) {
        return 0;
    }

    *start_seconds =
        start_hour * 3600.0 +
        start_minute * 60.0 +
        start_second +
        start_millisecond / 1000.0;

    *end_seconds =
        end_hour * 3600.0 +
        end_minute * 60.0 +
        end_second +
        end_millisecond / 1000.0;

    return (
        *end_seconds >=
        *start_seconds
    );
}


static void prospero_subtitle_clean_line(
    const char *input,
    char *output,
    size_t output_size
) {
    if (
        !input ||
        !output ||
        output_size == 0
    ) {
        return;
    }

    size_t write_index = 0;
    int inside_angle_tag = 0;
    int inside_brace_tag = 0;

    for (
        size_t index = 0;
        input[index] &&
        write_index + 1 < output_size;
        index++
    ) {
        unsigned char value =
            (unsigned char)input[index];

        if (value == '<') {
            inside_angle_tag = 1;
            continue;
        }

        if (
            inside_angle_tag &&
            value == '>'
        ) {
            inside_angle_tag = 0;
            continue;
        }

        if (value == '{') {
            inside_brace_tag = 1;
            continue;
        }

        if (
            inside_brace_tag &&
            value == '}'
        ) {
            inside_brace_tag = 0;
            continue;
        }

        if (
            inside_angle_tag ||
            inside_brace_tag
        ) {
            continue;
        }

        if (
            value == '\\' &&
            (
                input[index + 1] == 'N' ||
                input[index + 1] == 'n'
            )
        ) {
            output[write_index++] = '\n';
            index++;
            continue;
        }

        if (
            strncmp(
                input + index,
                "&amp;",
                5
            ) == 0
        ) {
            output[write_index++] = '&';
            index += 4;
            continue;
        }

        if (
            strncmp(
                input + index,
                "&lt;",
                4
            ) == 0
        ) {
            output[write_index++] = '<';
            index += 3;
            continue;
        }

        if (
            strncmp(
                input + index,
                "&gt;",
                4
            ) == 0
        ) {
            output[write_index++] = '>';
            index += 3;
            continue;
        }

        if (
            strncmp(
                input + index,
                "&nbsp;",
                6
            ) == 0
        ) {
            output[write_index++] = ' ';
            index += 5;
            continue;
        }

        /*
         * Normalize common UTF-8 punctuation to characters supported
         * by the current UI font.
         */
        if (
            value == 0xE2 &&
            (unsigned char)input[index + 1] == 0x80
        ) {
            unsigned char third =
                (unsigned char)input[index + 2];

            if (
                third == 0x98 ||
                third == 0x99
            ) {
                output[write_index++] = '\'';
                index += 2;
                continue;
            }

            if (
                third == 0x9C ||
                third == 0x9D
            ) {
                output[write_index++] = '"';
                index += 2;
                continue;
            }

            if (
                third == 0x93 ||
                third == 0x94
            ) {
                output[write_index++] = '-';
                index += 2;
                continue;
            }

            if (third == 0xA6) {
                if (
                    write_index + 3 <
                    output_size
                ) {
                    output[write_index++] = '.';
                    output[write_index++] = '.';
                    output[write_index++] = '.';
                }

                index += 2;
                continue;
            }
        }

        if (
            value == 0xC2 &&
            (unsigned char)input[index + 1] == 0xA0
        ) {
            output[write_index++] = ' ';
            index++;
            continue;
        }

        if (value >= 128) {
            output[write_index++] = '?';

            while (
                input[index + 1] &&
                (
                    (
                        (unsigned char)
                        input[index + 1]
                    ) & 0xC0
                ) == 0x80
            ) {
                index++;
            }

            continue;
        }

        if (value == '\t') {
            value = ' ';
        }

        output[write_index++] =
            (char)value;
    }

    output[write_index] = 0;
    prospero_subtitle_trim(output);
}


static void prospero_subtitle_append_text(
    char *destination,
    size_t destination_size,
    const char *line
) {
    if (
        !destination ||
        !line ||
        destination_size == 0
    ) {
        return;
    }

    size_t used =
        strlen(destination);

    if (
        used > 0 &&
        used + 1 < destination_size
    ) {
        destination[used++] = '\n';
        destination[used] = 0;
    }

    if (used + 1 >= destination_size) {
        return;
    }

    snprintf(
        destination + used,
        destination_size - used,
        "%s",
        line
    );
}


static int prospero_subtitle_sidecar_path(
    const char *media_path,
    char *output,
    size_t output_size,
    const char *extension
) {
    if (
        !media_path ||
        !media_path[0] ||
        !output ||
        output_size == 0
    ) {
        return 0;
    }

    snprintf(
        output,
        output_size,
        "%s",
        media_path
    );

    char *last_slash =
        strrchr(output, '/');

    char *last_dot =
        strrchr(output, '.');

    if (
        last_dot &&
        (
            !last_slash ||
            last_dot > last_slash
        )
    ) {
        *last_dot = 0;
    }

    size_t used =
        strlen(output);

    if (
        used + strlen(extension) + 1 >
        output_size
    ) {
        return 0;
    }

    strcat(
        output,
        extension
    );

    return 1;
}


static int prospero_subtitle_load_file(
    const char *path
) {
    FILE *file =
        fopen(
            path,
            "rb"
        );

    if (!file) {
        return 0;
    }

    prospero_subtitle_count = 0;

    char line[1024];
    int first_line = 1;

    while (
        prospero_subtitle_count <
            PROSPERO_SUBTITLE_MAX_CUES &&
        fgets(
            line,
            sizeof(line),
            file
        )
    ) {
        if (
            first_line &&
            (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF
        ) {
            memmove(
                line,
                line + 3,
                strlen(line + 3) + 1
            );
        }

        first_line = 0;
        prospero_subtitle_trim(line);

        if (!line[0]) {
            continue;
        }

        char timing_line[1024];

        if (strstr(line, "-->")) {
            snprintf(
                timing_line,
                sizeof(timing_line),
                "%s",
                line
            );
        } else {
            if (
                !fgets(
                    timing_line,
                    sizeof(timing_line),
                    file
                )
            ) {
                break;
            }

            prospero_subtitle_trim(
                timing_line
            );

            if (
                !strstr(
                    timing_line,
                    "-->"
                )
            ) {
                continue;
            }
        }

        double start_seconds = 0.0;
        double end_seconds = 0.0;

        if (
            !prospero_subtitle_parse_timing(
                timing_line,
                &start_seconds,
                &end_seconds
            )
        ) {
            continue;
        }

        ProsperoSubtitleCue *cue =
            &prospero_subtitle_cues[
                prospero_subtitle_count
            ];

        cue->start_seconds =
            start_seconds;

        cue->end_seconds =
            end_seconds;

        cue->text[0] = 0;

        while (
            fgets(
                line,
                sizeof(line),
                file
            )
        ) {
            prospero_subtitle_trim(line);

            if (!line[0]) {
                break;
            }

            char cleaned[
                PROSPERO_SUBTITLE_TEXT_SIZE
            ];

            prospero_subtitle_clean_line(
                line,
                cleaned,
                sizeof(cleaned)
            );

            if (cleaned[0]) {
                prospero_subtitle_append_text(
                    cue->text,
                    sizeof(cue->text),
                    cleaned
                );
            }
        }

        if (cue->text[0]) {
            prospero_subtitle_count++;
        }
    }

    fclose(file);

    if (prospero_subtitle_count <= 0) {
        return 0;
    }

    snprintf(
        prospero_subtitle_path,
        sizeof(prospero_subtitle_path),
        "%s",
        path
    );

    return prospero_subtitle_count;
}


static int prospero_subtitle_load_for_media(
    const char *media_path
) {
    prospero_subtitle_clear();

    char sidecar_path[512];

    if (
        prospero_subtitle_sidecar_path(
            media_path,
            sidecar_path,
            sizeof(sidecar_path),
            ".srt"
        )
    ) {
        int count =
            prospero_subtitle_load_file(
                sidecar_path
            );

        if (count > 0) {
            char message[96];

            snprintf(
                message,
                sizeof(message),
                "Loaded %d cues",
                count
            );

            toast(
                "SUBTITLES",
                message
            );

            return count;
        }
    }

    if (
        prospero_subtitle_sidecar_path(
            media_path,
            sidecar_path,
            sizeof(sidecar_path),
            ".SRT"
        )
    ) {
        int count =
            prospero_subtitle_load_file(
                sidecar_path
            );

        if (count > 0) {
            char message[96];

            snprintf(
                message,
                sizeof(message),
                "Loaded %d cues",
                count
            );

            toast(
                "SUBTITLES",
                message
            );

            return count;
        }
    }

    prospero_subtitle_clear();
    return 0;
}


static const ProsperoSubtitleCue *
prospero_subtitle_active_cue(
    double position
) {
    int low = 0;
    int high =
        prospero_subtitle_count - 1;

    while (low <= high) {
        int middle =
            low + (high - low) / 2;

        const ProsperoSubtitleCue *cue =
            &prospero_subtitle_cues[middle];

        if (
            position <
            cue->start_seconds
        ) {
            high = middle - 1;
        } else if (
            position >
            cue->end_seconds
        ) {
            low = middle + 1;
        } else {
            return cue;
        }
    }

    return NULL;
}


static int prospero_subtitle_wrap_text(
    const char *text,
    char lines[
        PROSPERO_SUBTITLE_MAX_LINES
    ][
        PROSPERO_SUBTITLE_LINE_SIZE
    ]
) {
    int line_count = 0;
    const char *cursor = text;

    while (
        cursor &&
        *cursor &&
        line_count <
            PROSPERO_SUBTITLE_MAX_LINES
    ) {
        while (*cursor == ' ') {
            cursor++;
        }

        if (*cursor == '\n') {
            cursor++;
            continue;
        }

        if (!*cursor) {
            break;
        }

        int length = 0;
        int last_space = -1;

        while (
            cursor[length] &&
            cursor[length] != '\n' &&
            length < 44
        ) {
            if (cursor[length] == ' ') {
                last_space = length;
            }

            length++;
        }

        if (
            cursor[length] &&
            cursor[length] != '\n' &&
            length >= 44 &&
            last_space > 20
        ) {
            length = last_space;
        }

        if (length <= 0) {
            cursor++;
            continue;
        }

        int copy_length =
            length;

        if (
            copy_length >=
            PROSPERO_SUBTITLE_LINE_SIZE
        ) {
            copy_length =
                PROSPERO_SUBTITLE_LINE_SIZE - 1;
        }

        memcpy(
            lines[line_count],
            cursor,
            copy_length
        );

        lines[line_count][copy_length] = 0;
        prospero_subtitle_trim(
            lines[line_count]
        );

        if (lines[line_count][0]) {
            line_count++;
        }

        cursor += length;

        while (*cursor == ' ') {
            cursor++;
        }

        if (*cursor == '\n') {
            cursor++;
        }
    }

    if (
        cursor &&
        *cursor &&
        line_count ==
            PROSPERO_SUBTITLE_MAX_LINES
    ) {
        char *last =
            lines[
                PROSPERO_SUBTITLE_MAX_LINES - 1
            ];

        size_t length =
            strlen(last);

        if (
            length + 3 <
            PROSPERO_SUBTITLE_LINE_SIZE
        ) {
            strcat(last, "...");
        }
    }

    return line_count;
}


/*
 * Measure subtitle line width with the same glyph advances as rr_text face 1
 * (RR_SUB_*). Fixed 12px/char was too narrow → text sat off-center.
 */
static int prospero_subtitle_text_width(
    const char *text
) {
    int width = 0;

    if (!text) {
        return 0;
    }

    for (const char *q = text; *q; q++) {
        int id = -1;
        char ch = *q;

        for (int i = 0; i < RR_COUNT; i++) {
            if (RR_CHARS[i] == ch) {
                id = i;
                break;
            }
        }

        if (id < 0) {
            /* rr_text uses +12 for unknown glyphs */
            width += 12;
            continue;
        }

        width += RR_SUB_ADV[id];
    }

    return width;
}


static void prospero_subtitle_draw(
    uint32_t *framebuffer,
    double position,
    int osd_visibility
) {
    if (
        !framebuffer ||
        !prospero_subtitle_enabled
    ) {
        return;
    }

    /* Apply user subtitle delay (positive = subs later) */
    double sub_pos =
        position - ((double)prospero_subtitle_delay_ms / 1000.0);

    const char *active_text = NULL;

    char embedded_text[
        PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE
    ] = {0};

    if (prospero_subtitle_use_external) {
        const ProsperoSubtitleCue *cue =
            prospero_subtitle_active_cue(
                sub_pos
            );

        if (cue) {
            active_text =
                cue->text;
        }
    } else if (
        prospero_embedded_subtitle_text_at(
            sub_pos,
            embedded_text,
            sizeof(embedded_text)
        )
    ) {
        active_text =
            embedded_text;
    }

    if (
        !active_text ||
        !active_text[0]
    ) {
        return;
    }

    char lines[
        PROSPERO_SUBTITLE_MAX_LINES
    ][
        PROSPERO_SUBTITLE_LINE_SIZE
    ] = {{0}};

    int line_count =
        prospero_subtitle_wrap_text(
            active_text,
            lines
        );

    if (line_count <= 0) {
        return;
    }

    /*
     * Subtitles move upward while the playback OSD is displayed.
     */
    int bottom_y =
        osd_visibility > 0
            ? 625
            : 910;

    int line_spacing = 72;

    int first_y =
        bottom_y -
        (line_count - 1) *
            line_spacing;

    for (
        int index = 0;
        index < line_count;
        index++
    ) {
        const char *line =
            lines[index];

        int text_width =
            prospero_subtitle_text_width(
                line
            );

        /*
         * Exact horizontal center (width matches face 1 / RR_SUB advances).
         */
        int x =
            (WIDTH - text_width) / 2;
        if (x < 0)
            x = 0;

        int y =
            first_y +
            index * line_spacing;

        /*
         * Soft lower shadow (symmetric-ish so it doesn't pull center).
         */
        rr_text(
            framebuffer,
            x + 2,
            y + 3,
            line,
            RR_BGRA(
                0,
                0,
                0,
                190
            ),
            1
        );

        /*
         * Thin outline around the letters. No background rectangle.
         * face 1 = subtitle glyph set (same as width measure).
         */
        rr_text(
            framebuffer,
            x - 2,
            y,
            line,
            RR_BGRA(
                0,
                0,
                0,
                245
            ),
            1
        );

        rr_text(
            framebuffer,
            x + 2,
            y,
            line,
            RR_BGRA(
                0,
                0,
                0,
                245
            ),
            1
        );

        rr_text(
            framebuffer,
            x,
            y - 2,
            line,
            RR_BGRA(
                0,
                0,
                0,
                245
            ),
            1
        );

        rr_text(
            framebuffer,
            x,
            y + 2,
            line,
            RR_BGRA(
                0,
                0,
                0,
                245
            ),
            1
        );

        /*
         * Main subtitle text.
         */
        rr_text(
            framebuffer,
            x,
            y,
            line,
            RR_BGRA(
                250,
                252,
                255,
                255
            ),
            1
        );
    }
}

/* PROSPERO_SRT_MODULE_END */

/* PROSPERO_SUBTITLE_CONTROLS_START */

static void prospero_subtitle_toggle(void) {
    int available =
        prospero_subtitle_count > 0 ||
        prospero_embedded_subtitle_stream_index >= 0;

    if (!available) {
        prospero_subtitle_enabled = 0;

        toast(
            "SUBTITLES",
            "NO SUPPORTED TEXT TRACK"
        );

        return;
    }

    prospero_subtitle_enabled =
        !prospero_subtitle_enabled;

    toast(
        "SUBTITLES",
        prospero_subtitle_enabled
            ? "ON"
            : "OFF"
    );

    controls_last_used_ms = now_ms();
}


static int prospero_subtitle_collect_tracks(
    int *tracks,
    int capacity
) {
    if (
        !tracks ||
        capacity <= 0 ||
        !play_fmt
    ) {
        return 0;
    }

    int count = 0;

    /*
     * -1 represents the matching external SRT.
     */
    if (
        prospero_subtitle_count > 0 &&
        count < capacity
    ) {
        tracks[count++] = -1;
    }

    for (
        unsigned int index = 0;
        index < play_fmt->nb_streams &&
        count < capacity;
        index++
    ) {
        AVStream *stream =
            play_fmt->streams[index];

        if (
            !stream ||
            !stream->codecpar ||
            stream->codecpar->codec_type !=
                AVMEDIA_TYPE_SUBTITLE
        ) {
            continue;
        }

        if (
            prospero_embedded_subtitle_supported(
                stream->codecpar->codec_id
            )
        ) {
            tracks[count++] = (int)index;
        }
    }

    return count;
}


static void prospero_subtitle_track_label(
    int track,
    char *output,
    size_t output_size
) {
    if (
        !output ||
        output_size == 0
    ) {
        return;
    }

    if (track < 0) {
        snprintf(
            output,
            output_size,
            "EXTERNAL SRT"
        );

        return;
    }

    if (
        !play_fmt ||
        track >= (int)play_fmt->nb_streams
    ) {
        snprintf(
            output,
            output_size,
            "EMBEDDED TEXT"
        );

        return;
    }

    AVStream *stream =
        play_fmt->streams[track];

    const char *language_name =
        "UNSPECIFIED";

    AVDictionaryEntry *language =
        av_dict_get(
            stream->metadata,
            "language",
            NULL,
            0
        );

    if (
        language &&
        language->value &&
        language->value[0]
    ) {
        language_name = language->value;
    }

    const char *codec_name =
        avcodec_get_name(
            stream->codecpar->codec_id
        );

    snprintf(
        output,
        output_size,
        "%s / %s",
        language_name,
        codec_name
            ? codec_name
            : "TEXT"
    );
}


static void prospero_subtitle_cycle_track(void) {
    if (
        screen != SCREEN_PLAYER ||
        !play_fmt ||
        !current_media_path[0]
    ) {
        return;
    }

    int tracks[64];

    int track_count =
        prospero_subtitle_collect_tracks(
            tracks,
            64
        );

    if (track_count <= 0) {
        prospero_subtitle_enabled = 0;

        toast(
            "SUBTITLES",
            "NO SUPPORTED TEXT TRACK"
        );

        return;
    }

    int current_track =
        prospero_subtitle_use_external
            ? -1
            : prospero_embedded_subtitle_stream_index;

    int current_slot = -1;

    for (
        int index = 0;
        index < track_count;
        index++
    ) {
        if (tracks[index] == current_track) {
            current_slot = index;
            break;
        }
    }

    int next_slot =
        current_slot < 0
            ? 0
            : (current_slot + 1) % track_count;

    int next_track =
        tracks[next_slot];

    char label[128];

    prospero_subtitle_track_label(
        next_track,
        label,
        sizeof(label)
    );

    if (
        track_count == 1 &&
        current_slot == 0
    ) {
        prospero_subtitle_enabled = 1;
        toast("SUBTITLES", label);
        return;
    }

    double position =
        resume_base_offset_seconds +
        (
            audio_ctx &&
            audio_handle >= 1
                ? audio_clock_seconds
                : video_clock_seconds
        );

    if (position < 0.0) {
        position = 0.0;
    }

    int restore_paused = player_paused;

    char playback_path[512];

    snprintf(
        playback_path,
        sizeof(playback_path),
        "%s",
        current_media_path
    );

    prospero_subtitle_requested_stream =
        next_track;

    requested_resume_seek_pos = position;
    resume_base_offset_seconds = position;

    if (
        !start_video_playback(
            playback_path
        )
    ) {
        prospero_subtitle_requested_stream = -2;

        toast(
            "SUBTITLES",
            "TRACK CHANGE FAILED"
        );

        return;
    }

    if (restore_paused) {
        player_paused = 1;
    }

    prospero_subtitle_enabled = 1;

    toast(
        "SUBTITLES",
        label
    );

    controls_last_used_ms = now_ms();
}

/* PROSPERO_SUBTITLE_CONTROLS_END */



void draw_player_screen(uint32_t *fb) {
    static int osd_visibility = 0;
    static int music_pulse = 0;

    /*
     * Video: paint decoded frame. Music: now-playing visualizer UI.
     */
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        fb[i] = 0xFF000000;
    }

    if (!prospero_music_mode) {
        draw_video_frame_to_fb(
            fb,
            0,
            0,
            WIDTH,
            HEIGHT
        );
    } else {
        music_pulse = (music_pulse + 1) % 360;
        /* AMOLED black with a barely-visible neutral pulse */
        rr_fill(fb, 0, 0, 1920, 1080, RR_BGRA(0,0,0,255));
        rr_fill(fb, 0, 0, 1920, 1080, RR_BGRA(255, 255, 255, 4 + (music_pulse % 8)));

        rr_fill(fb, 82, 74, 5, 76, RR_BGRA(205,205,205,225));
        rr_fill(fb, 82, 146, 31, 2, RR_BGRA(205,205,205,195));
        rr_text(fb, 122, 70, "NOW PLAYING", RR_BGRA(232, 244, 255, 240), 3);
        rr_text(fb, 124, 132, "MUSIC", RR_BGRA(199,199,199,225), 1);

        /* Center card */
        ui_round_asset(
            fb, 460, 220, 1000, 420, 28,
            RR_BGRA(22,22,22,230),
            RR_BGRA(191,191,191,160),
            RR_BGRA(182,182,182,50)
        );

        rr_fill(fb, 520, 280, 64, 64, RR_BGRA(195,195,195,200));
        rr_text(fb, 620, 290, "PROSPERO MUSIC", RR_BGRA(205,205,205,235), 2);
        rr_text(
            fb, 620, 350,
            player_paused ? "PAUSED" : "PLAYING",
            RR_BGRA(232, 244, 255, 235),
            2
        );

        {
            char codec_line[96];
            const char *cname = "AUDIO";
            if (audio_ctx && audio_ctx->codec)
                cname = audio_ctx->codec->name ? audio_ctx->codec->name : "AUDIO";
            snprintf(
                codec_line, sizeof(codec_line),
                "%s  ·  CROSS PAUSE  ·  L/R SEEK",
                cname
            );
            rr_text(fb, 620, 420, codec_line, RR_BGRA(208,208,208,220), 1);
        }

        /* Simple pulse bars */
        for (int b = 0; b < 24; b++) {
            int h = 20 + ((music_pulse + b * 11) % 80);
            if (player_paused)
                h = 24;
            rr_fill(
                fb,
                520 + b * 36,
                560 - h,
                22,
                h,
                RR_BGRA(0, 180 + (b % 4) * 15, 255, 180)
            );
        }
    }

    long long now = now_ms();

    int wants_controls =
        prospero_music_mode ||
        ((now - controls_last_used_ms < 3500)
        || player_paused
        || show_stats_for_nerds)
        || prospero_scrub_active;

    /*
     * Smooth OSD entrance and disappearance.
     */
    if (wants_controls) {
        if (osd_visibility < 255) {
            osd_visibility += 42;

            if (osd_visibility > 255) {
                osd_visibility = 255;
            }
        }
    } else {
        if (osd_visibility > 0) {
            osd_visibility -= 28;

            if (osd_visibility < 0) {
                osd_visibility = 0;
            }
        }
    }


    double position =
        resume_base_offset_seconds +
        prospero_media_clock_seconds();

    if (position < 0.0) {
        position = 0.0;
    }

    double display_position =
        prospero_scrub_active
            ? prospero_scrub_target
            : position;

    if (display_position < 0.0) {
        display_position = 0.0;
    }

double percentage = 0.0;

    if (media_duration_sec > 0.1) {
        percentage =
            display_position / media_duration_sec;

        if (percentage < 0.0) {
            percentage = 0.0;
        }

        if (percentage > 1.0) {
            percentage = 1.0;
        }
    }

    char title[160];
    char metadata[160];

    title[0] = 0;
    metadata[0] = 0;

    clean_media_title(
        current_media_path,
        title,
        sizeof(title),
        metadata,
        sizeof(metadata)
    );

    if (strlen(title) > 55) {
        title[52] = '.';
        title[53] = '.';
        title[54] = '.';
        title[55] = 0;
    }

    if (strlen(metadata) > 76) {
        metadata[73] = '.';
        metadata[74] = '.';
        metadata[75] = '.';
        metadata[76] = 0;
    }

    if (osd_visibility > 0) {
        int alpha = osd_visibility;

        /*
         * PS Play minimal playback overlay.
         * Fully transparent: no panel, no background at all — the
         * video keeps full priority. Bare line, times and icons only.
         */

        /* Title (left) and state (right), small and quiet. */
        rr_text(
            fb,
            180, 862,
            title,
            RR_BGRA(235, 235, 235, alpha * 200 / 255),
            1
        );

        /* Pause indicator only — the video stays the focus. */
        if (player_paused) {
            const char *state_text = "PAUSED";

            rr_text(
                fb,
                1740 - rr_text_width(state_text, 0),
                864,
                state_text,
                RR_BGRA(255, 220, 120, alpha * 235 / 255),
                0
            );
        }

        if (prospero_scrub_active) {
            char target_time[32];
            char target_label[96];

            format_time_mmss(
                target_time,
                sizeof(target_time),
                prospero_scrub_target
            );

            snprintf(
                target_label,
                sizeof(target_label),
                "SEEK TO  %s",
                target_time
            );

            rr_text(
                fb,
                1740 - rr_text_width(target_label, 1),
                894,
                target_label,
                RR_BGRA(255, 255, 255, alpha * 235 / 255),
                1
            );
        }

        /*
         * Thin progress line.
         */
        int bar_x = 180;
        int bar_w = 1560;
        int bar_y = 924;
        int bar_h = 4;

        prospero_player_osd_update(
            prospero_scrub_active,
            prospero_scrub_target,
            media_duration_sec,
            bar_x,
            bar_w
        );

        prospero_thumbnail_request(
            current_media_path,
            prospero_scrub_target,
            prospero_scrub_active
        );

        int fill_w =
            prospero_player_osd_fill_width(
                bar_x,
                bar_w,
                (int)(bar_w * percentage)
            );

        rr_fill(
            fb,
            bar_x,
            bar_y,
            bar_w,
            bar_h,
            RR_BGRA(255, 255, 255, alpha * 70 / 255)
        );

        if (fill_w > 0) {
            rr_fill(
                fb,
                bar_x,
                bar_y,
                fill_w,
                bar_h,
                RR_BGRA(255, 255, 255, alpha * 235 / 255)
            );

            int scrubber_x = bar_x + fill_w - 4;

            rr_fill(
                fb,
                scrubber_x,
                bar_y - 4,
                8,
                bar_h + 8,
                RR_BGRA(255, 255, 255, alpha * 240 / 255)
            );
        }

        prospero_player_osd_draw_scrub_preview(
            fb,
            bar_y,
            alpha
        );

        prospero_player_osd_draw_scrub_handle(
            fb,
            bar_y,
            alpha
        );

        char current_time[32];
        char duration_time[32];

        format_time_mmss(
            current_time,
            sizeof(current_time),
            display_position
        );

        format_time_mmss(
            duration_time,
            sizeof(duration_time),
            media_duration_sec
        );

        rr_text(
            fb,
            bar_x,
            942,
            current_time,
            RR_BGRA(225, 225, 225, alpha * 220 / 255),
            0
        );

        rr_text(
            fb,
            bar_x + bar_w - rr_text_width(duration_time, 0),
            942,
            duration_time,
            RR_BGRA(225, 225, 225, alpha * 220 / 255),
            0
        );

        /*
         * Control hints: only the two safe actions left on the pad.
         */
        int hx = 180;
        int hy = 1000;

        /* X = play / pause */
        rr_control(fb, hx, hy, 0);
        const char *pp_label = player_paused ? "PLAY" : "PAUSE";
        rr_text(
            fb,
            hx + 58, hy + 16,
            pp_label,
            RR_BGRA(215, 215, 215, alpha * 215 / 255),
            0
        );
        hx += 58 + rr_text_width(pp_label, 0) + 44;

        /* OPTIONS = playback settings dialog */
        rr_text(
            fb,
            hx, hy + 16,
            "OPTIONS",
            RR_BGRA(235, 235, 235, alpha * 225 / 255),
            0
        );
        hx += rr_text_width("OPTIONS", 0) + 14;

        rr_text(
            fb,
            hx, hy + 16,
            "SETTINGS",
            RR_BGRA(200, 200, 200, alpha * 205 / 255),
            0
        );

    }

    /* PROSPERO_SRT_DRAW_CALL_START */

    double subtitle_position =
        resume_base_offset_seconds +
        (
            audio_ctx &&
            audio_handle >= 1
                ? audio_clock_seconds
                : video_clock_seconds
        );

    if (subtitle_position < 0.0) {
        subtitle_position = 0.0;
    }

    prospero_subtitle_draw(
        fb,
        subtitle_position,
        osd_visibility
    );

    /* PROSPERO_SRT_DRAW_CALL_END */

    /*
     * PROSPERO_STATS_V2_START
     * Detailed playback and queue-health panel.
     */
    /*
     * PROSPERO_STATS_OPTIONS_ONLY_FIX
     * The Options button alone controls the visible statistics card.
     * Developer Mode remains available for logs and diagnostics.
     */
    if (show_stats_for_nerds) {
        char video_codec[64] = "NONE";
        char audio_codec[64] = "NONE";
        char audio_language[32] = "UND";

        int video_width = current_metadata.width;
        int video_height = current_metadata.height;
        int audio_channels = 0;
        int audio_rate = detected_audio_rate;

        if (
            play_fmt &&
            video_stream_index >= 0 &&
            video_stream_index <
                (int)play_fmt->nb_streams
        ) {
            AVStream *stream =
                play_fmt->streams[video_stream_index];

            if (stream && stream->codecpar) {
                snprintf(
                    video_codec,
                    sizeof(video_codec),
                    "%s",
                    avcodec_get_name(
                        stream->codecpar->codec_id
                    )
                );

                video_width =
                    stream->codecpar->width;

                video_height =
                    stream->codecpar->height;
            }
        }

        if (
            play_fmt &&
            audio_stream_index >= 0 &&
            audio_stream_index <
                (int)play_fmt->nb_streams
        ) {
            AVStream *stream =
                play_fmt->streams[audio_stream_index];

            if (stream && stream->codecpar) {
                snprintf(
                    audio_codec,
                    sizeof(audio_codec),
                    "%s",
                    avcodec_get_name(
                        stream->codecpar->codec_id
                    )
                );

                audio_channels =
                    stream->codecpar->
                        ch_layout.nb_channels;

                audio_rate =
                    stream->codecpar->sample_rate;

                const AVDictionaryEntry *language =
                    av_dict_get(
                        stream->metadata,
                        "language",
                        NULL,
                        0
                    );

                if (
                    language &&
                    language->value &&
                    language->value[0]
                ) {
                    snprintf(
                        audio_language,
                        sizeof(audio_language),
                        "%s",
                        language->value
                    );
                }
            }
        }

        int video_queue =
            packet_queue_count(
                &video_packet_queue
            );

        int audio_packet_queue_count =
            packet_queue_count(
                &audio_packet_queue
            );

        char video_line[160];
        char audio_line[160];
        char performance_line[160];
        char queue_line[160];
        char clock_line[160];
        char subtitle_line[160];

        snprintf(
            video_line,
            sizeof(video_line),
            "VIDEO  %s  /  %d X %d  /  %.3F FPS",
            video_codec,
            video_width,
            video_height,
            video_fps
        );

        snprintf(
            audio_line,
            sizeof(audio_line),
            "AUDIO  %s  /  %s  /  %dCH  /  %d HZ",
            audio_codec,
            audio_language,
            audio_channels,
            audio_rate
        );

        snprintf(
            performance_line,
            sizeof(performance_line),
            "RENDER %d FPS  /  DECODE %d FPS",
            perf_render_fps,
            perf_decode_fps
        );

        snprintf(
            queue_line,
            sizeof(queue_line),
            "QUEUES  VIDEO %d/%d  /  AUDIO %d/%d  /  PCM %d",
            video_queue,
            video_packet_cap,
            audio_packet_queue_count,
            audio_packet_cap,
            audio_queue_count
        );

        snprintf(
            clock_line,
            sizeof(clock_line),
            "CLOCKS  VIDEO %.2F  /  AUDIO %.2F  /  DELTA %.3F",
            video_clock_seconds,
            audio_clock_seconds,
            video_clock_seconds -
                audio_clock_seconds
        );

        snprintf(
            subtitle_line,
            sizeof(subtitle_line),
            "SUBTITLES  %s  /  %s",
            prospero_subtitle_enabled
                ? "ON"
                : "OFF",
            prospero_subtitle_use_external
                ? "EXTERNAL"
                : (
                    prospero_embedded_subtitle_stream_index >= 0
                        ? "EMBEDDED"
                        : "NONE"
                )
        );

        int debug_x = 1115;
        int debug_y = 52;
        int debug_w = 735;
        int debug_h = 350;

        ui_round_asset(
            fb,
            debug_x,
            debug_y,
            debug_w,
            debug_h,
            24,
            RR_BGRA(14,14,14,232),
            RR_BGRA(192,192,192,170),
            RR_BGRA(182,182,182,50)
        );

        rr_text(
            fb,
            debug_x + 34,
            debug_y + 24,
            "PLAYBACK STATISTICS",
            RR_BGRA(205,205,205,240),
            1
        );

        rr_fill(
            fb,
            debug_x + 32,
            debug_y + 68,
            debug_w - 64,
            1,
            RR_BGRA(171,171,171,75)
        );

        const char *lines[6] = {
            video_line,
            audio_line,
            subtitle_line,
            performance_line,
            queue_line,
            clock_line
        };

        for (int row = 0; row < 6; row++) {
            rr_text(
                fb,
                debug_x + 34,
                debug_y + 91 + row * 41,
                lines[row],
                row >= 4
                    ? RR_BGRA(208,208,208,220)
                    : RR_BGRA(241,241,241,230),
                0
            );
        }
    }
    /* PROSPERO_STATS_V2_END */
}



/* RESUME_UI_FORWARD_DECLARATIONS_START */
static void rr_bg(uint32_t *fb);

static void rr_fill(
    uint32_t *fb,
    int x,
    int y,
    int w,
    int h,
    uint32_t color
);

static void rr_text(
    uint32_t *fb,
    int x,
    int y,
    const char *text,
    uint32_t color,
    int face
);

static void rr_control(
    uint32_t *fb,
    int x,
    int y,
    int index
);
/* RESUME_UI_FORWARD_DECLARATIONS_END */

void draw_resume_prompt(uint32_t *fb) {
    static char last_prompt_path[512] = {0};
    static int prompt_frame = 0;

    if (
        strcmp(last_prompt_path, pending_resume_path) != 0
    ) {
        snprintf(
            last_prompt_path,
            sizeof(last_prompt_path),
            "%s",
            pending_resume_path
        );

        prompt_frame = 0;
    }

    if (prompt_frame < 14) {
        prompt_frame++;
    }

    int remaining = 14 - prompt_frame;

    if (remaining < 0) {
        remaining = 0;
    }

    int panel_offset_y =
        (remaining * remaining) / 3;

    int panel_x = 330;
    int panel_y = 170 + panel_offset_y;
    int panel_w = 1260;
    int panel_h = 690;

    rr_bg(fb);

    /*
     * Darken the background behind the modal without replacing
     * the approved Prospero artwork.
     */
    rr_fill(
        fb,
        0,
        0,
        1920,
        1080,
        RR_BGRA(8,8,8,150)
    );

    /*
     * Main glass prompt.
     */
    ui_round_asset(
        fb,
        panel_x,
        panel_y,
        panel_w,
        panel_h,
        34,
        RR_BGRA(17,17,17,238),
        RR_BGRA(202,202,202,215),
        RR_BGRA(185,185,185,70)
    );

    rr_fill(
        fb,
        panel_x + 54,
        panel_y + 54,
        6,
        78,
        RR_BGRA(205,205,205,235)
    );

    rr_fill(
        fb,
        panel_x + 54,
        panel_y + 126,
        34,
        2,
        RR_BGRA(205,205,205,205)
    );

    rr_text(
        fb,
        panel_x + 98,
        panel_y + 48,
        "RESUME PLAYBACK",
        RR_BGRA(235, 246, 255, 245),
        3
    );

    rr_text(
        fb,
        panel_x + 100,
        panel_y + 108,
        "CONTINUE FROM YOUR LAST POSITION",
        RR_BGRA(199,199,199,225),
        1
    );

    char title[160];
    char metadata[160];

    title[0] = 0;
    metadata[0] = 0;

    clean_media_title(
        pending_resume_path,
        title,
        sizeof(title),
        metadata,
        sizeof(metadata)
    );

    if (strlen(title) > 49) {
        title[46] = '.';
        title[47] = '.';
        title[48] = '.';
        title[49] = 0;
    }

    if (strlen(metadata) > 67) {
        metadata[64] = '.';
        metadata[65] = '.';
        metadata[66] = '.';
        metadata[67] = 0;
    }

    rr_text(
        fb,
        panel_x + 100,
        panel_y + 190,
        title,
        RR_BGRA(235, 246, 255, 245),
        2
    );

    if (metadata[0]) {
        rr_text(
            fb,
            panel_x + 102,
            panel_y + 238,
            metadata,
            RR_BGRA(204,204,204,220),
            1
        );
    }

    char resume_time[48];
    char duration_time[48];
    char position_line[128];

    format_time_mmss(
        resume_time,
        sizeof(resume_time),
        pending_resume_pos
    );

    double duration =
        current_metadata.duration_sec > 1.0
            ? current_metadata.duration_sec
            : media_duration_sec;

    if (duration > 1.0) {
        format_time_mmss(
            duration_time,
            sizeof(duration_time),
            duration
        );

        snprintf(
            position_line,
            sizeof(position_line),
            "%s  /  %s",
            resume_time,
            duration_time
        );
    } else {
        snprintf(
            position_line,
            sizeof(position_line),
            "RESUME AT  %s",
            resume_time
        );
    }

    rr_text(
        fb,
        panel_x + 100,
        panel_y + 322,
        "LAST POSITION",
        RR_BGRA(196,196,196,210),
        0
    );

    rr_text(
        fb,
        panel_x + 100,
        panel_y + 360,
        position_line,
        RR_BGRA(232, 244, 255, 240),
        2
    );

    /*
     * Progress track.
     */
    int progress_x = panel_x + 100;
    int progress_y = panel_y + 430;
    int progress_w = panel_w - 200;
    int progress_h = 10;

    rr_fill(
        fb,
        progress_x,
        progress_y,
        progress_w,
        progress_h,
        RR_BGRA(86,86,86,175)
    );

    int progress_fill = 0;

    if (duration > 1.0) {
        double ratio =
            pending_resume_pos / duration;

        if (ratio < 0.0) {
            ratio = 0.0;
        }

        if (ratio > 1.0) {
            ratio = 1.0;
        }

        progress_fill =
            (int)(ratio * progress_w);
    }

    if (progress_fill > 0) {
        rr_fill(
            fb,
            progress_x,
            progress_y,
            progress_fill,
            progress_h,
            RR_BGRA(204,204,204,240)
        );

        rr_fill(
            fb,
            progress_x + progress_fill - 3,
            progress_y - 4,
            6,
            progress_h + 8,
            RR_BGRA(237,237,237,235)
        );
    }

    /*
     * Divider above controls.
     */
    rr_fill(
        fb,
        panel_x + 78,
        panel_y + 500,
        panel_w - 156,
        1,
        RR_BGRA(171,171,171,65)
    );

    /*
     * X Resume.
     */
    rr_control(
        fb,
        panel_x + 105,
        panel_y + 548,
        0
    );

    rr_text(
        fb,
        panel_x + 162,
        panel_y + 564,
        "RESUME",
        RR_BGRA(230, 242, 252, 235),
        0
    );

    /*
     * Circle Start Over.
     */
    rr_control(
        fb,
        panel_x + 430,
        panel_y + 548,
        4
    );

    rr_text(
        fb,
        panel_x + 487,
        panel_y + 564,
        "START OVER",
        RR_BGRA(230, 242, 252, 235),
        0
    );

    rr_text(
        fb,
        panel_x + panel_w - 300,
        panel_y + 564,
        "PS PLAY",
        RR_BGRA(192,192,192,190),
        0
    );
}



/* RR_FORWARD_DECLARATIONS_START */

static void rr_img(
    uint32_t *fb,
    int x,
    int y,
    int w,
    int h,
    const unsigned int *img
);

static void rr_bg(uint32_t *fb);

static void rr_fill(
    uint32_t *fb,
    int x,
    int y,
    int w,
    int h,
    uint32_t color
);

static void rr_text(
    uint32_t *fb,
    int x,
    int y,
    const char *text,
    uint32_t color,
    int face
);

static void rr_icon(
    uint32_t *fb,
    int x,
    int y,
    int index
);

static void rr_control(
    uint32_t *fb,
    int x,
    int y,
    int index
);
/* RR_FORWARD_DECLARATIONS_END */


/* USB_BROWSER_FILE_TYPE_HELPERS_START */

static const char *usb_browser_extension(const char *name) {
    const char *dot = strrchr(name, '.');

    if (!dot || !dot[1]) {
        return "";
    }

    return dot + 1;
}

static int usb_browser_extension_is(
    const char *extension,
    const char *value
) {
    return strcasecmp(extension, value) == 0;
}

static const char *usb_browser_type_label(
    const char *name,
    int entry_type
) {
    if (entry_type == 4) {
        return "FOLDER";
    }

    const char *ext = usb_browser_extension(name);

    if (
        usb_browser_extension_is(ext, "mkv") ||
        usb_browser_extension_is(ext, "mp4") ||
        usb_browser_extension_is(ext, "mov") ||
        usb_browser_extension_is(ext, "avi") ||
        usb_browser_extension_is(ext, "webm") ||
        usb_browser_extension_is(ext, "m2ts") ||
        usb_browser_extension_is(ext, "ts")
    ) {
        return "VIDEO";
    }

    if (
        usb_browser_extension_is(ext, "mp3") ||
        usb_browser_extension_is(ext, "flac") ||
        usb_browser_extension_is(ext, "wav") ||
        usb_browser_extension_is(ext, "aac") ||
        usb_browser_extension_is(ext, "m4a") ||
        usb_browser_extension_is(ext, "ogg")
    ) {
        return "AUDIO";
    }

    if (
        usb_browser_extension_is(ext, "jpg") ||
        usb_browser_extension_is(ext, "jpeg") ||
        usb_browser_extension_is(ext, "png") ||
        usb_browser_extension_is(ext, "bmp") ||
        usb_browser_extension_is(ext, "webp")
    ) {
        return "IMAGE";
    }

    if (
        usb_browser_extension_is(ext, "srt") ||
        usb_browser_extension_is(ext, "ass") ||
        usb_browser_extension_is(ext, "ssa") ||
        usb_browser_extension_is(ext, "vtt") ||
        usb_browser_extension_is(ext, "txt")
    ) {
        return "DOCUMENT";
    }

    if (
        usb_browser_extension_is(ext, "iso") ||
        usb_browser_extension_is(ext, "img")
    ) {
        return "DISC IMAGE";
    }

    if (
        usb_browser_extension_is(ext, "elf") ||
        usb_browser_extension_is(ext, "bin") ||
        usb_browser_extension_is(ext, "sprx")
    ) {
        return "HOMEBREW";
    }

    return "FILE";
}

/*
 * Existing icon assets are reused safely:
 *
 * 0 = USB/folder
 * 1 = clock/recent
 * 2 = star/favorites
 * 3 = settings
 * 4 = developer terminal
 * 5 = information/document
 */
static int usb_browser_icon_for_entry(
    const char *name,
    int entry_type
) {
    if (entry_type == 4) {
        return 0;
    }

    const char *extension =
        usb_browser_extension(name);

    if (
        usb_browser_extension_is(extension, "mkv") ||
        usb_browser_extension_is(extension, "mp4") ||
        usb_browser_extension_is(extension, "mov") ||
        usb_browser_extension_is(extension, "avi") ||
        usb_browser_extension_is(extension, "webm") ||
        usb_browser_extension_is(extension, "m2ts") ||
        usb_browser_extension_is(extension, "ts")
    ) {
        return 1;
    }

    if (
        usb_browser_extension_is(extension, "mp3") ||
        usb_browser_extension_is(extension, "flac") ||
        usb_browser_extension_is(extension, "wav") ||
        usb_browser_extension_is(extension, "aac") ||
        usb_browser_extension_is(extension, "m4a") ||
        usb_browser_extension_is(extension, "ogg")
    ) {
        return 2;
    }

    if (
        usb_browser_extension_is(extension, "jpg") ||
        usb_browser_extension_is(extension, "jpeg") ||
        usb_browser_extension_is(extension, "png") ||
        usb_browser_extension_is(extension, "bmp") ||
        usb_browser_extension_is(extension, "webp")
    ) {
        return 3;
    }

    if (
        usb_browser_extension_is(extension, "txt") ||
        usb_browser_extension_is(extension, "srt") ||
        usb_browser_extension_is(extension, "ass") ||
        usb_browser_extension_is(extension, "ssa") ||
        usb_browser_extension_is(extension, "vtt")
    ) {
        return 4;
    }

    if (
        usb_browser_extension_is(extension, "iso") ||
        usb_browser_extension_is(extension, "img")
    ) {
        return 5;
    }

    if (
        usb_browser_extension_is(extension, "elf") ||
        usb_browser_extension_is(extension, "bin") ||
        usb_browser_extension_is(extension, "sprx") ||
        usb_browser_extension_is(extension, "pkg")
    ) {
        return 6;
    }

    if (
        usb_browser_extension_is(extension, "zip") ||
        usb_browser_extension_is(extension, "rar") ||
        usb_browser_extension_is(extension, "7z") ||
        usb_browser_extension_is(extension, "tar") ||
        usb_browser_extension_is(extension, "gz")
    ) {
        return 7;
    }

    return 8;
}

/* USB_BROWSER_FILE_TYPE_HELPERS_END */


#ifndef RR_BGRA
#endif


/* BROWSER_ICON_RENDERER_START */
static void rr_browser_icon(
    uint32_t *fb,
    int x,
    int y,
    int type
) {
    switch (type) {
        case 0:
            rr_img(
                fb, x, y,
                BR_ICON_FOLDER_W,
                BR_ICON_FOLDER_H,
                BR_ICON_FOLDER
            );
            break;

        case 1:
            rr_img(
                fb, x, y,
                BR_ICON_VIDEO_W,
                BR_ICON_VIDEO_H,
                BR_ICON_VIDEO
            );
            break;

        case 2:
            rr_img(
                fb, x, y,
                BR_ICON_AUDIO_W,
                BR_ICON_AUDIO_H,
                BR_ICON_AUDIO
            );
            break;

        case 3:
            rr_img(
                fb, x, y,
                BR_ICON_IMAGE_W,
                BR_ICON_IMAGE_H,
                BR_ICON_IMAGE
            );
            break;

        case 4:
            rr_img(
                fb, x, y,
                BR_ICON_DOCUMENT_W,
                BR_ICON_DOCUMENT_H,
                BR_ICON_DOCUMENT
            );
            break;

        case 5:
            rr_img(
                fb, x, y,
                BR_ICON_DISC_W,
                BR_ICON_DISC_H,
                BR_ICON_DISC
            );
            break;

        case 6:
            rr_img(
                fb, x, y,
                BR_ICON_HOMEBREW_W,
                BR_ICON_HOMEBREW_H,
                BR_ICON_HOMEBREW
            );
            break;

        case 7:
            rr_img(
                fb, x, y,
                BR_ICON_ARCHIVE_W,
                BR_ICON_ARCHIVE_H,
                BR_ICON_ARCHIVE
            );
            break;

        default:
            rr_img(
                fb, x, y,
                BR_ICON_FILE_W,
                BR_ICON_FILE_H,
                BR_ICON_FILE
            );
            break;
    }
}
/* BROWSER_ICON_RENDERER_END */


/* USB_BROWSER_MARQUEE_START */
static void usb_browser_make_marquee(
    char *out,
    size_t out_size,
    const char *text,
    int frame,
    int visible_chars
) {
    if (!out || out_size == 0) {
        return;
    }

    out[0] = 0;

    if (!text) {
        return;
    }

    size_t length = strlen(text);

    if ((int)length <= visible_chars) {
        snprintf(out, out_size, "%s", text);
        return;
    }

    /*
     * Pause, scroll, pause.
     * Three frames per character keeps it smooth and readable.
     */
    const int pause_frames = 34;
    const int gap_chars = 7;
    const int speed_divisor = 3;

    int scrolling_frames =
        ((int)length + gap_chars) * speed_divisor;

    int cycle_frames =
        pause_frames +
        scrolling_frames +
        pause_frames;

    int local_frame =
        frame % cycle_frames;

    int offset = 0;

    if (
        local_frame >= pause_frames &&
        local_frame < pause_frames + scrolling_frames
    ) {
        offset =
            (local_frame - pause_frames) /
            speed_divisor;
    }

    for (
        int column = 0;
        column < visible_chars &&
        column + 1 < (int)out_size;
        column++
    ) {
        int virtual_index =
            offset + column;

        int repeated_length =
            (int)length + gap_chars;

        virtual_index %= repeated_length;

        if (virtual_index < (int)length) {
            out[column] = text[virtual_index];
        } else {
            out[column] = ' ';
        }

        out[column + 1] = 0;
    }
}
/* USB_BROWSER_MARQUEE_END */

void draw_usb_browser(uint32_t *fb) {
    static int browser_initialized = 0;
    static int selected_y_fp = 0;
    static int intro_frame = 0;
    static int browser_frame = 0;
    static char transition_path[256] = {0};
    static int folder_transition_frame = 12;
    static int folder_transition_direction = 1;
    static int marquee_frame = 0;
    static int marquee_selected = -1;

    const int visible = 5;
    const int row_x = 180;
    const int row_start_y = 252;
    const int row_gap = 126;

    if (file_count <= 0) {
        file_count = 0;
        file_selected = 0;
        file_scroll = 0;
    } else {
        if (file_selected < 0) {
            file_selected = 0;
        }

        if (file_selected >= file_count) {
            file_selected = file_count - 1;
        }

        if (file_selected < file_scroll) {
            file_scroll = file_selected;
        }

        if (file_selected >= file_scroll + visible) {
            file_scroll = file_selected - visible + 1;
        }

        if (file_scroll < 0) {
            file_scroll = 0;
        }
    }

    int visible_selected = file_selected - file_scroll;

    if (visible_selected < 0) {
        visible_selected = 0;
    }

    if (visible_selected >= visible) {
        visible_selected = visible - 1;
    }

    int target_y_fp =
        (row_start_y + visible_selected * row_gap) << 8;

    if (!browser_initialized) {
        selected_y_fp = target_y_fp;
        browser_initialized = 1;
    }

    int distance = target_y_fp - selected_y_fp;

    if (distance > -160 && distance < 160) {
        selected_y_fp = target_y_fp;
    } else {
        selected_y_fp += distance / 2;
    }

    int selected_draw_y = selected_y_fp >> 8;

    
    if (strcmp(transition_path, current_path) != 0) {
        int old_depth = 0;
        int new_depth = 0;

        for (int i = 0; transition_path[i]; i++) {
            if (transition_path[i] == '/') {
                old_depth++;
            }
        }

        for (int i = 0; current_path[i]; i++) {
            if (current_path[i] == '/') {
                new_depth++;
            }
        }

        if (transition_path[0] == 0) {
            folder_transition_direction = 1;
        } else if (new_depth > old_depth) {
            /* Opening a child folder: contents enter from the right. */
            folder_transition_direction = 1;
        } else if (new_depth < old_depth) {
            /* Going back: contents enter from the left. */
            folder_transition_direction = -1;
        } else {
            folder_transition_direction =
                strlen(current_path) >= strlen(transition_path)
                    ? 1
                    : -1;
        }

        snprintf(
            transition_path,
            sizeof(transition_path),
            "%s",
            current_path
        );

        folder_transition_frame = 0;
        intro_frame = 22;
        selected_y_fp =
            (row_start_y + visible_selected * row_gap) << 8;
    }


    browser_frame++;

    /*
     * Detect folder changes. The first rendered frame of a new path
     * starts with the rows offset to the right.
     */
    if (strcmp(transition_path, current_path) != 0) {
        snprintf(
            transition_path,
            sizeof(transition_path),
            "%s",
            current_path
        );

        folder_transition_frame = 0;
        intro_frame = 22;
    }

    if (folder_transition_frame < 12) {
        folder_transition_frame++;
    }

    int folder_enter_offset = 0;

    if (folder_transition_frame < 12) {
        int remaining = 12 - folder_transition_frame;

        /*
         * Starts about 180 pixels to the right and settles quickly.
         * The squared curve makes the final frames ease gently.
         */
        folder_enter_offset =
            folder_transition_direction *
            ((remaining * remaining * 5) / 4);
    }

    if (marquee_selected != file_selected) {
        marquee_selected = file_selected;
        marquee_frame = 0;
    } else {
        marquee_frame++;
    }

    rr_bg(fb);


    /* Header. */
    rr_fill(
        fb,
        82,
        74,
        5,
        76,
        RR_BGRA(205,205,205,225)
    );

    rr_fill(
        fb,
        82,
        146,
        31,
        2,
        RR_BGRA(205,205,205,195)
    );

    rr_text(
        fb,
        122,
        70,
        "USB MEDIA",
        RR_BGRA(232, 244, 255, 240),
        3
    );

    char breadcrumb[256];
    const char *shown_path = current_path;

    if (strncmp(current_path, "/mnt/", 5) == 0) {
        shown_path = current_path + 5;
    }

    snprintf(
        breadcrumb,
        sizeof(breadcrumb),
        "%s",
        shown_path
    );

    for (int i = 0; breadcrumb[i]; i++) {
        if (breadcrumb[i] == '/') {
            breadcrumb[i] = '>';
        }
    }

    rr_text(
        fb,
        124,
        132,
        breadcrumb,
        RR_BGRA(199,199,199,225),
        1
    );

    char count_text[64];

    if (file_count > 0) {
        snprintf(
            count_text,
            sizeof(count_text),
            "%d OF %d",
            file_selected + 1,
            file_count
        );
    } else {
        snprintf(
            count_text,
            sizeof(count_text),
            "EMPTY"
        );
    }

    rr_text(
        fb,
        1540,
        132,
        count_text,
        RR_BGRA(203,203,203,210),
        0
    );

    if (file_count == 0) {
        rr_text(
            fb,
            230,
            330,
            "THIS FOLDER IS EMPTY",
            RR_BGRA(232, 244, 255, 230),
            2
        );

        rr_text(
            fb,
            232,
            382,
            "PRESS O TO GO BACK",
            RR_BGRA(204,204,204,215),
            1
        );
    } else {
        /* Base rows. */
        for (int row = 0; row < visible; row++) {
            int index = file_scroll + row;

            if (index >= file_count) {
                break;
            }

            int local_frame = intro_frame - row;
            int slide_offset = 0;

            if (local_frame <= 0) {
                slide_offset = 18;
            } else if (local_frame < 6) {
                slide_offset = 18 - local_frame * 3;
            }

            int y = row_start_y + row * row_gap;

            rr_img(
                fb,
                row_x - slide_offset + folder_enter_offset,
                y,
                RR_CARD_NORMAL_W,
                RR_CARD_NORMAL_H,
                RR_CARD_NORMAL
            );
        }

        /* Gliding selected row. */
        rr_img(
            fb,
            row_x + folder_enter_offset,
            selected_draw_y,
            RR_CARD_SELECTED_W,
            RR_CARD_SELECTED_H,
            RR_CARD_SELECTED
        );

        /* Icons, titles, metadata and chevrons. */
        for (int row = 0; row < visible; row++) {
            int index = file_scroll + row;

            if (index >= file_count) {
                break;
            }

            int local_frame = intro_frame - row;
            int slide_offset = 0;

            if (local_frame <= 0) {
                slide_offset = 18;
            } else if (local_frame < 6) {
                slide_offset = 18 - local_frame * 3;
            }

            int x = row_x - slide_offset + folder_enter_offset;
            int y = row_start_y + row * row_gap;

            char title[160];
            char detail[192];
            char display_title[160];

            title[0] = 0;
            detail[0] = 0;

            if (usb_types[index] == 4) {
                snprintf(
                    title,
                    sizeof(title),
                    "%s",
                    usb_files[index]
                );

                snprintf(
                    detail,
                    sizeof(detail),
                    "OPEN FOLDER"
                );
            } else {
                char full_path[768];
                char metadata[128];

                metadata[0] = 0;

                snprintf(
                    full_path,
                    sizeof(full_path),
                    "%s/%s",
                    current_path,
                    usb_files[index]
                );

                clean_media_title(
                    full_path,
                    title,
                    sizeof(title),
                    metadata,
                    sizeof(metadata)
                );

                struct stat st;
                char size_text[48];

                size_text[0] = 0;

                if (stat(full_path, &st) == 0) {
                    format_file_size(
                        size_text,
                        sizeof(size_text),
                        (long long)st.st_size
                    );
                }

                const char *type_label =
                    usb_browser_type_label(
                        usb_files[index],
                        usb_types[index]
                    );

                if (metadata[0] && size_text[0]) {
                    snprintf(
                        detail,
                        sizeof(detail),
                        "%s  -  %s  -  %s",
                        type_label,
                        metadata,
                        size_text
                    );
                } else if (size_text[0]) {
                    snprintf(
                        detail,
                        sizeof(detail),
                        "%s  -  %s",
                        type_label,
                        size_text
                    );
                } else {
                    snprintf(
                        detail,
                        sizeof(detail),
                        "%s",
                        type_label
                    );
                }
            }

            if (index == file_selected) {
                usb_browser_make_marquee(
                    display_title,
                    sizeof(display_title),
                    title,
                    marquee_frame,
                    50
                );
            } else {
                snprintf(
                    display_title,
                    sizeof(display_title),
                    "%s",
                    title
                );

                if (strlen(display_title) > 50) {
                    display_title[47] = '.';
                    display_title[48] = '.';
                    display_title[49] = '.';
                    display_title[50] = 0;
                }
            }

            if (strlen(detail) > 68) {
                detail[65] = '.';
                detail[66] = '.';
                detail[67] = '.';
                detail[68] = 0;
            }

            int icon_index =
                usb_browser_icon_for_entry(
                    usb_files[index],
                    usb_types[index]
                );

            /* Stock icons only — large preview is on the right for selection */
            rr_browser_icon(
                fb,
                x + 48,
                y + 20,
                icon_index
            );

            rr_text(
                fb,
                x + 150,
                y + 25,
                display_title,
                RR_BGRA(232, 244, 255, 238),
                2
            );

            rr_text(
                fb,
                x + 152,
                y + 70,
                detail,
                RR_BGRA(204,204,204,220),
                1
            );

            int arrow_shift = 0;

            if (index == file_selected) {
                int phase = browser_frame % 36;

                if (phase < 9) {
                    arrow_shift = phase / 3;
                } else if (phase < 18) {
                    arrow_shift =
                        3 - ((phase - 9) / 3);
                }
            }

            /* Chevron stays left of the preview panel */
            rr_icon(
                fb,
                x + 980 + arrow_shift,
                y + 21,
                6
            );
        }
    }

    /*
     * Large thumbnail preview for the highlighted row (right blank area).
     * Like Infuse / BFplayer — list icons stay stock; art lives on the side.
     */
    if (file_count > 0 &&
        file_selected >= 0 &&
        file_selected < file_count &&
        strcmp(usb_files[file_selected], "..") != 0) {
        char sel_path[768];
        int is_dir = (usb_types[file_selected] == 4);
        const int preview_x = 1440;
        const int preview_y = 280;

        if (current_path[0] &&
            current_path[strlen(current_path) - 1] == '/')
            snprintf(
                sel_path,
                sizeof(sel_path),
                "%s%s",
                current_path,
                usb_files[file_selected]);
        else
            snprintf(
                sel_path,
                sizeof(sel_path),
                "%s/%s",
                current_path,
                usb_files[file_selected]);

        prospero_browser_preview_ensure(sel_path, is_dir);
        prospero_browser_preview_draw_panel(fb, preview_x, preview_y);
        rr_text(
            fb,
            preview_x + 14,
            preview_y + 14 + PROSPERO_BROWSER_PREVIEW_H + 14,
            prospero_browser_preview_valid ? "PREVIEW" : "NO PREVIEW",
            RR_BGRA(199,199,199,220),
            0
        );
    }

    /* Footer. */
    rr_fill(
        fb,
        0,
        948,
        1920,
        1,
        RR_BGRA(186,186,186,48)
    );

    rr_fill(
        fb,
        0,
        949,
        1920,
        79,
        RR_BGRA(12,12,12,138)
    );

    rr_control(
        fb,
        92,
        982,
        0
    );

    rr_text(
        fb,
        148,
        998,
        file_count > 0 ? "OPEN" : "SELECT",
        RR_BGRA(209,209,209,205),
        0
    );

    rr_control(
        fb,
        292,
        982,
        4
    );

    rr_text(
        fb,
        348,
        998,
        "BACK",
        RR_BGRA(209,209,209,205),
        0
    );

    rr_icon(
        fb,
        1512,
        965,
        0
    );

    rr_text(
        fb,
        1585,
        997,
        "USB MEDIA",
        RR_BGRA(203,203,203,205),
        0
    );


    /* PROSPERO_USB_FAVORITE_BUTTON_START */

    /*
     * Triangle icon and Favorite label.
     * Added independently so existing Open/Back controls remain untouched.
     */
    rr_control(
        fb,
        500,
        982,
        5
    );

    rr_text(
        fb,
        556,
        998,
        "FAVORITE",
        RR_BGRA(209,209,209,205),
        0
    );

    /* PROSPERO_USB_FAVORITE_BUTTON_END */

}






void draw_developer_tools_screen(uint32_t *fb) {
    static int developer_frame = 0;
    static int intro_frame = 0;

    developer_frame++;

    if (intro_frame < 14) {
        intro_frame++;
    }

    int remaining = 14 - intro_frame;

    if (remaining < 0) {
        remaining = 0;
    }

    int panel_offset =
        (remaining * remaining) / 3;

    rr_bg(fb);


    /*
     * Header.
     */
    rr_fill(
        fb,
        82,
        74,
        5,
        76,
        RR_BGRA(205,205,205,225)
    );

    rr_fill(
        fb,
        82,
        146,
        31,
        2,
        RR_BGRA(205,205,205,195)
    );

    rr_text(
        fb,
        122,
        70,
        "DEVELOPER TOOLS",
        RR_BGRA(232, 244, 255, 240),
        3
    );

    rr_text(
        fb,
        124,
        132,
        "PLAYBACK DIAGNOSTICS AND COMPATIBILITY REPORTING",
        RR_BGRA(199,199,199,225),
        1
    );

    /*
     * Main compatibility-report card.
     */
    int action_x = 180 - panel_offset;
    int action_y = 228;
    int action_w = 1560;
    int action_h = 156;

    ui_round_asset(
        fb,
        action_x,
        action_y,
        action_w,
        action_h,
        28,
        RR_BGRA(17,17,17,225),
        RR_BGRA(202,202,202,215),
        RR_BGRA(185,185,185,60)
    );

    rr_icon(
        fb,
        action_x + 54,
        action_y + 42,
        4
    );

    rr_text(
        fb,
        action_x + 150,
        action_y + 34,
        "GENERATE COMPATIBILITY REPORT",
        RR_BGRA(235, 246, 255, 245),
        2
    );

    rr_text(
        fb,
        action_x + 152,
        action_y + 84,
        "EXPORT CURRENT MEDIA DECODER AND PLAYBACK INFORMATION TO USB0",
        RR_BGRA(204,204,204,220),
        1
    );

    int arrow_shift = 0;
    int arrow_phase = developer_frame % 36;

    if (arrow_phase < 9) {
        arrow_shift = arrow_phase / 3;
    } else if (arrow_phase < 18) {
        arrow_shift =
            3 - ((arrow_phase - 9) / 3);
    }

    rr_icon(
        fb,
        action_x + action_w - 112 + arrow_shift,
        action_y + 46,
        6
    );

    /*
     * Runtime-status cards.
     */
    int card_y = 430;
    int card_w = 480;
    int card_h = 300;

    int card1_x = 180 - panel_offset;
    int card2_x = 720;
    int card3_x = 1260 + panel_offset;

    ui_round_asset(
        fb,
        card1_x,
        card_y,
        card_w,
        card_h,
        26,
        RR_BGRA(17,17,17,215),
        RR_BGRA(182,182,182,135),
        RR_BGRA(173,173,173,42)
    );

    ui_round_asset(
        fb,
        card2_x,
        card_y,
        card_w,
        card_h,
        26,
        RR_BGRA(17,17,17,215),
        RR_BGRA(182,182,182,135),
        RR_BGRA(173,173,173,42)
    );

    ui_round_asset(
        fb,
        card3_x,
        card_y,
        card_w,
        card_h,
        26,
        RR_BGRA(17,17,17,215),
        RR_BGRA(182,182,182,135),
        RR_BGRA(173,173,173,42)
    );

    /*
     * Card 1: playback profile.
     */
    rr_icon(
        fb,
        card1_x + 42,
        card_y + 38,
        3
    );

    rr_text(
        fb,
        card1_x + 126,
        card_y + 42,
        "PROFILE",
        RR_BGRA(235, 246, 255, 240),
        2
    );

    rr_fill(
        fb,
        card1_x + 42,
        card_y + 112,
        card_w - 84,
        1,
        RR_BGRA(171,171,171,60)
    );

    rr_text(
        fb,
        card1_x + 44,
        card_y + 154,
        "ACTIVE MODE",
        RR_BGRA(191,191,191,205),
        0
    );

    rr_text(
        fb,
        card1_x + 44,
        card_y + 198,
        profile_name(current_profile),
        RR_BGRA(205,205,205,235),
        1
    );

    /*
     * Card 2: decoder information.
     */
    rr_icon(
        fb,
        card2_x + 42,
        card_y + 38,
        4
    );

    rr_text(
        fb,
        card2_x + 126,
        card_y + 42,
        "DECODER",
        RR_BGRA(235, 246, 255, 240),
        2
    );

    rr_fill(
        fb,
        card2_x + 42,
        card_y + 112,
        card_w - 84,
        1,
        RR_BGRA(171,171,171,60)
    );

    char decoder_threads[96];

    snprintf(
        decoder_threads,
        sizeof(decoder_threads),
        "%d THREADS",
        decoder_thread_count
    );

    rr_text(
        fb,
        card2_x + 44,
        card_y + 154,
        "VIDEO DECODER",
        RR_BGRA(191,191,191,205),
        0
    );

    rr_text(
        fb,
        card2_x + 44,
        card_y + 198,
        decoder_threads,
        RR_BGRA(205,205,205,235),
        1
    );

    /*
     * Card 3: diagnostic status.
     */
    rr_icon(
        fb,
        card3_x + 42,
        card_y + 38,
        5
    );

    rr_text(
        fb,
        card3_x + 126,
        card_y + 42,
        "DIAGNOSTICS",
        RR_BGRA(235, 246, 255, 240),
        2
    );

    rr_fill(
        fb,
        card3_x + 42,
        card_y + 112,
        card_w - 84,
        1,
        RR_BGRA(171,171,171,60)
    );

    rr_text(
        fb,
        card3_x + 44,
        card_y + 154,
        "DEBUG OVERLAY",
        RR_BGRA(191,191,191,205),
        0
    );

    rr_text(
        fb,
        card3_x + 44,
        card_y + 198,
        show_debug_overlay ||
        current_profile == PROFILE_DEBUG
            ? "ENABLED"
            : "DISABLED",
        show_debug_overlay ||
        current_profile == PROFILE_DEBUG
            ? RR_BGRA(205,205,205,235)
            : RR_BGRA(190,190,190,210),
        1
    );

    /*
     * FFmpeg/runtime information strip.
     */
    char ffmpeg_text[128];

    snprintf(
        ffmpeg_text,
        sizeof(ffmpeg_text),
        "FFMPEG API  %u",
        avformat_version()
    );

    rr_text(
        fb,
        184,
        786,
        ffmpeg_text,
        RR_BGRA(196,196,196,205),
        0
    );

    char render_text[128];

    snprintf(
        render_text,
        sizeof(render_text),
        "RENDER  %d FPS   DECODER  %d FPS",
        perf_render_fps,
        perf_decode_fps
    );

    rr_text(
        fb,
        620,
        786,
        render_text,
        RR_BGRA(196,196,196,205),
        0
    );

    rr_text(
        fb,
        1260,
        786,
        "REPORT LOCATION  USB0",
        RR_BGRA(196,196,196,205),
        0
    );

    /*
     * Footer.
     */
    rr_fill(
        fb,
        0,
        948,
        1920,
        1,
        RR_BGRA(186,186,186,48)
    );

    rr_fill(
        fb,
        0,
        949,
        1920,
        79,
        RR_BGRA(12,12,12,138)
    );

    rr_control(
        fb,
        92,
        982,
        0
    );

    rr_text(
        fb,
        148,
        998,
        "GENERATE REPORT",
        RR_BGRA(209,209,209,205),
        0
    );

    rr_control(
        fb,
        390,
        982,
        4
    );

    rr_text(
        fb,
        446,
        998,
        "BACK",
        RR_BGRA(209,209,209,205),
        0
    );

    rr_icon(
        fb,
        1512,
        965,
        4
    );

    rr_text(
        fb,
        1585,
        997,
        "DEVELOPER MODE",
        RR_BGRA(203,203,203,205),
        0
    );
}





/* PROSPERO_DYNAMIC_MEDIA_LABELS_START */

static const char *prospero_file_extension(
    const char *path
) {
    if (!path) {
        return "";
    }

    const char *slash =
        strrchr(path, '/');

    const char *name =
        slash ? slash + 1 : path;

    const char *dot =
        strrchr(name, '.');

    if (!dot || !dot[1]) {
        return "";
    }

    return dot + 1;
}


static int prospero_contains_ci(
    const char *text,
    const char *needle
) {
    if (!text || !needle || !needle[0]) {
        return 0;
    }

    size_t needle_length =
        strlen(needle);

    for (
        const char *cursor = text;
        *cursor;
        cursor++
    ) {
        if (
            strncasecmp(
                cursor,
                needle,
                needle_length
            ) == 0
        ) {
            return 1;
        }
    }

    return 0;
}


static void prospero_display_container(
    char *output,
    size_t output_size,
    const char *raw_container,
    const char *path
) {
    if (!output || output_size == 0) {
        return;
    }

    output[0] = 0;

    const char *extension =
        prospero_file_extension(path);

    /*
     * Prefer the actual file extension because FFmpeg frequently
     * reports one demuxer name containing several format aliases.
     */
    if (
        strcasecmp(extension, "mkv") == 0
    ) {
        snprintf(
            output,
            output_size,
            "MKV"
        );
        return;
    }

    if (
        strcasecmp(extension, "webm") == 0
    ) {
        snprintf(
            output,
            output_size,
            "WEBM"
        );
        return;
    }

    if (
        strcasecmp(extension, "mp4") == 0 ||
        strcasecmp(extension, "m4v") == 0 ||
        strcasecmp(extension, "m4a") == 0
    ) {
        snprintf(
            output,
            output_size,
            "MP4"
        );
        return;
    }

    if (
        strcasecmp(extension, "mov") == 0
    ) {
        snprintf(
            output,
            output_size,
            "MOV"
        );
        return;
    }

    if (
        strcasecmp(extension, "avi") == 0
    ) {
        snprintf(
            output,
            output_size,
            "AVI"
        );
        return;
    }

    if (
        strcasecmp(extension, "m2ts") == 0
    ) {
        snprintf(
            output,
            output_size,
            "M2TS"
        );
        return;
    }

    if (
        strcasecmp(extension, "ts") == 0
    ) {
        snprintf(
            output,
            output_size,
            "MPEG-TS"
        );
        return;
    }

    if (
        strcasecmp(extension, "mpg") == 0 ||
        strcasecmp(extension, "mpeg") == 0
    ) {
        snprintf(
            output,
            output_size,
            "MPEG"
        );
        return;
    }

    if (
        strcasecmp(extension, "wmv") == 0
    ) {
        snprintf(
            output,
            output_size,
            "WMV"
        );
        return;
    }

    if (
        strcasecmp(extension, "flv") == 0
    ) {
        snprintf(
            output,
            output_size,
            "FLV"
        );
        return;
    }

    if (
        strcasecmp(extension, "ogg") == 0 ||
        strcasecmp(extension, "ogv") == 0
    ) {
        snprintf(
            output,
            output_size,
            "OGG"
        );
        return;
    }

    /*
     * Fall back to FFmpeg's raw demuxer string when no useful file
     * extension is available.
     */
    if (
        prospero_contains_ci(
            raw_container,
            "matroska"
        )
    ) {
        snprintf(
            output,
            output_size,
            "MKV"
        );
    } else if (
        prospero_contains_ci(
            raw_container,
            "mov"
        ) ||
        prospero_contains_ci(
            raw_container,
            "mp4"
        )
    ) {
        snprintf(
            output,
            output_size,
            "MP4"
        );
    } else if (
        prospero_contains_ci(
            raw_container,
            "mpegts"
        )
    ) {
        snprintf(
            output,
            output_size,
            "MPEG-TS"
        );
    } else if (
        prospero_contains_ci(
            raw_container,
            "avi"
        )
    ) {
        snprintf(
            output,
            output_size,
            "AVI"
        );
    } else if (
        prospero_contains_ci(
            raw_container,
            "webm"
        )
    ) {
        snprintf(
            output,
            output_size,
            "WEBM"
        );
    } else if (
        raw_container &&
        raw_container[0]
    ) {
        /*
         * Keep only the first FFmpeg alias rather than the complete
         * comma- or space-separated demuxer list.
         */
        size_t used = 0;

        while (
            raw_container[used] &&
            raw_container[used] != ',' &&
            raw_container[used] != ' ' &&
            used + 1 < output_size
        ) {
            char character =
                raw_container[used];

            if (
                character >= 'a' &&
                character <= 'z'
            ) {
                character =
                    (char)(
                        character - 'a' + 'A'
                    );
            }

            output[used] =
                character;

            used++;
        }

        output[used] = 0;
    } else {
        snprintf(
            output,
            output_size,
            "UNKNOWN"
        );
    }
}


static void prospero_display_codec(
    char *output,
    size_t output_size,
    const char *raw_codec,
    int audio_codec
) {
    if (!output || output_size == 0) {
        return;
    }

    output[0] = 0;

    if (!raw_codec || !raw_codec[0]) {
        snprintf(
            output,
            output_size,
            "UNKNOWN"
        );
        return;
    }

    if (
        strcasecmp(raw_codec, "h264") == 0 ||
        strcasecmp(raw_codec, "avc") == 0 ||
        strcasecmp(raw_codec, "avc1") == 0
    ) {
        snprintf(
            output,
            output_size,
            "H.264"
        );
    } else if (
        strcasecmp(raw_codec, "hevc") == 0 ||
        strcasecmp(raw_codec, "h265") == 0 ||
        strcasecmp(raw_codec, "hev1") == 0 ||
        strcasecmp(raw_codec, "hvc1") == 0
    ) {
        snprintf(
            output,
            output_size,
            "HEVC"
        );
    } else if (
        strcasecmp(raw_codec, "av1") == 0
    ) {
        snprintf(
            output,
            output_size,
            "AV1"
        );
    } else if (
        strcasecmp(raw_codec, "vp9") == 0
    ) {
        snprintf(
            output,
            output_size,
            "VP9"
        );
    } else if (
        strcasecmp(raw_codec, "vp8") == 0
    ) {
        snprintf(
            output,
            output_size,
            "VP8"
        );
    } else if (
        strcasecmp(raw_codec, "mpeg4") == 0
    ) {
        snprintf(
            output,
            output_size,
            "MPEG-4"
        );
    } else if (
        strcasecmp(raw_codec, "mpeg2video") == 0
    ) {
        snprintf(
            output,
            output_size,
            "MPEG-2"
        );
    } else if (
        strcasecmp(raw_codec, "vc1") == 0
    ) {
        snprintf(
            output,
            output_size,
            "VC-1"
        );
    } else if (
        strcasecmp(raw_codec, "aac") == 0
    ) {
        snprintf(
            output,
            output_size,
            "AAC"
        );
    } else if (
        strcasecmp(raw_codec, "ac3") == 0
    ) {
        snprintf(
            output,
            output_size,
            "AC-3"
        );
    } else if (
        strcasecmp(raw_codec, "eac3") == 0
    ) {
        snprintf(
            output,
            output_size,
            "E-AC-3"
        );
    } else if (
        strcasecmp(raw_codec, "truehd") == 0
    ) {
        snprintf(
            output,
            output_size,
            "Dolby TrueHD"
        );
    } else if (
        strcasecmp(raw_codec, "dts") == 0
    ) {
        snprintf(
            output,
            output_size,
            "DTS"
        );
    } else if (
        strcasecmp(raw_codec, "flac") == 0
    ) {
        snprintf(
            output,
            output_size,
            "FLAC"
        );
    } else if (
        strcasecmp(raw_codec, "opus") == 0
    ) {
        snprintf(
            output,
            output_size,
            "Opus"
        );
    } else if (
        strcasecmp(raw_codec, "vorbis") == 0
    ) {
        snprintf(
            output,
            output_size,
            "Vorbis"
        );
    } else if (
        strcasecmp(raw_codec, "mp3") == 0 ||
        strcasecmp(raw_codec, "mp3float") == 0
    ) {
        snprintf(
            output,
            output_size,
            "MP3"
        );
    } else if (
        strcasecmp(raw_codec, "pcm_s16le") == 0 ||
        strcasecmp(raw_codec, "pcm_s24le") == 0 ||
        strcasecmp(raw_codec, "pcm_s32le") == 0
    ) {
        snprintf(
            output,
            output_size,
            "PCM"
        );
    } else {
        /*
         * Unknown codecs remain dynamic. Clean capitalization without
         * hiding a format that Prospero has not explicitly mapped yet.
         */
        size_t used = 0;

        while (
            raw_codec[used] &&
            used + 1 < output_size
        ) {
            char character =
                raw_codec[used];

            if (character == '_') {
                character = ' ';
            }

            if (
                used == 0 &&
                character >= 'a' &&
                character <= 'z'
            ) {
                character =
                    (char)(
                        character - 'a' + 'A'
                    );
            }

            output[used] =
                character;

            used++;
        }

        output[used] = 0;
    }

    (void)audio_codec;
}


/* PROSPERO_DYNAMIC_MEDIA_LABELS_END */


/* PROSPERO_MEDIA_INFO_V2_HELPERS_START */

static int prospero_media_info_count_streams(
    AVFormatContext *format,
    enum AVMediaType media_type
) {
    if (!format) {
        return 0;
    }

    int count = 0;

    for (
        unsigned int index = 0;
        index < format->nb_streams;
        index++
    ) {
        AVStream *stream =
            format->streams[index];

        if (
            stream &&
            stream->codecpar &&
            stream->codecpar->codec_type ==
                media_type
        ) {
            count++;
        }
    }

    return count;
}


static const char *prospero_media_info_language(
    AVStream *stream
) {
    if (!stream) {
        return "und";
    }

    const AVDictionaryEntry *language =
        av_dict_get(
            stream->metadata,
            "language",
            NULL,
            0
        );

    if (
        language &&
        language->value &&
        language->value[0]
    ) {
        return language->value;
    }

    return "und";
}


static void prospero_media_info_bitrate(
    char *output,
    size_t output_size,
    int64_t bitrate
) {
    if (!output || output_size == 0) {
        return;
    }

    if (bitrate <= 0) {
        snprintf(
            output,
            output_size,
            "UNKNOWN"
        );

        return;
    }

    if (bitrate >= 1000000) {
        snprintf(
            output,
            output_size,
            "%.2f MBPS",
            (double)bitrate / 1000000.0
        );
    } else {
        snprintf(
            output,
            output_size,
            "%lld KBPS",
            (long long)(bitrate / 1000)
        );
    }
}


static void prospero_media_info_subtitle_source(
    char *output,
    size_t output_size
) {
    if (!output || output_size == 0) {
        return;
    }

    if (!prospero_subtitle_enabled) {
        snprintf(
            output,
            output_size,
            "OFF"
        );

        return;
    }

    if (
        prospero_subtitle_use_external &&
        prospero_subtitle_count > 0
    ) {
        snprintf(
            output,
            output_size,
            "EXTERNAL SRT"
        );

        return;
    }

    if (
        play_fmt &&
        prospero_embedded_subtitle_stream_index >= 0 &&
        prospero_embedded_subtitle_stream_index <
            (int)play_fmt->nb_streams
    ) {
        AVStream *stream =
            play_fmt->streams[
                prospero_embedded_subtitle_stream_index
            ];

        const char *codec_name =
            (
                stream &&
                stream->codecpar
            )
                ? avcodec_get_name(
                    stream->codecpar->codec_id
                )
                : "unknown";

        snprintf(
            output,
            output_size,
            "%s  %s",
            prospero_media_info_language(
                stream
            ),
            codec_name
                ? codec_name
                : "unknown"
        );

        return;
    }

    snprintf(
        output,
        output_size,
        "NONE"
    );
}

/* PROSPERO_MEDIA_INFO_V2_HELPERS_END */


void draw_media_info_screen(uint32_t *fb) {
    static int intro_frame = 0;

    if (intro_frame < 12) {
        intro_frame++;
    }

    int remaining =
        12 - intro_frame;

    if (remaining < 0) {
        remaining = 0;
    }

    int entrance_offset =
        (remaining * remaining) / 3;

    AVStream *video_stream = NULL;
    AVStream *audio_stream = NULL;

    AVCodecParameters *video_parameters = NULL;
    AVCodecParameters *audio_parameters = NULL;

    if (
        play_fmt &&
        video_stream_index >= 0 &&
        video_stream_index <
            (int)play_fmt->nb_streams
    ) {
        video_stream =
            play_fmt->streams[
                video_stream_index
            ];

        if (video_stream) {
            video_parameters =
                video_stream->codecpar;
        }
    }

    if (
        play_fmt &&
        audio_stream_index >= 0 &&
        audio_stream_index <
            (int)play_fmt->nb_streams
    ) {
        audio_stream =
            play_fmt->streams[
                audio_stream_index
            ];

        if (audio_stream) {
            audio_parameters =
                audio_stream->codecpar;
        }
    }

    int video_track_count =
        prospero_media_info_count_streams(
            play_fmt,
            AVMEDIA_TYPE_VIDEO
        );

    int audio_track_count =
        prospero_media_info_count_streams(
            play_fmt,
            AVMEDIA_TYPE_AUDIO
        );

    int embedded_subtitle_count =
        prospero_media_info_count_streams(
            play_fmt,
            AVMEDIA_TYPE_SUBTITLE
        );

    int external_subtitle_count =
        prospero_subtitle_count > 0
            ? 1
            : 0;

    int subtitle_track_count =
        embedded_subtitle_count +
        external_subtitle_count;

    char title[160];
    char size_text[64];
    char duration_text[32];
    char container_text[64];

    char video_codec_text[64];
    char resolution_text[64];
    char frame_rate_text[64];
    char video_bitrate_text[64];
    char video_heading[64];

    char audio_codec_text[64];
    char audio_track_text[64];
    char audio_channels_text[64];
    char audio_rate_text[64];
    char audio_bitrate_text[64];
    char audio_heading[64];

    char subtitle_source_text[96];
    char subtitle_count_text[64];
    char summary[320];

    snprintf(
        title,
        sizeof(title),
        "%s",
        current_metadata.title[0]
            ? current_metadata.title
            : "UNKNOWN MEDIA"
    );

    if (strlen(title) > 52) {
        title[49] = '.';
        title[50] = '.';
        title[51] = '.';
        title[52] = 0;
    }

    format_file_size(
        size_text,
        sizeof(size_text),
        current_metadata.file_size_bytes
    );

    format_time_mmss(
        duration_text,
        sizeof(duration_text),
        current_metadata.duration_sec
    );

    prospero_display_container(
        container_text,
        sizeof(container_text),
        current_metadata.container,
        current_metadata.path[0]
            ? current_metadata.path
            : current_media_path
    );

    const char *raw_video_codec =
        (
            video_parameters
        )
            ? avcodec_get_name(
                video_parameters->codec_id
            )
            : current_metadata.video_codec;

    prospero_display_codec(
        video_codec_text,
        sizeof(video_codec_text),
        raw_video_codec,
        0
    );

    const char *raw_audio_codec =
        (
            audio_parameters
        )
            ? avcodec_get_name(
                audio_parameters->codec_id
            )
            : current_metadata.audio_codec;

    prospero_display_codec(
        audio_codec_text,
        sizeof(audio_codec_text),
        raw_audio_codec,
        1
    );

    int display_width =
        video_parameters
            ? video_parameters->width
            : current_metadata.width;

    int display_height =
        video_parameters
            ? video_parameters->height
            : current_metadata.height;

    snprintf(
        resolution_text,
        sizeof(resolution_text),
        "%d X %d",
        display_width,
        display_height
    );

    double active_fps =
        video_fps;

    if (
        active_fps <= 0.0 &&
        video_stream &&
        video_stream->avg_frame_rate.num > 0 &&
        video_stream->avg_frame_rate.den > 0
    ) {
        active_fps =
            av_q2d(
                video_stream->avg_frame_rate
            );
    }

    if (active_fps > 0.0) {
        snprintf(
            frame_rate_text,
            sizeof(frame_rate_text),
            "%.3f FPS",
            active_fps
        );
    } else {
        snprintf(
            frame_rate_text,
            sizeof(frame_rate_text),
            "UNKNOWN"
        );
    }

    int64_t video_bitrate =
        video_parameters
            ? video_parameters->bit_rate
            : 0;

    if (
        video_bitrate <= 0 &&
        play_fmt
    ) {
        video_bitrate =
            play_fmt->bit_rate;
    }

    prospero_media_info_bitrate(
        video_bitrate_text,
        sizeof(video_bitrate_text),
        video_bitrate
    );

    int audio_ordinal = 0;

    if (
        play_fmt &&
        audio_stream_index >= 0
    ) {
        int seen = 0;

        for (
            unsigned int index = 0;
            index < play_fmt->nb_streams;
            index++
        ) {
            AVStream *stream =
                play_fmt->streams[index];

            if (
                !stream ||
                !stream->codecpar ||
                stream->codecpar->codec_type !=
                    AVMEDIA_TYPE_AUDIO
            ) {
                continue;
            }

            seen++;

            if (
                (int)index ==
                audio_stream_index
            ) {
                audio_ordinal = seen;
                break;
            }
        }
    }

    snprintf(
        audio_track_text,
        sizeof(audio_track_text),
        "%d / %d   %s",
        audio_ordinal,
        audio_track_count,
        prospero_media_info_language(
            audio_stream
        )
    );

    int audio_channels =
        audio_parameters
            ? audio_parameters->
                ch_layout.nb_channels
            : 0;

    snprintf(
        audio_channels_text,
        sizeof(audio_channels_text),
        "%d CHANNEL%s",
        audio_channels,
        audio_channels == 1
            ? ""
            : "S"
    );

    int audio_rate =
        audio_parameters
            ? audio_parameters->sample_rate
            : detected_audio_rate;

    if (audio_rate > 0) {
        snprintf(
            audio_rate_text,
            sizeof(audio_rate_text),
            "%d HZ",
            audio_rate
        );
    } else {
        snprintf(
            audio_rate_text,
            sizeof(audio_rate_text),
            "UNKNOWN"
        );
    }

    prospero_media_info_bitrate(
        audio_bitrate_text,
        sizeof(audio_bitrate_text),
        audio_parameters
            ? audio_parameters->bit_rate
            : 0
    );

    prospero_media_info_subtitle_source(
        subtitle_source_text,
        sizeof(subtitle_source_text)
    );

    snprintf(
        subtitle_count_text,
        sizeof(subtitle_count_text),
        "%d TRACK%s",
        subtitle_track_count,
        subtitle_track_count == 1
            ? ""
            : "S"
    );

    snprintf(
        video_heading,
        sizeof(video_heading),
        "VIDEO  /  %d STREAM%s",
        video_track_count,
        video_track_count == 1
            ? ""
            : "S"
    );

    snprintf(
        audio_heading,
        sizeof(audio_heading),
        "AUDIO  /  %d TRACK%s",
        audio_track_count,
        audio_track_count == 1
            ? ""
            : "S"
    );

    snprintf(
        summary,
        sizeof(summary),
        "%s  /  %s  /  %s  /  %s",
        container_text[0]
            ? container_text
            : "MEDIA",
        video_codec_text[0]
            ? video_codec_text
            : "UNKNOWN VIDEO",
        audio_codec_text[0]
            ? audio_codec_text
            : "UNKNOWN AUDIO",
        resolution_text
    );

    rr_bg(fb);

    rr_fill(
        fb,
        0,
        0,
        1920,
        1080,
        RR_BGRA(11,11,11,42)
    );

    /*
     * Header
     */
    rr_fill(
        fb,
        82,
        72,
        5,
        76,
        RR_BGRA(205,205,205,225)
    );

    rr_fill(
        fb,
        82,
        144,
        31,
        2,
        RR_BGRA(205,205,205,195)
    );

    rr_text(
        fb,
        122,
        68,
        "MEDIA INFORMATION",
        RR_BGRA(232, 244, 255, 240),
        3
    );

    rr_text(
        fb,
        124,
        130,
        "CURRENT FILE, STREAM AND PLAYBACK DETAILS",
        RR_BGRA(199,199,199,225),
        1
    );

    /*
     * Media identity card
     */
    int identity_x = 180;
    int identity_y =
        195 + entrance_offset;
    int identity_w = 1560;
    int identity_h = 135;

    ui_round_asset(
        fb,
        identity_x,
        identity_y,
        identity_w,
        identity_h,
        28,
        RR_BGRA(15,15,15,220),
        RR_BGRA(192,192,192,165),
        RR_BGRA(182,182,182,44)
    );

    rr_browser_icon(
        fb,
        identity_x + 48,
        identity_y + 31,
        1
    );

    rr_text(
        fb,
        identity_x + 144,
        identity_y + 24,
        title,
        RR_BGRA(235, 246, 255, 245),
        2
    );

    rr_text(
        fb,
        identity_x + 146,
        identity_y + 75,
        summary,
        RR_BGRA(204,204,204,220),
        1
    );

    char top_right[96];

    snprintf(
        top_right,
        sizeof(top_right),
        "%s  /  %s",
        duration_text,
        size_text
    );

    rr_text(
        fb,
        identity_x + identity_w - 390,
        identity_y + 31,
        top_right,
        RR_BGRA(211,211,211,215),
        0
    );

    /*
     * Video card
     */
    int card_y = 370;
    int card_w = 750;
    int card_h = 350;

    int video_x =
        180 - entrance_offset;

    int audio_x =
        990 + entrance_offset;

    ui_round_asset(
        fb,
        video_x,
        card_y,
        card_w,
        card_h,
        28,
        RR_BGRA(15,15,15,210),
        RR_BGRA(182,182,182,130),
        RR_BGRA(173,173,173,38)
    );

    rr_browser_icon(
        fb,
        video_x + 46,
        card_y + 34,
        1
    );

    rr_text(
        fb,
        video_x + 136,
        card_y + 38,
        video_heading,
        RR_BGRA(235, 246, 255, 240),
        1
    );

    rr_text(
        fb,
        video_x + 48,
        card_y + 112,
        "CODEC",
        RR_BGRA(191,191,191,205),
        0
    );

    rr_text(
        fb,
        video_x + 270,
        card_y + 110,
        video_codec_text[0]
            ? video_codec_text
            : "UNKNOWN",
        RR_BGRA(230, 242, 252, 235),
        1
    );

    rr_text(
        fb,
        video_x + 48,
        card_y + 166,
        "RESOLUTION",
        RR_BGRA(191,191,191,205),
        0
    );

    rr_text(
        fb,
        video_x + 270,
        card_y + 164,
        resolution_text,
        RR_BGRA(230, 242, 252, 235),
        1
    );

    rr_text(
        fb,
        video_x + 48,
        card_y + 220,
        "FRAME RATE",
        RR_BGRA(191,191,191,205),
        0
    );

    rr_text(
        fb,
        video_x + 270,
        card_y + 218,
        frame_rate_text,
        RR_BGRA(230, 242, 252, 235),
        1
    );

    rr_text(
        fb,
        video_x + 48,
        card_y + 274,
        "BITRATE",
        RR_BGRA(191,191,191,205),
        0
    );

    rr_text(
        fb,
        video_x + 270,
        card_y + 272,
        video_bitrate_text,
        RR_BGRA(230, 242, 252, 235),
        1
    );

    /*
     * Audio card
     */
    ui_round_asset(
        fb,
        audio_x,
        card_y,
        card_w,
        card_h,
        28,
        RR_BGRA(15,15,15,210),
        RR_BGRA(182,182,182,130),
        RR_BGRA(173,173,173,38)
    );

    rr_browser_icon(
        fb,
        audio_x + 46,
        card_y + 34,
        2
    );

    rr_text(
        fb,
        audio_x + 136,
        card_y + 38,
        audio_heading,
        RR_BGRA(235, 246, 255, 240),
        1
    );

    rr_text(
        fb,
        audio_x + 48,
        card_y + 112,
        "ACTIVE TRACK",
        RR_BGRA(191,191,191,205),
        0
    );

    rr_text(
        fb,
        audio_x + 270,
        card_y + 110,
        audio_track_count > 0
            ? audio_track_text
            : "NONE",
        RR_BGRA(230, 242, 252, 235),
        1
    );

    rr_text(
        fb,
        audio_x + 48,
        card_y + 166,
        "CODEC",
        RR_BGRA(191,191,191,205),
        0
    );

    rr_text(
        fb,
        audio_x + 270,
        card_y + 164,
        audio_codec_text[0]
            ? audio_codec_text
            : "UNKNOWN",
        RR_BGRA(230, 242, 252, 235),
        1
    );

    rr_text(
        fb,
        audio_x + 48,
        card_y + 220,
        "OUTPUT",
        RR_BGRA(191,191,191,205),
        0
    );

    char audio_output_text[96];

    snprintf(
        audio_output_text,
        sizeof(audio_output_text),
        "%s  /  %s",
        audio_channels_text,
        audio_rate_text
    );

    rr_text(
        fb,
        audio_x + 270,
        card_y + 218,
        audio_output_text,
        RR_BGRA(230, 242, 252, 235),
        1
    );

    rr_text(
        fb,
        audio_x + 48,
        card_y + 274,
        "BITRATE",
        RR_BGRA(191,191,191,205),
        0
    );

    rr_text(
        fb,
        audio_x + 270,
        card_y + 272,
        audio_bitrate_text,
        RR_BGRA(230, 242, 252, 235),
        1
    );

    /*
     * Playback and subtitle summary card
     */
    int playback_x = 180;
    int playback_y = 758;
    int playback_w = 1560;
    int playback_h = 132;

    ui_round_asset(
        fb,
        playback_x,
        playback_y,
        playback_w,
        playback_h,
        24,
        RR_BGRA(15,15,15,205),
        RR_BGRA(174,174,174,110),
        RR_BGRA(165,165,165,32)
    );

    rr_text(
        fb,
        playback_x + 46,
        playback_y + 24,
        "PLAYBACK PROFILE",
        RR_BGRA(191,191,191,205),
        0
    );

    rr_text(
        fb,
        playback_x + 46,
        playback_y + 67,
        profile_name(current_profile),
        RR_BGRA(230, 242, 252, 235),
        1
    );

    rr_text(
        fb,
        playback_x + 400,
        playback_y + 24,
        "CONTAINER",
        RR_BGRA(191,191,191,205),
        0
    );

    rr_text(
        fb,
        playback_x + 400,
        playback_y + 67,
        container_text[0]
            ? container_text
            : "UNKNOWN",
        RR_BGRA(230, 242, 252, 235),
        1
    );

    rr_text(
        fb,
        playback_x + 700,
        playback_y + 24,
        "SUBTITLE SOURCE",
        RR_BGRA(191,191,191,205),
        0
    );

    rr_text(
        fb,
        playback_x + 700,
        playback_y + 67,
        subtitle_source_text,
        RR_BGRA(230, 242, 252, 235),
        1
    );

    rr_text(
        fb,
        playback_x + 1070,
        playback_y + 24,
        "SUBTITLE TRACKS",
        RR_BGRA(191,191,191,205),
        0
    );

    rr_text(
        fb,
        playback_x + 1070,
        playback_y + 67,
        subtitle_count_text,
        RR_BGRA(230, 242, 252, 235),
        1
    );

    /*
     * Footer
     */
    rr_fill(
        fb,
        0,
        948,
        1920,
        1,
        RR_BGRA(186,186,186,48)
    );

    rr_fill(
        fb,
        0,
        949,
        1920,
        79,
        RR_BGRA(12,12,12,138)
    );

    rr_control(
        fb,
        92,
        982,
        4
    );

    rr_text(
        fb,
        148,
        998,
        "BACK",
        RR_BGRA(209,209,209,205),
        0
    );

    rr_icon(
        fb,
        1512,
        965,
        5
    );

    rr_text(
        fb,
        1585,
        997,
        "MEDIA DETAILS",
        RR_BGRA(203,203,203,205),
        0
    );
}


/* PROSPERO_RELEASE_1_0 */
void draw_about_support_screen(uint32_t *fb) {
    static int about_frame = 0;
    static int intro_frame = 0;

    about_frame++;

    if (intro_frame < 16) {
        intro_frame++;
    }

    int remaining = 16 - intro_frame;

    if (remaining < 0) {
        remaining = 0;
    }

    int panel_offset =
        (remaining * remaining) / 3;

    rr_bg(fb);


    /*
     * Header.
     */
    rr_fill(
        fb,
        82,
        74,
        5,
        76,
        RR_BGRA(205,205,205,225)
    );

    rr_fill(
        fb,
        82,
        146,
        31,
        2,
        RR_BGRA(205,205,205,195)
    );

    rr_text(
        fb,
        122,
        70,
        "ABOUT / SUPPORT",
        RR_BGRA(232, 244, 255, 240),
        3
    );

    rr_text(
        fb,
        124,
        132,
        "PROJECT INFORMATION AND CONTROLLER GUIDE",
        RR_BGRA(199,199,199,225),
        1
    );

    /*
     * Left information panel.
     */
    int left_x = 180 - panel_offset;
    int panel_y = 230;
    int panel_w = 740;
    int panel_h = 610;

    ui_round_asset(
        fb,
        left_x,
        panel_y,
        panel_w,
        panel_h,
        30,
        RR_BGRA(17,17,17,220),
        RR_BGRA(191,191,191,150),
        RR_BGRA(182,182,182,48)
    );

    rr_icon(
        fb,
        left_x + 54,
        panel_y + 48,
        5
    );

    rr_text(
        fb,
        left_x + 145,
        panel_y + 52,
        "PS PLAY",
        RR_BGRA(235, 246, 255, 245),
        2
    );

    rr_text(
        fb,
        left_x + 148,
        panel_y + 98,
        "VERSION 1.5",
        RR_BGRA(199,199,199,225),
        1
    );

    rr_fill(
        fb,
        left_x + 54,
        panel_y + 154,
        panel_w - 108,
        1,
        RR_BGRA(171,171,171,65)
    );

    rr_text(
        fb,
        left_x + 58,
        panel_y + 198,
        "PURPOSE",
        RR_BGRA(196,196,196,210),
        0
    );

    rr_text(
        fb,
        left_x + 58,
        panel_y + 238,
        "A MODERN HOMEBREW MEDIA PLAYER",
        RR_BGRA(232, 244, 255, 235),
        1
    );

    rr_text(
        fb,
        left_x + 58,
        panel_y + 276,
        "BUILT FOR PLAYSTATION 5",
        RR_BGRA(232, 244, 255, 235),
        1
    );

    rr_text(
        fb,
        left_x + 58,
        panel_y + 338,
        "CURRENT RELEASE",
        RR_BGRA(196,196,196,210),
        0
    );

    rr_text(
        fb,
        left_x + 58,
        panel_y + 378,
        "VERSION 1.5",
        RR_BGRA(232, 244, 255, 235),
        1
    );

    rr_text(
        fb,
        left_x + 58,
        panel_y + 440,
        "CREDITS",
        RR_BGRA(196,196,196,210),
        0
    );

    rr_text(
        fb,
        left_x + 58,
        panel_y + 480,
        "InsideMatrixDev/MounirHero",
        RR_BGRA(205,205,205,235),
        1
    );

    rr_text(
        fb,
        left_x + 58,
        panel_y + 520,
        "based on: Prospero Player 1.0",
        RR_BGRA(150,150,150,200),
        0
    );

    /*
     * Right controller guide with real face-button icons.
     */
    int right_x = 1000 + panel_offset;

    ui_round_asset(
        fb,
        right_x,
        panel_y,
        panel_w,
        panel_h,
        30,
        RR_BGRA(17,17,17,220),
        RR_BGRA(191,191,191,150),
        RR_BGRA(182,182,182,48)
    );

    rr_icon(
        fb,
        right_x + 54,
        panel_y + 48,
        4
    );

    rr_text(
        fb,
        right_x + 145,
        panel_y + 52,
        "CONTROLLER GUIDE",
        RR_BGRA(235, 246, 255, 245),
        2
    );

    rr_text(
        fb,
        right_x + 148,
        panel_y + 98,
        "PLAYER AND USB SHORTCUTS",
        RR_BGRA(199,199,199,225),
        1
    );

    rr_fill(
        fb,
        right_x + 54,
        panel_y + 154,
        panel_w - 108,
        1,
        RR_BGRA(171,171,171,65)
    );

    /*
     * Left column — face-button actions.
     */
    int left_icon_x =
        right_x + 62;

    int left_text_x =
        right_x + 125;

    int guide_y =
        panel_y + 198;

    rr_control(
        fb,
        left_icon_x,
        guide_y - 10,
        0
    );

    rr_text(
        fb,
        left_text_x,
        guide_y + 6,
        "PLAY / PAUSE",
        RR_BGRA(225, 239, 250, 230),
        0
    );

    guide_y += 78;

    rr_control(
        fb,
        left_icon_x,
        guide_y - 10,
        5
    );

    rr_text(
        fb,
        left_text_x,
        guide_y + 6,
        "MEDIA INFO",
        RR_BGRA(225, 239, 250, 230),
        0
    );

    guide_y += 78;

    rr_control(
        fb,
        left_icon_x,
        guide_y - 10,
        6
    );

    rr_text(
        fb,
        left_text_x,
        guide_y + 6,
        "SUBTITLES ON / OFF",
        RR_BGRA(225, 239, 250, 230),
        0
    );

    guide_y += 78;

    rr_control(
        fb,
        left_icon_x,
        guide_y - 10,
        5
    );

    rr_text(
        fb,
        left_text_x,
        guide_y + 6,
        "HOLD: SUBTITLE TRACK",
        RR_BGRA(225, 239, 250, 230),
        0
    );

    /*
     * Right column — shoulder and directional controls.
     */
    int right_label_x =
        right_x + 390;

    rr_text(
        fb,
        right_label_x,
        panel_y + 204,
        "R2",
        RR_BGRA(205,205,205,235),
        1
    );

    rr_text(
        fb,
        right_label_x + 76,
        panel_y + 208,
        "AUDIO TRACK",
        RR_BGRA(225, 239, 250, 230),
        0
    );

    rr_text(
        fb,
        right_label_x,
        panel_y + 282,
        "UP/DN",
        RR_BGRA(205,205,205,235),
        1
    );

    rr_text(
        fb,
        right_label_x + 76,
        panel_y + 286,
        "MINI PROGRESS BAR",
        RR_BGRA(225, 239, 250, 230),
        0
    );

    rr_text(
        fb,
        right_label_x,
        panel_y + 360,
        "L / R",
        RR_BGRA(205,205,205,235),
        1
    );

    rr_text(
        fb,
        right_label_x + 96,
        panel_y + 364,
        "SEEK 10 SEC",
        RR_BGRA(225, 239, 250, 230),
        0
    );

    rr_text(
        fb,
        right_label_x,
        panel_y + 438,
        "L1/R1",
        RR_BGRA(205,205,205,235),
        1
    );

    rr_text(
        fb,
        right_label_x + 96,
        panel_y + 442,
        "SEEK 10 SEC",
        RR_BGRA(225, 239, 250, 230),
        0
    );

    rr_text(
        fb,
        right_label_x,
        panel_y + 516,
        "L3",
        RR_BGRA(205,205,205,235),
        1
    );

    rr_text(
        fb,
        right_label_x + 126,
        panel_y + 520,
        "VIEW MODE",
        RR_BGRA(225, 239, 250, 230),
        0
    );

    rr_text(
        fb,
        right_x + 58,
        panel_y + 570,
        "OPTIONS  STATISTICS     /     CIRCLE  BACK",
        RR_BGRA(192,192,192,200),
        0
    );

    /*
     * Footer.
     */
    rr_fill(
        fb,
        0,
        948,
        1920,
        1,
        RR_BGRA(186,186,186,48)
    );

    rr_fill(
        fb,
        0,
        949,
        1920,
        79,
        RR_BGRA(12,12,12,138)
    );

    rr_control(
        fb,
        92,
        982,
        4
    );

    rr_text(
        fb,
        148,
        998,
        "BACK",
        RR_BGRA(209,209,209,205),
        0
    );

    rr_icon(
        fb,
        1512,
        965,
        5
    );

    rr_text(
        fb,
        1585,
        997,
        "ABOUT PROSPERO",
        RR_BGRA(203,203,203,205),
        0
    );
}


void draw_recent_files_screen(uint32_t *fb) {
    static int initialized = 0;
    static int selected_y_fp = 0;
    static int recent_frame = 0;
    static int recent_scroll = 0;

    const int visible = 5;
    const int row_x = 180;
    const int row_start_y = 254;
    const int row_gap = 126;

    if (recent_file_count <= 0) {
        recent_selected = 0;
        recent_scroll = 0;
    } else {
        if (recent_selected < 0) {
            recent_selected = 0;
        }

        if (recent_selected >= recent_file_count) {
            recent_selected = recent_file_count - 1;
        }

        if (recent_selected < recent_scroll) {
            recent_scroll = recent_selected;
        }

        if (recent_selected >= recent_scroll + visible) {
            recent_scroll =
                recent_selected - visible + 1;
        }

        if (recent_scroll < 0) {
            recent_scroll = 0;
        }
    }

    int visible_selected =
        recent_selected - recent_scroll;

    if (visible_selected < 0) {
        visible_selected = 0;
    }

    if (visible_selected >= visible) {
        visible_selected = visible - 1;
    }

    int target_y_fp =
        (
            row_start_y +
            visible_selected * row_gap
        ) << 8;

    if (!initialized) {
        selected_y_fp = target_y_fp;
        initialized = 1;
    }

    int distance =
        target_y_fp - selected_y_fp;

    if (distance > -160 && distance < 160) {
        selected_y_fp = target_y_fp;
    } else {
        selected_y_fp += distance / 2;
    }

    int selected_draw_y =
        selected_y_fp >> 8;

    recent_frame++;

    rr_bg(fb);


    /*
     * Header.
     */
    rr_fill(
        fb,
        82,
        74,
        5,
        76,
        RR_BGRA(205,205,205,225)
    );

    rr_fill(
        fb,
        82,
        146,
        31,
        2,
        RR_BGRA(205,205,205,195)
    );

    rr_text(
        fb,
        122,
        70,
        "RECENT FILES",
        RR_BGRA(232, 244, 255, 240),
        3
    );

    rr_text(
        fb,
        124,
        132,
        "CONTINUE WATCHING FROM YOUR SAVED POSITION",
        RR_BGRA(199,199,199,225),
        1
    );

    char count_text[64];

    if (recent_file_count > 0) {
        snprintf(
            count_text,
            sizeof(count_text),
            "%d OF %d",
            recent_selected + 1,
            recent_file_count
        );
    } else {
        snprintf(
            count_text,
            sizeof(count_text),
            "EMPTY"
        );
    }

    rr_text(
        fb,
        1540,
        132,
        count_text,
        RR_BGRA(203,203,203,210),
        0
    );

    if (recent_file_count <= 0) {
        rr_text(
            fb,
            230,
            330,
            "NO RECENT MEDIA",
            RR_BGRA(232, 244, 255, 230),
            2
        );

        rr_text(
            fb,
            232,
            382,
            "VIDEOS YOU PLAY WILL APPEAR HERE",
            RR_BGRA(204,204,204,215),
            1
        );
    } else {
        /*
         * Normal cards.
         */
        for (int row = 0; row < visible; row++) {
            int index =
                recent_scroll + row;

            if (index >= recent_file_count) {
                break;
            }

            int y =
                row_start_y + row * row_gap;

            rr_img(
                fb,
                row_x,
                y,
                RR_CARD_NORMAL_W,
                RR_CARD_NORMAL_H,
                RR_CARD_NORMAL
            );
        }

        /*
         * Gliding selected card.
         */
        rr_img(
            fb,
            row_x,
            selected_draw_y,
            RR_CARD_SELECTED_W,
            RR_CARD_SELECTED_H,
            RR_CARD_SELECTED
        );

        for (int row = 0; row < visible; row++) {
            int index =
                recent_scroll + row;

            if (index >= recent_file_count) {
                break;
            }

            RecentFileEntry *entry =
                &recent_files[index];

            int y =
                row_start_y + row * row_gap;

            char title[128];
            char position_text[96];
            char elapsed[32];
            char duration[32];

            snprintf(
                title,
                sizeof(title),
                "%s",
                entry->title[0]
                    ? entry->title
                    : entry->path
            );

            if (strlen(title) > 48) {
                title[45] = '.';
                title[46] = '.';
                title[47] = '.';
                title[48] = 0;
            }

            format_time_mmss(
                elapsed,
                sizeof(elapsed),
                entry->last_pos
            );

            if (entry->duration > 1.0) {
                format_time_mmss(
                    duration,
                    sizeof(duration),
                    entry->duration
                );

                snprintf(
                    position_text,
                    sizeof(position_text),
                    "%s  /  %s",
                    elapsed,
                    duration
                );
            } else {
                snprintf(
                    position_text,
                    sizeof(position_text),
                    "RESUME AT  %s",
                    elapsed
                );
            }

            rr_browser_icon(
                fb,
                row_x + 48,
                y + 20,
                1
            );

            rr_text(
                fb,
                row_x + 150,
                y + 24,
                title,
                RR_BGRA(232, 244, 255, 238),
                2
            );

            rr_text(
                fb,
                row_x + 152,
                y + 68,
                position_text,
                index == recent_selected
                    ? RR_BGRA(205,205,205,235)
                    : RR_BGRA(204,204,204,220),
                1
            );

            /*
             * One watch-progress bar (right of time text, left of chevron).
             * Leave room for the right-side PREVIEW panel (~1440).
             */
            int progress_x = row_x + 820;
            int progress_y = y + 76;
            int progress_w = 220;
            int progress_h = 5;

            if (entry->duration > 1.0) {
                rr_fill(
                    fb,
                    progress_x,
                    progress_y,
                    progress_w,
                    progress_h,
                    RR_BGRA(86,86,86,145)
                );

                double ratio =
                    entry->last_pos /
                    entry->duration;

                if (ratio < 0.0) {
                    ratio = 0.0;
                }

                if (ratio > 1.0) {
                    ratio = 1.0;
                }

                int filled =
                    (int)(ratio * progress_w);

                if (filled > 0) {
                    rr_fill(
                        fb,
                        progress_x,
                        progress_y,
                        filled,
                        progress_h,
                        RR_BGRA(204,204,204,225)
                    );
                }
            }

            int arrow_shift = 0;

            if (index == recent_selected) {
                int phase =
                    recent_frame % 36;

                if (phase < 9) {
                    arrow_shift =
                        phase / 3;
                } else if (phase < 18) {
                    arrow_shift =
                        3 - ((phase - 9) / 3);
                }
            }

            /* After progress bar (ends ~1040), before preview panel (1440) */
            rr_icon(
                fb,
                row_x + 1080 + arrow_shift,
                y + 21,
                6
            );
        }
    }

    if (recent_file_count > 0 &&
        recent_selected >= 0 &&
        recent_selected < recent_file_count) {
        prospero_browser_preview_ensure(
            recent_files[recent_selected].path, 0);
        prospero_browser_preview_draw_panel(fb, 1440, 280);
        rr_text(
            fb,
            1454,
            280 + 14 + PROSPERO_BROWSER_PREVIEW_H + 14,
            prospero_browser_preview_valid ? "PREVIEW" : "NO PREVIEW",
            RR_BGRA(199,199,199,220),
            0
        );
    }

    /*
     * Footer.
     */
    rr_fill(
        fb,
        0,
        948,
        1920,
        1,
        RR_BGRA(186,186,186,48)
    );

    rr_fill(
        fb,
        0,
        949,
        1920,
        79,
        RR_BGRA(12,12,12,138)
    );

    rr_control(
        fb,
        92,
        982,
        0
    );

    rr_text(
        fb,
        148,
        998,
        recent_file_count > 0
            ? "PLAY"
            : "SELECT",
        RR_BGRA(209,209,209,205),
        0
    );

    rr_control(
        fb,
        292,
        982,
        4
    );

    rr_text(
        fb,
        348,
        998,
        "BACK",
        RR_BGRA(209,209,209,205),
        0
    );

    rr_icon(
        fb,
        1512,
        965,
        1
    );

    rr_text(
        fb,
        1585,
        997,
        "RECENT MEDIA",
        RR_BGRA(203,203,203,205),
        0
    );
}


void draw_favorites_screen(uint32_t *fb) {
    static int initialized = 0;
    static int selected_y_fp = 0;
    static int favorites_frame = 0;
    static int favorites_scroll = 0;

    const int visible = 5;
    const int row_x = 180;
    const int row_start_y = 254;
    const int row_gap = 126;

    if (favorite_count <= 0) {
        favorite_selected = 0;
        favorites_scroll = 0;
    } else {
        if (favorite_selected < 0) {
            favorite_selected = 0;
        }

        if (favorite_selected >= favorite_count) {
            favorite_selected = favorite_count - 1;
        }

        if (favorite_selected < favorites_scroll) {
            favorites_scroll = favorite_selected;
        }

        if (
            favorite_selected >=
            favorites_scroll + visible
        ) {
            favorites_scroll =
                favorite_selected - visible + 1;
        }

        if (favorites_scroll < 0) {
            favorites_scroll = 0;
        }
    }

    int visible_selected =
        favorite_selected - favorites_scroll;

    if (visible_selected < 0) {
        visible_selected = 0;
    }

    if (visible_selected >= visible) {
        visible_selected = visible - 1;
    }

    int target_y_fp =
        (
            row_start_y +
            visible_selected * row_gap
        ) << 8;

    if (!initialized) {
        selected_y_fp = target_y_fp;
        initialized = 1;
    }

    int distance =
        target_y_fp - selected_y_fp;

    if (distance > -160 && distance < 160) {
        selected_y_fp = target_y_fp;
    } else {
        selected_y_fp += distance / 2;
    }

    int selected_draw_y =
        selected_y_fp >> 8;

    favorites_frame++;

    rr_bg(fb);


    /*
     * Header.
     */
    rr_fill(
        fb,
        82,
        74,
        5,
        76,
        RR_BGRA(205,205,205,225)
    );

    rr_fill(
        fb,
        82,
        146,
        31,
        2,
        RR_BGRA(205,205,205,195)
    );

    rr_text(
        fb,
        122,
        70,
        "FAVORITES",
        RR_BGRA(232, 244, 255, 240),
        3
    );

    rr_text(
        fb,
        124,
        132,
        "YOUR SAVED MEDIA COLLECTION",
        RR_BGRA(199,199,199,225),
        1
    );

    char count_text[64];

    if (favorite_count > 0) {
        snprintf(
            count_text,
            sizeof(count_text),
            "%d OF %d",
            favorite_selected + 1,
            favorite_count
        );
    } else {
        snprintf(
            count_text,
            sizeof(count_text),
            "EMPTY"
        );
    }

    rr_text(
        fb,
        1540,
        132,
        count_text,
        RR_BGRA(203,203,203,210),
        0
    );

    if (favorite_count <= 0) {
        rr_text(
            fb,
            230,
            330,
            "NO FAVORITES YET",
            RR_BGRA(232, 244, 255, 230),
            2
        );

        rr_text(
            fb,
            232,
            382,
            "ADD MEDIA TO FAVORITES FROM THE PLAYER",
            RR_BGRA(204,204,204,215),
            1
        );
    } else {
        /*
         * Normal rows.
         */
        for (int row = 0; row < visible; row++) {
            int index =
                favorites_scroll + row;

            if (index >= favorite_count) {
                break;
            }

            int y =
                row_start_y + row * row_gap;

            rr_img(
                fb,
                row_x,
                y,
                RR_CARD_NORMAL_W,
                RR_CARD_NORMAL_H,
                RR_CARD_NORMAL
            );
        }

        /*
         * Selected row.
         */
        rr_img(
            fb,
            row_x,
            selected_draw_y,
            RR_CARD_SELECTED_W,
            RR_CARD_SELECTED_H,
            RR_CARD_SELECTED
        );

        for (int row = 0; row < visible; row++) {
            int index =
                favorites_scroll + row;

            if (index >= favorite_count) {
                break;
            }

            FavoriteEntry *entry =
                &favorite_files[index];

            int y =
                row_start_y + row * row_gap;

            char title[128];
            char duration_text[96];
            char duration[32];

            snprintf(
                title,
                sizeof(title),
                "%s",
                entry->title[0]
                    ? entry->title
                    : entry->path
            );

            if (strlen(title) > 48) {
                title[45] = '.';
                title[46] = '.';
                title[47] = '.';
                title[48] = 0;
            }

            if (entry->duration > 1.0) {
                format_time_mmss(
                    duration,
                    sizeof(duration),
                    entry->duration
                );

                snprintf(
                    duration_text,
                    sizeof(duration_text),
                    "DURATION  %s",
                    duration
                );
            } else {
                snprintf(
                    duration_text,
                    sizeof(duration_text),
                    "SAVED FAVORITE"
                );
            }

            rr_browser_icon(
                fb,
                row_x + 48,
                y + 20,
                1
            );

            rr_text(
                fb,
                row_x + 150,
                y + 24,
                title,
                RR_BGRA(232, 244, 255, 238),
                2
            );

            rr_text(
                fb,
                row_x + 152,
                y + 69,
                duration_text,
                index == favorite_selected
                    ? RR_BGRA(205,205,205,235)
                    : RR_BGRA(204,204,204,220),
                1
            );

            /* One small watch bar if this favorite is also in Recent */
            {
                double fav_pos = 0.0;
                double fav_dur = entry->duration;
                fav_pos = prospero_recent_lookup(entry->path, &fav_dur);
                if (fav_dur < 1.0)
                    fav_dur = entry->duration;
                if (fav_dur > 1.0 && fav_pos > 1.0) {
                    int progress_x = row_x + 820;
                    int progress_y = y + 76;
                    int progress_w = 220;
                    int progress_h = 5;
                    double ratio = fav_pos / fav_dur;
                    int filled;
                    if (ratio < 0.0)
                        ratio = 0.0;
                    if (ratio > 1.0)
                        ratio = 1.0;
                    filled = (int)(ratio * progress_w);
                    rr_fill(
                        fb,
                        progress_x,
                        progress_y,
                        progress_w,
                        progress_h,
                        RR_BGRA(86,86,86,145));
                    if (filled > 0)
                        rr_fill(
                            fb,
                            progress_x,
                            progress_y,
                            filled,
                            progress_h,
                            RR_BGRA(204,204,204,225));
                }
            }

            int arrow_shift = 0;

            if (index == favorite_selected) {
                int phase =
                    favorites_frame % 36;

                if (phase < 9) {
                    arrow_shift =
                        phase / 3;
                } else if (phase < 18) {
                    arrow_shift =
                        3 - ((phase - 9) / 3);
                }
            }

            rr_icon(
                fb,
                row_x + 1080 + arrow_shift,
                y + 21,
                6
            );
        }
    }

    if (favorite_count > 0 &&
        favorite_selected >= 0 &&
        favorite_selected < favorite_count) {
        prospero_browser_preview_ensure(
            favorite_files[favorite_selected].path, 0);
        prospero_browser_preview_draw_panel(fb, 1440, 280);
        rr_text(
            fb,
            1454,
            280 + 14 + PROSPERO_BROWSER_PREVIEW_H + 14,
            prospero_browser_preview_valid ? "PREVIEW" : "NO PREVIEW",
            RR_BGRA(199,199,199,220),
            0
        );
    }

    /*
     * Footer.
     */
    rr_fill(
        fb,
        0,
        948,
        1920,
        1,
        RR_BGRA(186,186,186,48)
    );

    rr_fill(
        fb,
        0,
        949,
        1920,
        79,
        RR_BGRA(12,12,12,138)
    );

    rr_control(
        fb,
        92,
        982,
        0
    );

    rr_text(
        fb,
        148,
        998,
        favorite_count > 0
            ? "PLAY"
            : "SELECT",
        RR_BGRA(209,209,209,205),
        0
    );

    /*
     * Triangle placeholder drawn as text until a dedicated
     * Triangle asset is added.
     */
    rr_control(
        fb,
        292,
        982,
        5
    );

    rr_text(
        fb,
        348,
        998,
        "REMOVE",
        RR_BGRA(209,209,209,205),
        0
    );

    rr_control(
        fb,
        540,
        982,
        4
    );

    rr_text(
        fb,
        596,
        998,
        "BACK",
        RR_BGRA(209,209,209,205),
        0
    );

    rr_icon(
        fb,
        1512,
        965,
        2
    );

    rr_text(
        fb,
        1585,
        997,
        "FAVORITE MEDIA",
        RR_BGRA(203,203,203,205),
        0
    );
}



/* PROSPERO_PERSISTENT_SETTINGS_START */

#define PROSPERO_SETTINGS_FILE \
    "/mnt/usb0/.prospero_player_settings.cfg"



static const char *prospero_view_mode_name(
    int mode
) {
    if (mode == 1) {
        return "FILL";
    }

    if (mode == 2) {
        return "STRETCH";
    }

    return "FIT";
}


static void prospero_settings_save(void)
{
    FILE *file =
        fopen(
            PROSPERO_SETTINGS_FILE,
            "w"
        );

    if (!file) {
        toast(
            "SETTINGS",
            "SAVE FAILED"
        );

        return;
    }

    fprintf(
        file,
        "%d\n%d\n%d\n%d\n%d\n",
        (int)current_profile,
        prospero_resume_playback_enabled,
        prospero_default_view_mode,
        prospero_auto_subtitles_enabled,
        show_debug_overlay
    );

    fclose(file);
}


static void prospero_settings_load(void)
{
    int loaded_profile =
        (int)current_profile;

    int loaded_resume = 1;
    int loaded_view = 0;
    int loaded_subtitles = 1;
    int loaded_developer = 0;

    FILE *file =
        fopen(
            PROSPERO_SETTINGS_FILE,
            "r"
        );

    if (file) {
        int values_read =
            fscanf(
                file,
                "%d%d%d%d%d",
                &loaded_profile,
                &loaded_resume,
                &loaded_view,
                &loaded_subtitles,
                &loaded_developer
            );

        fclose(file);

        if (values_read != 5) {
            loaded_profile = 0;
            loaded_resume = 1;
            loaded_view = 0;
            loaded_subtitles = 1;
            loaded_developer = 0;
        }
    }

    if (
        loaded_profile < 0 ||
        loaded_profile > 3
    ) {
        loaded_profile = 0;
    }

    if (
        loaded_view < 0 ||
        loaded_view > 2
    ) {
        loaded_view = 0;
    }

    current_profile =
        (PlaybackProfile)loaded_profile;

    prospero_resume_playback_enabled =
        loaded_resume ? 1 : 0;

    prospero_default_view_mode =
        loaded_view;

    prospero_auto_subtitles_enabled =
        loaded_subtitles ? 1 : 0;

    show_debug_overlay =
        loaded_developer ? 1 : 0;

    video_view_mode =
        prospero_default_view_mode;
}


/* Media home tile uninstall (resident launcher on 127.0.0.1:9055) */
#define PROSPERO_MEDIA_LAUNCHER_PORT 9055
static time_t prospero_uninstall_confirm_until = 0;

static int prospero_request_home_tile_uninstall(void)
{
    int fd;
    struct sockaddr_in addr;
    char req[128];
    char resp[256];
    ssize_t n;
    int ok = 0;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PROSPERO_MEDIA_LAUNCHER_PORT);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    n = snprintf(
        req,
        sizeof(req),
        "GET /uninstall HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n"
        "\r\n"
    );
    if (n > 0)
        (void)send(fd, req, (size_t)n, 0);

    n = recv(fd, resp, sizeof(resp) - 1, 0);
    if (n > 0) {
        resp[n] = 0;
        if (strstr(resp, "204") || strstr(resp, "200"))
            ok = 1;
    }
    close(fd);
    return ok ? 0 : -2;
}

static void prospero_settings_activate_selected(void)
{
    if (settings_selected == 0) {
        profile_selected =
            (int)current_profile;

        screen =
            SCREEN_PROFILE_SELECT;

        return;
    }

    if (settings_selected == 1) {
        prospero_resume_playback_enabled =
            !prospero_resume_playback_enabled;

        prospero_settings_save();

        toast(
            "RESUME PLAYBACK",
            prospero_resume_playback_enabled
                ? "ON"
                : "OFF"
        );

        return;
    }

    if (settings_selected == 2) {
        prospero_default_view_mode++;

        if (
            prospero_default_view_mode > 2
        ) {
            prospero_default_view_mode = 0;
        }

        video_view_mode =
            prospero_default_view_mode;

        prospero_settings_save();

        toast(
            "DEFAULT VIEW",
            prospero_view_mode_name(
                prospero_default_view_mode
            )
        );

        return;
    }

    if (settings_selected == 3) {
        prospero_auto_subtitles_enabled =
            !prospero_auto_subtitles_enabled;

        prospero_settings_save();

        toast(
            "AUTO SUBTITLES",
            prospero_auto_subtitles_enabled
                ? "ON"
                : "OFF"
        );

        return;
    }

    if (settings_selected == 4) {
        show_debug_overlay =
            !show_debug_overlay;

        prospero_settings_save();

        toast(
            "DEVELOPER MODE",
            show_debug_overlay
                ? "ON"
                : "OFF"
        );

        return;
    }

    if (settings_selected == 5) {
        time_t now = time(NULL);

        if (
            prospero_uninstall_confirm_until == 0 ||
            now > prospero_uninstall_confirm_until
        ) {
            prospero_uninstall_confirm_until = now + 8;
            toast(
                "REMOVE HOME TILE",
                "PRESS X AGAIN TO CONFIRM"
            );
            return;
        }

        prospero_uninstall_confirm_until = 0;
        toast("REMOVE HOME TILE", "REMOVING...");

        if (prospero_request_home_tile_uninstall() == 0) {
            toast(
                "HOME TILE",
                "REMOVED — RESTART HOME IF NEEDED"
            );
        } else {
            toast(
                "HOME TILE",
                "LAUNCHER OFF — USE UNINSTALL ELF"
            );
        }

        return;
    }

    if (settings_selected == 6) {
        screen = SCREEN_DEVELOPER_TOOLS;
        return;
    }
}

/* PROSPERO_PERSISTENT_SETTINGS_END */


void draw_settings_screen(uint32_t *fb) {
    static int initialized = 0;
    static int selected_y_fp = 0;
    static int settings_frame = 0;

    const int row_x = 180;
    const int row_start_y = 180;
    const int row_gap = 108;
    const int settings_count = 7;

    if (settings_selected < 0) {
        settings_selected = 0;
    }

    if (settings_selected >= settings_count) {
        settings_selected = settings_count - 1;
    }

    int target_y_fp =
        (row_start_y + settings_selected * row_gap) << 8;

    if (!initialized) {
        selected_y_fp = target_y_fp;
        initialized = 1;
    }

    int distance = target_y_fp - selected_y_fp;

    if (distance > -160 && distance < 160) {
        selected_y_fp = target_y_fp;
    } else {
        selected_y_fp += distance / 2;
    }

    int selected_draw_y = selected_y_fp >> 8;

    settings_frame++;

    /* AMOLED pure black background. */
    rr_bg(fb);

    /*
     * Header.
     */
    rr_fill(
        fb,
        82,
        74,
        5,
        76,
        RR_BGRA(205,205,205,225)
    );

    rr_fill(
        fb,
        82,
        146,
        31,
        2,
        RR_BGRA(205,205,205,195)
    );

    rr_text(
        fb,
        122,
        70,
        "SETTINGS",
        RR_BGRA(232, 244, 255, 240),
        3
    );

    rr_text(
        fb,
        124,
        132,
        "PLAYBACK AND APPLICATION PREFERENCES",
        RR_BGRA(199,199,199,225),
        1
    );

    char profile_value[96];
    char resume_value[32];
    char view_value[32];
    char subtitle_value[32];
    char uninstall_value[48];

    snprintf(
        profile_value,
        sizeof(profile_value),
        "%s",
        profile_name(current_profile)
    );

    snprintf(
        resume_value,
        sizeof(resume_value),
        "%s",
        prospero_resume_playback_enabled
            ? "ON"
            : "OFF"
    );

    snprintf(
        view_value,
        sizeof(view_value),
        "%s",
        prospero_view_mode_name(
            prospero_default_view_mode
        )
    );

    snprintf(
        subtitle_value,
        sizeof(subtitle_value),
        "%s",
        prospero_auto_subtitles_enabled
            ? "ON"
            : "OFF"
    );

    snprintf(
        uninstall_value,
        sizeof(uninstall_value),
        "%s",
        (prospero_uninstall_confirm_until != 0 &&
         time(NULL) <= prospero_uninstall_confirm_until)
            ? "PRESS X TO CONFIRM"
            : "REMOVES MEDIA TILE"
    );

    const char *titles[7] = {
        "PLAYBACK PROFILE",
        "RESUME PLAYBACK",
        "DEFAULT ASPECT RATIO",
        "AUTO-DETECT SUBTITLES",
        "DEVELOPER MODE",
        "REMOVE HOME TILE",
        "DEVELOPER TOOLS"
    };

    const char *details[7] = {
        profile_value,
        resume_value,
        view_value,
        subtitle_value,
        show_debug_overlay ? "ON" : "OFF",
        uninstall_value,
        "LOGS AND DEBUG REPORTS"
    };

    /*
     * Normal rows first.
     */
    for (int row = 0; row < settings_count; row++) {
        int y = row_start_y + row * row_gap;

        rr_img(
            fb,
            row_x,
            y,
            RR_CARD_NORMAL_W,
            RR_CARD_NORMAL_H,
            RR_CARD_NORMAL
        );
    }

    /*
     * Gliding selected row.
     */
    rr_img(
        fb,
        row_x,
        selected_draw_y,
        RR_CARD_SELECTED_W,
        RR_CARD_SELECTED_H,
        RR_CARD_SELECTED
    );

    /*
     * Row content.
     */
    for (int row = 0; row < settings_count; row++) {
        int y = row_start_y + row * row_gap;

        rr_icon(
            fb,
            row_x + 52,
            y + 21,
            row == 0 ? 3 :
            row == 4 ? 4 :
            row == 5 ? 5 : 5
        );

        rr_text(
            fb,
            row_x + 150,
            y + 25,
            titles[row],
            RR_BGRA(232, 244, 255, 238),
            2
        );

        rr_text(
            fb,
            row_x + 152,
            y + 70,
            details[row],
            row == settings_selected
                ? RR_BGRA(205,205,205,235)
                : RR_BGRA(204,204,204,220),
            1
        );

        int arrow_shift = 0;

        if (row == settings_selected) {
            int phase = settings_frame % 36;

            if (phase < 9) {
                arrow_shift = phase / 3;
            } else if (phase < 18) {
                arrow_shift = 3 - ((phase - 9) / 3);
            }
        }

        rr_icon(
            fb,
            row_x + 1138 + arrow_shift,
            y + 21,
            6
        );
    }

    /*
     * Minimal footer.
     */
    rr_fill(
        fb,
        0,
        948,
        1920,
        1,
        RR_BGRA(186,186,186,48)
    );

    rr_fill(
        fb,
        0,
        949,
        1920,
        79,
        RR_BGRA(12,12,12,138)
    );

    rr_control(
        fb,
        92,
        982,
        0
    );

    rr_text(
        fb,
        148,
        998,
        "SELECT",
        RR_BGRA(209,209,209,205),
        0
    );

    rr_control(
        fb,
        292,
        982,
        4
    );

    rr_text(
        fb,
        348,
        998,
        "BACK",
        RR_BGRA(209,209,209,205),
        0
    );

    rr_icon(
        fb,
        1512,
        965,
        3
    );

    rr_text(
        fb,
        1585,
        997,
        "PROSPERO SETTINGS",
        RR_BGRA(203,203,203,205),
        0
    );
}

void draw_profile_screen(uint32_t *fb) {
    static int initialized = 0;
    static int selected_y_fp = 0;
    static int profile_frame = 0;

    const int row_x = 180;
    const int row_start_y = 286;
    const int row_gap = 142;

    if (profile_selected < 0) {
        profile_selected = 0;
    }

    if (profile_selected > 3) {
        profile_selected = 3;
    }

    int target_y_fp =
        (row_start_y + profile_selected * row_gap) << 8;

    if (!initialized) {
        selected_y_fp = target_y_fp;
        initialized = 1;
    }

    int distance = target_y_fp - selected_y_fp;

    if (distance > -160 && distance < 160) {
        selected_y_fp = target_y_fp;
    } else {
        selected_y_fp += distance / 2;
    }

    int selected_draw_y = selected_y_fp >> 8;

    profile_frame++;

    rr_bg(fb);


    /*
     * Header.
     */
    rr_fill(
        fb,
        82,
        74,
        5,
        76,
        RR_BGRA(205,205,205,225)
    );

    rr_fill(
        fb,
        82,
        146,
        31,
        2,
        RR_BGRA(205,205,205,195)
    );

    rr_text(
        fb,
        122,
        70,
        "PLAYBACK PROFILE",
        RR_BGRA(232, 244, 255, 240),
        3
    );

    rr_text(
        fb,
        124,
        132,
        "CHOOSE HOW PS PLAY HANDLES MEDIA",
        RR_BGRA(199,199,199,225),
        1
    );

    const char *names[4] = {
        "BALANCED",
        "PERFORMANCE",
        "COMPATIBILITY",
        "DEBUG"
    };

    const char *descriptions[4] = {
        "Auto tune from file type (recommended)",
        "Faster decode: more threads, loop-filter skip",
        "Safer decode: full filters, deeper queues, no FAST",
        "Technical toasts and decoder diagnostics"
    };

    /*
     * Draw normal cards.
     */
    for (int row = 0; row < 4; row++) {
        int y = row_start_y + row * row_gap;

        rr_img(
            fb,
            row_x,
            y,
            RR_CARD_NORMAL_W,
            RR_CARD_NORMAL_H,
            RR_CARD_NORMAL
        );
    }

    /*
     * Draw animated selected card.
     */
    rr_img(
        fb,
        row_x,
        selected_draw_y,
        RR_CARD_SELECTED_W,
        RR_CARD_SELECTED_H,
        RR_CARD_SELECTED
    );

    for (int row = 0; row < 4; row++) {
        int y = row_start_y + row * row_gap;

        int icon_index =
            row == 0 ? 3 :
            row == 1 ? 1 :
            row == 2 ? 5 :
            4;

        rr_icon(
            fb,
            row_x + 52,
            y + 21,
            icon_index
        );

        rr_text(
            fb,
            row_x + 150,
            y + 24,
            names[row],
            RR_BGRA(232, 244, 255, 238),
            2
        );

        rr_text(
            fb,
            row_x + 152,
            y + 69,
            descriptions[row],
            row == profile_selected
                ? RR_BGRA(205,205,205,235)
                : RR_BGRA(204,204,204,220),
            1
        );

        int arrow_shift = 0;

        if (row == profile_selected) {
            int phase = profile_frame % 36;

            if (phase < 9) {
                arrow_shift = phase / 3;
            } else if (phase < 18) {
                arrow_shift =
                    3 - ((phase - 9) / 3);
            }
        }

        rr_icon(
            fb,
            row_x + 1138 + arrow_shift,
            y + 21,
            6
        );
    }

    /*
     * Current profile indicator.
     */
    char active_profile[96];

    snprintf(
        active_profile,
        sizeof(active_profile),
        "CURRENT  %s",
        profile_name(current_profile)
    );

    rr_text(
        fb,
        1380,
        194,
        active_profile,
        RR_BGRA(203,203,203,210),
        0
    );

    /*
     * Footer.
     */
    rr_fill(
        fb,
        0,
        948,
        1920,
        1,
        RR_BGRA(186,186,186,48)
    );

    rr_fill(
        fb,
        0,
        949,
        1920,
        79,
        RR_BGRA(12,12,12,138)
    );

    rr_control(
        fb,
        92,
        982,
        0
    );

    rr_text(
        fb,
        148,
        998,
        "APPLY",
        RR_BGRA(209,209,209,205),
        0
    );

    rr_control(
        fb,
        292,
        982,
        4
    );

    rr_text(
        fb,
        348,
        998,
        "BACK",
        RR_BGRA(209,209,209,205),
        0
    );

    rr_icon(
        fb,
        1512,
        965,
        3
    );

    rr_text(
        fb,
        1585,
        997,
        "PLAYBACK PROFILE",
        RR_BGRA(203,203,203,205),
        0
    );
}







/* RENDERER_RESET_UI_START */

static inline uint32_t rr_blend(uint32_t dst,uint32_t src){
    unsigned a=(src>>24)&255;
    if(a==255) return src;
    if(a==0) return dst;
    unsigned sr=src&255, sg=(src>>8)&255, sb=(src>>16)&255;
    unsigned dr=dst&255, dg=(dst>>8)&255, db=(dst>>16)&255;
    unsigned r=(sr*a + dr*(255-a))/255;
    unsigned g=(sg*a + dg*(255-a))/255;
    unsigned b=(sb*a + db*(255-a))/255;
    return 0xFF000000|(b<<16)|(g<<8)|r;
}
static void rr_img(uint32_t *fb,int x,int y,int w,int h,const unsigned int *img){
    for(int yy=0;yy<h;yy++){int fy=y+yy;if((unsigned)fy>=1080)continue;
    for(int xx=0;xx<w;xx++){int fx=x+xx;if((unsigned)fx>=1920)continue;
    fb[fy*1920+fx]=rr_blend(fb[fy*1920+fx],img[yy*w+xx]);}}
}
/* PS Play 1.5: AMOLED pure-black theme (no floating shapes). */
static void rr_bg(uint32_t *fb){uint32_t *p=fb,*e=fb+1920*1080;while(p<e)*p++=0xFF000000u;}
static void rr_fill(uint32_t *fb,int x,int y,int w,int h,uint32_t c){for(int yy=0;yy<h;yy++)for(int xx=0;xx<w;xx++){int fx=x+xx,fy=y+yy;if((unsigned)fx<1920&&(unsigned)fy<1080)fb[fy*1920+fx]=rr_blend(fb[fy*1920+fx],c);}}
static int rr_idx(char ch){for(int i=0;i<RR_COUNT;i++)if(RR_CHARS[i]==ch)return i;return -1;}
static void rr_text(uint32_t *fb,int x,int y,const char *t,uint32_t color,int face){
    int cx=x; unsigned ca=(color>>24)&255;
    for(const char*q=t;*q;q++){int id=rr_idx(*q);if(id<0){cx+=12;continue;}
    int sx,sy,sw,sh,adv;
    if(face==3){sx=RR_TITLE_X[id];sy=RR_TITLE_Y[id];sw=RR_TITLE_W[id];sh=RR_TITLE_H[id];adv=RR_TITLE_ADV[id];}
    else if(face==2){sx=RR_MENU_X[id];sy=RR_MENU_Y[id];sw=RR_MENU_W[id];sh=RR_MENU_H[id];adv=RR_MENU_ADV[id];}
    else if(face==1){sx=RR_SUB_X[id];sy=RR_SUB_Y[id];sw=RR_SUB_W[id];sh=RR_SUB_H[id];adv=RR_SUB_ADV[id];}
    else{sx=RR_SMALL_X[id];sy=RR_SMALL_Y[id];sw=RR_SMALL_W[id];sh=RR_SMALL_H[id];adv=RR_SMALL_ADV[id];}
    for(int yy=0;yy<sh;yy++){int fy=y+yy;if((unsigned)fy>=1080)continue;
    for(int xx=0;xx<sw;xx++){int fx=cx+xx;if((unsigned)fx>=1920)continue;
    unsigned ma=RR_FONT[(sy+yy)*RR_FONT_W+(sx+xx)];if(ma>18){unsigned a=(ma*ca)/255;uint32_t src=(a<<24)|(color&0x00FFFFFF);fb[fy*1920+fx]=rr_blend(fb[fy*1920+fx],src);}}}
    cx+=adv;}}

/* Rounded-rectangle fill for the PS Play tile UI (integer corners). */
static int rr_isqrt(int v){int r=0;while((r+1)*(r+1)<=v)r++;return r;}
static void rr_fill_round(uint32_t *fb,int x,int y,int w,int h,int r,uint32_t c){
    if(r<0)r=0; if(r>40)r=40;
    int tab[41];
    for(int i=0;i<=r;i++){int dy=i;int v=r-rr_isqrt(r*r-dy*dy);tab[i]=v<0?0:v;}
    for(int yy=0;yy<h;yy++){
        int inset=0;
        if(yy<r) inset=tab[r-yy];
        else if(yy>=h-r) inset=tab[yy-(h-r)+1];
        rr_fill(fb,x+inset,y+yy,w-2*inset,1,c);
    }
}

/* Exact pixel width of a string in a given face (same metrics as rr_text). */
static int rr_text_width(const char *t,int face){
    int w=0;
    for(const char*q=t;*q;q++){
        int id=rr_idx(*q);
        if(id<0){w+=12;continue;}
        if(face==3)w+=RR_TITLE_ADV[id];
        else if(face==2)w+=RR_MENU_ADV[id];
        else if(face==1)w+=RR_SUB_ADV[id];
        else w+=RR_SMALL_ADV[id];
    }
    return w;
}

static void rr_text_center(uint32_t *fb,int cx,int y,const char *t,uint32_t color,int face){
    rr_text(fb,cx-rr_text_width(t,face)/2,y,t,color,face);
}
static void rr_icon(uint32_t *fb,int x,int y,int idx){
    switch(idx){case 0:rr_img(fb,x,y,RR_ICON_BROWSE_USB_W,RR_ICON_BROWSE_USB_H,RR_ICON_BROWSE_USB);break;case 1:rr_img(fb,x,y,RR_ICON_RECENT_FILES_W,RR_ICON_RECENT_FILES_H,RR_ICON_RECENT_FILES);break;case 2:rr_img(fb,x,y,RR_ICON_FAVORITES_W,RR_ICON_FAVORITES_H,RR_ICON_FAVORITES);break;case 3:rr_img(fb,x,y,RR_ICON_SETTINGS_W,RR_ICON_SETTINGS_H,RR_ICON_SETTINGS);break;case 4:rr_img(fb,x,y,RR_ICON_DEVELOPER_TOOLS_W,RR_ICON_DEVELOPER_TOOLS_H,RR_ICON_DEVELOPER_TOOLS);break;case 5:rr_img(fb,x,y,RR_ICON_ABOUT_SUPPORT_W,RR_ICON_ABOUT_SUPPORT_H,RR_ICON_ABOUT_SUPPORT);break;case 6:rr_img(fb,x,y,RR_ICON_CHEVRON_W,RR_ICON_CHEVRON_H,RR_ICON_CHEVRON);break;case 7:rr_img(fb,x,y,RR_ICON_DLNA_W,RR_ICON_DLNA_H,RR_ICON_DLNA);break;case 8:rr_img(fb,x,y,RR_ICON_IPTV_W,RR_ICON_IPTV_H,RR_ICON_IPTV);break;}}
static void rr_control(
    uint32_t *fb,
    int x,
    int y,
    int idx
) {
    switch (idx) {
        case 0:
            rr_img(
                fb, x, y,
                RR_CONTROL_X_W,
                RR_CONTROL_X_H,
                RR_CONTROL_X
            );
            break;

        case 1:
            rr_img(
                fb, x, y,
                RR_CONTROL_DPAD_W,
                RR_CONTROL_DPAD_H,
                RR_CONTROL_DPAD
            );
            break;

        case 2:
            rr_img(
                fb, x, y,
                RR_CONTROL_LEFT_STICK_W,
                RR_CONTROL_LEFT_STICK_H,
                RR_CONTROL_LEFT_STICK
            );
            break;

        case 3:
            rr_img(
                fb, x, y,
                RR_CONTROL_RIGHT_STICK_W,
                RR_CONTROL_RIGHT_STICK_H,
                RR_CONTROL_RIGHT_STICK
            );
            break;

        case 4:
            rr_img(
                fb, x, y,
                RR_CONTROL_CIRCLE_W,
                RR_CONTROL_CIRCLE_H,
                RR_CONTROL_CIRCLE
            );
            break;

        case 5:
            rr_img(
                fb, x, y,
                RR_CONTROL_TRIANGLE_W,
                RR_CONTROL_TRIANGLE_H,
                RR_CONTROL_TRIANGLE
            );
            break;

        case 6:
            rr_img(
                fb, x, y,
                RR_CONTROL_SQUARE_W,
                RR_CONTROL_SQUARE_H,
                RR_CONTROL_SQUARE
            );
            break;

        default:
            break;
    }
}

void draw_menu_linear(uint32_t *fb, int selected) {
    /*
     * PS Play 1.5 menu order: receiver first, favourites removed,
     * dev tools moved inside Settings.
     */
    const char *titles[7] = {
        "DLNA RECEIVER",
        "DLNA / UPNP",
        "IPTV / LIVE TV",
        "BROWSE USB",
        "RECENT FILES",
        "SETTINGS",
        "ABOUT"
    };

    const char *subs[7] = {
        "Cast to this console like a Chromecast",
        "Stream from media servers on your network",
        "Live channels from M3U playlists and URLs",
        "Explore videos and folders on USB storage",
        "Continue videos you recently played",
        "Profiles playback preferences and dev tools",
        "Credits project info and support"
    };

    /* Menu entry -> rr_icon index (7 = DLNA cast, 8 = IPTV television). */
    static const int menu_icon_map[7] = { 7, 7, 8, 0, 1, 3, 5 };

    static int initialized = 0;
    static int selected_x_fp = 0;
    static int intro_frame = 0;
    static int fade_alpha = 255;
    static int animation_frame = 0;
    static int last_selected = -1;

    /*
     * PS Play horizontal icon row: 7 rounded tiles in a single row,
     * white icons, label under each tile, white glow on the selection.
     */
    const int tw = 150;          /* tile width/height */
    const int th = 150;
    const int gap = 26;
    const int row_w = 7 * tw + 6 * gap;
    const int x0 = (1920 - row_w) / 2;
    const int ty = 400;

    if (selected < 0) selected = 0;
    if (selected > 6) selected = 6;

    int target_x_fp = (x0 + selected * (tw + gap)) << 8;

    if (!initialized) {
        selected_x_fp = target_x_fp;
        last_selected = selected;
        initialized = 1;
    }

    if (selected != last_selected) {
        last_selected = selected;
    }

    int distance = target_x_fp - selected_x_fp;

    if (distance > -160 && distance < 160) {
        selected_x_fp = target_x_fp;
    } else {
        selected_x_fp += distance / 2;
    }

    int selected_draw_x = selected_x_fp >> 8;

    intro_frame++;
    animation_frame++;

    if (intro_frame > 28) {
        intro_frame = 28;
    }

    if (fade_alpha > 0) {
        fade_alpha -= 42;
        if (fade_alpha < 0) fade_alpha = 0;
    }

    /* AMOLED pure black background — no shapes, no particles. */
    rr_bg(fb);

    /* Header */
    rr_fill(fb,82,90,5,66,RR_BGRA(255,255,255,225));
    rr_fill(fb,82,152,29,2,RR_BGRA(255,255,255,195));

    rr_text(
        fb,
        122,86,
        "PS PLAY",
        RR_BGRA(240,244,248,238),
        3
    );

    rr_text(
        fb,
        124,137,
        "VERSION 1.5",
        RR_BGRA(150,155,162,225),
        1
    );

    /*
     * Tiles with staggered intro rise.
     */
    for (int i = 0; i < 7; i++) {
        int local_frame = intro_frame - i;
        int rise;

        if (local_frame <= 0) {
            rise = 21;
        } else if (local_frame < 7) {
            rise = 21 - local_frame * 3;
        } else {
            rise = 0;
        }

        int tile_x = x0 + i * (tw + gap);
        int tile_y = ty + rise;

        /* Tile body: faint rim + dark rounded square. */
        rr_fill_round(fb, tile_x-1, tile_y-1, tw+2, th+2, 25, RR_BGRA(255,255,255,22));
        rr_fill_round(fb, tile_x, tile_y, tw, th, 24, RR_BGRA(16,17,20,190));

        rr_icon(
            fb,
            tile_x + (tw - 72) / 2,
            tile_y + 26,
            menu_icon_map[i]
        );

        uint32_t label_col = (i == selected)
            ? RR_BGRA(255,255,255,235)
            : RR_BGRA(120,124,130,215);

        rr_text_center(
            fb,
            tile_x + tw / 2,
            ty + th + 20,
            titles[i],
            label_col,
            0
        );
    }

    /*
     * Selection glow glides horizontally between tiles:
     * soft halo + crisp white border above the tile body.
     */
    {
        int gx = selected_draw_x;
        int gy = ty;
        rr_fill_round(fb, gx-7, gy-7, tw+14, th+14, 30, RR_BGRA(255,255,255,26));
        rr_fill_round(fb, gx-4, gy-4, tw+8, th+8, 27, RR_BGRA(255,255,255,60));
        /* crisp 2px white ring */
        rr_fill_round(fb, gx-2, gy-2, tw+4, th+4, 25, RR_BGRA(255,255,255,225));
        rr_fill_round(fb, gx, gy, tw, th, 24, RR_BGRA(20,21,25,235));
        rr_icon(
            fb,
            gx + (tw - 72) / 2,
            gy + 26,
            menu_icon_map[selected]
        );
        /* re-stamp the selected label in bright white over the glow */
        rr_text_center(
            fb,
            gx + tw / 2,
            ty + th + 20,
            titles[selected],
            RR_BGRA(255,255,255,245),
            0
        );
    }

    /* Description of the highlighted entry, centered under the row. */
    rr_text_center(
        fb,
        960,
        ty + th + 72,
        subs[selected],
        RR_BGRA(168,172,178,220),
        1
    );

    /* Minimal footer */
    rr_fill(fb,0,948,1920,1,RR_BGRA(255,255,255,42));
    rr_fill(fb,0,949,1920,79,RR_BGRA(0,0,0,150));

    rr_control(fb,92,982,0);

    rr_text(
        fb,
        148,998,
        "SELECT",
        RR_BGRA(185,190,196,205),
        0
    );

    rr_icon(fb,1480,965,5);

    rr_text(
        fb,
        1560,997,
        "PS PLAY V1.5",
        RR_BGRA(150,155,162,205),
        0
    );

    /* Faster initial fade-in. */
    if (fade_alpha > 0) {
        rr_fill(
            fb,
            0,0,1920,1080,
            RR_BGRA(0,0,0,fade_alpha)
        );
    }
}


/* PROSPERO_EXACT_MENU_RENDER_END */

/* ==========================================================================
 * NETWORK STREAMING — DLNA / UPNP SERVERS + IPTV / LIVE TV
 *
 * Added in ProsperoPlayer 1.1. DLNA stack (SSDP + SOAP ContentDirectory)
 * lives in net/pp_dlna.c, M3U playlist handling in net/pp_iptv.c.
 * Discovery and browsing run on worker threads so the UI never blocks.
 * ========================================================================== */

#define PP_NET_MAX_SERVERS 16
#define PP_NET_MAX_ITEMS   512
#define PP_NET_MAX_IPTV    600
#define PP_NET_MAX_DEPTH   24

static pthread_mutex_t g_dlna_mtx = PTHREAD_MUTEX_INITIALIZER;

static void pp_boot_log(const char *step); /* defined near main() */

/* 0 idle, 1 working, 2 done, 3 error */
static volatile int g_dlna_status = 0;
static char g_dlna_msg[192] = {0};

static pp_dlna_server g_dlna_servers[PP_NET_MAX_SERVERS];
static int g_dlna_server_count = 0;
static int g_dlna_server_sel = 0;
static int g_dlna_server_scrl = 0;

static pp_dlna_object g_dlna_items[PP_NET_MAX_ITEMS];
static int g_dlna_item_count = 0;
static int g_dlna_item_sel = 0;
static int g_dlna_item_scrl = 0;

static int g_dlna_cur_server = -1;
static char g_dlna_id_stack[PP_NET_MAX_DEPTH][PP_DLNA_MAX_ID];
static char g_dlna_title_stack[PP_NET_MAX_DEPTH][256];
static int g_dlna_depth = 0;

/* Single in-flight browse job (results copied under mutex). */
static pp_dlna_server g_browse_job_server;
static char g_browse_job_id[PP_DLNA_MAX_ID];

static pp_iptv_channel g_iptv[PP_NET_MAX_IPTV];
static int g_iptv_count = 0;
static int g_iptv_sel = 0;
static int g_iptv_scrl = 0;
static int g_iptv_loaded = 0;

/* On-screen keyboard for custom stream URLs. */
static char g_osk_buf[PP_IPTV_MAX_URL];
static int g_osk_len = 0;
static int g_osk_row = 0;
static int g_osk_col = 0;

static void *dlna_discover_worker(void *unused) {
    (void)unused;
    pp_dlna_server tmp[PP_NET_MAX_SERVERS];
    char err[160] = {0};

    int n = pp_dlna_discover(tmp, PP_NET_MAX_SERVERS, 5000, err, sizeof(err));

    pthread_mutex_lock(&g_dlna_mtx);
    if (n < 0) {
        g_dlna_status = 3;
        snprintf(g_dlna_msg, sizeof(g_dlna_msg), "%s",
                 err[0] ? err : "network search failed");
    } else {
        memcpy(g_dlna_servers, tmp, sizeof(tmp));
        g_dlna_server_count = n;
        g_dlna_status = 2;
        if (n == 0) {
            snprintf(g_dlna_msg, sizeof(g_dlna_msg), "%s",
                     err[0] ? err : "no DLNA server answered");
        } else {
            snprintf(g_dlna_msg, sizeof(g_dlna_msg),
                     "%d SERVER%s FOUND", n, n == 1 ? "" : "S");
        }
    }
    pthread_mutex_unlock(&g_dlna_mtx);
    {
        char lb[256];
        snprintf(lb, sizeof(lb), "dlna discover finished: n=%d msg=%s",
                 n, err[0] ? err : "ok");
        pp_boot_log(lb);
    }
    return NULL;
}

static void dlna_start_discover(void) {
    pthread_mutex_lock(&g_dlna_mtx);
    if (g_dlna_status == 1) {
        pthread_mutex_unlock(&g_dlna_mtx);
        return;
    }
    g_dlna_status = 1;
    g_dlna_server_count = 0;
    snprintf(g_dlna_msg, sizeof(g_dlna_msg), "SEARCHING YOUR NETWORK...");
    pthread_mutex_unlock(&g_dlna_mtx);

    pthread_t t;
    if (pthread_create(&t, NULL, dlna_discover_worker, NULL) == 0) {
        pthread_detach(t);
    } else {
        g_dlna_status = 3;
        snprintf(g_dlna_msg, sizeof(g_dlna_msg), "cannot start scan thread");
    }
}

static void *dlna_browse_worker(void *unused) {
    (void)unused;
    pp_dlna_object *tmp = malloc(sizeof(pp_dlna_object) * PP_NET_MAX_ITEMS);
    char err[160] = {0};

    if (!tmp) {
        pthread_mutex_lock(&g_dlna_mtx);
        g_dlna_status = 3;
        snprintf(g_dlna_msg, sizeof(g_dlna_msg), "out of memory");
        pthread_mutex_unlock(&g_dlna_mtx);
        return NULL;
    }

    int n = pp_dlna_browse(&g_browse_job_server, g_browse_job_id,
                           tmp, PP_NET_MAX_ITEMS, err, sizeof(err));

    pthread_mutex_lock(&g_dlna_mtx);
    if (n < 0) {
        g_dlna_status = 3;
        snprintf(g_dlna_msg, sizeof(g_dlna_msg), "%s",
                 err[0] ? err : "browse failed");
        /* pop the folder we failed to enter */
        if (g_dlna_depth > 0) g_dlna_depth--;
    } else {
        memcpy(g_dlna_items, tmp, sizeof(pp_dlna_object) * (size_t)n);
        g_dlna_item_count = n;
        g_dlna_item_sel = 0;
        g_dlna_item_scrl = 0;
        g_dlna_status = 2;
        if (n == 0) {
            snprintf(g_dlna_msg, sizeof(g_dlna_msg), "FOLDER IS EMPTY");
        }
    }
    pthread_mutex_unlock(&g_dlna_mtx);
    free(tmp);
    return NULL;
}

static void dlna_start_browse(int server_idx, const char *object_id) {
    pthread_mutex_lock(&g_dlna_mtx);
    if (g_dlna_status == 1) {
        pthread_mutex_unlock(&g_dlna_mtx);
        return;
    }
    memcpy(&g_browse_job_server, &g_dlna_servers[server_idx],
           sizeof(pp_dlna_server));
    snprintf(g_browse_job_id, sizeof(g_browse_job_id), "%s", object_id);
    g_dlna_status = 1;
    snprintf(g_dlna_msg, sizeof(g_dlna_msg), "LOADING FOLDER...");
    pthread_mutex_unlock(&g_dlna_mtx);

    pthread_t t;
    if (pthread_create(&t, NULL, dlna_browse_worker, NULL) == 0) {
        pthread_detach(t);
    } else {
        g_dlna_status = 3;
        snprintf(g_dlna_msg, sizeof(g_dlna_msg), "cannot start browse thread");
    }
}

static void prospero_net_enter_dlna(void) {
    g_dlna_server_sel = 0;
    g_dlna_server_scrl = 0;
    g_dlna_depth = 0;
    g_dlna_cur_server = -1;
    dlna_start_discover();
}

static void prospero_net_enter_iptv(void) {
    g_iptv_sel = 0;
    g_iptv_scrl = 0;
    if (!g_iptv_loaded) {
        g_iptv_count = pp_iptv_load(g_iptv, PP_NET_MAX_IPTV);
        g_iptv_loaded = 1;
    }
}

static void dlna_open_server(int idx) {
    if (idx < 0 || idx >= g_dlna_server_count) return;
    g_dlna_cur_server = idx;
    g_dlna_depth = 0;
    snprintf(g_dlna_id_stack[0], sizeof(g_dlna_id_stack[0]), "0");
    snprintf(g_dlna_title_stack[0], sizeof(g_dlna_title_stack[0]), "%s",
             g_dlna_servers[idx].friendly_name);
    g_dlna_depth = 1;
    dlna_start_browse(idx, "0");
    screen = SCREEN_DLNA_BROWSE;
}

static void dlna_open_container(const pp_dlna_object *o) {
    if (g_dlna_depth >= PP_NET_MAX_DEPTH) {
        toast("DLNA", "Folder depth limit reached");
        return;
    }
    snprintf(g_dlna_id_stack[g_dlna_depth], sizeof(g_dlna_id_stack[0]),
             "%s", o->id);
    snprintf(g_dlna_title_stack[g_dlna_depth], sizeof(g_dlna_title_stack[0]),
             "%s", o->title);
    g_dlna_depth++;
    dlna_start_browse(g_dlna_cur_server, o->id);
}

static void dlna_go_back(void) {
    if (g_dlna_depth <= 1) {
        screen = SCREEN_DLNA_SERVERS;
        return;
    }
    g_dlna_depth--;
    dlna_start_browse(g_dlna_cur_server, g_dlna_id_stack[g_dlna_depth - 1]);
}

static void dlna_refresh_folder(void) {
    if (g_dlna_cur_server < 0 || g_dlna_depth < 1) return;
    dlna_start_browse(g_dlna_cur_server, g_dlna_id_stack[g_dlna_depth - 1]);
}

static void prospero_network_play(const char *url, const char *title) {
    if (!url || !url[0]) {
        toast("STREAM ERROR", "Empty stream address");
        return;
    }
    pp_boot_log("network play requested");
    pp_boot_log(url);
    snprintf(current_media, sizeof(current_media), "%s",
             (title && title[0]) ? title : "NETWORK STREAM");
    snprintf(current_media_path, sizeof(current_media_path), "%s", url);
    toast("NETWORK STREAM", current_media);
    screen = SCREEN_PLAYER;
    start_video_playback(url);
}

/* ------------------------------------------------------------------ */
/* DLNA RENDERER (cast target) — command bridge                        */
/*                                                                     */
/* SOAP actions arrive on pp_dmr threads; this pump runs on the main   */
/* loop and applies them to the real player, then reports the actual   */
/* state back so BubbleUPnP & co. see true transport state.            */
/* ------------------------------------------------------------------ */

static double prospero_player_position(void); /* defined with the scrub code */

static int g_dmr_started = 0;

static void prospero_dmr_pump(void) {
    if (!g_dmr_started) return;

    pp_dmr_cmd c;
    while (pp_dmr_take_command(&c)) {
        switch (c.type) {
        case PP_DMR_CMD_PLAY_URI:
            pp_boot_log("dmr: play uri");
            pp_boot_log(c.url);
            if (g_pp_pb.active || screen == SCREEN_PLAYER)
                stop_video_playback();
            snprintf(current_media, sizeof(current_media), "%s",
                     c.title[0] ? c.title : "DLNA CAST");
            snprintf(current_media_path, sizeof(current_media_path), "%s", c.url);
            toast("DLNA CAST", current_media);
            screen = SCREEN_PLAYER;
            start_video_playback(c.url);
            pp_dmr_report_playing();
            break;

        case PP_DMR_CMD_STOP:
            pp_boot_log("dmr: stop");
            if (g_pp_pb.active || screen == SCREEN_PLAYER) {
                stop_video_playback();
                if (screen == SCREEN_PLAYER)
                    screen = SCREEN_MAIN_MENU;
            }
            pp_dmr_report_stopped();
            break;

        case PP_DMR_CMD_PAUSE:
            if (!player_paused && g_pp_pb.active) {
                player_paused = 1;
                pp_playback_pause(&g_pp_pb);
            }
            pp_dmr_report_paused();
            break;

        case PP_DMR_CMD_RESUME:
            if (player_paused) {
                player_paused = 0;
                pp_playback_resume(&g_pp_pb);
            }
            pp_dmr_report_playing();
            break;

        case PP_DMR_CMD_SEEK:
            if (g_pp_pb.active) {
                if (prospero_request_inplace_seek(c.seek_s, player_paused)) {
                    pp_dmr_report_position(c.seek_s);
                } else {
                    toast("DLNA CAST", "Seek failed");
                }
            }
            break;

        case PP_DMR_CMD_VOLUME:
            /* volume state tracked in pp_dmr; no global mixer hook yet */
            break;

        default:
            break;
        }
    }

    /* Keep the renderer state honest with what the pad did. */
    pp_dmr_status st;
    pp_dmr_get_status(&st);
    if ((st.transport == PP_DMR_TRANSPORT_PLAYING ||
         st.transport == PP_DMR_TRANSPORT_PAUSED) &&
        !g_pp_pb.active && screen != SCREEN_PLAYER) {
        pp_dmr_report_stopped();
    }
    if (g_pp_pb.active &&
        (st.transport == PP_DMR_TRANSPORT_PLAYING ||
         st.transport == PP_DMR_TRANSPORT_PAUSED ||
         st.transport == PP_DMR_TRANSPORT_TRANSITIONING)) {
        pp_dmr_report_position(prospero_player_position());
    }
}

/* ------------------------------------------------------------------ */
/* Shared list renderer (same visual language as the USB browser)     */
/* ------------------------------------------------------------------ */

#define PP_NET_LIST_VISIBLE 5
#define PP_NET_ROW_X        180
#define PP_NET_ROW_START_Y  252
#define PP_NET_ROW_GAP      126

typedef struct {
    char title[256];
    char detail[256];
    int icon; /* 0 folder 1 video 2 audio 3 image 4 doc 5 server 6 tv 7 link */
} pp_net_list_row;

static void pp_net_draw_header(uint32_t *fb, const char *title,
                               const char *subtitle, int frame) {
    rr_bg(fb);


    rr_fill(fb, 82, 74, 5, 76, RR_BGRA(205,205,205,225));
    rr_fill(fb, 82, 146, 31, 2, RR_BGRA(205,205,205,195));

    rr_text(fb, 122, 70, title, RR_BGRA(232, 244, 255, 240), 3);
    rr_text(fb, 124, 132, subtitle, RR_BGRA(199,199,199,225), 1);
}

static void pp_net_draw_status_line(uint32_t *fb, const char *text) {
    if (text && text[0]) {
        rr_text(fb, 1550, 86, text, RR_BGRA(203,203,203,210), 0);
    }
}

static void pp_net_draw_footer3(uint32_t *fb,
                                const char *hint_x,
                                const char *hint_square,
                                const char *hint_circle) {
    rr_fill(fb, 0, 948, 1920, 1, RR_BGRA(186,186,186,48));
    rr_fill(fb, 0, 949, 1920, 79, RR_BGRA(12,12,12,138));

    int x = 92;
    rr_control(fb, x, 982, 0);
    rr_text(fb, x + 56, 998, hint_x, RR_BGRA(209,209,209,205), 0);
    x += 56 + (int)strlen(hint_x) * 12 + 60;

    if (hint_square) {
        rr_control(fb, x, 982, 6);
        rr_text(fb, x + 56, 998, hint_square, RR_BGRA(209,209,209,205), 0);
        x += 56 + (int)strlen(hint_square) * 12 + 60;
    }

    rr_control(fb, x, 982, 4);
    rr_text(fb, x + 56, 998, hint_circle, RR_BGRA(209,209,209,205), 0);
}

/*
 * Draw a card list; handles its own scroll window + gliding selection.
 * Returns nothing; updates *scroll_io.
 */
static void pp_net_draw_rows(uint32_t *fb, pp_net_list_row *rows, int count,
                             int selected, int *scroll_io, int frame) {
    const int visible = PP_NET_LIST_VISIBLE;

    if (selected < 0) selected = 0;
    if (selected >= count) selected = count - 1;

    int scroll = *scroll_io;
    if (selected < scroll) scroll = selected;
    if (selected >= scroll + visible) scroll = selected - visible + 1;
    if (scroll < 0) scroll = 0;
    *scroll_io = scroll;

    int visible_selected = selected - scroll;
    if (visible_selected < 0) visible_selected = 0;
    if (visible_selected >= visible) visible_selected = visible - 1;

    /* Gliding selected row (fixed-point like the other browsers). */
    static int sel_y_fp = 0;
    int target_y_fp =
        (PP_NET_ROW_START_Y + visible_selected * PP_NET_ROW_GAP) << 8;
    int dist = target_y_fp - sel_y_fp;
    if (dist > -160 && dist < 160) sel_y_fp = target_y_fp;
    else sel_y_fp += dist / 2;
    int sel_draw_y = sel_y_fp >> 8;

    /* Base cards. */
    for (int row = 0; row < visible; row++) {
        int index = scroll + row;
        if (index >= count) break;
        rr_img(fb, PP_NET_ROW_X,
               PP_NET_ROW_START_Y + row * PP_NET_ROW_GAP,
               RR_CARD_NORMAL_W, RR_CARD_NORMAL_H, RR_CARD_NORMAL);
    }

    if (count > 0) {
        rr_img(fb, PP_NET_ROW_X, sel_draw_y,
               RR_CARD_SELECTED_W, RR_CARD_SELECTED_H, RR_CARD_SELECTED);
    }

    /* Icons and text. */
    for (int row = 0; row < visible; row++) {
        int index = scroll + row;
        if (index >= count) break;

        int x = PP_NET_ROW_X;
        int y = PP_NET_ROW_START_Y + row * PP_NET_ROW_GAP;

        if (rows[index].icon == 5) {
            rr_icon(fb, x + 48, y + 20, 7);      /* DLNA globe */
        } else if (rows[index].icon == 6) {
            rr_icon(fb, x + 48, y + 20, 8);      /* IPTV television */
        } else {
            rr_browser_icon(fb, x + 48, y + 20, rows[index].icon);
        }

        char disp_title[256];
        snprintf(disp_title, sizeof(disp_title), "%s", rows[index].title);
        if (strlen(disp_title) > 50) {
            disp_title[47] = '.';
            disp_title[48] = '.';
            disp_title[49] = '.';
            disp_title[50] = 0;
        }

        char disp_detail[256];
        snprintf(disp_detail, sizeof(disp_detail), "%s", rows[index].detail);
        if (strlen(disp_detail) > 68) {
            disp_detail[65] = '.';
            disp_detail[66] = '.';
            disp_detail[67] = '.';
            disp_detail[68] = 0;
        }

        rr_text(fb, x + 150, y + 25, disp_title,
                RR_BGRA(232, 244, 255, 238), 2);
        rr_text(fb, x + 152, y + 70, disp_detail,
                RR_BGRA(204,204,204,220), 1);

        int arrow_shift = 0;
        if (index == selected) {
            int phase = frame % 36;
            if (phase < 9) arrow_shift = phase / 3;
            else if (phase < 18) arrow_shift = 3 - ((phase - 9) / 3);
        }
        rr_icon(fb, x + 1138 + arrow_shift, y + 21, 6);
    }
}

static void pp_net_draw_working(uint32_t *fb, int frame, const char *what) {
    char dots[4] = "...";
    int nd = (frame / 20) % 4;
    dots[nd] = 0;

    char line[128];
    snprintf(line, sizeof(line), "%s%s", what, dots);
    rr_text(fb, 230, 330, line, RR_BGRA(232, 244, 255, 230), 2);

    /* scanning pulse bar */
    int bx = 230 + (frame * 9) % 900;
    rr_fill(fb, 230, 392, 920, 3, RR_BGRA(73,73,73,90));
    rr_fill(fb, bx, 390, 120, 6, RR_BGRA(205,205,205,190));
}

/* ------------------------------------------------------------------ */
/* DLNA screens                                                        */
/* ------------------------------------------------------------------ */

void draw_dlna_servers_screen(uint32_t *fb) {
    static int frame = 0;
    frame++;

    pp_net_draw_header(fb, "DLNA / UPNP",
                       "MEDIA SERVERS ON YOUR NETWORK", frame);

    int status;
    char msg[192];
    int count;
    pthread_mutex_lock(&g_dlna_mtx);
    status = g_dlna_status;
    count = g_dlna_server_count;
    snprintf(msg, sizeof(msg), "%s", g_dlna_msg);
    pthread_mutex_unlock(&g_dlna_mtx);

    pp_net_draw_status_line(fb, msg);

    if (status == 1) {
        pp_net_draw_working(fb, frame, "SEARCHING YOUR NETWORK");
    } else if (count <= 0) {
        rr_text(fb, 230, 330, "NO DLNA SERVERS FOUND",
                RR_BGRA(232, 244, 255, 230), 2);
        rr_text(fb, 232, 382,
                "ENABLE MEDIA SHARING ON YOUR PC / NAS, THEN PRESS SQUARE",
                RR_BGRA(204,204,204,215), 1);
    } else {
        static pp_net_list_row rows[PP_NET_MAX_SERVERS];
        pthread_mutex_lock(&g_dlna_mtx);
        for (int i = 0; i < count; i++) {
            snprintf(rows[i].title, sizeof(rows[i].title), "%s",
                     g_dlna_servers[i].friendly_name);
            if (g_dlna_servers[i].model[0]) {
                snprintf(rows[i].detail, sizeof(rows[i].detail),
                         "%s  -  %s", g_dlna_servers[i].model,
                         g_dlna_servers[i].host);
            } else {
                snprintf(rows[i].detail, sizeof(rows[i].detail),
                         "DLNA SERVER  -  %s", g_dlna_servers[i].host);
            }
            rows[i].icon = 5;
        }
        pthread_mutex_unlock(&g_dlna_mtx);

        pp_net_draw_rows(fb, rows, count, g_dlna_server_sel,
                         &g_dlna_server_scrl, frame);
    }

    pp_net_draw_footer3(fb, "OPEN", "RESCAN", "BACK");
}

void draw_dlna_browse_screen(uint32_t *fb) {
    static int frame = 0;
    frame++;

    /* Breadcrumb: server / folder / subfolder */
    char crumb[512] = {0};
    pthread_mutex_lock(&g_dlna_mtx);
    for (int i = 0; i < g_dlna_depth && i < PP_NET_MAX_DEPTH; i++) {
        if (i > 0) strncat(crumb, "  /  ", sizeof(crumb) - strlen(crumb) - 1);
        strncat(crumb, g_dlna_title_stack[i],
                sizeof(crumb) - strlen(crumb) - 1);
    }
    pthread_mutex_unlock(&g_dlna_mtx);
    for (char *c = crumb; *c; c++) {
        if (*c >= 'a' && *c <= 'z') *c -= 32;
    }
    if (strlen(crumb) > 60) {
        crumb[57] = '.';
        crumb[58] = '.';
        crumb[59] = '.';
        crumb[60] = 0;
    }

    pp_net_draw_header(fb, "DLNA BROWSER",
                       crumb[0] ? crumb : "SERVER CONTENTS", frame);

    int status, count;
    char msg[192];
    pthread_mutex_lock(&g_dlna_mtx);
    status = g_dlna_status;
    count = g_dlna_item_count;
    snprintf(msg, sizeof(msg), "%s", g_dlna_msg);
    pthread_mutex_unlock(&g_dlna_mtx);

    pp_net_draw_status_line(fb, msg);

    if (status == 1) {
        pp_net_draw_working(fb, frame, "LOADING FOLDER");
    } else if (count <= 0) {
        rr_text(fb, 230, 330,
                status == 3 ? "SERVER ERROR" : "THIS FOLDER IS EMPTY",
                RR_BGRA(232, 244, 255, 230), 2);
        rr_text(fb, 232, 382, msg,
                RR_BGRA(204,204,204,215), 1);
    } else {
        static pp_net_list_row rows[PP_NET_MAX_ITEMS];
        pthread_mutex_lock(&g_dlna_mtx);
        for (int i = 0; i < count; i++) {
            pp_dlna_object *o = &g_dlna_items[i];
            snprintf(rows[i].title, sizeof(rows[i].title), "%s", o->title);

            if (o->is_container) {
                if (o->child_count >= 0)
                    snprintf(rows[i].detail, sizeof(rows[i].detail),
                             "FOLDER  -  %d ITEM%s", o->child_count,
                             o->child_count == 1 ? "" : "S");
                else
                    snprintf(rows[i].detail, sizeof(rows[i].detail),
                             "OPEN FOLDER");
                rows[i].icon = 0;
            } else if (pp_dlna_object_is_video(o)) {
                char sz[48] = {0};
                if (o->size_bytes > 0)
                    format_file_size(sz, sizeof(sz), o->size_bytes);
                snprintf(rows[i].detail, sizeof(rows[i].detail),
                         "VIDEO%s%s%s%s%s%s",
                         o->resolution[0] ? "  -  " : "",
                         o->resolution,
                         o->duration[0] ? "  -  " : "",
                         o->duration,
                         sz[0] ? "  -  " : "", sz);
                rows[i].icon = 1;
            } else if (pp_dlna_object_is_audio(o)) {
                snprintf(rows[i].detail, sizeof(rows[i].detail),
                         "AUDIO%s%s%s%s",
                         o->artist[0] ? "  -  " : "", o->artist,
                         o->duration[0] ? "  -  " : "", o->duration);
                rows[i].icon = 2;
            } else if (pp_dlna_object_is_image(o)) {
                snprintf(rows[i].detail, sizeof(rows[i].detail), "IMAGE");
                rows[i].icon = 3;
            } else {
                snprintf(rows[i].detail, sizeof(rows[i].detail), "MEDIA FILE");
                rows[i].icon = 4;
            }
        }
        pthread_mutex_unlock(&g_dlna_mtx);

        pp_net_draw_rows(fb, rows, count, g_dlna_item_sel,
                         &g_dlna_item_scrl, frame);
    }

    pp_net_draw_footer3(fb, "OPEN / PLAY", "REFRESH", "BACK");
}

/* ------------------------------------------------------------------ */
/* IPTV screen                                                         */
/* ------------------------------------------------------------------ */

void draw_iptv_screen(uint32_t *fb) {
    static int frame = 0;
    frame++;

    pp_net_draw_header(fb, "IPTV / LIVE TV",
                       "M3U PLAYLISTS FROM USB + CUSTOM STREAMS", frame);

    char status[64];
    snprintf(status, sizeof(status), "%d CHANNEL%s", g_iptv_count,
             g_iptv_count == 1 ? "" : "S");
    pp_net_draw_status_line(fb, status);

    int total = g_iptv_count + 1; /* row 0 = custom URL action */

    static pp_net_list_row rows[PP_NET_MAX_IPTV + 1];
    snprintf(rows[0].title, sizeof(rows[0].title), "CUSTOM URL / STREAM");
    snprintf(rows[0].detail, sizeof(rows[0].detail),
             "TYPE ANY HTTP / HLS STREAM ADDRESS");
    rows[0].icon = 6;

    for (int i = 0; i < g_iptv_count; i++) {
        snprintf(rows[i + 1].title, sizeof(rows[i + 1].title), "%s",
                 g_iptv[i].name);
        snprintf(rows[i + 1].detail, sizeof(rows[i + 1].detail),
                 "%s  -  %s", g_iptv[i].group, g_iptv[i].source);
        rows[i + 1].icon = 6;
    }

    pp_net_draw_rows(fb, rows, total, g_iptv_sel, &g_iptv_scrl, frame);

    pp_net_draw_footer3(fb, "PLAY", "RELOAD", "BACK");
}

/* ------------------------------------------------------------------ */
/* System IME dialog (native PS5 on-screen keyboard)                   */
/*                                                                     */
/* PS4-inherited API kept on PS5: sceImeDialogInit opens the system    */
/* OSK over our frame; we poll GetStatus (1=RUNNING) until FINISHED    */
/* (2), then read the result. Text is UTF-16 in our own buffer.        */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t user_id;
    uint32_t type;                 /* 0 default, 2 url */
    uint64_t supported_languages;  /* 0 = all */
    uint32_t enter_label;          /* 0 default, 3 go */
    uint32_t input_method;         /* 0 default */
    void *filter;                  /* NULL */
    uint32_t option;               /* 0 */
    uint32_t max_text_length;
    uint16_t *input_text_buffer;
    float posx;
    float posy;
    uint32_t horizontal_alignment; /* 1 = center */
    uint32_t vertical_alignment;   /* 1 = center */
    const uint16_t *placeholder;
    const uint16_t *title;
    int8_t reserved[16];
} pp_ime_dialog_param;

typedef struct {
    int32_t endstatus; /* 0 ok, 1 user canceled, 2 aborted */
    int8_t reserved[12];
} pp_ime_dialog_result;

int sceImeDialogInit(void *param, void *extended);
int sceImeDialogGetStatus(void);
int sceImeDialogGetResult(void *result);
int sceImeDialogTerm(void);

#define PP_IME_MAX_TEXT 2047

static size_t pp_utf8_to_utf16(const char *in, uint16_t *out, size_t out_max) {
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && n < out_max - 1) {
        uint32_t cp;
        if (*p < 0x80) { cp = *p++; }
        else if ((*p & 0xE0) == 0xC0 && p[1]) {
            cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F); p += 2;
        } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3;
        } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            p += 4; cp = 0xFFFD; /* beyond BMP: replacement char */
        } else { cp = *p++; }
        out[n++] = (uint16_t)cp;
    }
    out[n] = 0;
    return n;
}

static size_t pp_utf16_to_utf8(const uint16_t *in, char *out, size_t out_max) {
    size_t n = 0;
    for (const uint16_t *p = in; *p; p++) {
        uint32_t cp = *p;
        if (cp < 0x80) {
            if (n + 1 >= out_max) break;
            out[n++] = (char)cp;
        } else if (cp < 0x800) {
            if (n + 2 >= out_max) break;
            out[n++] = (char)(0xC0 | (cp >> 6));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (n + 3 >= out_max) break;
            out[n++] = (char)(0xE0 | (cp >> 12));
            out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[n] = 0;
    return n;
}

/*
 * Open the system OSK and block until the user confirms or cancels.
 * Returns 0 = text confirmed (out filled), 1 = canceled, -1 = dialog
 * unavailable (caller should fall back to the built-in keyboard).
 */
static int pp_ime_get_text(int user_id, int url_type, const char *initial,
                           const char *title_utf8, char *out, size_t out_size) {
    static uint16_t buf16[PP_IME_MAX_TEXT + 1];
    static uint16_t title16[64];

    pp_ime_dialog_param param;
    memset(&param, 0, sizeof(param));
    param.user_id = user_id;
    param.type = url_type ? 2 : 0;
    param.supported_languages = 0;
    param.enter_label = 3; /* GO */
    param.input_method = 0;
    param.option = 0;
    param.max_text_length = PP_IME_MAX_TEXT;
    param.input_text_buffer = buf16;
    param.horizontal_alignment = 1;
    param.vertical_alignment = 1;
    param.title = title16;

    pp_utf8_to_utf16(initial ? initial : "", buf16, PP_IME_MAX_TEXT + 1);
    pp_utf8_to_utf16(title_utf8 ? title_utf8 : "PS PLAY", title16, 64);

    if (sceImeDialogInit(&param, NULL) != 0) {
        return -1;
    }

    /* wait for the dialog to actually come up (RUNNING = 1) */
    int started = 0;
    for (int i = 0; i < 120; i++) {
        if (sceImeDialogGetStatus() == 1) { started = 1; break; }
        usleep(16000);
    }
    if (!started) {
        sceImeDialogTerm();
        return -1;
    }

    /* modal: the system dialog draws over our frame, pad goes to it */
    while (sceImeDialogGetStatus() == 1) {
        usleep(16000);
    }

    pp_ime_dialog_result res;
    memset(&res, 0, sizeof(res));
    sceImeDialogGetResult(&res);
    int ok = (res.endstatus == 0);
    if (ok) {
        pp_utf16_to_utf8(buf16, out, out_size);
    }
    sceImeDialogTerm();
    return ok ? 0 : 1;
}

static int g_osk_ime_pending = 0;  /* auto-open system OSK on screen entry */
static int g_osk_ime_failed = 0;   /* system OSK unavailable: built-in only */

int sceUserServiceGetLoginUserIdList(int userId[4]);

static int pp_url_is_valid(const char *u) {
    size_t n = strlen(u);
    return (strncmp(u, "http://", 7) == 0 && n > 8) ||
           (strncmp(u, "https://", 8) == 0 && n > 9) ||
           (strncmp(u, "rtmp://", 7) == 0 && n > 8) ||
           (strncmp(u, "udp://", 6) == 0 && n > 7) ||
           (strncmp(u, "rtp://", 6) == 0 && n > 7);
}

/*
 * Drive the native OSK for the custom-stream URL: opens automatically,
 * reopens for corrections after an invalid URL, falls back to the
 * built-in keyboard when the system dialog is not available.
 */
static void prospero_url_entry_system_keyboard(void) {
    int users[4] = {0};
    sceUserServiceGetLoginUserIdList(users);
    for (;;) {
        int rc = pp_ime_get_text(users[0], 1, g_osk_buf, "PS PLAY - URL",
                                 g_osk_buf, sizeof(g_osk_buf));
        if (rc == -1) {
            g_osk_ime_failed = 1;
            toast("KEYBOARD", "SYSTEM KEYBOARD UNAVAILABLE - USING BUILT-IN");
            return;
        }
        if (rc == 1) { /* user canceled */
            screen = SCREEN_IPTV;
            return;
        }
        g_osk_len = (int)strlen(g_osk_buf);
        if (pp_url_is_valid(g_osk_buf)) {
            prospero_network_play(g_osk_buf, "CUSTOM STREAM");
            return;
        }
        toast("INVALID URL",
              "MUST START WITH HTTP:// HTTPS:// RTMP:// OR UDP://");
        /* loop: reopen the OSK with the current text for correction */
    }
}

/* ------------------------------------------------------------------ */
/* On-screen keyboard for custom stream URLs                           */
/* ------------------------------------------------------------------ */

static const char *OSK_CHAR_ROWS[] = {
    "0123456789",
    "abcdefghijklmnopqrstuvwxyz",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    ".-_/:+"
};
#define OSK_CHAR_ROW_COUNT 4
static const char *OSK_KEYS[] = { "DEL", "CLEAR", "SPACE", "PLAY", "CANCEL" };
#define OSK_KEY_COUNT 5

static int osk_row_len(int row) {
    if (row < OSK_CHAR_ROW_COUNT) {
        return (int)strlen(OSK_CHAR_ROWS[row]);
    }
    return OSK_KEY_COUNT;
}

static void osk_type_char(char c) {
    if (g_osk_len < (int)sizeof(g_osk_buf) - 1) {
        g_osk_buf[g_osk_len++] = c;
        g_osk_buf[g_osk_len] = 0;
    }
}

static void osk_backspace(void) {
    if (g_osk_len > 0) {
        g_osk_buf[--g_osk_len] = 0;
    }
}

static void osk_activate_key(int key) {
    switch (key) {
    case 0: osk_backspace(); break;
    case 1: g_osk_len = 0; g_osk_buf[0] = 0; break;
    case 2: osk_type_char(' '); break;
    case 3: {
        if (strncmp(g_osk_buf, "http://", 7) == 0 ||
            strncmp(g_osk_buf, "https://", 8) == 0 ||
            strncmp(g_osk_buf, "rtmp://", 7) == 0 ||
            strncmp(g_osk_buf, "udp://", 6) == 0 ||
            strncmp(g_osk_buf, "rtp://", 6) == 0) {
            prospero_network_play(g_osk_buf, "CUSTOM STREAM");
        } else {
            toast("INVALID URL",
                  "MUST START WITH HTTP:// HTTPS:// RTMP:// OR UDP://");
        }
        break;
    }
    case 4:
        screen = SCREEN_IPTV;
        break;
    }
}

void draw_url_entry_screen(uint32_t *fb) {
    static int frame = 0;
    frame++;

    pp_net_draw_header(fb, "CUSTOM STREAM URL",
                       "SYSTEM KEYBOARD OPENS AUTOMATICALLY  -  OPTIONS REOPEN", frame);

    /* URL field */
    ui_round_asset(fb, 140, 200, 1640, 90, 14,
                   RR_BGRA(41,41,41,220), RR_BGRA(205,205,205,160),
                   RR_BGRA(147,147,147,40));

    /* show the tail of long URLs */
    const char *disp = g_osk_buf;
    int dlen = g_osk_len;
    if (dlen > 78) disp = g_osk_buf + (dlen - 78);

    char field[96];
    snprintf(field, sizeof(field), "%s%s", disp,
             (frame / 24) % 2 == 0 ? "_" : " ");
    rr_text(fb, 172, 230, field, RR_BGRA(232, 244, 255, 240), 2);

    /* character grid */
    const int cell_w = 62;
    const int cell_h = 60;
    const int grid_x = 150;
    const int grid_y = 350;

    for (int r = 0; r < OSK_CHAR_ROW_COUNT; r++) {
        const char *chars = OSK_CHAR_ROWS[r];
        int n = (int)strlen(chars);
        for (int c = 0; c < n; c++) {
            int x = grid_x + c * cell_w;
            int y = grid_y + r * (cell_h + 12);
            int sel = (g_osk_row == r && g_osk_col == c);

            if (sel) {
                ui_round_asset(fb, x - 4, y - 6, cell_w - 4, cell_h, 8,
                               RR_BGRA(147,147,147,200),
                               RR_BGRA(205,205,205,230),
                               RR_BGRA(205,205,205,60));
            } else {
                ui_round_asset(fb, x - 4, y - 6, cell_w - 4, cell_h, 8,
                               RR_BGRA(40,40,40,170),
                               RR_BGRA(105,105,105,120), 0);
            }

            char s[2] = { chars[c], 0 };
            rr_text(fb, x + 14, y + 10, s,
                    sel ? RR_BGRA(255, 255, 255, 255)
                        : RR_BGRA(232,232,232,220),
                    2);
        }
    }

    /* action keys */
    const int keys_y = grid_y + OSK_CHAR_ROW_COUNT * (cell_h + 12) + 14;
    const int key_w = 300;
    const int key_gap = 22;
    int keys_total_w = OSK_KEY_COUNT * key_w + (OSK_KEY_COUNT - 1) * key_gap;
    int keys_x = (1920 - keys_total_w) / 2;

    for (int k = 0; k < OSK_KEY_COUNT; k++) {
        int x = keys_x + k * (key_w + key_gap);
        int sel = (g_osk_row == OSK_CHAR_ROW_COUNT && g_osk_col == k);

        if (sel) {
            ui_round_asset(fb, x, keys_y, key_w, 72, 10,
                           RR_BGRA(147,147,147,210),
                           RR_BGRA(205,205,205,235),
                           RR_BGRA(205,205,205,60));
        } else {
            ui_round_asset(fb, x, keys_y, key_w, 72, 10,
                           RR_BGRA(40,40,40,180),
                           RR_BGRA(105,105,105,130), 0);
        }

        int tw = (int)strlen(OSK_KEYS[k]) * 24;
        rr_text(fb, x + (key_w - tw) / 2, keys_y + 20, OSK_KEYS[k],
                sel ? RR_BGRA(255, 255, 255, 255)
                    : RR_BGRA(232,232,232,225),
                2);
    }

    pp_net_draw_footer3(fb, "TYPE", "SYSTEM KEYBOARD", "CANCEL");
}

/* ------------------------------------------------------------------ */
/* Input handling for all network screens                              */
/* ------------------------------------------------------------------ */

void draw_dmr_screen(uint32_t *fb) {
    static int frame = 0;
    frame++;

    pp_net_draw_header(fb, "DLNA RECEIVER",
                       "CAST TO THIS CONSOLE - WIRELESS", frame);

    pp_dmr_status st;
    pp_dmr_get_status(&st);

    char line[300];

    if (st.running) {
        snprintf(line, sizeof(line), "SERVICE ACTIVE  -  %s:%d", st.ip, st.port);
        pp_net_draw_status_line(fb, line);
    } else {
        pp_net_draw_status_line(fb, "SERVICE NOT RUNNING");
    }

    /* Renderer identity card */
    rr_text(fb, 230, 300, "RENDERER NAME", RR_BGRA(204,204,204,215), 1);
    rr_text(fb, 230, 336, st.running ? st.name : "-",
            RR_BGRA(232, 244, 255, 235), 2);

    /* Transport state */
    const char *state = "WAITING FOR CAST";
    uint32_t state_col = RR_BGRA(203,203,203,220);
    if (st.transport == PP_DMR_TRANSPORT_PLAYING) {
        state = "PLAYING";
        state_col = RR_BGRA(231,231,231,240);
    } else if (st.transport == PP_DMR_TRANSPORT_PAUSED) {
        state = "PAUSED";
        state_col = RR_BGRA(255, 220, 120, 240);
    } else if (st.transport == PP_DMR_TRANSPORT_STOPPED && st.uri[0]) {
        state = "READY - PRESS PLAY ON YOUR PHONE";
    } else if (st.transport == PP_DMR_TRANSPORT_TRANSITIONING) {
        state = "STARTING STREAM...";
    }

    rr_text(fb, 230, 420, "CAST STATE", RR_BGRA(204,204,204,215), 1);
    rr_text(fb, 230, 456, state, state_col, 2);

    if (st.uri[0]) {
        char posb[32], durb[32];
        format_time_mmss(posb, sizeof(posb), st.position_s);
        format_time_mmss(durb, sizeof(durb), st.duration_s);

        rr_text(fb, 230, 540, "NOW CASTING", RR_BGRA(204,204,204,215), 1);
        rr_text(fb, 230, 576, st.title[0] ? st.title : "NETWORK STREAM",
                RR_BGRA(232, 244, 255, 235), 2);

        if (st.duration_s > 1.0) {
            snprintf(line, sizeof(line), "%s  /  %s", posb, durb);
        } else {
            snprintf(line, sizeof(line), "%s  /  LIVE", posb);
        }
        rr_text(fb, 230, 626, line, RR_BGRA(203,203,203,220), 1);

        /* progress bar */
        int bx = 230, bw = 1000, by = 668;
        rr_fill(fb, bx, by, bw, 8, RR_BGRA(102,102,102,140));
        double frac = (st.duration_s > 1.0)
                          ? st.position_s / st.duration_s
                          : 0.0;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        rr_fill(fb, bx, by, (int)(bw * frac), 8, RR_BGRA(213,213,213,240));
    }

    /* Instructions */
    rr_text(fb, 230, 760, "HOW TO CAST", RR_BGRA(204,204,204,215), 1);
    rr_text(fb, 230, 796,
            "BUBBLEUPNP: DEVICES -> PROSPEROPLAYER PS5 -> PLAY MEDIA",
            RR_BGRA(230,230,230,220), 1);
    rr_text(fb, 230, 836,
            "THE CONSOLE PLAYS VIDEO AND AUDIO LIKE A CHROMECAST",
            RR_BGRA(230,230,230,220), 1);

    pp_net_draw_footer3(fb, "PAUSE / RESUME", "STOP CAST", "BACK");
}

static void prospero_net_handle_input(uint32_t pressed) {
    if (!pressed) return;

    if (screen == SCREEN_DLNA_SERVERS) {
        if (pressed & PS5_PAD_BUTTON_DOWN) {
            if (g_dlna_server_count > 0)
                g_dlna_server_sel =
                    (g_dlna_server_sel + 1) % g_dlna_server_count;
        } else if (pressed & PS5_PAD_BUTTON_UP) {
            if (g_dlna_server_count > 0)
                g_dlna_server_sel =
                    (g_dlna_server_sel + g_dlna_server_count - 1) %
                    g_dlna_server_count;
        } else if (pressed & PS5_PAD_BUTTON_RIGHT) {
            if (g_dlna_server_count > 0) {
                g_dlna_server_sel += PP_NET_LIST_VISIBLE;
                if (g_dlna_server_sel >= g_dlna_server_count)
                    g_dlna_server_sel = g_dlna_server_count - 1;
            }
        } else if (pressed & PS5_PAD_BUTTON_LEFT) {
            g_dlna_server_sel -= PP_NET_LIST_VISIBLE;
            if (g_dlna_server_sel < 0) g_dlna_server_sel = 0;
        } else if (pressed & PS5_PAD_BUTTON_CROSS) {
            if (g_dlna_status != 1 && g_dlna_server_count > 0) {
                dlna_open_server(g_dlna_server_sel);
            }
        } else if (pressed & PS5_PAD_BUTTON_SQUARE) {
            dlna_start_discover();
        } else if (pressed & PS5_PAD_BUTTON_CIRCLE) {
            screen = 0;
        }
        return;
    }

    if (screen == SCREEN_DLNA_BROWSE) {
        if (pressed & PS5_PAD_BUTTON_DOWN) {
            if (g_dlna_item_count > 0)
                g_dlna_item_sel = (g_dlna_item_sel + 1) % g_dlna_item_count;
        } else if (pressed & PS5_PAD_BUTTON_UP) {
            if (g_dlna_item_count > 0)
                g_dlna_item_sel =
                    (g_dlna_item_sel + g_dlna_item_count - 1) %
                    g_dlna_item_count;
        } else if (pressed & PS5_PAD_BUTTON_RIGHT) {
            if (g_dlna_item_count > 0) {
                g_dlna_item_sel += PP_NET_LIST_VISIBLE;
                if (g_dlna_item_sel >= g_dlna_item_count)
                    g_dlna_item_sel = g_dlna_item_count - 1;
            }
        } else if (pressed & PS5_PAD_BUTTON_LEFT) {
            g_dlna_item_sel -= PP_NET_LIST_VISIBLE;
            if (g_dlna_item_sel < 0) g_dlna_item_sel = 0;
        } else if (pressed & PS5_PAD_BUTTON_CROSS) {
            if (g_dlna_status != 1 && g_dlna_item_count > 0 &&
                g_dlna_item_sel < g_dlna_item_count) {
                pp_dlna_object o;
                pthread_mutex_lock(&g_dlna_mtx);
                memcpy(&o, &g_dlna_items[g_dlna_item_sel], sizeof(o));
                pthread_mutex_unlock(&g_dlna_mtx);

                if (o.is_container) {
                    dlna_open_container(&o);
                } else if (pp_dlna_object_is_image(&o)) {
                    toast("DLNA", "Image viewing is not supported yet");
                } else if (o.res_url[0]) {
                    prospero_network_play(o.res_url, o.title);
                } else {
                    toast("DLNA", "This item has no playable stream");
                }
            }
        } else if (pressed & PS5_PAD_BUTTON_SQUARE) {
            if (g_dlna_status != 1) dlna_refresh_folder();
        } else if (pressed & PS5_PAD_BUTTON_CIRCLE) {
            if (g_dlna_status != 1) dlna_go_back();
        }
        return;
    }

    if (screen == SCREEN_IPTV) {
        int total = g_iptv_count + 1;
        if (pressed & PS5_PAD_BUTTON_DOWN) {
            g_iptv_sel = (g_iptv_sel + 1) % total;
        } else if (pressed & PS5_PAD_BUTTON_UP) {
            g_iptv_sel = (g_iptv_sel + total - 1) % total;
        } else if (pressed & PS5_PAD_BUTTON_RIGHT) {
            g_iptv_sel += PP_NET_LIST_VISIBLE;
            if (g_iptv_sel >= total) g_iptv_sel = total - 1;
        } else if (pressed & PS5_PAD_BUTTON_LEFT) {
            g_iptv_sel -= PP_NET_LIST_VISIBLE;
            if (g_iptv_sel < 0) g_iptv_sel = 0;
        } else if (pressed & PS5_PAD_BUTTON_CROSS) {
            if (g_iptv_sel == 0) {
                /* custom URL entry */
                g_osk_row = 0;
                g_osk_col = 0;
                if (g_osk_len == 0) {
                    snprintf(g_osk_buf, sizeof(g_osk_buf), "http://");
                    g_osk_len = 7;
                }
                g_osk_ime_pending = 1;
                screen = SCREEN_URL_ENTRY;
            } else if (g_iptv_sel - 1 < g_iptv_count) {
                pp_iptv_channel *c = &g_iptv[g_iptv_sel - 1];
                prospero_network_play(c->url, c->name);
            }
        } else if (pressed & PS5_PAD_BUTTON_SQUARE) {
            g_iptv_count = pp_iptv_load(g_iptv, PP_NET_MAX_IPTV);
            g_iptv_loaded = 1;
            if (g_iptv_sel > g_iptv_count) g_iptv_sel = g_iptv_count;
            toast("IPTV", "PLAYLISTS RELOADED FROM USB");
        } else if (pressed & PS5_PAD_BUTTON_CIRCLE) {
            screen = 0;
        }
        return;
    }

    if (screen == SCREEN_DMR) {
        /*
         * Cast receiver status screen. The renderer service runs
         * independently of this screen (started at boot); playback
         * may be running in the background while we are here.
         */
        if (pressed & PS5_PAD_BUTTON_CROSS) {
            /* pause/resume whatever is being cast right now */
            pp_dmr_status st;
            pp_dmr_get_status(&st);
            if (st.transport == PP_DMR_TRANSPORT_PLAYING) {
                if (!player_paused) {
                    player_paused = 1;
                    pp_playback_pause(&g_pp_pb);
                }
                pp_dmr_report_paused();
            } else if (st.transport == PP_DMR_TRANSPORT_PAUSED) {
                if (player_paused) {
                    player_paused = 0;
                    pp_playback_resume(&g_pp_pb);
                }
                pp_dmr_report_playing();
            }
        } else if (pressed & PS5_PAD_BUTTON_SQUARE) {
            /* stop the current cast */
            pp_dmr_status st;
            pp_dmr_get_status(&st);
            if (st.transport == PP_DMR_TRANSPORT_PLAYING ||
                st.transport == PP_DMR_TRANSPORT_PAUSED ||
                st.transport == PP_DMR_TRANSPORT_TRANSITIONING ||
                g_pp_pb.active) {
                stop_video_playback();
            }
            pp_dmr_report_stopped();
            toast("DLNA CAST", "Cast stopped");
        } else if (pressed & PS5_PAD_BUTTON_CIRCLE) {
            screen = 0;
        }
        return;
    }

    if (screen == SCREEN_URL_ENTRY) {
        if (g_osk_ime_pending) {
            g_osk_ime_pending = 0;
            if (!g_osk_ime_failed) {
                prospero_url_entry_system_keyboard();
            }
            return;
        }
        if (pressed & PS5_PAD_BUTTON_OPTIONS && !g_osk_ime_failed) {
            /* reopen the native keyboard */
            g_osk_ime_pending = 1;
            return;
        }
        if (pressed & PS5_PAD_BUTTON_DOWN) {
            g_osk_row++;
            if (g_osk_row > OSK_CHAR_ROW_COUNT) g_osk_row = 0;
            int len = osk_row_len(g_osk_row);
            if (g_osk_col >= len) g_osk_col = len - 1;
        } else if (pressed & PS5_PAD_BUTTON_UP) {
            g_osk_row--;
            if (g_osk_row < 0) g_osk_row = OSK_CHAR_ROW_COUNT;
            int len = osk_row_len(g_osk_row);
            if (g_osk_col >= len) g_osk_col = len - 1;
        } else if (pressed & PS5_PAD_BUTTON_RIGHT) {
            int len = osk_row_len(g_osk_row);
            g_osk_col = (g_osk_col + 1) % len;
        } else if (pressed & PS5_PAD_BUTTON_LEFT) {
            int len = osk_row_len(g_osk_row);
            g_osk_col = (g_osk_col + len - 1) % len;
        } else if (pressed & PS5_PAD_BUTTON_CROSS) {
            if (g_osk_row < OSK_CHAR_ROW_COUNT) {
                osk_type_char(OSK_CHAR_ROWS[g_osk_row][g_osk_col]);
            } else {
                osk_activate_key(g_osk_col);
            }
        } else if (pressed & PS5_PAD_BUTTON_TRIANGLE) {
            osk_backspace();
        } else if (pressed & PS5_PAD_BUTTON_SQUARE) {
            osk_type_char(' ');
        } else if (pressed & PS5_PAD_BUTTON_OPTIONS) {
            osk_activate_key(3); /* PLAY */
        } else if (pressed & PS5_PAD_BUTTON_CIRCLE) {
            screen = SCREEN_IPTV;
        }
        return;
    }
}



int sceUserServiceInitialize(void *);
int sceUserServiceGetLoginUserIdList(int userId[4]);
int scePadInit(void);
int scePadOpen(int,int,int,void *);



/* PROSPERO_TOAST_RENDERER_START */

static void draw_prospero_toast(
    uint32_t *fb
) {
    if (!prospero_toast_active) {
        return;
    }

    long long elapsed =
        now_ms() - prospero_toast_started_ms;

    const int hold_ms = 2200;
    const int fade_ms = 360;
    const int total_ms =
        hold_ms + fade_ms;

    if (elapsed >= total_ms) {
        prospero_toast_active = 0;
        return;
    }

    int alpha = 255;

    if (elapsed > hold_ms) {
        alpha =
            255 -
            (int)(
                (elapsed - hold_ms) *
                255 /
                fade_ms
            );

        if (alpha < 0) {
            alpha = 0;
        }
    }

    int enter_offset = 0;

    if (elapsed < 180) {
        enter_offset =
            90 -
            (int)(elapsed * 90 / 180);
    }

    int panel_w = 530;
    int panel_h = 132;
    int panel_x =
        1920 - panel_w - 58 + enter_offset;
    int panel_y = 770;

    uint32_t border =
        prospero_toast_kind == 2
            ? RR_BGRA(
                255,
                105,
                105,
                alpha * 225 / 255
            )
            : RR_BGRA(
                0,
                205,
                255,
                alpha * 205 / 255
            );

    uint32_t glow =
        prospero_toast_kind == 2
            ? RR_BGRA(
                255,
                60,
                80,
                alpha * 55 / 255
            )
            : RR_BGRA(
                0,
                135,
                255,
                alpha * 55 / 255
            );

    ui_round_asset(
        fb,
        panel_x,
        panel_y,
        panel_w,
        panel_h,
        24,
        RR_BGRA(
            1,
            9,
            22,
            alpha * 230 / 255
        ),
        border,
        glow
    );

    rr_fill(
        fb,
        panel_x + 28,
        panel_y + 26,
        5,
        panel_h - 52,
        border
    );

    rr_text(
        fb,
        panel_x + 58,
        panel_y + 24,
        prospero_toast_title,
        prospero_toast_kind == 2
            ? RR_BGRA(
                255,
                185,
                185,
                alpha * 240 / 255
            )
            : RR_BGRA(
                225,
                245,
                255,
                alpha * 240 / 255
            ),
        1
    );

    char message[180];

    snprintf(
        message,
        sizeof(message),
        "%s",
        prospero_toast_message
    );

    if (strlen(message) > 54) {
        message[51] = '.';
        message[52] = '.';
        message[53] = '.';
        message[54] = 0;
    }

    rr_text(
        fb,
        panel_x + 58,
        panel_y + 72,
        message,
        RR_BGRA(
            135,
            200,
            228,
            alpha * 220 / 255
        ),
        0
    );
}

/* PROSPERO_TOAST_RENDERER_END */


/* PROSPERO_REAL_SEEK_START */




/* PROSPERO_REAL_SEEK_END */


/* PROSPERO_NETFLIX_SCRUB_START */

static int prospero_scrub_active = 0;
static int prospero_scrub_was_paused = 0;

/*
 * PS Play 1.5: playback settings dialog (OPTIONS in player).
 * All secondary playback actions live here so the pad stays safe:
 * subtitles, audio track, view mode, info and stats.
 */
static int g_player_menu_open = 0;
static int g_player_menu_sel = 0;

#define PP_PMENU_ITEM_COUNT 7

#define PP_PMENU_SUBS       0
#define PP_PMENU_SUB_TRACK  1
#define PP_PMENU_SUB_DELAY  2
#define PP_PMENU_AUDIO      3
#define PP_PMENU_VIEW       4
#define PP_PMENU_INFO       5
#define PP_PMENU_STATS      6

static double prospero_scrub_origin = 0.0;
static double prospero_scrub_target = 0.0;

static double prospero_player_position(void) {
    double position =
        resume_base_offset_seconds +
        prospero_media_clock_seconds();

    if (position < 0.0) {
        position = 0.0;
    }

    return position;
}

/*
 * Playback settings dialog (PS Play 1.5).
 * One rounded panel with every secondary option that used to be
 * scattered across TRIANGLE / SQUARE / L1-R1 / L2-R3 / L3 / R2.
 * Drawn over the video; the pad is safe while it is open.
 */
static void prospero_draw_player_menu(uint32_t *fb) {
    if (screen != SCREEN_PLAYER || !g_player_menu_open) return;

    int pw = 780;
    int ph = 560;
    int px = (1920 - pw) / 2;
    int py = (1080 - ph) / 2;

    /* dim the video a little so the panel is readable */
    rr_fill(fb, 0, 0, 1920, 1080, RR_BGRA(0, 0, 0, 90));
    rr_fill_round(fb, px, py, pw, ph, 18, RR_BGRA(16, 16, 18, 232));
    rr_fill_round(fb, px, py, pw, 64, 18, RR_BGRA(255, 255, 255, 18));

    rr_text_center(
        fb, 960, py + 20,
        "PLAYBACK SETTINGS",
        RR_BGRA(240, 240, 240, 245), 1
    );

    static const char *labels[PP_PMENU_ITEM_COUNT] = {
        "SUBTITLES",
        "SUBTITLE TRACK",
        "SUBTITLE DELAY",
        "AUDIO TRACK",
        "ASPECT RATIO",
        "MEDIA INFO",
        "STATISTICS"
    };

    int row_y = py + 96;
    int row_h = 54;

    for (int i = 0; i < PP_PMENU_ITEM_COUNT; i++) {
        int ry = row_y + i * row_h;

        if (i == g_player_menu_sel) {
            rr_fill_round(
                fb, px + 24, ry - 6, pw - 48, row_h - 6, 10,
                RR_BGRA(255, 255, 255, 34)
            );
        }

        uint32_t label_color =
            i == g_player_menu_sel
                ? RR_BGRA(255, 255, 255, 245)
                : RR_BGRA(205, 205, 205, 225);

        rr_text(
            fb, px + 48, ry + 6,
            labels[i], label_color, 1
        );

        char value[160];
        value[0] = 0;

        if (i == PP_PMENU_SUBS) {
            snprintf(
                value, sizeof(value), "%s",
                prospero_subtitle_enabled ? "ON" : "OFF"
            );
        } else if (i == PP_PMENU_SUB_TRACK) {
            int current_track =
                prospero_subtitle_use_external
                    ? -1
                    : prospero_embedded_subtitle_stream_index;
            if (!prospero_subtitle_enabled) {
                snprintf(value, sizeof(value), "DISABLED");
            } else {
                prospero_subtitle_track_label(
                    current_track, value, sizeof(value)
                );
            }
        } else if (i == PP_PMENU_SUB_DELAY) {
            snprintf(
                value, sizeof(value), "%+d MS",
                prospero_subtitle_delay_ms
            );
        } else if (i == PP_PMENU_AUDIO) {
            snprintf(
                value, sizeof(value), "%s",
                prospero_audio_active_label[0]
                    ? prospero_audio_active_label
                    : "DEFAULT"
            );
        } else if (i == PP_PMENU_VIEW) {
            snprintf(
                value, sizeof(value), "%s",
                video_view_mode == 0
                    ? "FIT"
                    : (video_view_mode == 1 ? "FILL" : "STRETCH")
            );
        } else if (i == PP_PMENU_INFO) {
            snprintf(value, sizeof(value), "OPEN");
        } else if (i == PP_PMENU_STATS) {
            snprintf(
                value, sizeof(value), "%s",
                show_stats_for_nerds ? "ON" : "OFF"
            );
        }

        if (strlen(value) > 30) {
            value[27] = '.';
            value[28] = '.';
            value[29] = '.';
            value[30] = 0;
        }

        uint32_t value_color =
            i == g_player_menu_sel
                ? RR_BGRA(255, 220, 120, 245)
                : RR_BGRA(170, 170, 170, 220);

        rr_text(
            fb,
            px + pw - 48 - rr_text_width(value, 0),
            ry + 10,
            value, value_color, 0
        );
    }

    rr_text_center(
        fb, 960, py + ph - 44,
        "X SELECT    < > ADJUST    OPTIONS / O CLOSE",
        RR_BGRA(190, 190, 190, 215), 0
    );
}

static double prospero_scrub_clamp(
    double position
) {
    if (position < 0.0) {
        position = 0.0;
    }

    if (media_duration_sec > 1.0) {
        double maximum =
            media_duration_sec - 1.0;

        if (maximum < 0.0) {
            maximum = 0.0;
        }

        if (position > maximum) {
            position = maximum;
        }
    }

    return position;
}

static void prospero_scrub_begin(void) {
    if (
        prospero_scrub_active ||
        screen != SCREEN_PLAYER ||
        !current_media_path[0]
    ) {
        return;
    }

    prospero_scrub_origin =
        prospero_player_position();

    prospero_scrub_target =
        prospero_scrub_origin;

    prospero_scrub_was_paused =
        player_paused;

    player_paused = 1;
    prospero_scrub_active = 1;
    controls_last_used_ms = now_ms();
}

static void prospero_scrub_move(
    double seconds
) {
    if (!prospero_scrub_active) {
        prospero_scrub_begin();
    }

    if (!prospero_scrub_active) {
        return;
    }

    prospero_scrub_target =
        prospero_scrub_clamp(
            prospero_scrub_target + seconds
        );

    controls_last_used_ms = now_ms();
}

static void prospero_chapter_jump(int direction)
{
    double pos;
    int cur;
    int next;
    char msg[96];
    char tbuf[32];

    if (prospero_chapter_count <= 0 || screen != SCREEN_PLAYER)
        return;

    pos = prospero_player_position();
    cur = prospero_chapter_index_at(pos);

    if (direction < 0) {
        if (cur >= 0 &&
            pos - prospero_chapter_start[cur] > 2.0)
            next = cur;
        else if (cur > 0)
            next = cur - 1;
        else
            next = 0;
    } else {
        if (cur < 0)
            next = 0;
        else if (cur + 1 < prospero_chapter_count)
            next = cur + 1;
        else
            next = prospero_chapter_count - 1;
    }

    if (!prospero_request_inplace_seek(
            prospero_chapter_start[next], player_paused)) {
        toast("CHAPTER", "Seek failed");
        return;
    }

    format_time_mmss(tbuf, sizeof(tbuf), prospero_chapter_start[next]);
    snprintf(
        msg,
        sizeof(msg),
        "%d/%d  %s  %s",
        next + 1,
        prospero_chapter_count,
        tbuf,
        prospero_chapter_title[next]);
    toast("CHAPTER", msg);
    controls_last_used_ms = now_ms();
}

static void prospero_subtitle_nudge_delay(int delta_ms)
{
    char msg[64];
    prospero_subtitle_delay_ms += delta_ms;
    if (prospero_subtitle_delay_ms < -5000)
        prospero_subtitle_delay_ms = -5000;
    if (prospero_subtitle_delay_ms > 5000)
        prospero_subtitle_delay_ms = 5000;

    if (prospero_subtitle_delay_ms == 0)
        snprintf(msg, sizeof(msg), "0 ms (sync)");
    else
        snprintf(
            msg,
            sizeof(msg),
            "%+d ms",
            prospero_subtitle_delay_ms);
    toast("SUB DELAY", msg);
    controls_last_used_ms = now_ms();
}


/*
 * OPTIONS playback dialog: activate or adjust the selected row.
 * back=1 when invoked with LEFT (adjust backwards), 0 for X/RIGHT.
 */
static void prospero_player_menu_activate(int back)
{
    switch (g_player_menu_sel) {
    case PP_PMENU_SUBS:
        prospero_subtitle_toggle();
        break;

    case PP_PMENU_SUB_TRACK:
        if (prospero_subtitle_enabled || !back)
            prospero_subtitle_cycle_track();
        break;

    case PP_PMENU_SUB_DELAY:
        prospero_subtitle_nudge_delay(back ? -100 : +100);
        break;

    case PP_PMENU_AUDIO:
        prospero_audio_cycle_track();
        break;

    case PP_PMENU_VIEW:
        if (prospero_music_mode)
            break;
        video_view_mode += back ? -1 : 1;
        if (video_view_mode > 2)
            video_view_mode = 0;
        if (video_view_mode < 0)
            video_view_mode = 2;
#if PP_BACKEND_ENABLED
        prospero_apply_view_mode();
#endif
        toast(
            "VIEW MODE",
            video_view_mode == 0
                ? "FIT"
                : (video_view_mode == 1 ? "FILL" : "STRETCH")
        );
        break;

    case PP_PMENU_INFO:
        g_player_menu_open = 0;
        /* Media Info is 1080 UI - leave 4K V8 surface first */
#if PP_BACKEND_ENABLED
        if (g_vo_w != 1920u || g_vo_h != 1080u ||
            g_pp_backend == PP_BACKEND_4K_V8_FUSED) {
            g_pp_backend = PP_BACKEND_1080_STANDARD;
            pp_playback_set_backend(&g_pp_pb, PP_BACKEND_1080_STANDARD);
            pp_product_request_vo(1920, 1080, 2,
                                  PP_BACKEND_1080_STANDARD,
                                  WIDTH, HEIGHT);
        }
#endif
        screen = SCREEN_MEDIA_INFO;
        break;

    case PP_PMENU_STATS:
        show_stats_for_nerds = !show_stats_for_nerds;
        toast("STATS", show_stats_for_nerds ? "ON" : "OFF");
        break;
    }
}


/* SCRUB_HOLD_FORWARD_DECLARATION_START */
static void prospero_scrub_hold_reset(void);
/* SCRUB_HOLD_FORWARD_DECLARATION_END */

static void prospero_scrub_cancel(void) {
    prospero_scrub_hold_reset();

    if (!prospero_scrub_active) {
        return;
    }

    prospero_scrub_active = 0;
    prospero_scrub_target =
        prospero_scrub_origin;

    player_paused =
        prospero_scrub_was_paused;

    controls_last_used_ms = now_ms();
}

static int prospero_scrub_confirm(void) {
    prospero_scrub_hold_reset();

    if (
        !prospero_scrub_active ||
        !current_media_path[0]
    ) {
        return 0;
    }

    double target =
        prospero_scrub_clamp(
            prospero_scrub_target
        );

    int restore_paused =
        prospero_scrub_was_paused;

    prospero_scrub_active = 0;
    controls_last_used_ms = now_ms();

    if (
        !prospero_request_inplace_seek(
            target,
            restore_paused
        )
    ) {
        player_paused =
            restore_paused;

        toast(
            "SEEK",
            "Unable to submit seek"
        );

        return 0;
    }

    char time_text[32];
    char message[96];

    format_time_mmss(
        time_text,
        sizeof(time_text),
        target
    );

    snprintf(
        message,
        sizeof(message),
        "Seeking to %s",
        time_text
    );

    toast(
        "SEEK",
        message
    );

    return 1;
}

/* PROSPERO_NETFLIX_SCRUB_END */


/* PROSPERO_SCRUB_HOLD_START */

static int prospero_scrub_hold_direction = 0;

static long long prospero_scrub_hold_started_ms = 0;
static long long prospero_scrub_hold_last_repeat_ms = 0;


static void prospero_scrub_hold_reset(void) {
    prospero_scrub_hold_direction = 0;
    prospero_scrub_hold_started_ms = 0;
    prospero_scrub_hold_last_repeat_ms = 0;
}


static void prospero_scrub_hold_update(
    uint32_t held_buttons
) {
    if (screen != SCREEN_PLAYER || g_player_menu_open) {
        prospero_scrub_hold_reset();
        return;
    }

    int left_held =
        (held_buttons & PS5_PAD_BUTTON_LEFT) != 0;

    int right_held =
        (held_buttons & PS5_PAD_BUTTON_RIGHT) != 0;

    /*
     * Ignore conflicting directions or a released D-pad.
     */
    if (
        left_held == right_held
    ) {
        prospero_scrub_hold_reset();
        return;
    }

    int direction =
        left_held ? -1 : 1;

    long long now =
        now_ms();

    /*
     * A new hold starts a fresh acceleration curve.
     * The original press is still handled by the existing
     * pressed-button code, so this does not double-step.
     */
    if (
        prospero_scrub_hold_direction != direction
    ) {
        prospero_scrub_hold_direction =
            direction;

        prospero_scrub_hold_started_ms =
            now;

        prospero_scrub_hold_last_repeat_ms =
            now;

        return;
    }

    /*
     * Do not begin repeating until the user has clearly held
     * the button instead of tapping it.
     */
    long long held_for =
        now - prospero_scrub_hold_started_ms;

    if (held_for < 350) {
        return;
    }

    /*
     * Ensure the initial edge press has entered scrub mode.
     */
    if (!prospero_scrub_active) {
        return;
    }

    double step_seconds = 10.0;
    long long repeat_interval_ms = 180;

    if (held_for >= 2500) {
        step_seconds = 60.0;
        repeat_interval_ms = 110;
    } else if (held_for >= 1200) {
        step_seconds = 30.0;
        repeat_interval_ms = 140;
    }

    if (
        now - prospero_scrub_hold_last_repeat_ms
        < repeat_interval_ms
    ) {
        return;
    }

    prospero_scrub_hold_last_repeat_ms =
        now;

    prospero_scrub_move(
        direction * step_seconds
    );
}

/* PROSPERO_SCRUB_HOLD_END */


/* PROSPERO_USB_FAVORITES_START */

static int prospero_usb_entry_is_video(
    const char *name
) {
    const char *ext =
        name ? strrchr(name, '.') : NULL;

    if (!ext) {
        return 0;
    }

    return (
        strcasecmp(ext, ".mkv") == 0 ||
        strcasecmp(ext, ".mp4") == 0 ||
        strcasecmp(ext, ".mov") == 0 ||
        strcasecmp(ext, ".avi") == 0 ||
        strcasecmp(ext, ".m4v") == 0 ||
        strcasecmp(ext, ".webm") == 0
    );
}

static int prospero_usb_entry_is_audio(
    const char *name
) {
    const char *ext =
        name ? strrchr(name, '.') : NULL;

    if (!ext) {
        return 0;
    }

    return (
        strcasecmp(ext, ".mp3") == 0 ||
        strcasecmp(ext, ".m4a") == 0 ||
        strcasecmp(ext, ".aac") == 0 ||
        strcasecmp(ext, ".wav") == 0 ||
        strcasecmp(ext, ".flac") == 0 ||
        strcasecmp(ext, ".ogg") == 0
    );
}

static int prospero_usb_entry_is_media(
    const char *name
) {
    return prospero_usb_entry_is_video(name) ||
           prospero_usb_entry_is_audio(name);
}


static void prospero_usb_toggle_selected_favorite(
    void
) {
    if (
        screen != 1 ||
        file_count <= 0 ||
        file_selected < 0 ||
        file_selected >= file_count
    ) {
        return;
    }

    if (usb_types[file_selected] == 4) {
        toast("FAVORITES", "SELECT A FILE");
        return;
    }

    const char *name =
        usb_files[file_selected];

    if (!prospero_usb_entry_is_media(name)) {
        toast("FAVORITES", "VIDEO OR MUSIC ONLY");
        return;
    }

    char full_path[768];

    snprintf(
        full_path,
        sizeof(full_path),
        "%s/%s",
        current_path,
        name
    );

    if (favorites_is_favorite(full_path)) {
        favorites_remove(full_path);
        favorites_save();
        toast("FAVORITES", "REMOVED");
        return;
    }

    char title[128];
    char metadata[96];

    title[0] = 0;
    metadata[0] = 0;

    clean_media_title(
        full_path,
        title,
        sizeof(title),
        metadata,
        sizeof(metadata)
    );

    favorites_add(
        full_path,
        title[0] ? title : name,
        0.0
    );

    favorites_save();
    toast("FAVORITES", "ADDED");
}

/* PROSPERO_USB_FAVORITES_END */



/* PROSPERO_PLAYBACK_COMPLETE_MODULE_START */

static double prospero_playback_current_position(
    void
) {
    double active_clock =
        audio_clock_seconds;

    if (
        !audio_ctx ||
        audio_stream_index < 0 ||
        video_clock_seconds >
            active_clock
    ) {
        active_clock =
            video_clock_seconds;
    }

    double position =
        resume_base_offset_seconds +
        active_clock;

    if (position < 0.0) {
        position = 0.0;
    }

    return position;
}


static void prospero_playback_finished_update(
    void
) {
    if (
        screen != SCREEN_PLAYER ||
        player_paused ||
        prospero_scrub_active
    ) {
        prospero_finish_candidate_ms = 0;
        return;
    }

    if (!video_decode_done) {
        prospero_finish_candidate_ms = 0;
        return;
    }

    int video_packets =
        packet_queue_count(
            &video_packet_queue
        );

    int audio_packets =
        packet_queue_count(
            &audio_packet_queue
        );

    int queues_drained =
        video_packets <= 0 &&
        audio_packets <= 0 &&
        audio_queue_count <= 0 &&
        video_pending_pkt == NULL &&
        video_video_pending_pkt == NULL;

    if (!queues_drained) {
        prospero_finish_candidate_ms = 0;
        return;
    }

    double position =
        prospero_playback_current_position();

    int near_end =
        media_duration_sec <= 0.1 ||
        position >=
            media_duration_sec - 2.0;

    if (!near_end) {
        prospero_finish_candidate_ms = 0;
        return;
    }

    long long current_ms =
        now_ms();

    if (
        prospero_finish_candidate_ms == 0
    ) {
        prospero_finish_candidate_ms =
            current_ms;

        return;
    }

    /*
     * Let the last decoded frame and remaining audio settle.
     */
    if (
        current_ms -
        prospero_finish_candidate_ms <
        750
    ) {
        return;
    }

    prospero_playback_finished = 1;
    player_paused = 1;
    screen = SCREEN_PLAYBACK_FINISHED;

    controls_last_used_ms =
        current_ms;

#if PP_BACKEND_ENABLED
    /*
     * Finished/menu UI is laid out for 1920x1080. If we stay on a 4K VO
     * the card is drawn only in the top-left (looks "smashed") and can
     * appear doubled with leftover video. Restore 1080 before next paint.
     */
    g_4k_diag_active = 0;
    g_4k_suppress_audio = 0;
    g_4k_suppress_ui = 0;
    if (g_vo_w != 1920u || g_vo_h != 1080u || g_pending_vo_reconfig) {
        pp_product_request_vo(1920, 1080, 2, PP_BACKEND_1080_STANDARD,
                              WIDTH, HEIGHT);
        pp_stage_bc_checkpoint("015_FINISH_UI_1080", "restore VO for end screen");
    }
#endif
}


static int prospero_find_next_video(
    char *output,
    size_t output_size
) {
    if (
        !output ||
        output_size == 0 ||
        !current_media_path[0]
    ) {
        return 0;
    }

    output[0] = 0;

    const char *slash =
        strrchr(
            current_media_path,
            '/'
        );

    if (!slash) {
        return 0;
    }

    size_t directory_length =
        (size_t)(
            slash -
            current_media_path
        );

    if (
        directory_length == 0 ||
        directory_length >= 500
    ) {
        return 0;
    }

    char directory[512];
    char current_name[256];

    memcpy(
        directory,
        current_media_path,
        directory_length
    );

    directory[directory_length] = 0;

    snprintf(
        current_name,
        sizeof(current_name),
        "%s",
        slash + 1
    );

    DIR *folder =
        opendir(directory);

    if (!folder) {
        return 0;
    }

    char first_video[256] = {0};
    char next_video[256] = {0};

    int found_current = 0;

    struct dirent *entry;

    while (
        (
            entry =
                readdir(folder)
        ) != NULL
    ) {
        const char *name =
            entry->d_name;

        if (
            !name ||
            !name[0] ||
            name[0] == '.'
        ) {
            continue;
        }

        if (
            !strcmp(
                name,
                "$RECYCLE.BIN"
            ) ||
            !strcmp(
                name,
                "System Volume Information"
            )
        ) {
            continue;
        }

        if (
            !prospero_usb_entry_is_media(
                name
            )
        ) {
            continue;
        }

        if (!first_video[0]) {
            snprintf(
                first_video,
                sizeof(first_video),
                "%s",
                name
            );
        }

        if (found_current) {
            snprintf(
                next_video,
                sizeof(next_video),
                "%s",
                name
            );

            break;
        }

        if (
            strcmp(
                name,
                current_name
            ) == 0
        ) {
            found_current = 1;
        }
    }

    closedir(folder);

    /*
     * Wrap around to the first file after the last video.
     */
    if (
        !next_video[0] &&
        first_video[0] &&
        strcmp(
            first_video,
            current_name
        ) != 0
    ) {
        snprintf(
            next_video,
            sizeof(next_video),
            "%s",
            first_video
        );
    }

    if (!next_video[0]) {
        return 0;
    }

    snprintf(
        output,
        output_size,
        "%s/%s",
        directory,
        next_video
    );

    return 1;
}


static int prospero_start_media_from_beginning(
    const char *path
) {
    if (!path || !path[0]) {
        return 0;
    }

    char safe_path[512];

    snprintf(
        safe_path,
        sizeof(safe_path),
        "%s",
        path
    );

    requested_resume_seek_pos = 0.0;
    resume_base_offset_seconds = 0.0;

    remove(RESUME_FILE);

    snprintf(
        current_media_path,
        sizeof(current_media_path),
        "%s",
        safe_path
    );

    prospero_playback_finished = 0;
    prospero_finish_candidate_ms = 0;

    player_paused = 0;
    screen = SCREEN_PLAYER;

    if (
        !start_video_playback(
            safe_path
        )
    ) {
        screen = SCREEN_USB_BROWSER;
        load_usb_files();

        return 0;
    }

    return 1;
}


static void prospero_playback_finished_replay(
    void
) {
    char replay_path[512];

    snprintf(
        replay_path,
        sizeof(replay_path),
        "%s",
        current_media_path
    );

    if (
        !prospero_start_media_from_beginning(
            replay_path
        )
    ) {
        toast(
            "PLAYBACK",
            "REPLAY FAILED"
        );
    }
}


static void prospero_playback_finished_next(
    void
) {
    char next_path[512];

    if (
        !prospero_find_next_video(
            next_path,
            sizeof(next_path)
        )
    ) {
        toast(
            "NEXT VIDEO",
            "NO OTHER VIDEO"
        );

        return;
    }

    /*
     * Keep the USB browser pointed at the next video's folder.
     */
    const char *slash =
        strrchr(next_path, '/');

    if (slash) {
        size_t directory_length =
            (size_t)(
                slash -
                next_path
            );

        if (
            directory_length > 0 &&
            directory_length <
                sizeof(current_path)
        ) {
            memcpy(
                current_path,
                next_path,
                directory_length
            );

            current_path[
                directory_length
            ] = 0;
        }
    }

    if (
        !prospero_start_media_from_beginning(
            next_path
        )
    ) {
        toast(
            "NEXT VIDEO",
            "OPEN FAILED"
        );
    }
}


static void prospero_playback_finished_back(
    void
) {
    stop_video_playback();

    prospero_playback_finished = 0;
    prospero_finish_candidate_ms = 0;

    screen = SCREEN_USB_BROWSER;

    load_usb_files();
}


static void draw_playback_finished_screen(
    uint32_t *fb
) {
    /*
     * Preserve the final decoded frame behind the completion card.
     */
    for (
        int index = 0;
        index < WIDTH * HEIGHT;
        index++
    ) {
        fb[index] = 0xFF000000;
    }

    draw_video_frame_to_fb(
        fb,
        0,
        0,
        WIDTH,
        HEIGHT
    );

    rr_fill(
        fb,
        0,
        0,
        WIDTH,
        HEIGHT,
        RR_BGRA(8,8,8,185)
    );

    int card_x = 430;
    int card_y = 280;
    int card_w = 1060;
    int card_h = 500;

    ui_round_asset(
        fb,
        card_x,
        card_y,
        card_w,
        card_h,
        34,
        RR_BGRA(15,15,15,238),
        RR_BGRA(194,194,194,180),
        RR_BGRA(182,182,182,54)
    );

    rr_text(
        fb,
        card_x + 92,
        card_y + 65,
        "PLAYBACK COMPLETE",
        RR_BGRA(
            235,
            247,
            255,
            248
        ),
        3
    );

    char title[160];
    char metadata[160];

    title[0] = 0;
    metadata[0] = 0;

    clean_media_title(
        current_media_path,
        title,
        sizeof(title),
        metadata,
        sizeof(metadata)
    );

    if (strlen(title) > 48) {
        title[45] = '.';
        title[46] = '.';
        title[47] = '.';
        title[48] = 0;
    }

    rr_text(
        fb,
        card_x + 94,
        card_y + 145,
        title[0]
            ? title
            : "CURRENT VIDEO",
        RR_BGRA(205,205,205,235),
        2
    );

    rr_fill(
        fb,
        card_x + 92,
        card_y + 220,
        card_w - 184,
        1,
        RR_BGRA(174,174,174,90)
    );

    int controls_y =
        card_y + 300;

    rr_control(
        fb,
        card_x + 110,
        controls_y,
        0
    );

    rr_text(
        fb,
        card_x + 170,
        controls_y + 16,
        "REPLAY",
        RR_BGRA(243,243,243,235),
        1
    );

    rr_control(
        fb,
        card_x + 405,
        controls_y,
        5
    );

    rr_text(
        fb,
        card_x + 465,
        controls_y + 16,
        "NEXT VIDEO",
        RR_BGRA(243,243,243,235),
        1
    );

    rr_control(
        fb,
        card_x + 770,
        controls_y,
        4
    );

    rr_text(
        fb,
        card_x + 830,
        controls_y + 16,
        "BACK",
        RR_BGRA(243,243,243,235),
        1
    );

    rr_text(
        fb,
        card_x + 94,
        card_y + 425,
        "CHOOSE WHAT TO DO NEXT",
        RR_BGRA(201,201,201,210),
        0
    );
}

/* PROSPERO_PLAYBACK_COMPLETE_MODULE_END */


/* Early-boot diagnostics: append a line to /data/PSPlay15.log so a
 * silent startup failure can be located via FTP. Appended, never blocks UI. */
static void pp_boot_log(const char *step) {
    FILE *fp = fopen("/data/PSPlay15.log", "a");
    if (!fp) fp = fopen("/mnt/usb0/PSPlay15.log", "a");
    if (fp) {
        fprintf(fp, "[boot] %s\n", step);
        fclose(fp);
    }
}

int main(void) {
    pp_boot_log("main() entered");
    toast("PS Play", "Version 1.5 - DLNA + IPTV + CAST");
    pp_boot_log("toast sent");
    recent_load();
    favorites_load();
    prospero_settings_load();
    prospero_last_folder_load();
    pp_boot_log("settings loaded");
    ffmpeg_test();
    pp_boot_log("ffmpeg_test ok");

    sceUserServiceInitialize(NULL);
    scePadInit();

    int users[4] = {0};
    sceUserServiceGetLoginUserIdList(users);

    int pad = scePadOpen(users[0], 0, 0, NULL);
    pp_boot_log("pad opened");

    if (pad < 0)
        toast("PAD OPEN FAIL", "Could not open controller");
    else
        toast("PAD OPEN OK", "Controller connected");

    PS5_PadData padData;
    uint32_t lastButtons = 0;
    int selected = 0;
    bool running = true;

#if PP_BACKEND_ENABLED
    memset(&g_pp_vo, 0, sizeof(g_pp_vo));
    pp_playback_init(&g_pp_pb);
    pp_boot_log("before pp_videoout_init");
    /*
     * BigApp context: the shell may still be transitioning when our ELF
     * starts — retry init a few times before giving up.
     */
    int vo_ok = 0;
    int vo_step = 0;
    int vo_rc = 0;
    for (int vo_attempt = 0; vo_attempt < 6 && !vo_ok; vo_attempt++) {
        if (vo_attempt > 0)
            usleep(500000);
        if (pp_videoout_init(&g_pp_vo, WIDTH, HEIGHT, PP_PIXEL_BGRA32_TILED, 2) == 0) {
            vo_ok = 1;
        } else {
            char vo_line[128];
            vo_step = pp_videoout_last_step;
            vo_rc = pp_videoout_last_rc;
            snprintf(vo_line, sizeof(vo_line),
                     "videoout attempt %d failed step=%d rc=0x%08x",
                     vo_attempt + 1, vo_step, (unsigned)vo_rc);
            pp_boot_log(vo_line);
        }
    }
    if (!vo_ok) {
        char vo_msg[160];
        snprintf(vo_msg, sizeof(vo_msg),
                 "pp_videoout_init failed\nstep %d rc 0x%08x",
                 vo_step, (unsigned)vo_rc);
        toast("VIDEOOUT", vo_msg);
        pp_boot_log("FATAL: pp_videoout_init failed");
        return 1;
    }
    if (pp_videoout_split_fallback) {
        toast("VIDEOOUT", "split-alloc fallback active");
        pp_boot_log("videoout ready via split-alloc fallback");
    } else {
        pp_boot_log("videoout ready");
    }
    g_pp_vo_ready = 1;
    pp_playback_attach_videoout(&g_pp_pb, &g_pp_vo);

    /* DLNA renderer (cast target): console becomes discoverable at once */
    if (pp_dmr_start("PS Play") == 0) {
        g_dmr_started = 1;
        pp_boot_log("dlna renderer active");
        toast("DLNA RECEIVER", "Console is castable - find it in BubbleUPnP");
    } else {
        pp_boot_log("dlna renderer start failed");
    }
    pp_playback_set_output(&g_pp_pb, WIDTH, HEIGHT, PP_ASPECT_FIT);
    uint32_t *linear = NULL;
    uint32_t pp_buf_idx = 0;
    uint32_t pp_pitch = 0;
#else
    int handle = sceVideoOutOpen(0xff, 0, 0, NULL);
    size_t memsize = 0x2000000;
    intptr_t paddr = 0;
    void* vaddr = NULL;

    sceKernelAllocateMainDirectMemory(memsize, 0x20000, 3, &paddr);
    sceKernelMapDirectMemory(&vaddr, memsize, 0x33, 0, paddr, 0x20000);

    PS5_VideoBuf vbuf[2];
    memset(vbuf, 0, sizeof(vbuf));
    vbuf[0].data = vaddr;
    vbuf[1].data = (void*)((uintptr_t)vaddr + memsize / 2);

    uint32_t *linear = malloc(WIDTH * HEIGHT * 4);

    struct kevent* queue = NULL;
    sceKernelCreateEqueue(&queue, "menu flip queue");
    sceVideoOutAddFlipEvent(queue, handle, NULL);
    sceVideoOutSetFlipRate(handle, 0);

    PS5_VideoAttr attr;
    memset(&attr, 0, sizeof(attr));
    sceVideoOutSetBufferAttribute2(&attr, 0x8000000022000000UL, 0, WIDTH, HEIGHT, 0, 0, 0);
    sceVideoOutRegisterBuffers2(handle, 0, 0, vbuf, 2, &attr, 0, NULL);
#endif

    for (int frame = 0; running; frame++) {
        if (frame == 0) pp_boot_log("main loop entered");
        if (frame == 60) pp_boot_log("60 frames rendered - UI alive");
#if PP_BACKEND_ENABLED
        /* Safe point: no buffer held — apply deferred 4K/1080 VO reconfig */
        pp_product_apply_pending_vo();

        /*
         * HARD UI RULE: non-player screens only draw on a live 1080 VO.
         * Skip the frame until restore succeeds (never 1080 UI into 4K plane).
         */
        if (screen != SCREEN_PLAYER) {
            if (!pp_product_ensure_ui_1080()) {
                usleep(3000);
                continue;
            }
        } else if (!pp_product_k4_live(screen) &&
                   (g_vo_w != 1920u || g_vo_h != 1080u)) {
            if (!pp_product_ensure_ui_1080()) {
                usleep(3000);
                continue;
            }
        }

        int idx = 0;
        uint32_t v8_idx = 0;
        uint64_t v8_fid = 0;
        int v8_presented = 0;

        /*
         * V8 4K path only when live VO is actually 3840x2160 (k4_live).
         */
#if PP_4K_V8_PRODUCT_ENABLE
        int v8_hold = 0;
        if (pp_product_k4_live(screen) &&
            g_pp_backend == PP_BACKEND_4K_V8_FUSED) {
            if (pp_playback_take_pending_present(&g_pp_pb, &v8_idx, &v8_fid)) {
                if (pp_videoout_present_pre_tiled(&g_pp_vo, v8_idx, v8_fid) == 0) {
                    static int s_v8_first_present;
                    if (!s_v8_first_present) {
                        pp_stage_bc_checkpoint("012_FIRST_FRAME_PRESENTED",
                                              "v8 pre_tiled");
                        s_v8_first_present = 1;
                    }
                    v8_presented = 1;
                }
            } else {
                v8_hold = 1;
                linear = NULL;
            }
        }
#else
        int v8_hold = 0;
        (void)v8_idx;
        (void)v8_fid;
#endif

        if (!v8_presented && !v8_hold) {
            if (screen != SCREEN_PLAYER || !pp_product_k4_live(screen)) {
                if (g_vo_w != 1920u || g_vo_h != 1080u ||
                    !g_pp_vo_ready || g_pp_vo.width != 1920u ||
                    g_pp_vo.height != 1080u) {
                    usleep(2000);
                    continue;
                }
            }
            linear = (uint32_t *)pp_videoout_acquire(&g_pp_vo, &pp_buf_idx, &pp_pitch);
            if (!linear) {
                usleep(2000);
                continue;
            }
            memset(linear, 0, (size_t)g_vo_w * (size_t)g_vo_h * 4u);
        } else if (v8_presented) {
            linear = NULL;
        }
#else
        int idx = frame % 2;
#endif

        memset(&padData, 0, sizeof(padData));

        if (pad >= 0 && scePadReadState(pad, &padData) == 0) {
            uint32_t pressed = padData.buttons & ~lastButtons;
            uint32_t released = lastButtons & ~padData.buttons;
            prospero_scrub_hold_update(padData.buttons);
            int prompt_button_handled = 0;

            if (screen >= SCREEN_DLNA_SERVERS && screen <= SCREEN_DMR) {
                /* DLNA / IPTV / URL-entry screens handle their own input. */
                prospero_net_handle_input(pressed);
            } else {

            if (pressed && screen == 2 && !g_player_menu_open) {
                controls_last_used_ms = now_ms();
            }

            /*
             * Player controls (PS Play 1.5):
             *   X                play/pause
             *   LEFT / RIGHT     scrub through the timeline
             *   OPTIONS          playback settings dialog
             *   CIRCLE           back
             * Everything else (subtitles, audio track, view mode,
             * info, stats) lives inside the OPTIONS dialog, so a
             * careless touch on the pad cannot change playback.
             */
            if (screen == SCREEN_PLAYER && g_player_menu_open) {
                int pm_consumed =
                    PS5_PAD_BUTTON_CROSS |
                    PS5_PAD_BUTTON_CIRCLE |
                    PS5_PAD_BUTTON_OPTIONS |
                    PS5_PAD_BUTTON_UP |
                    PS5_PAD_BUTTON_DOWN |
                    PS5_PAD_BUTTON_LEFT |
                    PS5_PAD_BUTTON_RIGHT;

                if (pressed & PS5_PAD_BUTTON_UP) {
                    g_player_menu_sel =
                        (g_player_menu_sel + PP_PMENU_ITEM_COUNT - 1) %
                        PP_PMENU_ITEM_COUNT;
                } else if (pressed & PS5_PAD_BUTTON_DOWN) {
                    g_player_menu_sel =
                        (g_player_menu_sel + 1) % PP_PMENU_ITEM_COUNT;
                } else if (
                    (pressed & PS5_PAD_BUTTON_CIRCLE) ||
                    (pressed & PS5_PAD_BUTTON_OPTIONS)
                ) {
                    g_player_menu_open = 0;
                } else if (
                    (pressed & PS5_PAD_BUTTON_CROSS) ||
                    (pressed & PS5_PAD_BUTTON_LEFT) ||
                    (pressed & PS5_PAD_BUTTON_RIGHT)
                ) {
                    int pm_back =
                        (pressed & PS5_PAD_BUTTON_LEFT) ? 1 : 0;
                    prospero_player_menu_activate(pm_back);
                }

                pressed &= ~pm_consumed;
                released &= ~pm_consumed;
            } else if (pressed & PS5_PAD_BUTTON_OPTIONS && screen == 2) {
                if (!prospero_scrub_active) {
                    g_player_menu_open = 1;
                    g_player_menu_sel = 0;
                    controls_last_used_ms = now_ms();
                }
                pressed &= ~PS5_PAD_BUTTON_OPTIONS;
            }

            if (
                screen == 1 &&
                (pressed & PS5_PAD_BUTTON_TRIANGLE)
            ) {
                prospero_usb_toggle_selected_favorite();
            }

            if (
                screen == SCREEN_FAVORITES &&
                (pressed & PS5_PAD_BUTTON_TRIANGLE) &&
                favorite_count > 0
            ) {
                favorites_remove(
                    favorite_files[favorite_selected].path);
                favorites_save();
                toast("FAVORITES", "Removed");
            }

            if (pressed & PS5_PAD_BUTTON_DOWN) {
                if (screen == 0) selected = (selected + 1) % 7;
                else if (screen == SCREEN_SETTINGS) settings_selected = (settings_selected + 1) % 7;
                else if (screen == SCREEN_PROFILE_SELECT) profile_selected = (profile_selected + 1) % 4;
                else if (screen == SCREEN_RECENT_FILES && recent_file_count > 0) recent_selected = (recent_selected + 1) % recent_file_count;
                else if (screen == SCREEN_FAVORITES && favorite_count > 0) favorite_selected = (favorite_selected + 1) % favorite_count;
                else if (file_count > 0) file_selected = (file_selected + 1) % file_count;
            }

            if (pressed & PS5_PAD_BUTTON_UP) {
                if (screen == 0) selected = (selected + 6) % 7;
                else if (screen == SCREEN_SETTINGS) settings_selected = (settings_selected + 6) % 7;
                else if (screen == SCREEN_PROFILE_SELECT) profile_selected = (profile_selected + 3) % 4;
                else if (screen == SCREEN_RECENT_FILES && recent_file_count > 0) recent_selected = (recent_selected + recent_file_count - 1) % recent_file_count;
                else if (screen == SCREEN_FAVORITES && favorite_count > 0) favorite_selected = (favorite_selected + favorite_count - 1) % favorite_count;
                else if (file_count > 0) file_selected = (file_selected + file_count - 1) % file_count;
            }

            if (
                screen == SCREEN_PLAYER &&
                (pressed & PS5_PAD_BUTTON_LEFT)
            ) {
                prospero_scrub_move(-10.0);
            }

            if (
                screen == SCREEN_PLAYER &&
                (pressed & PS5_PAD_BUTTON_RIGHT)
            ) {
                prospero_scrub_move(10.0);
            }

            /* PS Play horizontal menu: left/right move between tiles */
            if (screen == 0 && (pressed & PS5_PAD_BUTTON_LEFT))
                selected = (selected + 6) % 7;

            if (screen == 0 && (pressed & PS5_PAD_BUTTON_RIGHT))
                selected = (selected + 1) % 7;

            if (
                screen == SCREEN_PLAYER &&
                prospero_scrub_active &&
                (pressed & PS5_PAD_BUTTON_CROSS)
            ) {
                prospero_scrub_confirm();
                prompt_button_handled = 1;
            }

            if (
                screen == SCREEN_PLAYER &&
                prospero_scrub_active &&
                (pressed & PS5_PAD_BUTTON_CIRCLE)
            ) {
                prospero_scrub_cancel();
                prompt_button_handled = 1;
            }

            if (
                screen == SCREEN_PLAYBACK_FINISHED &&
                (pressed & PS5_PAD_BUTTON_CROSS)
            ) {
                prospero_playback_finished_replay();
                prompt_button_handled = 1;
            }

            if (
                screen == SCREEN_PLAYBACK_FINISHED &&
                (pressed & PS5_PAD_BUTTON_TRIANGLE)
            ) {
                prospero_playback_finished_next();
                prompt_button_handled = 1;
            }

            if (
                screen == SCREEN_PLAYBACK_FINISHED &&
                (pressed & PS5_PAD_BUTTON_CIRCLE)
            ) {
                prospero_playback_finished_back();
                prompt_button_handled = 1;
            }

            if (screen == 4 && (pressed & PS5_PAD_BUTTON_CROSS)) {
                resume_prompt_active = 0;
                requested_resume_seek_pos = pending_resume_pos;
                resume_base_offset_seconds = pending_resume_pos;
                screen = 2;
                start_video_playback(pending_resume_path);
                toast("RESUME", "Resuming");
                prompt_button_handled = 1;
            } else if (screen == 4 && (pressed & PS5_PAD_BUTTON_CIRCLE)) {
                resume_prompt_active = 0;
                pending_resume_pos = 0.0;
                requested_resume_seek_pos = 0.0;
                resume_base_offset_seconds = 0.0;
                remove(RESUME_FILE);
                screen = 2;
                start_video_playback(pending_resume_path);
                toast("RESUME", "Starting over");
                prompt_button_handled = 1;
            } else if (
                !prompt_button_handled &&
                (pressed & PS5_PAD_BUTTON_CROSS)
            ) {
                if (screen == SCREEN_SETTINGS) {
                    prospero_settings_activate_selected();
                } else if (screen == SCREEN_PROFILE_SELECT) {
                    current_profile =
                        (PlaybackProfile)profile_selected;

                    prospero_settings_save();

                    toast(
                        "PROFILE",
                        profile_name(current_profile)
                    );

                    screen = SCREEN_SETTINGS;
                } else if (screen == SCREEN_RECENT_FILES) {
                    if (recent_file_count > 0) {
                        snprintf(current_media_path, sizeof(current_media_path), "%s", recent_files[recent_selected].path);
                        requested_resume_seek_pos = recent_files[recent_selected].last_pos;
                        resume_base_offset_seconds = recent_files[recent_selected].last_pos;
                        screen = 2;
                        start_video_playback(current_media_path);
                    }
                } else if (screen == SCREEN_FAVORITES) {
                    if (favorite_count > 0) {
                        snprintf(current_media_path, sizeof(current_media_path), "%s", favorite_files[favorite_selected].path);
                        requested_resume_seek_pos = 0.0;
                        resume_base_offset_seconds = 0.0;
                        screen = 2;
                        start_video_playback(current_media_path);
                    }
                } else if (screen == SCREEN_DEVELOPER_TOOLS) {
                    write_compatibility_report();
                } else if (screen == 2) {
                    player_paused = !player_paused;
#if PP_BACKEND_ENABLED
                    if (player_paused)
                        pp_playback_pause(&g_pp_pb);
                    else
                        pp_playback_resume(&g_pp_pb);
#endif
                    controls_last_used_ms = now_ms();
                    toast("PLAYBACK", player_paused ? "Paused" : "Playing");
                } else if (screen == 1) {
                    enter_selected_usb();
                } else if (selected == 0) {
                    screen = SCREEN_DMR;
                } else if (selected == 1) {
                    prospero_net_enter_dlna();
                    screen = SCREEN_DLNA_SERVERS;
                } else if (selected == 2) {
                    prospero_net_enter_iptv();
                    screen = SCREEN_IPTV;
                } else if (selected == 3) {
                    load_usb_files();
                    screen = SCREEN_USB_BROWSER;
                } else if (selected == 4) {
                    screen = SCREEN_RECENT_FILES;
                } else if (selected == 5) {
                    screen = SCREEN_SETTINGS;
                    settings_selected = 0;
                } else if (selected == 6) {
                    screen = SCREEN_ABOUT_SUPPORT;
                }
            }

            if (!prompt_button_handled && (pressed & PS5_PAD_BUTTON_CIRCLE) && screen != 4) {
                if (screen == SCREEN_PROFILE_SELECT) {
                    screen = SCREEN_SETTINGS;
                } else if (screen == SCREEN_SETTINGS) {
                    screen = 0;
                } else if (screen == SCREEN_RECENT_FILES) {
                    screen = 0;
                } else if (screen == SCREEN_FAVORITES) {
                    screen = 0;
                } else if (screen == SCREEN_ABOUT_SUPPORT) {
                    screen = 0;
                } else if (screen == SCREEN_DEVELOPER_TOOLS) {
                    /* opened from Settings now — return there */
                    screen = SCREEN_SETTINGS;
                } else if (screen == SCREEN_MEDIA_INFO) {
                    screen = 2;
                } else if (screen == 3) {
                    screen = 1;
                } else if (screen == 2) {
                    recent_update_current_position();
                    stop_video_playback();
                    screen = 1;
                } else if (screen == 1) {
                    usb_go_back();
                } else {
                    toast("EXIT", "Use PS button to close for now");
                }
            }

            } /* end legacy (non-network) screens */

            lastButtons = padData.buttons;
        }

        /* DLNA renderer command bridge (cast play/pause/stop/seek) */
        prospero_dmr_pump();

        if (screen == 2 && !player_paused) {
            player_elapsed++;
        }

        prospero_playback_finished_update();

#if PP_BACKEND_ENABLED
        /*
         * After stop / leave-player while VO is still 4K: do not draw 1080 UI
         * into the 4K plane (one-frame stacked flash). Drop the buffer; next
         * frame top-of-loop applies 1080 and redraws cleanly.
         */
        if (linear && screen != SCREEN_PLAYER &&
            (g_vo_w != 1920u || g_vo_h != 1080u || g_pending_vo_reconfig)) {
            pp_videoout_release(&g_pp_vo, pp_buf_idx);
            linear = NULL;
            v8_hold = 1;
        }
#endif

        /* V8 direct present: video already on screen; skip UI composite this frame. */
        if (linear) {
        if (screen == 2 && pp_product_k4_live(screen)) {
            /*
             * Native UHD VO: draw_player_screen is 1080-only and would only
             * paint the top-left of a 4K buffer (choppy/wrong). Copy full VO
             * plane from the convert path (V3 letterbox or full).
             */
            if (g_pp_pb.active && pp_playback_has_display(&g_pp_pb)) {
                (void)pp_playback_copy_display(
                    &g_pp_pb, linear, g_vo_w * 4u, g_vo_w, g_vo_h);
            }
            {
                static int s_noui_bc;
                if (!s_noui_bc) {
                    pp_stage_bc_checkpoint("012B_UHD_FULL_PLANE_PRESENT",
                                          pp_4k_stage_name(PP_4K_PRODUCT_STAGE));
                    s_noui_bc = 1;
                }
            }
        } else if (screen == 2 && g_4k_diag_active && g_4k_suppress_ui) {
            if (g_pp_pb.active && pp_playback_has_display(&g_pp_pb)) {
                (void)pp_playback_copy_display(
                    &g_pp_pb, linear, g_vo_w * 4u, g_vo_w, g_vo_h);
            }
        } else if (screen == 0)
            draw_menu_linear(linear, selected);
        else if (screen == 1)
            draw_usb_browser(linear);
        else if (screen == 2)
            draw_player_screen(linear);
        else if (screen == 3)
            draw_image_screen(linear);
        else if (screen == 4)
            draw_resume_prompt(linear);
        else if (screen == SCREEN_SETTINGS)
            draw_settings_screen(linear);
        else if (screen == SCREEN_PROFILE_SELECT)
            draw_profile_screen(linear);
        else if (screen == SCREEN_RECENT_FILES)
            draw_recent_files_screen(linear);
        else if (screen == SCREEN_FAVORITES)
            draw_favorites_screen(linear);
        else if (screen == SCREEN_ABOUT_SUPPORT)
            draw_about_support_screen(linear);
        else if (screen == SCREEN_DEVELOPER_TOOLS)
            draw_developer_tools_screen(linear);
        else if (screen == SCREEN_DLNA_SERVERS)
            draw_dlna_servers_screen(linear);
        else if (screen == SCREEN_DLNA_BROWSE)
            draw_dlna_browse_screen(linear);
        else if (screen == SCREEN_IPTV)
            draw_iptv_screen(linear);
        else if (screen == SCREEN_URL_ENTRY)
            draw_url_entry_screen(linear);
        else if (screen == SCREEN_DMR)
            draw_dmr_screen(linear);
        else if (screen == SCREEN_MEDIA_INFO)
            draw_media_info_screen(linear);
        else if (screen == SCREEN_PLAYBACK_FINISHED) {
            /* Wait one frame if 1080 restore still pending */
            if (g_vo_w == 1920u && g_vo_h == 1080u)
                draw_playback_finished_screen(linear);
            else {
                /* fill black until VO is 1080 */
                size_t n = (size_t)g_vo_w * (size_t)g_vo_h;
                for (size_t i = 0; i < n; i++)
                    linear[i] = 0xFF000000u;
            }
        }
        else
            draw_menu_linear(linear, selected);
        /* OPTIONS playback settings dialog over the player frame */
        if (screen == 2)
            prospero_draw_player_menu(linear);
        /* Toasts only when UI allowed (or menus); stage 1-3 video-only skips toast draw */
        if (!(screen == 2 && g_4k_diag_active && g_4k_suppress_ui))
            draw_prospero_toast(linear);
        } /* if (linear) */

#if PP_BACKEND_ENABLED
        (void)idx;
        if (!v8_presented && !v8_hold) {
            if (pp_videoout_present(&g_pp_vo, pp_buf_idx, (uint64_t)frame) != 0) {
                /* present failed; buffer already released by backend */
            }
        } else if (v8_hold) {
            /* Light sleep so we don't spin at kHz while holding last V8 frame */
            usleep(2000);
        }
#else
        PS5_DrawPixelsAsTiles(linear, (uint32_t*)vbuf[idx].data, WIDTH, HEIGHT);

        sceVideoOutSubmitFlip(handle, idx, 1, frame);
#endif

        perf_render_frames++;
        {
            long long pm = perf_now_ms();
            if (perf_last_ms == 0)
                perf_last_ms = pm;
            if (pm - perf_last_ms >= 1000) {
                perf_render_fps = perf_render_frames;
                perf_decode_fps = perf_decode_frames;
                perf_render_frames = 0;
                perf_decode_frames = 0;
                perf_last_ms = pm;
            }
        }
        /*
         * Old path slept ~2ms every frame even during 4K play (choppy).
         * Player: yield briefly. Menus: slightly longer to cut CPU spin.
         */
        if (screen == SCREEN_PLAYER)
            usleep(500);
        else
            usleep(2000);
    }

#if PP_BACKEND_ENABLED
    if (g_dmr_started) {
        pp_dmr_stop();
        g_dmr_started = 0;
    }
    stop_video_playback();
    /* Restore 1080 VO for clean exit if still on 4K */
    if (g_vo_w != 1920u || g_vo_h != 1080u)
        (void)pp_product_reconfigure_vo(1920, 1080, 2);
    pp_playback_shutdown(&g_pp_pb);
    if (g_pp_vo_ready)
        pp_videoout_shutdown(&g_pp_vo);
#endif

    return 0;
}
