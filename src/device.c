/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - data-driven device descriptions.  DARKSOFT. */
#include "device.h"
#include "util.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Device **g_dev;
static int g_ndev;

int dev_count(void) { return g_ndev; }
const Device *dev_get(int i) { return (i >= 0 && i < g_ndev) ? g_dev[i] : NULL; }

const Device *dev_find(const char *m, const DevAlias **alias)
{
    int i, k;
    for (i = 0; i < g_ndev; i++) {
        for (k = 0; k < g_dev[i]->nalias; k++)
            if (str_ieq(g_dev[i]->alias[k].name, m)) {
                if (alias) *alias = &g_dev[i]->alias[k];
                return g_dev[i];
            }
    }
    for (i = 0; i < g_ndev; i++)
        if (str_ieq(g_dev[i]->name, m)) {
            if (alias) *alias = NULL;
            return g_dev[i];
        }
    return NULL;
}

int dev_fuse(const Device *d, int r, int c)
{
    if (!d->vr_addr) return c < d->ncols ? dev_row_addr(d, r) + c : -1;
    if (c >= d->vr_v0[r] && c < d->vr_v0[r] + d->vr_w0[r]) return d->vr_addr[r] + c - d->vr_v0[r];
    if (c >= d->vr_v1[r] && c < d->vr_v1[r] + d->vr_w1[r]) return d->vr_addr[r] + d->vr_w0[r] + c - d->vr_v1[r];
    return -1;
}

int dev_row_addr(const Device *d, int r)
{
    int i;
    if (d->vr_addr) return d->vr_addr[r];
    for (i = 0; i < d->nrowat; i++) if (d->rowat_row[i] == r) return d->rowat_addr[i];
    return r * d->ncols;
}

int dev_mc_at(const Device *d, int pin)
{
    int i;
    for (i = 0; i < d->nmc; i++) if (d->mc[i].pin == pin) return i;
    return -1;
}

int dev_mode_of(const Device *d, const unsigned char *fuse)
{
    int m, c;
    if (d->nmodes <= 1) return 0;
    for (m = 0; m < d->nmodes; m++) {
        int ok = 1;
        for (c = 0; c < d->modes[m].ncond; c++)
            if (fuse[d->modes[m].cond_fuse[c]] != d->modes[m].cond_val[c]) ok = 0;
        if (ok) return m;
    }
    return -1;
}

const char *dev_libtag(const Device *d, const char *resolved)
{
    int i;
    for (i = 0; i < d->nlibtag; i++) if (str_ieq(d->libtag[i][0], resolved)) return d->libtag[i][1];
    return "";
}

const LineSrc *dev_lines(const Device *d, int mode)
{
    if (mode < 0 || mode >= d->nmodes) return NULL;
    return d->modes[mode].lines;
}

typedef struct { const char *src; int line; int err; } Ctx;

static void perr(Ctx *c, const char *msg, const char *tok)
{
    fprintf(stderr, "%s:%d: %s%s%s\n", c->src, c->line, msg, tok ? ": " : "", tok ? tok : "");
    c->err = 1;
}

#define MAXTOK 128
static int tokenize(char *s, char **tok)
{
    int n = 0;
    char *h = strchr(s, '#');
    if (h) *h = 0;
    while (*s) {
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) break;
        if (n == MAXTOK) return -1;
        tok[n++] = s;
        while (*s && !isspace((unsigned char)*s)) s++;
        if (*s) *s++ = 0;
    }
    return n;
}

static int to_int(Ctx *c, const char *s)
{
    char *e;
    long v = strtol(s, &e, 10);
    if (!*s || *e || v < -1 || v > 10000000L) { perr(c, "bad number", s); return 0; }
    return (int)v;
}

static void range(Ctx *c, const char *s, int *a, int *b)
{
    const char *d = strchr(s, '-');
    char tmp[32];
    if (!d) { *a = *b = to_int(c, s); return; }
    if ((size_t)(d - s) >= sizeof tmp) { perr(c, "bad range", s); return; }
    memcpy(tmp, s, (size_t)(d - s));
    tmp[d - s] = 0;
    *a = to_int(c, tmp);
    *b = to_int(c, d + 1);
    if (*b < *a) perr(c, "bad range", s);
}

static int fuse_ref(Ctx *c, Device *d, const char *s)
{
    int i;
    if (isdigit((unsigned char)*s)) return to_int(c, s);
    for (i = 0; i < d->nfusenames; i++) if (!strcmp(d->fusename[i], s)) return d->fuseaddr[i];
    perr(c, "unknown fuse name", s);
    return 0;
}

static Device *dev_new(void)
{
    Device *d = (Device *)xcalloc(1, sizeof *d);
    d->ar_row = d->sp_row = d->ap_row = -1;
    d->ptd_base = -1;
    d->galreg_unused_ac1 = 1;
    d->ues_base = -1;
    d->pd_fuse = -1;
    d->nmodes = 0;
    return d;
}

static Device *dev_clone(const Device *s)
{
    Device *d = (Device *)xmalloc(sizeof *d);
    int m;
    *d = *s;
    d->mc = (Macrocell *)xmalloc(sizeof(Macrocell) * (size_t)(s->nmc + 1));
    memcpy(d->mc, s->mc, sizeof(Macrocell) * (size_t)s->nmc);
    for (m = 0; m < s->nmodes; m++) {
        if (s->modes[m].lines) {
            d->modes[m].lines = (LineSrc *)xmalloc(sizeof(LineSrc) * (size_t)s->nlines);
            memcpy(d->modes[m].lines, s->modes[m].lines, sizeof(LineSrc) * (size_t)s->nlines);
        }
    }
    if (s->vr_addr) {
        int **dst[5], *const *src[5], q;
        dst[0] = &d->vr_addr; dst[1] = &d->vr_v0; dst[2] = &d->vr_w0; dst[3] = &d->vr_v1; dst[4] = &d->vr_w1;
        src[0] = &s->vr_addr; src[1] = &s->vr_v0; src[2] = &s->vr_w0; src[3] = &s->vr_v1; src[4] = &s->vr_w1;
        for (q = 0; q < 5; q++) {
            *dst[q] = (int *)xmalloc(sizeof(int) * (size_t)(s->nrows + 1));
            memcpy(*dst[q], *src[q], sizeof(int) * (size_t)s->nrows);
        }
    }
    d->nalias = 0;
    d->alias = NULL;
    if (s->nlibtag) {
        d->libtag = (char (*)[2][24])xmalloc(sizeof(*d->libtag) * (size_t)s->nlibtag);
        memcpy(d->libtag, s->libtag, sizeof(*d->libtag) * (size_t)s->nlibtag);
    }
    d->descr = s->descr ? xstrdup(s->descr) : NULL;
    d->partname = NULL;   /* part names belong to the mnemonics, which are not inherited */
    if (s->nconst) {
        d->const_fuse = (int *)xmalloc(sizeof(int) * (size_t)s->nconst);
        memcpy(d->const_fuse, s->const_fuse, sizeof(int) * (size_t)s->nconst);
    }
    return d;
}

