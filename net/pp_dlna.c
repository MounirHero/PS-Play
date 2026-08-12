/*
 * pp_dlna.c — DLNA/UPnP client (SSDP + device description + SOAP browse).
 *
 * C port of the UPnP stack from ps5-payload-dev/dlnaplay
 * (GPL-3.0-or-later). Namespace-agnostic minimal XML scanning: matches
 * elements by local name so any prefix a server chooses still works.
 */
#include "pp_dlna.h"
#include "pp_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SSDP_ADDR "239.255.255.250"
#define SSDP_PORT 1900

/* ------------------------------------------------------------------ */
/* Small XML helpers (namespace-agnostic, no external deps)           */
/* ------------------------------------------------------------------ */

/* Compare element local name ("service" for "u:service"). */
static int xml_name_is(const char *name, size_t name_len, const char *local) {
    const char *colon = memchr(name, ':', name_len);
    const char *ln = colon ? colon + 1 : name;
    size_t ln_len = colon ? name_len - (size_t)(colon - name) - 1 : name_len;
    return strlen(local) == ln_len && strncmp(ln, local, ln_len) == 0;
}

/*
 * Find first child element of [start,end) whose local name matches.
 * Returns pointer just past '>' of the open tag, sets *tag_open (start
 * of '<') and *tag_close (start of matching close tag). NULL if none.
 */
static const char *xml_find_element(const char *start, const char *end,
                                    const char *local,
                                    const char **tag_open,
                                    const char **tag_close) {
    const char *p = start;
    while (p && p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt || lt + 1 >= end) return NULL;
        if (lt[1] == '/' || lt[1] == '!' || lt[1] == '?') {
            p = lt + 2;
            continue;
        }
        /* read tag name */
        const char *ns = lt + 1;
        const char *ne = ns;
        while (ne < end && (isalnum((unsigned char)*ne) || *ne == ':' ||
                            *ne == '_' || *ne == '-' || *ne == '.')) ne++;
        size_t nlen = (size_t)(ne - ns);
        /* find end of open tag */
        const char *gt = memchr(ne, '>', (size_t)(end - ne));
        if (!gt) return NULL;

        if (xml_name_is(ns, nlen, local)) {
            /* find matching close tag, respecting nesting of same name */
            int depth = 1;
            const char *q = gt + 1;
            /* self-closing? */
            if (gt > ns && gt[-1] == '/') {
                if (tag_open) *tag_open = lt;
                if (tag_close) *tag_close = gt; /* empty body */
                return gt + 1;
            }
            while (q < end) {
                const char *c = memchr(q, '<', (size_t)(end - q));
                if (!c) break;
                if (c + 1 < end && c[1] == '/') {
                    const char *cs = c + 2;
                    const char *ce = cs;
                    while (ce < end && (isalnum((unsigned char)*ce) ||
                                        *ce == ':' || *ce == '_' ||
                                        *ce == '-' || *ce == '.')) ce++;
                    if (xml_name_is(cs, (size_t)(ce - cs), local)) {
                        depth--;
                        if (depth == 0) {
                            if (tag_open) *tag_open = lt;
                            if (tag_close) *tag_close = c;
                            return gt + 1;
                        }
                    }
                    q = ce;
                } else if (c + 1 < end && (isalnum((unsigned char)c[1]))) {
                    const char *cs = c + 1;
                    const char *ce = cs;
                    while (ce < end && (isalnum((unsigned char)*ce) ||
                                        *ce == ':' || *ce == '_' ||
                                        *ce == '-' || *ce == '.')) ce++;
                    const char *cgt = memchr(ce, '>', (size_t)(end - ce));
                    if (!cgt) break;
                    if (xml_name_is(cs, (size_t)(ce - cs), local) &&
                        cgt[-1] != '/') {
                        depth++;
                    }
                    q = cgt + 1;
                } else {
                    q = c + 2;
                }
            }
            return NULL; /* unbalanced */
        }
        p = gt + 1;
    }
    return NULL;
}

