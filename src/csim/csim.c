/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - CSIM compatible simulator: .si in, .so/.wo and JED vectors
   out (docs/SIMFMT.txt).  DARKSOFT. */
#include "csim/csim.h"
#include "sim3.h"
#include "bugs.h"

#define UNVERIFIED_NOTE1 "Note: simulation model for "
#define UNVERIFIED_NOTE2 " follows WinCUPL and is not verified against the data sheet"
#include "util.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OI_SIG, OI_FIELD, OI_SPACE, OI_STR };

typedef struct {
    char *name;        /* signal name (no '!') */
    int pin;           /* pin or node number */
    int neg;           /* pin level = value ^ neg (declared ! xor ORDER !) */
    int ordneg;        /* '!' written in ORDER */
    int dneg;          /* level driven on an input = value ^ dneg (neg, except B039) */
    int mc;            /* macrocell, -1 none */
    int pos;           /* display column (after "nnnn: ") */
} Col;

typedef struct {
    int kind;
    int c0, nc;        /* signal columns */
    int width;
    int pos;
    char *label;
    char *str;
} OItem;

typedef struct {
    char *name;
    int n;
    char **mem;
} Grp;   /* FIELD or VAR of the .si */

/* a statement of the vector part */
enum { ST_VEC, ST_DIR, ST_ORDER, ST_OTHER };
typedef struct {
    int kind;
    int line;          /* 0-based line of the first character */
    int col;           /* column of the first character */
    char *text;        /* statement text (comments removed, for ORDER: raw lines) */
    int line1;         /* last line (ORDER) */
} Stmt;

typedef struct {
    const CsDesign *d;
    const CsOpts *o;
    Sim3 sim;
    int simok;
    Buf so, wo;
    char **line; int nline;
    /* lexer (part before VECTORS) */
    int ln, cp;
    int echoed;
    int listing;
    int lineoff;
    int fields_listed, hdr_checked;
    int hdr_seen[CH_COUNT];
    int nerr, fatal, stop;
    int base;
    Grp *grp; int ngrp;
    Grp *var; int nvar;
    OItem *it; int nit;
    Col *col; int ncol;
    int vw;
    unsigned long seed;
    int vecno;
    int trace;
    int simoff;
    char *uval;                /* user values per column (for $SET/$OUT) */
    char *lastdisp;            /* displayed values of the last vector */
    Buf jv; int njv;
    unsigned char pinused[SIM_MAXPINS + 1];   /* the device uses the pin (else N in JED vectors) */
    unsigned char pinseen[SIM_MAXPINS + 1];   /* the pin was in an ORDER (JED vectors show its value) */
    int hdr_pending, echo_from;
    const char *cur;                          /* user values of the vector being simulated */
    int refdef;
    unsigned char clockish[SIM_MAXPINS + 1];  /* pins that clock registers (level changes applied last) */
    int oepin_used;                           /* some output is enabled by the device OE pin */               /* table header after the next VECTORS (ORDER in the vector part) */
    char *caret;
    Buf emsg;
    Buf ewo;
} Cs;

static const char *const kwords[] = {
    "msg", "for", "repeat", "fld", "field", "while", "preload", "name", "partno", "rev", "revision",
    "date", "designer", "company", "assy", "assembly", "device", "format", "order", "location", "loc",
    "base", "vectors", "vector", "macro", "mend", "hex", "octal", "decimal", "binary", "exit", "stuckh",
    "stuckl", "trace", "else", "endif", "until", "simoff", "simon", "var", "set", "endf", "endw", "comp",
    "out", "call", NULL
};

static int is_kw(const char *s)
{
    int i;
    for (i = 0; kwords[i]; i++) if (str_ieq(s, kwords[i])) return 1;
    return 0;
}

static void so_line(Cs *c, const char *s)
{
    buf_puts(&c->so, s);
    buf_puts(&c->so, "\r\n");
}

static void so_printf(Cs *c, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    so_line(c, tmp);
}

static void wo_printf(Cs *c, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    buf_puts(&c->wo, tmp);
    buf_puts(&c->wo, "\r\n");
}

static const char *msg_text(int code)
{
    switch (code) {
    case 1: return "could not open:  ";
    case 9: return "invalid syntax:  ";
    case 11: return "unknown symbol:  ";
    case 13: return "excessive test value:  ";
    case 14: return "insufficient test values";
    case 19: return "user expected ";
    case 20: return "unstable output:  ";
    case 21: return "invalid test value:  ";
    case 22: return "bad fault id:  ";
    case 25: return "Please note: inconsistent header information";
    case 26: return "Please note: missing header item(s) ";
    case 29: return "invalid trace level:  ";
    case 33: return "pin number is undefined for:  ";
    }
    return "program error:  ";
}

static int is_fatal(int code) { return code == 11 || code == 14 || code == 1 || code == 1022; }

/* an error in a listed source line: caret (col >= 0), message, empty line */
static void line_error(Cs *c, int col, int code, const char *arg)
{
    if (col >= 0) {
        Buf b = {0, 0, 0};
        int i;
        for (i = 0; i < 6 + col; i++) buf_putc(&b, ' ');
        buf_putc(&b, '^');
        so_line(c, b.p);
        buf_free(&b);
    }
    so_printf(c, "[%04dsa] %s%s", code, msg_text(code), arg ? arg : "");
    wo_printf(c, "#E %04d %s", code, arg ? arg : "");
    if (!is_fatal(code)) {
        so_line(c, "");
        wo_printf(c, "%s", "");
    }
    if (code != 25 && code != 26) c->nerr++;
    if (is_fatal(code)) c->fatal = 1;
}

static void print_src(Cs *c, int ln)
{
    so_printf(c, "%4d: %s", ln + 1 + c->lineoff, c->line[ln]);
}

static void echo_to(Cs *c, int ln)
{
    if (!c->listing) { if (c->echoed < ln) c->echoed = ln; return; }
    while (c->echoed < ln && c->echoed < c->nline) {
        print_src(c, c->echoed);
        wo_printf(c, "#H %s", c->line[c->echoed]);
        c->echoed++;
    }
}

typedef struct {
    int type;          /* 0 eof, 'a' word, 'n' number, '"' string, '\'' quoted, or the character */
    char s[256];
    int ln, col;
    int ln1, col1;     /* position after the token */
} Tok;

static void skip_ws(Cs *c, int *ln, int *cp)
{
    for (;;) {
        const char *l;
        if (*ln >= c->nline) return;
        l = c->line[*ln];
        while (l[*cp] && isspace((unsigned char)l[*cp])) (*cp)++;
        if (!l[*cp]) { (*ln)++; *cp = 0; continue; }
        if (l[*cp] == '/' && l[*cp + 1] == '*') {
            const char *e;
            *cp += 2;
            for (;;) {
                if (*ln >= c->nline) return;
                l = c->line[*ln];
                e = strstr(l + *cp, "*/");
                if (e) { *cp = (int)(e - l) + 2; break; }
                (*ln)++;
                *cp = 0;
            }
            continue;
        }
        return;
    }
}

static int word_char(int ch) { return isalnum(ch) || ch == '_'; }

static void lex_at(Cs *c, int ln, int cp, Tok *t)
{
    const char *l;
    int n = 0;
    skip_ws(c, &ln, &cp);
    memset(t, 0, sizeof *t);
    t->ln = ln;
    t->col = cp;
    if (ln >= c->nline) { t->type = 0; t->ln1 = ln; t->col1 = 0; return; }
    l = c->line[ln];
    if (word_char((unsigned char)l[cp])) {
        int digits = 1;
        while (word_char((unsigned char)l[cp]) && n < 255) {
            if (!isdigit((unsigned char)l[cp])) digits = 0;
            t->s[n++] = l[cp++];
        }
        t->type = digits ? 'n' : 'a';
    } else if (l[cp] == '"' || l[cp] == '\'') {
        int q = l[cp++];
        while (l[cp] && l[cp] != q && n < 255) t->s[n++] = l[cp++];
        if (l[cp] == q) cp++;
        t->type = q;
    } else if (l[cp] == '.' && l[cp + 1] == '.') {
        t->type = 'r';
        strcpy(t->s, "..");
        cp += 2;
    } else {
        t->type = (unsigned char)l[cp];
        t->s[0] = l[cp++];
    }
    t->ln1 = ln;
    t->col1 = cp;
}

static void peek(Cs *c, Tok *t)
{
    lex_at(c, c->ln, c->cp, t);
    echo_to(c, t->ln);
}

static void take(Cs *c, Tok *t)
{
    lex_at(c, c->ln, c->cp, t);
    echo_to(c, t->ln + 1);
    c->ln = t->ln1;
    c->cp = t->col1;
}

/* raw text up to ';' (header values); returns the column of the ';' */
static char *raw_to_semi(Cs *c, int *semi_ln, int *semi_col)
{
    Buf b = {0, 0, 0};
    char *r;
    size_t s, e;
    for (;;) {
        const char *l;
        if (c->ln >= c->nline) break;
        l = c->line[c->ln];
        if (!l[c->cp]) {
            c->ln++;
            c->cp = 0;
            if (c->ln < c->nline) echo_to(c, c->ln + 1);
            buf_putc(&b, ' ');
            continue;
        }
        if (l[c->cp] == ';') break;
        buf_putc(&b, l[c->cp++]);
    }
    *semi_ln = c->ln;
    *semi_col = c->cp;
    if (c->ln < c->nline) c->cp++;
    if (!b.p) return xstrdup("");
    for (s = 0; b.p[s] && isspace((unsigned char)b.p[s]); s++) ;
    e = strlen(b.p);
    while (e > s && isspace((unsigned char)b.p[e - 1])) e--;
    r = xstrndup(b.p + s, e - s);
    buf_free(&b);
    return r;
}

static void syn_error(Cs *c, const Tok *t, const char *what)
{
    line_error(c, t->ln < c->echoed && c->listing ? t->col : -1, 9, what ? what : t->s);
}

/* skip to the next ';' */
static void skip_stmt(Cs *c)
{
    Tok t;
    for (;;) {
        take(c, &t);
        if (t.type == 0 || t.type == ';') return;
    }
}

static const CsSym *find_sym(const Cs *c, const char *name)
{
    int i;
    for (i = 0; i < c->d->nsym; i++) if (str_ieq(c->d->sym[i].name, name)) return &c->d->sym[i];
    return NULL;
}

static const CsField *find_dfield(const Cs *c, const char *name)
{
    int i;
    for (i = 0; i < c->d->nfld; i++) if (str_ieq(c->d->fld[i].name, name)) return &c->d->fld[i];
    return NULL;
}

static Grp *find_grp(Grp *g, int n, const char *name)
{
    int i;
    for (i = 0; i < n; i++) if (str_ieq(g[i].name, name)) return &g[i];
    return NULL;
}

/* split "abc12" into prefix and index; returns index or -1 */
static int split_index(const char *s, char *prefix, size_t n)
{
    size_t l = strlen(s), k = l;
    while (k > 0 && isdigit((unsigned char)s[k - 1])) k--;
    if (k == l || k == 0) { snprintf(prefix, n, "%s", s); return -1; }
    snprintf(prefix, n, "%.*s", (int)k, s);
    return atoi(s + k);
}

typedef struct { char **v; int n; } NameList;

static void nl_add(NameList *l, const char *s)
{
    l->v = (char **)xrealloc(l->v, sizeof(char *) * (size_t)(l->n + 1));
    l->v[l->n++] = xstrdup(s);
}

