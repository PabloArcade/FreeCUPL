/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - JEDEC fuse file (JESD3) reader / writer.  DARKSOFT. */
#include "jed.h"
#include "util.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void jed_init(Jed *j)
{
    memset(j, 0, sizeof *j);
    j->fdefault = -1;
    j->security = -1;
}

void jed_free(Jed *j)
{
    int i;
    free(j->fuse);
    free(j->set);
    free(j->header);
    free(j->device);
    for (i = 0; i < j->nvec; i++) free(j->vec[i]);
    free(j->vec);
    for (i = 0; i < j->nnotes; i++) free(j->notes[i]);
    free(j->notes);
    jed_init(j);
}

void jed_alloc(Jed *j, int n, int v)
{
    free(j->fuse);
    free(j->set);
    j->nfuses = n;
    j->fuse = (unsigned char *)xmalloc((size_t)n + 1);
    j->set = (unsigned char *)xcalloc((size_t)n + 1, 1);
    memset(j->fuse, v ? 1 : 0, (size_t)n);
}

unsigned jed_fuse_checksum(const unsigned char *fuse, int n)
{
    unsigned sum = 0;
    int i;
    for (i = 0; i < n; i += 8) {
        unsigned byte = 0;
        int k;
        for (k = 0; k < 8 && i + k < n; k++)
            if (fuse[i + k]) byte |= 1u << k;
        sum = (sum + byte) & 0xFFFF;
    }
    return sum;
}

static void seterr(char *err, size_t errlen, const char *msg, long v)
{
    if (err && errlen) {
        char tmp[256];
        sprintf(tmp, "%.200s", msg);
        if (v >= 0) sprintf(tmp + strlen(tmp), " (%ld)", v);
        strncpy(err, tmp, errlen - 1);
        err[errlen - 1] = 0;
    }
}

