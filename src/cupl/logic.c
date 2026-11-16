/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - sum-of-products covers: complement, tautology, Espresso-style
   minimiser, exact cover for small functions.  DARKSOFT. */
#include "logic.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

int g_logic_raw;
int g_logic_dist;

void cv_init(Cover *f) { f->c = NULL; f->n = f->cap = 0; f->zeros = 0; }
void cv_free(Cover *f) { free(f->c); cv_init(f); }
void cv_clear(Cover *f) { f->n = 0; f->zeros = 0; }

void cv_add(Cover *f, const Cube *c)
{
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 8;
        f->c = (Cube *)xrealloc(f->c, sizeof(Cube) * (size_t)f->cap);
    }
    f->c[f->n++] = *c;
}

void cv_copy(Cover *d, const Cover *s)
{
    d->n = 0;
    if (d->cap < s->n) {
        d->cap = s->n ? s->n : 1;
        d->c = (Cube *)xrealloc(d->c, sizeof(Cube) * (size_t)d->cap);
    }
    if (s->n) memcpy(d->c, s->c, sizeof(Cube) * (size_t)s->n);
    d->n = s->n;
    d->zeros = s->zeros;
}

static int popc(u64 x)
{
    int n = 0;
    while (x) { x &= x - 1; n++; }
    return n;
}

int cube_nlits(const Cube *c)
{
    int i, n = 0;
    for (i = 0; i < CW; i++) n += popc(c->m[i]);
    return n;
}

int cube_lit(const Cube *c, int v)
{
    u64 b = (u64)1 << (v % 64);
    if (!(c->m[v / 64] & b)) return -1;
    return (c->v[v / 64] & b) ? 1 : 0;
}

void cube_set(Cube *c, int v, int pol)
{
    u64 b = (u64)1 << (v % 64);
    if (pol < 0) { c->m[v / 64] &= ~b; c->v[v / 64] &= ~b; return; }
    c->m[v / 64] |= b;
    if (pol) c->v[v / 64] |= b; else c->v[v / 64] &= ~b;
}

void cube_universe(Cube *c) { memset(c, 0, sizeof *c); }

static int cube_is_univ(const Cube *c)
{
    int i;
    for (i = 0; i < CW; i++) if (c->m[i]) return 0;
    return 1;
}

/* a contains b: every literal of a is in b with the same polarity */
static int cube_contains(const Cube *a, const Cube *b)
{
    int i;
    for (i = 0; i < CW; i++) {
        if (a->m[i] & ~b->m[i]) return 0;
        if ((a->v[i] ^ b->v[i]) & a->m[i]) return 0;
    }
    return 1;
}

static int cube_eq(const Cube *a, const Cube *b)
{
    int i;
    for (i = 0; i < CW; i++) if (a->m[i] != b->m[i] || ((a->v[i] ^ b->v[i]) & a->m[i])) return 0;
    return 1;
}

int cube_same(const Cube *a, const Cube *b) { return cube_eq(a, b); }

static int cube_and(const Cube *a, const Cube *b, Cube *r)
{
    int i;
    for (i = 0; i < CW; i++) {
        u64 both = a->m[i] & b->m[i];
        if ((a->v[i] ^ b->v[i]) & both) return 0;
        r->m[i] = a->m[i] | b->m[i];
        r->v[i] = (a->v[i] & a->m[i]) | (b->v[i] & b->m[i]);
    }
    return 1;
}

static int cube_disjoint(const Cube *a, const Cube *b)
{
    int i;
    for (i = 0; i < CW; i++) if ((a->v[i] ^ b->v[i]) & a->m[i] & b->m[i]) return 1;
    return 0;
}

void cv_absorb(Cover *f)
{
    int i, j, k = 0;
    unsigned char *dead;
    if (f->n < 2) return;
    dead = (unsigned char *)xcalloc((size_t)f->n, 1);
    for (i = 0; i < f->n; i++) {
        if (dead[i]) continue;
        for (j = 0; j < f->n; j++) {
            if (i == j || dead[j]) continue;
            if (cube_contains(&f->c[i], &f->c[j])) {
                if (cube_eq(&f->c[i], &f->c[j]) && j < i) continue;
                dead[j] = 1;
            }
        }
    }
    for (i = 0; i < f->n; i++) if (!dead[i]) f->c[k++] = f->c[i];
    f->n = k;
    free(dead);
}

