/* SPDX-License-Identifier: GPL-3.0-or-later */
/* equiv - behavioural equivalence of two JEDEC files.  DARKSOFT.
   Exit status 0 equivalent, 1 different, 2 error. */
#include "devload.h"
#include "sim.h"
#include "util.h"
#include "version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const Device *D;
static Sim A, B;
static int nin, inpins[SIM_MAXPINS];
static int nst, stmc[SIM_MAXPINS];
static int ignore[SIM_MAXPINS + 1];
static int quiet;

static void usage(void)
{
    fprintf(stderr, "equiv %s (%s) - JEDEC behavioural equivalence\n"
            "usage: equiv [-d dev] [-D file.dev] [-n bits] [-r points] [-S seqs]\n"
            "             [-s seed] [-i pins] [-q] a.jed b.jed\n"
            "  -n  exhaustive up to this many input and register bits (22)\n"
            "  -r  random points otherwise (300000)\n"
            "  -S  random clocked sequences of 64 steps (500)\n"
            "  -i  pins to ignore, comma separated\n"
            "exit status 0 equivalent, 1 different, 2 error\n",
            FREECUPL_VERSION, FREECUPL_CREDIT);
    exit(2);
}

static int pin_reads_matter(const Sim *s, int pin)
{
    const Device *d = s->d;
    int k, r;
    for (k = 0; k < d->nlines; k++) {
        LineSrc src = s->lines[k];
        int m;
        if (src.pin != pin) continue;
        if (src.kind == SRC_FB) {
            m = dev_mc_at(d, pin);
            if (SIM_HASREG(&s->mc[m]) && s->mc[m].fb != GF_PIN && !s->mc[m].bypass) continue; /* reads Q, not the pin */
        } else if (src.kind != SRC_PIN) continue;
        for (r = 0; r < d->nrows; r++) if (sim_row_uses_line(s, r, k)) return 1;
    }
    return 0;
}

/* a register whose state can matter: shown on its pin (output enable not
   constant false) or read by a live row through its feedback line */
static int is_reg(const Sim *s, int m)
{
    const McCfg *c = &s->mc[m];
    const Device *d = s->d;
    int k, r;
    if (!SIM_HASREG(c)) return 0;
    if (c->kind == MK_REG && !c->node && !(c->oe_kind == OE_ROW && !s->live[c->oe_row])) return 1;
    if (c->fb == GF_PIN) return 0;   /* the feedback line reads the pin (multiplexer) */
    for (k = 0; k < d->nlines; k++) {
        if (s->lines[k].kind != SRC_FB || s->lines[k].pin != d->mc[m].pin) continue;
        for (r = 0; r < d->nrows; r++) if (sim_row_uses_line(s, r, k)) return 1;
    }
    return 0;
}

static int uses_oe_pin(const Sim *s)
{
    int m;
    for (m = 0; m < s->d->nmc; m++)
        if (s->mc[m].kind != MK_NONE && (s->mc[m].oe_kind == OE_PIN || s->mc[m].oe_pin_too)) return 1;
    return 0;
}

static void apply(Sim *s, const unsigned char *in, const unsigned char *st)
{
    int i;
    for (i = 1; i <= D->npins; i++) sim_set_input(s, i, 0);
    for (i = 0; i < nin; i++) sim_set_input(s, inpins[i], in[i]);
    for (i = 0; i < nst; i++) if (is_reg(s, stmc[i])) sim_set_state(s, stmc[i], st[i]);
}

static void show_pins(const char *tag, Sim *s)
{
    int p;
    printf("  %s:", tag);
    for (p = 1; p <= D->npins; p++) {
        int m = dev_mc_at(D, p);
        if (m < 0 || s->mc[m].kind == MK_INPUT || s->mc[m].kind == MK_NONE) continue;
        printf(" p%d=%s", p, s->drv[p] ? (s->pin[p] ? "1" : "0") : "Z");
    }
    if (s->unstable) printf(" (unstable)");
    printf("\n");
}

static void report(const char *what, const unsigned char *in, const unsigned char *st, int pin)
{
    int i;
    if (quiet) return;
    printf("DIFFERENT (%s", what);
    if (pin) printf(" at pin %d", pin);
    printf(")\n  inputs:");
    for (i = 0; i < nin; i++) printf(" p%d=%d", inpins[i], in[i]);
    if (nst) {
        printf("\n  state (pin level):");
        for (i = 0; i < nst; i++) printf(" p%d=%d", D->mc[stmc[i]].pin, st[i]);
    }
    printf("\n");
    show_pins("a", &A);
    show_pins("b", &B);
}

/* compare settled outputs; returns 0 if equal */
static int cmp_outputs(int *badpin)
{
    int p;
    if (A.unstable != B.unstable) { *badpin = 0; return 1; }
    for (p = 1; p <= D->npins; p++) {
        int m = dev_mc_at(D, p);
        if (m < 0 || ignore[p]) continue;
        if (A.drv[p] != B.drv[p] || (A.drv[p] && A.pin[p] != B.pin[p])) { *badpin = p; return 1; }
    }
    return 0;
}

static int cmp_states(int *badpin)
{
    int i;
    for (i = 0; i < nst; i++) {
        int m = stmc[i];
        if (ignore[D->mc[m].pin]) continue;
        if (is_reg(&A, m) && is_reg(&B, m) && sim_get_state(&A, m) != sim_get_state(&B, m)) { *badpin = D->mc[m].pin; return 1; }
    }
    return 0;
}

