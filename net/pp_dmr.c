/*
 * pp_dmr — DLNA Digital Media Renderer for ProsperoPlayer (see pp_dmr.h).
 *
 * Self-contained: no external UPnP library. Threads:
 *  - ssdp_thread: UDP 1900 multicast responder + periodic ssdp:alive
 *  - http_thread: TCP 9080 — description docs, SOAP control, GENA eventing
 */
#include "pp_dmr.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PP_DMR_HTTP_PORT 9080
#define PP_DMR_SSDP_PORT 1900
#define PP_DMR_MCAST_ADDR "239.255.255.250"
#define PP_DMR_MAX_AGE 1800
#define PP_DMR_MAX_SUBS 8
#define PP_DMR_UDN "uuid:70726f73-7065-726f-2d70-6c6179657231"

#define PP_DMR_DEVICE_TYPE "urn:schemas-upnp-org:device:MediaRenderer:1"
#define PP_DMR_SERVICE_AVT "urn:schemas-upnp-org:service:AVTransport:1"
#define PP_DMR_SERVICE_RCS "urn:schemas-upnp-org:service:RenderingControl:1"
#define PP_DMR_SERVICE_CMS "urn:schemas-upnp-org:service:ConnectionManager:1"

#define PP_DMR_SERVER_HDR "PSPlay/1.5 UPnP/1.0 PSPlay/1.5"

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int used;
    int service; /* 0=AVT 1=RCS */
    char callback[768];
    char sid[80];
    unsigned seq;
    time_t expires;
} pp_dmr_sub;

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_running = 0;
static volatile int g_stop_req = 0;

static char g_name[64] = "PS Play";
static char g_ip[64] = {0};
static int g_transport = PP_DMR_TRANSPORT_NO_MEDIA;
static char g_uri[2048] = {0};
static char g_title[256] = {0};
static char g_metadata[4096] = {0};
static double g_duration_s = 0.0;
static double g_base_pos_s = 0.0;  /* position at base time */
static uint64_t g_base_time_us = 0;
static int g_volume = 100;
static int g_mute = 0;

static pp_dmr_cmd g_cmd;           /* single pending command */
static volatile int g_cmd_pending = 0;

static pp_dmr_sub g_subs[PP_DMR_MAX_SUBS];

static pthread_t g_ssdp_tid;
static pthread_t g_http_tid;
static int g_http_listen = -1;
static int g_udp_fd = -1;

static uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

static double position_now_locked(void) {
    double p = g_base_pos_s;
    if (g_transport == PP_DMR_TRANSPORT_PLAYING && g_base_time_us) {
        p += (double)(now_us() - g_base_time_us) / 1000000.0;
    }
    if (p < 0.0) p = 0.0;
    return p;
}

/* ------------------------------------------------------------------ */
/* Small XML / SOAP helpers                                            */
/* ------------------------------------------------------------------ */

static void xml_escape(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    if (!out || outsz == 0) return;
    if (!in) in = "";
    for (const char *p = in; *p && o + 8 < outsz; p++) {
        const char *rep = NULL;
        switch (*p) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        default: break;
        }
        if (rep) {
            size_t rl = strlen(rep);
            memcpy(out + o, rep, rl);
            o += rl;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
}

static void xml_unescape(char *s) {
    if (!s) return;
    char *r = s, *w = s;
    while (*r) {
        if (*r == '&') {
            if (!strncmp(r, "&amp;", 5)) { *w++ = '&'; r += 5; continue; }
            if (!strncmp(r, "&lt;", 4)) { *w++ = '<'; r += 4; continue; }
            if (!strncmp(r, "&gt;", 4)) { *w++ = '>'; r += 4; continue; }
            if (!strncmp(r, "&quot;", 6)) { *w++ = '"'; r += 6; continue; }
            if (!strncmp(r, "&apos;", 6)) { *w++ = '\''; r += 6; continue; }
        }
        *w++ = *r++;
    }
    *w = 0;
}

/*
 * Extract a SOAP argument value by local tag name, namespace-agnostic:
 * matches <CurrentURI>, <u:CurrentURI ...>, etc.
 */
static int soap_arg(const char *body, const char *name, char *out, size_t outsz) {
    if (!body || !name || !out || outsz == 0) return -1;
    out[0] = 0;
    size_t nl = strlen(name);
    const char *p = body;
    while ((p = strstr(p, name)) != NULL) {
        char prev = (p == body) ? 0 : p[-1];
        char next = p[nl];
        if ((prev == '<' || prev == ':') && (next == '>' || next == ' ' || next == '/')) {
            const char *gt = strchr(p + nl, '>');
            if (!gt) return -1;
            if (gt[-1] == '/') return 0; /* empty element */
            const char *val = gt + 1;
            const char *end = strstr(val, "</");
            if (!end) return -1;
            size_t len = (size_t)(end - val);
            if (len >= outsz) len = outsz - 1;
            memcpy(out, val, len);
            out[len] = 0;
            /* strip CDATA wrapper if present */
            if (!strncmp(out, "<![CDATA[", 9)) {
                size_t l = strlen(out);
                if (l >= 12 && !strcmp(out + l - 3, "]]>")) {
                    memmove(out, out + 9, l - 12);
                    out[l - 12] = 0;
                }
            }
            xml_unescape(out);
            return 0;
        }
        p += nl;
    }
    return -1;
}

/* Extract attr value from a DIDL <res ...> element (metadata already unescaped). */
static int didl_res_attr(const char *didl, const char *attr, char *out, size_t outsz) {
    if (!didl || !attr || !out || outsz == 0) return -1;
    out[0] = 0;
    const char *res = strstr(didl, "<res");
    if (!res) return -1;
    const char *gt = strchr(res, '>');
    if (!gt) return -1;
    char pat[64];
    snprintf(pat, sizeof(pat), "%s=\"", attr);
    const char *a = strstr(res, pat);
    if (!a || a > gt) return -1;
    a += strlen(pat);
    const char *q = strchr(a, '"');
    if (!q || q > gt) return -1;
    size_t len = (size_t)(q - a);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, a, len);
    out[len] = 0;
    return 0;
}

static double parse_upnp_time(const char *s) {
    if (!s || !*s) return 0.0;
    int h = 0, m = 0;
    double sec = 0.0;
    if (sscanf(s, "%d:%d:%lf", &h, &m, &sec) == 3)
        return h * 3600.0 + m * 60.0 + sec;
    if (sscanf(s, "%d:%lf", &m, &sec) == 2)
        return m * 60.0 + sec;
    return 0.0;
}

static void fmt_upnp_time(double t, char *out, size_t outsz) {
    if (t < 0) t = 0;
    unsigned total = (unsigned)(t + 0.5);
    snprintf(out, outsz, "%u:%02u:%02u", total / 3600u, (total / 60u) % 60u, total % 60u);
}

/* ------------------------------------------------------------------ */
/* Local IPv4 (primary non-loopback interface)                         */
/* ------------------------------------------------------------------ */

static int find_local_ip(char *out, size_t outsz) {
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) != 0 || !ifs) return -1;
    int found = -1;
    for (struct ifaddrs *ifa = ifs; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
        struct in_addr a = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
        if (!inet_ntop(AF_INET, &a, out, (socklen_t)outsz)) continue;
        found = 0;
        break;
    }
    freeifaddrs(ifs);
    return found;
}

/* ------------------------------------------------------------------ */
/* GENA eventing                                                       */
/* ------------------------------------------------------------------ */

static void subs_remove_dead_locked(void) {
    time_t now = time(NULL);
    for (int i = 0; i < PP_DMR_MAX_SUBS; i++)
        if (g_subs[i].used && g_subs[i].expires && g_subs[i].expires < now)
            g_subs[i].used = 0;
}