static int find_mode(Device *d, const char *name)
{
    int m;
    for (m = 0; m < d->nmodes; m++) if (!strcmp(d->modes[m].name, name)) return m;
    return -1;
}

static int add_mode(Ctx *c, Device *d, const char *name)
{
    int m = find_mode(d, name);
    if (m >= 0) return m;
    if (d->nmodes == DEV_MAXMODES || strlen(name) >= sizeof d->modes[0].name) { perr(c, "too many modes / bad mode name", name); return 0; }
    m = d->nmodes++;
    memset(&d->modes[m], 0, sizeof d->modes[m]);
    strcpy(d->modes[m].name, name);
    return m;
}

static void copyname(Ctx *c, char *dst, size_t n, const char *s)
{
    if (strlen(s) >= n) { perr(c, "name too long", s); return; }
    strcpy(dst, s);
    str_lower(dst);
}

static void check_device(Ctx *c, Device *d)
{
    int m, i;
    if (!d->npins || !d->nfuses || !d->ncols || !d->nrows) { perr(c, "device lacks pins/fuses/array", d->name); return; }
    if (d->ncols * d->nrows > d->nfuses && !d->nrowat && !d->vr_addr) perr(c, "array larger than fuse count", d->name);
    for (i = 0; i < d->nrows; i++)
        if (dev_row_addr(d, i) + (d->vr_addr ? d->vr_w0[i] + d->vr_w1[i] : d->ncols) > d->nfuses || (d->vr_addr && d->vr_w0[i] == 0)) { perr(c, "array row beyond the fuse count", d->name); break; }
    if (d->nmodes == 0) perr(c, "device has no 'lines'", d->name);
    for (m = 0; m < d->nmodes; m++)
        if (!d->modes[m].lines) perr(c, "mode without lines", d->modes[m].name);
    for (i = 0; i < d->nmc; i++) {
        Macrocell *k = &d->mc[i];
        if (k->sum1 >= d->nrows || k->oe_row >= d->nrows || k->xsum1 >= d->nrows || k->ck_row >= d->nrows ||
            k->ar_row >= d->nrows || k->ap_row >= d->nrows) perr(c, "macrocell row out of range", d->name);
        if (k->pol_fuse >= d->nfuses || k->reg_fuse >= d->nfuses) perr(c, "macrocell fuse out of range", d->name);
        if (k->type == MC_GAL && (k->xor_fuse < 0 || k->ac1_fuse < 0)) perr(c, "gal macrocell needs xor= and ac1=", d->name);
        if (k->type == MC_V10 && (k->s0_fuse < 0 || k->s1_fuse < 0)) perr(c, "v10 macrocell needs s0= and s1=", d->name);
    }
    for (m = 0; m < d->nmodes; m++) {
        for (i = 0; d->modes[m].lines && i < d->nlines; i++) {
            LineSrc s = d->modes[m].lines[i];
            if (s.kind == SRC_FB && dev_mc_at(d, s.pin) < 0) perr(c, "feedback from pin without macrocell", d->name);
        }
    }
}

static void finish(Ctx *c, Device *d)
{
    check_device(c, d);
    if (d->nalias == 0) {
        d->alias = (DevAlias *)xcalloc(1, sizeof(DevAlias));
        strcpy(d->alias[0].name, d->name);
        d->alias[0].nreserve = -1;
        d->nalias = 1;
    }
    g_dev = (Device **)xrealloc(g_dev, sizeof(Device *) * (size_t)(g_ndev + 1));
    g_dev[g_ndev++] = d;
}

