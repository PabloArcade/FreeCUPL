/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - fitter: design to device, fuse map.  DARKSOFT. */
#include "fit.h"
#include "diag.h"
#include "bugs.h"
#include "sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Sym *s;
    int pin, mc;
    Eqn *eq[X_COUNT];
    unsigned char minext[X_COUNT];   /* MIN statement for an extension without equation */
    int input;
    int out, reg, comb;
    int inv;            /* pin = sum ^ inv (next value for registers) */
    int quirk;
    int regcfg;         /* macrocell configured as register */
    int tff;            /* registered with .T (T flip-flop) */
    int kbflip;         /* -kb complemented this output's sum */
    int combspill;      /* BUG_COMBFULL reproduced: the last term goes into the next row */
    int nodecomb;       /* BUG_NODECOMB reproduced on this buried node */
    int kbfb_warned;
    int lq;             /* read through its input latch (.LQ / .IOL) */
    int tfbread;        /* read through .TFB: the register is a T flip-flop */
    int muxio, muxdfb;
    int plainread;      /* read by its plain name */
    int latchdone;      /* .L moved to the register's equation */  /* read through .IO / .DFB on a pin whose line is a feedback multiplexer */
    Cover sum;
    Cover xsum;         /* MC_GEN second (XOR) sum */
    int sumok;
} PinInfo;

static const Device *D;
static int MODE;
static const char *MODENAME;
static FitOpts O;
static PinInfo *PI;
static int NP;
static Sym *VS[MAXVARS];
static int VE[MAXVARS];
static int NV;
static int g_mark;
static unsigned char *g_undef_reported;
static unsigned char *g_inprog;
static unsigned char *g_cycle_reported;
static unsigned char *g_badext_reported;
static FitResult *R;

/* the resolved mnemonic in capitals, as the messages print it */
static const char *upname(void)
{
    static char up[40];
    size_t k;
    snprintf(up, sizeof up, "%s", R->resolved);
    for (k = 0; up[k]; k++) if (up[k] >= 'a' && up[k] <= 'z') up[k] = (char)(up[k] - 'a' + 'A');
    return up;
}
static unsigned char *g_split;
static unsigned char *g_qsplit;   /* per macrocell: its register (node alias) has its own sum */
static int g_lqbad;               /* .LQ/.IOL on a pin without an input latch (checked before the mode) */

static int inflag_of(int pin)
{
    int i;
    for (i = 0; i < D->nnolatch; i++) if (D->nolatch_pin[i] == pin) return 0;
    for (i = 0; i < D->ninflag; i++) if (D->inflag_pin[i] == pin) return 1;
    return 0;
}

static int tec_of(int pin)
{
    int i;
    for (i = 0; i < D->ntec; i++) if (D->tec_pin[i] == pin) return i;
    return -1;
}
static int g_aliasread_warned;
static int g_dupline_warned;  /* per macrocell: split sum in use (buried partner used) */

const char *fit_var_name(int v, int *ext)
{
    if (v < 0 || v >= NV) return "?";
    if (ext) *ext = VE[v];
    return VS[v]->name;
}

static int var_of(Sym *s, int ext)
{
    int i;
    for (i = 0; i < NV; i++) if (VS[i] == s && VE[i] == ext) return i;
    if (NV == MAXVARS) {
        diag(DG_FATAL, "0015cb", 0, "out of memory:  too many variables");
        return 0;
    }
    VS[NV] = s;
    VE[NV] = ext;
    return NV++;
}

static PinInfo *pin_info(Sym *s)
{
    if ((s->kind != SK_PIN && s->kind != SK_NODE) || s->pin < 1 || s->pin > NP) return NULL;
    if (PI[s->pin].s != s) return NULL;
    return &PI[s->pin];
}

static void name_ext(char *buf, size_t n, const Sym *s, int ext)
{
    snprintf(buf, n, "%s%s%s", s->name, ext ? "." : "", ext ? ext_names[ext] : "");
}

static void cover_of(const Expr *e, Cover *out);
static int has_fb_and_pin_lines(int pin);

static void lit_cover(Cover *out, int v, int pol)
{
    Cube c;
    cube_universe(&c);
    cube_set(&c, v, pol);
    cv_clear(out);
    cv_add(out, &c);
}

static void const_cover(Cover *out, int val)
{
    cv_clear(out);
    if (val) { Cube u; cube_universe(&u); cv_add(out, &u); }
}

static void intermediate_cover(Sym *s, Cover *out)
{
    Eqn *q = eqn_find(s, X_NONE);
    if (!q || !q->e) {
        if (!g_undef_reported[s->id]) {
            g_undef_reported[s->id] = 1;
            diag(DG_ERROR, "0018ca", 0, "intermediate var not assigned an expression:  %s", s->name);
        }
        const_cover(out, 0);
        return;
    }
    if (g_inprog[s->id]) {
        if (!g_cycle_reported[s->id]) {
            g_cycle_reported[s->id] = 1;
            diag(DG_ERROR, "0030ca", q->line, "intermediate dependent on itself:  %s", s->name);
        }
        const_cover(out, 0);
        return;
    }
    g_inprog[s->id] = 1;
    cover_of(q->e, out);
    if (q->neg) { Cover t; cv_init(&t); cv_not(&t, out); cv_free(out); *out = t; }
    g_inprog[s->id] = 0;
}

static void leaf_cover(const Expr *e, Cover *out)
{
    Sym *s = e->sym;
    if (s->kind == SK_PIN || s->kind == SK_NODE) {
        PinInfo *pi = pin_info(s);
        char nm[160];
        if ((e->ext == X_DFB || e->ext == X_IO) && pi && s->kind == SK_PIN && has_fb_and_pin_lines(s->pin) && !(e->ext == X_DFB && D->nodfb)) {
            /* register feedback (.dfb) or pin (.io) of a macrocell that has both */
            if (g_mark) pi->input = 1;
            lit_cover(out, var_of(s, e->ext), !e->neg);
            return;
        }
        if ((e->ext == X_DFB || e->ext == X_IO) && pi && s->kind == SK_PIN && pi->mc >= 0 && D->mc[pi->mc].type == MC_GEN &&
            D->mc[pi->mc].fb_fuse >= 0 && !has_fb_and_pin_lines(s->pin)) {
            /* one line through a feedback multiplexer (PAL18CV8): .io selects
               the pin, .dfb the register */
            if (g_mark) { pi->input = 1; if (e->ext == X_IO) pi->muxio = 1; else pi->muxdfb = 1; }
            lit_cover(out, var_of(s, e->ext), !e->neg);
            return;
        }
        if (e->ext == X_TFB && D->tfb && pi && s->kind == SK_PIN && has_fb_and_pin_lines(s->pin) && pi->mc >= 0 && D->mc[pi->mc].t_fuse >= 0) {
            /* .TFB: the register's feedback, read like .DFB; the register
               becomes a T flip-flop */
            if (g_mark) { pi->input = 1; pi->tfbread = 1; }
            lit_cover(out, var_of(s, X_DFB), !e->neg);
            return;
        }
        if ((e->ext == (D->inflag_dq ? X_DQ : X_LQ) || e->ext == X_IOL) && D->ninflag && s->kind == SK_PIN) {
            /* read through the pin's input latch (or register, .DQ): the
               same array line; the pin's 'input transparent' fuse stays 0 */
            if (inflag_of(s->pin) && (e->ext != X_IOL || dev_mc_at(D, s->pin) >= 0)) {
                if (g_mark && pi) { pi->input = 1; pi->lq = 1; }
                /* a variable of its own (as the reference: 'a & !a.lq' is
                   not simplified; both literals reach the same column) */
                lit_cover(out, var_of(s, e->ext), !e->neg);
                return;
            }
            name_ext(nm, sizeof nm, s, e->ext);
            if (!g_badext_reported[s->id]) {
                g_badext_reported[s->id] = 1;
                g_lqbad = 1;
                diag(DG_ERROR, "0012cb", 0, "pin/node %d invalid usage:  %s", s->pin, nm);
            }
            const_cover(out, 0);
            return;
        }
        if (e->ext != X_NONE) {
            name_ext(nm, sizeof nm, s, e->ext);
            if (!g_badext_reported[s->id]) {
                g_badext_reported[s->id] = 1;
                if (e->ext == X_D) diag(DG_ERROR, "0014cb", 0, "pin/node %d invalid input:  %s", s->pin, nm);
                else {
                    diag(DG_ERROR, "0011cb", 0, "unknown extension:  %s", nm);
                    diag(DG_ERROR, "0012cb", 0, "pin/node %d invalid usage:  %s", s->pin, nm);
                }
            }
            const_cover(out, 0);
            return;
        }
        if (g_mark && pi) { pi->input = 1; pi->plainread = 1; }
        if (g_mark && !pi && s->kind == SK_PIN && !g_badext_reported[s->id]) {
            g_badext_reported[s->id] = 1;
            if (s->pin < 1 || s->pin > D->npins) diag(DG_ERROR, "0012cb", 0, "pin/node %d invalid usage:  %s", s->pin, s->name);
        }
        if (s->kind == SK_NODE && !pi && g_mark && !g_badext_reported[s->id]) {
            g_badext_reported[s->id] = 1;
            diag(DG_ERROR, "0012cb", 0, "pin/node %d invalid usage:  %s", s->pin, s->name);
        }
        lit_cover(out, var_of(s, X_NONE), !e->neg);
        return;
    }
    if (s->kind == SK_FIELD) {
        if (s->nmem >= 1) {
            Expr t = *e;
            t.sym = s->mem[0];
            leaf_cover(&t, out);
            return;
        }
    }
    intermediate_cover(s, out);
    if (e->ext != X_NONE) {
        if (!g_undef_reported[s->id]) {
            char nm[160];
            name_ext(nm, sizeof nm, s, e->ext);
            g_undef_reported[s->id] = 1;
            diag(DG_ERROR, "0018ca", 0, "intermediate var not assigned an expression:  %s", nm);
        }
    }
    if (e->neg) { Cover t; cv_init(&t); cv_not(&t, out); cv_free(out); *out = t; }
}

static void cover_of(const Expr *e, Cover *out)
{
    Cover a, b, t, na, nb;
    switch (e->op) {
    case E_CONST: const_cover(out, e->val); return;
    case E_VAR: leaf_cover(e, out); return;
    case E_NOT:
        cv_init(&a);
        cover_of(e->a, &a);
        cv_not(out, &a);
        cv_free(&a);
        return;
    case E_AND: case E_OR:
        cv_init(&a); cv_init(&b);
        cover_of(e->a, &a);
        cover_of(e->b, &b);
        if (e->op == E_AND) cv_and(out, &a, &b); else cv_or(out, &a, &b);
        cv_free(&a); cv_free(&b);
        return;
    case E_XOR:
        cv_init(&a); cv_init(&b); cv_init(&t); cv_init(&na); cv_init(&nb);
        cover_of(e->a, &a);
        cover_of(e->b, &b);
        cv_not(&na, &a);
        cv_not(&nb, &b);
        cv_and(&t, &a, &nb);
        cv_and(&na, &na, &b);
        cv_or(out, &t, &na);
        cv_free(&a); cv_free(&b); cv_free(&t); cv_free(&na); cv_free(&nb);
        return;
    }
}

/* -m0/-m1: complements are pushed down to the literals before multiplying
   out, which gives the same terms as the original */
static void nnf_cover(const Expr *e, int neg, Cover *out);

/* BUG_POL_CONST: when the whole expression is complemented for the output
   polarity, a complemented constant keeps its value.  g_polflip is that
   pending complement. */
static int g_polflip;

static const Expr *const_of(const Expr *e, int *neg)
{
    int guard = 0;
    *neg = 0;
    while (guard++ < 1000) {
        if (e->op == E_CONST) return e;
        if (e->op == E_NOT) { *neg ^= 1; e = e->a; continue; }
        if (e->op == E_VAR && e->ext == X_NONE && e->sym->kind == SK_NONE) {
            Eqn *x = eqn_find(e->sym, X_NONE);
            if (!x || !x->e || g_inprog[e->sym->id]) return NULL;
            *neg ^= e->neg ^ x->neg;
            e = x->e;
            continue;
        }
        return NULL;
    }
    return NULL;
}

static void nnf_leaf(const Expr *e, int neg, Cover *out)
{
    Sym *s = e->sym;
    if (s->kind == SK_PIN || s->kind == SK_NODE || s->kind == SK_FIELD || e->ext != X_NONE) {
        Expr t = *e;
        t.neg = (unsigned char)(e->neg ^ neg);
        leaf_cover(&t, out);
        return;
    }
    {
        Eqn *q = eqn_find(s, X_NONE);
        int cn;
        const Expr *c;
        if (!q || !q->e || g_inprog[s->id]) { intermediate_cover(s, out); return; }
        if (g_polflip && (c = const_of(e, &cn)) && (cn ^ c->neg)) { const_cover(out, c->val ^ cn ^ neg ^ 1); return; }
        g_inprog[s->id] = 1;
        nnf_cover(q->e, neg ^ e->neg ^ q->neg, out);
        g_inprog[s->id] = 0;
    }
}

static void nnf_cover(const Expr *e, int neg, Cover *out)
{
    Cover a, b, c, d;
    switch (e->op) {
    case E_CONST: const_cover(out, e->val ^ neg ^ (g_polflip && e->neg)); return;
    case E_VAR: nnf_leaf(e, neg, out); return;
    case E_NOT: {
        int cn;
        const Expr *c;
        if (g_polflip && (c = const_of(e, &cn)) && (cn ^ c->neg)) { const_cover(out, c->val ^ cn ^ neg ^ 1); return; }
        nnf_cover(e->a, !neg, out);
        return;
    }
    case E_AND: case E_OR:
        cv_init(&a); cv_init(&b);
        nnf_cover(e->a, neg, &a);
        nnf_cover(e->b, neg, &b);
        if ((e->op == E_AND) != (neg != 0)) cv_and(out, &a, &b); else cv_or(out, &a, &b);
        cv_free(&a); cv_free(&b);
        return;
    case E_XOR:
        /* a $ b = a&!b # !a&b ;  !(a $ b) = a&b # !a&!b */
        cv_init(&a); cv_init(&b); cv_init(&c); cv_init(&d);
        {
            /* the complement goes to the right operand only */
            int pf = g_polflip;
            g_polflip = 0;
            nnf_cover(e->a, 0, &a);
            g_polflip = pf;
        }
        nnf_cover(e->b, !neg, &b);
        cv_and(&c, &a, &b);
        {
            int pf = g_polflip;
            g_polflip = 0;
            nnf_cover(e->a, 1, &a);
            g_polflip = pf;
        }
        nnf_cover(e->b, neg, &b);
        cv_and(&d, &a, &b);
        cv_or(out, &c, &d);
        cv_free(&a); cv_free(&b); cv_free(&c); cv_free(&d);
        return;
    }
}

