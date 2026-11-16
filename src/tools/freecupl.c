/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - compiler command line.  DARKSOFT. */
#include "device.h"
#include "util.h"
#include "version.h"
#include "bugs.h"
#include "cupl/design.h"
#include "cupl/diag.h"
#include "cupl/fit.h"
#include "cupl/output.h"
#include "cupl/pp.h"
#include "csim/csim.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    printf("freecupl - CUPL compiler, %s\n", FREECUPL_VERSION_STRING);
    printf("usage: freecupl -flags [library] [device] file[.pld]\n"
           "  -j   write JEDEC file             -a   write absolute file (.abs)\n"
           "  -l   write listing (.lst)         -e   write macro expansion (.mx)\n"
           "  -x   write documentation (.doc)   -f   fuse plot in .doc\n"
           "  -n   name outputs after source    -g   set security fuse\n"
           "  -s   simulate file.si (.so, .wo; vectors into the JEDEC file with -j)\n"
           "  -m0  no minimization   -m1 quick (default)   -m2 -m3 -m4 more\n"
           "  -kb  pick output polarity for fewer product terms\n"
           "  -u   library name (ignored)\n"
           "  -w -b -h -i -p -c -d -r -o -q -kd -ks -kx   accepted, ignored\n"
           "  -fix               correct WinCUPL 5.0a bugs (docs/BUGS.txt)%s\n"
           "  -compat-refdefect  reproduce WinCUPL 5.0a, bugs included%s\n",
           FREECUPL_FIX_DEFAULT ? " (default)" : "", FREECUPL_FIX_DEFAULT ? "" : " (default)");
    printf("  -simlatch          simulate latched outputs and input registers\n"
           "                     (timing not verified; default X as WinCUPL)\n"
           "  -devices           list device mnemonics\n"
           "  -V                 print version\n");
}

extern int g_name_twice;

static int has_ext(const char *p)
{
    const char *s = strrchr(p, '.'), *sl1 = strrchr(p, '/'), *sl2 = strrchr(p, '\\');
    if (!s) return 0;
    if ((sl1 && sl1 > s) || (sl2 && sl2 > s)) return 0;
    return 1;
}

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* directory part of a path including the separator ("" if none) */
static char *dir_of(const char *p)
{
    const char *a = strrchr(p, '/'), *b = strrchr(p, '\\');
    const char *s = a > b ? a : b;
    if (!s) return xstrdup("");
    return xstrndup(p, (size_t)(s - p + 1));
}

static char *stem_of(const char *p)
{
    const char *a = strrchr(p, '/'), *b = strrchr(p, '\\');
    const char *s = a > b ? a : b;
    const char *base = s ? s + 1 : p, *dot = strrchr(base, '.');
    return dot ? xstrndup(base, (size_t)(dot - base)) : xstrdup(base);
}

/* the compiled design as the simulator sees it */
static void add_reserve(CsDesign *cd, const Device *d, const char *mn)
{
    const DevAlias *al = NULL;
    int i, k;
    if (!mn || !*mn || dev_find(mn, &al) != d || !al) return;
    if (al->nreserve < 0) {
        if (d->pd_pin > 0 && cd->nreserve < 8) cd->reserve[cd->nreserve++] = d->pd_pin;
        return;
    }
    for (i = 0; i < al->nreserve; i++) {
        for (k = 0; k < cd->nreserve; k++) if (cd->reserve[k] == al->reserve[i]) break;
        if (k == cd->nreserve && cd->nreserve < 8) cd->reserve[cd->nreserve++] = al->reserve[i];
    }
}

/* the expression reads the symbol */
static int expr_reads(const Expr *e, const Sym *s)
{
    if (!e) return 0;
    if (e->op == E_VAR) return e->sym == s;
    return expr_reads(e->a, s) || expr_reads(e->b, s);
}

/* the design reads the pin: referenced by name or through a field, or
   present in an equation */
