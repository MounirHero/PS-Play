/*
 * pp_dlna.h — DLNA/UPnP client for ProsperoPlayer.
 *
 * SSDP discovery of MediaServer devices, device description parsing and
 * ContentDirectory SOAP browsing. Ported (in C) from ps5-payload-dev/dlnaplay
 * (GPL-3.0-or-later, by John Törnblom / claude.ai).
 *
 * All functions perform blocking network I/O: call from a worker thread.
 */
#ifndef PP_DLNA_H
#define PP_DLNA_H

#include <stddef.h>
#include <stdint.h>

#define PP_DLNA_MAX_NAME   160
#define PP_DLNA_MAX_URL    768
#define PP_DLNA_MAX_ID     320
#define PP_DLNA_MAX_CLASS  96

typedef struct {
    char friendly_name[PP_DLNA_MAX_NAME];
    char model[PP_DLNA_MAX_NAME];       /* "<manufacturer> <modelName>" */
    char udn[PP_DLNA_MAX_ID];
    char location[PP_DLNA_MAX_URL];     /* device description URL */
    char control_url[PP_DLNA_MAX_URL];  /* absolute ContentDirectory URL */
    char host[64];                      /* "192.168.x.x" for list display */
} pp_dlna_server;

typedef struct {
    char id[PP_DLNA_MAX_ID];
    char parent_id[PP_DLNA_MAX_ID];
    char title[256];
    char upnp_class[PP_DLNA_MAX_CLASS];
    int  is_container;
    int  child_count;        /* containers only, -1 unknown */
    char artist[160];
    char album[160];
    char res_url[PP_DLNA_MAX_URL];
    char duration[24];       /* raw "H:MM:SS.mmm" or "" */
    char resolution[24];     /* "1920x1080" or "" */
    long long size_bytes;    /* -1 unknown */
} pp_dlna_object;

/*
 * Discover DLNA media servers on the LAN via SSDP M-SEARCH.
 * Waits up to timeout_ms for responses, fetches each device description
 * and fills servers[]. Returns count (>=0) or -1 on fatal error.
 */
int pp_dlna_discover(pp_dlna_server *servers, int max_servers,
                     int timeout_ms, char *err, size_t errsz);

/*
 * Browse a server's ContentDirectory (BrowseDirectChildren).
 * object_id "0" is the root. Returns count or -1 (err filled).
 */
int pp_dlna_browse(const pp_dlna_server *server, const char *object_id,
                   pp_dlna_object *items, int max_items,
                   char *err, size_t errsz);

int pp_dlna_object_is_video(const pp_dlna_object *o);
int pp_dlna_object_is_audio(const pp_dlna_object *o);
int pp_dlna_object_is_image(const pp_dlna_object *o);

#endif