int cv_is_one(const Cover *f)
{
    int i;
    for (i = 0; i < f->n; i++) if (cube_is_univ(&f->c[i])) return 1;
    return 0;
}

void cv_or(Cover *r, const Cover *a, const Cover *b)
{
    Cover t;
    int i;
    cv_init(&t);
    for (i = 0; i < a->n; i++) cv_add(&t, &a->c[i]);
    for (i = 0; i < b->n; i++) cv_add(&t, &b->c[i]);
    if (cv_is_one(&t)) { Cube u; cube_universe(&u); t.n = 0; cv_add(&t, &u); }
    else if (!g_logic_raw) cv_absorb(&t);
    if (g_logic_raw)
        t.zeros = ((a->n == 0 && a->zeros == 0) ? 1 : a->zeros) + ((b->n == 0 && b->zeros == 0) ? 1 : b->zeros);
    cv_free(r);
    *r = t;
}

void cv_and(Cover *r, const Cover *a, const Cover *b)
{
    Cover t;
    int i, j;
    cv_init(&t);
    for (i = 0; i < a->n; i++)
        for (j = 0; j < b->n; j++) {
            Cube c;
            if (cube_and(&a->c[i], &b->c[j], &c)) cv_add(&t, &c);
        }
    cv_absorb(&t);
    cv_free(r);
    *r = t;
}

static void cofactor(Cover *r, const Cover *f, const Cube *c)
{
    int i, w;
    r->n = 0;
    for (i = 0; i < f->n; i++) {
        Cube x;
        if (cube_disjoint(&f->c[i], c)) continue;
        x = f->c[i];
        for (w = 0; w < CW; w++) { x.m[w] &= ~c->m[w]; x.v[w] &= x.m[w]; }
        cv_add(r, &x);
    }
}

/* variable counts: pos[v], neg[v] occurrences */
static int pick_binate(const Cover *f, int *unate_only)
{
    int v, best = -1, bestn = -1, anyvar = -1, anyn = -1;
    int cnt0[MAXVARS], cnt1[MAXVARS];
    int i;
    memset(cnt0, 0, sizeof cnt0);
    memset(cnt1, 0, sizeof cnt1);
    for (i = 0; i < f->n; i++) {
        int w;
        for (w = 0; w < CW; w++) {
            u64 m = f->c[i].m[w];
            while (m) {
                int b = 0;
                u64 low = m & (~m + 1);
                while (!((low >> b) & 1)) b++;
                v = w * 64 + b;
                if ((f->c[i].v[w] >> b) & 1) cnt1[v]++; else cnt0[v]++;
                m &= m - 1;
            }
        }
    }
    for (v = 0; v < MAXVARS; v++) {
        if (cnt0[v] && cnt1[v] && cnt0[v] + cnt1[v] > bestn) { best = v; bestn = cnt0[v] + cnt1[v]; }
        if (cnt0[v] + cnt1[v] > anyn) { anyvar = v; anyn = cnt0[v] + cnt1[v]; }
    }
    *unate_only = best < 0;
    return best >= 0 ? best : (anyn > 0 ? anyvar : -1);
}

int cv_tautology(const Cover *f)
{
    int v, un, r;
    Cube c;
    Cover f0, f1;
    if (cv_is_one(f)) return 1;
    if (f->n == 0) return 0;
    v = pick_binate(f, &un);
    if (un || v < 0) return 0;
    cv_init(&f0);
    cv_init(&f1);
    cube_universe(&c);
    cube_set(&c, v, 1);
    cofactor(&f1, f, &c);
    r = cv_tautology(&f1);
    if (r) {
        cube_set(&c, v, 0);
        cofactor(&f0, f, &c);
        r = cv_tautology(&f0);
    }
    cv_free(&f0);
    cv_free(&f1);
    return r;
}