static int pin_read(const Sym *s)
{
    int i, k;
    if (s->used) return 1;
    for (i = 0; i < g_des.nsyms; i++) {
        const Sym *f = g_des.syms[i];
        if (f->kind == SK_FIELD && f->used)
            for (k = 0; k < f->nmem; k++) if (f->mem[k] == s) return 1;
    }
    for (i = 0; i < g_des.neqns; i++) {
        const Eqn *q = g_des.eqns[i];
        if (expr_reads(q->e, s)) return 1;
        for (k = 0; k < 4; k++) if (expr_reads(q->grp[k], s)) return 1;
    }
    return 0;
}

static void design_for_sim(CsDesign *cd, const FitResult *fr, const char *mnemonic)
{
    int i, k;
    memset(cd, 0, sizeof *cd);
    add_reserve(cd, fr->dev, mnemonic);
    add_reserve(cd, fr->dev, fr->resolved);
    cd->dev = fr->dev;
    snprintf(cd->mnemonic, sizeof cd->mnemonic, "%s", fr->dev->name);
    cd->fuse = (unsigned char *)xmalloc((size_t)fr->dev->nfuses + 1);
    memcpy(cd->fuse, fr->fuse, (size_t)fr->dev->nfuses);
    for (i = 0; i < CH_COUNT && i < NHDR; i++) cd->hdr[i] = g_des.hdr[i] ? xstrdup(g_des.hdr[i]) : NULL;
    cd->ctlinfo = 1;
    for (i = 0; i < g_des.neqns; i++) {
        const Eqn *q = g_des.eqns[i];
        if (!q->sym || q->sym->pin <= 0) continue;
        if (q->ext == X_AR && cd->narpin < 128) cd->arpin[cd->narpin++] = q->sym->pin;
        if (q->ext == X_SP && cd->nsppin < 128) cd->sppin[cd->nsppin++] = q->sym->pin;
        if (q->ext == X_OE && cd->noepin < 128) cd->oepin[cd->noepin++] = q->sym->pin;
    }
    for (i = 0; i < g_des.nsyms; i++) {
        const Sym *s = g_des.syms[i];
        if ((s->kind == SK_PIN || s->kind == SK_NODE) && s->pin > 0 && cd->nnoeqn < 64) {
            /* no equation of its value: the original simulator treats it
               as an input or leaves it out */
            int j, has = 0;
            for (j = 0; j < g_des.neqns && !has; j++) {
                const Eqn *q = g_des.eqns[j];
                if (q->sym == s && (q->ext == X_NONE || q->ext == X_D || q->ext == X_T || q->ext == X_J || q->ext == X_K ||
                                    q->ext == X_S || q->ext == X_R || q->ext == X_L || q->ext == X_T1 || q->ext == X_T2)) has = 1;
            }
            if (!has && dev_mc_at(fr->dev, s->pin) >= 0) {
                cd->noeqn[cd->nnoeqn] = s->pin;
                cd->noeqn_used[cd->nnoeqn++] = (unsigned char)pin_read(s);
            }
        }
        if ((s->kind == SK_PIN || s->kind == SK_NODE) && s->pin > 0) {
            cd->sym = (CsSym *)xrealloc(cd->sym, sizeof(CsSym) * (size_t)(cd->nsym + 1));
            cd->sym[cd->nsym].name = xstrdup(s->name);
            cd->sym[cd->nsym].pin = s->pin;
            cd->sym[cd->nsym].neg = s->neg;
            /* WinCUPL bug B015 (nodeneg): the register holds the node's
               equation as written; the simulator shows it as it is */
            if (!g_fix && s->kind == SK_NODE && s->neg && fr->dev->refdefect_nodeneg && dev_mc_at(fr->dev, s->pin) >= 0 && fr->dev->mc[dev_mc_at(fr->dev, s->pin)].node) cd->sym[cd->nsym].neg = 0;
            cd->nsym++;
        } else if (s->kind == SK_FIELD && !s->hidden) {
            CsField *f;
            cd->fld = (CsField *)xrealloc(cd->fld, sizeof(CsField) * (size_t)(cd->nfld + 1));
            f = &cd->fld[cd->nfld++];
            f->name = xstrdup(s->name);
            f->n = s->nmem;
            f->mem = (char **)xcalloc((size_t)s->nmem + 1, sizeof(char *));
            for (k = 0; k < s->nmem; k++) f->mem[k] = xstrdup(s->mem[k]->name);
        }
    }
}