/* Build LastChange XML for AVT (escaped embedding handled by caller). */
static void build_avt_lastchange(char *out, size_t outsz) {
    char state[24] = "STOPPED";
    char actions[64] = "Play";
    char esc_uri[2600], esc_meta[4300], dur[32];
    double pos;

    pthread_mutex_lock(&g_mtx);
    switch (g_transport) {
    case PP_DMR_TRANSPORT_NO_MEDIA:
        snprintf(state, sizeof(state), "NO_MEDIA_PRESENT");
        actions[0] = 0;
        break;
    case PP_DMR_TRANSPORT_STOPPED:
        snprintf(state, sizeof(state), "STOPPED");
        snprintf(actions, sizeof(actions), "Play");
        break;
    case PP_DMR_TRANSPORT_PLAYING:
        snprintf(state, sizeof(state), "PLAYING");
        snprintf(actions, sizeof(actions), "Pause,Stop,Seek");
        break;
    case PP_DMR_TRANSPORT_PAUSED:
        snprintf(state, sizeof(state), "PAUSED_PLAYBACK");
        snprintf(actions, sizeof(actions), "Play,Stop,Seek");
        break;
    default:
        snprintf(state, sizeof(state), "TRANSITIONING");
        snprintf(actions, sizeof(actions), "Stop");
        break;
    }
    xml_escape(g_uri, esc_uri, sizeof(esc_uri));
    xml_escape(g_metadata, esc_meta, sizeof(esc_meta));
    fmt_upnp_time(g_duration_s, dur, sizeof(dur));
    (void)pos;
    pthread_mutex_unlock(&g_mtx);

    snprintf(out, outsz,
             "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/AVT/\">"
             "<InstanceID val=\"0\">"
             "<TransportState val=\"%s\"/>"
             "<TransportStatus val=\"OK\"/>"
             "<TransportPlaySpeed val=\"1\"/>"
             "<CurrentTransportActions val=\"%s\"/>"
             "<CurrentTrack val=\"1\"/>"
             "<NumberOfTracks val=\"1\"/>"
             "<CurrentTrackDuration val=\"%s\"/>"
             "<CurrentMediaDuration val=\"%s\"/>"
             "<CurrentTrackURI val=\"%s\"/>"
             "<AVTransportURI val=\"%s\"/>"
             "<AVTransportURIMetaData val=\"%s\"/>"
             "<CurrentTrackMetaData val=\"%s\"/>"
             "</InstanceID></Event>",
             state, actions, dur, dur, esc_uri, esc_uri, esc_meta, esc_meta);
}

static void build_rcs_lastchange(char *out, size_t outsz) {
    int vol, mute;
    pthread_mutex_lock(&g_mtx);
    vol = g_volume;
    mute = g_mute;
    pthread_mutex_unlock(&g_mtx);
    snprintf(out, outsz,
             "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/RCS/\">"
             "<InstanceID val=\"0\">"
             "<Volume val=\"%d\" channel=\"Master\"/>"
             "<Mute val=\"%d\" channel=\"Master\"/>"
             "</InstanceID></Event>",
             vol, mute);
}

/* Send one event NOTIFY to a subscriber (best effort, blocking 4s). */
static void event_send_one(const pp_dmr_sub *sub, const char *lastchange) {
    char host[256], path[512], esc_lc[9000], body[10000], hdr[1200], req[12288];
    struct sockaddr_in addr;
    int fd, n;
    size_t blen;

    if (!sub || !sub->used) return;

    /* callback: http://host:port/path */
    const char *cb = sub->callback;
    if (strncmp(cb, "http://", 7)) return;
    cb += 7;
    const char *slash = strchr(cb, '/');
    if (slash) {
        size_t hl = (size_t)(slash - cb);
        if (hl >= sizeof(host)) hl = sizeof(host) - 1;
        memcpy(host, cb, hl);
        host[hl] = 0;
        snprintf(path, sizeof(path), "%s", slash);
    } else {
        snprintf(host, sizeof(host), "%s", cb);
        snprintf(path, sizeof(path), "/");
    }
    char *colon = strrchr(host, ':');
    int port = 80;
    if (colon) { *colon = 0; port = atoi(colon + 1); if (port <= 0) port = 80; }

    xml_escape(lastchange, esc_lc, sizeof(esc_lc));
    blen = (size_t)snprintf(body, sizeof(body),
                            "<?xml version=\"1.0\"?>"
                            "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">"
                            "<e:property><LastChange>%s</LastChange></e:property>"
                            "</e:propertyset>",
                            esc_lc);

    n = snprintf(hdr, sizeof(hdr),
                 "NOTIFY %s HTTP/1.1\r\n"
                 "HOST: %s:%d\r\n"
                 "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
                 "NT: upnp:event\r\n"
                 "NTS: upnp:propchange\r\n"
                 "SID: %s\r\n"
                 "SEQ: %u\r\n"
                 "CONTENT-LENGTH: %zu\r\n"
                 "CONNECTION: close\r\n\r\n",
                 path, host, port, sub->sid, sub->seq, blen);
    if (n <= 0 || (size_t)n >= sizeof(hdr)) return;
    if ((size_t)n + blen >= sizeof(req)) return;
    memcpy(req, hdr, (size_t)n);
    memcpy(req + n, body, blen);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct timeval tv = {4, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1 ||
        connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return;
    }
    (void)send(fd, req, (size_t)n + blen, 0);
    char drain[256];
    (void)recv(fd, drain, sizeof(drain), 0);
    close(fd);
}

/* Notify all subscribers of a service (call WITHOUT mutex held). */
static void event_notify(int service) {
    char lc[9000];
    pp_dmr_sub subs[PP_DMR_MAX_SUBS];

    if (service == 0)
        build_avt_lastchange(lc, sizeof(lc));
    else
        build_rcs_lastchange(lc, sizeof(lc));

    pthread_mutex_lock(&g_mtx);
    subs_remove_dead_locked();
    memcpy(subs, g_subs, sizeof(subs));
    pthread_mutex_unlock(&g_mtx);

    for (int i = 0; i < PP_DMR_MAX_SUBS; i++) {
        if (!subs[i].used || subs[i].service != service) continue;
        event_send_one(&subs[i], lc);
        pthread_mutex_lock(&g_mtx);
        for (int j = 0; j < PP_DMR_MAX_SUBS; j++)
            if (g_subs[j].used && !strcmp(g_subs[j].sid, subs[i].sid))
                g_subs[j].seq++;
        pthread_mutex_unlock(&g_mtx);
    }
}

/* ------------------------------------------------------------------ */
/* Device description + SCPD documents                                 */
/* ------------------------------------------------------------------ */