int dev_parse(const char *text, const char *srcname)
{
    Ctx c;
    char *copy = xstrdup(text), *p = copy, *tok[MAXTOK];
    Device *d = NULL;
    int added = 0;
    c.src = srcname;
    c.line = 0;
    c.err = 0;
    while (*p && !c.err) {
        char *e = p;
        int n, i;
        while (*e && *e != '\n') e++;
        if (*e) *e++ = 0;
        c.line++;
        n = tokenize(p, tok);
        p = e;
        if (n < 0) { perr(&c, "too many tokens", NULL); break; }
        if (n == 0) continue;
        if (!strcmp(tok[0], "device")) {
            if (d) { perr(&c, "missing 'end'", NULL); break; }
            if (n != 2) { perr(&c, "usage: device <name>", NULL); break; }
            d = dev_new();
            copyname(&c, d->name, sizeof d->name, tok[1]);
            continue;
        }
        if (!d) { perr(&c, "statement outside device block", tok[0]); break; }
        if (!strcmp(tok[0], "end")) {
            finish(&c, d);
            if (!c.err) added++;
            d = NULL;
        } else if (!strcmp(tok[0], "inherit") && n == 2) {
            const Device *s = dev_find(tok[1], NULL);
            char nm[24];
            if (!s) { perr(&c, "unknown device to inherit", tok[1]); break; }
            strcpy(nm, d->name);
            free(d);
            d = dev_clone(s);
            strcpy(d->name, nm);
        } else if (!strcmp(tok[0], "pinmap") && n >= 2) {
            /* renumber an inherited description: OLD=NEW ... (package variant) */
            int map[256], m, k;
            for (i = 0; i < 256; i++) map[i] = -1;
            for (i = 1; i < n; i++) {
                char *eq = strchr(tok[i], '=');
                int a, b;
                if (!eq) { perr(&c, "pinmap expects OLD=NEW", tok[i]); break; }
                *eq = 0;
                a = to_int(&c, tok[i]);
                b = to_int(&c, eq + 1);
                if (a < 0 || a > 255 || b < 0 || b > 255) { perr(&c, "pin out of range", tok[i]); break; }
                map[a] = b;
            }
#define MAPPIN(x) do { if ((x) > 0) { if (map[(x)] < 0) { perr(&c, "pinmap does not cover a pin of the description", NULL); } else (x) = map[(x)]; } } while (0)
            /* NEW = 0: the pin's position in this package is unknown (power,
               clock or OE pins that cannot be located by probing) */
            MAPPIN(d->gnd);
            MAPPIN(d->vcc);
            MAPPIN(d->clk_pin);
            MAPPIN(d->sim_le);
            MAPPIN(d->oe_pin);
            MAPPIN(d->pd_pin);
            for (m = 0; m < d->nmodes; m++)
                for (k = 0; d->modes[m].lines && k < d->nlines; k++)
                    if (d->modes[m].lines[k].kind != SRC_NONE) {
                        int x = d->modes[m].lines[k].pin, mi = dev_mc_at(d, x);
                        /* buried node without a new number keeps its number */
                        if (mi >= 0 && d->mc[mi].node && map[x] < 0) continue;
                        MAPPIN(x);
                        d->modes[m].lines[k].pin = (unsigned char)x;
                    }
            for (k = 0; k < d->nmc; k++) {
                if (!d->mc[k].node) MAPPIN(d->mc[k].pin);
                else if (d->mc[k].pin < 256 && map[d->mc[k].pin] >= 0) d->mc[k].pin = map[d->mc[k].pin];
            }
            for (k = 0; k < d->nnodealias; k++) {
                MAPPIN(d->nodealias_pin[k]);
                if (d->nodealias_node[k] < 256 && map[d->nodealias_node[k]] >= 0) d->nodealias_node[k] = map[d->nodealias_node[k]];
            }
            for (k = 0; k < d->ninflag; k++) MAPPIN(d->inflag_pin[k]);
            for (k = 0; k < d->ntec; k++) MAPPIN(d->tec_pin[k]);
            MAPPIN(d->refdefect_pin);
#undef MAPPIN
        } else if (!strcmp(tok[0], "descr")) {
            Buf b = {0, 0, 0};
            for (i = 1; i < n; i++) { if (i > 1) buf_putc(&b, ' '); buf_puts(&b, tok[i]); }
            free(d->descr);
            d->descr = b.p ? b.p : xstrdup("");
        } else if (!strcmp(tok[0], "partname") && n >= 3) {
            /* documentation only (device list in docs/USERGUIDE.txt) */
            Buf b = {0, 0, 0};
            char **dst = NULL;
            for (i = 0; i < d->nalias; i++)
                if (!strcmp(d->alias[i].name, tok[1])) dst = &d->alias[i].partname;
            if (!dst && !strcmp(d->name, tok[1])) dst = &d->partname;
            if (!dst) {
                perr(&c, "partname: not a mnemonic of this description (give the alias first)", tok[1]);
            } else {
                for (i = 2; i < n; i++) { if (i > 2) buf_putc(&b, ' '); buf_puts(&b, tok[i]); }
                free(*dst);
                *dst = b.p ? b.p : xstrdup("");
            }
        } else if (!strcmp(tok[0], "alias") && n >= 2) {
            DevAlias *a;
            d->alias = (DevAlias *)xrealloc(d->alias, sizeof(DevAlias) * (size_t)(d->nalias + 1));
            a = &d->alias[d->nalias++];
            memset(a, 0, sizeof *a);
            a->nreserve = -1;
            copyname(&c, a->name, sizeof a->name, tok[1]);
            for (i = 2; i < n; i++) {
                char *eq = strchr(tok[i], '=');
                if (!eq) { copyname(&c, a->mode, sizeof a->mode, tok[i]); continue; }
                *eq = 0;
                if (!strcmp(tok[i], "quirk") && !strcmp(eq + 1, "fbinput")) a->fbquirk = 1;
                else if (!strcmp(tok[i], "autofb") && !strcmp(eq + 1, "simple")) a->autofb_simple = 1;
                else if (!strcmp(tok[i], "via")) a->via = atoi(eq + 1);
                else if (!strcmp(tok[i], "reserve")) {
                    char *q = eq + 1;
                    a->nreserve = 0;
                    while (*q && a->nreserve < 4) {
                        a->reserve[a->nreserve++] = (int)strtol(q, &q, 10);
                        if (*q == ',') q++; else break;
                    }
                }
                else if (!strcmp(tok[i], "simple")) copyname(&c, a->simple, sizeof a->simple, eq + 1);
                else if (!strcmp(tok[i], "complex")) copyname(&c, a->complex_, sizeof a->complex_, eq + 1);
                else if (!strcmp(tok[i], "registered")) copyname(&c, a->registered, sizeof a->registered, eq + 1);
                else perr(&c, "bad alias attribute", tok[i]);
            }
        } else if (!strcmp(tok[0], "libtag") && n == 3) {
            d->libtag = (char (*)[2][24])xrealloc(d->libtag, sizeof(*d->libtag) * (size_t)(d->nlibtag + 1));
            copyname(&c, d->libtag[d->nlibtag][0], 24, tok[1]);
            if (strlen(tok[2]) < 24) strcpy(d->libtag[d->nlibtag][1], tok[2]);
            d->nlibtag++;
        } else if (!strcmp(tok[0], "pins") && n == 2) {
            d->npins = to_int(&c, tok[1]);
        } else if (!strcmp(tok[0], "power") && n == 3) {
            d->gnd = to_int(&c, tok[1]);
            d->vcc = to_int(&c, tok[2]);
        } else if (!strcmp(tok[0], "fuses") && n == 2) {
            d->nfuses = to_int(&c, tok[1]);
        } else if (!strcmp(tok[0], "array") && n == 3) {
            d->ncols = to_int(&c, tok[1]);
            d->nrows = to_int(&c, tok[2]);
            if (d->ncols % 2) perr(&c, "column count must be even", tok[1]);
            d->nlines = d->ncols / 2;
        } else if (!strcmp(tok[0], "clock") && n == 2) {
            d->clk_pin = to_int(&c, tok[1]);
        } else if (!strcmp(tok[0], "simdefect") && n == 2 && !strcmp(tok[1], "inlevel")) {
            d->sim_inlevel = 1;
        } else if (!strcmp(tok[0], "simdefect") && n == 2 && !strcmp(tok[1], "regoe")) {
            d->sim_regoe = 1;
        } else if (!strcmp(tok[0], "simdefect") && n == 2 && !strcmp(tok[1], "noclock")) {
            d->sim_noclock = 1;
        } else if (!strcmp(tok[0], "simdefect") && n == 3 && !strcmp(tok[1], "oepin")) {
            d->sim_oepin = to_int(&c, tok[2]);
        } else if (!strcmp(tok[0], "simdefect") && n == 2 && !strcmp(tok[1], "regzero")) {
            d->sim_regzero = 1;
        } else if (!strcmp(tok[0], "simdefect") && n == 2 && !strcmp(tok[1], "noload")) {
            d->sim_noclock = 2;
        } else if (!strcmp(tok[0], "simdefect") && n == 2 && !strcmp(tok[1], "oeignore")) {
            d->sim_oe = 1;
        } else if (!strcmp(tok[0], "simdefect") && n == 2 && !strcmp(tok[1], "oehigh")) {
            d->sim_oe = 2;
        } else if (!strcmp(tok[0], "simcombinit") && n == 2 && !strcmp(tok[1], "low")) {
            d->sim_comb0 = 1;
        } else if (!strcmp(tok[0], "simpreload") && n == 2 && !strcmp(tok[1], "pin")) {
            d->sim_preload_pin = 1;
        } else if (!strcmp(tok[0], "simnote") && n == 2 && !strcmp(tok[1], "unverified")) {
            d->sim_unverified = 1;
        } else if (!strcmp(tok[0], "simle") && n == 2) {
            d->sim_le = to_int(&c, tok[1]);
        } else if (!strcmp(tok[0], "simpasses") && n == 2) {
            d->sim_passes = to_int(&c, tok[1]);
        } else if (!strcmp(tok[0], "powerup") && n == 2) {
            if (!strcmp(tok[1], "reset")) d->sim_powerup = PU_RESET;
            else if (!strcmp(tok[1], "pinhigh")) d->sim_powerup = PU_PINHIGH;
            else if (!strcmp(tok[1], "x")) d->sim_powerup = PU_X;
            else perr(&c, "powerup: reset, pinhigh or x expected", tok[1]);
        } else if (!strcmp(tok[0], "oepin") && n == 2) {
            d->oe_pin = to_int(&c, tok[1]);
        } else if (!strcmp(tok[0], "ar") && n == 2) {
            d->ar_row = to_int(&c, tok[1]);
        } else if (!strcmp(tok[0], "sp") && n == 2) {
            d->sp_row = to_int(&c, tok[1]);
        } else if (!strcmp(tok[0], "const") && n >= 2) {
            for (i = 1; i < n; i++) {
                int a, b, f;
                range(&c, tok[i], &a, &b);
                for (f = a; f <= b; f++) {
                    d->const_fuse = (int *)xrealloc(d->const_fuse, sizeof(int) * (size_t)(d->nconst + 1));
                    d->const_fuse[d->nconst++] = f;
                }
            }
        } else if (!strcmp(tok[0], "ap") && n == 2) {
            d->ap_row = to_int(&c, tok[1]);
        } else if (!strcmp(tok[0], "nolatch") && n == 2) {
            if (d->nnolatch == 4) { perr(&c, "too many nolatch statements", NULL); break; }
            d->nolatch_pin[d->nnolatch++] = to_int(&c, tok[1]);
        } else if (!strcmp(tok[0], "tec") && n == 2 && !strcmp(tok[1], "any")) {
            d->tecok = 1;
        } else if (!strcmp(tok[0], "tec") && n == 3) {
            if (d->ntec == 16) { perr(&c, "too many tec statements", NULL); break; }
            d->tec_pin[d->ntec] = to_int(&c, tok[1]);
            d->tec_fuse[d->ntec++] = to_int(&c, tok[2]);
        } else if (!strcmp(tok[0], "vrows") && (n == 7 || n == 9)) {
            int r0 = to_int(&c, tok[1]), cnt = to_int(&c, tok[2]), a = to_int(&c, tok[3]), st = to_int(&c, tok[4]);
            if (!d->nrows || r0 < 0 || r0 + cnt > d->nrows) { perr(&c, "vrows outside the array", tok[1]); break; }
            if (!d->vr_addr) {
                d->vr_addr = (int *)xcalloc((size_t)d->nrows + 1, sizeof(int));
                d->vr_v0 = (int *)xcalloc((size_t)d->nrows + 1, sizeof(int));
                d->vr_w0 = (int *)xcalloc((size_t)d->nrows + 1, sizeof(int));
                d->vr_v1 = (int *)xcalloc((size_t)d->nrows + 1, sizeof(int));
                d->vr_w1 = (int *)xcalloc((size_t)d->nrows + 1, sizeof(int));
            }
            for (i = 0; i < cnt; i++) {
                d->vr_addr[r0 + i] = a + i * st;
                d->vr_v0[r0 + i] = to_int(&c, tok[5]);
                d->vr_w0[r0 + i] = to_int(&c, tok[6]);
                d->vr_v1[r0 + i] = n == 9 ? to_int(&c, tok[7]) : 0;
                d->vr_w1[r0 + i] = n == 9 ? to_int(&c, tok[8]) : 0;
            }
        } else if (!strcmp(tok[0], "nopin") && n >= 2) {
            for (i = 1; i < n && d->nnopin < 8; i++) d->nopin[d->nnopin++] = to_int(&c, tok[i]);
        } else if (!strcmp(tok[0], "refdefect") && n == 4 && !strcmp(tok[1], "iolfuse")) {
            d->iolbug_pin = to_int(&c, tok[2]);
            d->iolbug_fuse = to_int(&c, tok[3]);
        } else if (!strcmp(tok[0], "refdefect") && n == 2 && !strcmp(tok[1], "kball")) {
            d->refdefect_kball = 1;
        } else if (!strcmp(tok[0], "refdefect") && n == 2 && !strcmp(tok[1], "xquad")) {
            d->refdefect_xquad = 1;
        } else if (!strcmp(tok[0], "inflag") && n == 2 && !strcmp(tok[1], "pinread")) {
            d->inflag_pinread = 1;
        } else if (!strcmp(tok[0], "inflag") && (n == 3 || n == 4)) {
            if (d->ninflag == 64) { perr(&c, "too many inflag statements", NULL); break; }
            d->inflag_le[d->ninflag] = n == 4 ? to_int(&c, tok[3]) : 0;
            d->inflag_pin[d->ninflag] = to_int(&c, tok[1]);
            d->inflag_fuse[d->ninflag++] = to_int(&c, tok[2]);
        } else if (!strcmp(tok[0], "fbkind") && n == 3) {
            int mi = dev_mc_at(d, to_int(&c, tok[1]));
            if (mi < 0) { perr(&c, "fbkind: no macrocell on pin", tok[1]); break; }
            d->mc[mi].fb = !strcmp(tok[2], "xq") ? GF_XQ : !strcmp(tok[2], "xp") ? GF_XP : !strcmp(tok[2], "q") ? GF_Q : !strcmp(tok[2], "pin") ? GF_PIN : GF_NQ;
        } else if (!strcmp(tok[0], "mcfb") && n == 2) {
            int k2, fb = !strcmp(tok[1], "xp") ? GF_XP : !strcmp(tok[1], "xq") ? GF_XQ : !strcmp(tok[1], "q") ? GF_Q : !strcmp(tok[1], "pin") ? GF_PIN : GF_NQ;
            for (k2 = 0; k2 < d->nmc; k2++) if (!d->mc[k2].node) d->mc[k2].fb = fb;
        } else if (!strcmp(tok[0], "pold") && n == 2) {
            /* every (non-buried) macrocell described so far */
            int k2;
            for (k2 = 0; k2 < d->nmc; k2++) if (!d->mc[k2].node) d->mc[k2].pol_d = to_int(&c, tok[1]);
        } else if (!strcmp(tok[0], "nodfb") && n == 1) {
            d->nodfb = 1;
        } else if (!strcmp(tok[0], "tfb") && n == 1) {
            d->tfb = 1;
        } else if (!strcmp(tok[0], "truecol") && n == 2) {
            d->truecol_odd = !strcmp(tok[1], "odd");
        } else if (!strcmp(tok[0], "nodealias") && n == 3) {
            if (d->nnodealias == 48) { perr(&c, "too many nodealias statements", NULL); break; }
            d->nodealias_node[d->nnodealias] = to_int(&c, tok[1]);
            d->nodealias_pin[d->nnodealias++] = to_int(&c, tok[2]);
        } else if (!strcmp(tok[0], "rowat") && n == 3) {
            if (d->nrowat == 8) { perr(&c, "too many rowat statements", NULL); break; }
            d->rowat_row[d->nrowat] = to_int(&c, tok[1]);
            d->rowat_addr[d->nrowat++] = to_int(&c, tok[2]);
        } else if (!strcmp(tok[0], "galregunused") && n == 2) {
            d->galreg_unused_ac1 = to_int(&c, tok[1]) ? 1 : 0;
        } else if (!strcmp(tok[0], "refdefect") && n == 2 && !strcmp(tok[1], "oe0")) {
            d->refdefect_oe0 = 1;
        } else if (!strcmp(tok[0], "refdefect") && n > 2 && !strcmp(tok[1], "oe0")) {
            int mi;
            for (i = 2; i < n; i++) {
                mi = dev_mc_at(d, to_int(&c, tok[i]));
                if (mi < 0) { perr(&c, "no macrocell on pin", tok[i]); break; }
                d->mc[mi].oe0 = 1;
            }
        } else if (!strcmp(tok[0], "refdefect") && n == 2 && !strcmp(tok[1], "nodeneg")) {
            d->refdefect_nodeneg = 1;
        } else if (!strcmp(tok[0], "refdefect") && n == 2 && !strcmp(tok[1], "nodecomb")) {
            d->refdefect_nodecomb = 1;
        } else if (!strcmp(tok[0], "refdefect") && n >= 2 && !strcmp(tok[1], "aliasread")) {
            /* refdefect aliasread [PIN ...]: every macrocell described so far, or the listed ones */
            int mi;
            if (n == 2) {
                for (mi = 0; mi < d->nmc; mi++) if (!d->mc[mi].node) d->mc[mi].alias_inv = 1;
            } else if (n == 3 && !strcmp(tok[2], "off")) {
                for (mi = 0; mi < d->nmc; mi++) d->mc[mi].alias_inv = 0;   /* not inherited */
            } else {
                for (i = 2; i < n; i++) {
                    mi = dev_mc_at(d, to_int(&c, tok[i]));
                    if (mi < 0) { perr(&c, "no macrocell on pin", tok[i]); break; }
                    d->mc[mi].alias_inv = 1;
                }
            }
        } else if (!strcmp(tok[0], "refdefect") && n >= 4 && !strcmp(tok[1], "ioaddr")) {
            char *e2;
            long off = strtol(tok[2], &e2, 10);
            int mi;
            if (!*tok[2] || *e2 || off < -1000 || off > 1000) { perr(&c, "bad offset", tok[2]); break; }
            for (i = 3; i < n; i++) {
                mi = dev_mc_at(d, to_int(&c, tok[i]));
                if (mi < 0) { perr(&c, "no macrocell on pin", tok[i]); break; }
                d->mc[mi].io_bad = 1;
                d->mc[mi].io_off = (int)off;
            }
        } else if (!strcmp(tok[0], "inflag") && n == 2 && !strcmp(tok[1], "dq")) {
            d->inflag_dq = 1;
        } else if (!strcmp(tok[0], "refdefect") && n > 2 && !strcmp(tok[1], "combfull")) {
            int mi;
            for (i = 2; i < n; i++) {
                mi = dev_mc_at(d, to_int(&c, tok[i]));
                if (mi < 0) { perr(&c, "no macrocell on pin", tok[i]); break; }
                d->mc[mi].combfull = 1;
            }
        } else if (!strcmp(tok[0], "refdefect") && n == 2 && !strcmp(tok[1], "inreg")) {
            d->refdefect_inreg = 1;
        } else if (!strcmp(tok[0], "refdefect") && n == 2 && !strcmp(tok[1], "combd")) {
            d->refdefect_combd = 1;
        } else if (!strcmp(tok[0], "refdefect") && n == 2 && !strcmp(tok[1], "kbfbinv")) {
            d->refdefect_kbfbinv = 1;
        } else if (!strcmp(tok[0], "refdefect") && n == 2 && !strcmp(tok[1], "nojoin")) {
            d->refdefect_nojoin = 1;
        } else if (!strcmp(tok[0], "refdefect") && n >= 2 && !strcmp(tok[1], "kbnoflip")) {
            /* refdefect kbnoflip [MODE ...]: -kb does not change the polarity (all modes, or the listed ones) */
            if (n == 2) d->refdefect_kbnoflip = ~0u;
            for (i = 2; i < n; i++) {
                /* MODE, or MODE/reg: registered outputs only */
                int mi2, ro = 0;
                char mn[32];
                snprintf(mn, sizeof mn, "%s", tok[i]);
                if (strlen(mn) > 4 && !strcmp(mn + strlen(mn) - 4, "/reg")) { ro = 1; mn[strlen(mn) - 4] = 0; }
                for (mi2 = 0; mi2 < d->nmodes; mi2++) if (!strcmp(d->modes[mi2].name, mn)) break;
                if (mi2 == d->nmodes) { perr(&c, "unknown mode", tok[i]); break; }
                if (ro) d->refdefect_kbnoflip_reg |= 1u << mi2;
                else d->refdefect_kbnoflip |= 1u << mi2;
            }
        } else if (!strcmp(tok[0], "refdefect") && n == 4 && !strcmp(tok[1], "ones")) {
            d->refdefect_ones = to_int(&c, tok[2]);
            d->refdefect_nones = to_int(&c, tok[3]);
        } else if (!strcmp(tok[0], "refdefect") && n == 3 && !strcmp(tok[1], "shift")) {
            d->refdefect_shift = to_int(&c, tok[2]);
        } else if (!strcmp(tok[0], "refdefect") && n == 3) {
            copyname(&c, d->refdefect_mode, sizeof d->refdefect_mode, tok[1]);
            d->refdefect_pin = to_int(&c, tok[2]);
        } else if (!strcmp(tok[0], "steershare") && n == 2) {
            d->steer_share = to_int(&c, tok[1]) ? 1 : 0;
        } else if (!strcmp(tok[0], "nodedt") && n == 2 && !strcmp(tok[1], "reject")) {
            d->nodedt_reject = 1;
        } else if (!strcmp(tok[0], "rowfill") && n == 2) {
            d->rowfill = to_int(&c, tok[1]) ? 1 : 0;
        } else if (!strcmp(tok[0], "ptd") && n == 2) {
            d->ptd_base = to_int(&c, tok[1]);
        } else if (!strcmp(tok[0], "ues") && n == 3) {
            d->ues_base = to_int(&c, tok[1]);
            d->ues_len = to_int(&c, tok[2]);
        } else if (!strcmp(tok[0], "pd") && n == 3) {
            d->pd_fuse = to_int(&c, tok[1]);
            d->pd_pin = to_int(&c, tok[2]);
        } else if (!strcmp(tok[0], "fuse") && n == 3) {
            if (d->nfusenames == DEV_MAXFUSENAMES || strlen(tok[1]) >= 16) { perr(&c, "too many fuse names", tok[1]); break; }
            strcpy(d->fusename[d->nfusenames], tok[1]);
            d->fuseaddr[d->nfusenames++] = to_int(&c, tok[2]);
        } else if (!strcmp(tok[0], "mode") && n >= 2) {
            int m = add_mode(&c, d, tok[1]);
            d->modes[m].ncond = 0;
            for (i = 2; i < n; i++) {
                char *eq = strchr(tok[i], '=');
                DevMode *dm = &d->modes[m];
                if (!eq || dm->ncond == DEV_MAXCOND) { perr(&c, "bad mode condition", tok[i]); break; }
                *eq = 0;
                dm->cond_fuse[dm->ncond] = fuse_ref(&c, d, tok[i]);
                dm->cond_val[dm->ncond] = to_int(&c, eq + 1) ? 1 : 0;
                dm->ncond++;
            }
        } else if (!strcmp(tok[0], "lines+") && n >= 2) {
            /* a long line list in several statements */
            int m = add_mode(&c, d, "all");
            if (!d->nlines) { perr(&c, "'array' must precede 'lines'", NULL); break; }
            if (!d->modes[m].lines) { d->modes[m].lines = (LineSrc *)xcalloc((size_t)d->nlines, sizeof(LineSrc)); d->lines_fill = 0; }
            if (d->lines_fill + n - 1 > d->nlines) { perr(&c, "line count does not match array columns/2", "lines+"); break; }
            for (i = 1; i < n; i++) {
                LineSrc *s = &d->modes[m].lines[d->lines_fill++];
                if (!strcmp(tok[i], "-")) s->kind = SRC_NONE;
                else if (tok[i][0] == 'p' || tok[i][0] == 'f') {
                    s->kind = tok[i][0] == 'p' ? SRC_PIN : SRC_FB;
                    s->pin = (unsigned char)to_int(&c, tok[i] + 1);
                } else perr(&c, "bad line source", tok[i]);
            }
        } else if (!strcmp(tok[0], "lines") && n >= 2) {
            int first = 1, m;
            const char *mname = "all";
            if (!d->nlines) { perr(&c, "'array' must precede 'lines'", NULL); break; }
            if (!isdigit((unsigned char)tok[1][1]) && tok[1][0] != '-') { mname = tok[1]; first = 2; }
            m = add_mode(&c, d, mname);
            if (n - first != d->nlines) { perr(&c, "line count does not match array columns/2", mname); break; }
            free(d->modes[m].lines);
            d->modes[m].lines = (LineSrc *)xcalloc((size_t)d->nlines, sizeof(LineSrc));
            for (i = first; i < n; i++) {
                LineSrc *s = &d->modes[m].lines[i - first];
                if (!strcmp(tok[i], "-")) s->kind = SRC_NONE;
                else if (tok[i][0] == 'p' || tok[i][0] == 'f') {
                    s->kind = tok[i][0] == 'p' ? SRC_PIN : SRC_FB;
                    s->pin = (unsigned char)to_int(&c, tok[i] + 1);
                } else perr(&c, "bad line source", tok[i]);
            }
        } else if (!strcmp(tok[0], "mc") && n >= 3) {
            Macrocell k;
            memset(&k, 0, sizeof k);
            k.pin = to_int(&c, tok[1]);
            k.oe_row = -1;
            k.sum0 = k.sum1 = -1;
            k.xor_fuse = k.ac1_fuse = k.s0_fuse = k.s1_fuse = -1;
            k.pol = 1;
            k.xsum0 = k.xsum1 = -1;
            k.pol_fuse = -1;
            k.pol_hi = 1;
            k.reg_fuse = -1;
            k.reg_on = 1;
            k.ck_row = k.ar_row = k.ap_row = -1;
            memset(k.cfg_val, -1, sizeof k.cfg_val);
            k.fb_fuse = -1;
            k.fb_pin = 1;
            k.t_fuse = -1;
            k.split_fuse = -1;
            k.ckmux_fuse = -1;
            k.half0 = k.half1 = -1;
            k.split_val = 1;
            k.qsplit_fuse = -1;
            k.qhalf0 = k.qhalf1 = k.qsum0 = k.qsum1 = -1;
            k.sp_row = -1;
            k.reg_idle = -1;
            k.ce_fuse = -1;
            k.sel_comb = k.sel_reg = -1;
            k.steer_fuse = -1;
            k.fb_fuse2 = -1;
            k.oemux_fuse = -1;
            k.steer_step = 1;
            k.ac1_val = 1;
            k.xorr_fuse = -1;
            k.xorr_val = 1;
            k.sumb = k.sumc = -1;
            k.xv_fuse = -1;
            k.cjoin_fuse = k.cjoin_bug = k.cksync_fuse = -1;
            k.cksync_idle = 1;
            k.reg_nout = -1;
            if (!strcmp(tok[2], "comb")) k.type = MC_COMB;
            else if (!strcmp(tok[2], "reg")) k.type = MC_REG;
            else if (!strcmp(tok[2], "gal")) k.type = MC_GAL;
            else if (!strcmp(tok[2], "v10")) k.type = MC_V10;
            else if (!strcmp(tok[2], "gen")) k.type = MC_GEN;
            else { perr(&c, "unknown macrocell type", tok[2]); break; }
            for (i = 3; i < n; i++) {
                char *eq = strchr(tok[i], '=');
                if (!eq) { perr(&c, "expected key=value", tok[i]); break; }
                *eq++ = 0;
                if (!strcmp(tok[i], "sum") || !strcmp(tok[i], "rows")) range(&c, eq, &k.sum0, &k.sum1);
                else if (!strcmp(tok[i], "oe")) {
                    if (!strcmp(eq, "pin")) k.oe_kind = GO_PIN;
                    else if (!strcmp(eq, "always")) k.oe_kind = GO_ALWAYS;
                    else { k.oe_row = to_int(&c, eq); k.oe_kind = GO_ROW; }
                } else if (!strcmp(tok[i], "oepin")) {
                    if (k.oe_kind == GO_ROW && to_int(&c, eq)) k.oe_kind = GO_ROWPIN;
                } else if (!strcmp(tok[i], "xsum")) range(&c, eq, &k.xsum0, &k.xsum1);
                else if (!strcmp(tok[i], "polfuse")) k.pol_fuse = fuse_ref(&c, d, eq);
                else if (!strcmp(tok[i], "polhi")) k.pol_hi = to_int(&c, eq) ? 1 : 0;
                else if (!strcmp(tok[i], "reg")) {
                    if (!strcmp(eq, "0") || !strcmp(eq, "comb")) k.reg_kind = GR_COMB;
                    else if (!strcmp(eq, "1") || !strcmp(eq, "d")) k.reg_kind = GR_REG;
                    else if (!strcmp(eq, "arap")) k.reg_kind = GR_ARAP;
                    else { k.reg_kind = GR_FUSE; k.reg_fuse = fuse_ref(&c, d, eq); }
                } else if (!strcmp(tok[i], "regon")) k.reg_on = to_int(&c, eq) ? 1 : 0;
                else if (!strcmp(tok[i], "ck")) k.ck_row = to_int(&c, eq);
                else if (!strcmp(tok[i], "ar")) k.ar_row = to_int(&c, eq);
                else if (!strcmp(tok[i], "ap")) k.ap_row = to_int(&c, eq);
                else if (!strcmp(tok[i], "oereg")) k.oe_reg_pin = !strcmp(eq, "pin");
                else if (!strcmp(tok[i], "fbfuse")) k.fb_fuse = fuse_ref(&c, d, eq);
                else if (!strcmp(tok[i], "oemux")) k.oemux_fuse = fuse_ref(&c, d, eq);
                else if (!strcmp(tok[i], "fbxor")) k.fb_xor = to_int(&c, eq) ? 1 : 0;
                else if (!strcmp(tok[i], "fbfuse2")) k.fb_fuse2 = fuse_ref(&c, d, eq);
                else if (!strcmp(tok[i], "fbpin")) k.fb_pin = to_int(&c, eq) ? 1 : 0;
                else if (!strcmp(tok[i], "ckmux")) k.ckmux_fuse = fuse_ref(&c, d, eq);
                else if (!strcmp(tok[i], "alwaysreg")) k.alwaysreg = to_int(&c, eq) ? 1 : 0;
                else if (!strcmp(tok[i], "node")) k.node = to_int(&c, eq) ? 1 : 0;
                else if (!strcmp(tok[i], "split")) k.split_fuse = fuse_ref(&c, d, eq);
                else if (!strcmp(tok[i], "half")) range(&c, eq, &k.half0, &k.half1);
                else if (!strcmp(tok[i], "splitval")) k.split_val = to_int(&c, eq) ? 1 : 0;
                else if (!strcmp(tok[i], "splitout")) k.split_out = to_int(&c, eq) ? 1 : 0;
                else if (!strcmp(tok[i], "qsplit")) k.qsplit_fuse = fuse_ref(&c, d, eq);
                else if (!strcmp(tok[i], "qhalf")) range(&c, eq, &k.qhalf0, &k.qhalf1);
                else if (!strcmp(tok[i], "qsum")) range(&c, eq, &k.qsum0, &k.qsum1);
                else if (!strcmp(tok[i], "sp")) k.sp_row = to_int(&c, eq);
                else if (!strcmp(tok[i], "regidle")) k.reg_idle = to_int(&c, eq) ? 1 : 0;
                else if (!strcmp(tok[i], "splitneed")) k.split_need = to_int(&c, eq) ? 1 : 0;
                else if (!strcmp(tok[i], "steer")) k.steer_fuse = fuse_ref(&c, d, eq);
                else if (!strcmp(tok[i], "steerstep")) k.steer_step = to_int(&c, eq);
                else if (!strcmp(tok[i], "latch")) k.latch = to_int(&c, eq) ? 1 : 0;
                else if (!strcmp(tok[i], "xone")) k.xone = to_int(&c, eq) ? 1 : 0;
                else if (!strcmp(tok[i], "xswap")) k.xswap = to_int(&c, eq) ? 1 : 0;
                else if (!strcmp(tok[i], "fbreg")) k.fb_reg = to_int(&c, eq) ? 1 : 0;
                else if (!strcmp(tok[i], "selc")) k.sel_comb = to_int(&c, eq);
                else if (!strcmp(tok[i], "selr")) k.sel_reg = to_int(&c, eq);
                else if (!strcmp(tok[i], "ac1val")) k.ac1_val = to_int(&c, eq) ? 1 : 0;
                else if (!strcmp(tok[i], "xorr")) k.xorr_fuse = fuse_ref(&c, d, eq);
                else if (!strcmp(tok[i], "xorrval")) k.xorr_val = to_int(&c, eq) ? 1 : 0;
                else if (!strcmp(tok[i], "cefuse")) k.ce_fuse = fuse_ref(&c, d, eq);
                else if (!strcmp(tok[i], "sumb")) k.sumb = to_int(&c, eq);
                else if (!strcmp(tok[i], "sumc")) k.sumc = to_int(&c, eq);
                else if (!strcmp(tok[i], "cjoin")) k.cjoin_fuse = fuse_ref(&c, d, eq);
                else if (!strcmp(tok[i], "cjoinbug")) k.cjoin_bug = fuse_ref(&c, d, eq);
                else if (!strcmp(tok[i], "cksync")) k.cksync_fuse = fuse_ref(&c, d, eq);
                else if (!strcmp(tok[i], "cksyncidle")) k.cksync_idle = to_int(&c, eq) ? 1 : 0;
                else if (!strcmp(tok[i], "ckpin")) k.ck_pin = to_int(&c, eq);
                else if (!strcmp(tok[i], "regnout")) k.reg_nout = to_int(&c, eq) ? 1 : 0;
                else if (!strcmp(tok[i], "noctl")) k.noctl = to_int(&c, eq) ? 1 : 0;
                else if (!strcmp(tok[i], "xvfuse")) k.xv_fuse = fuse_ref(&c, d, eq);
                else if (!strcmp(tok[i], "tfuse")) k.t_fuse = fuse_ref(&c, d, eq);
                else if (!strcmp(tok[i], "tval")) k.t_val = to_int(&c, eq) ? 1 : 0;
                else if (!strcmp(tok[i], "pold")) k.pol_d = to_int(&c, eq);
                else if (!strcmp(tok[i], "cfg")) {
                    char *q = eq;
                    k.ncfg = 0;
                    while (*q && k.ncfg < MC_MAXCFG) {
                        char nm2[24];
                        int j = 0;
                        while (*q && *q != ',' && j < 23) nm2[j++] = *q++;
                        nm2[j] = 0;
                        k.cfg_fuse[k.ncfg++] = fuse_ref(&c, d, nm2);
                        if (*q == ',') q++;
                    }
                } else if (!strncmp(tok[i], "cfg", 3) && tok[i][3]) {
                    static const char *const rn[CR_COUNT] = { "u", "i", "c", "cn", "r", "rn", "cf", "o" };
                    int r, j;
                    for (r = 0; r < CR_COUNT; r++) if (!strcmp(tok[i] + 3, rn[r])) break;
                    if (r == CR_COUNT || (int)strlen(eq) != k.ncfg) { perr(&c, "bad cfg role or length", tok[i]); break; }
                    for (j = 0; j < k.ncfg; j++) k.cfg_val[r][j] = (signed char)(eq[j] == '1');
                }
                else if (!strcmp(tok[i], "fb")) {
                    if (!strcmp(eq, "nq")) k.fb = GF_NQ;
                    else if (!strcmp(eq, "q")) k.fb = GF_Q;
                    else if (!strcmp(eq, "pin")) k.fb = GF_PIN;
                    else if (!strcmp(eq, "xq")) k.fb = GF_XQ;
                    else perr(&c, "fb must be nq, q or pin", eq);
                }
                else if (!strcmp(tok[i], "pol")) {
                    if (!strcmp(eq, "h")) k.pol = 1;
                    else if (!strcmp(eq, "l")) k.pol = 0;
                    else perr(&c, "pol must be h or l", eq);
                } else if (!strcmp(tok[i], "xor")) k.xor_fuse = fuse_ref(&c, d, eq);
                else if (!strcmp(tok[i], "ac1")) k.ac1_fuse = fuse_ref(&c, d, eq);
                else if (!strcmp(tok[i], "s0")) k.s0_fuse = fuse_ref(&c, d, eq);
                else if (!strcmp(tok[i], "s1")) k.s1_fuse = fuse_ref(&c, d, eq);
                else perr(&c, "unknown macrocell attribute", tok[i]);
            }
            if (k.sum0 < 0) perr(&c, "macrocell without sum rows", tok[1]);
            if (k.type == MC_GAL && k.sum1 - k.sum0 != 7) perr(&c, "gal macrocell must span 8 rows", tok[1]);
            if (dev_mc_at(d, k.pin) >= 0) {
                d->mc[dev_mc_at(d, k.pin)] = k;
            } else {
                d->mc = (Macrocell *)xrealloc(d->mc, sizeof(Macrocell) * (size_t)(d->nmc + 1));
                d->mc[d->nmc++] = k;
            }
        } else {
            perr(&c, "unknown statement", tok[0]);
        }
    }
    if (d && !c.err) perr(&c, "missing 'end' at end of file", NULL);
    free(copy);
    return c.err ? -1 : added;
}