/* -s: simulate <dir><stem>.si; jedbase: append the vectors to <jedbase>.jed */
static int g_simlatch;

static int simulate(const CsDesign *cd, const char *dir, const char *stem, const char *jedbase)
{
    CsOpts o;
    Buf si = {0, 0, 0}, so = {0, 0, 0}, wo = {0, 0, 0}, jp = {0, 0, 0}, nm = {0, 0, 0}, ver = {0, 0, 0}, cr = {0, 0, 0};
    int rc;
    buf_printf(&si, "%s%s.si", dir, stem);
    if (!file_exists(si.p)) { buf_free(&si); buf_printf(&si, "%s%s.SI", dir, stem); }
    memset(&o, 0, sizeof o);
    o.simlatch = g_simlatch;
    o.refdefect = !g_fix;
    o.si_path = si.p;
    buf_printf(&nm, "%s.si", stem);
    o.si_name = nm.p;
    buf_printf(&so, "%s%s.so", dir, stem);
    buf_printf(&wo, "%s%s.wo", dir, stem);
    o.so_path = so.p;
    o.wo_path = wo.p;
    if (jedbase) { buf_printf(&jp, "%s.jed", jedbase); o.jed_path = jp.p; }
    buf_printf(&ver, "%s", FREECUPL_VERSION_STRING);
    buf_printf(&cr, "CREATED %s", created_string());
    o.banner[0] = "FreeCSIM: CUPL compatible simulation program";
    o.banner[1] = ver.p;
    o.banner[2] = "";
    o.banner[3] = cr.p;
    rc = csim_run(cd, &o);
    if (rc == 2) printf("[0001sa] could not open:  %s\n", si.p);
    if (rc) printf("\nSimulation errors encountered\n");
    buf_free(&si); buf_free(&so); buf_free(&wo); buf_free(&jp); buf_free(&nm); buf_free(&ver); buf_free(&cr);
    return rc ? 1 : 0;
}