static void build_desc_xml(char *out, size_t outsz) {
    char name[64], ip[64];
    pthread_mutex_lock(&g_mtx);
    snprintf(name, sizeof(name), "%s", g_name);
    snprintf(ip, sizeof(ip), "%s", g_ip);
    pthread_mutex_unlock(&g_mtx);

    snprintf(out, outsz,
             "<?xml version=\"1.0\"?>\n"
             "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\n"
             " <specVersion><major>1</major><minor>0</minor></specVersion>\n"
             " <device>\n"
             "  <deviceType>" PP_DMR_DEVICE_TYPE "</deviceType>\n"
             "  <friendlyName>%s</friendlyName>\n"
             "  <manufacturer>InsideMatrixDev</manufacturer>\n"
             "  <modelName>PS Play</modelName>\n"
             "  <modelDescription>PS5 DLNA Media Renderer</modelDescription>\n"
             "  <modelNumber>1.5</modelNumber>\n"
             "  <UDN>" PP_DMR_UDN "</UDN>\n"
             "  <dlna:X_DLNADOC xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">DMR-1.50</dlna:X_DLNADOC>\n"
             "  <serviceList>\n"
             "   <service>\n"
             "    <serviceType>" PP_DMR_SERVICE_AVT "</serviceType>\n"
             "    <serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>\n"
             "    <SCPDURL>/dmr/avt-scpd.xml</SCPDURL>\n"
             "    <controlURL>/dmr/avt/control</controlURL>\n"
             "    <eventSubURL>/dmr/avt/events</eventSubURL>\n"
             "   </service>\n"
             "   <service>\n"
             "    <serviceType>" PP_DMR_SERVICE_RCS "</serviceType>\n"
             "    <serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>\n"
             "    <SCPDURL>/dmr/rcs-scpd.xml</SCPDURL>\n"
             "    <controlURL>/dmr/rcs/control</controlURL>\n"
             "    <eventSubURL>/dmr/rcs/events</eventSubURL>\n"
             "   </service>\n"
             "   <service>\n"
             "    <serviceType>" PP_DMR_SERVICE_CMS "</serviceType>\n"
             "    <serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>\n"
             "    <SCPDURL>/dmr/cms-scpd.xml</SCPDURL>\n"
             "    <controlURL>/dmr/cms/control</controlURL>\n"
             "    <eventSubURL>/dmr/cms/events</eventSubURL>\n"
             "   </service>\n"
             "  </serviceList>\n"
             "  <presentationURL>http://%s:%d/</presentationURL>\n"
             " </device>\n"
             "</root>\n",
             name, ip, PP_DMR_HTTP_PORT);
}

#define SCPD_AVT_ACTIONS                                                 \
    ACTION0("GetTransportInfo")                                          \
    ACTION0("GetPositionInfo")                                           \
    ACTION0("GetMediaInfo")                                              \
    ACTION0("GetDeviceCapabilities")                                     \
    ACTION0("GetTransportSettings")                                      \
    ACTION0("GetCurrentTransportActions")                                \
    ACTION0("Stop")                                                      \
    ACTION0("Pause")                                                     \
    "<action><name>Play</name><argumentList>"                            \
    ARG("Speed", "in", "TransportPlaySpeed")                             \
    "</argumentList></action>"                                           \
    "<action><name>SetAVTransportURI</name><argumentList>"               \
    ARG("CurrentURI", "in", "AVTransportURI")                            \
    ARG("CurrentURIMetaData", "in", "AVTransportURIMetaData")            \
    "</argumentList></action>"                                           \
    "<action><name>SetNextAVTransportURI</name><argumentList>"           \
    ARG("NextURI", "in", "NextAVTransportURI")                           \
    ARG("NextURIMetaData", "in", "NextAVTransportURIMetaData")           \
    "</argumentList></action>"                                           \
    "<action><name>Seek</name><argumentList>"                            \
    ARG("Unit", "in", "A_ARG_TYPE_SeekMode")                             \
    ARG("Target", "in", "A_ARG_TYPE_SeekTarget")                         \
    "</argumentList></action>"

#define ARG(n, d, v)                                                     \
    "<argument><name>" n "</name><direction>" d                          \
    "</direction><relatedStateVariable>" v "</relatedStateVariable></argument>"
#define ACTION0(n) "<action><name>" n "</name></action>"
#define SV(n, t, ev)                                                     \
    "<stateVariable sendEvents=\"" ev "\"><name>" n "</name><dataType>" t \
    "</dataType></stateVariable>"

static void build_avt_scpd(char *out, size_t outsz) {
    snprintf(out, outsz,
             "<?xml version=\"1.0\"?>\n"
             "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\n"
             "<specVersion><major>1</major><minor>0</minor></specVersion>\n"
             "<actionList>" SCPD_AVT_ACTIONS "</actionList>\n"
             "<serviceStateTable>"
             SV("TransportState", "string", "no")
             SV("TransportStatus", "string", "no")
             SV("TransportPlaySpeed", "string", "no")
             SV("CurrentTrack", "ui4", "no")
             SV("NumberOfTracks", "ui4", "no")
             SV("CurrentTrackDuration", "string", "no")
             SV("CurrentMediaDuration", "string", "no")
             SV("CurrentTrackURI", "string", "no")
             SV("AVTransportURI", "string", "no")
             SV("AVTransportURIMetaData", "string", "no")
             SV("CurrentTrackMetaData", "string", "no")
             SV("NextAVTransportURI", "string", "no")
             SV("NextAVTransportURIMetaData", "string", "no")
             SV("RelativeTimePosition", "string", "no")
             SV("AbsoluteTimePosition", "string", "no")
             SV("CurrentTransportActions", "string", "no")
             SV("PlaybackStorageMedium", "string", "no")
             SV("PossiblePlaybackStorageMedia", "string", "no")
             SV("PossibleRecordStorageMedia", "string", "no")
             SV("PossibleRecordQualityModes", "string", "no")
             SV("RecordMediumWriteStatus", "string", "no")
             SV("CurrentPlayMode", "string", "no")
             SV("LastChange", "string", "yes")
             SV("A_ARG_TYPE_SeekMode", "string", "no")
             SV("A_ARG_TYPE_SeekTarget", "string", "no")
             SV("A_ARG_TYPE_InstanceID", "ui4", "no")
             "</serviceStateTable>\n</scpd>\n");
}

static void build_rcs_scpd(char *out, size_t outsz) {
    snprintf(out, outsz,
             "<?xml version=\"1.0\"?>\n"
             "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\n"
             "<specVersion><major>1</major><minor>0</minor></specVersion>\n"
             "<actionList>"
             "<action><name>ListPresets</name><argumentList>"
             ARG("CurrentPresetNameList", "out", "PresetNameList")
             "</argumentList></action>"
             "<action><name>GetVolume</name><argumentList>"
             ARG("Channel", "in", "A_ARG_TYPE_Channel")
             ARG("CurrentVolume", "out", "Volume")
             "</argumentList></action>"
             "<action><name>SetVolume</name><argumentList>"
             ARG("Channel", "in", "A_ARG_TYPE_Channel")
             ARG("DesiredVolume", "in", "Volume")
             "</argumentList></action>"
             "<action><name>GetMute</name><argumentList>"
             ARG("Channel", "in", "A_ARG_TYPE_Channel")
             ARG("CurrentMute", "out", "Mute")
             "</argumentList></action>"
             "<action><name>SetMute</name><argumentList>"
             ARG("Channel", "in", "A_ARG_TYPE_Channel")
             ARG("DesiredMute", "in", "Mute")
             "</argumentList></action>"
             "</actionList>\n"
             "<serviceStateTable>"
             SV("Volume", "ui2", "no")
             SV("Mute", "boolean", "no")
             SV("PresetNameList", "string", "no")
             SV("LastChange", "string", "yes")
             SV("A_ARG_TYPE_Channel", "string", "no")
             SV("A_ARG_TYPE_InstanceID", "ui4", "no")
             "</serviceStateTable>\n</scpd>\n");
}

static void build_cms_scpd(char *out, size_t outsz) {
    snprintf(out, outsz,
             "<?xml version=\"1.0\"?>\n"
             "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\n"
             "<specVersion><major>1</major><minor>0</minor></specVersion>\n"
             "<actionList>"
             "<action><name>GetProtocolInfo</name><argumentList>"
             ARG("Source", "out", "SourceProtocolInfo")
             ARG("Sink", "out", "SinkProtocolInfo")
             "</argumentList></action>"
             "<action><name>GetCurrentConnectionIDs</name><argumentList>"
             ARG("ConnectionIDs", "out", "CurrentConnectionIDs")
             "</argumentList></action>"
             "</actionList>\n"
             "<serviceStateTable>"
             SV("SourceProtocolInfo", "string", "yes")
             SV("SinkProtocolInfo", "string", "yes")
             SV("CurrentConnectionIDs", "string", "yes")
             SV("A_ARG_TYPE_ProtocolInfo", "string", "no")
             SV("A_ARG_TYPE_ConnectionID", "i4", "no")
             SV("A_ARG_TYPE_ConnectionManager", "string", "no")
             SV("A_ARG_TYPE_ConnectionStatus", "string", "no")
             SV("A_ARG_TYPE_Direction", "string", "no")
             "</serviceStateTable>\n</scpd>\n");
}