static void comp_rec(Cover *r, const Cover *f)
{
    int v, un, i, j;
    Cube c;
    Cover f0, f1, c0, c1;
    r->n = 0;
    if (f->n == 0) { cube_universe(&c); cv_add(r, &c); return; }
    if (cv_is_one(f)) return;
    if (f->n == 1) {
        int w;
        for (w = 0; w < CW; w++) {
            u64 m = f->c[0].m[w];
            while (m) {
                int b = 0;
                u64 low = m & (~m + 1);
                while (!((low >> b) & 1)) b++;
                cube_universe(&c);
                cube_set(&c, w * 64 + b, !((f->c[0].v[w] >> b) & 1));
                cv_add(r, &c);
                m &= m - 1;
            }
        }
        return;
    }
    v = pick_binate(f, &un);
    cv_init(&f0); cv_init(&f1); cv_init(&c0); cv_init(&c1);
    cube_universe(&c);
    cube_set(&c, v, 1);
    cofactor(&f1, f, &c);
    cube_set(&c, v, 0);
    cofactor(&f0, f, &c);
    comp_rec(&c1, &f1);
    comp_rec(&c0, &f0);
    /* merge: cubes present in both lose the variable */
    {
        unsigned char *u0 = (unsigned char *)xcalloc((size_t)c0.n + 1, 1);
        for (i = 0; i < c1.n; i++) {
            int both = 0;
            for (j = 0; j < c0.n; j++)
                if (!u0[j] && cube_eq(&c1.c[i], &c0.c[j])) { u0[j] = 1; both = 1; break; }
            c = c1.c[i];
            if (!both) cube_set(&c, v, 1);
            cv_add(r, &c);
        }
        for (j = 0; j < c0.n; j++) {
            if (u0[j]) continue;
            c = c0.c[j];
            cube_set(&c, v, 0);
            cv_add(r, &c);
        }
        free(u0);
    }
    cv_absorb(r);
    cv_free(&f0); cv_free(&f1); cv_free(&c0); cv_free(&c1);
}

/* DeMorgan + distribution; returns 0 if it grew too large */
static int comp_dist(Cover *r, const Cover *f)
{
    Cube u;
    int i, w;
    cube_universe(&u);
    r->n = 0;
    cv_add(r, &u);
    for (i = 0; i < f->n; i++) {
        Cover s, t;
        int j, k;
        if (cube_is_univ(&f->c[i])) { r->n = 0; return 1; }
        cv_init(&s);
        for (w = 0; w < CW; w++) {
            u64 m = f->c[i].m[w];
            while (m) {
                int b = 0;
                Cube c;
                u64 low = m & (~m + 1);
                while (!((low >> b) & 1)) b++;
                cube_universe(&c);
                cube_set(&c, w * 64 + b, !((f->c[i].v[w] >> b) & 1));
                cv_add(&s, &c);
                m &= m - 1;
            }
        }
        cv_init(&t);
        for (j = 0; j < r->n; j++)
            for (k = 0; k < s.n; k++) {
                Cube c;
                if (cube_and(&r->c[j], &s.c[k], &c)) cv_add(&t, &c);
            }
        cv_absorb(&t);
        cv_free(&s);
        cv_free(r);
        *r = t;
        if (r->n > 20000) return 0;
    }
    return 1;
}

void cv_not(Cover *r, const Cover *a)
{
    Cover t;
    cv_init(&t);
    if (!g_logic_dist || !comp_dist(&t, a)) comp_rec(&t, a);
    cv_free(r);
    *r = t;
}

void cv_quick_merge(Cover *f)
{
    int changed, i, j, w;
    f->zeros = 0;
    cv_absorb(f);
    do {
        changed = 0;
        for (i = 0; i < f->n; i++) {
            for (j = i + 1; j < f->n; j++) {
                int same = 1, diff = 0;
                u64 d[CW];
                for (w = 0; w < CW; w++) {
                    if (f->c[i].m[w] != f->c[j].m[w]) { same = 0; break; }
                    d[w] = (f->c[i].v[w] ^ f->c[j].v[w]) & f->c[i].m[w];
                    diff += popc(d[w]);
                }
                if (!same || diff != 1) continue;
                for (w = 0; w < CW; w++) { f->c[i].m[w] &= ~d[w]; f->c[i].v[w] &= f->c[i].m[w]; }
                memmove(&f->c[j], &f->c[j + 1], sizeof(Cube) * (size_t)(f->n - j - 1));
                f->n--;
                changed = 1;
                break;
            }
        }
        cv_absorb(f);
    } while (changed);
}

