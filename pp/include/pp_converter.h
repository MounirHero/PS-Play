/*
 * pp_converter — scale + color convert into a display buffer.
 * Does NOT open VideoOut or flip. Swappable for AGC/compositor later.
 */
#ifndef PP_CONVERTER_H
#define PP_CONVERTER_H

#include "pp_frame.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum pp_aspect_mode {
    PP_ASPECT_FIT = 0,     /* letterbox / pillarbox, preserve AR */
    PP_ASPECT_FILL = 1,    /* crop source to fill, preserve AR */
    PP_ASPECT_STRETCH = 2  /* ignore AR */
} pp_aspect_mode;

typedef struct pp_converter_config {
    pp_aspect_mode aspect;
    uint32_t clear_color_bgra; /* letterbox bars; default black */
} pp_converter_config;

/**
 * Convert source frame into destination BGRA32 linear framebuffer.
 * destination_pitch = bytes per row (typically destination_width * 4).
 * Returns 0 on success.
 */
int pp_converter_convert(const pp_frame *source,
                         void *destination,
                         uint32_t destination_width,
                         uint32_t destination_height,
                         uint32_t destination_pitch);

/** Same with explicit aspect / clear color. */
int pp_converter_convert_ex(const pp_frame *source,
                            void *destination,
                            uint32_t destination_width,
                            uint32_t destination_height,
                            uint32_t destination_pitch,
                            const pp_converter_config *cfg);

#ifdef __cplusplus
}
#endif

#endif /* PP_CONVERTER_H */