/* ------------------------------------------------------------------ */
/* HTTP plumbing                                                       */
/* ------------------------------------------------------------------ */

static int send_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static void http_reply(int fd, int status, const char *ctype,
                       const char *body, size_t blen) {
    char hdr[512];
    const char *reason = status == 200 ? "OK" : status == 500 ? "Internal Server Error" : "Not Found";
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "CONTENT-TYPE: %s\r\n"
                     "CONTENT-LENGTH: %zu\r\n"
                     "EXT:\r\n"
                     "SERVER: " PP_DMR_SERVER_HDR "\r\n"
                     "CONNECTION: close\r\n\r\n",
                     status, reason, ctype, blen);
    if (n > 0) (void)send_all(fd, hdr, (size_t)n);
    if (body && blen) (void)send_all(fd, body, blen);
}

static void http_reply_xml(int fd, const char *body) {
    http_reply(fd, 200, "text/xml; charset=\"utf-8\"", body, strlen(body));
}

static void soap_error(int fd, int code, const char *desc) {
    char body[1024];
    snprintf(body, sizeof(body),
             "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
             "<s:Body><s:Fault><faultcode>s:Client</faultcode>"
             "<faultstring>UPnPError</faultstring><detail>"
             "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
             "<errorCode>%d</errorCode><errorDescription>%s</errorDescription>"
             "</UPnPError></detail></s:Fault></s:Body></s:Envelope>",
             code, desc);
    http_reply(fd, 500, "text/xml; charset=\"utf-8\"", body, strlen(body));
}

static void soap_ok(int fd, const char *service, const char *action, const char *args_xml) {
    char body[8192];
    snprintf(body, sizeof(body),
             "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
             "<s:Body><u:%sResponse xmlns:u=\"%s\">%s</u:%sResponse>"
             "</s:Body></s:Envelope>",
             action, service, args_xml ? args_xml : "", action);
    http_reply_xml(fd, body);
}

/* ------------------------------------------------------------------ */
/* Commands towards the player                                         */
/* ------------------------------------------------------------------ */

static void cmd_push(pp_dmr_cmd_type type, const char *url, const char *title,
                     double seek_s, int volume) {
    pthread_mutex_lock(&g_mtx);
    g_cmd.type = type;
    if (url) snprintf(g_cmd.url, sizeof(g_cmd.url), "%s", url);
    else g_cmd.url[0] = 0;
    if (title) snprintf(g_cmd.title, sizeof(g_cmd.title), "%s", title);
    else g_cmd.title[0] = 0;
    g_cmd.seek_s = seek_s;
    g_cmd.volume = volume;
    g_cmd_pending = 1;
    pthread_mutex_unlock(&g_mtx);
}

int pp_dmr_take_command(pp_dmr_cmd *out) {
    if (!out) return 0;
    pthread_mutex_lock(&g_mtx);
    if (!g_cmd_pending) {
        pthread_mutex_unlock(&g_mtx);
        return 0;
    }
    memcpy(out, &g_cmd, sizeof(*out));
    g_cmd_pending = 0;
    g_cmd.type = PP_DMR_CMD_NONE;
    pthread_mutex_unlock(&g_mtx);
    return 1;
}

/* ------------------------------------------------------------------ */
/* SOAP action handlers                                                */
/* ------------------------------------------------------------------ */

static void avt_set_uri(int fd, const char *body, int next) {
    char uri[2048], meta[4096], title[256], dur[32];

    if (soap_arg(body, next ? "NextURI" : "CurrentURI", uri, sizeof(uri)) != 0)
        uri[0] = 0;
    if (soap_arg(body, next ? "NextURIMetaData" : "CurrentURIMetaData",
                 meta, sizeof(meta)) != 0)
        meta[0] = 0;

    title[0] = 0;
    dur[0] = 0;
    if (meta[0]) {
        if (soap_arg(meta, "dc:title", title, sizeof(title)) != 0)
            (void)soap_arg(meta, "title", title, sizeof(title));
        (void)didl_res_attr(meta, "duration", dur, sizeof(dur));
    }

    pthread_mutex_lock(&g_mtx);
    if (!next) {
        snprintf(g_uri, sizeof(g_uri), "%s", uri);
        snprintf(g_title, sizeof(g_title), "%s", title);
        snprintf(g_metadata, sizeof(g_metadata), "%s", meta);
        g_duration_s = parse_upnp_time(dur);
        g_base_pos_s = 0.0;
        g_base_time_us = now_us();
        g_transport = uri[0] ? PP_DMR_TRANSPORT_STOPPED : PP_DMR_TRANSPORT_NO_MEDIA;
    }
    pthread_mutex_unlock(&g_mtx);

    soap_ok(fd, PP_DMR_SERVICE_AVT, next ? "SetNextAVTransportURI" : "SetAVTransportURI", "");
    if (!next) event_notify(0);
}

static void avt_play(int fd) {
    char uri[2048], title[256];
    int transport, paused_media;

    pthread_mutex_lock(&g_mtx);
    transport = g_transport;
    paused_media = g_uri[0] != 0;
    snprintf(uri, sizeof(uri), "%s", g_uri);
    snprintf(title, sizeof(title), "%s", g_title);
    pthread_mutex_unlock(&g_mtx);

    if (!paused_media) {
        soap_error(fd, 402, "No media");
        return;
    }

    if (transport == PP_DMR_TRANSPORT_PAUSED)
        cmd_push(PP_DMR_CMD_RESUME, NULL, NULL, 0, 0);
    else
        cmd_push(PP_DMR_CMD_PLAY_URI, uri, title, 0, 0);

    pthread_mutex_lock(&g_mtx);
    g_transport = PP_DMR_TRANSPORT_TRANSITIONING;
    pthread_mutex_unlock(&g_mtx);

    soap_ok(fd, PP_DMR_SERVICE_AVT, "Play", "");
    event_notify(0);
}

static void avt_pause(int fd) {
    cmd_push(PP_DMR_CMD_PAUSE, NULL, NULL, 0, 0);
    pthread_mutex_lock(&g_mtx);
    if (g_transport == PP_DMR_TRANSPORT_PLAYING) {
        g_base_pos_s = position_now_locked();
        g_base_time_us = now_us();
        g_transport = PP_DMR_TRANSPORT_PAUSED;
    }
    pthread_mutex_unlock(&g_mtx);
    soap_ok(fd, PP_DMR_SERVICE_AVT, "Pause", "");
    event_notify(0);
}

static void avt_stop(int fd) {
    cmd_push(PP_DMR_CMD_STOP, NULL, NULL, 0, 0);
    pthread_mutex_lock(&g_mtx);
    g_base_pos_s = 0.0;
    g_base_time_us = now_us();
    if (g_uri[0])
        g_transport = PP_DMR_TRANSPORT_STOPPED;
    else
        g_transport = PP_DMR_TRANSPORT_NO_MEDIA;
    pthread_mutex_unlock(&g_mtx);
    soap_ok(fd, PP_DMR_SERVICE_AVT, "Stop", "");
    event_notify(0);
}

static void avt_seek(int fd, const char *body) {
    char unit[32], target[64];
    double t;

    if (soap_arg(body, "Unit", unit, sizeof(unit)) != 0)
        snprintf(unit, sizeof(unit), "REL_TIME");
    if (soap_arg(body, "Target", target, sizeof(target)) != 0 || !target[0]) {
        soap_error(fd, 402, "Missing target");
        return;
    }
    if (strcasecmp(unit, "REL_TIME") && strcasecmp(unit, "ABS_TIME")) {
        soap_error(fd, 710, "Seek mode not supported");
        return;
    }
    t = parse_upnp_time(target);
    cmd_push(PP_DMR_CMD_SEEK, NULL, NULL, t, 0);

    pthread_mutex_lock(&g_mtx);
    g_base_pos_s = t;
    g_base_time_us = now_us();
    pthread_mutex_unlock(&g_mtx);

    soap_ok(fd, PP_DMR_SERVICE_AVT, "Seek", "");
    event_notify(0);
}

