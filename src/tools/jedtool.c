/* SPDX-License-Identifier: GPL-3.0-or-later */
/* jedtool - JEDEC file utilities.  DARKSOFT. */
#include "jed.h"
#include "util.h"
#include "version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    fprintf(stderr, "jedtool %s (%s) - JEDEC file utilities\n"
            "usage: jedtool check FILE         verify checksums\n"
            "       jedtool fuses FILE         print fuses, one 0/1 each\n"
            "       jedtool rewrite IN OUT     rewrite, keep header text\n"
            "       jedtool diff A B           list differing fuses\n"
            "       jedtool flip IN OUT N...   invert fuses N...\n"
            "       jedtool stdheader IN OUT   rebuild the header block\n",
            FREECUPL_VERSION, FREECUPL_CREDIT);
    exit(2);
}

static void rd(const char *p, Jed *j)
{
    char err[256];
    if (jed_read_file(p, j, err, sizeof err) < 0) die("%s: %s", p, err);
}

int main(int argc, char **argv)
{
    Jed a, b;
    if (argc < 3) usage();
    if (!strcmp(argv[1], "check")) {
        unsigned c;
        int ok = 1;
        rd(argv[2], &a);
        c = jed_fuse_checksum(a.fuse, a.nfuses);
        if (!a.has_fcsum || a.fcsum != c) ok = 0;
        if (!a.has_xcsum || (a.xcsum != a.xcsum_calc)) ok = 0;
        printf("%s: QF%d C%04X(%s) X%04X(%s)\n", argv[2], a.nfuses, c,
               a.has_fcsum ? (a.fcsum == c ? "ok" : "BAD") : "absent", a.xcsum_calc,
               a.has_xcsum ? (a.xcsum == a.xcsum_calc ? "ok" : "BAD") : "absent");
        return ok ? 0 : 1;
    }
    if (!strcmp(argv[1], "fuses")) {
        int i;
        rd(argv[2], &a);
        for (i = 0; i < a.nfuses; i++) putchar('0' + a.fuse[i]);
        putchar('\n');
        return 0;
    }
    if (argc < 4) usage();
    if (!strcmp(argv[1], "rewrite") || !strcmp(argv[1], "flip")) {
        JedWriteOpts o;
        const char *hl[64];
        int nh = 0;
        char *h, *p;
        size_t len;
        char *out;
        rd(argv[2], &a);
        if (!strcmp(argv[1], "flip")) {
            int k;
            for (k = 4; k < argc; k++) {
                long n = atol(argv[k]);
                if (n < 0 || n >= a.nfuses) die("fuse %ld out of range", n);
                a.fuse[n] ^= 1;
            }
        }
        h = xstrdup(a.header ? a.header : "");
        for (p = strtok(h, "\r\n"); p && nh < 64; p = strtok(NULL, "\r\n")) hl[nh++] = p;
        memset(&o, 0, sizeof o);
        o.header = hl;
        o.nheader = nh;
        o.npins = a.npins;
        o.security = a.security > 0;
        out = jed_format(a.fuse, a.nfuses, &o, &len);
        if (write_file(argv[3], out, len) < 0) die("cannot write %s", argv[3]);
        free(out);
        free(h);
        return 0;
    }
    if (!strcmp(argv[1], "stdheader")) {
        JedHeader jh;
        JedWriteOpts o;
        char *lines[16], *h, *p, *dev = NULL, *lib = NULL;
        const char *raw[16];
        int nraw = 0, i, k, n;
        size_t len;
        char *out;
        rd(argv[2], &a);
        memset(&jh, 0, sizeof jh);
        h = xstrdup(a.header ? a.header : "");
        for (p = strtok(h, "\r\n"); p && nraw < 16; p = strtok(NULL, "\r\n")) raw[nraw++] = p;
        if (nraw != 11) die("%s: expected an 11-line header block", argv[2]);
        jh.tool = raw[0];
        /* "Device          <dev>  Library <lib>" */
        dev = xstrdup(raw[1] + 16);
        lib = strstr(dev, "  Library ");
        if (!lib) die("%s: no Library on Device line", argv[2]);
        *lib = 0;
        lib += 10;
        jh.device = dev;
        jh.library = lib;
        jh.created = raw[2] + 16;
        for (i = 0; i < JED_NFIELDS; i++) jh.field[i] = raw[3 + i] + 16;
        n = jed_std_header(&jh, lines);
        memset(&o, 0, sizeof o);
        o.header = (const char **)lines;
        o.nheader = n;
        o.npins = a.npins;
        o.security = a.security > 0;
        out = jed_format(a.fuse, a.nfuses, &o, &len);
        if (write_file(argv[3], out, len) < 0) die("cannot write %s", argv[3]);
        for (k = 0; k < n; k++) free(lines[k]);
        free(out);
        free(h);
        free(dev);
        return 0;
    }
    if (!strcmp(argv[1], "diff")) {
        int i, n = 0;
        rd(argv[2], &a);
        rd(argv[3], &b);
        if (a.nfuses != b.nfuses) { printf("fuse counts differ: %d %d\n", a.nfuses, b.nfuses); return 1; }
        for (i = 0; i < a.nfuses; i++) if (a.fuse[i] != b.fuse[i]) { printf("%d: %d %d\n", i, a.fuse[i], b.fuse[i]); n++; }
        return n ? 1 : 0;
    }
    usage();
    return 2;
}
