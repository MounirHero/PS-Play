/*
 * pp_http.c — minimal blocking HTTP/1.1 client (PS5 / FreeBSD sockets).
 */
#include "pp_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PP_HTTP_CONNECT_TIMEOUT_MS 5000
#define PP_HTTP_IO_TIMEOUT_MS      8000
#define PP_HTTP_MAX_BODY           (4 * 1024 * 1024)

static void set_err(char *err, size_t errsz, const char *msg) {
    if (err && errsz) {
        snprintf(err, errsz, "%s", msg);
    }
}

int pp_http_parse_url(const char *url, char *host, size_t host_sz,
                      int *port, char *path, size_t path_sz) {
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        return -1; /* TLS not supported by this mini client */
    }

    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : p + strlen(p);
    const char *colon = NULL;
    for (const char *c = p; c < hostend; c++) {
        if (*c == ':') colon = c;
    }

    size_t hlen = colon ? (size_t)(colon - p) : (size_t)(hostend - p);
    if (hlen == 0 || hlen >= host_sz) return -1;
    memcpy(host, p, hlen);
    host[hlen] = 0;

    *port = 80;
    if (colon) {
        *port = atoi(colon + 1);
        if (*port <= 0 || *port > 65535) *port = 80;
    }

    if (slash) {
        snprintf(path, path_sz, "%s", slash);
    } else {
        snprintf(path, path_sz, "/");
    }
    return 0;
}

void pp_http_resolve_url(const char *base, const char *ref,
                         char *out, size_t out_sz) {
    if (!ref || !*ref) {
        snprintf(out, out_sz, "%s", base ? base : "");
        return;
    }
    if (strncmp(ref, "http://", 7) == 0 || strncmp(ref, "https://", 8) == 0) {
        snprintf(out, out_sz, "%s", ref);
        return;
    }

    char host[256], path[1024];
    int port;
    if (!base || pp_http_parse_url(base, host, sizeof(host),
                                   &port, path, sizeof(path)) != 0) {
        snprintf(out, out_sz, "%s", ref);
        return;
    }

    if (ref[0] == '/') {
        if (port == 80)
            snprintf(out, out_sz, "http://%s%s", host, ref);
        else
            snprintf(out, out_sz, "http://%s:%d%s", host, port, ref);
        return;
    }

    /* relative: replace last path segment of base */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    char *last = strrchr(dir, '/');
    if (last) last[1] = 0;
    else snprintf(dir, sizeof(dir), "/");

    if (port == 80)
        snprintf(out, out_sz, "http://%s%s%s", host, dir, ref);
    else
        snprintf(out, out_sz, "http://%s:%d%s%s", host, port, dir, ref);
}

static int tcp_connect_timeout(const char *host, int port, char *err, size_t errsz) {
    struct addrinfo hints, *res = NULL, *rp;
    char portstr[16];
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        set_err(err, errsz, "DNS lookup failed");
        return -1;
    }

    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        /* non-blocking connect with timeout */
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            struct pollfd pfd = { fd, POLLOUT, 0 };
            rc = poll(&pfd, 1, PP_HTTP_CONNECT_TIMEOUT_MS);
            if (rc > 0) {
                int soerr = 0;
                socklen_t sl = sizeof(soerr);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
                if (soerr == 0) {
                    fcntl(fd, F_SETFL, flags); /* back to blocking */
                    break;
                }
            }
            close(fd);
            fd = -1;
            continue;
        } else if (rc == 0) {
            fcntl(fd, F_SETFL, flags);
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    if (fd < 0) set_err(err, errsz, "connection failed (server unreachable)");
    return fd;
}

static int send_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        struct pollfd pfd = { fd, POLLOUT, 0 };
        if (poll(&pfd, 1, PP_HTTP_IO_TIMEOUT_MS) <= 0) return -1;
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

/* Read one header line (CRLF terminated). Returns length or <=0 on error. */
static int read_line(int fd, char *buf, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        struct pollfd pfd = { fd, POLLIN, 0 };
        if (poll(&pfd, 1, PP_HTTP_IO_TIMEOUT_MS) <= 0) return -1;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '\n') {
            if (n > 0 && buf[n - 1] == '\r') n--;
            buf[n] = 0;
            return (int)n;
        }
        buf[n++] = c;
    }
    buf[n] = 0;
    return (int)n;
}

static int read_exact(int fd, char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        if (poll(&pfd, 1, PP_HTTP_IO_TIMEOUT_MS) <= 0) return -1;
        ssize_t n = recv(fd, buf + off, len - off, 0);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static int body_append(char **body, size_t *len, size_t *cap,
                       const char *data, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t ncap = (*cap ? *cap * 2 : 65536);
        while (ncap < *len + n + 1) ncap *= 2;
        if (ncap > PP_HTTP_MAX_BODY) ncap = PP_HTTP_MAX_BODY + 1;
        if (*len + n + 1 > ncap) return -1;
        char *nb = realloc(*body, ncap);
        if (!nb) return -1;
        *body = nb;
        *cap = ncap;
    }
    memcpy(*body + *len, data, n);
    *len += n;
    (*body)[*len] = 0;
    return 0;
}