static void avt_get_transport_info(int fd) {
    char state[24] = "STOPPED", args[512];
    int t;
    pthread_mutex_lock(&g_mtx);
    t = g_transport;
    pthread_mutex_unlock(&g_mtx);
    switch (t) {
    case PP_DMR_TRANSPORT_NO_MEDIA: snprintf(state, sizeof(state), "NO_MEDIA_PRESENT"); break;
    case PP_DMR_TRANSPORT_STOPPED: snprintf(state, sizeof(state), "STOPPED"); break;
    case PP_DMR_TRANSPORT_PLAYING: snprintf(state, sizeof(state), "PLAYING"); break;
    case PP_DMR_TRANSPORT_PAUSED: snprintf(state, sizeof(state), "PAUSED_PLAYBACK"); break;
    default: snprintf(state, sizeof(state), "TRANSITIONING"); break;
    }
    snprintf(args, sizeof(args),
             "<CurrentTransportState>%s</CurrentTransportState>"
             "<CurrentTransportStatus>OK</CurrentTransportStatus>"
             "<CurrentSpeed>1</CurrentSpeed>",
             state);
    soap_ok(fd, PP_DMR_SERVICE_AVT, "GetTransportInfo", args);
}

static void avt_get_position_info(int fd) {
    char args[4200], esc_uri[2600], esc_meta[4300], dur[32], pos[32];
    pthread_mutex_lock(&g_mtx);
    xml_escape(g_uri, esc_uri, sizeof(esc_uri));
    xml_escape(g_metadata, esc_meta, sizeof(esc_meta));
    fmt_upnp_time(g_duration_s, dur, sizeof(dur));
    fmt_upnp_time(position_now_locked(), pos, sizeof(pos));
    pthread_mutex_unlock(&g_mtx);
    snprintf(args, sizeof(args),
             "<Track>1</Track>"
             "<TrackDuration>%s</TrackDuration>"
             "<TrackMetaData>%s</TrackMetaData>"
             "<TrackURI>%s</TrackURI>"
             "<RelTime>%s</RelTime>"
             "<AbsTime>%s</AbsTime>"
             "<RelCount>0</RelCount>"
             "<AbsCount>0</AbsCount>",
             dur, esc_meta, esc_uri, pos, pos);
    soap_ok(fd, PP_DMR_SERVICE_AVT, "GetPositionInfo", args);
}

static void avt_get_media_info(int fd) {
    char args[5200], esc_uri[2600], esc_meta[4300], dur[32];
    pthread_mutex_lock(&g_mtx);
    xml_escape(g_uri, esc_uri, sizeof(esc_uri));
    xml_escape(g_metadata, esc_meta, sizeof(esc_meta));
    fmt_upnp_time(g_duration_s, dur, sizeof(dur));
    pthread_mutex_unlock(&g_mtx);
    snprintf(args, sizeof(args),
             "<NrTracks>1</NrTracks>"
             "<MediaDuration>%s</MediaDuration>"
             "<CurrentURI>%s</CurrentURI>"
             "<CurrentURIMetaData>%s</CurrentURIMetaData>"
             "<NextURI></NextURI>"
             "<NextURIMetaData></NextURIMetaData>"
             "<PlayMedium>NETWORK</PlayMedium>"
             "<RecordMedium>NOT_IMPLEMENTED</RecordMedium>"
             "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>",
             dur, esc_uri, esc_meta);
    soap_ok(fd, PP_DMR_SERVICE_AVT, "GetMediaInfo", args);
}

static void avt_get_actions(int fd) {
    char actions[64] = "Play", args[160];
    int t;
    pthread_mutex_lock(&g_mtx);
    t = g_transport;
    pthread_mutex_unlock(&g_mtx);
    if (t == PP_DMR_TRANSPORT_PLAYING)
        snprintf(actions, sizeof(actions), "Pause,Stop,Seek");
    else if (t == PP_DMR_TRANSPORT_PAUSED)
        snprintf(actions, sizeof(actions), "Play,Stop,Seek");
    else if (t == PP_DMR_TRANSPORT_STOPPED)
        snprintf(actions, sizeof(actions), "Play");
    else
        actions[0] = 0;
    snprintf(args, sizeof(args), "<Actions>%s</Actions>", actions);
    soap_ok(fd, PP_DMR_SERVICE_AVT, "GetCurrentTransportActions", args);
}

static void rcs_list_presets(int fd) {
    soap_ok(fd, PP_DMR_SERVICE_RCS, "ListPresets",
            "<CurrentPresetNameList>Factory defaults</CurrentPresetNameList>");
}

static void rcs_get_volume(int fd) {
    char args[128];
    int v;
    pthread_mutex_lock(&g_mtx);
    v = g_volume;
    pthread_mutex_unlock(&g_mtx);
    snprintf(args, sizeof(args), "<CurrentVolume>%d</CurrentVolume>", v);
    soap_ok(fd, PP_DMR_SERVICE_RCS, "GetVolume", args);
}

static void rcs_set_volume(int fd, const char *body) {
    char v[16];
    if (soap_arg(body, "DesiredVolume", v, sizeof(v)) != 0) {
        soap_error(fd, 402, "Missing volume");
        return;
    }
    int vol = atoi(v);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    pthread_mutex_lock(&g_mtx);
    g_volume = vol;
    pthread_mutex_unlock(&g_mtx);
    cmd_push(PP_DMR_CMD_VOLUME, NULL, NULL, 0, vol);
    soap_ok(fd, PP_DMR_SERVICE_RCS, "SetVolume", "");
    event_notify(1);
}

static void rcs_get_mute(int fd) {
    char args[128];
    int m;
    pthread_mutex_lock(&g_mtx);
    m = g_mute;
    pthread_mutex_unlock(&g_mtx);
    snprintf(args, sizeof(args), "<CurrentMute>%d</CurrentMute>", m);
    soap_ok(fd, PP_DMR_SERVICE_RCS, "GetMute", args);
}

static void rcs_set_mute(int fd, const char *body) {
    char v[16];
    if (soap_arg(body, "DesiredMute", v, sizeof(v)) != 0) {
        soap_error(fd, 402, "Missing mute");
        return;
    }
    int mute = (!strcmp(v, "1") || !strcasecmp(v, "true")) ? 1 : 0;
    pthread_mutex_lock(&g_mtx);
    g_mute = mute;
    pthread_mutex_unlock(&g_mtx);
    cmd_push(PP_DMR_CMD_VOLUME, NULL, NULL, 0, mute ? 0 : g_volume);
    soap_ok(fd, PP_DMR_SERVICE_RCS, "SetMute", "");
    event_notify(1);
}

#define PP_DMR_PROTOCOL_INFO                                                \
    "http-get:*:video/mp4:*,"                                               \
    "http-get:*:video/x-matroska:*,"                                        \
    "http-get:*:video/webm:*,"                                              \
    "http-get:*:video/x-msvideo:*,"                                         \
    "http-get:*:video/mpeg:*,"                                              \
    "http-get:*:video/mp2t:*,"                                              \
    "http-get:*:video/3gpp:*,"                                              \
    "http-get:*:video/quicktime:*,"                                         \
    "http-get:*:audio/mpeg:*,"                                              \
    "http-get:*:audio/mp4:*,"                                               \
    "http-get:*:audio/x-flac:*,"                                            \
    "http-get:*:audio/flac:*,"                                              \
    "http-get:*:audio/wav:*,"                                               \
    "http-get:*:audio/x-ms-wma:*,"                                          \
    "http-get:*:application/octet-stream:*"