/* Copy text of first matching child element (entity-unescaped). */
static void xml_child_text(const char *blk_start, const char *blk_end,
                           const char *local, char *out, size_t out_sz);
static void xml_unescape(const char *s, size_t n, char *out, size_t out_sz) {
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < out_sz; i++) {
        if (s[i] == '&') {
            if (i + 4 < n && strncmp(s + i, "&amp;", 5) == 0) {
                out[o++] = '&'; i += 4; continue;
            }
            if (i + 3 < n && strncmp(s + i, "&lt;", 4) == 0) {
                out[o++] = '<'; i += 3; continue;
            }
            if (i + 3 < n && strncmp(s + i, "&gt;", 4) == 0) {
                out[o++] = '>'; i += 3; continue;
            }
            if (i + 5 < n && strncmp(s + i, "&quot;", 6) == 0) {
                out[o++] = '"'; i += 5; continue;
            }
            if (i + 5 < n && strncmp(s + i, "&apos;", 6) == 0) {
                out[o++] = '\''; i += 5; continue;
            }
        }
        out[o++] = s[i];
    }
    out[o] = 0;
}

static void xml_child_text(const char *blk_start, const char *blk_end,
                           const char *local, char *out, size_t out_sz) {
    const char *open, *close;
    out[0] = 0;
    const char *body = xml_find_element(blk_start, blk_end, local, &open, &close);
    if (!body || close < body) return;
    xml_unescape(body, (size_t)(close - body), out, out_sz);
}

/* Extract attribute value from an open tag text [tag_start, tag_end). */
static void xml_attr(const char *tag_start, const char *tag_end,
                     const char *attr, char *out, size_t out_sz) {
    out[0] = 0;
    size_t alen = strlen(attr);
    const char *p = tag_start;
    while (p < tag_end) {
        const char *a = NULL;
        /* find attr= or attr = */
        const char *f = memchr(p, '=', (size_t)(tag_end - p));
        if (!f) return;
        /* walk back over spaces */
        const char *ne = f;
        while (ne > p && (ne[-1] == ' ' || ne[-1] == '\t')) ne--;
        const char *ns = ne;
        while (ns > p && (isalnum((unsigned char)ns[-1]) || ns[-1] == '_' ||
                          ns[-1] == '-' || ns[-1] == ':')) ns--;
        size_t nlen = (size_t)(ne - ns);
        const char *v = f + 1;
        while (v < tag_end && (*v == ' ' || *v == '\t')) v++;
        if (v >= tag_end || (*v != '"' && *v != '\'')) { p = f + 1; continue; }
        char q = *v++;
        const char *ve = memchr(v, q, (size_t)(tag_end - v));
        if (!ve) return;
        int match = 0;
        if (nlen == alen && strncmp(ns, attr, alen) == 0) match = 1;
        /* namespace-prefixed attr (rare): compare local part */
        if (!match) {
            const char *colon = memchr(ns, ':', nlen);
            if (colon) {
                size_t llen = nlen - (size_t)(colon - ns) - 1;
                if (llen == alen && strncmp(colon + 1, attr, alen) == 0) match = 1;
            }
        }
        if (match) {
            a = v;
            xml_unescape(a, (size_t)(ve - a), out, out_sz);
            return;
        }
        p = ve + 1;
    }
}

