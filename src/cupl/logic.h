/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - sum-of-products covers, complement and minimisation.  DARKSOFT. */
#ifndef FREECUPL_LOGIC_H
#define FREECUPL_LOGIC_H

#define CW 2                    /* 64-bit words per cube: 128 variables */
#define MAXVARS (64 * CW)
typedef unsigned long long u64;

typedef struct { u64 m[CW], v[CW]; } Cube;  /* m: literal present, v: 1 = true literal */
/* zeros: in raw mode (m0) the reference keeps a constant-false product
   term for every empty operand of an OR; they occupy rows */
typedef struct { Cube *c; int n, cap; int zeros; } Cover;

void cv_init(Cover *f);
void cv_free(Cover *f);
void cv_add(Cover *f, const Cube *c);
void cv_copy(Cover *d, const Cover *s);
void cv_clear(Cover *f);
int  cube_nlits(const Cube *c);
int  cube_lit(const Cube *c, int var);      /* -1 absent, 0 neg, 1 pos */
void cube_set(Cube *c, int var, int pol);
void cube_universe(Cube *c);
int  cube_same(const Cube *a, const Cube *b);

/* raw mode (m0): OR concatenates without absorption */
extern int g_logic_raw;
/* distribution mode (m0/m1): complements are expanded by DeMorgan and
   distribution like the reference compiler does, instead of the exact
   recursive complement (same function, different set of cubes) */
extern int g_logic_dist;
/* the reference "quick" minimisation: absorption plus greedy merging of
   cubes that differ in the polarity of one variable */
void cv_quick_merge(Cover *f);

void cv_absorb(Cover *f);
void cv_or(Cover *r, const Cover *a, const Cover *b);
void cv_and(Cover *r, const Cover *a, const Cover *b);
void cv_not(Cover *r, const Cover *a);
int  cv_tautology(const Cover *f);
int  cv_is_one(const Cover *f);
/* heuristic minimisation (expand / irredundant / reduce); level 0 = none */
void cv_minimize(Cover *f, int level);
/* exact minimum cover for small supports; returns 0 if not attempted */
int  cv_minimize_exact(Cover *f, int maxvars);
int  cv_equal(const Cover *a, const Cover *b);

#endif