int cv_equal(const Cover *a, const Cover *b)
{
    /* a == b  <=>  a & !b == 0 and b & !a == 0 */
    Cover na, nb, x;
    int r;
    cv_init(&na); cv_init(&nb); cv_init(&x);
    cv_not(&na, a);
    cv_not(&nb, b);
    cv_and(&x, a, &nb);
    r = x.n == 0;
    if (r) { cv_and(&x, b, &na); r = x.n == 0; }
    cv_free(&na); cv_free(&nb); cv_free(&x);
    return r;
}

static int intersects_any(const Cube *c, const Cover *r)
{
    int i;
    for (i = 0; i < r->n; i++) if (!cube_disjoint(c, &r->c[i])) return 1;
    return 0;
}

static int lit_list(const Cube *c, int *vars)
{
    int w, n = 0;
    for (w = 0; w < CW; w++) {
        u64 m = c->m[w];
        while (m) {
            int b = 0;
            u64 low = m & (~m + 1);
            while (!((low >> b) & 1)) b++;
            vars[n++] = w * 64 + b;
            m &= m - 1;
        }
    }
    return n;
}

/* expand every cube to a prime against the off-set */
static void expand(Cover *f, const Cover *off)
{
    int i, j, k;
    unsigned char *dead = (unsigned char *)xcalloc((size_t)f->n + 1, 1);
    for (i = 0; i < f->n; i++) {
        int vars[MAXVARS], nv, improved;
        Cube c;
        if (dead[i]) continue;
        c = f->c[i];
        nv = lit_list(&c, vars);
        /* first pass: raise literals that let c cover other cubes */
        do {
            int bestv = -1, bestgain = 0;
            improved = 0;
            for (k = 0; k < nv; k++) {
                Cube t = c;
                int gain = 0;
                if (vars[k] < 0) continue;
                cube_set(&t, vars[k], -1);
                if (intersects_any(&t, off)) continue;
                for (j = 0; j < f->n; j++)
                    if (j != i && !dead[j] && cube_contains(&t, &f->c[j]) && !cube_contains(&c, &f->c[j])) gain++;
                if (gain > bestgain) { bestgain = gain; bestv = k; }
            }
            if (bestv >= 0) {
                cube_set(&c, vars[bestv], -1);
                vars[bestv] = -1;
                improved = 1;
            }
        } while (improved);
        /* second pass: raise any remaining literal that stays valid */
        for (k = 0; k < nv; k++) {
            Cube t = c;
            if (vars[k] < 0) continue;
            cube_set(&t, vars[k], -1);
            if (!intersects_any(&t, off)) { c = t; vars[k] = -1; }
        }
        f->c[i] = c;
        for (j = 0; j < f->n; j++)
            if (j != i && !dead[j] && cube_contains(&c, &f->c[j])) dead[j] = 1;
    }
    k = 0;
    for (i = 0; i < f->n; i++) if (!dead[i]) f->c[k++] = f->c[i];
    f->n = k;
    free(dead);
}

static void irredundant(Cover *f)
{
    int i, j, order[4096], n = f->n;
    Cover rest, cf;
    if (n > 4096) return;
    for (i = 0; i < n; i++) order[i] = i;
    /* most literals first (smallest cubes are the likeliest redundant) */
    for (i = 1; i < n; i++) {
        int x = order[i], k = i;
        while (k > 0 && cube_nlits(&f->c[order[k - 1]]) < cube_nlits(&f->c[x])) { order[k] = order[k - 1]; k--; }
        order[k] = x;
    }
    {
        unsigned char *dead = (unsigned char *)xcalloc((size_t)n + 1, 1);
        cv_init(&rest);
        cv_init(&cf);
        for (i = 0; i < n; i++) {
            int c = order[i];
            rest.n = 0;
            for (j = 0; j < n; j++) if (j != c && !dead[j]) cv_add(&rest, &f->c[j]);
            cofactor(&cf, &rest, &f->c[c]);
            if (cv_tautology(&cf)) dead[c] = 1;
        }
        j = 0;
        for (i = 0; i < n; i++) if (!dead[i]) f->c[j++] = f->c[i];
        f->n = j;
        free(dead);
        cv_free(&rest);
        cv_free(&cf);
    }
}