/* evaluate one point; returns 0 if equal */
static int check_point(const unsigned char *in, const unsigned char *st, int clock)
{
    int bad;
    apply(&A, in, st);
    apply(&B, in, st);
    sim_settle(&A);
    sim_settle(&B);
    if (cmp_outputs(&bad)) { report("outputs", in, st, bad); return 1; }
    if (clock) {
        sim_clock(&A);
        sim_clock(&B);
        if (cmp_states(&bad)) { report("next state after clock", in, st, bad); return 1; }
        if (cmp_outputs(&bad)) { report("outputs after clock", in, st, bad); return 1; }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *devname = NULL, *pa = NULL, *pb = NULL;
    int maxbits = 22, i, p, m, clock;
    long npoints = 300000, nseq = 500;
    unsigned long long seed = 1;
    Jed ja, jb;
    char err[256];
    unsigned char in[SIM_MAXPINS], st[SIM_MAXPINS];
    Rng rng;
    dev_load_builtin();
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i + 1 < argc) devname = argv[++i];
        else if (!strcmp(argv[i], "-D") && i + 1 < argc) load_devfile(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) maxbits = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) npoints = atol(argv[++i]);
        else if (!strcmp(argv[i], "-S") && i + 1 < argc) nseq = atol(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) seed = (unsigned long long)atol(argv[++i]);
        else if (!strcmp(argv[i], "-q")) quiet = 1;
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) {
            char *t = argv[++i], *q;
            for (q = strtok(t, ","); q; q = strtok(NULL, ",")) {
                int x = atoi(q[0] == 'p' ? q + 1 : q);
                if (x > 0 && x <= SIM_MAXPINS) ignore[x] = 1;
            }
        } else if (argv[i][0] == '-') usage();
        else if (!pa) pa = argv[i];
        else if (!pb) pb = argv[i];
        else usage();
    }
    if (!pa || !pb) usage();
    if (maxbits > 30) maxbits = 30;
    must_read_jed(pa, &ja, 0);
    must_read_jed(pb, &jb, 0);
    D = pick_device(devname, &ja, pa);
    if (pick_device(devname ? devname : (jb.device ? jb.device : D->name), &jb, pb) != D)
        die("%s and %s are for different devices", pa, pb);
    if (sim_init(&A, D, ja.fuse, err, sizeof err) < 0) die("%s: %s", pa, err);
    if (sim_init(&B, D, jb.fuse, err, sizeof err) < 0) die("%s: %s", pb, err);
    for (p = 1; p <= D->npins; p++) {
        if (p == D->gnd || p == D->vcc) continue;
        if (!(sim_pin_is_input(&A, p) || sim_pin_is_input(&B, p))) continue;
        if (pin_reads_matter(&A, p) || pin_reads_matter(&B, p) ||
            (p == D->oe_pin && (uses_oe_pin(&A) || uses_oe_pin(&B))))
            inpins[nin++] = p;
    }
    for (m = 0; m < D->nmc; m++) if (is_reg(&A, m) || is_reg(&B, m)) stmc[nst++] = m;
    clock = nst > 0;
    if (!quiet) printf("device %s: modes %s / %s, %d relevant inputs, %d register bits\n",
                       D->name, sim_mode_name(&A), sim_mode_name(&B), nin, nst);
    if (nin + nst <= maxbits) {
        unsigned long long n = 1ULL << (nin + nst), x;
        for (x = 0; x < n; x++) {
            for (i = 0; i < nin; i++) in[i] = (unsigned char)((x >> i) & 1);
            for (i = 0; i < nst; i++) st[i] = (unsigned char)((x >> (nin + i)) & 1);
            if (check_point(in, st, clock)) return 1;
        }
        if (!quiet) printf("EQUIVALENT (exhaustive, %llu points)\n", n);
        return 0;
    }
    rng_seed(&rng, seed);
    for (i = 0; i < npoints; i++) {
        int k;
        for (k = 0; k < nin; k++) in[k] = (unsigned char)rng_bit(&rng);
        for (k = 0; k < nst; k++) st[k] = (unsigned char)rng_bit(&rng);
        if (check_point(in, st, clock)) return 1;
    }
    if (clock) {
        long q;
        for (q = 0; q < nseq; q++) {
            int k, step, bad;
            for (k = 0; k < nst; k++) st[k] = (unsigned char)rng_bit(&rng);
            for (k = 0; k < nin; k++) in[k] = (unsigned char)rng_bit(&rng);
            apply(&A, in, st);
            apply(&B, in, st);
            for (step = 0; step < 64; step++) {
                for (k = 0; k < nin; k++) {
                    in[k] = (unsigned char)rng_bit(&rng);
                    sim_set_input(&A, inpins[k], in[k]);
                    sim_set_input(&B, inpins[k], in[k]);
                }
                sim_settle(&A);
                sim_settle(&B);
                if (cmp_outputs(&bad)) { report("outputs in random sequence", in, st, bad); return 1; }
                sim_clock(&A);
                sim_clock(&B);
                if (cmp_states(&bad) || cmp_outputs(&bad)) { report("state in random sequence", in, st, bad); return 1; }
            }
        }
    }
    if (!quiet) printf("EQUIVALENT (random: %ld points%s)\n", npoints, clock ? ", plus clocked sequences" : "");
    return 0;
}