int main(int argc, char **argv)
{
    int i, fj = 0, fa = 0, fl = 0, fe = 0, fx = 0, ff = 0, fn = 0, fg = 0, fu = 0, fs = 0;
    int nopos = 0, rc;
    const char *pos[8];
    FitOpts fo;
    PPOut pp;
    FitResult fr;
    char *src, *dir, *stem, *outbase, *path;
    const char *device = NULL;
    char used_device[64] = "";
    Buf pb = {0, 0, 0};

    memset(&fo, 0, sizeof fo);
    fo.minlevel = 1;
    dev_load_builtin();
    if (argc < 2) { usage(); return 1; }
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-devices")) {
            int d, k;
            for (d = 0; d < dev_count(); d++)
                for (k = 0; k < dev_get(d)->nalias; k++) printf("%s\n", dev_get(d)->alias[k].name);
            return 0;
        }
        if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
            printf("%s\n", FREECUPL_VERSION_STRING);
            return 0;
        }
        if (!strcmp(a, "-fix")) { g_fix = 1; continue; }
        if (!strcmp(a, "-compat-refdefect")) { g_fix = 0; continue; }
        if (!strcmp(a, "-simlatch")) { g_simlatch = 1; continue; }
        if (a[0] == '-' && a[1]) {
            const char *f = a + 1;
            for (; *f; f++) {
                int c = tolower((unsigned char)*f);
                switch (c) {
                case 'j': fj = 1; break;
                case 'a': fa = 1; break;
                case 'l': fl = 1; break;
                case 'e': fe = 1; break;
                case 'x': fx = 1; break;
                case 'f': ff = 1; break;
                case 'n': fn = 1; break;
                case 'g': fg = 1; break;
                case 'u': fu = 1; break;
                case 's': fs = 1; break;
                case 'w': break;
                case 'm':
                    if (isdigit((unsigned char)f[1])) {
                        fo.minlevel = f[1] - '0';
                        if (fo.minlevel > 4) { printf("[0007ck] invalid flag:  -m%c\n", f[1]); return 1; }
                        f++;
                    }
                    break;
                case 'k':
                    if (f[1]) {
                        if (tolower((unsigned char)f[1]) == 'b') fo.kb = 1;
                        f++;
                    }
                    break;
                case 'b': case 'h': case 'i': case 'p': case 'c': case 'd': case 'r': case 'o': case 'q':
                    break;
                default:
                    printf("[0007ck] invalid flag:  -%c\n", *f);
                    return 1;
                }
            }
            continue;
        }
        if (nopos < 8) pos[nopos++] = a;
    }
    if (nopos == 0) { usage(); return 1; }
    /* [library] [device] file ; a library is recognised by -u, by its
       position (three arguments) or by the .dl extension */
    src = (char *)pos[nopos - 1];
    if (nopos >= 2) {
        int first = 0;
        size_t l0 = strlen(pos[0]);
        if (fu || nopos >= 3 || (l0 > 3 && str_ieq(pos[0] + l0 - 3, ".dl"))) first = 1;
        if (nopos - 1 > first) device = pos[nopos - 2];
    }
    if (!has_ext(src) && !file_exists(src)) {
        buf_printf(&pb, "%s.pld", src);
        if (!file_exists(pb.p)) { buf_free(&pb); buf_printf(&pb, "%s.PLD", src); }
        src = pb.p;
    }
    dir = dir_of(src);
    stem = stem_of(src);
    diag_reset();
    rc = pp_run(src, &pp);
    if (fe && pp.n) {
        Buf p = {0, 0, 0};
        buf_printf(&p, "%s%s.mx", dir, stem);
        write_mx(p.p, &pp);
        buf_free(&p);
    }
    if (rc == 0) rc = parse_design(&pp);
    if (rc == 0) {
        const char *dv = device ? device : g_des.hdr[H_DEVICE];
        if (g_des.hdr_missing) diag(DG_NOTE, "0022ca", 0, "missing header item(s)");
        if (g_name_twice) dv = NULL;   /* BUG_HEADER_LATE */
        if (!dv || !*dv) {
            diag(DG_FATAL, "0038ca", 0, "target device not specified");
            rc = -1;
        } else {
            char dvl[64];
            snprintf(dvl, sizeof dvl, "%s", dv);
            str_lower(dvl);
            rc = fit_design(dvl, &fo, &fr);
            snprintf(used_device, sizeof used_device, "%s", dvl);
        }
    }
    if (g_des.hdr[H_NAME] && !fn) {
        Buf p = {0, 0, 0};
        buf_printf(&p, "%s%s", dir, g_des.hdr[H_NAME]);
        outbase = p.p;
    } else {
        Buf p = {0, 0, 0};
        if (!fn && !g_des.hdr[H_NAME] && rc == 0) diag(DG_WARN, "0009cc", 0, "header name undefined, using no_name");
        buf_printf(&p, "%s%s", dir, fn ? stem : "no_name");
        outbase = p.p;
    }
    path = (char *)xmalloc(strlen(outbase) + 8);
    if (fl) {
        sprintf(path, "%s%s.lst", dir, stem);
        write_lst(path, src);
    }
    if (rc != 0 || g_diag.nerr) {
        printf("\nFatal errors encountered ... compilation aborted\n");
        return 1;
    }
    if (fj || (!fx && !fl && !fe && !fa)) {
        sprintf(path, "%s.jed", outbase);
        if (write_jed(path, &fr, fg) != 0) { printf("[0010cc] disk write error:  %s\n", path); return 1; }
    }
    if (fx || ff) {
        sprintf(path, "%s.doc", outbase);
        write_doc(path, &fr, ff, fx);
    }
    if (fa || fs) {
        CsDesign cd;
        int jed_written = fj || (!fx && !fl && !fe && !fa);
        design_for_sim(&cd, &fr, used_device);
        sprintf(path, "%s.abs", outbase);
        if (cs_abs_write(path, &cd) != 0) { printf("[0010cc] disk write error:  %s\n", path); return 1; }
        if (fs) rc = simulate(&cd, dir, stem, jed_written && fj ? outbase : NULL);
        cd.dev = NULL;
        cs_design_free(&cd);
        if (fs && rc) return 1;
    }
    return 0;
}