/* every 'inherit NAME' in text names a device that is already known */
static int dev_inherit_ready(const char *text)
{
    const char *p = text;
    while ((p = strstr(p, "inherit")) != NULL) {
        char nm[32];
        int k = 0;
        if (p != text && p[-1] != '\n') { p++; continue; }
        p += 7;
        while (*p == ' ' || *p == '\t') p++;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '#' && k < 31) nm[k++] = *p++;
        nm[k] = 0;
        if (!dev_find(nm, NULL)) return 0;
    }
    return 1;
}

extern const char *const *const freecupl_builtin_devices[];
extern const char *const freecupl_builtin_device_names[];

void dev_load_builtin(void)
{
    static int loaded = 0;
    int i, n, pass, left;
    unsigned char *done;
    char **text;
    if (loaded) return;
    loaded = 1;
    for (n = 0; freecupl_builtin_devices[n]; n++) ;
    done = (unsigned char *)xcalloc((size_t)n + 1, 1);
    /* join the chunks of every block (see src/host/embed.c) */
    text = (char **)xcalloc((size_t)n + 1, sizeof(char *));
    for (i = 0; i < n; i++) {
        size_t len = 1;
        int k;
        for (k = 0; freecupl_builtin_devices[i][k]; k++) len += strlen(freecupl_builtin_devices[i][k]);
        text[i] = (char *)xmalloc(len);
        text[i][0] = 0;
        for (k = 0; freecupl_builtin_devices[i][k]; k++) strcat(text[i], freecupl_builtin_devices[i][k]);
    }
    /* blocks may inherit from blocks of later files: retry until no progress */
    for (pass = 0, left = n; left && pass < 16; pass++) {
        int before = left;
        for (i = 0; i < n; i++) {
            if (done[i] || !dev_inherit_ready(text[i])) continue;
            if (dev_parse(text[i], freecupl_builtin_device_names[i]) < 0)
                die("internal error in built-in device description %s", freecupl_builtin_device_names[i]);
            done[i] = 1;
            left--;
        }
        if (left == before) break;
    }
    if (left) die("internal error: built-in device descriptions inherit from unknown devices");
    for (i = 0; i < n; i++) free(text[i]);
    free(text);
    free(done);
}