static void cms_get_protocol_info(int fd) {
    char args[1400];
    char esc[1200];
    xml_escape(PP_DMR_PROTOCOL_INFO, esc, sizeof(esc));
    snprintf(args, sizeof(args),
             "<Source>%s</Source><Sink></Sink>", esc);
    soap_ok(fd, PP_DMR_SERVICE_CMS, "GetProtocolInfo", args);
}

static void cms_get_connection_ids(int fd) {
    soap_ok(fd, PP_DMR_SERVICE_CMS, "GetCurrentConnectionIDs",
            "<ConnectionIDs>0</ConnectionIDs>");
}

/* ------------------------------------------------------------------ */
/* Request dispatch                                                    */
/* ------------------------------------------------------------------ */

static void handle_soap_avt(int fd, const char *body) {
    if (strstr(body, "SetNextAVTransportURI")) { avt_set_uri(fd, body, 1); return; }
    if (strstr(body, "SetAVTransportURI")) { avt_set_uri(fd, body, 0); return; }
    if (strstr(body, "GetCurrentTransportActions")) { avt_get_actions(fd); return; }
    if (strstr(body, "GetTransportSettings")) {
        soap_ok(fd, PP_DMR_SERVICE_AVT, "GetTransportSettings",
                "<PlayMode>NORMAL</PlayMode><RecQualityMode>NOT_IMPLEMENTED</RecQualityMode>");
        return;
    }
    if (strstr(body, "GetDeviceCapabilities")) {
        soap_ok(fd, PP_DMR_SERVICE_AVT, "GetDeviceCapabilities",
                "<PlayMedia>NETWORK,NONE</PlayMedia><RecMedia>NOT_IMPLEMENTED</RecMedia>"
                "<RecQualityModes>NOT_IMPLEMENTED</RecQualityModes>");
        return;
    }
    if (strstr(body, "GetTransportInfo")) { avt_get_transport_info(fd); return; }
    if (strstr(body, "GetPositionInfo")) { avt_get_position_info(fd); return; }
    if (strstr(body, "GetMediaInfo")) { avt_get_media_info(fd); return; }
    if (strstr(body, "Seek")) { avt_seek(fd, body); return; }
    if (strstr(body, "Play")) { avt_play(fd); return; }
    if (strstr(body, "Pause")) { avt_pause(fd); return; }
    if (strstr(body, "Stop")) { avt_stop(fd); return; }
    if (strstr(body, "Next") || strstr(body, "Previous")) {
        soap_error(fd, 711, "No next/previous track");
        return;
    }
    soap_error(fd, 401, "Invalid action");
}

static void handle_soap_rcs(int fd, const char *body) {
    if (strstr(body, "ListPresets")) { rcs_list_presets(fd); return; }
    if (strstr(body, "GetVolume")) { rcs_get_volume(fd); return; }
    if (strstr(body, "SetVolume")) { rcs_set_volume(fd, body); return; }
    if (strstr(body, "GetMute")) { rcs_get_mute(fd); return; }
    if (strstr(body, "SetMute")) { rcs_set_mute(fd, body); return; }
    soap_error(fd, 401, "Invalid action");
}

static void handle_soap_cms(int fd, const char *body) {
    if (strstr(body, "GetProtocolInfo")) { cms_get_protocol_info(fd); return; }
    if (strstr(body, "GetCurrentConnectionIDs")) { cms_get_connection_ids(fd); return; }
    if (strstr(body, "GetCurrentConnectionInfo")) {
        soap_ok(fd, PP_DMR_SERVICE_CMS, "GetCurrentConnectionInfo",
                "<RcsID>-1</RcsID><AVTransportID>-1</AVTransportID>"
                "<ProtocolInfo></ProtocolInfo><PeerConnectionManager></PeerConnectionManager>"
                "<PeerConnectionID>-1</PeerConnectionID><Direction>Input</Direction>"
                "<Status>Unknown</Status>");
        return;
    }
    soap_error(fd, 401, "Invalid action");
}

/* Extract an HTTP header value (case-insensitive, request head only). */
static int hdr_value(const char *head, const char *name, char *out, size_t outsz) {
    size_t nl = strlen(name);
    const char *p = head;
    while (p && *p) {
        const char *eol = strstr(p, "\r\n");
        if (!eol) eol = p + strlen(p);
        if ((size_t)(eol - p) > nl && !strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t len = (size_t)(eol - v);
            if (len >= outsz) len = outsz - 1;
            memcpy(out, v, len);
            out[len] = 0;
            return 0;
        }
        p = (*eol) ? eol + 2 : NULL;
    }
    return -1;
}

static void handle_subscribe(int fd, const char *head, int service, int unsubscribe) {
    char cb[768], sid[80], resp[512];

    if (unsubscribe) {
        if (hdr_value(head, "SID", sid, sizeof(sid)) == 0) {
            pthread_mutex_lock(&g_mtx);
            for (int i = 0; i < PP_DMR_MAX_SUBS; i++)
                if (g_subs[i].used && !strcmp(g_subs[i].sid, sid))
                    g_subs[i].used = 0;
            pthread_mutex_unlock(&g_mtx);
        }
        int n = snprintf(resp, sizeof(resp),
                         "HTTP/1.1 200 OK\r\nCONTENT-LENGTH: 0\r\n"
                         "SERVER: " PP_DMR_SERVER_HDR "\r\nCONNECTION: close\r\n\r\n");
        (void)send_all(fd, resp, (size_t)n);
        return;
    }

    if (hdr_value(head, "CALLBACK", cb, sizeof(cb)) != 0 || cb[0] != '<') {
        http_reply(fd, 400, "text/plain", "missing callback", 16);
        return;
    }
    /* strip < > */
    size_t cl = strlen(cb);
    if (cl >= 2 && cb[cl - 1] == '>') {
        memmove(cb, cb + 1, cl - 2);
        cb[cl - 2] = 0;
    }

    pp_dmr_sub sub;
    memset(&sub, 0, sizeof(sub));
    sub.used = 1;
    sub.service = service;
    snprintf(sub.callback, sizeof(sub.callback), "%s", cb);
    snprintf(sub.sid, sizeof(sub.sid), "uuid:70726f73-ev%06lx-%04x",
             (unsigned long)(now_us() & 0xFFFFFF), (unsigned)(rand() & 0xFFFF));
    sub.seq = 0;
    sub.expires = time(NULL) + 1800;

    int slot = -1;
    pthread_mutex_lock(&g_mtx);
    subs_remove_dead_locked();
    for (int i = 0; i < PP_DMR_MAX_SUBS; i++)
        if (!g_subs[i].used) { slot = i; break; }
    if (slot < 0) slot = 0; /* evict oldest */
    g_subs[slot] = sub;
    pthread_mutex_unlock(&g_mtx);

    int n = snprintf(resp, sizeof(resp),
                     "HTTP/1.1 200 OK\r\n"
                     "SID: %s\r\n"
                     "TIMEOUT: Second-1800\r\n"
                     "CONTENT-LENGTH: 0\r\n"
                     "SERVER: " PP_DMR_SERVER_HDR "\r\n"
                     "CONNECTION: close\r\n\r\n",
                     sub.sid);
    (void)send_all(fd, resp, (size_t)n);

    /* initial event (after the 200, per GENA) */
    sub.seq = 0;
    char lc[9000];
    if (service == 0)
        build_avt_lastchange(lc, sizeof(lc));
    else
        build_rcs_lastchange(lc, sizeof(lc));
    event_send_one(&sub, lc);
    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < PP_DMR_MAX_SUBS; i++)
        if (g_subs[i].used && !strcmp(g_subs[i].sid, sub.sid))
            g_subs[i].seq = 1;
    pthread_mutex_unlock(&g_mtx);
}

