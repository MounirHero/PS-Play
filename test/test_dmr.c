/* Host test for pp_dmr: SSDP response, desc.xml, SOAP SetAVTransportURI+Play. */
#include "../net/pp_dmr.h"
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int udp_msearch(const char *st, char *resp, size_t rsz) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct timeval tv = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char req[512];
    int n = snprintf(req, sizeof(req),
                     "M-SEARCH * HTTP/1.1\r\n"
                     "HOST: 239.255.255.250:1900\r\n"
                     "MAN: \"ssdp:discover\"\r\n"
                     "MX: 2\r\n"
                     "ST: %s\r\n\r\n", st);
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(1900);
    to.sin_addr.s_addr = inet_addr("239.255.255.250");
    sendto(fd, req, n, 0, (struct sockaddr *)&to, sizeof(to));

    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    ssize_t r = recvfrom(fd, resp, rsz - 1, 0, (struct sockaddr *)&from, &fl);
    close(fd);
    if (r <= 0) return -1;
    resp[r] = 0;
    return 0;
}

static int tcp_http(const char *req, char *resp, size_t rsz) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = {4, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(9080);
    to.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(fd, (struct sockaddr *)&to, sizeof(to)) != 0) { close(fd); return -1; }
    send(fd, req, strlen(req), 0);
    size_t off = 0;
    ssize_t r;
    while (off < rsz - 1 && (r = recv(fd, resp + off, rsz - 1 - off, 0)) > 0)
        off += (size_t)r;
    close(fd);
    resp[off] = 0;
    return (int)off;
}

