#ifndef PROSPERO_METADATA_H
#define PROSPERO_METADATA_H

#include <stddef.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

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

void clear_media_metadata(MediaMetadata *m);
const char* safe_codec_name(enum AVCodecID id);
void load_media_metadata_basic(const char *path, MediaMetadata *m);
void load_media_metadata_from_format(AVFormatContext *fmt, const char *path, MediaMetadata *m);
void format_file_size(char *out, size_t outsz, long long bytes);

#endif
