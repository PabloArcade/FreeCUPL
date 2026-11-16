/* SPDX-License-Identifier: GPL-3.0-or-later */
/* jedsim - fuse-level simulator for JEDEC files.  DARKSOFT. */
#include "devload.h"
#include "sim.h"
#include "util.h"
#include "version.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    fprintf(stderr, "jedsim %s (%s) - JEDEC fuse-level simulator\n"
            "usage: jedsim [-d device] [-D file.dev] [-l | -c] file.jed\n"
            "       jedsim -L\n"
            "  -l  print the decoded configuration   -c  check checksums only\n"
            "  -L  list devices\n"
            "Otherwise commands are read from stdin:\n"
            "  v CHARS        one char per pin: 0/1 drive, C clock, H/L/Z expect\n"
            "  set P=V ...    drive inputs          preload P=V ...  set registers\n"
            "  clock          clock pulse           eval             print pins\n"
            "  state          print registers\n", FREECUPL_VERSION, FREECUPL_CREDIT);
    exit(2);
}

static void print_pins(Sim *s, FILE *o)
{
    const Device *d = s->d;
    int p;
    fputs("= ", o);
    for (p = 1; p <= d->npins; p++) {
        if (p == d->gnd || p == d->vcc) fputc('N', o);
        else if (s->drv[p]) fputc(s->pin[p] ? 'H' : 'L', o);
        else fputc(s->pin[p] ? '1' : '0', o);
    }
    if (s->unstable) fputs(" unstable", o);
}

static int assign(Sim *s, char *arg, int preload)
{
    char *eq = strchr(arg, '=');
    int pin, v, k;
    if (!eq) return -1;
    *eq = 0;
    pin = atoi(arg[0] == 'p' ? arg + 1 : arg);
    v = atoi(eq + 1) ? 1 : 0;
    if (pin < 1 || (pin > s->d->npins && (!preload || pin > SIM_MAXPINS))) return -1;
    if (!preload) { sim_set_input(s, pin, v); return 0; }
    k = dev_mc_at(s->d, pin);
    if (k < 0 || !SIM_HASREG(&s->mc[k])) return -1;
    sim_set_state(s, k, v);
    return 0;
}

static int run_commands(Sim *s, FILE *in, FILE *o)
{
    char line[1024];
    int lineno = 0, bad = 0;
    const Device *d = s->d;
    while (fgets(line, sizeof line, in)) {
        char *p = line, *cmd;
        lineno++;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        cmd = strtok(p, " \t\r\n");
        if (!strcmp(cmd, "v")) {
            char *vec = strtok(NULL, " \t\r\n");
            int i, clk = 0, mism = 0;
            if (!vec || (int)strlen(vec) != d->npins) { fprintf(o, "error line %d: vector needs %d chars\n", lineno, d->npins); bad = 1; continue; }
            for (i = 0; i < d->npins; i++) {
                char c = (char)toupper((unsigned char)vec[i]);
                if (c == '0' || c == '1') sim_set_input(s, i + 1, c == '1');
                else if (c == 'C' || c == 'K') { sim_set_input(s, i + 1, 0); if (i + 1 == d->clk_pin) clk = 1; }
            }
            if (clk) sim_clock(s); else sim_settle(s);
            print_pins(s, o);
            for (i = 0; i < d->npins; i++) {
                char c = (char)toupper((unsigned char)vec[i]);
                int p = i + 1;
                if ((c == 'H' && !(s->drv[p] && s->pin[p])) || (c == 'L' && !(s->drv[p] && !s->pin[p])) || (c == 'Z' && s->drv[p])) {
                    if (!mism) fputs(" mismatch:", o);
                    fprintf(o, " %d", p);
                    mism = 1;
                    bad = 1;
                }
            }
            fputc('\n', o);
        } else if (!strcmp(cmd, "set") || !strcmp(cmd, "preload")) {
            char *a;
            int pre = !strcmp(cmd, "preload");
            while ((a = strtok(NULL, " \t\r\n")) != NULL)
                if (assign(s, a, pre) < 0) { fprintf(o, "error line %d: bad assignment %s\n", lineno, a); bad = 1; }
        } else if (!strcmp(cmd, "clock")) {
            sim_clock(s);
        } else if (!strcmp(cmd, "eval")) {
            sim_settle(s);
            print_pins(s, o);
            fputc('\n', o);
        } else if (!strcmp(cmd, "state")) {
            int k, any = 0;
            for (k = 0; k < d->nmc; k++)
                if (SIM_HASREG(&s->mc[k])) { fprintf(o, "%sp%d=%d", any ? " " : "", d->mc[k].pin, sim_get_state(s, k)); any = 1; }
            fputc('\n', o);
        } else {
            fprintf(o, "error line %d: unknown command %s\n", lineno, cmd);
            bad = 1;
        }
        fflush(o);
    }
    return bad;
}

int main(int argc, char **argv)
{
    const char *devname = NULL, *path = NULL;
    int describe = 0, check = 0, list = 0, i;
    Jed j;
    const Device *d;
    Sim s;
    char err[256];
    dev_load_builtin();
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) devname = argv[++i];
        else if (!strcmp(argv[i], "-D") && i + 1 < argc) load_devfile(argv[++i]);
        else if (!strcmp(argv[i], "-l")) describe = 1;
        else if (!strcmp(argv[i], "-c")) check = 1;
        else if (!strcmp(argv[i], "-L")) list = 1;
        else if (argv[i][0] == '-') usage();
        else path = argv[i];
    }
    if (list) {
        for (i = 0; i < dev_count(); i++) {
            const Device *x = dev_get(i);
            int k;
            printf("%-10s pins=%d fuses=%d array=%dx%d", x->name, x->npins, x->nfuses, x->ncols, x->nrows);
            for (k = 0; k < x->nalias; k++) printf(" %s%s%s", x->alias[k].name, x->alias[k].mode[0] ? ":" : "", x->alias[k].mode);
            printf("\n");
        }
        return 0;
    }
    if (!path) usage();
    must_read_jed(path, &j, check);
    if (check) {
        unsigned c = jed_fuse_checksum(j.fuse, j.nfuses);
        int ok = 1;
        printf("fuses %d, fuse checksum %04X", j.nfuses, c);
        if (j.has_fcsum) { printf(" (file %04X %s)", j.fcsum, j.fcsum == c ? "ok" : "BAD"); if (j.fcsum != c) ok = 0; }
        if (j.has_xcsum) {
            printf(", transmission %04X (file %04X %s)", j.xcsum_calc, j.xcsum, (j.xcsum == j.xcsum_calc || j.xcsum == 0) ? "ok" : "BAD");
            if (j.xcsum != j.xcsum_calc && j.xcsum != 0) ok = 0;
        }
        printf("\n");
        jed_free(&j);
        return ok ? 0 : 1;
    }
    d = pick_device(devname, &j, path);
    if (sim_init(&s, d, j.fuse, err, sizeof err) < 0) die("%s: %s", path, err);
    if (describe) {
        Buf b = {0, 0, 0};
        sim_describe(&s, &b);
        fputs(b.p, stdout);
        buf_free(&b);
        return 0;
    }
    i = run_commands(&s, stdin, stdout);
    sim_free(&s);
    jed_free(&j);
    return i ? 1 : 0;
}