static const Expr *resolve_top(const Expr *r)
{
    int guard = 0;
    while (r->op == E_VAR && r->ext == X_NONE && r->sym->kind == SK_NONE && guard++ < 1000) {
        Eqn *x = eqn_find(r->sym, X_NONE);
        Expr *sub;
        if (!x || !x->e || g_inprog[r->sym->id]) break;
        sub = x->e;
        if (x->neg) sub = mk_not(sub);
        if (r->neg) sub = mk_not(sub);
        r = sub;
    }
    return r;
}

static const Expr *root_strip(const Eqn *q, int *flip)
{
    const Expr *r = resolve_top(q->e), *c;
    *flip = q->neg;
    if (r->op != E_NOT) return r;
    c = resolve_top(r->a);
    if (c->op == E_VAR || c->op == E_CONST) return r;
    *flip ^= 1;
    return c;
}

static int ngroups(const Eqn *q)
{
    int g, n = 0;
    for (g = 0; g < 4; g++) if (q->grp[g]) n++;
    return n;
}

/* -m0/-m1 with equations from several origins: each group is expanded and
   quick-minimised on its own, then the groups are concatenated (absorbed
   at -m1), as the reference does */
static void group_cover(const Eqn *q, int level, Cover *out)
{
    int g, first = 1;
    cv_clear(out);
    for (g = 0; g < 4; g++) {
        Cover t;
        if (!q->grp[g]) continue;
        cv_init(&t);
        g_logic_raw = level == 0;
        g_logic_dist = 1;
        nnf_cover(q->grp[g], 0, &t);
        if (level == 1) cv_quick_merge(&t);
        if (first) { cv_free(out); *out = t; first = 0; continue; }
        cv_or(out, out, &t);
        cv_free(&t);
    }
    if (level == 1) cv_absorb(out);
    g_logic_raw = 0;
    g_logic_dist = 0;
}

static int level_of(const Eqn *q)
{
    return q && q->minlvl >= 0 ? q->minlvl : O.minlevel;
}

/* cover of an equation's function at its minimisation level */
static void eqn_cover_neg(const Expr *e, int level, int neg, Cover *out)
{
    g_logic_raw = level == 0;
    g_logic_dist = level <= 1;
    if (level <= 1) nnf_cover(e, neg, out);
    else {
        cover_of(e, out);
        if (neg) { Cover t; cv_init(&t); cv_not(&t, out); cv_free(out); *out = t; }
    }
    g_logic_raw = 0;
    g_logic_dist = 0;
}

static int same_fn(const Cover *a, const Cover *b)
{
    Cover n, x;
    int d;
    cv_init(&n); cv_init(&x);
    cv_not(&n, a); cv_and(&x, b, &n); d = x.n;
    cv_not(&n, b); cv_and(&x, a, &n); d += x.n;
    cv_free(&n); cv_free(&x);
    return d == 0;
}

/* the cover of the sum of an output; neg = the complement for the output's
   polarity, under which WinCUPL keeps complemented constants
   (BUG_POL_CONST) */
static void eqn_cover_pol(const Expr *e, int level, int neg, const Sym *s, Cover *out)
{
    Cover w;
    eqn_cover_neg(e, level, neg, out);
    if (!neg) return;
    cv_init(&w);
    g_polflip = 1;
    g_logic_raw = level == 0;
    g_logic_dist = level <= 1;
    nnf_cover(e, neg, &w);
    g_polflip = 0;
    g_logic_raw = 0;
    g_logic_dist = 0;
    if (!same_fn(&w, out)) {
        diag_bug(DG_WARN, "0905cb", 0, BUG_POL_CONST, g_fix ? "%s: a complemented constant complemented with the rest of the equation for the output's polarity"
                                                     : "%s: a complemented constant keeps its value when the equation is complemented for the output's polarity", s->name);
        if (!g_fix) { cv_free(out); *out = w; return; }
    }
    cv_free(&w);
}

static void eqn_cover(const Expr *e, int level, Cover *out)
{
    eqn_cover_neg(e, level, 0, out);
}

/* m1 reproduces the reference quick minimisation (its result decides
   whether a design fits); m2..m4 use the Espresso-style minimiser */
static void minimize(Cover *f, int level, int capacity)
{
    if (level <= 0) return;
    if (level == 1) { cv_quick_merge(f); return; }
    cv_minimize(f, level);
    if (capacity > 0 && f->n > capacity) cv_minimize_exact(f, 14);
}

static int mc_capacity(int mc, int regcfg)
{
    const Macrocell *k = &D->mc[mc];
    int n = k->sum1 - k->sum0 + 1;
    if (k->type == MC_GEN && k->qsplit_fuse >= 0 && g_qsplit && g_qsplit[mc]) return k->qhalf1 - k->qhalf0 + 1;
    if (k->type == MC_GEN && k->split_fuse >= 0 && g_split && g_split[mc]) return k->half1 - k->half0 + 1;
    if (k->type == MC_GAL) {
        if (!strcmp(MODENAME, "simple")) return 8;
        if (!strcmp(MODENAME, "complex")) return 7;
        return regcfg ? 8 : 7;
    }
    return n;
}

static int mc_first_row(int mc, int regcfg)
{
    const Macrocell *k = &D->mc[mc];
    if (k->type == MC_GEN && k->qsplit_fuse >= 0 && g_qsplit && g_qsplit[mc]) return k->qhalf0;
    if (k->type == MC_GEN && k->split_fuse >= 0 && g_split && g_split[mc]) return k->half0;
    if (k->type == MC_GAL) {
        if (!strcmp(MODENAME, "simple")) return k->sum0;
        if (!strcmp(MODENAME, "complex")) return k->sum0 + 1;
        return regcfg ? k->sum0 : k->sum0 + 1;
    }
    return k->sum0;
}

static int mc_oe_row(int mc, int regcfg)
{
    const Macrocell *k = &D->mc[mc];
    if (k->type == MC_GAL) {
        if (!strcmp(MODENAME, "simple")) return -1;
        if (!strcmp(MODENAME, "complex")) return k->sum0;
        return regcfg ? -1 : k->sum0;
    }
    if (k->type == MC_REG) return -1;
    if (k->type == MC_GEN && k->oe_kind != GO_ROW && k->oe_kind != GO_ROWPIN) return -1;
    if (k->type == MC_GEN && k->oe_reg_pin && regcfg) return -1;
    return k->oe_row;
}

static int prog_polarity(int mc)
{
    int t = D->mc[mc].type;
    return t == MC_GAL || t == MC_V10 || (t == MC_GEN && D->mc[mc].pol_fuse >= 0);
}

/* extensions known to the device in the current mode */
static int ext_known(int ext)
{
    int i;
    if (ext == X_NONE) return 1;
    for (i = 0; i < D->nmc; i++) {
        const Macrocell *k = &D->mc[i];
        switch (k->type) {
        case MC_COMB: if (ext == X_OE && k->oe_row >= 0) return 1; break;
        case MC_REG: if (ext == X_D) return 1; break;
        case MC_GAL:
            if (!strcmp(MODENAME, "registered") && ext == X_T && k->t_fuse >= 0) return 1;
            if (!strcmp(MODENAME, "complex") && ext == X_OE) return 1;
            if (!strcmp(MODENAME, "registered") && (ext == X_OE || ext == X_D)) return 1;
            break;
        case MC_V10:
            if (ext == X_D || ext == X_OE) return 1;
            if (ext == X_AR && D->ar_row >= 0) return 1;
            if (ext == X_SP && D->sp_row >= 0) return 1;
            break;
        case MC_GEN:
            if (ext == X_D && k->reg_kind != GR_COMB) return 1;   /* (on a latch: .L, moved to .D before this check) */
            if (ext == X_L && k->latch) return 1;
            if (ext == X_T && k->reg_kind != GR_COMB && k->t_fuse >= 0) return 1;
            if (ext == X_OE && (k->oe_kind == GO_ROW || k->oe_kind == GO_ROWPIN)) return 1;
            if (ext == X_CK && k->ck_row >= 0) return 1;
            if (ext == X_CKMUX && k->ckmux_fuse >= 0) return 1;
            if (ext == X_OEMUX && k->oemux_fuse >= 0) return 1;   /* accepted, no effect (as the reference) */
            if (ext == X_CE && k->ce_fuse >= 0) return 1;
            if (ext == X_AR && (k->ar_row >= 0 || D->ar_row >= 0)) return 1;
            if (ext == X_AP && (k->ap_row >= 0 || D->ap_row >= 0)) return 1;
            if (ext == X_SP && (k->sp_row >= 0 || D->sp_row >= 0)) return 1;
            break;
        }
    }
    if (ext == X_TEC && (D->ntec || D->tecok)) return 1;
    return 0;
}

/* clock / reset / preset / clock-enable equations */
static int has_ctl(const PinInfo *pi)
{
    return pi->eq[X_CK] || pi->eq[X_AR] || pi->eq[X_AP] || pi->eq[X_SP] || pi->eq[X_CE];
}

static int alias_pin(int pin)
{
    int i;
    for (i = 0; i < D->nnodealias; i++) if (D->nodealias_node[i] == pin) return D->nodealias_pin[i];
    return 0;
}

/* the node alias of the register of the macrocell on pin (0: none) */
static int alias_node(int pin)
{
    int i;
    for (i = 0; i < D->nnodealias; i++) if (D->nodealias_pin[i] == pin) return D->nodealias_node[i];
    return 0;
}

/* a register named by its node alias that has its own sum (qsplit) */
static int qsplit_alias(int pin)
{
    int p = alias_pin(pin), m;
    if (!p || (m = dev_mc_at(D, p)) < 0) return 0;
    return D->mc[m].type == MC_GEN && D->mc[m].qsplit_fuse >= 0;
}

/* a macrocell with both a feedback and a pin line is read through the
   feedback line when registered (or named by its node alias) */
static int find_line_pref(int pin, int fb)
{
    const LineSrc *L = dev_lines(D, MODE);
    int k, any = -1, ap = alias_pin(pin);
    if (ap) { pin = ap; fb = 1; }
    for (k = 0; k < D->nlines; k++) {
        if ((L[k].kind == SRC_PIN || L[k].kind == SRC_FB) && L[k].pin == pin) {
            if ((L[k].kind == SRC_FB) == (fb != 0)) return k;
            if (any < 0) any = k;
        }
    }
    return any;
}

static int has_fb_and_pin_lines(int pin)
{
    const LineSrc *L = dev_lines(D, MODE);
    int k, fb = 0, pn = 0;
    for (k = 0; k < D->nlines; k++) {
        if (L[k].pin != pin) continue;
        if (L[k].kind == SRC_FB) fb = 1;
        if (L[k].kind == SRC_PIN) pn = 1;
    }
    return fb && pn;
}

static int find_line(int pin)
{
    PinInfo *pi = pin >= 1 && pin <= NP ? &PI[pin] : NULL;
    return find_line_pref(pin, pi && pi->regcfg);
}

/* inversion between a variable's logical value and its array line */
static int line_inv(Sym *s, int line);

static int line_inv_ext(Sym *s, int line, int ext)
{
    PinInfo *pi = pin_info(s);
    (void)line;
    /* as the reference: .dfb is the feedback line itself for an output,
       its complement otherwise; .io is the pin */
    if ((ext == X_DFB || ext == X_IO) && pi && pi->mc >= 0 && D->mc[pi->mc].type == MC_GEN && D->mc[pi->mc].fb_fuse >= 0 && !has_fb_and_pin_lines(s->pin)) {
        /* feedback multiplexer: .dfb of a register reads as the plain name,
           .io of a combinational output too; .io of a register is the pin */
        int xr = D->mc[pi->mc].fb_xor;
        if (ext == X_IO) return pi->regcfg ? s->neg : line_inv(s, line);
        if (pi->regcfg) return s->neg;   /* xr: the mux goes to the pin; otherwise as the reference (ATV750 rule): the declaration only */
        if (xr && !pi->out) return s->neg;   /* an input-only pin read through .dfb (as the reference) */
        return xr ? D->mc[pi->mc].fb == GF_NQ : pi->inv ^ s->neg;
    }
    if (ext == X_DFB && pi && pi->mc >= 0 && D->mc[pi->mc].type == MC_GEN && D->mc[pi->mc].fb == GF_XQ) return 1 ^ s->neg;
    /* .DFB/.TFB of a combinational output (ATV750): with Q or /Q feedback the
       column follows the output polarity, with xq/xp the declaration only */
    if (ext == X_DFB && pi && pi->out && !pi->regcfg && pi->mc >= 0 && D->mc[pi->mc].type == MC_GEN) {
        int fb = D->mc[pi->mc].fb;
        if (fb == GF_Q || fb == GF_NQ) return pi->inv ^ s->neg;
        if (fb == GF_XP) return 1 ^ s->neg;
    }
    if (ext == X_DFB) return !(s->neg || (pi && pi->out && !pi->regcfg));
    return s->neg;
}

