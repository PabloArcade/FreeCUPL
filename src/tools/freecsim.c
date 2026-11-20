/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecsim - simulator command line.  DARKSOFT. */
#include "device.h"
#include "jed.h"
#include "util.h"
#include "version.h"
#include "namebase.h"
#include "bugs.h"
#include "csim/csim.h"
#include "sim3.h"
#include "cupl/output.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    printf("freecsim - CUPL simulator, %s\n", FREECUPL_VERSION_STRING);
    printf("usage: freecsim -flags [library] [device] file[.si]\n"
           "  -l   write listing (.so)          -j   add vectors to JEDEC file\n"
           "  -n   name JEDEC file after source -v   listing to the terminal\n"
           "  -u   library name (ignored)       -w -d   accepted, ignored\n"
           "  -fuses FILE.jed    simulate the fuse map of FILE\n"
           "  -simlatch          simulate latched outputs and input registers\n"
           "                     (timing not verified; default X as WinCUPL)\n"
           "  -fix               correct WinCUPL 5.0a bugs (docs/BUGS.txt)%s\n"
           "  -compat-refdefect  reproduce WinCUPL 5.0a, bugs included%s\n"
           "  -V                 print version\n",
           FREECUPL_FIX_DEFAULT ? " (default)" : "", FREECUPL_FIX_DEFAULT ? "" : " (default)");
}

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

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* path with extension ext (lower case first, then upper case) */
static char *with_ext(const char *dir, const char *stem, const char *ext)
{
    Buf b = {0, 0, 0};
    char up[16];
    int i;
    buf_printf(&b, "%s%s.%s", dir, stem, ext);
    if (file_exists(b.p)) return b.p;
    for (i = 0; ext[i] && i < 15; i++) up[i] = (char)toupper((unsigned char)ext[i]);
    up[i] = 0;
    buf_free(&b);
    buf_printf(&b, "%s%s.%s", dir, stem, up);
    if (file_exists(b.p)) return b.p;
    buf_free(&b);
    buf_printf(&b, "%s%s.%s", dir, stem, ext);
    return b.p;
}