static void handle_connection(int fd) {
    char head[4096];
    char *body = NULL;
    size_t head_len = 0;
    int content_length = 0;

    /* read headers */
    while (head_len + 1 < sizeof(head)) {
        ssize_t n = recv(fd, head + head_len, sizeof(head) - 1 - head_len, 0);
        if (n <= 0) return;
        head_len += (size_t)n;
        head[head_len] = 0;
        if (strstr(head, "\r\n\r\n")) break;
    }
    if (!strstr(head, "\r\n\r\n")) return;

    char cl[32];
    if (hdr_value(head, "CONTENT-LENGTH", cl, sizeof(cl)) == 0)
        content_length = atoi(cl);
    if (content_length < 0) content_length = 0;
    if (content_length > 262144) content_length = 262144;

    /* body bytes may already follow the header terminator inside `head` */
    size_t already = 0;
    char *term = strstr(head, "\r\n\r\n");
    if (term) {
        size_t hdr_end = (size_t)(term - head) + 4;
        already = head_len > hdr_end ? head_len - hdr_end : 0;
    }

    if (content_length > 0) {
        body = malloc((size_t)content_length + 1);
        if (body) {
            size_t off = 0;
            if (already > 0) {
                size_t take = already > (size_t)content_length
                                  ? (size_t)content_length
                                  : already;
                memcpy(body, term + 4, take);
                off = take;
            }
            while (off < (size_t)content_length) {
                ssize_t n = recv(fd, body + off, (size_t)content_length - off, 0);
                if (n <= 0) break;
                off += (size_t)n;
            }
            body[off] = 0;
        }
    }

    char method[16] = {0}, path[512] = {0};
    (void)sscanf(head, "%15s %511s", method, path);

    if (!strcmp(method, "GET")) {
        if (!strcmp(path, "/dmr/desc.xml")) {
            char doc[6000];
            build_desc_xml(doc, sizeof(doc));
            http_reply_xml(fd, doc);
        } else if (!strcmp(path, "/dmr/avt-scpd.xml")) {
            char doc[14000];
            build_avt_scpd(doc, sizeof(doc));
            http_reply_xml(fd, doc);
        } else if (!strcmp(path, "/dmr/rcs-scpd.xml")) {
            char doc[8000];
            build_rcs_scpd(doc, sizeof(doc));
            http_reply_xml(fd, doc);
        } else if (!strcmp(path, "/dmr/cms-scpd.xml")) {
            char doc[6000];
            build_cms_scpd(doc, sizeof(doc));
            http_reply_xml(fd, doc);
        } else if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
            pp_dmr_status st;
            pp_dmr_get_status(&st);
            char page[1600];
            snprintf(page, sizeof(page),
                     "<html><body style=\"font-family:sans-serif\">"
                     "<h2>%s</h2><p>DLNA Media Renderer active on %s:%d</p>"
                     "<p>State: %d<br>URI: %s</p></body></html>",
                     st.name, st.ip, st.port, st.transport, st.uri);
            http_reply(fd, 200, "text/html", page, strlen(page));
        } else {
            http_reply(fd, 404, "text/plain", "not found", 9);
        }
    } else if (!strcmp(method, "POST")) {
        if (!body) body = strdup("");
        if (!strcmp(path, "/dmr/avt/control"))
            handle_soap_avt(fd, body);
        else if (!strcmp(path, "/dmr/rcs/control"))
            handle_soap_rcs(fd, body);
        else if (!strcmp(path, "/dmr/cms/control"))
            handle_soap_cms(fd, body);
        else
            http_reply(fd, 404, "text/plain", "not found", 9);
    } else if (!strcmp(method, "SUBSCRIBE")) {
        int svc = strstr(path, "rcs") ? 1 : 0;
        handle_subscribe(fd, head, svc, 0);
    } else if (!strcmp(method, "UNSUBSCRIBE")) {
        handle_subscribe(fd, head, 0, 1);
    } else {
        http_reply(fd, 404, "text/plain", "not found", 9);
    }

    free(body);
}

