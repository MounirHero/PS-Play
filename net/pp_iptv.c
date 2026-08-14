/*
 * pp_iptv.c — M3U/M3U8 playlist parser and USB scanner.
 */
#include "pp_iptv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

/* Built-in demo channels (public test streams, HLS). */
static const struct { const char *name; const char *url; const char *group; }
BUILTIN_CHANNELS[] = {
    { "Big Buck Bunny (HLS Test)",
      "https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8", "DEMO" },
    { "Sintel (HLS Test)",
      "https://bitdash-a.akamaihd.net/content/sintel/hls/playlist.m3u8", "DEMO" },
    { "Apple BipBop (HLS Test)",
      "https://devstreaming-cdn.apple.com/videos/streaming/examples/img_bipbop_adv_example_fmp4/master.m3u8", "DEMO" },
    { "NASA TV Public",
      "https://ntv1.akamaized.net/hls/live/2014075/NASA-NTV1-HLS/master.m3u8", "LIVE" },
};

static void trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' ||
                     s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = 0;
}

static int has_playlist_ext(const char *name) {
    size_t n = strlen(name);
    if (n >= 4 && strcasecmp(name + n - 4, ".m3u") == 0) return 1;
    if (n >= 5 && strcasecmp(name + n - 5, ".m3u8") == 0) return 1;
    return 0;
}

/* Extract value of key="value" from an #EXTINF line. */
static const char *pp_strcasestr(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (!nl) return hay;
    for (const char *p = hay; *p; p++) {
        if (strncasecmp(p, needle, nl) == 0) return p;
    }
    return NULL;
}

static void extinf_attr(const char *line, const char *key,
                        char *out, size_t out_sz) {
    out[0] = 0;
    char pat[64];
    snprintf(pat, sizeof(pat), "%s=\"", key);
    const char *p = pp_strcasestr(line, pat);
    if (!p) return;
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if (!e) return;
    size_t n = (size_t)(e - p);
    if (n >= out_sz) n = out_sz - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

int pp_iptv_parse_file(const char *path, const char *source_label,
                       pp_iptv_channel *channels, int max_channels,
                       int already_loaded) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    int count = already_loaded;
    char line[2048];
    char pending_name[PP_IPTV_MAX_NAME] = {0};
    char pending_group[PP_IPTV_MAX_GROUP] = {0};

    while (fgets(line, sizeof(line), fp) && count < max_channels) {
        trim(line);
        if (!line[0]) continue;

        if (strncmp(line, "#EXTINF:", 8) == 0) {
            /* channel name after last comma */
            const char *comma = strrchr(line, ',');
            if (comma && comma[1]) {
                snprintf(pending_name, sizeof(pending_name), "%s", comma + 1);
                trim(pending_name);
            }
            extinf_attr(line, "group-title", pending_group,
                        sizeof(pending_group));
            if (!pending_group[0]) {
                char tvg[PP_IPTV_MAX_GROUP];
                extinf_attr(line, "tvg-group", tvg, sizeof(tvg));
                snprintf(pending_group, sizeof(pending_group), "%s", tvg);
            }
            continue;
        }
        if (line[0] == '#') continue;

        /* URL line */
        if (strncmp(line, "http://", 7) == 0 ||
            strncmp(line, "https://", 8) == 0 ||
            strncmp(line, "rtmp://", 7) == 0 ||
            strncmp(line, "rtmps://", 8) == 0 ||
            strncmp(line, "rtmpt://", 8) == 0 ||
            strncmp(line, "rtsp://", 7) == 0 ||
            strncmp(line, "rtp://", 6) == 0 ||
            strncmp(line, "mmsh://", 7) == 0 ||
            strncmp(line, "mmst://", 7) == 0 ||
            strncmp(line, "ftp://", 6) == 0 ||
            strncmp(line, "udp://", 6) == 0) {
            pp_iptv_channel *c = &channels[count];
            memset(c, 0, sizeof(*c));
            if (pending_name[0])
                snprintf(c->name, sizeof(c->name), "%s", pending_name);
            else {
                /* derive a name from the URL tail */
                const char *tail = strrchr(line, '/');
                snprintf(c->name, sizeof(c->name), "%s",
                         (tail && tail[1]) ? tail + 1 : line);
            }
            snprintf(c->url, sizeof(c->url), "%s", line);
            snprintf(c->group, sizeof(c->group), "%s",
                     pending_group[0] ? pending_group : "IPTV");
            snprintf(c->source, sizeof(c->source), "%s", source_label);
            count++;
            pending_name[0] = 0;
            pending_group[0] = 0;
        }
    }

    fclose(fp);
    return count - already_loaded;
}

/* Recursive scan: every subfolder of the given roots is searched. */
static void scan_dir(const char *dir, int depth, pp_iptv_channel *channels,
                     int max_channels, int *count) {
    if (depth > 4) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && *count < max_channels) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_dir(path, depth + 1, channels, max_channels, count);
        } else if (has_playlist_ext(e->d_name)) {
            char label[64];
            snprintf(label, sizeof(label), "%s", e->d_name);
            *count += pp_iptv_parse_file(path, label, channels,
                                         max_channels, *count);
        }
    }
    closedir(d);
}

int pp_iptv_load(pp_iptv_channel *channels, int max_channels) {
    int count = 0;

    /* /data/PS Play plus USB sticks, all subfolders included. */
    static const char *scan_dirs[] = {
        "/data/PS Play",
        "/mnt/usb0",
        "/mnt/usb1",
    };
    for (size_t i = 0; i < sizeof(scan_dirs) / sizeof(scan_dirs[0]); i++) {
        scan_dir(scan_dirs[i], 0, channels, max_channels, &count);
    }

    /* Built-in demo channels */
    for (size_t i = 0;
         i < sizeof(BUILTIN_CHANNELS) / sizeof(BUILTIN_CHANNELS[0]) &&
         count < max_channels;
         i++) {
        pp_iptv_channel *c = &channels[count];
        memset(c, 0, sizeof(*c));
        snprintf(c->name, sizeof(c->name), "%s", BUILTIN_CHANNELS[i].name);
        snprintf(c->url, sizeof(c->url), "%s", BUILTIN_CHANNELS[i].url);
        snprintf(c->group, sizeof(c->group), "%s", BUILTIN_CHANNELS[i].group);
        snprintf(c->source, sizeof(c->source), "BUILT-IN");
        count++;
    }

    return count;
}
