/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl serve - HTTP/1.1 requests: parsing and the checks on Host,
   Origin and the session token.  No sockets here.  DARKSOFT. */
#ifndef FREECUPL_HTTP_H
#define FREECUPL_HTTP_H
#include <stddef.h>

#define HTTP_MAX_HEAD 8192            /* request line and header fields */
#define HTTP_MAX_BODY (4u << 20)      /* a source file */

typedef struct {
    char method[8];         /* "GET" or "POST" */
    char path[128];         /* without the query, not decoded */
    char query[1024];       /* after the '?', not decoded */
    char host[128];
    char origin[128];       /* "" when the field is absent */
    char token[80];         /* X-FreeCUPL-Token */
    const char *body;       /* into the buffer given to http_parse */
    size_t body_len;
} HttpReq;

/* buf[0..n-1] is what has arrived.  Returns the length of the complete
   request, 0 when more must be read, or minus the HTTP status that
   answers a request not accepted (400, 405, 411, 413, 414, 431, 501,
   505). */
int http_parse(const char *buf, size_t n, HttpReq *r);

/* The same for the header alone: 1 when it is complete (its length and
   that of the body it announces are returned; r has no body yet), 0
   when more must be read, or minus a status. */
int http_head(const char *buf, size_t n, HttpReq *r, size_t *headlen, size_t *bodylen);

/* Value of key in "a=1&b=x%20y", decoded.  Returns 0; -1 when the key
   is absent, the value does not fit or holds a NUL or a bad escape. */
int http_query_get(const char *query, const char *key, char *out, size_t n);

/* Host must be 127.0.0.1:port or localhost:port (no port given: 80) */
int http_host_ok(const char *host, int port);
/* Origin must be http://127.0.0.1:port or http://localhost:port */
int http_origin_ok(const char *origin, int port);
/* compares in time that depends on the length only */
int http_token_eq(const char *a, const char *b);

const char *http_reason(int status);

#endif