int main(int argc, char **argv)
{
    int i, fj = 0, fl = 0, fn = 0, fv = 0, fu = 0, nopos = 0, rc, info = 0, simlatch = 0;
    const char *pos[8], *fuses = NULL, *device = NULL;
    char *src, *dir, *stem, *abs, *sip, err[512];
    CsDesign d;
    CsOpts o;
    Buf so = {0, 0, 0}, wo = {0, 0, 0}, jp = {0, 0, 0}, nm = {0, 0, 0}, ver = {0, 0, 0}, cr = {0, 0, 0};

    dev_load_builtin();
    if (argc < 2) { usage(); return 1; }
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-fuses") && i + 1 < argc) { fuses = argv[++i]; continue; }
        if (!strcmp(a, "-info")) { info = 1; continue; }
        if (!strcmp(a, "-simlatch")) { simlatch = 1; continue; }
        if (!strcmp(a, "-fix")) { g_fix = 1; continue; }
        if (!strcmp(a, "-compat-refdefect")) { g_fix = 0; continue; }
        if (!strcmp(a, "-V") || !strcmp(a, "--version")) { printf("%s\n", FREECUPL_VERSION_STRING); return 0; }
        if (a[0] == '-' && a[1]) {
            const char *f;
            for (f = a + 1; *f; f++) {
                switch (tolower((unsigned char)*f)) {
                case 'j': fj = 1; break;
                case 'l': fl = 1; break;
                case 'n': fn = 1; break;
                case 'v': fv = 1; break;
                case 'u': fu = 1; break;
                case 'w': case 'd': break;
                default:
                    printf("[0007sk] invalid flag:  -%c\n", *f);
                    return 1;
                }
            }
            continue;
        }
        if (nopos < 8) pos[nopos++] = a;
    }
    if (nopos == 0) { usage(); return 1; }
    src = (char *)pos[nopos - 1];
    if (nopos >= 2) {
        int first = 0;
        size_t l0 = strlen(pos[0]);
        if (fu || nopos >= 3 || (l0 > 3 && str_ieq(pos[0] + l0 - 3, ".dl"))) first = 1;
        if (nopos - 1 > first) device = pos[nopos - 2];
    }
    dir = dir_of(src);
    stem = stem_of(src);
    sip = file_exists(src) && strrchr(src, '.') > strrchr(src, '/') ? xstrdup(src) : with_ext(dir, stem, "si");
    abs = with_ext(dir, stem, "abs");
    if (cs_abs_read(abs, &d, err, sizeof err) != 0) {
        printf("[0001sa] %s\n", err);
        return 1;
    }
    if (device && !str_ieq(device, d.mnemonic)) {
        const Device *dv = dev_find(device, NULL);
        if (!dv) { printf("[0007sa] device not in library:  %s\n", device); cs_design_free(&d); return 1; }
        if (dv != d.dev) { printf("[0010sa] expecting device:  %s\n", d.mnemonic); cs_design_free(&d); return 1; }
    }
    if (fuses) {
        Jed j;
        if (jed_read_file(fuses, &j, err, sizeof err) != 0 || j.nfuses != d.dev->nfuses) {
            printf("[0001sa] could not open:  %s\n", fuses);
            cs_design_free(&d);
            return 1;
        }
        memcpy(d.fuse, j.fuse, (size_t)j.nfuses);
        jed_free(&j);
    }
    if (info) {
        Sim3 t;
        if (sim3_init(&t, d.dev, d.fuse, err, sizeof err) != 0) { printf("error %s\n", err); cs_design_free(&d); return 1; }
        printf("device %s clock %d oepin %d\n", d.mnemonic, d.dev->clk_pin, d.dev->oe_pin);
        for (i = 0; i < d.nsym; i++) {
            int m = sim3_mc_of(&t, d.sym[i].pin);
            const char *role = "in";
            if (d.sym[i].pin > d.dev->npins) role = "node";
            else if (sim3_is_output(&t, d.sym[i].pin)) {
                const McCfg *mc = &t.s.mc[m];
                role = mc->kind == MK_REG ? "reg" : "comb";
                if (mc->oe_kind != OE_ALWAYS) role = mc->kind == MK_REG ? "regoe" : "combo";
            }
            if (d.sym[i].pin == d.dev->gnd || d.sym[i].pin == d.dev->vcc) continue;
            printf("sig %s %d %d %s%s\n", d.sym[i].name, d.sym[i].pin, d.sym[i].neg, role,
                   d.sym[i].pin == d.dev->clk_pin ? " clock" : "");
        }
        for (i = 0; i < d.nfld; i++) printf("field %s %d\n", d.fld[i].name, d.fld[i].n);
        sim3_free(&t);
        cs_design_free(&d);
        return 0;
    }
    memset(&o, 0, sizeof o);
    o.simlatch = simlatch;
    o.refdefect = !g_fix;
    o.si_path = sip;
    buf_printf(&nm, "%s.si", stem);
    o.si_name = nm.p;
    buf_printf(&so, "%s%s.so", dir, stem);
    buf_printf(&wo, "%s%s.wo", dir, stem);
    o.so_path = fl || fv ? so.p : NULL;
    o.wo_path = wo.p;
    if (fj) {
        /* as in freecupl */
        if (fn || !d.hdr[CH_NAME]) buf_printf(&jp, "%s%s.jed", dir, stem);
        else {
            char nb[64];
            int bad = name_file_base(d.hdr[CH_NAME], nb, sizeof nb) != 0;
            buf_printf(&jp, "%s%s.jed", dir, nb);
            if (bad) { printf("[0001sa] could not open:  %s.jed\n", nb); return 1; }
        }
        o.jed_path = jp.p;
    }
    buf_printf(&ver, "%s", FREECUPL_VERSION_STRING);
    buf_printf(&cr, "CREATED %s", created_string());
    o.banner[0] = "FreeCSIM: CUPL compatible simulation program";
    o.banner[1] = ver.p;
    o.banner[2] = "";
    o.banner[3] = cr.p;
    o.verbose = fv;
    rc = csim_run(&d, &o);
    if (rc == 2) printf("[0001sa] could not open:  %s\n", sip);
    else if (rc) printf("Simulation errors encountered\n");
    cs_design_free(&d);
    return rc ? 1 : 0;
}
