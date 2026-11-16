/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - small portable helpers.  DARKSOFT. */
#include "util.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) die("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
    char *d = (char *)xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    Buf b = {0, 0, 0};
    char tmp[8192];
    size_t k;
    if (!f) return NULL;
    while ((k = fread(tmp, 1, sizeof tmp, f)) > 0) {
        size_t i;
        for (i = 0; i < k; i++) buf_putc(&b, tmp[i]);
    }
    fclose(f);
    buf_putc(&b, 0);
    b.n--;
    if (len) *len = b.n;
    return b.p;
}

int write_file(const char *path, const char *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(buf, 1, len, f) != len) { fclose(f); return -1; }
    return fclose(f) == 0 ? 0 : -1;
}

int str_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

int str_nieq(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
        if (!a[i]) return 1;
    }
    return 1;
}

char *str_lower(char *s)
{
    char *p;
    for (p = s; *p; p++) *p = (char)tolower((unsigned char)*p);
    return s;
}

void buf_putc(Buf *b, int c)
{
    if (b->n + 2 > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 256;
        b->p = (char *)xrealloc(b->p, b->cap);
    }
    b->p[b->n++] = (char)c;
    b->p[b->n] = 0;
}

void buf_puts(Buf *b, const char *s)
{
    while (*s) buf_putc(b, *s++);
}

void buf_printf(Buf *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    int k;
    va_start(ap, fmt);
    k = vsprintf(tmp, fmt, ap); /* callers keep formatted pieces short */
    va_end(ap);
    if (k < 0 || k >= (int)sizeof tmp) die("internal: buf_printf overflow");
    buf_puts(b, tmp);
}

void buf_free(Buf *b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

void rng_seed(Rng *r, unsigned long long seed)
{
    r->s = seed ? seed : 0x9E3779B97F4A7C15ULL;
}

unsigned long long rng_next(Rng *r)
{
    unsigned long long x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

unsigned rng_bit(Rng *r)
{
    return (unsigned)(rng_next(r) >> 63);
}