/* reduce each cube to the smallest cube still covering what only it covers */
static void reduce(Cover *f)
{
    int i, j;
    Cover rest, cf, comp;
    cv_init(&rest); cv_init(&cf); cv_init(&comp);
    for (i = 0; i < f->n; i++) {
        Cube sc;
        int k, w;
        rest.n = 0;
        for (j = 0; j < f->n; j++) if (j != i) cv_add(&rest, &f->c[j]);
        cofactor(&cf, &rest, &f->c[i]);
        cv_not(&comp, &cf);
        if (comp.n == 0) continue;
        /* supercube of the complement */
        sc = comp.c[0];
        for (k = 1; k < comp.n; k++)
            for (w = 0; w < CW; w++) {
                u64 keep = sc.m[w] & comp.c[k].m[w] & ~(sc.v[w] ^ comp.c[k].v[w]);
                sc.m[w] = keep;
                sc.v[w] &= keep;
            }
        {
            Cube r;
            if (cube_and(&f->c[i], &sc, &r)) f->c[i] = r;
        }
    }
    cv_free(&rest); cv_free(&cf); cv_free(&comp);
}

static long cost(const Cover *f)
{
    long l = 0;
    int i;
    for (i = 0; i < f->n; i++) l += cube_nlits(&f->c[i]);
    return (long)f->n * 100000L + l;
}

void cv_minimize(Cover *f, int level)
{
    Cover off, best;
    long bc;
    int it;
    if (level <= 0) return;
    f->zeros = 0;
    cv_absorb(f);
    if (f->n <= 1) return;
    if (cv_is_one(f)) { Cube u; cube_universe(&u); f->n = 0; cv_add(f, &u); return; }
    cv_init(&off);
    cv_init(&best);
    cv_not(&off, f);
    if (off.n == 0) { Cube u; cube_universe(&u); f->n = 0; cv_add(f, &u); cv_free(&off); return; }
    expand(f, &off);
    irredundant(f);
    cv_copy(&best, f);
    bc = cost(f);
    for (it = 0; it < 6; it++) {
        long c;
        reduce(f);
        expand(f, &off);
        irredundant(f);
        c = cost(f);
        if (c < bc) { bc = c; cv_copy(&best, f); }
        else break;
    }
    cv_copy(f, &best);
    cv_free(&off);
    cv_free(&best);
}

static int support(const Cover *f, int *vars)
{
    Cube s;
    int i, w;
    memset(&s, 0, sizeof s);
    for (i = 0; i < f->n; i++) for (w = 0; w < CW; w++) s.m[w] |= f->c[i].m[w];
    return lit_list(&s, vars);
}

/* all primes by iterated consensus */
static void primes(Cover *p, const Cover *f)
{
    int changed = 1, i, j, guard = 0;
    cv_copy(p, f);
    cv_absorb(p);
    while (changed && guard++ < 64) {
        int n = p->n;
        changed = 0;
        for (i = 0; i < n; i++)
            for (j = i + 1; j < n; j++) {
                /* consensus exists if exactly one opposing variable */
                int w, opp = 0, ov = -1;
                Cube c;
                for (w = 0; w < CW; w++) {
                    u64 o = (p->c[i].v[w] ^ p->c[j].v[w]) & p->c[i].m[w] & p->c[j].m[w];
                    opp += popc(o);
                    if (o && ov < 0) { int b = 0; while (!((o >> b) & 1)) b++; ov = w * 64 + b; }
                }
                if (opp != 1) continue;
                for (w = 0; w < CW; w++) {
                    c.m[w] = p->c[i].m[w] | p->c[j].m[w];
                    c.v[w] = (p->c[i].v[w] & p->c[i].m[w]) | (p->c[j].v[w] & p->c[j].m[w]);
                }
                cube_set(&c, ov, -1);
                {
                    int k, cov = 0;
                    for (k = 0; k < p->n; k++) if (cube_contains(&p->c[k], &c)) { cov = 1; break; }
                    if (!cov) { cv_add(p, &c); changed = 1; }
                }
                if (p->n > 3000) return;
            }
        cv_absorb(p);
    }
}