int main(void) {
    static char resp[65536];

    assert(pp_dmr_start("TestRenderer") == 0);
    printf("started\n");

    /* give threads a moment to bind */
    usleep(400000);

    /* 1. SSDP M-SEARCH -> response with LOCATION */
    assert(udp_msearch("urn:schemas-upnp-org:device:MediaRenderer:1", resp, sizeof(resp)) == 0);
    assert(strstr(resp, "LOCATION: http://"));
    assert(strstr(resp, ":9080/dmr/desc.xml"));
    assert(strstr(resp, "MediaRenderer:1"));
    printf("PASS ssdp msearch\n");

    /* 2. GET desc.xml */
    int n = tcp_http("GET /dmr/desc.xml HTTP/1.0\r\nHost: x\r\n\r\n", resp, sizeof(resp));
    assert(n > 0);
    assert(strstr(resp, "MediaRenderer:1"));
    assert(strstr(resp, "TestRenderer"));
    assert(strstr(resp, "AVTransport"));
    printf("PASS desc.xml\n");

    /* 3. GET SCPDs */
    n = tcp_http("GET /dmr/avt-scpd.xml HTTP/1.0\r\nHost: x\r\n\r\n", resp, sizeof(resp));
    assert(n > 0 && strstr(resp, "SetAVTransportURI") && strstr(resp, "GetPositionInfo"));
    n = tcp_http("GET /dmr/rcs-scpd.xml HTTP/1.0\r\nHost: x\r\n\r\n", resp, sizeof(resp));
    assert(n > 0 && strstr(resp, "SetVolume"));
    printf("PASS scpds\n");

    /* 4. SOAP SetAVTransportURI */
    const char *seturi =
        "POST /dmr/avt/control HTTP/1.0\r\nHost: x\r\n"
        "CONTENT-TYPE: text/xml\r\n"
        "SOAPACTION: \"urn:schemas-upnp-org:service:AVTransport:1#SetAVTransportURI\"\r\n"
        "CONTENT-LENGTH: %d\r\n\r\n%s";
    const char *body1 =
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
        "<u:SetAVTransportURI xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<InstanceID>0</InstanceID>"
        "<CurrentURI>http://phone.lan/media/film.mkv</CurrentURI>"
        "<CurrentURIMetaData>&lt;DIDL-Lite&gt;&lt;item&gt;&lt;dc:title&gt;Film Test&lt;/dc:title&gt;"
        "&lt;res duration=\"1:23:45\"&gt;http://phone.lan/media/film.mkv&lt;/res&gt;&lt;/item&gt;&lt;/DIDL-Lite&gt;</CurrentURIMetaData>"
        "</u:SetAVTransportURI></s:Body></s:Envelope>";
    char req[8192];
    snprintf(req, sizeof(req), seturi, (int)strlen(body1), body1);
    n = tcp_http(req, resp, sizeof(resp));
    assert(n > 0 && strstr(resp, "SetAVTransportURIResponse"));
    printf("PASS SetAVTransportURI\n");

    /* 5. SOAP Play -> command must arrive */
    const char *body2 =
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
        "<u:Play xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<InstanceID>0</InstanceID><Speed>1</Speed>"
        "</u:Play></s:Body></s:Envelope>";
    snprintf(req, sizeof(req), "POST /dmr/avt/control HTTP/1.0\r\nHost: x\r\nCONTENT-LENGTH: %d\r\n\r\n%s",
             (int)strlen(body2), body2);
    n = tcp_http(req, resp, sizeof(resp));
    assert(n > 0 && strstr(resp, "PlayResponse"));

    pp_dmr_cmd c;
    assert(pp_dmr_take_command(&c) == 1);
    assert(c.type == PP_DMR_CMD_PLAY_URI);
    assert(!strcmp(c.url, "http://phone.lan/media/film.mkv"));
    printf("PASS Play -> PLAY_URI command url=%s title=%s\n", c.url, c.title);

    /* status reflects metadata */
    pp_dmr_status st;
    pp_dmr_report_playing();
    pp_dmr_get_status(&st);
    assert(st.transport == PP_DMR_TRANSPORT_PLAYING);
    printf("status: transport=%d title=%s dur=%.0f\n", st.transport, st.title, st.duration_s);
    assert(st.duration_s > 5000.0 && st.duration_s < 5100.0); /* 1:23:45 = 5025 */
    assert(strstr(st.title, "Film Test"));

    /* 6. GetPositionInfo */
    const char *body3 =
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
        "<u:GetPositionInfo xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<InstanceID>0</InstanceID></u:GetPositionInfo></s:Body></s:Envelope>";
    snprintf(req, sizeof(req), "POST /dmr/avt/control HTTP/1.0\r\nHost: x\r\nCONTENT-LENGTH: %d\r\n\r\n%s",
             (int)strlen(body3), body3);
    n = tcp_http(req, resp, sizeof(resp));
    assert(n > 0 && strstr(resp, "TrackDuration>1:23:45"));
    assert(strstr(resp, "RelTime>"));
    printf("PASS GetPositionInfo\n");

    /* 7. Pause + Stop commands */
    const char *body4 =
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
        "<u:Pause xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<InstanceID>0</InstanceID></u:Pause></s:Body></s:Envelope>";
    snprintf(req, sizeof(req), "POST /dmr/avt/control HTTP/1.0\r\nHost: x\r\nCONTENT-LENGTH: %d\r\n\r\n%s",
             (int)strlen(body4), body4);
    n = tcp_http(req, resp, sizeof(resp));
    assert(n > 0);
    assert(pp_dmr_take_command(&c) == 1 && c.type == PP_DMR_CMD_PAUSE);
    printf("PASS Pause\n");

    /* 8. SUBSCRIBE (eventing) */
    const char *sub =
        "SUBSCRIBE /dmr/avt/events HTTP/1.0\r\nHost: x\r\n"
        "CALLBACK: <http://127.0.0.1:19999/ev>\r\nNT: upnp:event\r\n"
        "TIMEOUT: Second-1800\r\n\r\n";
    n = tcp_http(sub, resp, sizeof(resp));
    assert(n > 0 && strstr(resp, "SID: uuid:"));
    printf("PASS subscribe\n");

    /* 9. RenderingControl Get/SetVolume */
    const char *body5 =
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
        "<u:SetVolume xmlns:u=\"urn:schemas-upnp-org:service:RenderingControl:1\">"
        "<InstanceID>0</InstanceID><Channel>Master</Channel><DesiredVolume>42</DesiredVolume>"
        "</u:SetVolume></s:Body></s:Envelope>";
    snprintf(req, sizeof(req), "POST /dmr/rcs/control HTTP/1.0\r\nHost: x\r\nCONTENT-LENGTH: %d\r\n\r\n%s",
             (int)strlen(body5), body5);
    n = tcp_http(req, resp, sizeof(resp));
    assert(n > 0 && strstr(resp, "SetVolumeResponse"));
    assert(pp_dmr_take_command(&c) == 1 && c.type == PP_DMR_CMD_VOLUME && c.volume == 42);
    printf("PASS volume\n");

    /* 10. ConnectionManager GetProtocolInfo */
    const char *body6 =
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
        "<u:GetProtocolInfo xmlns:u=\"urn:schemas-upnp-org:service:ConnectionManager:1\">"
        "<InstanceID>0</InstanceID></u:GetProtocolInfo></s:Body></s:Envelope>";
    snprintf(req, sizeof(req), "POST /dmr/cms/control HTTP/1.0\r\nHost: x\r\nCONTENT-LENGTH: %d\r\n\r\n%s",
             (int)strlen(body6), body6);
    n = tcp_http(req, resp, sizeof(resp));
    assert(n > 0 && strstr(resp, "video/x-matroska"));
    printf("PASS protocolinfo\n");

    pp_dmr_stop();
    printf("ALL DMR TESTS PASS\n");
    return 0;
}