static int line_inv(Sym *s, int line)
{
    const LineSrc *L = dev_lines(D, MODE);
    PinInfo *pi = pin_info(s);
    int neg = s->neg;
    if ((s->kind == SK_PIN || s->kind == SK_NODE) && alias_pin(s->pin) && dev_mc_at(D, alias_pin(s->pin)) >= 0 && D->mc[dev_mc_at(D, alias_pin(s->pin))].alias_inv) {
        /* the reference reads this register through its node alias with
           the opposite polarity to its reads through the pin (same fuse
           map otherwise); freecupl reads both the same way */
        if (!g_aliasread_warned) {
            const char *up = upname();
            diag_bug(DG_WARN, "0905cb", 0, BUG_ALIASREAD, g_fix ? "%s: pinnode %d (%s), the register of pin %d, read with the polarity of a read through the pin" : "%s: pinnode %d (%s), the register of pin %d, is read with the opposite polarity to a read through the pin",
                 up, s->pin, s->name, alias_pin(s->pin));
            g_aliasread_warned = 1;
        }
        /* (a node declared active low: the reproduction of 'nodeneg'
           already reads it as the reference does) */
        if (!g_fix && !(D->refdefect_nodeneg && s->neg)) neg ^= 1;
    }
    if (L[line].kind == SRC_FB && pi && pi->mc >= 0 && (pi->regcfg || alias_pin(pi->pin))) {
        const Macrocell *k = &D->mc[pi->mc];
        int out_inv = k->type == MC_REG ? !k->pol : k->type == MC_GAL ? 1 : pi->inv;
        if (k->type == MC_GEN && k->pol_d == 1) out_inv = 1;
        if (k->type == MC_GEN && k->pol_d == 2) out_inv = 0;
        if (O.kb && D->refdefect_kbfbinv && pi->regcfg && !pi->kbflip && pi->out && !alias_pin(pi->pin) && !k->node &&
            (k->type == MC_V10 || (k->type == MC_GEN && (k->fb == GF_Q || k->fb == GF_NQ) && k->pol_d == 0))) {
            /* -kb: the reference reads a registered output that -kb did
               not complement as if it had been complemented */
            if (!pi->kbfb_warned) {
                pi->kbfb_warned = 1;
                diag_bug(DG_WARN, "0905cb", 0, BUG_KB_FEEDBACK, g_fix ? "%s: -kb: register of pin %d (%s) read back with its polarity"
                                                               : "%s: -kb: the register of pin %d (%s) is read back with the opposite polarity",
                         upname(), pi->pin, pi->s->name);
            }
            if (!g_fix) out_inv ^= 1;
        }
        if (k->type == MC_GEN) {
            if (k->fb == GF_PIN) return neg;
            if (k->fb == GF_XQ) return 1 ^ neg;
            if (k->fb == GF_XP) return neg;
            if (k->fb == GF_Q) return out_inv ^ neg;
        }
        return 1 ^ out_inv ^ neg;
    }
    return neg;
}

/* BUG_GAL_COMBREG: the rows a .D sum and a combinational output (its
   OE term first, then its terms) would take agree */
static int gal_combreg_ok(PinInfo *pi)
{
    Cover d, c, o;
    int lv = level_of(pi->eq[X_D]), ok, j;
    cv_init(&d); cv_init(&c); cv_init(&o);
    eqn_cover_neg(pi->eq[X_D]->e, lv, pi->eq[X_D]->neg ^ pi->s->neg, &d);
    eqn_cover_neg(pi->eq[X_NONE]->e, lv, pi->eq[X_NONE]->neg ^ pi->s->neg, &c);
    if (pi->eq[X_OE]) eqn_cover_neg(pi->eq[X_OE]->e, lv, pi->eq[X_OE]->neg, &o);
    else { Cube u; cube_universe(&u); cv_add(&o, &u); }
    minimize(&d, lv, 64); minimize(&c, lv, 64); minimize(&o, lv, 64);
    ok = o.n == 1 && d.n >= 1 && cube_same(&o.c[0], &d.c[0]);
    for (j = 1; ok && j < d.n && j - 1 < c.n; j++) if (!cube_same(&d.c[j], &c.c[j - 1])) ok = 0;
    cv_free(&d); cv_free(&c); cv_free(&o);
    return ok;
}

static int g_bugones, g_shiftpin, g_rowshift;
static unsigned char *g_amoved;
static Cube g_spill[8];
static int g_spillrow[8], g_nspill;   /* BUG_ATV2500_ALIAS8 corrected on this macrocell (-fix) */

static void write_row(int row, const Cube *c)
{
    int base = dev_row_addr(D, row), k, v, pass;
    unsigned char *save = NULL;
    if (g_rowshift) {
        /* BUG_G24_PIN19: the row goes g_rowshift fuses further */
        save = (unsigned char *)xmalloc((size_t)D->ncols);
        memcpy(save, R->fuse + base, (size_t)D->ncols);
    }
    for (k = 0; k < D->ncols; k++) R->fuse[base + k] = 1;
    /* plain names first, then names with an extension: where two names
       of one signal meet, WinCUPL keeps the literal with the extension
       (BUG_TWO_NAMES) */
    for (pass = 0; pass < 2; pass++)
    for (v = 0; v < NV; v++) {
        int pol = cube_lit(c, v), line, need;
        if (pol < 0 || (VE[v] != X_NONE) != pass) continue;
        if (VE[v] == X_DFB || VE[v] == X_IO) {
            line = find_line_pref(VS[v]->pin, VE[v] == X_DFB);
            if (line < 0) continue;
            need = pol ^ line_inv_ext(VS[v], line, VE[v]);
        } else if (VE[v] == X_LQ || VE[v] == X_DQ || VE[v] == X_IOL) {
            line = find_line(VS[v]->pin);     /* the pin's line, through its latch */
            if (line < 0) continue;
            need = pol ^ line_inv(VS[v], line);
        } else {
            line = find_line(VS[v]->pin);
            if (line < 0) continue;
            need = pol ^ line_inv(VS[v], line);
        }
        {
            int col = 2 * line + ((need ? 0 : 1) ^ D->truecol_odd), other = col ^ 1;
            if (R->fuse[base + other] == 0 && !g_dupline_warned) {
                /* two different names of one signal (e.g. o and o.dfb) with
                   opposite polarity: the term is false; the reference keeps
                   only the last literal written */
                g_dupline_warned = 1;
                diag_bug(DG_WARN, "0906cb", 0, BUG_TWO_NAMES, g_fix ? "a product term reads %s%s%s with both polarities through another name of the same signal; the term is false"
                                                         : "a product term reads %s%s%s with both polarities through another name of the same signal; only the last literal is kept",
                     VS[v]->name, VE[v] ? "." : "", VE[v] ? ext_names[VE[v]] : "");
            }
            if (!g_fix) R->fuse[base + other] = 1;
            R->fuse[base + col] = 0;
        }
    }
    if (save) {
        unsigned char *row_fuses = (unsigned char *)xmalloc((size_t)D->ncols);
        memcpy(row_fuses, R->fuse + base, (size_t)D->ncols);
        memcpy(R->fuse + base, save, (size_t)D->ncols);
        for (k = 0; k < D->ncols; k++)
            if (base + g_rowshift + k < D->nfuses) R->fuse[base + g_rowshift + k] = row_fuses[k];
        free(row_fuses);
        free(save);
    }
}

static int put_rows(int r0, int cap, const Cover *f, const char *name)
{
    int i;
    /* constant-false placeholder terms (-m0) count unless the sum is empty */
    if (f->n + (f->n ? f->zeros : 0) > cap) {
        diag(DG_ERROR, "0006cc", 0, "excessive number of product terms:  %s", name);
        return -1;
    }
    for (i = 0; i < f->n; i++) write_row(r0 + i, &f->c[i]);
    return 0;
}

static void add_doc(Sym *s, int ext, int inv, const Cover *f, int max, int level)
{
    DocEq *d;
    R->doc = (DocEq *)xrealloc(R->doc, sizeof(DocEq) * (size_t)(R->ndoc + 1));
    d = &R->doc[R->ndoc++];
    memset(d, 0, sizeof *d);
    d->sym = s;
    d->ext = ext;
    d->inv = inv;
    cv_init(&d->f);
    cv_copy(&d->f, f);
    d->used = f->n;
    d->max = max;
    d->level = level;
}

static int control_cover(Eqn **qs, int nq, Cover *f, int *lvl)
{
    int i, level = O.minlevel;
    cv_clear(f);
    for (i = 0; i < nq; i++) {
        Cover t;
        int l = level_of(qs[i]);
        cv_init(&t);
        eqn_cover_neg(qs[i]->e, l, qs[i]->neg, &t);
        if (i == 0) level = l;
        cv_or(f, f, &t);
        cv_free(&t);
    }
    minimize(f, level, 1);
    *lvl = level;
    return 0;
}

/* per-macrocell control rows of a generic macrocell (clock, reset,
   preset); a bypassable register used combinationally gets AR and AP
   both true */
static void gen_controls(int mc, PinInfo *pi)
{
    const Macrocell *k = &D->mc[mc];
    static const int xs[3] = { X_CK, X_AR, X_AP };
    int rows[3], j;
    rows[0] = k->ck_row; rows[1] = k->ar_row; rows[2] = k->ap_row;
    if (k->reg_kind == GR_ARAP && !pi->regcfg) {
        Cube u;
        cube_universe(&u);
        if (k->ar_row >= 0) write_row(k->ar_row, &u);
        if (k->ap_row >= 0) write_row(k->ap_row, &u);
        rows[1] = rows[2] = -1;
    }
    for (j = 0; j < 3; j++) {
        Cover f;
        int lvl, x = xs[j];
        char nm[160];
        if (j == 0 && k->ce_fuse >= 0 && pi->eq[X_CE]) {
            /* .CE: the register is clocked by the clock pin and the CK row
               is its clock enable; with .CK as well both need the row */
            if (pi->eq[X_CK]) {
                name_ext(nm, sizeof nm, pi->s, X_CE);
                diag(DG_ERROR, "0006cc", 0, "excessive number of product terms:  %s", nm);
                continue;
            }
            x = X_CE;
            R->fuse[k->ce_fuse] = 1;
        }
        if (rows[j] < 0 || !pi->eq[x]) continue;
        cv_init(&f);
        control_cover(&pi->eq[x], 1, &f, &lvl);
        name_ext(nm, sizeof nm, pi->s, x);
        put_rows(rows[j], 1, &f, nm);
        add_doc(pi->s, x, 0, &f, 1, lvl);
        cv_free(&f);
    }
}

static char g_redirect[24];
static int g_probe_mode;
static int fit_one(const char *mnemonic, const FitOpts *o, FitResult *r);

/* 'via' aliases: a quiet first pass picks the mode, then the design is
   fitted to the description of the resolved mnemonic */
int fit_design(const char *mnemonic, const FitOpts *o, FitResult *r)
{
    const DevAlias *al = NULL;
    if (dev_find(mnemonic, &al) && al && al->via) {
        Diag save = g_diag;
        size_t n0 = g_diag.list.n;
        int rc;
        char target[24];
        g_diag.quiet = 1;
        g_probe_mode = 1;
        g_redirect[0] = 0;
        rc = fit_one(mnemonic, o, r);
        g_probe_mode = 0;
        save.list = g_diag.list;
        save.list.n = n0;
        if (save.list.p) save.list.p[n0] = 0;
        g_diag = save;
        if (!g_redirect[0]) return rc < 0 ? fit_one(mnemonic, o, r) : rc;
        strcpy(target, g_redirect);
        return fit_one(target, o, r);
    }
    return fit_one(mnemonic, o, r);
}