static void nl_free(NameList *l)
{
    int i;
    for (i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    l->v = NULL;
    l->n = 0;
}

/* expand name..end (end: number or name with the same prefix) */
static int expand_range(const char *a, const Tok *b, NameList *out)
{
    char pa[256], pb[256];
    int ia = split_index(a, pa, sizeof pa), ib, i;
    if (ia < 0) return -1;
    if (b->type == 'n') ib = atoi(b->s);
    else {
        ib = split_index(b->s, pb, sizeof pb);
        if (ib < 0 || !str_ieq(pa, pb)) return -1;
    }
    for (i = ia;; i += ia <= ib ? 1 : -1) {
        char nm[300];
        snprintf(nm, sizeof nm, "%s%d", pa, i);
        nl_add(out, nm);
        if (i == ib) break;
    }
    return 0;
}

/* list of names in a FIELD/VAR: [a, b0..3, c] or a bare list up to ';' */
static int parse_namelist(Cs *c, NameList *out, Tok *bad)
{
    Tok t, u;
    int br = 0;
    peek(c, &t);
    if (t.type == '[') { take(c, &t); br = 1; }
    for (;;) {
        take(c, &t);
        if (t.type == 'a' || t.type == 'n') {
            char name[256];
            snprintf(name, sizeof name, "%s", t.s);
            peek(c, &u);
            if (u.type == 'r') {
                Tok r;
                take(c, &r);
                take(c, &u);
                if (expand_range(name, &u, out) != 0) { *bad = r; return -1; }
            } else nl_add(out, name);
            peek(c, &u);
            if (u.type == ',') { take(c, &u); continue; }
            if (br && u.type == ']') { take(c, &u); return 0; }
            if (!br && u.type == ';') return 0;
            *bad = u;
            return -1;
        }
        if (t.type == '!') continue;
        *bad = t;
        return -1;
    }
}

static const char *const hdr_kw[CH_COUNT][2] = {
    {"name", NULL}, {"partno", NULL}, {"rev", "revision"}, {"date", NULL}, {"designer", NULL},
    {"company", NULL}, {"assy", "assembly"}, {"loc", "location"}, {"device", NULL}, {"format", NULL}
};

static int hdr_index(const char *s)
{
    int i;
    for (i = 0; i < CH_COUNT; i++)
        if (str_ieq(s, hdr_kw[i][0]) || (hdr_kw[i][1] && str_ieq(s, hdr_kw[i][1]))) return i;
    return -1;
}

static void header_stmt(Cs *c, int h)
{
    int sl, sc;
    char *v = raw_to_semi(c, &sl, &sc);
    c->hdr_seen[h] = 1;
    if (h != CH_DEVICE && h != CH_FORMAT) {
        const char *ref = c->d->hdr[h];
        if (!ref || strcmp(ref, v) != 0) line_error(c, sc, 25, NULL);
    }
    free(v);
}

static void check_header(Cs *c)
{
    int i, missing = 0;
    if (c->hdr_checked) return;
    c->hdr_checked = 1;
    for (i = 0; i < CH_FORMAT; i++) if (!c->hdr_seen[i]) missing = 1;
    if (missing) so_printf(c, "[0026sa] %s", msg_text(26));
}

static void add_col(Cs *c, const char *name, int ordneg)
{
    const CsSym *s = find_sym(c, name);
    Col *k;
    c->col = (Col *)xrealloc(c->col, sizeof(Col) * (size_t)(c->ncol + 1));
    k = &c->col[c->ncol++];
    memset(k, 0, sizeof *k);
    k->name = xstrdup(s ? s->name : name);
    k->pin = s ? s->pin : 0;
    k->ordneg = ordneg;
    k->neg = (s ? s->neg : 0) ^ ordneg;
    k->mc = c->simok && k->pin ? sim3_mc_of(&c->sim, k->pin) : -1;
    k->dneg = k->neg;
    /* B039: the value of an input is its pin level */
    if (c->refdef && c->d->dev->sim_inlevel && c->simok && k->pin) {
        const McCfg *m = k->mc >= 0 ? &c->sim.s.mc[k->mc] : NULL;
        /* (a cell whose OE term is false never drives its pin) */
        if (!m || m->node == 0) {
            if (!m || !sim3_is_output(&c->sim, k->pin) || (m->oe_kind == OE_ROW && !c->sim.s.live[m->oe_row])) k->dneg = ordneg;
        }
    }
    if (k->pin > 0 && k->pin <= SIM_MAXPINS) c->pinseen[k->pin] = 1;
}

static OItem *add_item(Cs *c, int kind)
{
    OItem *it;
    c->it = (OItem *)xrealloc(c->it, sizeof(OItem) * (size_t)(c->nit + 1));
    it = &c->it[c->nit++];
    memset(it, 0, sizeof *it);
    it->kind = kind;
    it->c0 = c->ncol;
    return it;
}

static void free_order(Cs *c)
{
    int i;
    for (i = 0; i < c->nit; i++) { free(c->it[i].label); free(c->it[i].str); }
    for (i = 0; i < c->ncol; i++) free(c->col[i].name);
    free(c->it); free(c->col);
    c->it = NULL; c->col = NULL;
    c->nit = c->ncol = 0;
}

/* members of a group name (si field, VAR, design field); NULL if none */
static int group_members(Cs *c, const char *name, NameList *out)
{
    Grp *g = find_grp(c->grp, c->ngrp, name);
    const CsField *f;
    int i;
    if (!g) g = find_grp(c->var, c->nvar, name);
    if (g) { for (i = 0; i < g->n; i++) nl_add(out, g->mem[i]); return 1; }
    f = find_dfield(c, name);
    if (f) { for (i = 0; i < f->n; i++) nl_add(out, f->mem[i]); return 1; }
    return 0;
}

static const char *fld_name(const Cs *c, int i)
{
    return i < c->d->nfld ? c->d->fld[i].name : c->grp[i - c->d->nfld].name;
}

static void list_fields(Cs *c)
{
    int i, j, n = 0, nt = c->d->nfld + c->ngrp, *ord;
    if (c->fields_listed) return;
    c->fields_listed = 1;
    /* in name order, as the reference */
    ord = (int *)xcalloc((size_t)nt + 1, sizeof(int));
    for (i = 0; i < nt; i++) ord[i] = i;
    for (i = 1; i < nt; i++)
        for (j = i; j > 0 && strcmp(fld_name(c, ord[j - 1]), fld_name(c, ord[j])) > 0; j--) {
            int t = ord[j]; ord[j] = ord[j - 1]; ord[j - 1] = t;
        }
    for (j = 0; j < nt; j++) {
        const char *nm;
        int nm_n, k;
        char **mem;
        Buf b = {0, 0, 0};
        i = ord[j];
        if (i < c->d->nfld) { nm = c->d->fld[i].name; nm_n = c->d->fld[i].n; mem = c->d->fld[i].mem; }
        else { nm = c->grp[i - c->d->nfld].name; nm_n = c->grp[i - c->d->nfld].n; mem = c->grp[i - c->d->nfld].mem; }
        buf_printf(&b, "%s = [", nm);
        for (k = 0; k < nm_n; k++) buf_printf(&b, "%s%s", k ? "," : "", mem[k]);
        buf_puts(&b, "];");
        so_printf(c, "%4d: FIELD %s", c->echoed + 1 + c->lineoff, b.p);
        wo_printf(c, "#F %s", b.p);
        c->lineoff++;
        n++;
        buf_free(&b);
    }
    free(ord);
    if (n) {
        so_printf(c, "%4d:", c->echoed + 1 + c->lineoff);
        wo_printf(c, "%s", "");
        c->lineoff++;
    }
}

/* ORDER: items ; (the ORDER keyword has been taken) */
static void parse_order(Cs *c)
{
    Tok t, u;
    free_order(c);
    take(c, &t);
    if (t.type != ':') { syn_error(c, &t, NULL); }
    for (;;) {
        int neg = 0;
        take(c, &t);
        if (t.type == 0) break;
        if (t.type == ';') break;
        if (t.type == ',') continue;
        if (t.type == '%') {
            OItem *it;
            take(c, &u);
            if (u.type != 'n') { syn_error(c, &u, NULL); skip_stmt(c); break; }
            it = add_item(c, OI_SPACE);
            it->width = atoi(u.s);
            continue;
        }
        if (t.type == '"') {
            OItem *it = add_item(c, OI_STR);
            it->str = xstrdup(t.s);
            it->width = (int)strlen(t.s);
            continue;
        }
        if (t.type == '!') { neg = 1; take(c, &t); }
        if (t.type != 'a') { syn_error(c, &t, NULL); skip_stmt(c); break; }
        if (is_kw(t.s)) { syn_error(c, &t, NULL); skip_stmt(c); break; }
        {
            NameList nl = {0, 0};
            char name[256];
            snprintf(name, sizeof name, "%s", t.s);
            peek(c, &u);
            if (u.type == 'r') {
                take(c, &u);
                take(c, &u);
                if (expand_range(name, &u, &nl) != 0) { syn_error(c, &u, NULL); nl_free(&nl); skip_stmt(c); break; }
                {
                    int i;
                    for (i = 0; i < nl.n; i++) {
                        OItem *it;
                        if (!find_sym(c, nl.v[i])) {
                            line_error(c, -1, 11, nl.v[i]);
                            nl_free(&nl);
                            return;
                        }
                        it = add_item(c, OI_SIG);
                        add_col(c, nl.v[i], neg);
                        it->nc = 1;
                        it->width = 1;
                        it->label = (char *)xmalloc(strlen(nl.v[i]) + 2);
                        sprintf(it->label, "%s%s", neg ? "!" : "", nl.v[i]);
                    }
                }
                nl_free(&nl);
                continue;
            }
            if (group_members(c, name, &nl)) {
                OItem *it = add_item(c, OI_FIELD);
                int i, bad = -1;
                for (i = 0; i < nl.n; i++) if (!find_sym(c, nl.v[i])) bad = i;
                if (bad >= 0) { line_error(c, -1, 11, nl.v[bad]); nl_free(&nl); return; }
                for (i = 0; i < nl.n; i++) add_col(c, nl.v[i], 0);
                it->nc = nl.n;
                it->width = nl.n;
                it->label = xstrdup(name);
                nl_free(&nl);
                continue;
            }
            if (!find_sym(c, name)) { line_error(c, -1, 11, name); return; }
            {
                OItem *it = add_item(c, OI_SIG);
                add_col(c, name, neg);
                it->nc = 1;
                it->width = 1;
                it->label = (char *)xmalloc(strlen(name) + 2);
                sprintf(it->label, "%s%s", neg ? "!" : "", find_sym(c, name)->name);
            }
        }
    }
    {
        int i, pos = 0;
        for (i = 0; i < c->nit; i++) {
            int k;
            c->it[i].pos = pos;
            for (k = 0; k < c->it[i].nc; k++) c->col[c->it[i].c0 + k].pos = pos + k;
            pos += c->it[i].width;
        }
        c->vw = pos;
    }
    free(c->uval);
    free(c->lastdisp);
    c->uval = (char *)xcalloc((size_t)c->ncol + 1, 1);
    c->lastdisp = (char *)xcalloc((size_t)c->ncol + 1, 1);
    memset(c->uval, 'X', (size_t)c->ncol);
    memset(c->lastdisp, 'X', (size_t)c->ncol);
}

static void table_header(Cs *c)
{
    int W = c->vw, H = 0, i, r;
    char **rows;
    Buf b = {0, 0, 0};
    int *rows_of = (int *)xcalloc((size_t)c->nit + 1, sizeof(int));
    for (i = 0; i < c->nit; i++) {
        const OItem *it = &c->it[i];
        int h = 0, l = it->label ? (int)strlen(it->label) : 0;
        if (it->kind == OI_SIG || (it->kind == OI_FIELD && it->nc == 1)) h = l;
        else if (it->kind == OI_FIELD && it->width > 0) {
            /* one row when the name fits into the field and the blank
               columns after it (the header's three trailing blanks count) */
            int room = 0, j;
            for (j = i + 1; j < c->nit && c->it[j].kind == OI_SPACE; j++) room += c->it[j].width;
            if (j == c->nit) room += 3;
            h = l <= it->width + room - 1 ? 1 : (l + it->width - 1) / it->width;
        }
        rows_of[i] = h;
        if (h > H) H = h;
    }
    rows = (char **)xcalloc((size_t)H + 1, sizeof(char *));
    for (r = 0; r < H; r++) {
        rows[r] = (char *)xmalloc((size_t)W + 4);
        memset(rows[r], ' ', (size_t)W + 3);
        rows[r][W + 3] = 0;
    }
    for (i = 0; i < c->nit; i++) {
        const OItem *it = &c->it[i];
        int l = it->label ? (int)strlen(it->label) : 0;
        if (it->kind == OI_SIG || (it->kind == OI_FIELD && it->nc == 1)) {
            int k;
            const char *lb = it->kind == OI_SIG ? it->label : c->col[it->c0].name;
            l = (int)strlen(lb);
            for (k = 0; k < l && k < H; k++) rows[H - l + k][it->pos] = lb[k];
        } else if (it->kind == OI_FIELD && it->width > 0 && l > 0) {
            int h = rows_of[i], k;
            if (h == 1) {
                int off = (it->width - l) / 2, p0 = it->pos + off;
                for (k = 0; k < l; k++) if (p0 + k >= 0 && p0 + k < W + 3) rows[H - 1][p0 + k] = it->label[k];
            } else {
                /* rows of the field width; the last one centred */
                for (k = 0; k < l; k++) {
                    int row = k / it->width, last = (l - 1) / it->width, off = 0;
                    if (row == last) off = (it->width - (l - last * it->width)) / 2;
                    rows[H - h + row][it->pos + off + k % it->width] = it->label[k];
                }
            }
        }
    }
    free(rows_of);
    so_line(c, "\f");
    for (i = 0; i < 6 + W + 3; i++) buf_putc(&b, '=');
    so_line(c, b.p);
    for (r = 0; r < H; r++) {
        so_printf(c, "      %s", rows[r]);
        free(rows[r]);
    }
    free(rows);
    so_line(c, b.p);
    buf_free(&b);
}

static void parse_field_stmt(Cs *c, int isvar)
{
    Tok t, u, bad;
    NameList nl = {0, 0};
    Grp *g;
    take(c, &t);
    if (t.type != 'a' || isdigit((unsigned char)t.s[strlen(t.s) - 1]) || is_kw(t.s)) {
        syn_error(c, &t, NULL);
        skip_stmt(c);
        return;
    }
    take(c, &u);
    if (u.type != '=') { syn_error(c, &u, NULL); skip_stmt(c); return; }
    if (parse_namelist(c, &nl, &bad) != 0) {
        syn_error(c, &bad, bad.type == 'r' ? "." : NULL);
        if (bad.type != ';') skip_stmt(c);
        nl_free(&nl);
        return;
    }
    take(c, &u);
    if (isvar) {
        c->var = (Grp *)xrealloc(c->var, sizeof(Grp) * (size_t)(c->nvar + 1));
        g = &c->var[c->nvar++];
    } else {
        c->grp = (Grp *)xrealloc(c->grp, sizeof(Grp) * (size_t)(c->ngrp + 1));
        g = &c->grp[c->ngrp++];
    }
    g->name = xstrdup(t.s);
    g->n = nl.n;
    g->mem = nl.v;
}

static void parse_stuck(Cs *c, int v)
{
    Tok t, u;
    take(c, &t);
    if (t.type != 'n') { syn_error(c, &t, NULL); skip_stmt(c); return; }
    if (c->simok && sim3_stuck(&c->sim, atoi(t.s), v) != 0) line_error(c, t.col, 22, t.s);
    take(c, &u);
    if (u.type != ';') { syn_error(c, &u, NULL); skip_stmt(c); }
}

/* returns 1 when VECTORS was reached */
static int parse_decls(Cs *c)
{
    Tok t;
    for (;;) {
        int h;
        peek(c, &t);
        if (t.type == 0) return 0;
        if (t.type == 'a' && (h = hdr_index(t.s)) >= 0) {
            take(c, &t);
            header_stmt(c, h);
            continue;
        }
        if (t.type == 'a' && !c->hdr_checked) check_header(c);
        if (t.type == 'a' && (str_ieq(t.s, "field") || str_ieq(t.s, "fld"))) {
            take(c, &t);
            parse_field_stmt(c, 0);
        } else if (t.type == 'a' && str_ieq(t.s, "var")) {
            take(c, &t);
            parse_field_stmt(c, 1);
        } else if (t.type == 'a' && str_ieq(t.s, "base")) {
            Tok u;
            take(c, &t);
            take(c, &u);
            if (u.type == ':') take(c, &u);
            if (str_ieq(u.s, "octal")) c->base = 8;
            else if (str_ieq(u.s, "decimal")) c->base = 10;
            else if (str_ieq(u.s, "hex")) c->base = 16;
            else if (str_ieq(u.s, "binary")) c->base = 2;
            else syn_error(c, &u, NULL);
            skip_stmt(c);
        } else if (t.type == 'a' && str_ieq(t.s, "order")) {
            list_fields(c);
            take(c, &t);
            parse_order(c);
        } else if (t.type == 'a' && (str_ieq(t.s, "stuckl") || str_ieq(t.s, "stuckh"))) {
            take(c, &t);
            parse_stuck(c, str_ieq(t.s, "stuckh"));
        } else if (t.type == 'a' && (str_ieq(t.s, "vectors") || str_ieq(t.s, "vector"))) {
            /* the VECTORS line itself is not listed */
            int l0 = t.ln;
            c->listing = 0;
            take(c, &t);
            lex_at(c, c->ln, c->cp, &t);
            if (t.type == ':' && t.ln == l0) { c->ln = t.ln1; c->cp = t.col1; }
            if (c->echoed < l0 + 1) c->echoed = l0 + 1;
            return 1;
        } else {
            take(c, &t);
            syn_error(c, &t, NULL);
            skip_stmt(c);
        }
        if (c->fatal) return 0;
    }
}

static Stmt *split_stmts(Cs *c, int *nst)
{
    Stmt *st = NULL;
    int n = 0, ln = c->ln, cp = c->cp;
    while (ln < c->nline) {
        const char *l;
        skip_ws(c, &ln, &cp);
        if (ln >= c->nline) break;
        l = c->line[ln];
        st = (Stmt *)xrealloc(st, sizeof(Stmt) * (size_t)(n + 1));
        memset(&st[n], 0, sizeof(Stmt));
        st[n].line = ln;
        st[n].col = cp;
        if (l[cp] == '$') {
            /* directive: up to ';' (quotes respected) */
            Buf b = {0, 0, 0};
            int q = 0;
            int colon;
            {
                /* $FOR $IF $ELSE $WHILE $DO end with ':' */
                char w[16];
                int k2 = 0, j = cp + 1;
                while (l[j] && isspace((unsigned char)l[j])) j++;
                while (word_char((unsigned char)l[j]) && k2 < 15) w[k2++] = l[j++];
                w[k2] = 0;
                colon = str_ieq(w, "for") || str_ieq(w, "if") || str_ieq(w, "else") || str_ieq(w, "while") || str_ieq(w, "do");
            }
            while (ln < c->nline) {
                l = c->line[ln];
                if (!l[cp]) { ln++; cp = 0; buf_putc(&b, ' '); continue; }
                if (!q && l[cp] == '/' && l[cp + 1] == '*') { skip_ws(c, &ln, &cp); continue; }
                if (l[cp] == '"') q = !q;
                if (!q && (l[cp] == ';' || (colon && l[cp] == ':'))) { cp++; break; }
                buf_putc(&b, l[cp++]);
            }
            st[n].kind = ST_DIR;
            st[n].text = b.p ? b.p : xstrdup("");
            st[n].line1 = ln;
            n++;
            continue;
        }
        {
            /* keyword statement (ORDER, VECTORS, ...)? */
            char w[32];
            int k = 0;
            while (word_char((unsigned char)l[cp + k]) && k < 31) { w[k] = l[cp + k]; k++; }
            w[k] = 0;
            if (k > 0 && (str_ieq(w, "order") || str_ieq(w, "vectors") || str_ieq(w, "vector") || str_ieq(w, "var") ||
                          str_ieq(w, "field") || str_ieq(w, "fld") || str_ieq(w, "base")) &&
                (l[cp + k] == ':' || isspace((unsigned char)l[cp + k]) || !l[cp + k])) {
                int vec = str_ieq(w, "vectors") || str_ieq(w, "vector");
                st[n].kind = str_ieq(w, "order") ? ST_ORDER : ST_OTHER;
                st[n].text = xstrdup(w);
                if (vec) {
                    cp += k;
                    while (l[cp] && isspace((unsigned char)l[cp])) cp++;
                    if (l[cp] == ':') cp++;
                    st[n].line1 = ln;
                } else {
                    while (ln < c->nline) {
                        l = c->line[ln];
                        if (!l[cp]) { ln++; cp = 0; continue; }
                        if (l[cp] == ';') { cp++; break; }
                        cp++;
                    }
                    st[n].line1 = ln < c->nline ? ln : c->nline - 1;
                }
                n++;
                continue;
            }
        }
        {
            /* a vector: to the end of the line or a '$' */
            Buf b = {0, 0, 0};
            int q = 0;
            while (l[cp]) {
                if (!q && l[cp] == '$') break;
                if (!q && l[cp] == '/' && l[cp + 1] == '*') {
                    int l0 = ln;
                    skip_ws(c, &ln, &cp);
                    if (ln != l0) break;
                    continue;
                }
                if (l[cp] == '\'' || l[cp] == '"') q = q ? (q == l[cp] ? 0 : q) : l[cp];
                buf_putc(&b, l[cp++]);
            }
            if (!l[cp] && ln < c->nline && c->line[ln] == l) { ln++; cp = 0; }
            st[n].kind = ST_VEC;
            st[n].text = b.p ? b.p : xstrdup("");
            st[n].line1 = st[n].line;
            n++;
        }
    }
    *nst = n;
    return st;
}

static int bits_per_digit(int base) { return base == 8 ? 3 : base == 2 ? 1 : 4; }

/* expand a quoted value into the characters it stands for; width w for a
   field start (0: digit expansion) */
static int expand_quoted(const char *q, int out, int base, int w, char *buf, int max)
{
    int nd = (int)strlen(q), bpd = bits_per_digit(base), nb = nd * bpd, i;
    unsigned long long v = 0;
    char hi = out ? 'H' : '1', lo = out ? 'L' : '0';
    if (nd == 0) return 0;
    if (strchr("XxHhLlZz", q[0])) {
        int n = w ? w : nb;
        if (n > max) n = max;
        for (i = 0; i < n; i++) buf[i] = (char)toupper((unsigned char)q[0]);
        return n;
    }
    for (i = 0; i < nd; i++) {
        int dv;
        char ch = (char)toupper((unsigned char)q[i]);
        if (isdigit((unsigned char)ch)) dv = ch - '0';
        else if (ch >= 'A' && ch <= 'F') dv = ch - 'A' + 10;
        else dv = 0;
        v = v * (unsigned)base + (unsigned)dv;
    }
    if (w) nb = w;
    if (nb > max) nb = max;
    for (i = 0; i < nb; i++) buf[i] = ((v >> (nb - 1 - i)) & 1) ? hi : lo;
    return nb;
}

typedef struct {
    char *v;
    int *srcpos;
    int nv;
    int excess;        /* values beyond the columns */
    int *xpos; char *xval; int nx;
} VecVals;

static int field_start(const Cs *c, int col, int *width)
{
    int i;
    for (i = 0; i < c->nit; i++)
        if (c->it[i].kind == OI_FIELD && c->it[i].c0 == col) { *width = c->it[i].nc; return 1; }
    return 0;
}

/* 1: the pin is declared without an equation and read by the design,
   2: declared without an equation and not read, 0: otherwise */
static int col_noeqn(const Cs *c, int k)
{
    int i;
    for (i = 0; i < c->d->nnoeqn; i++)
        if (c->d->noeqn[i] == c->col[k].pin) return c->d->noeqn_used[i] ? 1 : 2;
    return 0;
}

static int col_is_output(const Cs *c, int k)
{
    return c->simok && c->col[k].pin && sim3_is_output(&c->sim, c->col[k].pin) && !col_noeqn(c, k);
}

static void parse_values(Cs *c, const char *text, VecVals *vv)
{
    int i = 0, n = 0;
    memset(vv, 0, sizeof *vv);
    vv->v = (char *)xcalloc((size_t)c->ncol + 1, 1);
    vv->srcpos = (int *)xcalloc((size_t)c->ncol + 1, sizeof(int));
    while (text[i]) {
        char ch = text[i];
        if (isspace((unsigned char)ch)) { i++; continue; }
        if (ch == '\'' || ch == '"') {
            char q[128], tmp[512];
            int k = 0, j, w = 0, cnt, start = i;
            i++;
            while (text[i] && text[i] != ch && k < 127) q[k++] = text[i++];
            q[k] = 0;
            if (text[i] == ch) i++;
            if (n < c->ncol) field_start(c, n, &w);
            cnt = expand_quoted(q, ch == '"', c->base, w, tmp, (int)sizeof tmp);
            for (j = 0; j < cnt; j++) {
                if (n < c->ncol) { vv->v[n] = tmp[j]; vv->srcpos[n] = start; n++; }
                else {
                    vv->xpos = (int *)xrealloc(vv->xpos, sizeof(int) * (size_t)(vv->nx + 1));
                    vv->xval = (char *)xrealloc(vv->xval, (size_t)vv->nx + 2);
                    vv->xpos[vv->nx] = i - 1;
                    vv->xval[vv->nx] = 0;
                    vv->nx++;
                }
            }
            continue;
        }
        if (n < c->ncol) { vv->v[n] = (char)toupper((unsigned char)ch); vv->srcpos[n] = i; n++; }
        else {
            vv->xpos = (int *)xrealloc(vv->xpos, sizeof(int) * (size_t)(vv->nx + 1));
            vv->xval = (char *)xrealloc(vv->xval, (size_t)vv->nx + 2);
            vv->xpos[vv->nx] = i;
            vv->xval[vv->nx] = (char)ch;
            vv->nx++;
        }
        i++;
    }
    vv->nv = n;
}

static void free_vals(VecVals *vv)
{
    free(vv->v); free(vv->srcpos); free(vv->xpos); free(vv->xval);
}

static unsigned rnd_bit(Cs *c)
{
    c->seed = (c->seed * 22695477UL + 1UL) & 0xFFFFFFFFUL;
    return (unsigned)((c->seed >> 16) & 0x7FFF) & 1u;
}

/* a pin that may take C, K and P: the device clock pin; on devices with
   product-term clocks (no clock pin of their own) any input */
static int is_clock_pin(const Cs *c, int pin)
{
    const Device *d = c->d->dev;
    int m;
    if (!pin || d->sim_noclock == 1 || (c->refdef && d->sim_regzero)) return 0;
    if (pin == d->clk_pin) return 1;
    if (d->clk_pin) return 0;
    for (m = 0; m < d->nmc; m++) if (d->mc[m].ck_row >= 0) return 1;
    return 0;
}

/* display character of column k from the simulation */
static char sim_char(const Cs *c, int k)
{
    int v = sim3_value(&c->sim, c->col[k].pin);
    if (v == VX) return 'X';
    if (v == VZ) return 'Z';
    return (v ^ c->col[k].neg) ? 'H' : 'L';
}

/* displayed value of column k for the user value ch */
static char out_disp(const Cs *c, int k, char ch)
{
    int m, oe;
    if (!col_is_output(c, k)) return ch;
    m = c->col[k].mc;
    oe = m >= 0 ? c->sim.oe[m] : V1;
    if (ch == 'N') return 'N';
    if (oe == V0 && (ch == '0' || ch == '1' || ch == 'X')) return ch;
    return sim_char(c, k);
}

static char pin_char(int v)
{
    return v == V0 ? 'L' : v == V1 ? 'H' : v == VZ ? 'Z' : 'X';
}

typedef struct { int code; int col; char arg[64]; } VErr;

static void add_err(VErr **e, int *ne, int code, int col, const char *arg)
{
    *e = (VErr *)xrealloc(*e, sizeof(VErr) * (size_t)(*ne + 1));
    (*e)[*ne].code = code;
    (*e)[*ne].col = col;
    snprintf((*e)[*ne].arg, sizeof (*e)[*ne].arg, "%s", arg);
    (*ne)++;
}

static void format_vals(const Cs *c, const char *disp, Buf *b, int withstr)
{
    int i, k;
    for (i = 0; i < c->nit; i++) {
        const OItem *it = &c->it[i];
        if (it->kind == OI_SPACE) { if (withstr) for (k = 0; k < it->width; k++) buf_putc(b, ' '); }
        else if (it->kind == OI_STR) { if (withstr) buf_puts(b, it->str); }
        else for (k = 0; k < it->nc; k++) buf_putc(b, disp[it->c0 + k]);
    }
    if (!b->p) buf_puts(b, "");
}

static void trace_line(Cs *c, const char *prefix)
{
    char *disp = (char *)xcalloc((size_t)c->ncol + 1, 1);
    Buf b = {0, 0, 0};
    int k;
    const char *u = c->cur ? c->cur : c->uval;
    for (k = 0; k < c->ncol; k++) disp[k] = out_disp(c, k, u[k] ? u[k] : 'X');
    buf_puts(&b, prefix);
    format_vals(c, disp, &b, 1);
    so_line(c, b.p);
    buf_free(&b);
    free(disp);
}

static void trace_pass(void *ctx, int pass)
{
    Cs *c = (Cs *)ctx;
    (void)pass;
    if (c->trace == 1) trace_line(c, "      ");
}

static void phase(Cs *c)
{
    int guard = 0;
    sim3_settle(&c->sim);
    while (sim3_clock_step(&c->sim) && guard++ < 8) sim3_settle(&c->sim);
}

static void jed_vector(Cs *c, const char *user, const char *disp, int preload, int simoff)
{
    const Device *dv = c->d->dev;
    Buf b = {0, 0, 0};
    char *v = (char *)xmalloc((size_t)dv->npins + 2);
    int p, k;
    for (p = 1; p <= dv->npins; p++) {
        v[p] = 'X';
        /* an output named in an earlier ORDER still shows its value */
        if (c->pinseen[p] && !preload && !simoff && c->simok && sim3_is_output(&c->sim, p)) v[p] = pin_char(sim3_value(&c->sim, p));
    }
    v[0] = 0;
    for (k = 0; k < c->ncol; k++) {
        int pin = c->col[k].pin;
        char u = user[k], ch;
        if (pin < 1 || pin > dv->npins) continue;
        if (preload || simoff) {
            ch = u;
            if (ch == '*') ch = 'X';
            if (ch == 'R') ch = 'X';
            /* preload vectors are written as given; $SIMOFF vectors as pin levels */
            if (ch == '0' || ch == '1') { if (!preload) ch = (char)('0' + ((ch - '0') ^ c->col[k].neg)); }
            else if (ch == 'H' || ch == 'L') ch = ((ch == 'H') ^ c->col[k].neg) ? 'H' : 'L';
        } else if (col_is_output(c, k)) {
            int m = c->col[k].mc;
            if (u == 'N') ch = 'N';
            else if (m >= 0 && c->sim.oe[m] == V0 && (u == '0' || u == '1' || u == 'X'))
                ch = u == 'X' ? 'X' : (char)('0' + ((u - '0') ^ c->col[k].neg));
            else ch = pin_char(sim3_value(&c->sim, pin));
        } else {
            ch = u;
            if (ch == 'R' || ch == '*') ch = 'X';   /* '*': invalid on an input, X (observed) */
            else if (ch == '0' || ch == '1') ch = (char)('0' + ((ch - '0') ^ c->col[k].neg));
            else if (ch == 'H' || ch == 'L') ch = ((ch == 'H') ^ c->col[k].neg) ? 'H' : 'L';
            (void)disp;
        }
        v[pin] = ch;
    }
    /* power and no-connect pins: N (probed table; else the power pins) */
    {
        const CsNcEntry *e;
        for (e = cs_nctable; e->dev; e++) if (!strcmp(e->dev, dv->name)) break;
        if (e->dev) {
            const char *q = e->pins;
            while (*q) {
                int n = atoi(q);
                /* a pin named in an ORDER keeps its value (observed: PC508
                   clock and outputs) */
                if (n >= 1 && n <= dv->npins && !c->pinseen[n]) v[n] = 'N';
                while (*q && *q != ',') q++;
                if (*q) q++;
            }
        } else {
            if (dv->gnd >= 1 && dv->gnd <= dv->npins) v[dv->gnd] = 'N';
            if (dv->vcc >= 1 && dv->vcc <= dv->npins) v[dv->vcc] = 'N';
        }
    }
    for (p = 0; p < c->d->nreserve; p++)   /* power-down pins held low */
        if (c->d->reserve[p] >= 1 && c->d->reserve[p] <= dv->npins) v[c->d->reserve[p]] = '0';
    buf_printf(&b, "*V%04d ", c->vecno);
    for (p = 1; p <= dv->npins; p++) buf_putc(&b, v[p]);
    buf_puts(&c->jv, b.p);
    buf_puts(&c->jv, "\r\n");
    c->njv++;
    buf_free(&b);
    free(v);
}

/* simulate one vector given the user values (one character per column) */
static void run_vector(Cs *c, const char *user)
{
    int k, ne = 0, preload = 0, anyc = 0, anyk = 0, i;
    char *disp = (char *)xcalloc((size_t)c->ncol + 1, 1);
    char *u = xstrdup(user);
    VErr *e = NULL;
    Buf b = {0, 0, 0};
    c->vecno++;
    c->cur = u;
    /* P anywhere in the inputs makes a preload vector (shown and written as
       given, registers loaded); on a pin that is not a clock it is also
       reported invalid (observed) */
    for (k = 0; k < c->ncol; k++) if (u[k] == 'P' && !col_is_output(c, k)) preload = is_clock_pin(c, c->col[k].pin) ? 2 : (preload ? preload : 1);
    for (k = 0; k < c->ncol; k++) if (u[k] == 'R') u[k] = (char)('0' + rnd_bit(c));
    if (c->simoff || !c->simok) {
        for (k = 0; k < c->ncol; k++) disp[k] = u[k];
    } else if (preload) {
        for (k = 0; k < c->ncol; k++) {
            int m = c->col[k].mc;
            disp[k] = u[k];
            if (col_is_output(c, k)) {
                if (u[k] == 'L' || u[k] == 'H') { char a[2] = {u[k], 0}; add_err(&e, &ne, 21, k, a); }
                else if ((u[k] == '0' || u[k] == '1') && m >= 0 && SIM_HASREG(&c->sim.s.mc[m]))
                    sim3_preload(&c->sim, m, u[k] - '0');
            } else if (u[k] == 'P' && c->col[k].pin && !is_clock_pin(c, c->col[k].pin)) {
                char a[2] = {'P', 0};
                add_err(&e, &ne, 21, k, a);
            }
        }
        phase(c);
    } else {
        int p, pfound = 0;
        unsigned char prev[SIM_MAXPINS + 1], nd[SIM_MAXPINS + 1], fired[SIM_MAXPINS + 1];
        memcpy(prev, c->sim.drv, sizeof prev);
        for (p = 0; p <= SIM_MAXPINS; p++) nd[p] = prev[p];
        for (p = 1; p <= c->d->dev->npins; p++) {
            if (p == c->d->dev->gnd || p == c->d->dev->vcc) continue;
            nd[p] = sim3_is_output(&c->sim, p) ? VNONE : VX;
        }
        /* the device OE pin, when the design does not declare it, enables
           the outputs (observed; declared but not in ORDER it is X) */
        if (c->sim.oepin && c->sim.oepin <= SIM_MAXPINS && !sim3_is_output(&c->sim, c->sim.oepin)) {
            int i2, decl = 0;
            for (i2 = 0; i2 < c->d->nsym; i2++) if (c->d->sym[i2].pin == c->sim.oepin) decl = 1;
            if (!decl) nd[c->sim.oepin] = c->d->dev->sim_oe == 2 ? V1 : V0;
        }
        for (k = 0; k < c->ncol; k++) if (u[k] == 'P') pfound = 1;
        for (k = 0; k < c->ncol; k++) {
            int pin = c->col[k].pin, neg = c->col[k].dneg;
            char ch = u[k];
            if (!pin || pin > SIM_MAXPINS) continue;
            if (col_is_output(c, k)) {
                if (ch == '0' || ch == '1') nd[pin] = (unsigned char)((ch - '0') ^ neg);
                else if (ch == 'X') nd[pin] = VX;
                else nd[pin] = VNONE;
                if (ch == 'C' || ch == 'K' || ch == 'P' || ch == 'R' || (pfound && (ch == 'L' || ch == 'H'))) {
                    char a[2] = {ch, 0};
                    add_err(&e, &ne, 21, k, a);
                }
                continue;
            }
            /* a pin without equation that the design does not read: the
               reference keeps the column as written (observed) */
            if (col_noeqn(c, k) == 2) continue;
            switch (ch) {
            case '0': case '1': nd[pin] = (unsigned char)((ch - '0') ^ neg); break;
            case 'X': nd[pin] = VX; break;
            case 'C':   /* C: loads with the clock variable low; not a clock pin: input 0 */
                if (is_clock_pin(c, pin)) { anyc = 1; nd[pin] = (unsigned char)neg; } else nd[pin] = V0;
                break;
            case 'K':   /* K: loads with the clock pin low (observed); not a clock pin: input 1 */
                if (is_clock_pin(c, pin)) { anyk = 1; nd[pin] = V0; } else nd[pin] = V1;
                break;
            case 'P': nd[pin] = V0; break;
            case 'L': case 'H': case 'Z':
                /* not driven: an input keeps its previous value */
                nd[pin] = prev[pin] == VNONE ? VX : prev[pin];
                break;
            default: nd[pin] = VX; break;
            }
            if ((ch == 'C' || ch == 'K' || ch == 'P') && !is_clock_pin(c, pin)) { char a[2] = {ch, 0}; add_err(&e, &ne, 21, k, a); }
            else if (!strchr("01XCKLHZRP", ch)) { char a[2] = {ch, 0}; add_err(&e, &ne, 21, k, a); }
        }
        {
            /* new inputs first with the clock pins at their old level, then the
               new level.  C/K pulse the pin after the evaluation; registers
               on the clock pin load at most once per vector. */
            int lvl = 0, pre = c->trace >= 2;
            for (p = 1; p <= SIM_MAXPINS; p++) {
                if (c->clockish[p] && nd[p] != prev[p] && nd[p] != VNONE && prev[p] != VNONE &&
                    !(anyc || anyk)) { sim3_drive(&c->sim, p, prev[p]); lvl = 1; }
                else sim3_drive(&c->sim, p, nd[p]);
            }
            if (anyc || anyk) {
                /* (registers on another clock pin keep their own edges) */
                memset(fired, 0, sizeof fired);
                for (k = 0; k < c->ncol; k++)
                    if (c->col[k].pin && c->col[k].pin <= SIM_MAXPINS && !col_is_output(c, k) && is_clock_pin(c, c->col[k].pin) && (u[k] == 'K' || u[k] == 'C'))
                        fired[c->col[k].pin] = 1;
                c->sim.firepins = fired;
                c->sim.pinclk_off = 1;
                phase(c);
                if (pre) { so_line(c, "****** Before Clock ******"); trace_line(c, "      "); }
                /* as the original: C and K act on the pin whatever the
                   polarity of the variable.  C loads only when the pin was
                   low after the previous vector; K always loads. */
                {
                    int fire = 0;
                    for (k = 0; k < c->ncol; k++)
                        if (c->col[k].pin && !col_is_output(c, k) && is_clock_pin(c, c->col[k].pin) &&
                            (u[k] == 'K' || (u[k] == 'C' && prev[c->col[k].pin] != V1))) fire = 1;
                    if (fire) sim3_clock_force(&c->sim);
                }
                for (k = 0; k < c->ncol; k++)
                    if (c->col[k].pin && (u[k] == 'C' || u[k] == 'K') && !col_is_output(c, k) && is_clock_pin(c, c->col[k].pin))
                        sim3_drive(&c->sim, c->col[k].pin, V1);
                phase(c);
                if (pre) {
                    Buf r = {0, 0, 0};
                    char *rd = (char *)xcalloc((size_t)c->ncol + 1, 1);
                    so_line(c, "****** At Clock ******");
                    for (k = 0; k < c->ncol; k++) {
                        int m = c->col[k].mc;
                        rd[k] = (m >= 0 && sim3_is_reg(&c->sim, m)) ? (c->sim.q[m] == VX ? 'X' : (char)('0' + c->sim.q[m])) : '0';
                    }
                    buf_puts(&r, "REGS  ");
                    format_vals(c, rd, &r, 1);
                    so_line(c, r.p);
                    buf_free(&r);
                    free(rd);
                }
                for (k = 0; k < c->ncol; k++)
                    if (c->col[k].pin && (u[k] == 'C' || u[k] == 'K') && !col_is_output(c, k) && is_clock_pin(c, c->col[k].pin))
                        sim3_drive(&c->sim, c->col[k].pin, u[k] == 'K' ? V1 : V0);
                phase(c);
                c->sim.pinclk_off = 0;
                c->sim.firepins = NULL;
                sim3_clock_sync(&c->sim);
                if (pre) { so_line(c, "****** After Clock ******"); trace_line(c, "      "); }
            } else {
                /* the intermediate step only detects clock edges; AR/AP
                   terms see the inputs of the whole vector (as the original) */
                c->sim.noasync = lvl;
                phase(c);
                c->sim.noasync = 0;
                if (lvl) {
                    for (p = 1; p <= SIM_MAXPINS; p++) sim3_drive(&c->sim, p, nd[p]);
                    phase(c);
                }
            }
        }
        for (k = 0; k < c->ncol; k++) disp[k] = out_disp(c, k, u[k]);
        {
            /* the unstable outputs are reported once per 20 passes of the
               loop iteration (40 or 100 passes), in ORDER (observed) */
            int r, rounds = c->sim.budget / 20;
            for (r = 0; r < rounds; r++)
                for (k = 0; k < c->ncol; k++) {
                    int m = c->col[k].mc;
                    if (col_is_output(c, k) && m >= 0 && c->sim.unst[m]) add_err(&e, &ne, 20, k, c->col[k].name);
                }
        }
        for (k = 0; k < c->ncol; k++) {
            char ch = u[k], a[64];
            int m = c->col[k].mc, oe;
            if (!col_is_output(c, k)) continue;
            oe = m >= 0 ? c->sim.oe[m] : V1;
            if (ch == 'N' || ch == '*') continue;
            if ((ch == '0' || ch == '1' || ch == 'X') && oe == V0) continue;
            if (ch == '0' || ch == '1' || ch == 'C' || ch == 'K' || ch == 'P' || ch == 'R' || disp[k] != ch) {
                snprintf(a, sizeof a, "(%c) for %s", ch, c->col[k].name);
                add_err(&e, &ne, 19, k, a);
            }
        }
    }
    buf_printf(&b, "%04d: ", c->vecno);
    format_vals(c, disp, &b, 1);
    if (c->trace == 4 && c->simok && !preload && !c->simoff) {
        char *pb = (char *)xcalloc((size_t)c->ncol + 1, 1);
        Buf r = {0, 0, 0};
        for (k = 0; k < c->ncol; k++) {
            int pin = c->col[k].pin;
            if (!col_is_output(c, k) || pin > c->d->dev->npins) pb[k] = '?';
            else {
                int v = sim3_prebuffer(&c->sim, pin);
                pb[k] = v == VX ? 'X' : v ? 'H' : 'L';
            }
        }
        so_line(c, "");
        so_line(c, "****** before output buffer ******");
        buf_puts(&r, "      ");
        format_vals(c, pb, &r, 1);
        so_line(c, r.p);
        buf_free(&r);
        free(pb);
    }
    so_line(c, b.p);
    buf_free(&b);
    {
        Buf w = {0, 0, 0};
        buf_printf(&w, "#V %04d ", c->vecno);
        format_vals(c, disp, &w, 0);
        buf_puts(&c->wo, w.p);
        buf_puts(&c->wo, "\r\n");
        buf_free(&w);
    }
    if (ne) {
        /* caret line, then 0021, 0020, 0019 */
        char *cl = (char *)xmalloc((size_t)c->vw + 8);
        int last = -1, order[3] = {21, 20, 19}, o;
        memset(cl, ' ', (size_t)c->vw + 7);
        for (i = 0; i < ne; i++) {
            int pos = c->col[e[i].col].pos;
            cl[6 + pos] = '^';
            if (6 + pos > last) last = 6 + pos;
        }
        cl[last + 1] = 0;
        so_line(c, cl);
        free(cl);
        for (o = 0; o < 3; o++)
            for (i = 0; i < ne; i++) {
                if (e[i].code != order[o]) continue;
                so_printf(c, "[%04dsa] %s%s", e[i].code, msg_text(e[i].code), e[i].arg);
                if (e[i].code == 19) {
                    char ex = '?';
                    const char *sp = strchr(e[i].arg, ')');
                    if (e[i].arg[0] == '(') ex = e[i].arg[1];
                    wo_printf(c, "#E 0019 %c %s", ex, sp && sp[1] == ' ' && sp[2] ? sp + 6 : "");
                } else wo_printf(c, "#E %04d %s", e[i].code, e[i].arg);
                c->nerr++;
            }
        so_line(c, "");
        wo_printf(c, "%s", "");
    }
    free(e);
    jed_vector(c, user, disp, preload, c->simoff);
    c->cur = NULL;
    memcpy(c->lastdisp, disp, (size_t)c->ncol);
    memcpy(c->uval, user, (size_t)c->ncol);
    free(u);
    free(disp);
}

/* tokenizer over a directive's text */
typedef struct { const char *s; int i; } DLex;

static int dlex(DLex *x, char *out, int max, int *pos)
{
    int n = 0;
    while (x->s[x->i] && isspace((unsigned char)x->s[x->i])) x->i++;
    if (pos) *pos = x->i;
    if (!x->s[x->i]) { out[0] = 0; return 0; }
    if (word_char((unsigned char)x->s[x->i])) {
        while (word_char((unsigned char)x->s[x->i]) && n < max - 1) out[n++] = x->s[x->i++];
        out[n] = 0;
        return 'a';
    }
    if (x->s[x->i] == '"' || x->s[x->i] == '\'') {
        int q = x->s[x->i];
        out[n++] = x->s[x->i++];
        while (x->s[x->i] && x->s[x->i] != q && n < max - 2) out[n++] = x->s[x->i++];
        if (x->s[x->i] == q) out[n++] = x->s[x->i++];
        out[n] = 0;
        return q;
    }
    if (x->s[x->i] == '.' && x->s[x->i + 1] == '.') { strcpy(out, ".."); x->i += 2; return 'r'; }
    if ((x->s[x->i] == '>' || x->s[x->i] == '<') && x->s[x->i + 1] == '=') {
        out[0] = x->s[x->i]; out[1] = '='; out[2] = 0; x->i += 2; return out[0] == '>' ? 'G' : 'S';
    }
    out[0] = x->s[x->i++];
    out[1] = 0;
    return (unsigned char)out[0];
}

/* columns of a $SET/$COMP/$IF variable: signal, field or VAR */
static int var_cols(Cs *c, const char *name, int **cols)
{
    NameList nl = {0, 0};
    int i, j, n = 0;
    *cols = NULL;
    if (!group_members(c, name, &nl)) nl_add(&nl, name);
    *cols = (int *)xcalloc((size_t)nl.n + 1, sizeof(int));
    for (i = 0; i < nl.n; i++)
        for (j = 0; j < c->ncol; j++)
            if (str_ieq(c->col[j].name, nl.v[i])) { (*cols)[n++] = j; break; }
    nl_free(&nl);
    return n;
}

static unsigned long cols_value(const Cs *c, const char *vals, const int *cols, int n, int *hasx)
{
    unsigned long v = 0;
    int i;
    *hasx = 0;
    for (i = 0; i < n; i++) {
        char ch = vals[cols[i]];
        v <<= 1;
        if (ch == '1' || ch == 'H') v |= 1;
        else if (ch != '0' && ch != 'L') *hasx = 1;
    }
    return v;
}

static void set_cols_value(Cs *c, const int *cols, int n, unsigned long v)
{
    int i;
    for (i = 0; i < n; i++) {
        int bit = (int)((v >> (n - 1 - i)) & 1);
        c->uval[cols[i]] = col_is_output(c, cols[i]) ? (bit ? 'H' : 'L') : (char)('0' + bit);
    }
}

/* constant of $SET/$IF: quoted value or test-value string */
static int const_chars(Cs *c, const char *tok, int n, char *buf)
{
    if (tok[0] == '\'' || tok[0] == '"') {
        char q[128];
        size_t l = strlen(tok);
        snprintf(q, sizeof q, "%.*s", (int)(l >= 2 ? l - 2 : 0), tok + 1);
        return expand_quoted(q, tok[0] == '"', c->base, n, buf, n);
    }
    {
        int i, k = 0;
        for (i = 0; tok[i] && k < n; i++) if (!isspace((unsigned char)tok[i])) buf[k++] = (char)toupper((unsigned char)tok[i]);
        return k;
    }
}

/* expression of $COMP: numbers and variables, ! & # $ * / + - ( ) */
typedef struct { Cs *c; DLex x; char tok[256]; int tt; int err; } Ex;

static void ex_next(Ex *e) { e->tt = dlex(&e->x, e->tok, (int)sizeof e->tok, NULL); }
static long ex_or(Ex *e);

static long ex_prim(Ex *e)
{
    long v = 0;
    if (e->tt == '(') { ex_next(e); v = ex_or(e); if (e->tt == ')') ex_next(e); else e->err = 1; return v; }
    if (e->tt == '!') { ex_next(e); return ~ex_prim(e); }
    if (e->tt == '-') { ex_next(e); return -ex_prim(e); }
    if (e->tt == 'a') {
        if (isdigit((unsigned char)e->tok[0])) v = strtol(e->tok, NULL, 10);
        else {
            int *cols, n = var_cols(e->c, e->tok, &cols), hx;
            if (n == 0) e->err = 1;
            v = (long)cols_value(e->c, e->c->uval, cols, n, &hx);
            free(cols);
        }
        ex_next(e);
        return v;
    }
    e->err = 1;
    return 0;
}

static long ex_and(Ex *e) { long v = ex_prim(e); while (e->tt == '&') { ex_next(e); v &= ex_prim(e); } return v; }
static long ex_orl(Ex *e) { long v = ex_and(e); while (e->tt == '#') { ex_next(e); v |= ex_and(e); } return v; }
static long ex_xor(Ex *e) { long v = ex_orl(e); while (e->tt == '$') { ex_next(e); v ^= ex_orl(e); } return v; }
static long ex_mul(Ex *e)
{
    long v = ex_xor(e);
    while (e->tt == '*' || e->tt == '/') {
        int op = e->tt;
        long w;
        ex_next(e);
        w = ex_xor(e);
        if (op == '*') v *= w;
        else v = w ? v / w : 0;
    }
    return v;
}
static long ex_or(Ex *e)
{
    long v = ex_mul(e);
    while (e->tt == '+' || e->tt == '-') {
        int op = e->tt;
        long w;
        ex_next(e);
        w = ex_mul(e);
        v = op == '+' ? v + w : v - w;
    }
    return v;
}

/* condition of $IF/$WHILE/$UNTIL on the last simulated values */
static int eval_cond(Cs *c, const char *text, int *err)
{
    DLex x = {text, 0};
    char name[256], op[8], val[256];
    int *cols, n, hx, cmp;
    unsigned long a, b;
    char buf[256];
    *err = 0;
    if (dlex(&x, name, sizeof name, NULL) != 'a') { *err = 1; return 0; }
    dlex(&x, op, sizeof op, NULL);
    while (x.s[x.i] && isspace((unsigned char)x.s[x.i])) x.i++;
    snprintf(val, sizeof val, "%s", x.s + x.i);
    {
        size_t l = strlen(val);
        while (l > 0 && (isspace((unsigned char)val[l - 1]) || val[l - 1] == ':')) val[--l] = 0;
    }
    n = var_cols(c, name, &cols);
    if (n == 0) { free(cols); *err = 1; return 0; }
    a = cols_value(c, c->lastdisp, cols, n, &hx);
    {
        int k = const_chars(c, val, n, buf), i;
        int ix[64];
        for (i = 0; i < k && i < 64; i++) ix[i] = i;
        b = 0;
        for (i = 0; i < k; i++) { b <<= 1; if (buf[i] == '1' || buf[i] == 'H') b |= 1; }
        (void)ix;
    }
    free(cols);
    cmp = a < b ? -1 : a > b ? 1 : 0;
    if (!strcmp(op, "=")) return cmp == 0;
    if (!strcmp(op, "#")) return cmp != 0;
    if (!strcmp(op, ">")) return cmp > 0;
    if (!strcmp(op, "<")) return cmp < 0;
    if (!strcmp(op, ">=")) return cmp >= 0;
    if (!strcmp(op, "<=")) return cmp <= 0;
    *err = 1;
    return 0;
}

typedef struct Macro { char *name; char **arg; int narg; Stmt *body; int nbody; } Macro;

typedef struct {
    Macro *m; int nm;
    int repeat;
    int depth;
} Exec;

static void exec_stmts(Cs *c, Exec *x, Stmt *st, int n);

/* directive name and the rest */
static void dir_split(const char *text, char *name, size_t nn, const char **rest)
{
    int i = 1, k = 0;
    while (text[i] && isspace((unsigned char)text[i])) i++;
    while (word_char((unsigned char)text[i]) && k < (int)nn - 1) name[k++] = text[i++];
    name[k] = 0;
    *rest = text + i;
}

static int block_end(Stmt *st, int n, int i, const char *open, const char *const *close, int nclose, int *mid, const char *midname)
{
    int depth = 0, j;
    if (mid) *mid = -1;
    for (j = i + 1; j < n; j++) {
        char name[32];
        const char *rest;
        int k;
        if (st[j].kind != ST_DIR) continue;
        dir_split(st[j].text, name, sizeof name, &rest);
        if (str_ieq(name, open)) { depth++; continue; }
        for (k = 0; k < nclose; k++)
            if (str_ieq(name, close[k])) {
                if (depth == 0) return j;
                depth--;
                break;
            }
        if (k == nclose && depth == 0 && mid && midname && str_ieq(name, midname) && *mid < 0) *mid = j;
    }
    return -1;
}

static void dir_error(Cs *c, const Stmt *s, int off, int code, const char *arg)
{
    print_src(c, s->line);
    line_error(c, off >= 0 ? s->col + off : -1, code, arg);
}

static void do_order(Cs *c, const Stmt *s)
{
    int l;
    for (l = s->line; l <= s->line1 && l < c->nline; l++) {
        print_src(c, l);
        wo_printf(c, "#H %s", c->line[l]);
    }
    c->listing = 0;
    c->ln = s->line;
    c->cp = s->col;
    {
        Tok t;
        take(c, &t);
        parse_order(c);
    }
    /* the following lines are listed up to VECTORS, then the table header */
    c->hdr_pending = !c->fatal;
    c->echo_from = s->line1 + 1;
}

static void flush_header(Cs *c, int upto)
{
    int l;
    if (!c->hdr_pending) return;
    for (l = c->echo_from; l < upto && l < c->nline; l++) {
        print_src(c, l);
        wo_printf(c, "#H %s", c->line[l]);
    }
    c->hdr_pending = 0;
    table_header(c);
}

static void do_vector(Cs *c, Exec *x, const Stmt *s)
{
    VecVals vv;
    int r, times;
    if (!c->ncol && !c->nit) return;
    parse_values(c, s->text, &vv);
    if (vv.nv == 0 && vv.nx == 0) { free_vals(&vv); return; }
    if (vv.nx) {
        Buf b = {0, 0, 0};
        int i, last = 0;
        print_src(c, s->line);
        for (i = 0; i < vv.nx; i++) if (s->col + vv.xpos[i] > last) last = s->col + vv.xpos[i];
        for (i = 0; i < 6 + last + 1; i++) buf_putc(&b, ' ');
        for (i = 0; i < vv.nx; i++) b.p[6 + s->col + vv.xpos[i]] = '^';
        b.p[6 + last + 1] = 0;
        so_line(c, b.p);
        buf_free(&b);
        for (i = 0; i < vv.nx; i++) {
            char a[2] = {vv.xval[i], 0};
            so_printf(c, "[0013sa] %s%s", msg_text(13), a);
            wo_printf(c, "#E 0013 %s", a);
            c->nerr++;
        }
        so_line(c, "");
        wo_printf(c, "%s", "");
    }
    if (vv.nv < c->ncol) {
        so_printf(c, "[0014sa] %s", msg_text(14));
        wo_printf(c, "#E 0014 ");
        c->nerr++;
        c->fatal = 1;
        free_vals(&vv);
        return;
    }
    times = x->repeat > 0 ? x->repeat : 1;
    x->repeat = 0;
    for (r = 0; r < times && !c->fatal; r++) run_vector(c, vv.v);
    free_vals(&vv);
}

static void exec_dir(Cs *c, Exec *x, Stmt *st, int n, int *pi)
{
    Stmt *s = &st[*pi];
    char name[32];
    const char *rest;
    dir_split(s->text, name, sizeof name, &rest);
    if (str_ieq(name, "msg")) {
        const char *q = strchr(rest, '"'), *e = q ? strrchr(q + 1, '"') : NULL;
        if (q && e && e > q) {
            char *m = xstrndup(q + 1, (size_t)(e - q - 1));
            so_line(c, m);
            free(m);
        } else so_line(c, "");
    } else if (str_ieq(name, "repeat")) {
        x->repeat = atoi(rest);
    } else if (str_ieq(name, "trace")) {
        int v = atoi(rest), off = (int)(rest - s->text);
        while (s->text[off] && isspace((unsigned char)s->text[off])) off++;
        if (v < 0 || v > 4 || !isdigit((unsigned char)s->text[off])) {
            char a[32];
            snprintf(a, sizeof a, "%d", v);
            dir_error(c, s, off, 29, a);
        } else c->trace = v;
    } else if (str_ieq(name, "simoff")) c->simoff = 1;
    else if (str_ieq(name, "simon")) c->simoff = 0;
    else if (str_ieq(name, "exit")) { c->stop = 1; so_line(c, ""); }
    else if (str_ieq(name, "set") || str_ieq(name, "comp")) {
        DLex d = {rest, 0};
        char v[256], eq[8];
        int *cols, nc;
        if (dlex(&d, v, sizeof v, NULL) != 'a' || dlex(&d, eq, sizeof eq, NULL) != '=') { dir_error(c, s, -1, 9, v); return; }
        nc = var_cols(c, v, &cols);
        if (nc == 0) { dir_error(c, s, -1, 11, v); free(cols); return; }
        if (str_ieq(name, "set")) {
            char buf[256];
            int k, i;
            while (d.s[d.i] && isspace((unsigned char)d.s[d.i])) d.i++;
            k = const_chars(c, d.s + d.i, nc, buf);
            for (i = 0; i < k && i < nc; i++) c->uval[cols[i]] = buf[i];
        } else {
            Ex e;
            long val;
            memset(&e, 0, sizeof e);
            e.c = c;
            e.x = d;
            ex_next(&e);
            val = ex_or(&e);
            if (e.err) dir_error(c, s, -1, 9, e.tok);
            else set_cols_value(c, cols, nc, (unsigned long)val);
        }
        free(cols);
    } else if (str_ieq(name, "out")) {
        int times = x->repeat > 0 ? x->repeat : 1, r;
        x->repeat = 0;
        for (r = 0; r < times; r++) run_vector(c, c->uval);
    } else if (str_ieq(name, "if")) {
        static const char *const cl[] = {"endif"};
        int mid, end = block_end(st, n, *pi, "if", cl, 1, &mid, "else"), err, v;
        if (end < 0) { dir_error(c, s, -1, 9, "if"); c->fatal = 1; return; }
        v = eval_cond(c, rest, &err);
        if (err) dir_error(c, s, -1, 9, rest);
        if (v) exec_stmts(c, x, st + *pi + 1, (mid >= 0 ? mid : end) - *pi - 1);
        else if (mid >= 0) exec_stmts(c, x, st + mid + 1, end - mid - 1);
        *pi = end;
    } else if (str_ieq(name, "for")) {
        static const char *const cl[] = {"endf"};
        int end = block_end(st, n, *pi, "for", cl, 1, NULL, NULL), a, b, i;
        DLex d = {rest, 0};
        char t1[64], t2[8], t3[64], t4[8], t5[64];
        if (end < 0) { dir_error(c, s, -1, 9, "for"); c->fatal = 1; return; }
        dlex(&d, t1, sizeof t1, NULL);
        dlex(&d, t2, sizeof t2, NULL);
        dlex(&d, t3, sizeof t3, NULL);
        dlex(&d, t4, sizeof t4, NULL);
        dlex(&d, t5, sizeof t5, NULL);
        a = atoi(t3);
        b = atoi(t5);
        for (i = a;; i += a <= b ? 1 : -1) {
            exec_stmts(c, x, st + *pi + 1, end - *pi - 1);
            if (i == b || c->fatal || c->stop) break;
        }
        *pi = end;
    } else if (str_ieq(name, "while")) {
        static const char *const cl[] = {"endw"};
        int end = block_end(st, n, *pi, "while", cl, 1, NULL, NULL), err, guard = 0;
        if (end < 0) { dir_error(c, s, -1, 9, "while"); c->fatal = 1; return; }
        while (eval_cond(c, rest, &err) && !err && guard++ < 10000 && !c->fatal && !c->stop)
            exec_stmts(c, x, st + *pi + 1, end - *pi - 1);
        *pi = end;
    } else if (str_ieq(name, "do")) {
        static const char *const cl[] = {"until"};
        int end = block_end(st, n, *pi, "do", cl, 1, NULL, NULL), err, guard = 0;
        char nm2[32];
        const char *r2;
        if (end < 0) { dir_error(c, s, -1, 9, "do"); c->fatal = 1; return; }
        dir_split(st[end].text, nm2, sizeof nm2, &r2);
        do exec_stmts(c, x, st + *pi + 1, end - *pi - 1);
        while (!eval_cond(c, r2, &err) && !err && guard++ < 10000 && !c->fatal && !c->stop);
        *pi = end;
    } else if (str_ieq(name, "macro")) {
        static const char *const cl[] = {"mend"};
        int end = block_end(st, n, *pi, "macro", cl, 1, NULL, NULL), i;
        DLex d = {rest, 0};
        char t[64];
        Macro *m;
        if (end < 0) { dir_error(c, s, -1, 9, "macro"); c->fatal = 1; return; }
        x->m = (Macro *)xrealloc(x->m, sizeof(Macro) * (size_t)(x->nm + 1));
        m = &x->m[x->nm++];
        memset(m, 0, sizeof *m);
        dlex(&d, t, sizeof t, NULL);
        m->name = xstrdup(t);
        while (dlex(&d, t, sizeof t, NULL)) {
            if (t[0] == '(' || t[0] == ',' || t[0] == ')') continue;
            m->arg = (char **)xrealloc(m->arg, sizeof(char *) * (size_t)(m->narg + 1));
            m->arg[m->narg++] = xstrdup(t);
        }
        m->body = st + *pi + 1;
        m->nbody = end - *pi - 1;
        (void)i;
        *pi = end;
    } else if (str_ieq(name, "call")) {
        DLex d = {rest, 0};
        char t[64];
        int i, k, na = 0;
        char *act[16];
        Macro *m = NULL;
        dlex(&d, t, sizeof t, NULL);
        for (i = 0; i < x->nm; i++) if (str_ieq(x->m[i].name, t)) m = &x->m[i];
        if (!m || x->depth > 10) { dir_error(c, s, -1, 9, t); return; }
        {
            const char *p = strchr(rest, '(');
            int depth = 0;
            Buf b = {0, 0, 0};
            if (p) p++;
            while (p && *p && na < 16) {
                if (*p == '(') depth++;
                if ((*p == ',' && depth == 0) || (*p == ')' && depth == 0)) {
                    act[na++] = b.p ? b.p : xstrdup("");
                    b.p = NULL; b.n = b.cap = 0;
                    if (*p == ')') break;
                    p++;
                    continue;
                }
                if (*p == ')') depth--;
                buf_putc(&b, *p++);
            }
            buf_free(&b);
        }
        {
            Stmt *body = (Stmt *)xcalloc((size_t)m->nbody + 1, sizeof(Stmt));
            for (k = 0; k < m->nbody; k++) {
                Buf b = {0, 0, 0};
                const char *p = m->body[k].text;
                body[k] = m->body[k];
                while (*p) {
                    if (word_char((unsigned char)*p) && (p == m->body[k].text || !word_char((unsigned char)p[-1]))) {
                        int l = 0, j;
                        while (word_char((unsigned char)p[l])) l++;
                        for (j = 0; j < m->narg && j < na; j++)
                            if ((int)strlen(m->arg[j]) == l && str_nieq(p, m->arg[j], (size_t)l)) break;
                        if (j < m->narg && j < na) {
                            const char *a = act[j];
                            while (*a && isspace((unsigned char)*a)) a++;
                            buf_puts(&b, a);
                            p += l;
                            continue;
                        }
                        buf_puts(&b, "");
                        while (l-- > 0) buf_putc(&b, *p++);
                        continue;
                    }
                    buf_putc(&b, *p++);
                }
                body[k].text = b.p ? b.p : xstrdup("");
            }
            x->depth++;
            exec_stmts(c, x, body, m->nbody);
            x->depth--;
            for (k = 0; k < m->nbody; k++) free(body[k].text);
            free(body);
        }
        for (i = 0; i < na; i++) free(act[i]);
    } else if (str_ieq(name, "else") || str_ieq(name, "endif") || str_ieq(name, "endf") || str_ieq(name, "endw") ||
               str_ieq(name, "until") || str_ieq(name, "mend")) {
        dir_error(c, s, -1, 9, name);
    } else {
        so_printf(c, "[0009sa] %s%s", msg_text(9), "bad number");
        wo_printf(c, "#E 0009 bad number");
        c->nerr++;
        c->fatal = 1;
    }
}

static void exec_stmts(Cs *c, Exec *x, Stmt *st, int n)
{
    int i;
    for (i = 0; i < n && !c->fatal && !c->stop; i++) {
        switch (st[i].kind) {
        case ST_VEC: flush_header(c, st[i].line); do_vector(c, x, &st[i]); break;
        case ST_DIR: flush_header(c, st[i].line); exec_dir(c, x, st, n, &i); break;
        case ST_ORDER: flush_header(c, st[i].line); do_order(c, &st[i]); break;
        default:
            if (str_ieq(st[i].text, "vectors") || str_ieq(st[i].text, "vector")) flush_header(c, st[i].line);
            break;
        }
    }
}

static int jed_append(Cs *c, const char *path)
{
    size_t len, i;
    char *b = read_file(path, &len), *qf, *etx, *nl;
    Buf o = {0, 0, 0};
    unsigned sum = 0;
    int p;
    if (!b) return -1;
    qf = strstr(b, "*QF");
    etx = memchr(b, 0x03, len);
    if (!qf || !etx || etx == b || etx[-1] != '*') { free(b); return -1; }
    nl = strchr(qf, '\n');
    if (!nl || nl > etx) { free(b); return -1; }
    o.p = NULL;
    for (i = 0; b + i <= nl; i++) buf_putc(&o, b[i]);
    buf_printf(&o, "*QV%d\r\n", c->njv);
    for (; b + i < etx - 1; i++) buf_putc(&o, b[i]);
    buf_puts(&o, "*P");
    for (p = 1; p <= c->d->dev->npins; p++) buf_printf(&o, " %d", p);
    buf_puts(&o, "\r\n");
    if (c->jv.p) buf_puts(&o, c->jv.p);
    buf_putc(&o, '*');
    buf_putc(&o, 0x03);
    {
        char *stx = memchr(o.p, 0x02, o.n);
        size_t k;
        for (k = stx ? (size_t)(stx - o.p) : 0; k < o.n; k++) sum += (unsigned char)o.p[k];
    }
    buf_printf(&o, "%04X", sum & 0xFFFF);
    p = write_file(path, o.p, o.n);
    buf_free(&o);
    free(b);
    return p;
}

static void split_lines(Cs *c, const char *buf, size_t len)
{
    size_t i = 0, s = 0;
    for (;;) {
        if (i == len || buf[i] == '\n') {
            size_t e = i;
            char *l;
            if (e > s && buf[e - 1] == '\r') e--;
            l = xstrndup(buf + s, e - s);
            {
                char *z = strchr(l, 0x1a);
                if (z) *z = 0;
            }
            c->line = (char **)xrealloc(c->line, sizeof(char *) * (size_t)(c->nline + 1));
            c->line[c->nline++] = l;
            if (i == len) break;
            s = i + 1;
        }
        i++;
    }
    if (c->nline > 1 && c->line[c->nline - 1][0] == 0 && len > 0 && buf[len - 1] == '\n') {
        free(c->line[--c->nline]);
    }
}

static int run_one(const CsDesign *d, const CsOpts *o, Buf *keep)
{
    Cs cs, *c = &cs;
    size_t len;
    char *buf = read_file(o->si_path, &len), err[256];
    int i, rc;
    Stmt *st = NULL;
    int nst = 0;
    Exec x;
    if (!buf) return 2;
    memset(c, 0, sizeof *c);
    memset(&x, 0, sizeof x);
    c->d = d;
    c->refdef = o->refdefect;
    c->o = o;
    c->base = 16;
    c->seed = 1;
    rnd_bit(c);   /* the reference draws one value before the first R */
    c->simok = sim3_init(&c->sim, d->dev, d->fuse, err, sizeof err) == 0;
    {
        const Device *dv = d->dev;
        int k, m;
        if (dv->clk_pin > 0 && dv->clk_pin <= SIM_MAXPINS) c->pinused[dv->clk_pin] = 1;
        if (dv->oe_pin > 0 && dv->oe_pin <= SIM_MAXPINS) c->pinused[dv->oe_pin] = 1;
        for (m = 0; m < dv->nmc; m++) if (!dv->mc[m].node && dv->mc[m].pin <= SIM_MAXPINS) c->pinused[dv->mc[m].pin] = 1;
        for (m = 0; m < dv->nmodes; m++) {
            const LineSrc *ls = dev_lines(dv, m);
            if (ls) for (k = 0; k < dv->nlines; k++) if (ls[k].kind == SRC_PIN && ls[k].pin <= SIM_MAXPINS) c->pinused[ls[k].pin] = 1;
        }
        if (c->simok) {
            if (dv->clk_pin > 0 && dv->clk_pin <= SIM_MAXPINS) c->clockish[dv->clk_pin] = 1;
            for (m = 0; m < dv->nmc; m++) {
                const McCfg *mc = &c->sim.s.mc[m];
                if (mc->oe_kind == OE_PIN || mc->oe_pin_too) {
                    if (mc->kind == MK_COMB || mc->kind == MK_REG) c->oepin_used = 1;
                }
                if (SIM_HASREG(mc) && mc->ce && !dv->clk_pin) c->clockish[mc->ck_pin > 0 && mc->ck_pin <= SIM_MAXPINS ? mc->ck_pin : 1] = 1;
                if (SIM_HASREG(mc) && mc->ck_row >= 0 && !mc->ce)
                    for (k = 0; k < dv->nlines; k++)
                        if (c->sim.s.lines[k].kind == SRC_PIN && sim_row_uses_line(&c->sim.s, mc->ck_row, k) && c->sim.s.lines[k].pin <= SIM_MAXPINS)
                            c->clockish[c->sim.s.lines[k].pin] = 1;
            }
        }
    }
    if (c->simok) {
        int j2, m3;
        for (j2 = 0; j2 < d->nnoeqn; j2++)   /* buried nodes without an equation read X */
            if ((m3 = dev_mc_at(d->dev, d->noeqn[j2])) >= 0 && d->dev->mc[m3].node) c->sim.fbunk[m3] = 1;
        c->sim.trace = trace_pass; c->sim.trace_ctx = c;
        c->sim.simlatch = o->simlatch;
        c->sim.regzero = o->refdefect && d->dev->sim_regzero;
        c->sim.noce = o->refdefect;
        if (o->refdefect && d->dev->sim_inlevel)
            for (j2 = 0; j2 < d->nfbinv; j2++)
                if ((m3 = dev_mc_at(d->dev, d->fbinv[j2])) >= 0 && !d->dev->mc[m3].node) c->sim.fbinv[m3] = 1;
        if (o->refdefect) {
            /* WinCUPL's simulator clocks a register without .CK from its
               clock pin although the clock term in the fuse map is left
               unprogrammed (B036) */
            int m2, cc;
            for (m2 = 0; m2 < d->dev->nmc; m2++) {
                const Macrocell *k = &d->dev->mc[m2];
                McCfg *mc = &c->sim.s.mc[m2];
                int used = 0;
                if (k->cksync_fuse < 0 || k->ck_row < 0 || mc->ce || !k->ck_pin) continue;
                for (cc = 0; cc < d->dev->ncols && !used; cc++) {
                    int a = dev_fuse(d->dev, k->ck_row, cc);
                    if (a >= 0 && d->fuse[a]) used = 1;
                }
                if (used) continue;
                mc->ce = 1;
                if (k->ck_pin <= SIM_MAXPINS) c->clockish[k->ck_pin] = 1;
            }
        }
        if (o->refdefect && d->dev->sim_oepin) c->sim.oepin = d->dev->sim_oepin;
        if (o->refdefect && d->dev->sim_regoe) {
            /* the reference simulator works from the equations: registered
               outputs and outputs without .OE are always enabled (PEEL) */
            int m2, j;
            for (m2 = 0; m2 < d->dev->nmc; m2++) {
                int has = 0;
                for (j = 0; j < d->noepin; j++) if (d->oepin[j] == d->dev->mc[m2].pin) has = 1;
                if (c->sim.s.mc[m2].kind == MK_REG || !has) c->sim.oeforce[m2] = 1;
            }
        }
    }
    /* device-wide AR / SP terms (22V10): the physical chip applies them to
       every register; the reference simulator only to the registers with an
       .AR / .SP equation (reproduced with -compat-refdefect) */
    if (c->simok && d->ctlinfo && o->refdefect) {
        int m2, j;
        memset(c->sim.gar, 0, (size_t)d->dev->nmc + 1);
        memset(c->sim.gsp, 0, (size_t)d->dev->nmc + 1);
        for (j = 0; j < d->narpin; j++) if ((m2 = sim3_mc_of(&c->sim, d->arpin[j])) >= 0) c->sim.gar[m2] = 1;
        for (j = 0; j < d->nsppin; j++) if ((m2 = sim3_mc_of(&c->sim, d->sppin[j])) >= 0) c->sim.gsp[m2] = 1;
    }
    split_lines(c, buf, len);
    free(buf);
    for (i = 0; i < 4; i++) so_line(c, o->banner[i] ? o->banner[i] : "");
    if (csim_unverified(d->dev)) {
        /* a part model copied from the reference simulator, not verified
           against the data sheet (docs/SIMFMT.txt) */
        char up[40];
        size_t k;
        snprintf(up, sizeof up, "%s", d->mnemonic);
        for (k = 0; up[k]; k++) up[k] = (char)toupper((unsigned char)up[k]);
        so_printf(c, "%s%s%s", UNVERIFIED_NOTE1, up, UNVERIFIED_NOTE2);
        if (o->so_path && !o->quiet) fprintf(stderr, "%s%s%s\n", UNVERIFIED_NOTE1, up, UNVERIFIED_NOTE2);
    }
    so_line(c, "");
    so_printf(c, "LISTING FOR SIMULATION FILE: %s", o->si_name);
    so_line(c, "");
    buf_puts(&c->wo, "#WAVEFORM\r\n");
    c->listing = 1;
    if (parse_decls(c) && !c->fatal) {
        c->listing = 0;
        if (c->nit) table_header(c);
        st = split_stmts(c, &nst);
        exec_stmts(c, &x, st, nst);
    }
    for (i = 0; i < nst; i++) free(st[i].text);
    free(st);
    for (i = 0; i < x.nm; i++) {
        int k;
        free(x.m[i].name);
        for (k = 0; k < x.m[i].narg; k++) free(x.m[i].arg[k]);
        free(x.m[i].arg);
    }
    free(x.m);
    if (o->so_path) write_file(o->so_path, c->so.p ? c->so.p : "", c->so.n);
    if (o->wo_path) write_file(o->wo_path, c->wo.p ? c->wo.p : "", c->wo.n);
    if (o->verbose && c->so.p) fwrite(c->so.p, 1, c->so.n, stdout);
    if (o->jed_path && !c->fatal) jed_append(c, o->jed_path);
    if (keep) { buf_puts(keep, c->so.p ? c->so.p : ""); buf_puts(keep, c->jv.p ? c->jv.p : ""); }
    rc = c->nerr ? 1 : 0;
    free_order(c);
    for (i = 0; i < c->ngrp; i++) { int k; free(c->grp[i].name); for (k = 0; k < c->grp[i].n; k++) free(c->grp[i].mem[k]); free(c->grp[i].mem); }
    for (i = 0; i < c->nvar; i++) { int k; free(c->var[i].name); for (k = 0; k < c->var[i].n; k++) free(c->var[i].mem[k]); free(c->var[i].mem); }
    free(c->grp); free(c->var);
    for (i = 0; i < c->nline; i++) free(c->line[i]);
    free(c->line);
    free(c->uval); free(c->lastdisp);
    buf_free(&c->so); buf_free(&c->wo); buf_free(&c->jv);
    if (c->simok) sim3_free(&c->sim);
    return rc;
}

/* the reference simulator's model of this part is copied and cannot be
   checked against the fuse map (clock/OE pins that never reach the array,
   latches, input registers, PEEL levels) */
int csim_unverified(const Device *dv)
{
    int i;
    if (dv->sim_noclock || dv->sim_oe || dv->sim_le || dv->sim_preload_pin || dv->sim_unverified || dv->ninflag) return 1;
    for (i = 0; i < dv->nmc; i++) if (dv->mc[i].latch) return 1;
    return 0;
}

/* a register whose D/T sum has a programmed row */
static int reg_used(const Device *dv, const unsigned char *fuse, int i)
{
    int r, cc;
    for (r = dv->mc[i].sum0; r <= dv->mc[i].sum1; r++)
        for (cc = 0; cc < dv->ncols; cc++) {
            int a = dev_fuse(dv, r, cc);
            if (a >= 0 && fuse[a]) return 1;
        }
    return 0;
}
#define SIM_HASREG_FUSES(dv, fuse, i) reg_used(dv, fuse, i)

/* the design meets a bug of the reference simulator (bugs.h); *what
   describes it */
static int refdefect_bug(const CsDesign *d, const char **what)
{
    const Device *dv = d->dev;
    int i;
    if (dv->sim_regoe) {
        *what = "the output enable of registered outputs is ignored on this device";
        return BUG_SIM_PEEL_OE;
    }
    if (dv->sim_oepin) {
        *what = "the output enable of registered outputs comes from pin 11, the GAL16V8's OE pin, which is an I/O pin of this package";
        return BUG_SIM_VP8_OEPIN;
    }
    if (dv->sim_regzero) {
        *what = "the registers of this device load 0 at the clock (C, K and P on its clock pin are rejected)";
        return BUG_SIM_REGZERO;
    }
    if (dv->sim_inlevel)
        for (i = 0; i < d->nsym; i++)
            if (d->sym[i].neg && d->sym[i].pin <= dv->npins) {
                *what = "the test values of inputs declared active low are taken as pin levels";
                return BUG_SIM_V5K_INLEVEL;
            }
    if (dv->sim_inlevel && d->nfbinv) {
        *what = "an output declared active low, or registered with '!' on its equation, is read complemented by other equations";
        return BUG_SIM_V5K_READ;
    }
    for (i = 0; i < dv->nmc; i++)
        if (dv->mc[i].cksync_fuse >= 0 && dv->mc[i].ck_row >= 0 && d->fuse && SIM_HASREG_FUSES(dv, d->fuse, i)) {
            int cc, used = 0;
            for (cc = 0; cc < dv->ncols && !used; cc++) {
                int a = dev_fuse(dv, dv->mc[i].ck_row, cc);
                if (a >= 0 && d->fuse[a]) used = 1;
            }
            if (!used) {
                *what = "a register without .CK and .CE is clocked from its clock pin although its clock term is false in the fuse map";
                return BUG_V5K_CLOCK;
            }
            if (d->fuse[dv->mc[i].cksync_fuse]) {
                *what = "the clock enable (.CE) of the registers is ignored";
                return BUG_SIM_NOCE;
            }
        }
    for (i = 0; i < dv->nmc; i++)
        if (dv->mc[i].ce_fuse >= 0 && d->fuse && d->fuse[dv->mc[i].ce_fuse]) {
            *what = "the clock enable (.CE) of the registers is ignored";
            return BUG_SIM_NOCE;
        }
    if (d->ctlinfo && (dv->ar_row >= 0 || dv->sp_row >= 0)) {
        int nreg = 0;
        for (i = 0; i < dv->nmc; i++) if (dv->mc[i].pin > 0) nreg++;
        if ((dv->ar_row >= 0 && d->narpin < nreg) || (dv->sp_row >= 0 && d->nsppin < nreg)) {
            *what = "the shared AR/SP product term acts only on registers with an .AR/.SP equation (the device applies it to all registers)";
            return BUG_SIM_SHARED_ARSP;
        }
    }
    return 0;
}

int csim_run(const CsDesign *d, const CsOpts *o)
{
    const char *what = "";
    Buf a = {0, 0, 0}, b = {0, 0, 0};
    int rc, bug;
    if (!(bug = refdefect_bug(d, &what))) return run_one(d, o, NULL);
    rc = run_one(d, o, &a);
    {
        /* the other mode: is the result affected by the bug? */
        CsOpts o2 = *o;
        o2.so_path = o2.wo_path = o2.jed_path = NULL;
        o2.verbose = 0;
        o2.refdefect = !o->refdefect;
        run_one(d, &o2, &b);
    }
    if (!o->quiet && (a.n != b.n || (a.n && memcmp(a.p, b.p, a.n)))) {
        char t[600];
        int sv = g_fix;
        g_fix = !o->refdefect;
        bug_text(t, sizeof t, bug, what);
        g_fix = sv;
        printf("[0905sa] warning: %s\n", t);
    }
    buf_free(&a); buf_free(&b);
    return rc;
}

void cs_design_free(CsDesign *d)
{
    int i, k;
    free(d->fuse);
    for (i = 0; i < CH_COUNT; i++) free(d->hdr[i]);
    for (i = 0; i < d->nsym; i++) free(d->sym[i].name);
    free(d->sym);
    for (i = 0; i < d->nfld; i++) {
        free(d->fld[i].name);
        for (k = 0; k < d->fld[i].n; k++) free(d->fld[i].mem[k]);
        free(d->fld[i].mem);
    }
    free(d->fld);
    memset(d, 0, sizeof *d);
}

int cs_abs_write(const char *path, const CsDesign *d)
{
    Buf b = {0, 0, 0};
    int i, k, r;
    buf_puts(&b, "FREECUPL-ABS 1\n");
    buf_printf(&b, "DEVICE %s\n", d->mnemonic);
    for (i = 0; i < CH_COUNT; i++) if (d->hdr[i]) buf_printf(&b, "HDR %d %s\n", i, d->hdr[i]);
    for (i = 0; i < d->nsym; i++) buf_printf(&b, "SYM %d %d %s\n", d->sym[i].pin, d->sym[i].neg, d->sym[i].name);
    for (i = 0; i < d->nreserve; i++) buf_printf(&b, "RESERVE %d\n", d->reserve[i]);
    if (d->ctlinfo) {
        buf_puts(&b, "CTL");
        for (i = 0; i < d->narpin; i++) buf_printf(&b, " ar%d", d->arpin[i]);
        for (i = 0; i < d->nsppin; i++) buf_printf(&b, " sp%d", d->sppin[i]);
        for (i = 0; i < d->noepin; i++) buf_printf(&b, " oe%d", d->oepin[i]);
        buf_puts(&b, "\n");
    }
    for (i = 0; i < d->nnoeqn; i++) buf_printf(&b, "NOEQN %d %d\n", d->noeqn[i], d->noeqn_used[i]);
    for (i = 0; i < d->nfbinv; i++) buf_printf(&b, "FBINV %d\n", d->fbinv[i]);
    for (i = 0; i < d->nfld; i++) {
        buf_printf(&b, "FIELD %s %d", d->fld[i].name, d->fld[i].n);
        for (k = 0; k < d->fld[i].n; k++) buf_printf(&b, " %s", d->fld[i].mem[k]);
        buf_puts(&b, "\n");
    }
    buf_printf(&b, "FUSES %d\n", d->dev->nfuses);
    for (i = 0; i < d->dev->nfuses; i += 64) {
        for (k = i; k < i + 64 && k < d->dev->nfuses; k++) buf_putc(&b, d->fuse[k] ? '1' : '0');
        buf_puts(&b, "\n");
    }
    buf_puts(&b, "END\n");
    r = write_file(path, b.p, b.n);
    buf_free(&b);
    return r;
}

int cs_abs_read(const char *path, CsDesign *d, char *err, size_t errlen)
{
    size_t len;
    char *buf = read_file(path, &len), *p, *next;
    int nf = -1, got = 0;
    memset(d, 0, sizeof *d);
    if (!buf) { snprintf(err, errlen, "could not open:  %s", path); return -1; }
    if (strncmp(buf, "FREECUPL-ABS 1", 14) != 0) { snprintf(err, errlen, "invalid file format:  %s", path); free(buf); return -1; }
    for (p = buf; p && *p; p = next) {
        size_t l;
        next = strchr(p, '\n');
        if (next) *next++ = 0;
        l = strlen(p);
        if (l && p[l - 1] == '\r') p[--l] = 0;
        if (nf >= 0 && got < nf && (p[0] == '0' || p[0] == '1')) {
            size_t k;
            for (k = 0; k < l && got < nf; k++) d->fuse[got++] = (unsigned char)(p[k] == '1');
            continue;
        }
        if (!strncmp(p, "DEVICE ", 7)) {
            snprintf(d->mnemonic, sizeof d->mnemonic, "%s", p + 7);
            d->dev = dev_find(d->mnemonic, NULL);
            if (!d->dev) { snprintf(err, errlen, "device not in library:  %s", d->mnemonic); free(buf); cs_design_free(d); return -1; }
        } else if (!strncmp(p, "HDR ", 4)) {
            int h = atoi(p + 4);
            char *v = strchr(p + 4, ' ');
            if (h >= 0 && h < CH_COUNT && v) { free(d->hdr[h]); d->hdr[h] = xstrdup(v + 1); }
        } else if (!strncmp(p, "SYM ", 4)) {
            char *q = p + 4, *e;
            int pin = (int)strtol(q, &e, 10), neg;
            q = e;
            neg = (int)strtol(q, &e, 10);
            if (e != q) {
                while (*e == ' ') e++;
                d->sym = (CsSym *)xrealloc(d->sym, sizeof(CsSym) * (size_t)(d->nsym + 1));
                d->sym[d->nsym].pin = pin;
                d->sym[d->nsym].neg = neg;
                d->sym[d->nsym].name = xstrdup(e);
                d->nsym++;
            }
        } else if (!strncmp(p, "FIELD ", 6)) {
            /* FIELD name n m1 m2 ... */
            char *w[512];
            int nw = 0, k, n;
            char *q = p + 6;
            while (*q && nw < 512) {
                while (*q == ' ') *q++ = 0;
                if (!*q) break;
                w[nw++] = q;
                while (*q && *q != ' ') q++;
            }
            if (nw >= 2 && (n = atoi(w[1])) >= 0 && n <= nw - 2) {
                CsField *f;
                d->fld = (CsField *)xrealloc(d->fld, sizeof(CsField) * (size_t)(d->nfld + 1));
                f = &d->fld[d->nfld++];
                f->name = xstrdup(w[0]);
                f->n = n;
                f->mem = (char **)xcalloc((size_t)n + 1, sizeof(char *));
                for (k = 0; k < n; k++) f->mem[k] = xstrdup(w[2 + k]);
            }
        } else if (!strncmp(p, "CTL", 3)) {
            char *q = p + 3;
            d->ctlinfo = 1;
            while (*q) {
                while (*q == ' ') q++;
                if (!strncmp(q, "ar", 2) && d->narpin < 128) d->arpin[d->narpin++] = atoi(q + 2);
                else if (!strncmp(q, "sp", 2) && d->nsppin < 128) d->sppin[d->nsppin++] = atoi(q + 2);
                else if (!strncmp(q, "oe", 2) && d->noepin < 128) d->oepin[d->noepin++] = atoi(q + 2);
                while (*q && *q != ' ') q++;
            }
        } else if (!strncmp(p, "NOEQN ", 6)) {
            char *e;
            int pin = (int)strtol(p + 6, &e, 10);
            if (d->nnoeqn < 64) { d->noeqn[d->nnoeqn] = pin; d->noeqn_used[d->nnoeqn++] = (unsigned char)(atoi(e) != 0); }
        } else if (!strncmp(p, "FBINV ", 6)) {
            if (d->nfbinv < 128) d->fbinv[d->nfbinv++] = atoi(p + 6);
        } else if (!strncmp(p, "RESERVE ", 8)) {
            if (d->nreserve < 8) d->reserve[d->nreserve++] = atoi(p + 8);
        } else if (!strncmp(p, "FUSES ", 6)) {
            nf = atoi(p + 6);
            if (nf < 0) nf = 0;
            d->fuse = (unsigned char *)xcalloc((size_t)nf + 1, 1);
        }
    }
    free(buf);
    if (!d->dev || !d->fuse || got != nf || nf != d->dev->nfuses) {
        snprintf(err, errlen, "invalid file format:  %s", path);
        cs_design_free(d);
        return -1;
    }
    return 0;
}