static int is_ws(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

static void extract_device(Jed *j)
{
    const char *h = j->header, *p;
    if (!h) return;
    for (p = h; *p; ) {
        const char *e = p;
        while (*e && *e != '\n' && *e != '\r') e++;
        if (e - p > 6 && str_nieq(p, "Device", 6) && is_ws((unsigned char)p[6])) {
            const char *s = p + 6, *t;
            while (s < e && is_ws((unsigned char)*s)) s++;
            t = s;
            while (t < e && !is_ws((unsigned char)*t)) t++;
            if (t > s) j->device = xstrndup(s, (size_t)(t - s));
            return;
        }
        p = e;
        while (*p == '\n' || *p == '\r') p++;
    }
}

static int parse_uint(const char **pp, const char *end, long *out, int base)
{
    const char *p = *pp;
    long v = 0;
    int any = 0;
    while (p < end && is_ws((unsigned char)*p)) p++;
    while (p < end) {
        int c = (unsigned char)*p, d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = v * base + d;
        if (v > 100000000L) return -1;
        any = 1;
        p++;
    }
    if (!any) return -1;
    *out = v;
    *pp = p;
    return 0;
}

int jed_parse(const char *buf, size_t len, Jed *j, char *err, size_t errlen)
{
    const char *p = buf, *end = buf + len, *stx, *etx;
    jed_init(j);
    stx = memchr(buf, 0x02, len);
    if (stx) {
        j->has_stx = 1;
        p = stx + 1;
    }
    etx = memchr(p, 0x03, (size_t)(end - p));
    if (etx) {
        const char *q;
        unsigned s = 0;
        long v;
        j->has_etx = 1;
        for (q = stx ? stx : buf; q <= etx; q++) s += (unsigned char)*q;
        j->xcsum_calc = s & 0xFFFF;
        q = etx + 1;
        if (parse_uint(&q, end, &v, 16) == 0) {
            j->has_xcsum = 1;
            j->xcsum = (unsigned)v & 0xFFFF;
        }
        end = etx;
    }
    /* header: text up to the first '*' */
    {
        const char *star = memchr(p, '*', (size_t)(end - p));
        const char *he = star ? star : end;
        j->header = xstrndup(p, (size_t)(he - p));
        extract_device(j);
        p = he;
    }
    while (p < end) {
        const char *f, *fe;
        if (*p != '*') { p++; continue; }
        p++;
        f = p;
        while (f < end && is_ws((unsigned char)*f)) f++;
        if (f >= end) break;
        fe = memchr(f, '*', (size_t)(end - f));
        if (!fe) fe = end;
        p = fe;
        if (f >= fe) continue;
        switch (*f) {
        case 'Q': {
            const char *q = f + 2;
            long v;
            if (f + 1 >= fe) break;
            if (parse_uint(&q, fe, &v, 10) < 0) { seterr(err, errlen, "bad Q field", -1); return -1; }
            if (f[1] == 'F') {
                if (j->fuse) { seterr(err, errlen, "*QF after fuse data", -1); return -1; }
                jed_alloc(j, (int)v, j->fdefault > 0 ? 1 : 0);
            } else if (f[1] == 'P') j->npins = (int)v;
            else if (f[1] == 'V') j->nvectors = (int)v;
            break;
        }
        case 'F': {
            const char *q = f + 1;
            long v;
            if (parse_uint(&q, fe, &v, 10) < 0 || v > 1) { seterr(err, errlen, "bad F field", -1); return -1; }
            j->fdefault = (int)v;
            if (j->fuse) {
                int i;
                for (i = 0; i < j->nfuses; i++) if (!j->set[i]) j->fuse[i] = (unsigned char)v;
            }
            break;
        }
        case 'G': {
            const char *q = f + 1;
            long v;
            if (parse_uint(&q, fe, &v, 10) == 0) j->security = (int)v;
            break;
        }
        case 'L': {
            const char *q = f + 1;
            long addr;
            if (!j->fuse) { seterr(err, errlen, "*L before *QF", -1); return -1; }
            if (parse_uint(&q, fe, &addr, 10) < 0) { seterr(err, errlen, "bad L address", -1); return -1; }
            for (; q < fe; q++) {
                if (*q == '0' || *q == '1') {
                    if (addr >= j->nfuses) { seterr(err, errlen, "fuse address beyond QF", addr); return -1; }
                    j->fuse[addr] = (unsigned char)(*q - '0');
                    j->set[addr] = 1;
                    addr++;
                } else if (!is_ws((unsigned char)*q)) {
                    seterr(err, errlen, "bad character in L field at fuse", addr);
                    return -1;
                }
            }
            break;
        }
        case 'C': {
            const char *q = f + 1;
            long v;
            if (parse_uint(&q, fe, &v, 16) == 0) { j->has_fcsum = 1; j->fcsum = (unsigned)v & 0xFFFF; }
            break;
        }
        case 'V':
            j->vec = (char **)xrealloc(j->vec, sizeof(char *) * (size_t)(j->nvec + 1));
            j->vec[j->nvec++] = xstrndup(f, (size_t)(fe - f));
            break;
        case 'N':
            j->notes = (char **)xrealloc(j->notes, sizeof(char *) * (size_t)(j->nnotes + 1));
            j->notes[j->nnotes++] = xstrndup(f, (size_t)(fe - f));
            break;
        default:
            break; /* D, X, J, T, A, E, S, R, P ... ignored */
        }
    }
    if (!j->fuse) { seterr(err, errlen, "no *QF field", -1); return -1; }
    return 0;
}

int jed_read_file(const char *path, Jed *j, char *err, size_t errlen)
{
    size_t len;
    char *b = read_file(path, &len);
    int r;
    if (!b) {
        jed_init(j);
        seterr(err, errlen, "cannot read file", -1);
        return -1;
    }
    r = jed_parse(b, len, j, err, errlen);
    free(b);
    return r;
}

char *jed_format(const unsigned char *fuse, int nfuses, const JedWriteOpts *o, size_t *len)
{
    Buf b = {0, 0, 0};
    int i, a;
    unsigned sum = 0;
    size_t k;
    buf_putc(&b, 0x02);
    buf_puts(&b, "\r\n");
    for (i = 0; i < o->nheader; i++) {
        buf_puts(&b, o->header[i]);
        buf_puts(&b, "\r\n");
    }
    if (o->npins) buf_printf(&b, "*QP%d \r\n", o->npins);
    buf_printf(&b, "*QF%d \r\n", nfuses);
    if (o->nvectors) buf_printf(&b, "*QV%d \r\n", o->nvectors);
    buf_printf(&b, "*G%d \r\n", o->security ? 1 : 0);
    buf_puts(&b, "*F0 \r\n");
    for (a = 0; a < nfuses; a += 32) {
        int e = a + 32 < nfuses ? a + 32 : nfuses, any = 0;
        for (i = a; i < e; i++) if (fuse[i]) any = 1;
        if (!any) continue;
        buf_printf(&b, "*L%05d ", a);
        for (i = a; i < e; i++) buf_putc(&b, fuse[i] ? '1' : '0');
        buf_puts(&b, "\r\n");
    }
    buf_printf(&b, "*C%04X\r\n", jed_fuse_checksum(fuse, nfuses));
    for (i = 0; i < o->nvectors; i++) {
        buf_putc(&b, '*');
        buf_puts(&b, o->vectors[i]);
        buf_puts(&b, " \r\n");
    }
    buf_putc(&b, '*');
    buf_putc(&b, 0x03);
    for (k = 0; k < b.n; k++) sum += (unsigned char)b.p[k];
    buf_printf(&b, "%04X", sum & 0xFFFF);
    if (len) *len = b.n;
    return b.p;
}

const char *const jed_field_names[JED_NFIELDS] = {
    "Name", "Partno", "Revision", "Date", "Designer", "Company", "Assembly", "Location"
};

static char *hdr_line(const char *label, const char *v1, const char *sep, const char *v2)
{
    Buf b = {0, 0, 0};
    buf_puts(&b, label);
    while (b.n < 16) buf_putc(&b, ' ');
    buf_puts(&b, v1 ? v1 : "");
    if (sep) { buf_puts(&b, sep); buf_puts(&b, v2 ? v2 : ""); }
    return b.p;
}

int jed_std_header(const JedHeader *h, char **lines)
{
    int n = 0, i;
    lines[n++] = xstrdup(h->tool ? h->tool : "");
    lines[n++] = hdr_line("Device", h->device, "  Library ", h->library);
    lines[n++] = hdr_line("Created", h->created, NULL, NULL);
    for (i = 0; i < JED_NFIELDS; i++) lines[n++] = hdr_line(jed_field_names[i], h->field[i], NULL, NULL);
    return n;
}
