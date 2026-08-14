/*
 * pp_dmr — DLNA Digital Media Renderer ("cast target") for ProsperoPlayer.
 *
 * Turns the console into a UPnP MediaRenderer:1 device:
 *  - SSDP responder (M-SEARCH) + periodic NOTIFY ssdp:alive
 *  - HTTP server: device description, SCPDs, SOAP control, GENA eventing
 *  - AVTransport:1  — SetAVTransportURI / Play / Pause / Stop / Seek / info
 *  - RenderingControl:1 — volume / mute state
 *  - ConnectionManager:1 — protocolInfo
 *
 * SOAP actions arrive on pp_dmr's own threads; the game loop consumes them
 * via pp_dmr_take_command() and reports the real playback state back with
 * pp_dmr_report_*() so control points see the truth.
 */
#ifndef PP_DMR_H
#define PP_DMR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum pp_dmr_cmd_type {
    PP_DMR_CMD_NONE = 0,
    PP_DMR_CMD_PLAY_URI, /* url + title set */
    PP_DMR_CMD_STOP,
    PP_DMR_CMD_PAUSE,
    PP_DMR_CMD_RESUME,
    PP_DMR_CMD_SEEK,     /* seek_s set */
    PP_DMR_CMD_VOLUME    /* volume 0-100 set */
} pp_dmr_cmd_type;

typedef struct pp_dmr_cmd {
    pp_dmr_cmd_type type;
    char url[2048];
    char title[256];
    double seek_s;
    int volume;
} pp_dmr_cmd;

typedef enum pp_dmr_transport {
    PP_DMR_TRANSPORT_NO_MEDIA = 0,
    PP_DMR_TRANSPORT_STOPPED,
    PP_DMR_TRANSPORT_PLAYING,
    PP_DMR_TRANSPORT_PAUSED,
    PP_DMR_TRANSPORT_TRANSITIONING
} pp_dmr_transport;

typedef struct pp_dmr_status {
    int running;
    char name[64];
    char ip[64];
    int port;
    int transport; /* pp_dmr_transport */
    char uri[2048];
    char title[256];
    double duration_s;
    double position_s;
    int volume;    /* 0-100 */
    int mute;      /* 0/1 */
} pp_dmr_status;

/* Start renderer (spawns SSDP + HTTP threads). 0 on success. */
int pp_dmr_start(const char *friendly_name);

/* Stop renderer (ssdp:byebye + shutdown). Safe when not running. */
void pp_dmr_stop(void);

/* Fetch next pending command (1 = got one, 0 = none). */
int pp_dmr_take_command(pp_dmr_cmd *out);

/*
 * Fetch a pending device-connect event (controller IP that subscribed
 * to GENA events for the first time). 1 = event copied to out.
 */
int pp_dmr_take_device_event(char *out, size_t outsz);

/* Playback state feedback from the player (drives SOAP + LastChange). */
void pp_dmr_report_playing(void);
void pp_dmr_report_paused(void);
void pp_dmr_report_stopped(void);
void pp_dmr_report_position(double position_s);

/* Snapshot for the UI status screen. */
void pp_dmr_get_status(pp_dmr_status *out);

#ifdef __cplusplus
}
#endif

#endif /* PP_DMR_H */
