/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - output files.  DARKSOFT. */
#include "output.h"
#include "diag.h"
#include "jed.h"
#include "version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *hdr_or_x(int h)
{
    return g_des.hdr[h] ? g_des.hdr[h] : "xxxxxx";
}

const char *created_string(void)
{
    static char buf[64];
    time_t t;
    const char *e = getenv("SOURCE_DATE_EPOCH");
    struct tm *tm;
    if (e && *e) {
        t = (time_t)strtol(e, NULL, 10);
        tm = gmtime(&t);
    } else {
        t = time(NULL);
        tm = localtime(&t);
    }
    if (!tm) return "";
    {
        static const char *const wd[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
        static const char *const mo[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
        snprintf(buf, sizeof buf, "%s %s %2d %02d:%02d:%02d %d", wd[tm->tm_wday % 7], mo[tm->tm_mon % 12],
                 tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
    }
    return buf;
}

static char *tool_line(void)
{
    Buf b = {0, 0, 0};
    buf_puts(&b, FREECUPL_VERSION_STRING);
    return b.p;
}

int write_jed(const char *path, const FitResult *r, int security)
{
    JedHeader h;
    char *lines[16];
    JedWriteOpts o;
    char *txt, *tool = tool_line();
    size_t len;
    int n, i, rc;
    memset(&h, 0, sizeof h);
    h.tool = tool;
    h.device = r->resolved;
    h.library = dev_libtag(r->dev, r->resolved);
    h.created = created_string();
    h.field[0] = hdr_or_x(H_NAME);
    h.field[1] = hdr_or_x(H_PARTNO);
    h.field[2] = hdr_or_x(H_REV);
    h.field[3] = hdr_or_x(H_DATE);
    h.field[4] = hdr_or_x(H_DESIGNER);
    h.field[5] = hdr_or_x(H_COMPANY);
    h.field[6] = hdr_or_x(H_ASSEMBLY);
    h.field[7] = hdr_or_x(H_LOCATION);
    n = jed_std_header(&h, lines);
    memset(&o, 0, sizeof o);
    o.header = (const char **)lines;
    o.nheader = n;
    o.npins = r->dev->npins;
    o.security = security;
    txt = jed_format(r->fuse, r->dev->nfuses, &o, &len);
    rc = write_file(path, txt, len);
    for (i = 0; i < n; i++) free(lines[i]);
    free(txt);
    free(tool);
    return rc;
}

static void put_cover(Buf *b, const Cover *f)
{
    int i, v;
    if (f->n == 0) { buf_puts(b, "    0 \n"); return; }
    for (i = 0; i < f->n; i++) {
        int first = 1, any = 0;
        buf_puts(b, i ? "  # " : "    ");
        for (v = 0; v < MAXVARS; v++) {
            int pol = cube_lit(&f->c[i], v), ext;
            const char *nm;
            if (pol < 0) continue;
            nm = fit_var_name(v, &ext);
            buf_printf(b, "%s%s%s", first ? "" : " & ", pol ? "" : "!", nm);
            if (ext) buf_printf(b, ".%s", ext_names[ext]);
            first = 0;
            any = 1;
        }
        if (!any) buf_puts(b, "1 ");
        buf_putc(b, '\n');
    }
}

static void banner(Buf *b, const char *title)
{
    int pad = (79 - (int)strlen(title)) / 2;
    buf_puts(b, "===============================================================================\n");
    buf_printf(b, "%*s%s\n", pad > 0 ? pad : 0, "", title);
    buf_puts(b, "===============================================================================\n\n");
}

int write_doc(const char *path, const FitResult *r, int fuseplot, int expanded)
{
    Buf b = {0, 0, 0};
    char *tool = tool_line();
    int i, k;
    const char *nm = hdr_or_x(H_NAME);
    int pad = (79 - (int)strlen(nm)) / 2;
    buf_puts(&b, "*******************************************************************************\n");
    buf_printf(&b, "%*s%s\n", pad > 0 ? pad : 0, "", nm);
    buf_puts(&b, "*******************************************************************************\n\n");
    buf_printf(&b, "%s\n", tool);
    buf_printf(&b, "%-16s%s  Library %s\n", "Device", r->resolved, dev_libtag(r->dev, r->resolved));
    buf_printf(&b, "%-16s%s\n", "Created", created_string());
    for (i = 0; i < 8; i++) {
        static const int hi[8] = { H_NAME, H_PARTNO, H_REV, H_DATE, H_DESIGNER, H_COMPANY, H_ASSEMBLY, H_LOCATION };
        buf_printf(&b, "%-16s%s\n", jed_field_names[i], hdr_or_x(hi[i]));
    }
    buf_puts(&b, "\n");
    if (expanded) {
        banner(&b, "Expanded Product Terms");
        for (i = 0; i < r->ndoc; i++) {
            const DocEq *d = &r->doc[i];
            buf_printf(&b, "%s%s%s%s =>\n", d->inv ? "!" : "", d->sym->name, d->ext ? "." : "", d->ext ? ext_names[d->ext] : "");
            put_cover(&b, &d->f);
            buf_puts(&b, "\n");
        }
        buf_puts(&b, "\n");
        banner(&b, "Symbol Table");
        buf_puts(&b, "Pin Variable                                    Pterms   Max     Min    \n");
        buf_puts(&b, "Pol   Name              Ext     Pin     Type     Used   Pterms  Level   \n");
        buf_puts(&b, "--- --------            ---     ---     ----    ------  ------  -----   \n\n");
        for (k = 0; k < g_des.nsyms; k++) {
            Sym *s = g_des.syms[k];
            if (s->kind == SK_PIN) {
                const DocEq *d = NULL;
                for (i = 0; i < r->ndoc; i++) if (r->doc[i].sym == s && (r->doc[i].ext == X_NONE || r->doc[i].ext == X_D)) d = &r->doc[i];
                if (d) buf_printf(&b, " %s  %-18s%-8s%-9d%-9s%-8d%-8d%-8d\n", s->neg ? "!" : " ", s->name, "", s->pin, "V", d->used, d->max, d->level);
                else buf_printf(&b, " %s  %-18s%-8s%-9d%-9s%-8s%-8s%-8s\n", s->neg ? "!" : " ", s->name, "", s->pin, "V", "-", "-", "-");
            }
        }
        for (k = 0; k < g_des.nsyms; k++) {
            Sym *s = g_des.syms[k];
            if (s->kind == SK_FIELD) buf_printf(&b, "    %-18s%-8s%-9d%-9s%-8s%-8s%-8s\n", s->name, "", 0, "F", "-", "-", "-");
            else if (s->kind == SK_NONE && eqn_find(s, X_NONE) && !s->hidden)
                buf_printf(&b, "    %-18s%-8s%-9d%-9s%-8s%-8s%-8s\n", s->name, "", 0, "I", "-", "-", "-");
        }
        for (i = 0; i < r->ndoc; i++) {
            const DocEq *d = &r->doc[i];
            if (d->ext == X_NONE || d->ext == X_D) continue;
            buf_printf(&b, " %s  %-18s%-8s%-9d%-9s%-8d%-8d%-8d\n", " ", d->sym->name, ext_names[d->ext], d->sym->pin, "D", d->used, d->max, d->level);
        }
        buf_puts(&b, "\n\nLEGEND    D : default variable         F : field      G : group\n");
        buf_puts(&b, "          I : intermediate variable    N : node       M : extended node\n");
        buf_puts(&b, "          U : undefined                V : variable   X : extended variable\n");
        buf_puts(&b, "          T : function\n\n");
    }
    if (fuseplot) {
        const Device *d = r->dev;
        int row, c;
        banner(&b, "Fuse Plot");
        for (row = 0; row < d->nrows; row++) {
            int any = 0;
            for (c = 0; c < d->ncols; c++) if (r->fuse[dev_row_addr(d, row) + c]) any = 1;
            buf_printf(&b, "%04d ", dev_row_addr(d, row));
            for (c = 0; c < d->ncols; c++) {
                int f = r->fuse[dev_row_addr(d, row) + c];
                buf_putc(&b, any ? (f ? '-' : 'x') : 'x');
            }
            buf_putc(&b, '\n');
        }
        buf_puts(&b, "\nLEGEND    X : fuse not blown\n          - : fuse blown\n\n");
        buf_puts(&b, "Configuration fuses:");
        for (c = d->ncols * d->nrows; c < d->nfuses; c++) {
            if ((c - d->ncols * d->nrows) % 64 == 0) buf_printf(&b, "\n%05d ", c);
            buf_putc(&b, r->fuse[c] ? '1' : '0');
        }
        buf_puts(&b, "\n\n");
    }
    i = write_file(path, b.p, b.n);
    buf_free(&b);
    free(tool);
    return i;
}

int write_lst(const char *path, const char *srcname)
{
    Buf b = {0, 0, 0};
    int n, i;
    char **src = pp_source_lines(&n);
    buf_printf(&b, "LISTING FOR LOGIC DESCRIPTION FILE: %s\n\n", srcname);
    buf_printf(&b, "freecupl: CUPL compatible PLD compiler, %s\n", FREECUPL_VERSION_STRING);
    buf_printf(&b, "Created %s\n\n", created_string());
    for (i = 0; i < n; i++) buf_printf(&b, "%3d:%s\n", i + 1, src[i]);
    if (g_diag.list.n) {
        buf_puts(&b, "\n");
        buf_puts(&b, g_diag.list.p);
    }
    i = write_file(path, b.p, b.n);
    buf_free(&b);
    return i;
}

int write_mx(const char *path, const PPOut *pp)
{
    Buf b = {0, 0, 0};
    int i;
    for (i = 0; i < pp->n; i++) { buf_puts(&b, pp->v[i].text); buf_puts(&b, "\r\n"); }
    i = write_file(path, b.p ? b.p : "", b.n);
    buf_free(&b);
    return i;
}
