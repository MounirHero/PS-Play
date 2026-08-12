#include "metadata.h"

#include <stdio.h>
#include <string.h>

extern void clean_media_title(const char *path, char *title, int title_sz, char *meta, int meta_sz);

void clear_media_metadata(MediaMetadata *m) {
    if (!m) return;
    memset(m, 0, sizeof(MediaMetadata));
    snprintf(m->container, sizeof(m->container), "Unknown");
    snprintf(m->video_codec, sizeof(m->video_codec), "Unknown");
    snprintf(m->audio_codec, sizeof(m->audio_codec), "Unknown");
}

const char* safe_codec_name(enum AVCodecID id) {
    const char *name = avcodec_get_name(id);
    return name ? name : "Unknown";
}

void load_media_metadata_basic(const char *path, MediaMetadata *m) {
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

void load_media_metadata_from_format(AVFormatContext *fmt, const char *path, MediaMetadata *m) {
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

void format_file_size(char *out, size_t outsz, long long bytes) {
    if (!out || outsz == 0) return;

    double v = (double)bytes;
    const char *unit = "B";

    if (v >= 1024.0) { v /= 1024.0; unit = "KB"; }
    if (v >= 1024.0) { v /= 1024.0; unit = "MB"; }
    if (v >= 1024.0) { v /= 1024.0; unit = "GB"; }

    snprintf(out, outsz, "%.2f %s", v, unit);
}
