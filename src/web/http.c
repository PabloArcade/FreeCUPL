/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl serve - HTTP requests.  DARKSOFT. */
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* field name of len bytes equals name, letters in any case */
static int name_is(const char *f, size_t len, const char *name)
{
    size_t i;
    if (strlen(name) != len) return 0;
    for (i = 0; i < len; i++)
        if (lower((unsigned char)f[i]) != name[i]) return 0;
    return 1;
}

/* copies a field value; -1 when it does not fit */
static int put(char *out, size_t n, const char *v, size_t len)
{
    if (len >= n) return -1;
    memcpy(out, v, len);
    out[len] = 0;
    return 0;
}

int http_head(const char *buf, size_t n, HttpReq *r, size_t *headlen, size_t *bodylen)
{
    const char *end = NULL, *p, *e, *sp1, *sp2, *q;
    size_t i, head;
    long clen = -1;
    int have_host = 0;

    /* end of the header: an empty line */
    for (i = 0; i + 3 < n && i < HTTP_MAX_HEAD; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
            buf[i + 3] == '\n') {
            end = buf + i;
            break;
        }
    if (!end) return n >= HTTP_MAX_HEAD ? -431 : 0;
    head = (size_t)(end - buf) + 4;
    for (i = 0; i < head; i++)
        if (!buf[i]) return -400;

    memset(r, 0, sizeof *r);
    /* METHOD SP target SP HTTP/1.x */
    e = memchr(buf, '\r', head);
    sp1 = memchr(buf, ' ', (size_t)(e - buf));
    if (!sp1) return -400;
    sp2 = memchr(sp1 + 1, ' ', (size_t)(e - sp1 - 1));
    if (!sp2) return -400;
    if (e - sp2 - 1 != 8 || memcmp(sp2 + 1, "HTTP/1.", 7) ||
        (sp2[8] != '0' && sp2[8] != '1'))
        return e - sp2 - 1 >= 5 && !memcmp(sp2 + 1, "HTTP/", 5) ? -505 : -400;
    if (sp1 - buf == 3 && !memcmp(buf, "GET", 3)) strcpy(r->method, "GET");
    else if (sp1 - buf == 4 && !memcmp(buf, "POST", 4)) strcpy(r->method, "POST");
    else return -405;
    /* origin form only */
    if (sp1[1] != '/') return -400;
    for (p = sp1 + 1; p < sp2; p++)
        if ((unsigned char)*p <= 32 || *p == 127) return -400;
    q = memchr(sp1 + 1, '?', (size_t)(sp2 - sp1 - 1));
    if (put(r->path, sizeof r->path, sp1 + 1, (size_t)((q ? q : sp2) - sp1 - 1)))
        return -414;
    if (q && put(r->query, sizeof r->query, q + 1, (size_t)(sp2 - q - 1)))
        return -414;

    for (p = e + 2; p < end + 2; p = e + 2) {
        const char *colon, *v, *ve;
        size_t nl;
        e = memchr(p, '\r', (size_t)(end + 2 - p));
        if (!e || e[1] != '\n') return -400;
        /* no folded lines */
        if (*p == ' ' || *p == '\t') return -400;
        colon = memchr(p, ':', (size_t)(e - p));
        if (!colon || colon == p) return -400;
        nl = (size_t)(colon - p);
        if (p[nl - 1] == ' ' || p[nl - 1] == '\t') return -400;
        v = colon + 1;
        ve = e;
        while (v < ve && (*v == ' ' || *v == '\t')) v++;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        if (name_is(p, nl, "host")) {
            if (have_host++ || put(r->host, sizeof r->host, v, (size_t)(ve - v)))
                return -400;
        } else if (name_is(p, nl, "origin")) {
            if (r->origin[0] || ve == v ||
                put(r->origin, sizeof r->origin, v, (size_t)(ve - v)))
                return -400;
        } else if (name_is(p, nl, "x-freecupl-token")) {
            if (put(r->token, sizeof r->token, v, (size_t)(ve - v))) return -400;
        } else if (name_is(p, nl, "content-length")) {
            const char *d;
            if (clen >= 0 || ve == v || ve - v > 9) return -400;
            for (clen = 0, d = v; d < ve; d++) {
                if (*d < '0' || *d > '9') return -400;
                clen = clen * 10 + (*d - '0');
            }
        } else if (name_is(p, nl, "transfer-encoding")) {
            return -501;
        }
    }
    if (!have_host) return -400;
    if (!strcmp(r->method, "POST")) {
        if (clen < 0) return -411;
        if ((unsigned long)clen > HTTP_MAX_BODY) return -413;
    } else if (clen > 0) {
        return -400;
    }
    if (clen < 0) clen = 0;
    *headlen = head;
    *bodylen = (size_t)clen;
    return 1;
}

int http_parse(const char *buf, size_t n, HttpReq *r)
{
    size_t head, clen;
    int st = http_head(buf, n, r, &head, &clen);
    if (st <= 0) return st;
    if (n - head < clen) return 0;
    r->body = buf + head;
    r->body_len = clen;
    return (int)(head + clen);
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = lower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int http_query_get(const char *query, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    const char *p = query;
    while (*p) {
        const char *e = strchr(p, '&');
        if (!e) e = p + strlen(p);
        if ((size_t)(e - p) > kl && !memcmp(p, key, kl) && p[kl] == '=') {
            size_t k = 0;
            for (p += kl + 1; p < e; p++) {
                int c = (unsigned char)*p;
                if (c == '+') {
                    c = ' ';
                } else if (c == '%') {
                    int h, l;
                    if (e - p < 3 || (h = hexval((unsigned char)p[1])) < 0 ||
                        (l = hexval((unsigned char)p[2])) < 0)
                        return -1;
                    c = h * 16 + l;
                    p += 2;
                    if (!c) return -1;
                }
                if (k + 1 >= n) return -1;
                out[k++] = (char)c;
            }
            out[k] = 0;
            return 0;
        }
        p = *e ? e + 1 : e;
    }
    return -1;
}

/* s is "127.0.0.1" or "localhost" (any case) followed by ":port" */
static int local_authority(const char *s, int port)
{
    static const char *const names[] = { "127.0.0.1", "localhost" };
    size_t i, k;
    for (i = 0; i < 2; i++) {
        size_t l = strlen(names[i]);
        for (k = 0; k < l; k++)
            if (lower((unsigned char)s[k]) != names[i][k]) break;
        if (k < l) continue;
        if (!s[l]) return port == 80;
        if (s[l] == ':') {
            char want[16];
            snprintf(want, sizeof want, "%d", port);
            return !strcmp(s + l + 1, want);
        }
    }
    return 0;
}

int http_host_ok(const char *host, int port)
{
    return local_authority(host, port);
}

int http_origin_ok(const char *origin, int port)
{
    if (strncmp(origin, "http://", 7)) return 0;
    return local_authority(origin + 7, port);
}

int http_token_eq(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b), i;
    unsigned d = 0;
    if (la != lb || !la) return 0;
    for (i = 0; i < la; i++) d |= (unsigned)(a[i] ^ b[i]);
    return d == 0;
}

const char *http_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 303: return "See Other";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    }
    return "Error";
}