static int read_chunked(int fd, char **body, size_t *len, size_t *cap) {
    char line[128];
    for (;;) {
        if (read_line(fd, line, sizeof(line)) < 0) return -1;
        unsigned long chunk = strtoul(line, NULL, 16);
        if (chunk == 0) {
            /* trailing headers until empty line */
            while (read_line(fd, line, sizeof(line)) > 0) { }
            return 0;
        }
        if (chunk > PP_HTTP_MAX_BODY) return -1;
        char *tmp = malloc(chunk);
        if (!tmp) return -1;
        int rc = read_exact(fd, tmp, chunk);
        if (rc == 0) rc = body_append(body, len, cap, tmp, chunk);
        free(tmp);
        /* consume trailing CRLF */
        char crlf[2];
        read_exact(fd, crlf, 2);
        if (rc != 0) return -1;
    }
}

int pp_http_request(const char *method, const char *url,
                    const char *extra_headers, const char *body,
                    pp_http_response *resp, char *err, size_t errsz) {
    char host[256], path[2048];
    int port;
    int ok = 0;

    memset(resp, 0, sizeof(*resp));

    if (pp_http_parse_url(url, host, sizeof(host), &port,
                          path, sizeof(path)) != 0) {
        set_err(err, errsz, "unsupported or malformed URL (only http://)");
        return 0;
    }

    int fd = tcp_connect_timeout(host, port, err, errsz);
    if (fd < 0) return 0;

    size_t body_len = body ? strlen(body) : 0;
    size_t req_cap = strlen(method) + strlen(path) + strlen(host) +
                     (extra_headers ? strlen(extra_headers) : 0) + 256;
    char *req = malloc(req_cap);
    if (!req) {
        close(fd);
        set_err(err, errsz, "out of memory");
        return 0;
    }

    char host_hdr[320];
    if (port == 80)
        snprintf(host_hdr, sizeof(host_hdr), "%s", host);
    else
        snprintf(host_hdr, sizeof(host_hdr), "%s:%d", host, port);

    int rn = snprintf(req, req_cap,
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: ProsperoPlayer/1.1 UPnP/1.1\r\n"
        "Connection: close\r\n"
        "%s"
        "Content-Length: %zu\r\n"
        "\r\n",
        method, path, host_hdr,
        extra_headers ? extra_headers : "",
        body_len);

    if (rn > 0 && send_all(fd, req, (size_t)rn) == 0) {
        if (body_len == 0 || send_all(fd, body, body_len) == 0) {
            ok = 1;
        }
    }
    free(req);

    if (!ok) {
        close(fd);
        set_err(err, errsz, "failed to send request");
        return 0;
    }

    /* status line */
    char line[2048];
    if (read_line(fd, line, sizeof(line)) <= 0 ||
        sscanf(line, "HTTP/%*s %d", &resp->status) != 1) {
        close(fd);
        set_err(err, errsz, "no valid HTTP response");
        return 0;
    }

    /* headers */
    long content_length = -1;
    int chunked = 0;
    while (read_line(fd, line, sizeof(line)) > 0) {
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            content_length = atol(line + 15);
        } else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0 &&
                   strstr(line + 18, "chunked")) {
            chunked = 1;
        }
    }

    char *rbody = NULL;
    size_t rlen = 0, rcap = 0;
    ok = 0;

    if (chunked) {
        ok = (read_chunked(fd, &rbody, &rlen, &rcap) == 0);
    } else if (content_length >= 0) {
        if (content_length > PP_HTTP_MAX_BODY) content_length = PP_HTTP_MAX_BODY;
        rbody = malloc((size_t)content_length + 1);
        if (rbody) {
            ok = (read_exact(fd, rbody, (size_t)content_length) == 0);
            if (ok) {
                rlen = (size_t)content_length;
                rbody[rlen] = 0;
            } else {
                free(rbody);
                rbody = NULL;
            }
        }
    } else {
        /* read until close */
        char buf[16384];
        for (;;) {
            struct pollfd pfd = { fd, POLLIN, 0 };
            int pr = poll(&pfd, 1, PP_HTTP_IO_TIMEOUT_MS);
            if (pr < 0) break;
            if (pr == 0) { ok = (rlen > 0); break; }
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n < 0) break;
            if (n == 0) { ok = 1; break; }
            if (body_append(&rbody, &rlen, &rcap, buf, (size_t)n) != 0) break;
        }
    }

    close(fd);

    if (!ok || !rbody) {
        free(rbody);
        set_err(err, errsz, "failed to read response body");
        return 0;
    }

    resp->body = rbody;
    resp->body_len = rlen;
    return 1;
}

void pp_http_response_free(pp_http_response *resp) {
    if (resp && resp->body) {
        free(resp->body);
        resp->body = NULL;
        resp->body_len = 0;
    }
}