static void *http_thread(void *arg) {
    (void)arg;
    struct sockaddr_in addr;
    int on = 1;

    g_http_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_http_listen < 0) return NULL;
    setsockopt(g_http_listen, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PP_DMR_HTTP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(g_http_listen, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(g_http_listen, 8) < 0) {
        close(g_http_listen);
        g_http_listen = -1;
        return NULL;
    }

    while (!g_stop_req) {
        struct pollfd pfd = {g_http_listen, POLLIN, 0};
        int pr = poll(&pfd, 1, 500);
        if (pr <= 0) continue;
        int cfd = accept(g_http_listen, NULL, NULL);
        if (cfd < 0) continue;
        struct timeval tv = {6, 0};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        handle_connection(cfd);
        close(cfd);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* SSDP: M-SEARCH responder + NOTIFY alive/byebye                      */
/* ------------------------------------------------------------------ */

static void ssdp_sendto(int fd, const struct sockaddr_in *to, const char *msg) {
    (void)sendto(fd, msg, strlen(msg), 0, (const struct sockaddr *)to, sizeof(*to));
}

static void ssdp_make_location(char *out, size_t outsz) {
    char ip[64];
    pthread_mutex_lock(&g_mtx);
    snprintf(ip, sizeof(ip), "%s", g_ip);
    pthread_mutex_unlock(&g_mtx);
    snprintf(out, outsz, "http://%s:%d/dmr/desc.xml", ip, PP_DMR_HTTP_PORT);
}

/* one NOTIFY/response per NT type */
static const char *const g_ssdp_nts[] = {
    "upnp:rootdevice",
    PP_DMR_UDN,
    PP_DMR_DEVICE_TYPE,
    PP_DMR_SERVICE_AVT,
    PP_DMR_SERVICE_RCS,
    PP_DMR_SERVICE_CMS,
    NULL
};

static void ssdp_usn(const char *nt, char *out, size_t outsz) {
    if (!strcmp(nt, "upnp:rootdevice"))
        snprintf(out, outsz, PP_DMR_UDN "::upnp:rootdevice");
    else if (!strcmp(nt, PP_DMR_UDN))
        snprintf(out, outsz, "%s", PP_DMR_UDN);
    else
        snprintf(out, outsz, PP_DMR_UDN "::%s", nt);
}

static void ssdp_advertise(int fd, const char *nts_val) {
    struct sockaddr_in mcast;
    char loc[256], packet[1024], usn[256];

    ssdp_make_location(loc, sizeof(loc));
    memset(&mcast, 0, sizeof(mcast));
    mcast.sin_family = AF_INET;
    mcast.sin_port = htons(PP_DMR_SSDP_PORT);
    mcast.sin_addr.s_addr = inet_addr(PP_DMR_MCAST_ADDR);

    for (int i = 0; g_ssdp_nts[i]; i++) {
        ssdp_usn(g_ssdp_nts[i], usn, sizeof(usn));
        snprintf(packet, sizeof(packet),
                 "NOTIFY * HTTP/1.1\r\n"
                 "HOST: " PP_DMR_MCAST_ADDR ":1900\r\n"
                 "CACHE-CONTROL: max-age=%d\r\n"
                 "LOCATION: %s\r\n"
                 "NT: %s\r\n"
                 "NTS: %s\r\n"
                 "SERVER: " PP_DMR_SERVER_HDR "\r\n"
                 "USN: %s\r\n"
                 "BOOTID.UPNP.ORG: 1\r\n"
                 "CONFIGID.UPNP.ORG: 1\r\n\r\n",
                 PP_DMR_MAX_AGE, loc, g_ssdp_nts[i], nts_val, usn);
        ssdp_sendto(fd, &mcast, packet);
        usleep(20000);
    }
}

static int st_matches(const char *st) {
    if (!st) return 0;
    if (!strcmp(st, "ssdp:all")) return 1;
    for (int i = 0; g_ssdp_nts[i]; i++)
        if (!strcmp(st, g_ssdp_nts[i])) return 1;
    return 0;
}

static void ssdp_respond(int fd, const struct sockaddr_in *to, const char *st) {
    char loc[256], packet[1024], usn[256];

    ssdp_make_location(loc, sizeof(loc));
    ssdp_usn(st, usn, sizeof(usn));
    snprintf(packet, sizeof(packet),
             "HTTP/1.1 200 OK\r\n"
             "CACHE-CONTROL: max-age=%d\r\n"
             "EXT:\r\n"
             "LOCATION: %s\r\n"
             "SERVER: " PP_DMR_SERVER_HDR "\r\n"
             "ST: %s\r\n"
             "USN: %s\r\n"
             "BOOTID.UPNP.ORG: 1\r\n"
             "CONFIGID.UPNP.ORG: 1\r\n\r\n",
             PP_DMR_MAX_AGE, loc, st, usn);
    /* send twice — UDP loss is common on busy LANs */
    ssdp_sendto(fd, to, packet);
    usleep(20000);
    ssdp_sendto(fd, to, packet);
}

static void *ssdp_thread(void *arg) {
    (void)arg;
    struct sockaddr_in bind_addr, mcast;
    struct ip_mreq mreq;
    int on = 1;
    uint64_t last_alive = 0;

    g_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_fd < 0) return NULL;
    setsockopt(g_udp_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(PP_DMR_SSDP_PORT);
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(g_udp_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        close(g_udp_fd);
        g_udp_fd = -1;
        return NULL;
    }

    /* join the SSDP multicast group on the primary interface */
    char ip[64];
    pthread_mutex_lock(&g_mtx);
    snprintf(ip, sizeof(ip), "%s", g_ip);
    pthread_mutex_unlock(&g_mtx);
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr(PP_DMR_MCAST_ADDR);
    mreq.imr_interface.s_addr = ip[0] ? inet_addr(ip) : INADDR_ANY;
    (void)setsockopt(g_udp_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    memset(&mcast, 0, sizeof(mcast));
    mcast.sin_family = AF_INET;
    mcast.sin_port = htons(PP_DMR_SSDP_PORT);
    mcast.sin_addr.s_addr = inet_addr(PP_DMR_MCAST_ADDR);
    struct in_addr out_if;
    out_if.s_addr = ip[0] ? inet_addr(ip) : INADDR_ANY;
    (void)setsockopt(g_udp_fd, IPPROTO_IP, IP_MULTICAST_IF, &out_if, sizeof(out_if));

    /* initial announcement bursts */
    ssdp_advertise(g_udp_fd, "ssdp:alive");
    usleep(250000);
    ssdp_advertise(g_udp_fd, "ssdp:alive");
    last_alive = now_us();

    char buf[2048];
    while (!g_stop_req) {
        struct pollfd pfd = {g_udp_fd, POLLIN, 0};
        int pr = poll(&pfd, 1, 500);

        /* periodic re-announcement (~ every 15 min / 4 with jitter margin) */
        uint64_t t = now_us();
        if (t - last_alive > (uint64_t)(PP_DMR_MAX_AGE / 4) * 1000000ull) {
            ssdp_advertise(g_udp_fd, "ssdp:alive");
            last_alive = t;
        }

        if (pr <= 0) continue;
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(g_udp_fd, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&from, &flen);
        if (n <= 0) continue;
        buf[n] = 0;

        if (strncmp(buf, "M-SEARCH", 8)) continue;

        char st[128];
        if (hdr_value(buf, "ST", st, sizeof(st)) != 0) continue;
        if (!st_matches(st)) continue;
        ssdp_respond(g_udp_fd, &from, st);
    }

    ssdp_advertise(g_udp_fd, "ssdp:byebye");
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int pp_dmr_start(const char *friendly_name) {
    if (g_running) return 0;

    if (find_local_ip(g_ip, sizeof(g_ip)) != 0)
        snprintf(g_ip, sizeof(g_ip), "127.0.0.1");
    if (friendly_name && friendly_name[0])
        snprintf(g_name, sizeof(g_name), "%s", friendly_name);

    g_stop_req = 0;
    g_transport = PP_DMR_TRANSPORT_NO_MEDIA;
    g_uri[0] = 0;
    g_title[0] = 0;
    g_metadata[0] = 0;
    g_duration_s = 0.0;
    g_base_pos_s = 0.0;
    g_base_time_us = now_us();
    g_cmd_pending = 0;

    if (pthread_create(&g_http_tid, NULL, http_thread, NULL) != 0)
        return -1;
    if (pthread_create(&g_ssdp_tid, NULL, ssdp_thread, NULL) != 0) {
        g_stop_req = 1;
        pthread_join(g_http_tid, NULL);
        return -1;
    }
    g_running = 1;
    return 0;
}

void pp_dmr_stop(void) {
    if (!g_running) return;
    g_stop_req = 1;
    if (g_http_listen >= 0) {
        shutdown(g_http_listen, SHUT_RDWR);
        close(g_http_listen);
        g_http_listen = -1;
    }
    if (g_udp_fd >= 0) {
        close(g_udp_fd);
        g_udp_fd = -1;
    }
    pthread_join(g_http_tid, NULL);
    pthread_join(g_ssdp_tid, NULL);
    g_running = 0;
}

void pp_dmr_report_playing(void) {
    pthread_mutex_lock(&g_mtx);
    int changed = (g_transport != PP_DMR_TRANSPORT_PLAYING);
    g_transport = PP_DMR_TRANSPORT_PLAYING;
    g_base_time_us = now_us();
    pthread_mutex_unlock(&g_mtx);
    if (changed) event_notify(0);
}

void pp_dmr_report_paused(void) {
    pthread_mutex_lock(&g_mtx);
    int changed = (g_transport != PP_DMR_TRANSPORT_PAUSED);
    if (g_transport == PP_DMR_TRANSPORT_PLAYING)
        g_base_pos_s = position_now_locked();
    g_transport = PP_DMR_TRANSPORT_PAUSED;
    g_base_time_us = now_us();
    pthread_mutex_unlock(&g_mtx);
    if (changed) event_notify(0);
}

void pp_dmr_report_stopped(void) {
    pthread_mutex_lock(&g_mtx);
    int changed = (g_transport == PP_DMR_TRANSPORT_PLAYING ||
                   g_transport == PP_DMR_TRANSPORT_PAUSED ||
                   g_transport == PP_DMR_TRANSPORT_TRANSITIONING);
    g_base_pos_s = 0.0;
    g_base_time_us = now_us();
    g_transport = g_uri[0] ? PP_DMR_TRANSPORT_STOPPED : PP_DMR_TRANSPORT_NO_MEDIA;
    pthread_mutex_unlock(&g_mtx);
    if (changed) event_notify(0);
}

void pp_dmr_report_position(double position_s) {
    pthread_mutex_lock(&g_mtx);
    g_base_pos_s = position_s;
    g_base_time_us = now_us();
    pthread_mutex_unlock(&g_mtx);
}

void pp_dmr_get_status(pp_dmr_status *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&g_mtx);
    out->running = g_running;
    snprintf(out->name, sizeof(out->name), "%s", g_name);
    snprintf(out->ip, sizeof(out->ip), "%s", g_ip);
    out->port = PP_DMR_HTTP_PORT;
    out->transport = g_transport;
    snprintf(out->uri, sizeof(out->uri), "%s", g_uri);
    snprintf(out->title, sizeof(out->title), "%s", g_title);
    out->duration_s = g_duration_s;
    out->position_s = position_now_locked();
    out->volume = g_volume;
    out->mute = g_mute;
    pthread_mutex_unlock(&g_mtx);
}