static int g_bestn;
static int *g_best, *g_sel;
static long g_budget;

static void bb(unsigned char **covers, int np, int nm, unsigned char *covered, int ncov, int depth)
{
    int m, k, i;
    if (depth >= g_bestn || --g_budget < 0) return;
    if (ncov == nm) {
        g_bestn = depth;
        memcpy(g_best, g_sel, sizeof(int) * (size_t)depth);
        return;
    }
    if (depth + 1 >= g_bestn) return;
    /* pick the uncovered minterm with fewest covering primes */
    {
        int bm = -1, bc = 1 << 30;
        for (m = 0; m < nm; m++) {
            int c = 0;
            if (covered[m]) continue;
            for (k = 0; k < np; k++) if (covers[k][m]) c++;
            if (c < bc) { bc = c; bm = m; }
        }
        if (bm < 0 || bc == 0) return;
        for (k = 0; k < np; k++) {
            unsigned char *nc;
            int add = 0;
            if (!covers[k][bm]) continue;
            nc = (unsigned char *)xmalloc((size_t)nm);
            memcpy(nc, covered, (size_t)nm);
            for (i = 0; i < nm; i++) if (covers[k][i] && !nc[i]) { nc[i] = 1; add++; }
            g_sel[depth] = k;
            bb(covers, np, nm, nc, ncov + add, depth + 1);
            free(nc);
        }
    }
}

int cv_minimize_exact(Cover *f, int maxvars)
{
    int vars[MAXVARS], nv, nm = 0, i, k, np;
    Cover p;
    unsigned *mins;
    unsigned char **covers, *covered;
    nv = support(f, vars);
    if (nv > maxvars || nv > 14) return 0;
    if (f->n <= 1) return 1;
    cv_init(&p);
    primes(&p, f);
    if (p.n > 400) { cv_free(&p); return 0; }
    mins = (unsigned *)xmalloc(sizeof(unsigned) << nv);
    for (i = 0; i < (1 << nv); i++) {
        Cube m;
        int j, in = 0;
        cube_universe(&m);
        for (j = 0; j < nv; j++) cube_set(&m, vars[j], (i >> j) & 1);
        for (j = 0; j < f->n; j++) if (cube_contains(&f->c[j], &m)) { in = 1; break; }
        if (in) mins[nm++] = (unsigned)i;
    }
    np = p.n;
    covers = (unsigned char **)xmalloc(sizeof(unsigned char *) * (size_t)np);
    for (k = 0; k < np; k++) {
        covers[k] = (unsigned char *)xcalloc((size_t)nm + 1, 1);
        for (i = 0; i < nm; i++) {
            Cube m;
            int j;
            cube_universe(&m);
            for (j = 0; j < nv; j++) cube_set(&m, vars[j], (mins[i] >> j) & 1);
            covers[k][i] = (unsigned char)cube_contains(&p.c[k], &m);
        }
    }
    covered = (unsigned char *)xcalloc((size_t)nm + 1, 1);
    g_bestn = f->n + 1;
    g_budget = 200000;
    g_best = (int *)xcalloc((size_t)g_bestn + 1, sizeof(int));
    g_sel = (int *)xcalloc((size_t)g_bestn + 1, sizeof(int));
    bb(covers, np, nm, covered, 0, 0);
    if (g_bestn <= f->n) {
        Cover r;
        cv_init(&r);
        for (i = 0; i < g_bestn; i++) cv_add(&r, &p.c[g_best[i]]);
        cv_free(f);
        *f = r;
    }
    for (k = 0; k < np; k++) free(covers[k]);
    free(covers);
    free(covered);
    free(mins);
    free(g_best);
    free(g_sel);
    cv_free(&p);
    return 1;
}
