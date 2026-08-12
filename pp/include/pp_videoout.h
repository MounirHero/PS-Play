/*
 * pp_videoout — ProsperoPlayer presentation backend (Workstream A / product path).
 *
 * PROVEN stack (Probe 002):
 *   sceVideoOutOpen
 *   → AllocateMainDirectMemory + MapDirectMemory
 *   → SetBufferAttribute2 (tiled BGRA) + RegisterBuffers2
 *   → CPU write linear staging
 *   → tile into registered buffer
 *   → SubmitFlip
 *   → wait / reuse after safe
 *
 * No GNM. No AGC. No decoder/convert logic here.
 *
 * Media RE conclusion (003Q): stock player is service-mediated
 * (VideoCore → CompositorShmClient → CompositorVideo → VideoOut).
 * Ship path for ProsperoPlayer remains *this* direct VideoOut backend.
 * FUTURE_TARGET: CompositorShmClient descriptor/handoff format.
 */
#ifndef PP_VIDEOOUT_H
#define PP_VIDEOOUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PP_VO_MAX_BUFFERS
#define PP_VO_MAX_BUFFERS 3
#endif

/* Display pixel formats understood by this backend */
typedef enum pp_pixel_format {
    PP_PIXEL_BGRA32_TILED = 0, /* Probe 002 / ProsperoPlayer known-good */
    PP_PIXEL_RGBA32_TILED = 1  /* same path; channel order is app-side convert */
} pp_pixel_format;

typedef struct pp_videoout_stats {
    uint64_t acquires;
    uint64_t presents;
    uint64_t flips_ok;
    uint64_t flips_fail;
    uint64_t waits;
    uint64_t wait_timeouts;
    uint64_t reuse_blocked; /* acquire had to wait for in-flight */
} pp_videoout_stats;

/*
 * Public state object — zero-initialize before pp_videoout_init.
 * Callers own the struct storage; backend owns GPU/VideoOut resources.
 */
typedef struct pp_videoout {
    int handle;
    void *equeue;
    intptr_t paddr;
    void *vaddr;           /* mapped direct memory base */
    intptr_t paddr_split[PP_VO_MAX_BUFFERS]; /* per-buffer fallback blocks */
    void *vaddr_split[PP_VO_MAX_BUFFERS];
    int split_alloc;       /* 1 = per-buffer direct memory (fallback path) */
    size_t memsize;
    size_t plane_bytes;    /* bytes per registered buffer */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;        /* CPU staging pitch in bytes (width*4) */
    uint32_t buffer_count;
    pp_pixel_format format;
    int flip_rate;

    void *gpu_bufs[PP_VO_MAX_BUFFERS];   /* registered tiled planes */
    uint32_t *cpu_bufs[PP_VO_MAX_BUFFERS]; /* linear staging (CPU) */
    size_t cpu_bytes;

    uint8_t state[PP_VO_MAX_BUFFERS]; /* 0 free, 1 acquired, 2 in_flight */
    uint64_t submit_tsc[PP_VO_MAX_BUFFERS];
    uint64_t submit_frame[PP_VO_MAX_BUFFERS];
    uint32_t next_index;

    pp_videoout_stats stats;
    int inited;
    int registered;
} pp_videoout;

/**
 * Open VideoOut, allocate one direct-memory block, map, split into buffer_count
 * planes, RegisterBuffers2 once. Default for ProsperoPlayer TV path: 1280x720, 3 buffers.
 * Returns 0 on success, negative on failure (safe to call shutdown after partial init).
 */
int pp_videoout_init(pp_videoout *vo,
                     uint32_t width,
                     uint32_t height,
                     pp_pixel_format format,
                     uint32_t buffer_count);

/**
 * Acquire a free CPU-writable linear buffer.
 * *buffer_index = slot, *pitch = row pitch in bytes, return = pointer to plane.
 * Blocks (wait_available) if all buffers are in-flight.
 * Returns NULL on failure.
 */
void *pp_videoout_acquire(pp_videoout *vo, uint32_t *buffer_index, uint32_t *pitch);

/** Return an ACQUIRED buffer without presenting (e.g. convert failure). */
void pp_videoout_release(pp_videoout *vo, uint32_t buffer_index);

/**
 * Tile/copy staging → registered buffer and SubmitFlip.
 * buffer_index must be currently ACQUIRED by this backend.
 * Returns 0 on success.
 */
int pp_videoout_present(pp_videoout *vo, uint32_t buffer_index, uint64_t frame_id);

/**
 * SubmitFlip only — GPU plane already holds final tiled BGRA (V8 fused path).
 */
int pp_videoout_present_pre_tiled(pp_videoout *vo, uint32_t buffer_index, uint64_t frame_id);

/** Tiled GPU plane for acquired buffer (V8 write target). */
void *pp_videoout_gpu_plane(pp_videoout *vo, uint32_t buffer_index);

/**
 * Wait until buffer_index is safe to reuse (not in-flight), or timeout.
 * timeout_ms = 0 → poll once; UINT32_MAX → long wait.
 * Returns 0 if available, -1 on timeout/error.
 */
int pp_videoout_wait_available(pp_videoout *vo, uint32_t buffer_index, uint32_t timeout_ms);

/** Release VideoOut resources. Safe on zeroed or partial vo. */
void pp_videoout_shutdown(pp_videoout *vo);

void pp_videoout_get_stats(const pp_videoout *vo, pp_videoout_stats *out);

/* Optional: reopen with new size (shutdown + init into same struct). */
int pp_videoout_reconfigure(pp_videoout *vo,
                            uint32_t width,
                            uint32_t height,
                            pp_pixel_format format,
                            uint32_t buffer_count);

/* Diagnostics after failed init (step codes: 10=open,11=alloc,12=map,13=equeue,14=register,15=malloc,16=split-alloc). */
extern int pp_videoout_last_step;
extern int pp_videoout_last_rc;
/* 1 when init succeeded only via the per-buffer split-allocation fallback. */
extern int pp_videoout_split_fallback;

#ifdef __cplusplus
}
#endif

#endif /* PP_VIDEOOUT_H */
