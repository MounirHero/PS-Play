/*
 * Neutral decoded-frame structure — no FFmpeg types.
 * FFmpeg (or any decoder) adapts *into* this shape; videoout never sees AVFrame.
 */
#ifndef PP_FRAME_H
#define PP_FRAME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum pp_frame_format {
    PP_FRAME_NV12 = 0,
    PP_FRAME_YUV420P = 1,
    PP_FRAME_RGBA = 2,
    PP_FRAME_BGRA = 3
} pp_frame_format;

typedef struct pp_frame {
    pp_frame_format format;
    uint32_t width;
    uint32_t height;
    const uint8_t *planes[4];
    int strides[4];
    int64_t pts_us;
} pp_frame;

#ifdef __cplusplus
}
#endif

#endif /* PP_FRAME_H */
