/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - small portable helpers.  DARKSOFT. */
#ifndef FREECUPL_UTIL_H
#define FREECUPL_UTIL_H
#include <stddef.h>

void  die(const char *fmt, ...);
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
/* Reads a whole file (binary); returns NULL on failure. *len gets the size.
   The buffer is NUL-terminated. */
char *read_file(const char *path, size_t *len);
int   write_file(const char *path, const char *buf, size_t len);
int   str_ieq(const char *a, const char *b);
int   str_nieq(const char *a, const char *b, size_t n);
char *str_lower(char *s);

typedef struct { char *p; size_t n, cap; } Buf;
void buf_putc(Buf *b, int c);
void buf_puts(Buf *b, const char *s);
void buf_printf(Buf *b, const char *fmt, ...);
void buf_free(Buf *b);

/* deterministic PRNG (xorshift64*), identical on every platform */
typedef struct { unsigned long long s; } Rng;
void rng_seed(Rng *r, unsigned long long seed);
unsigned long long rng_next(Rng *r);
unsigned rng_bit(Rng *r);

#endif
