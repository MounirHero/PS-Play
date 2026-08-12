/*
 * pp_http.h — minimal blocking HTTP/1.1 client for ProsperoPlayer.
 *
 * Only plain http:// (DLNA/UPnP and most IPTV playlists are plain HTTP).
 * All functions are blocking and must be called from a worker thread.
 */
#ifndef PP_HTTP_H
#define PP_HTTP_H

#include <stddef.h>

typedef struct {
    int status;        /* HTTP status code, 0 on transport error */
    char *body;        /* malloc'd, NUL-terminated (may contain binary) */
    size_t body_len;
} pp_http_response;

/*
 * Perform an HTTP request.
 *   method         "GET" or "POST"
 *   url            absolute http:// URL
 *   extra_headers  optional, e.g. "SOAPACTION: \"...\"\r\n" (may be NULL)
 *   body           optional request body (may be NULL)
 *   resp           output; body must be freed with pp_http_response_free()
 *   err/errsz      human readable error on failure
 * Returns 1 on HTTP-level success (any status), 0 on transport failure.
 */
int pp_http_request(const char *method, const char *url,
                    const char *extra_headers, const char *body,
                    pp_http_response *resp, char *err, size_t errsz);

void pp_http_response_free(pp_http_response *resp);

/* Resolve possibly-relative ref against base URL into out (out_sz). */
void pp_http_resolve_url(const char *base, const char *ref,
                         char *out, size_t out_sz);

/* Parse "http://host[:port]/path" — port defaults to 80, path to "/". */
int pp_http_parse_url(const char *url, char *host, size_t host_sz,
                      int *port, char *path, size_t path_sz);

#endif