static int fit_one(const char *mnemonic, const FitOpts *o, FitResult *r)
{
    const DevAlias *al = NULL;
    int i, j, nerr0 = g_diag.nerr;
    int anyreg = 0, anyoe = 0, anyfb = 0, anyar = 0, anysp = 0;
    Eqn **arq = NULL, **spq = NULL, **apq = NULL;
    int nar = 0, nsp = 0, nap = 0;
    unsigned char *rowowner;
    Cover *mcsum;
    int *mcsumset;
    unsigned char *steerdone;   /* product-term steering groups: 1 written, 2 too many terms */
    int *steerrow;              /* row of each term of a steered sum (64 per macrocell) */

    memset(r, 0, sizeof *r);
    R = r;
    g_dupline_warned = 0;
    g_aliasread_warned = 0;
    g_bugones = 0;
    g_shiftpin = -1;
    g_rowshift = 0;
    g_lqbad = 0;
    O = *o;
    NV = 0;
    D = dev_find(mnemonic, &al);
    if (!D) {
        diag(DG_FATAL, "0006ca", 0, "device not in library:  %s", mnemonic);
        return -1;
    }
    r->dev = D;
    NP = D->npins;
    for (i = 0; i < D->nmc; i++) if (D->mc[i].pin > NP) NP = D->mc[i].pin;
    for (i = 0; i < D->nnodealias; i++) if (D->nodealias_node[i] > NP) NP = D->nodealias_node[i];
    if (g_des.nseq) {
        int regs = 0;
        for (i = 0; i < D->nmc; i++) if (D->mc[i].type != MC_COMB) regs = 1;
        if (!regs) {
            diag(DG_FATAL, "0036ca", 0, "device does not support synchronous state machines");
            return -1;
        }
    }
    PI = (PinInfo *)xcalloc((size_t)NP + 1, sizeof(PinInfo));
    for (i = 0; i <= NP; i++) { PI[i].mc = dev_mc_at(D, i); PI[i].pin = i; cv_init(&PI[i].sum); cv_init(&PI[i].xsum); }
    for (i = 0; i < D->nnodealias; i++) PI[D->nodealias_node[i]].mc = dev_mc_at(D, D->nodealias_pin[i]);
    g_undef_reported = (unsigned char *)xcalloc((size_t)g_des.nsyms + 1, 1);
    g_inprog = (unsigned char *)xcalloc((size_t)g_des.nsyms + 1, 1);
    g_cycle_reported = (unsigned char *)xcalloc((size_t)g_des.nsyms + 1, 1);
    g_badext_reported = (unsigned char *)xcalloc((size_t)g_des.nsyms + 1, 1);

    /* NODE declarations get pseudo-pin numbers after the package pins */
    {
        int next = NP + 1;
        for (i = 0; i < g_des.nsyms; i++)
            if (g_des.syms[i]->kind == SK_NODE && g_des.syms[i]->pin == 0) g_des.syms[i]->pin = next++;
    }
    for (i = 0; i < g_des.nsyms; i++) {
        Sym *s = g_des.syms[i];
        if (s->kind != SK_PIN && s->kind != SK_NODE) continue;
        if (s->pin < 1 || s->pin > NP) continue;
        if (s->kind == SK_NODE && PI[s->pin].mc < 0) continue;
        if (s->kind == SK_PIN && s->pin > D->npins && PI[s->pin].mc < 0) continue;   /* PIN on a node number: the node */
        if (PI[s->pin].s) {
            diag(DG_ERROR, "0009cb", 0, "pin/node %d redeclared:  %s", s->pin, s->name);
            continue;
        }
        PI[s->pin].s = s;
    }
    for (i = 0; i < g_des.neqns; i++) {
        Eqn *q = g_des.eqns[i];
        Sym *s = q->sym;
        PinInfo *pi;
        char nm[160];
        if (!q->e) {
            if (s->kind == SK_PIN && (pi = pin_info(s)) != NULL) pi->minext[q->ext] = 1;
            continue;
        }
        if (s->kind == SK_NODE && (pi = pin_info(s)) != NULL) {
            pi->eq[q->ext] = q;
            continue;
        }
        if (s->kind == SK_NODE) {
            name_ext(nm, sizeof nm, s, q->ext);
            if (!g_badext_reported[s->id]) {
                g_badext_reported[s->id] = 1;
                diag(DG_ERROR, "0012cb", 0, "pin/node %d invalid usage:  %s", s->pin, nm);
            }
            continue;
        }
        if (s->kind != SK_PIN) continue;
        pi = pin_info(s);
        if (!pi) {
            if (s->pin < 1 || s->pin > NP) {
                name_ext(nm, sizeof nm, s, q->ext);
                diag(DG_ERROR, "0012cb", 0, "pin/node %d invalid usage:  %s", s->pin, nm);
            }
            continue;
        }
        pi->eq[q->ext] = q;
    }
    /* all equations, intermediate ones included: undefined variables are
       reported and every pin referenced counts as an input */
    g_mark = 1;
    for (i = 0; i < g_des.neqns; i++) {
        Eqn *q = g_des.eqns[i];
        Cover t;
        /* (also an equation with an extension on a name that is not a pin,
           e.g. the state bits of a SEQUENCE on undeclared names: the
           reference reports its undefined variables) */
        if (!q->e || q->sym->kind != SK_NONE) continue;
        cv_init(&t);
        if (q->ext == X_NONE) g_inprog[q->sym->id] = 1;
        eqn_cover(q->e, 1, &t);
        if (q->ext == X_NONE) g_inprog[q->sym->id] = 0;
        cv_free(&t);
    }
    for (i = 1; i <= NP; i++) {
        PinInfo *pi = &PI[i];
        int x;
        if (!pi->s) continue;
        for (x = 0; x < X_COUNT; x++) {
            Cover t;
            if (!pi->eq[x]) continue;
            cv_init(&t);
            eqn_cover(pi->eq[x]->e, 1, &t);
            cv_free(&t);
        }
    }
    g_mark = 0;
    for (i = 1; i <= NP; i++) {
        PinInfo *pi = &PI[i];
        if (!pi->s) continue;
        if (pi->mc >= 0 && D->mc[pi->mc].latch && !pi->latchdone) {
            /* a latch: .L is its equation (no .D) */
            pi->latchdone = 1;
            if (pi->eq[X_D]) {
                char nm[160];
                name_ext(nm, sizeof nm, pi->s, X_D);
                diag(DG_ERROR, "0011cb", 0, "unknown extension:  %s", nm);
                diag(DG_ERROR, "0012cb", 0, "pin/node %d invalid usage:  %s", i, nm);
            }
            pi->eq[X_D] = pi->eq[X_L];
            pi->eq[X_L] = NULL;
        }
        pi->comb = pi->eq[X_NONE] != NULL;
        pi->reg = pi->eq[X_D] != NULL || pi->eq[X_T] != NULL;
        pi->tff = pi->eq[X_T] != NULL;
        if (pi->eq[X_T] && pi->eq[X_D] && pi->mc >= 0 && D->nodedt_reject && (D->mc[pi->mc].node || alias_pin(i))) {
            /* as the reference (ATV750 family): not on a PINNODE */
            char nm[160];
            name_ext(nm, sizeof nm, pi->s, X_T);
            diag(DG_ERROR, "0025cb", 0, "pin/node %d output mutually excluded by previous output:  %s", i, nm);
        } else if (pi->eq[X_T] && pi->eq[X_D] && pi->mc >= 0 && D->mc[pi->mc].t_fuse >= 0) {
            /* as the reference: .D and .T on one register give a T flip-flop
               whose sum is the OR of both equations */
            Eqn *m = (Eqn *)xcalloc(1, sizeof *m);
            Eqn *qd = pi->eq[X_D], *qt = pi->eq[X_T];
            *m = *qt;
            m->e = mk_bin(E_OR, qd->neg ? mk_not(qd->e) : qd->e, qt->neg ? mk_not(qt->e) : qt->e);
            m->neg = 0;
            memset(m->grp, 0, sizeof m->grp);
            pi->eq[X_T] = m;
            pi->eq[X_D] = NULL;
        }
        pi->out = pi->comb || pi->reg;
        if (pi->reg) anyreg = 1;
        if (pi->eq[X_OE]) anyoe = 1;
        if (pi->out && pi->input) anyfb = 1;
    }

    MODE = 0;
    if (D->nmodes > 1) {
        const char *want = NULL, *res = NULL;
        if (al && (!strcmp(al->mode, "simple") || !strcmp(al->mode, "complex") || !strcmp(al->mode, "registered"))) {
            want = al->mode;
            res = al->name;
        } else {
            want = anyreg ? "registered" : (anyoe || (anyfb && !(al && al->autofb_simple))) ? "complex" : "simple";
            if (al) res = !strcmp(want, "simple") ? al->simple : !strcmp(want, "complex") ? al->complex_ : al->registered;
        }
        for (i = 0; i < D->nmodes; i++) if (!strcmp(D->modes[i].name, want)) MODE = i;
        snprintf(r->resolved, sizeof r->resolved, "%s", res && res[0] ? res : D->name);
    } else snprintf(r->resolved, sizeof r->resolved, "%s", al ? al->name : D->name);
    MODENAME = D->modes[MODE].name;
    r->mode = MODE;
    if (g_probe_mode && al && al->via && g_lqbad) return -1;   /* reported by the full pass */
    if (g_probe_mode && al && al->via) {
        const char *t = !strcmp(MODENAME, "simple") || D->nmodes <= 1 ? NULL : NULL;
        int areg = 0, aoe = 0, afb = 0;
        (void)t;
        for (i = 1; i <= NP; i++) {
            if (!PI[i].s) continue;
            if (PI[i].eq[X_D]) areg = 1;
            if (PI[i].eq[X_OE]) aoe = 1;
            if ((PI[i].eq[X_NONE] || PI[i].eq[X_D]) && PI[i].input) afb = 1;
        }
        t = areg ? al->registered : (aoe || (afb && !al->autofb_simple)) ? al->complex_ : al->simple;
        snprintf(g_redirect, sizeof g_redirect, "%s", t);
        return -2;
    }

    {
        int res[12], nres = 0;
        const DevAlias *ra = NULL;
        if (al && al->nreserve >= 0) {
            for (i = 0; i < al->nreserve; i++) res[nres++] = al->reserve[i];
        } else if (D->pd_pin > 0) res[nres++] = D->pd_pin;
        /* an automatic mnemonic: the pins its resolved mnemonic reserves */
        if (al && strcmp(r->resolved, al->name) && dev_find(r->resolved, &ra) == D && ra && ra->nreserve > 0)
            for (i = 0; i < ra->nreserve && nres < 12; i++) res[nres++] = ra->reserve[i];
        for (i = 0; i < nres; i++) {
            PinInfo *pi = res[i] >= 1 && res[i] <= NP ? &PI[res[i]] : NULL;
            if (!pi || !pi->s) continue;
            {
                diag(DG_ERROR, "0012cb", 0, "pin/node %d invalid usage:  %s", res[i], pi->s->name);
                pi->s = NULL;
            }
        }
        if (g_diag.nerr > nerr0) return -1;
    }

    for (i = 1; i <= NP; i++) {
        PinInfo *pi = &PI[i];
        const Macrocell *k;
        char nm[160];
        int x, bad = 0;
        if (!pi->s) continue;
        for (x = 1; x < X_COUNT; x++) {
            if ((!pi->eq[x] && !pi->minext[x]) || ext_known(x)) continue;
            name_ext(nm, sizeof nm, pi->s, x);
            diag(DG_ERROR, "0011cb", 0, "unknown extension:  %s", nm);
            diag(DG_ERROR, "0012cb", 0, "pin/node %d invalid usage:  %s", i, nm);
            bad = 1;
        }
        if (bad) continue;
        if (pi->eq[X_TEC] && tec_of(i) < 0 && !(D->tecok && pi->mc >= 0)) {
            name_ext(nm, sizeof nm, pi->s, X_TEC);
            diag(DG_ERROR, "0012cb", 0, "pin/node %d invalid usage:  %s", i, nm);
        }
        if (pi->mc < 0) {
            if (pi->out || pi->eq[X_OE]) {
                diag(DG_ERROR, "0010cb", 0, "pin/node %d invalid output:  %s", i, pi->s->name);
            }
            continue;
        }
        k = &D->mc[pi->mc];
        if (pi->eq[X_T] && k->t_fuse < 0) {
            name_ext(nm, sizeof nm, pi->s, X_T);
            diag(DG_ERROR, "0012cb", 0, "pin/node %d invalid usage:  %s", i, nm);
        }
        switch (k->type) {
        case MC_COMB:
            if (pi->reg) { name_ext(nm, sizeof nm, pi->s, X_D); diag(DG_ERROR, "0012cb", 0, "pin/node %d invalid usage:  %s", i, nm); }
            if (pi->eq[X_OE] && k->oe_row < 0) { name_ext(nm, sizeof nm, pi->s, X_OE); diag(DG_ERROR, "0012cb", 0, "pin/node %d invalid usage:  %s", i, nm); }
            if (pi->eq[X_AR] || pi->eq[X_SP]) { name_ext(nm, sizeof nm, pi->s, pi->eq[X_AR] ? X_AR : X_SP); diag(DG_ERROR, "0012cb", 0, "pin/node %d invalid usage:  %s", i, nm); }
            break;
        case MC_REG:
            if (pi->comb) diag(DG_ERROR, "0013cb", 0, "pin/node %d invalid output extension or usage:  %s", i, pi->s->name);
            if (pi->eq[X_OE]) { name_ext(nm, sizeof nm, pi->s, X_OE); diag(DG_ERROR, "0012cb", 0, "pin/node %d invalid usage:  %s", i, nm); }
            pi->regcfg = 1;
            break;
        case MC_GAL:
            if (pi->reg && pi->comb && !g_fix && !pi->tff && pi->eq[X_D] && pi->eq[X_NONE] && !strcmp(MODENAME, "registered") && gal_combreg_ok(pi)) {
                /* BUG_GAL_COMBREG: the reference writes both into the rows
                   of the macrocell; where the rows agree it accepts, and the
                   register gets all of them */
                Eqn *c = pi->eq[X_NONE], *dq = pi->eq[X_D], *m = (Eqn *)xcalloc(1, sizeof(Eqn));
                *m = *dq;
                m->e = mk_bin(E_OR, dq->neg ? mk_not(dq->e) : dq->e, c->neg ? mk_not(c->e) : c->e);
                m->neg = 0;
                memset(m->grp, 0, sizeof m->grp);
                m->grp[0] = m->e;
                pi->eq[X_D] = m;
                pi->eq[X_NONE] = NULL;
                pi->comb = 0;
                diag_bug(DG_WARN, "0905cb", 0, BUG_GAL_COMBREG, "pin %d (%s) has a combinational and a .D equation: both are written into the register's sum", i, pi->s->name);
            }
            if (pi->reg && pi->comb) {
                name_ext(nm, sizeof nm, pi->s, X_D);
                diag(DG_ERROR, "0013cc", 0, "conflicting usage of pinnode:  %s", nm);
                if (g_fix && pi->eq[X_D] && pi->eq[X_NONE] && !strcmp(MODENAME, "registered") && gal_combreg_ok(pi))
                    diag_bug(DG_NOTE, "0013cc", 0, BUG_GAL_COMBREG, "pin %d (%s): rejected", i, pi->s->name);
            }
            pi->regcfg = pi->reg && !strcmp(MODENAME, "registered");
            break;
        case MC_V10:
            if (pi->reg && pi->comb) { name_ext(nm, sizeof nm, pi->s, X_D); diag(DG_ERROR, "0025cb", 0, "pin/node %d output mutually excluded by previous output:  %s", i, nm); }
            pi->regcfg = pi->reg || (!pi->comb && !pi->input);
            break;
        case MC_GEN:
            if ((k->node || alias_pin(i)) && pi->comb) {
                if (k->node && D->refdefect_nodecomb) {
                    /* the reference accepts this and writes a never-clocked
                       register (and cuts the sum of the macrocell's pin) */
                    const char *up = upname();
                    if (g_fix)
                        diag(DG_ERROR, "0907cb", 0, "%s: pin/node %d is a register; a combinational equation is not possible:  %s", up, i, pi->s->name);
                    else {
                        Eqn *c = pi->eq[X_NONE];
                        int x2 = pi->tff ? X_T : X_D;
                        if (pi->eq[x2]) {
                            Eqn *dq = pi->eq[x2], *m = (Eqn *)xcalloc(1, sizeof(Eqn));
                            *m = *dq;
                            m->e = mk_bin(E_OR, dq->neg ? mk_not(dq->e) : dq->e, c->neg ? mk_not(c->e) : c->e);
                            m->neg = 0;
                            memset(m->grp, 0, sizeof m->grp);
                            m->grp[0] = m->e;
                            pi->eq[x2] = m;
                            pi->nodecomb = 2;
                        } else pi->eq[x2] = c;
                        pi->eq[X_NONE] = NULL;
                        pi->comb = 0;
                        pi->reg = 1;
                        if (!pi->nodecomb) pi->nodecomb = 1;
                    }
                    diag_bug(g_fix ? DG_NOTE : DG_WARN, "0905cb", 0, BUG_NODECOMB, g_fix ? "%s: pinnode %d (%s): rejected"
                             : "%s: pinnode %d (%s), a register, has a combinational equation: it is written as a register that is never clocked, and the pin of its macrocell keeps only its first sum term group",
                             up, i, pi->s->name);
                    if (g_fix) break;
                } else {
                    diag(DG_ERROR, "0010cb", 0, "pin/node %d invalid output:  %s", i, pi->s->name);
                    break;
                }
            }
            if (alias_pin(i) && pi->out && PI[alias_pin(i)].s && PI[alias_pin(i)].out) {
                Eqn *re = pi->eq[pi->tff ? X_T : X_D];
                PinInfo *pp = &PI[alias_pin(i)];
                if (!qsplit_alias(i) && re && re->neg && !pp->reg && strcmp(pp->s->name, pi->s->name) < 0) {
                    /* the original checks the pin first and misses this case:
                       it ORs the register equation into the pin's sum */
                    const char *up = upname();
                    if (g_fix)
                        diag(DG_ERROR, "0907cb", 0, "%s: pin %d (%s) is combinational and uses the macrocell's only sum, which is also the D input of its register (pinnode %d, %s): the register cannot have equations of its own:  %s",
                             up, alias_pin(i), pp->s->name, i, pi->s->name, pi->s->name);
                    else if (pp->eq[X_NONE]) {
                        Eqn *c = pp->eq[X_NONE], *m = (Eqn *)xcalloc(1, sizeof(Eqn));
                        *m = *c;
                        m->e = mk_bin(E_OR, c->neg ? mk_not(c->e) : c->e, re->neg ? mk_not(re->e) : re->e);
                        m->neg = 0;
                        memset(m->grp, 0, sizeof m->grp);
                        m->grp[0] = m->e;
                        pp->eq[X_NONE] = m;
                        pi->eq[pi->tff ? X_T : X_D] = NULL;
                        pi->reg = 0;
                        pi->out = 0;
                    }
                    diag_bug(g_fix ? DG_NOTE : DG_WARN, "0905cb", 0, BUG_ALIASCOMB, g_fix ? "%s: pinnode %d (%s) with pin %d (%s): rejected"
                                                                   : "%s: the equation of pinnode %d (%s), the register of combinational pin %d (%s), is ORed into the pin's equation",
                             up, i, pi->s->name, alias_pin(i), pp->s->name);
                } else if (!qsplit_alias(i))
                    diag(DG_ERROR, "0025cb", 0, "pin/node %d output mutually excluded by previous output:  %s", alias_pin(i), PI[alias_pin(i)].s->name);
                else if (PI[alias_pin(i)].reg) {
                    /* the register is the pin's: it cannot have a sum of its own */
                    name_ext(nm, sizeof nm, pi->s, pi->tff ? X_T : X_D);
                    diag(DG_ERROR, "0013cc", 0, "conflicting usage of pinnode:  %s", nm);
                }
            }
            for (x = X_CK; x < X_COUNT; x++) {
                int ok = (x == X_CK && k->ck_row >= 0) || (x == X_AR && (k->ar_row >= 0 || D->ar_row >= 0)) ||
                         (x == X_AP && (k->ap_row >= 0 || D->ap_row >= 0)) || (x == X_SP && (k->sp_row >= 0 || D->sp_row >= 0));
                if (x == X_CK || x == X_AR || x == X_AP || x == X_SP) {
                    if (pi->eq[x] && !ok) { name_ext(nm, sizeof nm, pi->s, x); diag(DG_ERROR, "0012cb", 0, "pin/node %d invalid usage:  %s", i, nm); }
                }
            }
            if (pi->eq[X_OE] && mc_oe_row(pi->mc, 0) < 0) { name_ext(nm, sizeof nm, pi->s, X_OE); diag(DG_ERROR, "0012cb", 0, "pin/node %d invalid usage:  %s", i, nm); }
            if (k->reg_kind == GR_ARAP && !pi->reg && (pi->eq[X_AR] || pi->eq[X_AP])) {
                name_ext(nm, sizeof nm, pi->s, pi->eq[X_AR] ? X_AR : X_AP);
                diag(DG_ERROR, "0021cb", 0, "no expression allowed for:  %s", nm);
            }
            if (k->reg_kind == GR_COMB) {
                if (pi->reg) { name_ext(nm, sizeof nm, pi->s, X_D); diag(DG_ERROR, "0012cb", 0, "pin/node %d invalid usage:  %s", i, nm); }
            } else if (k->reg_kind == GR_REG) {
                if (pi->comb) diag(DG_ERROR, "0013cb", 0, "pin/node %d invalid output extension or usage:  %s", i, pi->s->name);
                pi->regcfg = 1;
            } else {
                if (pi->reg && pi->comb && D->refdefect_combd) {
                    const char *up = upname();
                    name_ext(nm, sizeof nm, pi->s, X_D);
                    if (g_fix)
                        diag(DG_ERROR, "0907cb", 0, "%s: pin %d has a combinational and a registered equation, but the pin shows either its register or its sum, not both:  %s", up, i, nm);
                    else if (!pi->tff) {
                        /* the reference ORs both into one combinational sum */
                        Eqn *c = pi->eq[X_NONE], *dq = pi->eq[X_D], *m = (Eqn *)xcalloc(1, sizeof(Eqn));
                        *m = *c;
                        m->e = mk_bin(E_OR, c->neg ? mk_not(c->e) : c->e, dq->neg ? mk_not(dq->e) : dq->e);
                        m->neg = 0;
                        memset(m->grp, 0, sizeof m->grp);
                        m->grp[0] = m->e;
                        pi->eq[X_NONE] = m;
                        pi->eq[X_D] = NULL;
                        pi->reg = 0;
                        diag_bug(DG_WARN, "0905cb", 0, BUG_MEGA_COMBD, "%s: pin %d (%s) has a combinational and a registered equation; both are ORed into one combinational output", up, i, pi->s->name);
                    }
                    if (g_fix) diag_bug(DG_NOTE, "0907cb", 0, BUG_MEGA_COMBD, "%s: pin %d (%s): rejected", up, i, pi->s->name);
                } else if (pi->reg && pi->comb) { name_ext(nm, sizeof nm, pi->s, X_D); diag(DG_ERROR, "0025cb", 0, "pin/node %d output mutually excluded by previous output:  %s", i, nm); }
                pi->regcfg = pi->reg;
            }
            break;
        }
    }
    /* inputs must be readable in this mode */
    for (i = 1; i <= NP; i++) {
        PinInfo *pi = &PI[i];
        if (!pi->s || !pi->input) continue;
        if (pi->mc >= 0 && (D->mc[pi->mc].type == MC_REG || (D->mc[pi->mc].type == MC_GEN && D->mc[pi->mc].reg_kind == GR_REG && D->mc[pi->mc].fb != GF_PIN && !D->mc[pi->mc].node)) && !pi->reg) {
            /* a fixed register output only feeds back its register */
            diag(DG_ERROR, "0008cc", 0, "pin can not be used as input:  %s", pi->s->name);
            continue;
        }
        if (find_line(i) < 0 || (pi->mc >= 0 && D->mc[pi->mc].fb_reg && pi->out && !pi->regcfg)) {
            if (i == D->gnd || i == D->vcc) diag(DG_ERROR, "0014cb", 0, "invalid input:  %s", pi->s->name);
            else diag(DG_ERROR, "0014cb", 0, "pin/node %d invalid input:  %s", i, pi->s->name);
        }
        if (pi->out && pi->mc >= 0 && D->mc[pi->mc].type == MC_GAL && !strcmp(MODENAME, "simple") && al && al->fbquirk) {
            /* non-A simple mode has no output feedback: the pin becomes a
               dedicated input, its terms are still written (as the original) */
            char up[48], alt[48], base[48];
            const char *sp;
            size_t k2;
            pi->quirk = 1;
            snprintf(up, sizeof up, "%s", r->resolved);
            for (k2 = 0; up[k2]; k2++) if (up[k2] >= 'a' && up[k2] <= 'z') up[k2] = (char)(up[k2] - 'a' + 'A');
            sp = strrchr(r->resolved, 's');
            alt[0] = base[0] = 0;
            if (sp && (size_t)(sp - r->resolved) < sizeof base - 1) {
                snprintf(base, sizeof base, "%.*s%s", (int)(sp - r->resolved), r->resolved, sp + 1);
                snprintf(alt, sizeof alt, "%.*sa%s", (int)(sp - r->resolved), r->resolved, sp);
                if (!dev_find(alt, NULL)) alt[0] = 0;
                if (!dev_find(base, NULL)) base[0] = 0;
            }
            diag(DG_WARN, "0901cb", 0, "%s: output with feedback not available in simple mode; pin %d (%s) configured as input, its equation is unused%s%s%s%s",
                 up, i, pi->s->name, alt[0] || base[0] ? " - use " : "", alt, alt[0] && base[0] ? " or " : "", base);
        }
    }
    if (g_diag.nerr > nerr0) return -1;

    /* sums shared with a buried register: split when the register is used */
    g_split = (unsigned char *)xcalloc((size_t)D->nmc + 1, 1);
    g_qsplit = (unsigned char *)xcalloc((size_t)D->nmc + 1, 1);
    g_amoved = (unsigned char *)xcalloc((size_t)D->nmc + 1, 1);
    for (i = 0; i < D->nmc; i++) {
        const Macrocell *k = &D->mc[i];
        int an;
        if (k->type != MC_GEN || k->qsplit_fuse < 0 || !(an = alias_node(k->pin))) continue;
        if (an <= NP && PI[an].s && PI[an].out) g_qsplit[i] = 1;
        if (g_qsplit[i] && !(PI[k->pin].s && (PI[k->pin].out || PI[k->pin].eq[X_OE])) && PI[an].reg && !PI[an].tff && PI[an].eq[X_D]) {
            /* BUG_ATV2500_ALIAS8: the register named by its alias gets its own
               4 rows only; -fix lends it the pin's rows when the pin is not
               an output */
            Cover t;
            int n4 = k->qsum1 - k->qsum0 + 1;
            cv_init(&t);
            eqn_cover_neg(PI[an].eq[X_D]->e, level_of(PI[an].eq[X_D]), PI[an].eq[X_D]->neg, &t);
            minimize(&t, level_of(PI[an].eq[X_D]), 64);
            if (t.n + (t.n ? t.zeros : 0) > n4) {
                diag_bug(g_fix ? DG_WARN : DG_NOTE, "0905cb", 0, BUG_ATV2500_ALIAS8, g_fix ? "%s: the register of pin %d (pinnode %d, %s) takes the rows of the pin, which is not used as an output"
                                                             : "%s: the register of pin %d (pinnode %d, %s) gets only its own %d rows although the pin is not used as an output",
                         upname(), k->pin, an, PI[an].s->name, n4);
                if (g_fix) { g_qsplit[i] = 0; g_amoved[i] = 1; }
            }
            cv_free(&t);
        }
    }
    for (i = 0; i < D->nmc; i++) {
        const Macrocell *k = &D->mc[i];
        if (k->type != MC_GEN || k->split_fuse < 0) continue;
        if (k->split_out && (g_qsplit[i] || !(PI[k->pin].s && PI[k->pin].out))) { g_split[i] = 1; continue; }
        for (j = 0; j < D->nmc; j++) {
            const Macrocell *b = &D->mc[j];
            if (!b->node || b->sum0 < k->sum0 || b->sum1 > k->sum1) continue;
            if (b->pin <= NP && PI[b->pin].s && PI[b->pin].out) g_split[i] = 1;
        }
    }
    for (i = 1; i <= NP; i++) {
        if (!PI[i].s) continue;
        if (PI[i].eq[X_AR] && D->ar_row >= 0) anyar = 1;
        if (PI[i].eq[X_SP] && D->sp_row >= 0) anysp = 1;
        if (PI[i].eq[X_AP] && D->ap_row >= 0) anysp = 1;
    }
    for (i = 1; i <= NP; i++) {
        PinInfo *pi = &PI[i];
        Eqn *q;
        const Macrocell *k;
        const Expr *core;
        int flip, want, level, cap, pp, kpol, qa, cflip, gq, rootnot;
        Cover f;
        if (!pi->s || !pi->out || pi->mc < 0) continue;
        q = pi->reg ? pi->eq[pi->tff ? X_T : X_D] : pi->eq[X_NONE];
        k = &D->mc[pi->mc];
        core = root_strip(q, &flip);
        rootnot = core != resolve_top(q->e);   /* a complement at the root was stripped */
        level = level_of(q);
        /* a register with its own sum (qsplit) has no polarity of its own:
           the polarity fuse belongs to the pin */
        qa = qsplit_alias(i) && g_qsplit[pi->mc];
        cap = qa ? k->qsum1 - k->qsum0 + 1 : mc_capacity(pi->mc, pi->regcfg);
        want = flip ^ pi->s->neg;
        cflip = gq = 0;
        if (D->refdefect_nodeneg && pi->s->neg && (k->node || alias_pin(i)) && !g_fix) {
            /* reproduced: the register is written as for a plain node (only
               the left side's '!' complements it); the node is read
               complemented */
            if (!k->node && !qa && k->fb == GF_Q) { gq = 1; cflip = flip; }
            else qa = 1;
        }
        /* an output-register alias has no polarity of its own either (the
           pin's polarity fuse is not written for it): a complement goes
           into the sum, as the reference does */
        pp = prog_polarity(pi->mc) && !qa && !(alias_pin(i) && !k->node && !gq);
        kpol = qa ? 1 : k->pol;
        if (D->refdefect_nodeneg && pi->s->neg && (k->node || alias_pin(i))) {
            /* CUPLREF: '!' in a PINNODE declaration gives the node's
               polarity (the equation is complemented); the reference reads
               the node complemented but leaves the equation as written */
            const char *up = upname();
            diag_bug(DG_WARN, "0905cb", 0, BUG_NODENEG, g_fix ? "%s: equation of pinnode %d (%s), declared active low, complemented as declared"
                                                         : "%s: the equation of pinnode %d (%s), declared active low, is not complemented, but the node is read complemented",
                 up, i, pi->s->name);
            if (!g_fix && !gq) want ^= 1;
        }
        cv_init(&f);
        if (level <= 1 && ngroups(q) > 1 && (pp ? !O.kb : (!kpol) == want)) {
            /* several groups: the root is an OR (want = left '!' ^ pin '!');
               the sum is the expression itself */
            pi->inv = want;
            group_cover(q, level, &f);
        } else if (k->type == MC_GEN && k->xsum0 >= 0) {
            /* XOR macrocell: a top-level '$' is split over the two sums, the
               complement (fixed polarity) goes on the first operand */
            int inv = pp ? want : !kpol;
            pi->inv = inv;
            cv_init(&pi->xsum);
            if (core->op == E_XOR) {
                eqn_cover_pol(core->a, level, inv != want, pi->s, &f);
                minimize(&f, level, cap);
                eqn_cover(core->b, level, &pi->xsum);
                minimize(&pi->xsum, level, k->xsum1 - k->xsum0 + 1);
            } else {
                eqn_cover_pol(core, level, inv != want, pi->s, &f);
                minimize(&f, level, cap);
            }
            if (k->xone && f.n + (f.n ? f.zeros : 0) <= k->xsum1 - k->xsum0 + 1) {
                /* the first operand (or the whole sum) in the second sum
                   when it fits, as the reference */
                Cover t = f;
                f = pi->xsum;
                pi->xsum = t;
            } else if (k->xswap && (core->op == E_XOR ? f.n + (f.n ? f.zeros : 0) <= pi->xsum.n + (pi->xsum.n ? pi->xsum.zeros : 0)
                                               : f.n + (f.n ? f.zeros : 0) <= k->xsum1 - k->xsum0 + 1)) {
                /* the smaller operand (or the whole sum, when it fits) in the
                   second sum, as the reference */
                Cover t = f;
                f = pi->xsum;
                pi->xsum = t;
            }
            if (level >= 2 && (f.n > cap || pi->xsum.n > k->xsum1 - k->xsum0 + 1)) {
                /* with full minimisation the reference finds another split
                   when the written one does not fit: try F = A $ v for every
                   single variable v */
                Cover F, nF, L, nL, a1, a2;
                int v;
                cv_init(&F); cv_init(&nF); cv_init(&L); cv_init(&nL); cv_init(&a1); cv_init(&a2);
                eqn_cover_neg(core, level, inv != want, &F);
                cv_not(&nF, &F);
                for (v = 0; v < NV; v++) {
                    lit_cover(&L, v, 1);
                    lit_cover(&nL, v, 0);
                    cv_and(&a1, &F, &nL);
                    cv_and(&a2, &nF, &L);
                    cv_or(&a1, &a1, &a2);
                    minimize(&a1, level, cap);
                    if (a1.n <= cap) {
                        cv_free(&f);
                        cv_init(&f);
                        cv_copy(&f, &a1);
                        cv_free(&pi->xsum);
                        cv_init(&pi->xsum);
                        cv_copy(&pi->xsum, &L);
                        break;
                    }
                }
                cv_free(&F); cv_free(&nF); cv_free(&L); cv_free(&nL); cv_free(&a1); cv_free(&a2);
            }
        } else if (pp) {
            eqn_cover_neg(core, level, cflip, &f);
            pi->inv = gq ? pi->s->neg : want;
            /* -kb never flips a register whose reset/preset is in use: that
               would change what AR/SP do to the pin */
            if (O.kb && !(pi->regcfg && (anyar || anysp || pi->eq[X_AR] || pi->eq[X_AP])) && !pi->tff) {
                Cover a, b;
                cv_init(&a); cv_init(&b);
                cv_copy(&a, &f);
                minimize(&a, level < 1 ? 1 : level, cap);
                eqn_cover_neg(core, level, 1, &b);
                minimize(&b, level < 1 ? 1 : level, cap);
                int nf = (int)((D->refdefect_kbnoflip | (pi->regcfg ? D->refdefect_kbnoflip_reg : 0u)) >> MODE & 1u);
                int an = a.n, bn = b.n;
                if (level == 0) {
                    /* -m0: the counts of the unminimised sums */
                    Cover b0;
                    cv_init(&b0);
                    eqn_cover_neg(core, level, 1, &b0);
                    int t;
                    an = f.n + (f.n ? f.zeros : 0);
                    bn = b0.n + (b0.n ? b0.zeros : 0);
                    /* a sum with a constant-true term counts as that term */
                    for (t = 0; t < f.n; t++) if (cube_nlits(&f.c[t]) == 0) an = 1;
                    for (t = 0; t < b0.n; t++) if (cube_nlits(&b0.c[t]) == 0) bn = 1;
                    cv_free(&b0);
                }
                /* an empty sum is written as one constant-false term */
                if (!an) an = 1;
                if (!bn) bn = 1;
                /* WinCUPL: an equation complemented at the root keeps the
                   polarity that rule gives it */
                if (rootnot) bn = an;
                if (bn < an) {
                    cv_free(&f); f = b; pi->inv = !want; cv_free(&a);
                    pi->kbflip = 1;
                    if (nf) {
                        /* the reference writes the complemented sum but
                           leaves the polarity: the output is complemented */
                        diag_bug(DG_WARN, "0905cb", 0, BUG_KB_POLARITY, g_fix ? "%s: -kb: sum of %s complemented together with its output polarity"
                                                                       : "%s: -kb: the sum of %s is complemented but the output polarity is not: the output is the complement of its equation",
                                 upname(), pi->s->name);
                        if (!g_fix) pi->inv = want;
                    }
                }
                else { cv_free(&b); cv_free(&f); f = a; }
            } else minimize(&f, level, cap);
        } else {
            pi->inv = !kpol;
            eqn_cover_pol(core, level, pi->inv != want, pi->s, &f);
            minimize(&f, level, cap);
        }
        pi->sum = f;
        pi->sumok = 1;
        if (!g_fix && k->combfull && !strcmp(MODENAME, "registered") && !pi->regcfg && pi->out &&
            f.n + (f.n ? f.zeros : 0) == cap + 1) {
            /* BUG_COMBFULL: the terms are written from the row after the OE
               term, so the last one lands in the next row */
            pi->combspill = 1;
            g_bugones = 1;
        }
        if (D->refdefect_pin == i && !strcmp(D->refdefect_mode, MODENAME) && (!pi->regcfg || f.n < cap)) {
            /* the reference compiler's fuse map is wrong for this output in
               this mode (see the device description); freecupl writes the
               equations as given */
            const char *up = upname();
            diag_bug(DG_WARN, "0905cb", 0, BUG_G24_PIN19, g_fix ? "%s: pin %d (%s) in %s mode written as given"
                     : (pi->regcfg ? "%s: pin %d (%s) in %s mode: an unused row of its sum is constant true" : "%s: pin %d (%s) in %s mode: its product terms are written into the wrong rows and an unused row is constant true"),
                     up, i, pi->s->name, MODENAME);
            if (!g_fix) {
                g_bugones = 1;
                if (!pi->regcfg) g_shiftpin = i;
            }
        }
    }

    r->fuse = (unsigned char *)xcalloc((size_t)D->nfuses + 1, 1);
    if (D->rowfill)
        for (i = 0; i < D->nrows; i++) memset(r->fuse + dev_row_addr(D, i), 1, (size_t)D->ncols);
    if (D->ptd_base >= 0) for (i = 0; i < D->nrows; i++) r->fuse[D->ptd_base + i] = 1;
    if (g_bugones)   /* BUG_G24_PIN19: written before the product terms */
        for (i = 0; i < D->refdefect_nones; i++) if (D->refdefect_ones + i < D->nfuses) r->fuse[D->refdefect_ones + i] = 1;
    for (i = 0; i < D->nconst; i++) if (D->const_fuse[i] < D->nfuses) r->fuse[D->const_fuse[i]] = 1;
    for (i = 0; i < D->modes[MODE].ncond; i++) r->fuse[D->modes[MODE].cond_fuse[i]] = (unsigned char)D->modes[MODE].cond_val[i];
    rowowner = (unsigned char *)xcalloc((size_t)D->nrows + 1, 1);
    mcsum = (Cover *)xcalloc((size_t)D->nmc + 1, sizeof(Cover));
    mcsumset = (int *)xcalloc((size_t)D->nmc + 1, sizeof(int));
    steerdone = (unsigned char *)xcalloc((size_t)D->nmc + 1, 1);
    steerrow = (int *)xcalloc((size_t)(D->nmc + 1) * 64, sizeof(int));

    for (i = 0; i < D->nmc; i++) {
        const Macrocell *k = &D->mc[i];
        PinInfo *pi = &PI[k->pin];
        int used, oe_row, r0, cap, shared = -1;
        char nm[160];
        if (!(pi->s && pi->out) && !(k->type == MC_GEN && k->qsplit_fuse >= 0 && g_qsplit[i])) {
            for (j = 0; j < D->nnodealias; j++)
                if (D->nodealias_pin[j] == k->pin && PI[D->nodealias_node[j]].s && (PI[D->nodealias_node[j]].out || has_ctl(&PI[D->nodealias_node[j]])))
                    pi = &PI[D->nodealias_node[j]];
        }
        used = pi->s && (pi->out || pi->eq[X_OE]);
        if (!pi->s) pi = NULL;
        if (k->type == MC_GEN && k->split_fuse >= 0) {
            int sp = g_split[i];
            /* written unsplit only when the sum needs the rows */
            if (k->split_need && pi && pi->s && pi->out && pi->sumok && pi->sum.n + (pi->sum.n ? pi->sum.zeros : 0) <= k->half1 - k->half0 + 1) sp = 1;
            if (!sp && D->refdefect_nojoin && pi && pi->s && pi->out) {
                /* the sum needs the third group: the reference never joins it */
                const char *up = upname();
                diag_bug(DG_WARN, "0905cb", 0, BUG_NOJOIN, g_fix ? "%s: third sum term of pin %d (%s) joined with the others"
                                                         : "%s: the third sum term of pin %d (%s) is not joined with the others; its product terms beyond the 8th do not reach the output",
                     up, k->pin, pi->s->name);
                if (!g_fix) sp = 1;
            }
            r->fuse[k->split_fuse] = (unsigned char)(sp ? k->split_val : !k->split_val);
        }
        if (k->type == MC_GEN && k->qsplit_fuse >= 0) r->fuse[k->qsplit_fuse] = g_qsplit[i];
        if (k->type == MC_GAL) {
            int ac1;
            if (!strcmp(MODENAME, "simple")) ac1 = !(pi && pi->out) || (pi && pi->quirk);
            else if (!strcmp(MODENAME, "complex")) ac1 = 1;
            else ac1 = pi && pi->regcfg ? 0 : (pi && (pi->out || pi->eq[X_OE])) ? 1 : (pi && pi->input && D->galreg_unused_ac1) ? 1 : D->galreg_unused_ac1;
            r->fuse[k->ac1_fuse] = (unsigned char)(ac1 ? k->ac1_val : !k->ac1_val);
            r->fuse[k->xor_fuse] = (unsigned char)(pi && pi->out ? !pi->inv : 0);
            if (k->xorr_fuse >= 0 && pi && pi->out && pi->regcfg)
                r->fuse[k->xorr_fuse] = (unsigned char)(pi->inv ? !k->xorr_val : k->xorr_val);
        } else if (k->type == MC_V10) {
            r->fuse[k->s1_fuse] = (unsigned char)(pi && !pi->regcfg);
            r->fuse[k->s0_fuse] = (unsigned char)(pi && pi->out ? !pi->inv : 0);
        } else if (k->type == MC_GEN) {
            if (k->pol_fuse >= 0)
                r->fuse[k->pol_fuse] = (unsigned char)(pi && pi->out && (!alias_pin(pi->pin) || g_amoved[i]) ? (pi->inv ? !k->pol_hi : k->pol_hi) : 0);
            if (k->reg_kind == GR_FUSE) {
                if (k->qsplit_fuse >= 0 && g_qsplit[i] && pi && pi->out) r->fuse[k->reg_fuse] = (unsigned char)!k->reg_on;
                else if (k->reg_idle >= 0 && !(pi && (pi->out || pi->input || pi->eq[X_OE]))) r->fuse[k->reg_fuse] = (unsigned char)k->reg_idle;
                else if (k->reg_idle >= 0 && !pi->out) r->fuse[k->reg_fuse] = (unsigned char)!k->reg_on;
                else if (pi && pi->tfbread && !pi->out) r->fuse[k->reg_fuse] = (unsigned char)k->reg_on;   /* read only through .TFB */
                else r->fuse[k->reg_fuse] = (unsigned char)(pi && (pi->out || pi->eq[X_OE] || pi->input) ? (pi->regcfg ? k->reg_on : !k->reg_on) : 0);
            }
            if (k->reg_kind == GR_FUSE && D->refdefect_inreg && pi && pi->input && !pi->out && !pi->eq[X_OE]) {
                /* the reference leaves the pin in registered configuration:
                   the array then reads the register, not the pin */
                const char *up = upname();
                diag_bug(DG_WARN, "0905cb", 0, BUG_INREG, g_fix ? "%s: pin %d (%s), used as an input, configured as an input"
                                                         : "%s: pin %d (%s), used as an input, stays in registered configuration, so the array reads the register instead of the pin",
                     up, k->pin, pi->s->name);
                if (!g_fix) r->fuse[k->reg_fuse] = (unsigned char)k->reg_on;
            }
            if (pi && (pi->out || (has_ctl(pi) && (k->node || alias_pin(pi->pin))))) gen_controls(i, pi);
            if (k->ckmux_fuse >= 0 && pi && pi->eq[X_CKMUX]) {
                /* as the reference: only the clock pin itself selects the
                   pin clock; any other .CKMUX expression changes nothing */
                const Expr *x = pi->eq[X_CKMUX]->e;
                if (x && x->op == E_VAR && x->ext == X_NONE && !x->neg && !pi->eq[X_CKMUX]->neg &&
                    x->sym->kind == SK_PIN && x->sym->pin == D->clk_pin)
                    r->fuse[k->ckmux_fuse] = 1;
            }
        }
        if ((k->type == MC_GEN || k->type == MC_GAL) && k->ncfg) {
            {
                int role = !pi ? CR_UNUSED : pi->out ? (pi->regcfg ? (pi->inv ? CR_REGN : CR_REG) : (pi->inv ? CR_COMBN : CR_COMB))
                         : pi->input ? CR_INPUT : pi->eq[X_OE] ? (k->cfg_val[CR_OEONLY][0] >= 0 ? CR_OEONLY : CR_COMB) : CR_UNUSED;
                if ((role == CR_COMB || role == CR_COMBN) && pi->input && k->cfg_val[CR_COMBFB][0] >= 0) role = CR_COMBFB;
                if (k->cfg_val[role][0] < 0 && (role == CR_COMBN || role == CR_REGN)) role--;
                if (k->cfg_val[role][0] >= 0)
                    for (j = 0; j < k->ncfg; j++) r->fuse[k->cfg_fuse[j]] = (unsigned char)k->cfg_val[role][j];
            }
        }
        if (k->type == MC_GEN && k->fb_fuse >= 0 && pi && pi->muxio && pi->muxdfb && pi->out && !pi->regcfg)
            /* as the reference: .io wins, the .dfb literal reads the pin */
            diag(DG_WARN, "0908cb", 0, "pin %d (%s) is read through .io and .dfb; one feedback multiplexer cannot select both: both read the pin (as the reference compiler)", k->pin, pi->s->name);
        if (k->type == MC_GEN && k->fb_fuse >= 0 && k->fb_xor && pi && (pi->plainread || pi->muxio || pi->muxdfb)) {
            /* feedback select 'the other source' (a register read through
               .io or .dfb - as the reference, .dfb selects the pin too -, a
               combinational output read through .dfb) or the natural one */
            int other = (pi->regcfg && (pi->muxio || pi->muxdfb)) || (!pi->regcfg && pi->muxdfb && !pi->muxio);
            int v = other ? k->fb_pin : !k->fb_pin;
            if (k->io_bad && pi->muxio) {
                /* the reference writes the select .io asks for at fbfuse +
                   OFF; its own select comes from the other reads only */
                int want = pi->regcfg ? k->fb_pin : !k->fb_pin;
                int refv = pi->muxdfb ? k->fb_pin : pi->plainread ? !k->fb_pin : 0;
                if (refv != v) {
                    const char *up = upname();
                    if (g_fix) diag_bug(DG_WARN, "0905cb", 0, BUG_IOADDR, "%s: feedback select for the .io read of pin %d (%s) written at its address (the pin)", up, k->pin, pi->s->name);
                    else diag_bug(DG_WARN, "0905cb", 0, BUG_IOADDR, "%s: the feedback select for the .io read of pin %d (%s) is written at the wrong address: its .io reads %s",
                                  up, k->pin, pi->s->name, refv == (pi->regcfg ? !k->fb_pin : k->fb_pin) ? "the register" : "the pin");
                }
                if (!g_fix) {
                    if (k->fb_fuse + k->io_off >= 0) r->fuse[k->fb_fuse + k->io_off] = (unsigned char)want;
                    v = refv;
                }
            }
            r->fuse[k->fb_fuse] = (unsigned char)v;
            if (k->fb_fuse2 >= 0) r->fuse[k->fb_fuse2] = (unsigned char)v;
        } else if (k->type == MC_GEN && k->fb_fuse >= 0 && !k->fb_xor && pi && ((pi->muxio && pi->regcfg) || (pi->muxdfb && !pi->muxio && pi->out && !pi->regcfg))) {
            /* feedback multiplexer set by .io (register: the pin) or .dfb
               (combinational output: the register) */
            int v = pi->muxio && pi->regcfg ? k->fb_pin : !k->fb_pin;
            r->fuse[k->fb_fuse] = (unsigned char)v;
            if (k->fb_fuse2 >= 0) r->fuse[k->fb_fuse2] = (unsigned char)v;
        }
        if (k->t_fuse >= 0)
            r->fuse[k->t_fuse] = (unsigned char)(pi && pi->regcfg && pi->reg ? (pi->tff ? k->t_val : !k->t_val) : 0);
        if (pi && pi->nodecomb && k->node) {
            /* BUG_NODECOMB: T flip-flop fuse, and the pin macrocell whose
               rows hold the node's sum gets its own-sum and split fuses */
            int j2;
            /* (only the combinational equation: T flip-flop, unsplit) */
            if (k->t_fuse >= 0 && pi->nodecomb == 1) r->fuse[k->t_fuse] = (unsigned char)k->t_val;
            for (j2 = 0; j2 < D->nmc; j2++) {
                const Macrocell *b = &D->mc[j2];
                if (b->node || b->type != MC_GEN || b->sum0 > k->sum0 || b->sum1 < k->sum1) continue;
                if (b->qsplit_fuse >= 0) r->fuse[b->qsplit_fuse] = 1;
                if (b->split_fuse >= 0 && pi->nodecomb == 1) r->fuse[b->split_fuse] = 1;
            }
        }
        if (k->t_fuse >= 0 && !k->node && PI[k->pin].s && PI[k->pin].tfbread) {
            PinInfo *tp = &PI[k->pin];
            if (tp->reg && !tp->tff) {
                /* .TFB on a D register: the reference makes it a T flip-flop */
                const char *up = upname();
                diag_bug(DG_WARN, "0905cb", 0, BUG_TFB_D, g_fix ? "%s: pin %d (%s), a D register read through .TFB, kept a D register"
                                                         : "%s: pin %d (%s), a D register read through .TFB, becomes a T flip-flop",
                     up, k->pin, tp->s->name);
                if (!g_fix) r->fuse[k->t_fuse] = (unsigned char)k->t_val;
            } else r->fuse[k->t_fuse] = (unsigned char)k->t_val;
        }
        if (!pi) continue;
        oe_row = mc_oe_row(i, pi->regcfg);
        if (k->type == MC_GEN && k->oemux_fuse >= 0) {
            /* registered output without .OE: OE pin, otherwise the term.
               .OEMUX on a combinational output without .OE: never enabled */
            if (pi->out || pi->eq[X_OE]) r->fuse[k->oemux_fuse] = (unsigned char)(pi->regcfg && !pi->eq[X_OE]);
            if (pi->regcfg && !pi->eq[X_OE]) oe_row = -1;
            if (!pi->regcfg && !pi->eq[X_OE] && pi->eq[X_OEMUX]) oe_row = -1;
        }
        if (oe_row >= 0 && used && !alias_pin(pi->pin)) {
            if (pi->eq[X_OE]) {
                Cover f;
                int lvl;
                cv_init(&f);
                control_cover(&pi->eq[X_OE], 1, &f, &lvl);
                name_ext(nm, sizeof nm, pi->s, X_OE);
                put_rows(oe_row, 1, &f, nm);
                add_doc(pi->s, X_OE, 0, &f, 1, lvl);
                cv_free(&f);
            } else if (pi->out) {
                Cover f;
                Cube u;
                cv_init(&f);
                cube_universe(&u);
                cv_add(&f, &u);
                if (D->refdefect_oe0 || k->oe0) {
                    /* CUPLREF: the output enable of an output defaults to 1;
                       the reference leaves the term false here */
                    const char *up = upname();
                    diag_bug(DG_WARN, "0905cb", 0, BUG_PEEL_OE, g_fix ? "%s: output enable of pin %d (%s) without .OE: always enabled (CUPL default)"
                                                             : "%s: the output enable of pin %d (%s) is left false when no .OE is given", up, k->pin, pi->s->name);
                }
                if (!((D->refdefect_oe0 || k->oe0) && !g_fix)) write_row(oe_row, &u);
                add_doc(pi->s, X_OE, 0, &f, 1, 0);
                cv_free(&f);
            }
        }
        if (k->type == MC_GAL && pi->regcfg && pi->eq[X_OE] && pi->sumok) {
            /* the OE term would use the first row of the register's sum:
               accepted (and without effect) only if it is that very term */
            Cover f;
            int lvl;
            cv_init(&f);
            control_cover(&pi->eq[X_OE], 1, &f, &lvl);
            if (!(f.n == 1 && pi->sum.n >= 1 && cube_same(&f.c[0], &pi->sum.c[0]))) {
                name_ext(nm, sizeof nm, pi->s, X_D);
                diag(DG_ERROR, "0013cc", 0, "conflicting usage of pinnode:  %s", nm);
            }
            cv_free(&f);
        }
        if (!pi->out || !pi->sumok) continue;
        r0 = mc_first_row(i, pi->regcfg);
        cap = mc_capacity(i, pi->regcfg);
        if (k->type == MC_GEN && k->steer_fuse >= 0) {
            /* steering: outputs sharing rows take them in name order
               (strcmp, as the original), written with the group's first
               macrocell */
            int mem[8], nmem = 0, tot = 0, a2, b2, t2, row, size = k->sum1 - k->sum0 + 1;
            name_ext(nm, sizeof nm, pi->s, pi->reg ? (pi->tff ? X_T : X_D) : X_NONE);
            if (!steerdone[i]) {
                for (j = 0; j < D->nmc && nmem < 8; j++) {
                    const Macrocell *b = &D->mc[j];
                    PinInfo *bp = b->pin <= NP ? &PI[b->pin] : NULL;
                    if (b->type != MC_GEN || b->steer_fuse < 0 || b->sum0 != k->sum0 || b->sum1 != k->sum1) continue;
                    if (!bp || !bp->s || !bp->out || !bp->sumok) continue;
                    mem[nmem++] = j;
                }
                for (a2 = 1; a2 < nmem; a2++)
                    for (b2 = a2; b2 > 0 && strcmp(PI[D->mc[mem[b2]].pin].s->name, PI[D->mc[mem[b2 - 1]].pin].s->name) < 0; b2--) {
                        t2 = mem[b2]; mem[b2] = mem[b2 - 1]; mem[b2 - 1] = t2;
                    }
                for (a2 = 0; a2 < nmem; a2++) {
                    PinInfo *ap = &PI[D->mc[mem[a2]].pin];
                    tot += ap->sum.n ? ap->sum.zeros : 0;
                    for (t2 = 0; t2 < ap->sum.n; t2++) {
                        int dup = 0;
                        for (b2 = 0; D->steer_share && b2 < a2 && !dup; b2++) {
                            const Cover *bc = &PI[D->mc[mem[b2]].pin].sum;
                            int u;
                            for (u = 0; u < bc->n; u++) if (cube_same(&bc->c[u], &ap->sum.c[t2])) { dup = 1; break; }
                        }
                        if (!dup) tot++;
                    }
                }
                for (a2 = 0; a2 < nmem; a2++) steerdone[mem[a2]] = tot > size ? 2 : 1;
                if (nmem && tot > size) {
                    char fnm[160];
                    PinInfo *fp = &PI[D->mc[mem[0]].pin];
                    name_ext(fnm, sizeof fnm, fp->s, fp->reg ? (fp->tff ? X_T : X_D) : X_NONE);
                    diag(DG_ERROR, "0006cc", 0, "excessive number of product terms:  %s", fnm);
                }
                if (tot <= size) {
                    /* write: each term on the next free row, a term already
                       written for an earlier output reaches it as well */
                    int *own = (int *)xcalloc((size_t)size + 1, sizeof(int));
                    row = 0;
                    for (a2 = 0; a2 < nmem; a2++) {
                        PinInfo *ap = &PI[D->mc[mem[a2]].pin];
                        for (t2 = 0; t2 < ap->sum.n; t2++) {
                            int r2, at = -1;
                            for (r2 = 0; D->steer_share && r2 < row && at < 0; r2++) {
                                int b3;
                                for (b3 = 0; b3 < a2 && at < 0; b3++) {
                                    const Cover *bc = &PI[D->mc[mem[b3]].pin].sum;
                                    int u;
                                    if (!((own[r2] >> b3) & 1)) continue;
                                    for (u = 0; u < bc->n; u++)
                                        if (cube_same(&bc->c[u], &ap->sum.c[t2]) && steerrow[mem[b3] * 64 + u] == r2) { at = r2; break; }
                                }
                            }
                            if (at < 0) {
                                at = row++;
                                write_row(k->sum0 + at, &ap->sum.c[t2]);
                            }
                            own[at] |= 1 << a2;
                            if (t2 < 64) steerrow[mem[a2] * 64 + t2] = at;
                        }
                    }
                    for (row = 0; row < size; row++) {
                        if (!own[row]) continue;
                        for (a2 = 0; a2 < nmem; a2++) {
                            const Macrocell *b = &D->mc[mem[a2]];
                            /* steershare: a fuse only for a row of one output */
                            if (((own[row] >> a2) & 1) && (!D->steer_share || own[row] == (1 << a2)))
                                r->fuse[b->steer_fuse + row * b->steer_step] = 1;
                        }
                    }
                    free(own);
                }
            }
            if (steerdone[i] == 1)
                add_doc(pi->s, pi->reg ? (pi->tff ? X_T : X_D) : X_NONE, pi->inv, &pi->sum, size, level_of(pi->reg ? pi->eq[pi->tff ? X_T : X_D] : pi->eq[X_NONE]));
            continue;
        }
        /* rows shared with an earlier macrocell (complementary outputs) */
        for (j = 0; j < i; j++)
            if (mcsumset[j] && !k->node && !D->mc[j].node && D->mc[j].sum0 <= k->sum1 && k->sum0 <= D->mc[j].sum1) shared = j;
        name_ext(nm, sizeof nm, pi->s, pi->reg ? (pi->tff ? X_T : X_D) : X_NONE);
        if (pi->sum.n + (pi->sum.n ? pi->sum.zeros : 0) > cap && cap < k->sum1 - k->sum0 + 1 &&
            pi->sum.n + (pi->sum.n ? pi->sum.zeros : 0) <= k->sum1 - k->sum0 + 1) {
            /* fits the whole sum, but rows are taken by a buried register or
               by the macrocell's register used on its own: the reference
               names the node */
            int n = pi->sum.n + (pi->sum.n ? pi->sum.zeros : 0), named = 0;
            for (j = 0; j < D->nmc; j++) {
                const Macrocell *b = &D->mc[j];
                if (!b->node || b->sum0 < k->sum0 || b->sum1 > k->sum1 || b->pin > NP || !PI[b->pin].s || !PI[b->pin].out) continue;
                if (k->split_fuse >= 0 && n <= k->half1 - k->half0 + 1) continue;
                name_ext(nm, sizeof nm, PI[b->pin].s, PI[b->pin].tff ? X_T : X_D);
                diag(DG_ERROR, "0013cc", 0, "conflicting usage of pinnode:  %s", nm);
                named = 1;
            }
            if (k->qsplit_fuse >= 0 && g_qsplit[i]) {
                PinInfo *ap = &PI[alias_node(k->pin)];
                name_ext(nm, sizeof nm, ap->s, ap->tff ? X_T : X_D);
                diag(DG_ERROR, "0013cc", 0, "conflicting usage of pinnode:  %s", nm);
                named = 1;
            }
            /* otherwise the rows are taken by the output enable (GAL
               complex mode, combinational output in registered mode) */
            if (!named && pi->combspill) {
                int t2;
                diag_bug(DG_WARN, "0905cb", 0, BUG_COMBFULL, "%s: pin %d (%s) needs %d product terms but has %d rows next to its output enable term: the last term is written into row %d%s",
                         upname(), k->pin, pi->s->name, n, cap, r0 + cap,
                         dev_row_addr(D, r0 + cap) >= D->nrows * D->ncols ? ", over the configuration fuses (the device ends up in another mode)" : ", the first row of the next macrocell");
                for (t2 = 0; t2 < pi->sum.n && t2 < cap; t2++) write_row(r0 + t2, &pi->sum.c[t2]);
                for (; t2 < pi->sum.n && g_nspill < 8; t2++) { g_spillrow[g_nspill] = r0 + t2; g_spill[g_nspill++] = pi->sum.c[t2]; }
                add_doc(pi->s, X_NONE, pi->inv, &pi->sum, cap + 1, level_of(pi->eq[X_NONE]));
            } else if (!named && k->combfull && !strcmp(MODENAME, "registered")) {
                const char *up = upname();
                diag(DG_ERROR, "0907cb", 0, "%s: pin %d (%s) needs %d product terms but has %d rows next to its output enable term:  %s",
                     up, k->pin, pi->s->name, n, cap, nm);
                diag_bug(DG_NOTE, "0907cb", 0, BUG_COMBFULL, "%s: pin %d (%s): rejected (WinCUPL writes the last term into the next row)", up, k->pin, pi->s->name);
            } else if (!named) diag(DG_ERROR, "0006cc", 0, "excessive number of product terms:  %s", nm);
            continue;
        }
        if (shared >= 0) {
            /* PAL16C1: one sum for both outputs; a second equation is
               rejected on an active-low pin, else both are ORed into the
               sum (BUG_C1_SECOND) */
            if (pi->s->neg)
                diag(DG_ERROR, "0025cb", 0, "pin/node %d output mutually excluded by previous output:  %s", k->pin, pi->s->name);
            else if (!cv_equal(&mcsum[shared], &pi->sum)) {
                Cover u;
                if (g_fix) {
                    diag(DG_ERROR, "0902cb", 0, "pin/node %d shares its product terms with pin %d; the two equations must be complements of each other:  %s",
                         k->pin, D->mc[shared].pin, pi->s->name);
                    diag_bug(DG_NOTE, "0902cb", 0, BUG_C1_SECOND, "pins %d and %d (%s): rejected", D->mc[shared].pin, k->pin, pi->s->name);
                } else {
                    diag_bug(DG_WARN, "0902cb", 0, BUG_C1_SECOND, "pins %d and %d (%s) share their product terms; the terms of both equations are written into the shared sum, which implements neither",
                             D->mc[shared].pin, k->pin, pi->s->name);
                    cv_init(&u);
                    cv_or(&u, &mcsum[shared], &pi->sum);
                    if (level_of(pi->eq[X_NONE]) >= 1) cv_absorb(&u);
                    put_rows(r0, cap, &u, nm);
                    cv_free(&mcsum[shared]);
                    mcsum[shared] = u;
                }
            }
            continue;
        }
        if (i == g_shiftpin && !pi->regcfg) g_rowshift = D->refdefect_shift;
        j = put_rows(r0, cap, &pi->sum, nm);
        g_rowshift = 0;
        if (j == 0 && k->type == MC_GAL && (pi->regcfg ? k->sel_reg : k->sel_comb) >= 0) {
            /* select columns: each written term is connected to its destination */
            int sc = pi->regcfg ? k->sel_reg : k->sel_comb;
            for (j = 0; j < pi->sum.n; j++) r->fuse[dev_row_addr(D, r0 + j) + sc] = 0;
        }
        if (cap >= pi->sum.n + (pi->sum.n ? pi->sum.zeros : 0))
            add_doc(pi->s, pi->reg ? (pi->tff ? X_T : X_D) : X_NONE, pi->inv, &pi->sum, cap, level_of(pi->reg ? pi->eq[pi->tff ? X_T : X_D] : pi->eq[X_NONE]));
        if (k->type == MC_GEN && k->xsum0 >= 0 && pi->xsum.n)
            put_rows(k->xsum0, k->xsum1 - k->xsum0 + 1, &pi->xsum, nm);
        cv_init(&mcsum[i]);
        cv_copy(&mcsum[i], &pi->sum);
        mcsumset[i] = 1;
        (void)rowowner;
    }
    /* bank fuse (PAL32R16): combinational if any output of the bank is;
       a mixed bank is rejected (the original silently makes it combinational) */
    for (i = 0; i < D->nmc; i++) {
        const Macrocell *k = &D->mc[i];
        int ncomb = 0, nreg = 0, nshare = 0;
        if (k->type != MC_GEN || k->reg_kind != GR_FUSE) continue;
        for (j = 0; j < D->nmc; j++) {
            const Macrocell *b = &D->mc[j];
            PinInfo *bp;
            if (b->type != MC_GEN || b->reg_kind != GR_FUSE || b->reg_fuse != k->reg_fuse) continue;
            if (j < i) { nshare = -1; break; }
            nshare++;
            bp = b->pin <= NP ? &PI[b->pin] : NULL;
            if (!bp || !bp->s || !bp->out) continue;
            if (bp->regcfg) nreg++; else ncomb++;
        }
        if (nshare <= 1) continue;
        r->fuse[k->reg_fuse] = (unsigned char)(ncomb ? !k->reg_on : k->reg_on);
        if (!ncomb) r->fuse[k->reg_fuse] = 0 ^ (unsigned char)(nreg ? k->reg_on : 0);
        if (ncomb && nreg) {
            const char *up = upname();
            for (j = i; j < D->nmc; j++) {
                const Macrocell *b = &D->mc[j];
                PinInfo *bp = b->pin <= NP ? &PI[b->pin] : NULL;
                char nm[160];
                if (b->type != MC_GEN || b->reg_kind != GR_FUSE || b->reg_fuse != k->reg_fuse) continue;
                if (!bp || !bp->s || !bp->out || !bp->regcfg) continue;
                name_ext(nm, sizeof nm, bp->s, bp->tff ? X_T : X_D);
                if (g_fix)
                    diag(DG_ERROR, "0907cb", 0, "%s: pin %d (%s) is registered but another output of its bank is combinational; one fuse selects registered or combinational for the whole bank:  %s",
                         up, b->pin, bp->s->name, nm);
                diag_bug(g_fix ? DG_NOTE : DG_WARN, "0905cb", 0, BUG_MEGA_BANK, g_fix ? "%s: pin %d (%s): rejected" : "%s: pin %d (%s) is registered but another output of its bank is combinational: it becomes combinational",
                         up, b->pin, bp->s->name);
            }
        }
    }
    /* registers with their own sum (qsplit): the sum, clock and reset */
    for (i = 0; i < D->nmc; i++) {
        const Macrocell *k = &D->mc[i];
        PinInfo *pi;
        char nm[160];
        if (k->type != MC_GEN || k->qsplit_fuse < 0 || !g_qsplit[i]) continue;
        pi = &PI[alias_node(k->pin)];
        gen_controls(i, pi);
        if (k->t_fuse >= 0) r->fuse[k->t_fuse] = (unsigned char)(pi->tff ? k->t_val : !k->t_val);
        if (!pi->sumok) continue;
        name_ext(nm, sizeof nm, pi->s, pi->tff ? X_T : X_D);
        if (put_rows(k->qsum0, k->qsum1 - k->qsum0 + 1, &pi->sum, nm) == 0)
            add_doc(pi->s, pi->tff ? X_T : X_D, 0, &pi->sum, k->qsum1 - k->qsum0 + 1, level_of(pi->eq[pi->tff ? X_T : X_D]));
    }
    /* synchronous preset terms shared by several macrocells: the OR of the
       .SP equations of every register on the term (one product term) */
    for (i = 0; i < D->nmc; i++) {
        int row = D->mc[i].sp_row, n = 0, dup = 0;
        Eqn **qs = NULL;
        if (D->mc[i].type != MC_GEN || row < 0) continue;
        for (j = 0; j < i; j++) if (D->mc[j].sp_row == row) dup = 1;
        if (dup) continue;
        for (j = 1; j <= NP; j++) {
            PinInfo *pi = &PI[j];
            if (!pi->s || !pi->eq[X_SP] || pi->mc < 0 || D->mc[pi->mc].sp_row != row) continue;
            qs = (Eqn **)xrealloc(qs, sizeof(Eqn *) * (size_t)(n + 1));
            qs[n++] = pi->eq[X_SP];
        }
        if (n) {
            /* each equation on its own; only identical terms merge (the
               reference does not absorb one term into another) */
            Cover f, t;
            int lvl, l2, a2, b2;
            char nm[160];
            cv_init(&f);
            control_cover(qs, 1, &f, &lvl);
            for (a2 = 1; a2 < n; a2++) {
                cv_init(&t);
                control_cover(&qs[a2], 1, &t, &l2);
                for (b2 = 0; b2 < t.n; b2++) {
                    int k2, dup = 0;
                    for (k2 = 0; k2 < f.n; k2++) if (cube_same(&f.c[k2], &t.c[b2])) dup = 1;
                    if (!dup) cv_add(&f, &t.c[b2]);
                }
                cv_free(&t);
            }
            name_ext(nm, sizeof nm, qs[0]->sym, X_SP);
            if (put_rows(row, 1, &f, nm) == 0) add_doc(qs[0]->sym, X_SP, 0, &f, 1, lvl);
            cv_free(&f);
        }
        free(qs);
    }
    for (i = 1; i <= NP; i++) {
        PinInfo *pi = &PI[i];
        if (!pi->s) continue;
        if (pi->eq[X_AR]) { arq = (Eqn **)xrealloc(arq, sizeof(Eqn *) * (size_t)(nar + 1)); arq[nar++] = pi->eq[X_AR]; }
        if (pi->eq[X_SP]) { spq = (Eqn **)xrealloc(spq, sizeof(Eqn *) * (size_t)(nsp + 1)); spq[nsp++] = pi->eq[X_SP]; }
        if (pi->eq[X_AP] && D->ap_row >= 0) { apq = (Eqn **)xrealloc(apq, sizeof(Eqn *) * (size_t)(nap + 1)); apq[nap++] = pi->eq[X_AP]; }
    }
    if (D->ar_row >= 0 || D->sp_row >= 0 || D->ap_row >= 0) {
        for (i = 1; i <= NP; i++) {
            PinInfo *pi = &PI[i];
            char nm[160];
            if (!pi->s || !pi->regcfg || !pi->reg) continue;
            if (pi->mc >= 0 && D->mc[pi->mc].type == MC_GEN && D->mc[pi->mc].ck_row >= 0 && !pi->eq[X_CK] && !pi->eq[X_CKMUX]) { name_ext(nm, sizeof nm, pi->s, X_CK); diag(DG_NOTE, "0016cb", 0, "no expression assigned to:  %s", nm); }
            if (D->ar_row >= 0 && !pi->eq[X_AR]) { name_ext(nm, sizeof nm, pi->s, X_AR); diag(DG_NOTE, "0016cb", 0, "no expression assigned to:  %s", nm); }
            if (D->sp_row >= 0 && !pi->eq[X_SP]) { name_ext(nm, sizeof nm, pi->s, X_SP); diag(DG_NOTE, "0016cb", 0, "no expression assigned to:  %s", nm); }
            if (D->ap_row >= 0 && !pi->eq[X_AP]) { name_ext(nm, sizeof nm, pi->s, X_AP); diag(DG_NOTE, "0016cb", 0, "no expression assigned to:  %s", nm); }
        }
    }
    if (nar && D->ar_row >= 0) {
        Cover f;
        int lvl;
        char nm[160];
        cv_init(&f);
        control_cover(arq, nar, &f, &lvl);
        name_ext(nm, sizeof nm, arq[0]->sym, X_AR);
        put_rows(D->ar_row, 1, &f, nm);
        add_doc(arq[0]->sym, X_AR, 0, &f, 1, lvl);
        cv_free(&f);
    }
    if (nsp && D->sp_row >= 0) {
        Cover f;
        int lvl;
        char nm[160];
        cv_init(&f);
        control_cover(spq, nsp, &f, &lvl);
        name_ext(nm, sizeof nm, spq[0]->sym, X_SP);
        put_rows(D->sp_row, 1, &f, nm);
        add_doc(spq[0]->sym, X_SP, 0, &f, 1, lvl);
        cv_free(&f);
    }
    if (nap && D->ap_row >= 0) {
        Cover f;
        int lvl;
        char nm[160];
        cv_init(&f);
        control_cover(apq, nap, &f, &lvl);
        name_ext(nm, sizeof nm, apq[0]->sym, X_AP);
        put_rows(D->ap_row, 1, &f, nm);
        add_doc(apq[0]->sym, X_AP, 0, &f, 1, lvl);
        cv_free(&f);
    }
    free(apq);
    if (D->ues_base >= 0 && D->ues_len > 0) {
        const char *pn = g_des.hdr[H_PARTNO] ? g_des.hdr[H_PARTNO] : "xxxxxx";
        int n = D->ues_len / 8, b;
        for (i = 0; i < n && pn[i]; i++)
            for (b = 0; b < 8; b++) r->fuse[D->ues_base + i * 8 + b] = (unsigned char)((((unsigned char)pn[i]) >> (7 - b)) & 1);
    }
    if (D->pd_fuse >= 0) r->fuse[D->pd_fuse] = 1;
    for (i = 0; i < D->ninflag; i++) {
        int p = D->inflag_pin[i];
        if (p >= 1 && p <= NP && PI[p].s && PI[p].input && !PI[p].lq) r->fuse[D->inflag_fuse[i]] = 1;
    }
    /* BUG_COMBFULL: rows written past the macrocell, after everything else */
    for (i = 0; i < g_nspill; i++) write_row(g_spillrow[i], &g_spill[i]);
    g_nspill = 0;
    /* .TEC = 'b'0 clears the pin's technology fuse */
    for (i = 0; i < D->ntec; i++) {
        int p = D->tec_pin[i];
        if (p >= 1 && p <= NP && PI[p].s && PI[p].eq[X_TEC]) {
            Cover f;
            cv_init(&f);
            eqn_cover_neg(PI[p].eq[X_TEC]->e, 2, PI[p].eq[X_TEC]->neg, &f);
            if (f.n == 0) r->fuse[D->tec_fuse[i]] = 0;
            cv_free(&f);
        }
    }
    for (i = 0; i < g_des.nfuse; i++) {
        if (g_des.fuse_val[i] > 1) { diag(DG_ERROR, "1005cc", 0, "program error:  blowing fuse"); break; }
        if (g_des.fuse_addr[i] >= 0 && g_des.fuse_addr[i] < D->nfuses) r->fuse[g_des.fuse_addr[i]] = (unsigned char)g_des.fuse_val[i];
    }
    free(rowowner);
    free(steerdone);
    free(steerrow);
    free(g_split);
    g_split = NULL;
    free(g_qsplit);
    g_qsplit = NULL;
    return g_diag.nerr > nerr0 ? -1 : 0;
}
