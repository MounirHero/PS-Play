/*
 * pp_iptv.h — IPTV channel lists for ProsperoPlayer.
 *
 * Loads .m3u / .m3u8 playlists from USB storage (root and /IPTV folder)
 * and merges them with a small set of built-in demo channels. Every
 * channel is played through ffmpeg's network protocols (http/https/HLS).
 */
#ifndef PP_IPTV_H
#define PP_IPTV_H

#include <stddef.h>

#define PP_IPTV_MAX_NAME  160
#define PP_IPTV_MAX_URL   768
#define PP_IPTV_MAX_GROUP 96

typedef struct {
    char name[PP_IPTV_MAX_NAME];
    char url[PP_IPTV_MAX_URL];
    char group[PP_IPTV_MAX_GROUP];
    char source[64];      /* playlist file or "BUILT-IN" */
} pp_iptv_channel;

/*
 * Scan USB storage for playlists and load all channels.
 * Returns channel count (>= 0). Built-in demo channels are always
 * appended after playlist entries.
 */
int pp_iptv_load(pp_iptv_channel *channels, int max_channels);

/* Parse a single M3U file, appending to channels[]. Returns added count. */
int pp_iptv_parse_file(const char *path, const char *source_label,
                       pp_iptv_channel *channels, int max_channels,
                       int already_loaded);

#endif