/* Case-insensitive substring search in [hay,hay_end). */
static const char *memcasestr_n(const char *hay, size_t haylen, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0 || haylen < nl) return NULL;
    for (size_t i = 0; i + nl <= haylen; i++) {
        if (strncasecmp(hay + i, needle, nl) == 0) return hay + i;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* SSDP discovery                                                      */
/* ------------------------------------------------------------------ */

static const char *MSEARCH =
    "M-SEARCH * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:1900\r\n"
    "MAN: \"ssdp:discover\"\r\n"
    "MX: 2\r\n"
    "ST: urn:schemas-upnp-org:device:MediaServer:1\r\n"
    "USER-AGENT: ProsperoPlayer/1.1 UPnP/1.1\r\n"
    "\r\n";

/* Case-insensitive "Header: value" lookup in an SSDP response. */
static void ssdp_header(const char *resp, const char *name,
                        char *out, size_t out_sz) {
    out[0] = 0;
    size_t nl = strlen(name);
    const char *p = resp;
    while (*p) {
        const char *eol = strstr(p, "\r\n");
        size_t ll = eol ? (size_t)(eol - p) : strlen(p);
        const char *colon = memchr(p, ':', ll);
        if (colon && (size_t)(colon - p) == nl &&
            strncasecmp(p, name, nl) == 0) {
            const char *v = colon + 1;
            while (v < p + ll && *v == ' ') v++;
            size_t vl = (size_t)(p + ll - v);
            if (vl >= out_sz) vl = out_sz - 1;
            memcpy(out, v, vl);
            out[vl] = 0;
            return;
        }
        if (!eol) break;
        p = eol + 2;
    }
}

int pp_dlna_discover(pp_dlna_server *servers, int max_servers,
                     int timeout_ms, char *err, size_t errsz) {
    int count = 0;
    char locations[32][PP_DLNA_MAX_URL];
    int loc_count = 0;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        snprintf(err, errsz, "cannot create UDP socket");
        return -1;
    }

    /* Pick the primary non-loopback IPv4 interface for multicast. */
    struct in_addr ifaddr;
    ifaddr.s_addr = INADDR_ANY;
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) == 0 && ifs) {
        for (struct ifaddrs *ifa = ifs; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
            ifaddr = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
            break;
        }
        freeifaddrs(ifs);
    }
    if (ifaddr.s_addr != INADDR_ANY) {
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr));
        struct sockaddr_in bind_addr;
        memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr = ifaddr;
        bind_addr.sin_port = 0;
        bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
    }
    unsigned char ttl = 4;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(SSDP_PORT);
    dest.sin_addr.s_addr = inet_addr(SSDP_ADDR);

    /* Send the search twice: one packet can get lost on busy WiFi. */
    for (int i = 0; i < 2; i++) {
        sendto(fd, MSEARCH, strlen(MSEARCH), 0,
               (struct sockaddr *)&dest, sizeof(dest));
        usleep(150 * 1000);
    }

    /* Collect responses until timeout with no new data. */
    long deadline = timeout_ms;
    for (;;) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int slice = deadline > 700 ? 700 : (int)deadline;
        if (slice <= 0) break;
        int pr = poll(&pfd, 1, slice);
        deadline -= slice;
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) {
            if (loc_count > 0 || deadline <= 0) break;
            continue;
        }
        char buf[2048];
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) continue;
        buf[n] = 0;

        char loc[PP_DLNA_MAX_URL];
        ssdp_header(buf, "LOCATION", loc, sizeof(loc));
        if (!loc[0]) continue;

        int dup = 0;
        for (int i = 0; i < loc_count; i++) {
            if (strcmp(locations[i], loc) == 0) { dup = 1; break; }
        }
        if (!dup && loc_count < 32) {
            snprintf(locations[loc_count++], sizeof(locations[0]), "%s", loc);
        }
        if (deadline < 800) deadline = 800; /* brief extension per reply */
    }
    close(fd);

    if (loc_count == 0) {
        snprintf(err, errsz, "no DLNA server answered the network search");
        return 0;
    }

    /* Fetch each device description. */
    for (int i = 0; i < loc_count && count < max_servers; i++) {
        pp_http_response resp;
        char derr[160] = {0};
        if (!pp_http_request("GET", locations[i], NULL, NULL, &resp,
                             derr, sizeof(derr))) {
            continue;
        }
        if (resp.status == 200 && resp.body) {
            pp_dlna_server *s = &servers[count];
            memset(s, 0, sizeof(*s));
            snprintf(s->location, sizeof(s->location), "%s", locations[i]);

            const char *body = resp.body;
            size_t blen = resp.body_len;

            /* Only MediaServer devices */
            if (!memcasestr_n(body, blen, ":device:MediaServer:")) {
                pp_http_response_free(&resp);
                continue;
            }

            xml_child_text(body, body + blen, "friendlyName",
                           s->friendly_name, sizeof(s->friendly_name));
            char manuf[PP_DLNA_MAX_NAME] = {0}, model[PP_DLNA_MAX_NAME] = {0};
            xml_child_text(body, body + blen, "manufacturer", manuf, sizeof(manuf));
            xml_child_text(body, body + blen, "modelName", model, sizeof(model));
            if (manuf[0] && model[0])
                snprintf(s->model, sizeof(s->model), "%s %s", manuf, model);
            else
                snprintf(s->model, sizeof(s->model), "%s%s", manuf, model);
            xml_child_text(body, body + blen, "UDN", s->udn, sizeof(s->udn));

            /* Find ContentDirectory service -> controlURL */
            const char *p = body;
            const char *bend = body + blen;
            while (p < bend) {
                const char *sopen, *sclose;
                const char *sbody = xml_find_element(p, bend, "service",
                                                     &sopen, &sclose);
                if (!sbody) break;
                char stype[160] = {0};
                xml_child_text(sbody, sclose, "serviceType", stype, sizeof(stype));
                if (strstr(stype, ":service:ContentDirectory:")) {
                    char curl[PP_DLNA_MAX_URL] = {0};
                    xml_child_text(sbody, sclose, "controlURL", curl, sizeof(curl));
                    if (curl[0]) {
                        pp_http_resolve_url(locations[i], curl,
                                            s->control_url, sizeof(s->control_url));
                    }
                    break;
                }
                p = sclose + 1;
            }

            /* display host */
            {
                char h[256], pa[256];
                int po;
                if (pp_http_parse_url(locations[i], h, sizeof(h), &po,
                                      pa, sizeof(pa)) == 0) {
                    snprintf(s->host, sizeof(s->host), "%s", h);
                }
            }

            if (!s->friendly_name[0])
                snprintf(s->friendly_name, sizeof(s->friendly_name), "DLNA Server");

            if (s->control_url[0]) count++;
        }
        pp_http_response_free(&resp);
    }

    if (count == 0) {
        snprintf(err, errsz, "devices found but none offers DLNA content");
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* ContentDirectory browse                                             */
/* ------------------------------------------------------------------ */

static void xml_escape_to(const char *in, char *out, size_t out_sz) {
    size_t o = 0;
    for (const char *p = in; *p && o + 6 < out_sz; p++) {
        switch (*p) {
        case '&': memcpy(out + o, "&amp;", 5); o += 5; break;
        case '<': memcpy(out + o, "&lt;", 4); o += 4; break;
        case '>': memcpy(out + o, "&gt;", 4); o += 4; break;
        case '"': memcpy(out + o, "&quot;", 6); o += 6; break;
        default: out[o++] = *p; break;
        }
    }
    out[o] = 0;
}

static void parse_didl_object(const char *blk_open, const char *body,
                              const char *blk_close, int is_container,
                              const pp_dlna_server *server,
                              pp_dlna_object *o) {
    memset(o, 0, sizeof(*o));
    o->is_container = is_container;
    o->child_count = -1;
    o->size_bytes = -1;

    /* attributes live on the open tag: from '<' to matching '>' */
    const char *gt = memchr(blk_open, '>', (size_t)(body - blk_open + 1));
    if (!gt) gt = body;
    xml_attr(blk_open, gt, "id", o->id, sizeof(o->id));
    xml_attr(blk_open, gt, "parentID", o->parent_id, sizeof(o->parent_id));
    if (is_container) {
        char cc[24] = {0};
        xml_attr(blk_open, gt, "childCount", cc, sizeof(cc));
        if (cc[0]) o->child_count = atoi(cc);
    }

    xml_child_text(body, blk_close, "title", o->title, sizeof(o->title));
    xml_child_text(body, blk_close, "class", o->upnp_class, sizeof(o->upnp_class));
    xml_child_text(body, blk_close, "artist", o->artist, sizeof(o->artist));
    if (!o->artist[0])
        xml_child_text(body, blk_close, "creator", o->artist, sizeof(o->artist));
    xml_child_text(body, blk_close, "album", o->album, sizeof(o->album));

    if (is_container) return;

    /* pick best <res>: first http-get that is not an image preview */
    const char *p = body;
    char fallback_url[PP_DLNA_MAX_URL] = {0};
    char fallback_pi[192] = {0};
    char fallback_dur[24] = {0};
    char fallback_res[24] = {0};
    char fallback_size[32] = {0};
    while (p < blk_close) {
        const char *ropen, *rclose;
        const char *rbody = xml_find_element(p, blk_close, "res", &ropen, &rclose);
        if (!rbody) break;
        const char *rgt = memchr(ropen, '>', (size_t)(rbody - ropen + 1));
        if (!rgt) rgt = rbody;

        char url[PP_DLNA_MAX_URL] = {0};
        xml_unescape(rbody, (size_t)(rclose - rbody), url, sizeof(url));
        /* trim whitespace some servers pad with */
        {
            char *s = url;
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
            if (s != url) memmove(url, s, strlen(s) + 1);
            size_t L = strlen(url);
            while (L > 0 && (url[L-1] == ' ' || url[L-1] == '\t' ||
                             url[L-1] == '\n' || url[L-1] == '\r')) url[--L] = 0;
        }
        char pi[192] = {0}, dur[24] = {0}, res[24] = {0}, size[32] = {0};
        xml_attr(ropen, rgt, "protocolInfo", pi, sizeof(pi));
        xml_attr(ropen, rgt, "duration", dur, sizeof(dur));
        xml_attr(ropen, rgt, "resolution", res, sizeof(res));
        xml_attr(ropen, rgt, "size", size, sizeof(size));

        if (url[0]) {
            int is_image_res = (strstr(pi, ":image/") != NULL);
            if (!is_image_res || pp_dlna_object_is_image(o)) {
                if (!fallback_url[0]) {
                    snprintf(fallback_url, sizeof(fallback_url), "%s", url);
                    snprintf(fallback_pi, sizeof(fallback_pi), "%s", pi);
                    snprintf(fallback_dur, sizeof(fallback_dur), "%s", dur);
                    snprintf(fallback_res, sizeof(fallback_res), "%s", res);
                    snprintf(fallback_size, sizeof(fallback_size), "%s", size);
                }
                if (strncmp(pi, "http-get", 8) == 0 && !o->res_url[0]) {
                    snprintf(o->res_url, sizeof(o->res_url), "%s", url);
                    snprintf(o->duration, sizeof(o->duration), "%s", dur);
                    snprintf(o->resolution, sizeof(o->resolution), "%s", res);
                    if (size[0]) o->size_bytes = atoll(size);
                    (void)fallback_pi;
                }
            }
        }
        p = rclose + 1;
    }
    if (!o->res_url[0] && fallback_url[0]) {
        snprintf(o->res_url, sizeof(o->res_url), "%s", fallback_url);
        snprintf(o->duration, sizeof(o->duration), "%s", fallback_dur);
        snprintf(o->resolution, sizeof(o->resolution), "%s", fallback_res);
        if (fallback_size[0]) o->size_bytes = atoll(fallback_size);
    }

    /* Resolve relative URLs against the description location. */
    if (o->res_url[0] && strncmp(o->res_url, "http", 4) != 0) {
        char abs[PP_DLNA_MAX_URL];
        pp_http_resolve_url(server->location, o->res_url, abs, sizeof(abs));
        snprintf(o->res_url, sizeof(o->res_url), "%s", abs);
    }
}

int pp_dlna_browse(const pp_dlna_server *server, const char *object_id,
                   pp_dlna_object *items, int max_items,
                   char *err, size_t errsz) {
    char oid_esc[PP_DLNA_MAX_ID * 2];
    xml_escape_to(object_id, oid_esc, sizeof(oid_esc));

    char soap[2048];
    snprintf(soap, sizeof(soap),
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
        " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body>"
        "<u:Browse xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">"
        "<ObjectID>%s</ObjectID>"
        "<BrowseFlag>BrowseDirectChildren</BrowseFlag>"
        "<Filter>*</Filter>"
        "<StartingIndex>0</StartingIndex>"
        "<RequestedCount>0</RequestedCount>"
        "<SortCriteria></SortCriteria>"
        "</u:Browse>"
        "</s:Body>"
        "</s:Envelope>",
        oid_esc);

    pp_http_response resp;
    if (!pp_http_request("POST", server->control_url,
                         "Content-Type: text/xml; charset=\"utf-8\"\r\n"
                         "SOAPACTION: \"urn:schemas-upnp-org:service:ContentDirectory:1#Browse\"\r\n",
                         soap, &resp, err, errsz)) {
        return -1;
    }

    if (resp.status != 200) {
        snprintf(err, errsz, "server rejected browse (HTTP %d)", resp.status);
        pp_http_response_free(&resp);
        return -1;
    }

    /* Envelope -> Body -> BrowseResponse -> Result (escaped DIDL) */
    const char *body = resp.body;
    const char *bend = resp.body + resp.body_len;
    const char *ropen, *rclose;
    const char *rbody = xml_find_element(body, bend, "Result", &ropen, &rclose);
    if (!rbody || rclose <= rbody) {
        snprintf(err, errsz, "empty or unexpected SOAP response");
        pp_http_response_free(&resp);
        return -1;
    }

    /* Unescape DIDL document */
    size_t didl_len = (size_t)(rclose - rbody);
    char *didl = malloc(didl_len + 1);
    if (!didl) {
        pp_http_response_free(&resp);
        snprintf(err, errsz, "out of memory");
        return -1;
    }
    xml_unescape(rbody, didl_len, didl, didl_len + 1);
    pp_http_response_free(&resp);

    /* Iterate container/item elements in document order */
    int count = 0;
    const char *p = didl;
    const char *dend = didl + strlen(didl);
    while (p < dend && count < max_items) {
        const char *lt = memchr(p, '<', (size_t)(dend - p));
        if (!lt) break;
        if (lt + 1 >= dend || lt[1] == '/' || lt[1] == '!' || lt[1] == '?') {
            p = lt + 2;
            continue;
        }
        const char *ns = lt + 1;
        const char *ne = ns;
        while (ne < dend && (isalnum((unsigned char)*ne) || *ne == ':' ||
                             *ne == '_' || *ne == '-' || *ne == '.')) ne++;
        int is_container = xml_name_is(ns, (size_t)(ne - ns), "container");
        int is_item = xml_name_is(ns, (size_t)(ne - ns), "item");
        if (!is_container && !is_item) {
            const char *gt = memchr(ne, '>', (size_t)(dend - ne));
            p = gt ? gt + 1 : dend;
            continue;
        }

        const char *local = is_container ? "container" : "item";
        const char *oopen, *oclose;
        const char *obody = xml_find_element(lt, dend, local, &oopen, &oclose);
        if (!obody) break;

        parse_didl_object(oopen, obody, oclose, is_container,
                          server, &items[count]);

        /* skip entries without title and non-containers without resource */
        if (items[count].title[0] &&
            (is_container || items[count].res_url[0])) {
            count++;
        }
        p = oclose + 1;
    }

    free(didl);

    if (count == 0) {
        snprintf(err, errsz, "folder is empty");
    }
    return count;
}

int pp_dlna_object_is_video(const pp_dlna_object *o) {
    return strncmp(o->upnp_class, "object.item.videoItem", 21) == 0;
}
int pp_dlna_object_is_audio(const pp_dlna_object *o) {
    return strncmp(o->upnp_class, "object.item.audioItem", 21) == 0;
}
int pp_dlna_object_is_image(const pp_dlna_object *o) {
    return strncmp(o->upnp_class, "object.item.imageItem", 21) == 0;
}
