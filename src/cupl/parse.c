/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - CUPL lexer and parser.  DARKSOFT.
   Set expressions are evaluated per bit (lane) of the left side; a number
   gives its bit at the lane's position, as the original aligns them. */
#include "design.h"
#include "diag.h"
#include "bugs.h"
#include "pp.h"
#include "util.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Design g_des;

const char *const ext_names[X_COUNT] = {
    "", "d", "oe", "ar", "sp", "t", "j", "k", "s", "r", "ck", "l", "le",
    "ap", "sr", "ce", "ca", "dq", "io", "dfb", "int", "lfb", "tfb", "iod", "iol",
    "lq", "ioar", "ioap", "iock", "iosp", "iosr", "apmux", "armux", "ckmux",
    "oemux", "lemux", "imux", "byp", "obs", "pr", "tec", "t1", "t2"
};

int ext_lookup(const char *s)
{
    int i;
    for (i = 1; i < X_COUNT; i++) if (str_ieq(ext_names[i], s)) return i;
    return -1;
}

Sym *sym_find(const char *name)
{
    int i;
    for (i = 0; i < g_des.nsyms; i++) if (!strcmp(g_des.syms[i]->name, name)) return g_des.syms[i];
    return NULL;
}

static int index_of(const char *n)
{
    size_t l = strlen(n), k = l;
    long v = 0;
    while (k > 0 && isdigit((unsigned char)n[k - 1])) k--;
    if (k == l || k == 0) return -1;
    if (l - k > 3) return -1;
    v = strtol(n + k, NULL, 10);
    return v <= 31 ? (int)v : -1;
}

Sym *sym_get(const char *name)
{
    Sym *s = sym_find(name);
    if (s) return s;
    s = (Sym *)xcalloc(1, sizeof(Sym));
    s->name = xstrdup(name);
    s->index = index_of(name);
    s->id = g_des.nsyms;
    if (g_des.nsyms == g_des.capsyms) {
        g_des.capsyms = g_des.capsyms ? g_des.capsyms * 2 : 64;
        g_des.syms = (Sym **)xrealloc(g_des.syms, sizeof(Sym *) * (size_t)g_des.capsyms);
    }
    g_des.syms[g_des.nsyms++] = s;
    return s;
}

Eqn *eqn_find(const Sym *s, int ext)
{
    int i;
    for (i = 0; i < g_des.neqns; i++)
        if (g_des.eqns[i]->sym == s && g_des.eqns[i]->ext == ext) return g_des.eqns[i];
    return NULL;
}

static Eqn *eqn_new(Sym *s, int ext, int line)
{
    Eqn *q = (Eqn *)xcalloc(1, sizeof(Eqn));
    q->sym = s;
    q->ext = ext;
    q->line = line;
    q->minlvl = -1;
    if (g_des.neqns == g_des.capeqns) {
        g_des.capeqns = g_des.capeqns ? g_des.capeqns * 2 : 64;
        g_des.eqns = (Eqn **)xrealloc(g_des.eqns, sizeof(Eqn *) * (size_t)g_des.capeqns);
    }
    g_des.eqns[g_des.neqns++] = q;
    return q;
}

Expr *mk_const(int v)
{
    Expr *e = (Expr *)xcalloc(1, sizeof(Expr));
    e->op = E_CONST;
    e->val = (unsigned char)(v ? 1 : 0);
    return e;
}

Expr *mk_var(Sym *s, int ext, int neg)
{
    Expr *e = (Expr *)xcalloc(1, sizeof(Expr));
    e->op = E_VAR;
    e->sym = s;
    e->ext = (unsigned char)ext;
    e->neg = (unsigned char)(neg ? 1 : 0);
    return e;
}

/* complement: a literal or constant is complemented in place (a copy);
   anything else gets a NOT node (never folded: the reference compiler
   uses such a NOT at the root to choose the output polarity) */
Expr *mk_not(Expr *e)
{
    Expr *n;
    if (e->op == E_VAR) { n = mk_var(e->sym, e->ext, !e->neg); return n; }
    if (e->op == E_CONST) {
        /* neg marks a complemented constant (BUG_POL_CONST) */
        n = mk_const(!e->val);
        n->neg = (unsigned char)!e->neg;
        return n;
    }
    n = (Expr *)xcalloc(1, sizeof(Expr));
    n->op = E_NOT;
    n->a = e;
    return n;
}

Expr *mk_bin(int op, Expr *a, Expr *b)
{
    Expr *n = (Expr *)xcalloc(1, sizeof(Expr));
    n->op = (unsigned char)op;
    n->a = a;
    n->b = b;
    return n;
}

static void ep(const Expr *e, Buf *b, int prec)
{
    switch (e->op) {
    case E_CONST: buf_printf(b, "'b'%d", e->val); break;
    case E_VAR:
        buf_printf(b, "%s%s%s%s", e->neg ? "!" : "", e->sym->name, e->ext ? "." : "", e->ext ? ext_names[e->ext] : "");
        break;
    case E_NOT: buf_puts(b, "!("); ep(e->a, b, 0); buf_puts(b, ")"); break;
    default: {
        int p = e->op == E_AND ? 3 : e->op == E_OR ? 2 : 1;
        if (p < prec) buf_putc(b, '(');
        ep(e->a, b, p);
        buf_puts(b, e->op == E_AND ? " & " : e->op == E_OR ? " # " : " $ ");
        ep(e->b, b, p + 1);
        if (p < prec) buf_putc(b, ')');
    }
    }
}

void expr_print(const Expr *e, char *out, int n)
{
    Buf b = {0, 0, 0};
    ep(e, &b, 0);
    buf_putc(&b, 0);
    strncpy(out, b.p, (size_t)n - 1);
    out[n - 1] = 0;
    buf_free(&b);
}

enum {
    T_EOF = 0, T_WORD, T_BASE, T_HDR, T_LBRK, T_RBRK, T_LPAR, T_RPAR, T_LBRACE,
    T_RBRACE, T_SEMI, T_COMMA, T_EQ, T_COLON, T_DOT, T_DOTDOT, T_AND, T_OR,
    T_XOR, T_NOT, T_ARROW, T_OTHER
};

typedef struct {
    int t;
    char *s;
    int base;   /* T_BASE: 2, 8, 10, 16 */
    int hdr;    /* T_HDR: header index */
    int pl;     /* preprocessed line index */
    int col;
    int hkw, epl, ecol;  /* T_HDR: spelling (hdr_kw), position of its ';' */
} Tok;

static PPOut *g_pp;
static Tok *g_tok;
static int g_ntok;

static const char *const hdr_kw[] = {
    "NAME", "PARTNO", "REVISION", "REV", "DATE", "DESIGNER", "COMPANY", "ASSEMBLY",
    "ASSY", "LOCATION", "LOC", "DEVICE", "FORMAT", NULL
};
static const int hdr_idx[] = {
    H_NAME, H_PARTNO, H_REV, H_REV, H_DATE, H_DESIGNER, H_COMPANY, H_ASSEMBLY,
    H_ASSEMBLY, H_LOCATION, H_LOCATION, H_DEVICE, H_FORMAT
};

const char *pp_line_text(int line)
{
    if (!g_pp || line < 0 || line >= g_pp->n) return "";
    return g_pp->v[line].text;
}

static int src_line(int pl)
{
    if (!g_pp || pl < 0 || pl >= g_pp->n) return 0;
    return g_pp->v[pl].line;
}

static void add_tok(int t, char *s, int pl, int col)
{
    static int cap;
    if (g_ntok == cap) {
        cap = cap ? cap * 2 : 1024;
        g_tok = (Tok *)xrealloc(g_tok, sizeof(Tok) * (size_t)cap);
    }
    memset(&g_tok[g_ntok], 0, sizeof(Tok));
    g_tok[g_ntok].t = t;
    g_tok[g_ntok].s = s;
    g_tok[g_ntok].pl = pl;
    g_tok[g_ntok].col = col;
    g_ntok++;
}

static int is_word(int c) { return isalnum(c) || c == '_'; }

static void lex_all(void)
{
    int pl = 0, col = 0, incomment = 0, cstart = 0, stmt_start = 1;
    while (pl < g_pp->n) {
        const char *L = g_pp->v[pl].text;
        int c = (unsigned char)L[col];
        if (!c) { pl++; col = 0; continue; }
        if (incomment) {
            if (c == '*' && L[col + 1] == '/') { incomment = 0; col += 2; }
            else col++;
            continue;
        }
        if (c == '/' && L[col + 1] == '*') { incomment = 1; cstart = pl; col += 2; continue; }
        if (isspace(c) || c == 0x1A) { col++; continue; }
        if (is_word(c)) {
            int s = col;
            char *w;
            while (is_word((unsigned char)L[col])) col++;
            w = xstrndup(L + s, (size_t)(col - s));
            if (stmt_start) {
                int k;
                for (k = 0; hdr_kw[k]; k++) if (str_ieq(w, hdr_kw[k])) break;
                if (hdr_kw[k]) {
                    Buf b = {0, 0, 0};
                    int p2 = pl, c2 = col;
                    char *v;
                    size_t vl;
                    for (;;) {
                        const char *T = p2 < g_pp->n ? g_pp->v[p2].text : NULL;
                        if (!T) break;
                        if (!T[c2]) { p2++; c2 = 0; buf_putc(&b, ' '); continue; }
                        if (T[c2] == '/' && T[c2 + 1] == '*') {
                            /* comment inside header value */
                            c2 += 2;
                            for (;;) {
                                const char *U = p2 < g_pp->n ? g_pp->v[p2].text : NULL;
                                if (!U) break;
                                if (!U[c2]) { p2++; c2 = 0; continue; }
                                if (U[c2] == '*' && U[c2 + 1] == '/') { c2 += 2; break; }
                                c2++;
                            }
                            continue;
                        }
                        if (T[c2] == ';') break;
                        buf_putc(&b, T[c2]);
                        c2++;
                    }
                    buf_putc(&b, 0);
                    v = b.p;
                    while (*v == ' ' || *v == '\t') v++;
                    vl = strlen(v);
                    while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
                    add_tok(T_HDR, xstrndup(v, vl), pl, s);
                    g_tok[g_ntok - 1].hdr = hdr_idx[k];
                    g_tok[g_ntok - 1].hkw = k;
                    g_tok[g_ntok - 1].epl = p2;
                    g_tok[g_ntok - 1].ecol = c2;
                    free(b.p);
                    free(w);
                    if (p2 >= g_pp->n) { pl = p2; col = 0; add_tok(T_EOF, NULL, g_pp->n - 1, 0); return; }
                    pl = p2;
                    col = c2 + 1;
                    stmt_start = 1;
                    continue;
                }
            }
            add_tok(T_WORD, w, pl, s);
            stmt_start = 0;
            continue;
        }
        if (c == '\'' && isalpha((unsigned char)L[col + 1]) && L[col + 2] == '\'') {
            int b = toupper((unsigned char)L[col + 1]);
            int base = b == 'B' ? 2 : b == 'O' ? 8 : b == 'D' ? 10 : b == 'H' ? 16 : 0;
            if (base) {
                add_tok(T_BASE, xstrndup(L + col, 3), pl, col);
                g_tok[g_ntok - 1].base = base;
                col += 3;
                stmt_start = 0;
                continue;
            }
        }
        {
            int t = T_OTHER, len = 1;
            switch (c) {
            case '[': t = T_LBRK; break;
            case ']': t = T_RBRK; break;
            case '(': t = T_LPAR; break;
            case ')': t = T_RPAR; break;
            case '{': t = T_LBRACE; break;
            case '}': t = T_RBRACE; break;
            case ';': t = T_SEMI; break;
            case ',': t = T_COMMA; break;
            case ':': t = T_COLON; break;
            case '&': t = T_AND; break;
            case '#': t = T_OR; break;
            case '$': t = T_XOR; break;
            case '!': t = T_NOT; break;
            case '.': if (L[col + 1] == '.') { t = T_DOTDOT; len = 2; } else t = T_DOT; break;
            case '=': if (L[col + 1] == '>') { t = T_ARROW; len = 2; } else t = T_EQ; break;
            default: break;
            }
            add_tok(t, xstrndup(L + col, (size_t)len), pl, col);
            col += len;
            stmt_start = (t == T_SEMI || t == T_LBRACE || t == T_RBRACE);
        }
    }
    if (incomment) diag(DG_ERROR, "0026cx", src_line(cstart), "reached end-of-file before ending comment");
    add_tok(T_EOF, NULL, g_pp->n ? g_pp->n - 1 : 0, 0);
}

static Tok *T;      /* current token array */
static int NT, P;   /* size, position */
static int g_errs_here;

static Tok *cur(void) { return &T[P < NT ? P : NT - 1]; }
static Tok *peekn(int k) { return &T[P + k < NT ? P + k : NT - 1]; }
static int at(int t) { return cur()->t == t; }
static int line_of(Tok *t) { return src_line(t->pl); }
static int kw(const char *k) { return cur()->t == T_WORD && str_ieq(cur()->s, k); }

static const char *tok_text(Tok *t)
{
    if (t->t == T_EOF) return "end-of-file";
    return t->s ? t->s : "";
}

static void syntax_error(Tok *t)
{
    if (g_errs_here++ == 0 || 1) {
        if (t->pl >= 0 && t->pl < g_pp->n && t->t != T_EOF)
            diag_context(src_line(t->pl), g_pp->v[t->pl].text, t->col);
        if (t->t == T_EOF) diag(DG_ERROR, "0044ca", 0, "unexpected end-of-file");
        else diag(DG_ERROR, "0007ca", src_line(t->pl), "invalid syntax:  %s", tok_text(t));
    }
}

static void missing(const char *what)
{
    Tok *t = cur();
    if (t->pl >= 0 && t->pl < g_pp->n && t->t != T_EOF)
        diag_context(src_line(t->pl), g_pp->v[t->pl].text, t->col);
    if (t->t == T_EOF) diag(DG_ERROR, "0044ca", 0, "unexpected end-of-file");
    else diag(DG_ERROR, "0009ca", src_line(t->pl), "missing symbol:  %s", what);
}

static void recover(void)
{
    int depth = 0;
    while (!at(T_EOF)) {
        if (at(T_LBRACE)) depth++;
        else if (at(T_RBRACE)) { if (depth == 0) return; depth--; }
        else if (at(T_SEMI) && depth == 0) { P++; return; }
        P++;
    }
}

static int expect(int t, const char *what)
{
    if (at(t)) { P++; return 1; }
    missing(what);
    return 0;
}

static int g_negfield;   /* '!' before a field compared with ':' */
static int g_lhs_list;   /* parsing the [list] left of an equation */
#define LHS_NUM "\001"     /* a number on the left of an equation */

/* Words the original never takes as a name, in any letter case.  ELSE,
   JUMP and MACRO are in the manual's table but are accepted; FUSE is
   handled by the preprocessor (pp.c). */
static int is_reserved(const char *w)
{
    static const char *const kws[] = {
        "APPEND", "ASSEMBLY", "ASSY", "COMPANY", "CONDITION", "DATE", "DEFAULT", "DESIGNER",
        "DEVICE", "FIELD", "FLD", "FORMAT", "FUNCTION", "GROUP", "IF", "LOC", "LOCATION",
        "MIN", "NAME", "NEXT", "NODE", "OUT", "PARTNO", "PIN", "PINNODE", "PRESENT",
        "PROPERTY", "REV", "REVISION", "SEQUENCE", "SEQUENCED", "SEQUENCEJK", "SEQUENCERS",
        "SEQUENCET", "TABLE", NULL
    };
    int i;
    for (i = 0; kws[i]; i++) if (str_ieq(w, kws[i])) return 1;
    return 0;
}

static int at_name(void) { return at(T_WORD) && !is_reserved(cur()->s); }

static int has_alpha(const char *c)
{
    for (; *c; c++) if (isalpha((unsigned char)*c) || *c == '_') return 1;
    return 0;
}

typedef struct {
    unsigned long val, dc;   /* 32-bit value and don't-care mask */
    int width;               /* bits given by the digits */
    int ok;
} Num;

static int digit_val(int c)
{
    c = toupper(c);
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static Num conv_num(const char *s, int base, int line)
{
    Num n;
    int bits = base == 2 ? 1 : base == 8 ? 3 : 4, dcdec = 0;
    memset(&n, 0, sizeof n);
    n.ok = 1;
    for (; *s; s++) {
        int d = digit_val((unsigned char)*s);
        if (toupper((unsigned char)*s) == 'X') {
            if (base == 10) { dcdec = 1; d = 0; }
            else {
                n.val <<= bits; n.dc = (n.dc << bits) | ((1UL << bits) - 1);
                n.width += bits;
                continue;
            }
        }
        if (d < 0 || d >= base) { n.ok = 0; return n; }
        if (base == 10) n.val = n.val * 10 + (unsigned long)d;
        else { n.val = (n.val << bits) | (unsigned long)d; n.dc <<= bits; }
        n.width += bits;
    }
    if (dcdec) diag(DG_WARN, "0041ca", line, "don't care(s) not allowed for decimal number, treated as 0");
    n.val &= 0xFFFFFFFFUL;
    n.dc &= 0xFFFFFFFFUL;
    return n;
}

/* number: [base] WORD ; default base hex */
static int parse_number(Num *n, int defbase)
{
    int base = defbase;
    Tok *t;
    if (at(T_BASE)) { base = cur()->base; P++; }
    t = cur();
    if (t->t != T_WORD) { syntax_error(t); return 0; }
    *n = conv_num(t->s, base, line_of(t));
    if (!n->ok) {
        diag(DG_ERROR, "0002ca", line_of(t), "invalid number:  %s", t->s);
        P++;
        return 0;
    }
    P++;
    return 1;
}

typedef struct { char *name; int neg; int ext; } LItem;
typedef struct { LItem *v; int n; Tok *exttok; } LList;   /* exttok: first extension written on an element */

static void ll_add(LList *l, const char *name, int neg, int ext)
{
    l->v = (LItem *)xrealloc(l->v, sizeof(LItem) * (size_t)(l->n + 1));
    l->v[l->n].name = xstrdup(name);
    l->v[l->n].neg = neg;
    l->v[l->n].ext = ext;
    l->n++;
}

/* split "abc012" into prefix and number; returns 0 if no trailing digits */
static int split_idx(const char *w, char *pre, size_t np, long *num)
{
    size_t l = strlen(w), k = l;
    while (k > 0 && isdigit((unsigned char)w[k - 1])) k--;
    if (k == l) return 0;
    if (k >= np) return 0;
    memcpy(pre, w, k);
    pre[k] = 0;
    *num = strtol(w + k, NULL, 10);
    return 1;
}

static int parse_ext(void)
{
    if (at(T_DOT) && peekn(1)->t == T_WORD) {
        int e = ext_lookup(peekn(1)->s);
        if (e < 0) {
            diag(DG_ERROR, "0011cb", line_of(cur()), "unknown extension:  %s", peekn(1)->s);
            P += 2;
            return X_NONE;
        }
        P += 2;
        return e;
    }
    return X_NONE;
}

/* '[' item {, item} ']' where item = [!]name[..[prefix]num][.ext];
   bare: a single item without brackets is accepted (FIELD f = a7..a0;) */
static int parse_varlist_x(LList *l, int bare)
{
    if (bare && !at(T_LBRK)) bare = 2;
    else if (!expect(T_LBRK, "[")) return 0;
    for (;;) {
        int neg = 0, ext;
        Tok *t;
        if (at(T_NOT)) { neg = 1; P++; }
        t = cur();
        if (g_lhs_list && t->t == T_BASE && peekn(1)->t == T_WORD) {
            Num nn;
            if (!parse_number(&nn, 16)) return 0;
            ll_add(l, LHS_NUM, 0, 0);
            if (at(T_COMMA)) { P++; continue; }
            if (at(T_RBRK)) { P++; break; }
            missing("]");
            return 0;
        }
        if (t->t != T_WORD || is_reserved(t->s)) { syntax_error(t); return 0; }
        P++;
        if (at(T_DOTDOT)) {
            char pre[128], pre2[128];
            long a, b, k;
            Tok *u;
            P++;
            u = cur();
            if (u->t != T_WORD || !split_idx(t->s, pre, sizeof pre, &a)) { syntax_error(u); return 0; }
            P++;
            if (isdigit((unsigned char)u->s[0])) {
                char *e;
                b = strtol(u->s, &e, 10);
                if (*e) { syntax_error(u); return 0; }
            } else {
                if (!split_idx(u->s, pre2, sizeof pre2, &b) || strcmp(pre, pre2)) { syntax_error(u); return 0; }
            }
            ext = parse_ext();
            if (a < 0 || b < 0 || a - b > 1024 || b - a > 1024) { syntax_error(u); return 0; }
            /* the first name keeps its spelling, generated ones have no leading zeros */
            for (k = a;; k += (a <= b ? 1 : -1)) {
                char nm[160];
                if (k == a) snprintf(nm, sizeof nm, "%s", t->s);
                else snprintf(nm, sizeof nm, "%s%ld", pre, k);
                ll_add(l, nm, neg, ext);
                if (k == b) break;
            }
        } else {
            if (at(T_DOT) && peekn(1)->t == T_WORD && !l->exttok) l->exttok = peekn(1);
            ext = parse_ext();
            ll_add(l, t->s, neg, ext);
        }
        if (bare == 2) {
            if (at(T_COMMA)) { P++; continue; }
            break;
        }
        if (at(T_COMMA)) { P++; continue; }
        if (at(T_RBRK)) { P++; break; }
        missing("]");
        return 0;
    }
    if (at(T_DOT) && peekn(1)->t == T_WORD) {
        int x = parse_ext(), i;
        for (i = 0; i < l->n; i++) if (!l->v[i].ext) l->v[i].ext = x;
    }
    return 1;
}

static int parse_varlist(LList *l) { return parse_varlist_x(l, 0); }

/* pin number list: n | '[' n[..m] {, ...} ']' */
static int parse_numlist(int **out, int *nout)
{
    int *v = NULL, n = 0;
    if (at(T_WORD)) {
        char *e;
        long x = strtol(cur()->s, &e, 10);
        if (*e) { syntax_error(cur()); return 0; }
        v = (int *)xmalloc(sizeof(int));
        v[0] = (int)x;
        n = 1;
        P++;
    } else {
        if (!at(T_LBRK)) { syntax_error(cur()); return 0; }
        P++;
        for (;;) {
            char *e;
            long a, b, k;
            if (!at(T_WORD)) { syntax_error(cur()); return 0; }
            a = strtol(cur()->s, &e, 10);
            if (*e) { syntax_error(cur()); return 0; }
            P++;
            b = a;
            if (at(T_DOTDOT)) {
                P++;
                if (!at(T_WORD)) { syntax_error(cur()); return 0; }
                b = strtol(cur()->s, &e, 10);
                if (*e) { syntax_error(cur()); return 0; }
                P++;
            }
            if (a < 0 || b < 0 || a > 9999 || b > 9999) { syntax_error(cur()); return 0; }
            for (k = a;; k += (a <= b ? 1 : -1)) {
                v = (int *)xrealloc(v, sizeof(int) * (size_t)(n + 1));
                v[n++] = (int)k;
                if (k == b) break;
            }
            if (at(T_COMMA)) { P++; continue; }
            if (at(T_RBRK)) { P++; break; }
            missing("]");
            return 0;
        }
    }
    *out = v;
    *nout = n;
    return 1;
}

enum { PN_VAR, PN_NUM, PN_LIST, PN_NOT, PN_AND, PN_OR, PN_XOR, PN_EQ, PN_RED };

typedef struct PNode PNode;
typedef struct { unsigned long lo, hi, dclo, dchi; int range; } Val;
struct PNode {
    int k;
    Sym *sym;
    int ext, neg;
    Num num;
    PNode **items; int nitems;
    PNode *a, *b;
    Val *vals; int nvals;
    int redop;                  /* PN_RED: E_AND / E_OR / E_XOR */
    int paren;                  /* PN_NOT: operand not a plain variable (keeps a NOT node) */
    Tok *exttok;                /* PN_LIST: an extension written on an element */
    int line;
    Tok *tok;
};

/* default base of equality / range constants: a base prefix on a number
   operand changes it until the next binary operator (reference behaviour) */
static int g_eqbase = 16;

static PNode *pn(int k, int line)
{
    PNode *n = (PNode *)xcalloc(1, sizeof(PNode));
    n->k = k;
    n->line = line;
    return n;
}

static PNode *parse_expr(void);
static Sym *call_function(Sym *f, int line);

/* elements of a list / field / variable as PN_VAR nodes */
static int elements(PNode *n, PNode ***out)
{
    if (n->k == PN_LIST) { *out = n->items; return n->nitems; }
    if (n->k == PN_VAR && n->sym->kind == SK_FIELD) {
        PNode **v = (PNode **)xcalloc((size_t)n->sym->nmem + 1, sizeof(PNode *));
        int i;
        for (i = 0; i < n->sym->nmem; i++) {
            v[i] = pn(PN_VAR, n->line);
            v[i]->sym = n->sym->mem[i];
            v[i]->ext = n->ext ? n->ext : n->sym->mem_ext[i];
            v[i]->neg = n->neg;
        }
        *out = v;
        return n->sym->nmem;
    }
    if (n->k == PN_VAR) { *out = (PNode **)xcalloc(2, sizeof(PNode *)); (*out)[0] = n; return 1; }
    *out = NULL;
    return -1;
}

static int parse_vals(PNode *n, int base)
{
    /* after ':' : number | [ items ] with ranges ; optional base prefix
       (an explicit prefix here resets the persistent default to hex) */
    if (at(T_BASE)) { base = cur()->base; P++; g_eqbase = 16; }
    if (at(T_LBRK)) {
        P++;
        for (;;) {
            Num a, b;
            Val v;
            int ib = base;
            if (at(T_BASE)) { ib = cur()->base; P++; }
            if (!parse_number(&a, ib)) return 0;
            memset(&v, 0, sizeof v);
            v.lo = v.hi = a.val;
            v.dclo = v.dchi = a.dc;
            if (at(T_DOTDOT)) {
                P++;
                if (!parse_number(&b, ib)) return 0;
                v.hi = b.val;
                v.dchi = b.dc;
                v.range = 1;
            }
            n->vals = (Val *)xrealloc(n->vals, sizeof(Val) * (size_t)(n->nvals + 1));
            n->vals[n->nvals++] = v;
            if (at(T_COMMA)) { P++; continue; }
            if (at(T_RBRK)) { P++; break; }
            missing("]");
            return 0;
        }
        return 1;
    } else {
        Num a;
        Val v;
        if (!parse_number(&a, base)) return 0;
        memset(&v, 0, sizeof v);
        v.lo = v.hi = a.val;
        v.dclo = v.dchi = a.dc;
        n->vals = (Val *)xmalloc(sizeof(Val));
        n->vals[0] = v;
        n->nvals = 1;
        return 1;
    }
}

static PNode *parse_colon(PNode *x)
{
    Tok *t = cur();
    PNode *n;
    PNode **el;
    if (elements(x, &el) < 0) { syntax_error(t); return x; }
    P++;
    if (at(T_AND) || at(T_OR) || at(T_XOR)) {
        /* as the reference: no extension on an element of a reduced list */
        if (x->k == PN_LIST && x->exttok) syntax_error(x->exttok);
        n = pn(PN_RED, line_of(t));
        n->redop = at(T_AND) ? E_AND : at(T_OR) ? E_OR : E_XOR;
        P++;
        g_eqbase = 16;
        n->a = x;
        return n;
    }
    n = pn(PN_EQ, line_of(t));
    n->a = x;
    if (!parse_vals(n, g_eqbase)) return x;
    g_eqbase = 16;   /* the match used the base change up */
    return n;
}

/* BUG_NOT_NUMBER: '!' before a number applies to the next operand */
static int g_pendnot;

static PNode *parse_primary0(void);

static PNode *parse_primary(void)
{
    PNode *n;
    int pend = g_pendnot && !at(T_BASE);
    if (pend) g_pendnot = 0;
    n = parse_primary0();
    if (pend && n) {
        if (n->k == PN_VAR && n->tok && n->paren == 0) n->neg = !n->neg;
        else {
            PNode *m = pn(PN_NOT, n->line);
            m->a = n;
            m->paren = 1;
            n = m;
        }
    }
    return n;
}

static PNode *parse_primary0(void)
{
    Tok *t = cur();
    PNode *n;
    if (t->t == T_LPAR) {
        P++;
        n = parse_expr();
        if (!n) return NULL;
        if (!expect(T_RPAR, ")")) return NULL;
        if (n->k == PN_VAR) n->paren = 1;
        return n;
    }
    if (t->t == T_BASE) {
        n = pn(PN_NUM, line_of(t));
        g_eqbase = t->base;
        if (!parse_number(&n->num, 16)) return NULL;
        return n;
    }
    if (t->t == T_LBRK) {
        LList l;
        int i;
        memset(&l, 0, sizeof l);
        {
            int negm = g_negfield;
            g_negfield = 0;
            if (!parse_varlist(&l)) return NULL;
            if (negm) for (i = 0; i < l.n; i++) l.v[i].neg ^= 1;
        }
        n = pn(PN_LIST, line_of(t));
        n->exttok = l.exttok;
        for (i = 0; i < l.n; i++) {
            Sym *s = sym_get(l.v[i].name);
            if (s->kind == SK_FIELD) {
                int k;
                for (k = 0; k < s->nmem; k++) {
                    PNode *e = pn(PN_VAR, line_of(t));
                    e->sym = s->mem[k];
                    e->ext = l.v[i].ext ? l.v[i].ext : s->mem_ext[k];
                    e->neg = l.v[i].neg;
                    n->items = (PNode **)xrealloc(n->items, sizeof(PNode *) * (size_t)(n->nitems + 1));
                    n->items[n->nitems++] = e;
                }
            } else {
                PNode *e = pn(PN_VAR, line_of(t));
                e->sym = s;
                s->used = 1;
                e->ext = l.v[i].ext;
                e->neg = l.v[i].neg;
                n->items = (PNode **)xrealloc(n->items, sizeof(PNode *) * (size_t)(n->nitems + 1));
                n->items[n->nitems++] = e;
            }
        }
        if (at(T_DOT)) {
            int x = parse_ext();
            for (i = 0; i < n->nitems; i++) if (!n->items[i]->ext) n->items[i]->ext = x;
        }
        if (at(T_COLON)) n = parse_colon(n);
        return n;
    }
    if (t->t == T_WORD) {
        Sym *s;
        if (is_reserved(t->s)) { syntax_error(t); return NULL; }
        if (!isalpha((unsigned char)t->s[0]) && t->s[0] != '_') {
            /* must contain a letter */
            const char *c = t->s;
            while (*c && !isalpha((unsigned char)*c)) c++;
            if (!*c) { syntax_error(t); return NULL; }
        }
        s = sym_find(t->s);
        if (s && s->kind == SK_FUNC && peekn(1)->t == T_LPAR) {
            Sym *h;
            P++;
            h = call_function(s, line_of(t));
            if (!h) return NULL;
            n = pn(PN_VAR, line_of(t));
            n->sym = h;
            return n;
        }
        s = sym_get(t->s);
        s->used = 1;
        P++;
        n = pn(PN_VAR, line_of(t));
        n->sym = s;
        n->tok = t;
        n->ext = parse_ext();
        if (g_negfield && s->kind == SK_FIELD && at(T_COLON)) n->neg = 1;
        g_negfield = 0;
        if (at(T_COLON)) n = parse_colon(n);
        return n;
    }
    syntax_error(t);
    return NULL;
}

/* '!' directly before a variable makes a complemented literal; before
   anything else it is a NOT operator.  As in the reference compiler,
   "!!name" is the same as "!name", and "!!!" or "!!(" are errors. */
/* a field or a [list] compared with ':' follows: '!' before it
   complements the members */
static int neg_members(void)
{
    if (at(T_WORD)) {
        Sym *fs = sym_find(cur()->s);
        return peekn(1)->t == T_COLON && fs && fs->kind == SK_FIELD;
    }
    if (at(T_LBRK)) {
        int i = 1;
        while (peekn(i)->t != T_RBRK && peekn(i)->t != T_EOF && peekn(i)->t != T_SEMI) i++;
        return peekn(i)->t == T_RBRK && peekn(i + 1)->t == T_COLON;
    }
    return 0;
}

static PNode *parse_unary(void)
{
    if (at(T_NOT)) {
        Tok *t = cur();
        PNode *n, *a;
        P++;
        if (at(T_NOT)) {
            P++;
            if (!at(T_WORD)) { syntax_error(cur()); return NULL; }
            if (neg_members()) {
                g_negfield = 1;
                a = parse_primary();
                g_negfield = 0;
                return a;
            }
            a = parse_primary();
            if (!a) return NULL;
            if (a->k == PN_VAR) { a->neg = 1; a->paren = 1; return a; }
            n = pn(PN_NOT, line_of(t));
            n->a = a;
            return n;
        }
        if (at(T_BASE)) {
            diag_bug(DG_WARN, "0903ca", line_of(t), BUG_NOT_NUMBER, g_fix ? "'!' directly before a number complements the number" : "'!' directly before a number is not applied to it but to the next operand");
            if (!g_fix) { g_pendnot = 1; return parse_primary(); }
        }
        /* an operand's own '!' takes the place of a pending one */
        g_pendnot = 0;
        if (neg_members()) {
            g_negfield = 1;
            a = parse_primary();
            g_negfield = 0;
            return a;
        }
        a = parse_primary();
        if (!a) return NULL;
        if (a->k == PN_VAR && a->tok && a->paren == 0) {
            /* literal: toggle (a fresh PN_VAR from parse_primary has neg 0) */
            a->neg = !a->neg;
            return a;
        }
        n = pn(PN_NOT, line_of(t));
        n->a = a;
        n->paren = 1;
        return n;
    }
    return parse_primary();
}

static PNode *parse_and(void)
{
    PNode *a = parse_unary();
    while (a && at(T_AND)) {
        PNode *n = pn(PN_AND, line_of(cur()));
        P++;
        g_eqbase = 16;   /* a binary operator ends a base change */
        n->a = a;
        n->b = parse_unary();
        if (!n->b) return NULL;
        a = n;
    }
    return a;
}

static PNode *parse_or(void)
{
    PNode *a = parse_and();
    while (a && at(T_OR)) {
        PNode *n = pn(PN_OR, line_of(cur()));
        P++;
        g_eqbase = 16;   /* a binary operator ends a base change */
        n->a = a;
        n->b = parse_and();
        if (!n->b) return NULL;
        a = n;
    }
    return a;
}

static PNode *parse_expr(void)
{
    PNode *a = parse_or();
    while (a && at(T_XOR)) {
        PNode *n = pn(PN_XOR, line_of(cur()));
        P++;
        g_eqbase = 16;   /* a binary operator ends a base change */
        n->a = a;
        n->b = parse_or();
        if (!n->b) return NULL;
        a = n;
    }
    return a;
}

static int g_lane_err;

/* width of a set expression: 0 = number (adapts), 1 = scalar, n = list */
static int pn_width(PNode *n, const char **bad)
{
    int a, b;
    switch (n->k) {
    case PN_VAR: return n->sym->kind == SK_FIELD ? n->sym->nmem : 1;
    case PN_NUM: return 0;
    case PN_LIST: return n->nitems;
    case PN_EQ: case PN_RED: return 1;
    case PN_NOT: return pn_width(n->a, bad);
    default:
        a = pn_width(n->a, bad);
        b = pn_width(n->b, bad);
        if (a <= 1) return b > a ? b : a;
        if (b <= 1) return a;
        if (a != b) {
            if (bad && !*bad) {
                PNode **el;
                int k = elements(a > b ? n->a : n->b, &el);
                *bad = (k > 0) ? el[(a < b ? a : b)]->sym->name : "";
            }
            return a > b ? a : b;
        }
        return a;
    }
}

static int g_mixed;     /* 0019ca: a match on indexed and plain names */

/* position of each element for equality: the index when all elements are
   indexed variables, otherwise the distance from the end (the original
   then notes 0019ca) */
static int elem_pos(PNode **el, int n, int k)
{
    int i, plain = 0, idx = 0;
    for (i = 0; i < n; i++) {
        if (el[i]->sym->index >= 0) idx = 1;
        else plain = 1;
    }
    if (!plain) return el[k]->sym->index;
    if (idx) g_mixed = 1;
    return n - 1 - k;
}

/* as an assignment target: the index of an indexed variable, the
   distance from the end otherwise */
static int tgt_pos(PNode **el, int n, int k)
{
    return el[k]->sym->index >= 0 ? el[k]->sym->index : n - 1 - k;
}

static Expr *lane(PNode *n, int j, int pos);

static Expr *and2(Expr *a, Expr *b) { return a ? mk_bin(E_AND, a, b) : b; }
static Expr *or2(Expr *a, Expr *b) { return a ? mk_bin(E_OR, a, b) : b; }

/* cubes of the 32-bit interval [lo, hi] (prefix decomposition) */
static void range_cubes(unsigned long lo, unsigned long hi, unsigned long *cv, unsigned long *cm, int *nc)
{
    /* cm: mask of fixed bits, cv: their values */
    while (lo <= hi) {
        unsigned long size = 1;
        int k = 0;
        while (k < 32) {
            unsigned long s2 = size << 1;
            if ((lo & (s2 - 1)) != 0) break;
            if (lo + s2 - 1 > hi || lo + s2 - 1 < lo) break;
            size = s2;
            k++;
        }
        cv[*nc] = lo;
        cm[*nc] = (~(size - 1)) & 0xFFFFFFFFUL;
        (*nc)++;
        if (lo + size - 1 >= 0xFFFFFFFFUL) break;
        lo += size;
        if (size == 0) break;
    }
}

static Expr *match_value(PNode **el, int ne, unsigned long v, unsigned long dc, int line, int *alldc)
{
    Expr *r = NULL;
    int k, any = 0;
    for (k = 0; k < ne; k++) {
        int p = elem_pos(el, ne, k);
        Expr *x;
        if (p > 31 || ((dc >> p) & 1)) continue;
        any = 1;
        x = lane(el[k], 0, 0);
        if (!((v >> p) & 1)) x = mk_not(x);
        r = and2(r, x);
    }
    (void)line;
    if (!any) { *alldc = 1; return NULL; }
    return r;
}

static Expr *eval_eq(PNode *n)
{
    PNode **el;
    int ne = elements(n->a, &el), i, alldc = 0;
    Expr *r = NULL;
    for (i = 0; i < n->nvals; i++) {
        Val *v = &n->vals[i];
        if (!v->range || v->lo == v->hi) {
            Expr *m = match_value(el, ne, v->lo, v->dclo, n->line, &alldc);
            if (m) r = or2(r, m);
        } else {
            unsigned long cv[80], cm[80];
            unsigned long lo = v->lo & ~v->dclo, hi = v->hi | v->dchi;
            int nc = 0, c, always = 0;
            if (lo > hi) { unsigned long t = lo; lo = hi; hi = t; }
            range_cubes(lo, hi, cv, cm, &nc);
            if (ne <= 16) {
                unsigned long pat;
                always = 1;
                for (pat = 0; pat < (1UL << ne) && always; pat++) {
                    int hit = 0;
                    for (c = 0; c < nc && !hit; c++) {
                        int k, ok = 1;
                        for (k = 0; k < ne && ok; k++) {
                            int p = elem_pos(el, ne, k);
                            if (p > 31) continue;
                            if (((cm[c] >> p) & 1) && (((cv[c] >> p) & 1) != ((pat >> k) & 1))) ok = 0;
                        }
                        hit = ok;
                    }
                    if (!hit) always = 0;
                }
            }
            if (always) {
                diag(DG_ERROR, "0023ca", n->line, "invalid range arguments:  always true (in range)");
                r = or2(r, mk_const(1));
                continue;
            }
            {
                /* as the original: trailing positions where lo is 0 and hi
                   is 1 are dropped, the rest enumerated value by value */
                int tz = 0;
                unsigned long ulo, uhi;
                while (tz < 32 && !((lo >> tz) & 1) && ((hi >> tz) & 1)) tz++;
                ulo = tz >= 32 ? 0 : lo >> tz;
                uhi = tz >= 32 ? 0 : hi >> tz;
                if (ne <= 16 && uhi - ulo < (1UL << 20)) {
                    unsigned char *seen = (unsigned char *)xcalloc((size_t)1 << ne, 1);
                    unsigned long x, dcm = tz >= 32 ? 0xFFFFFFFFUL : ((1UL << tz) - 1);
                    for (x = ulo;; x++) {
                        unsigned long v2 = tz >= 32 ? 0 : (x << tz), key = 0;
                        int k;
                        for (k = 0; k < ne; k++) {
                            int p = elem_pos(el, ne, k);
                            if (p <= 31 && p >= tz && ((v2 >> p) & 1)) key |= 1UL << k;
                            if (p <= 31 && p < tz) key |= 0;
                        }
                        if (!seen[key]) {
                            Expr *m = match_value(el, ne, v2, dcm, n->line, &alldc);
                            seen[key] = 1;
                            r = or2(r, m ? m : mk_const(1));
                        }
                        if (x == uhi) break;
                    }
                    free(seen);
                    alldc = 0;
                    continue;
                }
            }
            for (c = 0; c < nc; c++) {
                Expr *m = match_value(el, ne, cv[c], ~cm[c] & 0xFFFFFFFFUL, n->line, &alldc);
                r = or2(r, m ? m : mk_const(1));
            }
            alldc = 0;
        }
    }
    if (!r) {
        diag(DG_NOTE, "0042ca", 0, "range or match list completely don't cared, decoded to 0");
        return mk_const(0);
    }
    return r;
}

static Expr *lane(PNode *n, int j, int pos)
{
    switch (n->k) {
    case PN_VAR:
        if (n->sym->kind == SK_FIELD) {
            int k = n->sym->nmem == 1 ? 0 : j;
            if (k >= n->sym->nmem) k = n->sym->nmem - 1;
            return mk_var(n->sym->mem[k], n->ext ? n->ext : n->sym->mem_ext[k], n->neg);
        }
        return mk_var(n->sym, n->ext, n->neg);
    case PN_NUM:
        if (pos > 31) return mk_const(0);
        return mk_const((int)((n->num.val >> pos) & 1));
    case PN_LIST: {
        int k = n->nitems == 1 ? 0 : j;
        if (k >= n->nitems) k = n->nitems - 1;
        return lane(n->items[k], 0, 0);
    }
    case PN_NOT: {
        Expr *x = lane(n->a, j, pos);
        if (n->paren && x->op == E_VAR) {
            Expr *e = (Expr *)xcalloc(1, sizeof(Expr));
            e->op = E_NOT;
            e->a = x;
            return e;
        }
        return mk_not(x);
    }
    case PN_AND: return mk_bin(E_AND, lane(n->a, j, pos), lane(n->b, j, pos));
    case PN_OR: return mk_bin(E_OR, lane(n->a, j, pos), lane(n->b, j, pos));
    case PN_XOR: return mk_bin(E_XOR, lane(n->a, j, pos), lane(n->b, j, pos));
    case PN_EQ: return eval_eq(n);
    case PN_RED: {
        PNode **el;
        int ne = elements(n->a, &el), k;
        Expr *r = NULL;
        for (k = 0; k < ne; k++) {
            Expr *x = lane(el[k], 0, 0);
            r = r ? mk_bin(n->redop, r, x) : x;
        }
        return r ? r : mk_const(0);
    }
    }
    return mk_const(0);
}

static Expr *scalar(PNode *n)
{
    const char *bad = NULL;
    int w = pn_width(n, &bad);
    if (bad || w > 1) {
        PNode **el;
        const char *nm = bad;
        if (!nm) { int k = elements(n, &el); nm = k > 1 ? el[1]->sym->name : ""; }
        diag(DG_ERROR, "0012ca", n->line, "vector size mismatch:  %s", nm);
        g_lane_err = 1;
    }
    return lane(n, 0, 0);
}

typedef struct { Sym *sym; int ext; int neg; int pos; } Target;

static int targets_of(LList *l, Target **out)
{
    Target *t = NULL;
    int n = 0, i, k;
    for (i = 0; i < l->n; i++) {
        Sym *s;
        if (!strcmp(l->v[i].name, LHS_NUM)) continue;
        s = sym_get(l->v[i].name);
        if (s->kind == SK_FIELD) {
            for (k = 0; k < s->nmem; k++) {
                t = (Target *)xrealloc(t, sizeof(Target) * (size_t)(n + 1));
                t[n].sym = s->mem[k];
                t[n].ext = l->v[i].ext ? l->v[i].ext : s->mem_ext[k];
                t[n].neg = l->v[i].neg;
                n++;
            }
        } else {
            t = (Target *)xrealloc(t, sizeof(Target) * (size_t)(n + 1));
            t[n].sym = s;
            t[n].ext = l->v[i].ext;
            t[n].neg = l->v[i].neg;
            n++;
        }
    }
    for (i = 0; i < n; i++) {
        if (n == 1) t[i].pos = 0;
        else t[i].pos = t[i].sym->index >= 0 ? t[i].sym->index : n - 1 - i;
    }
    *out = t;
    return n;
}

static int g_group;      /* origin group of the statements being parsed */

static int g_empty_err;

static void assign_one(Sym *s, int ext, int neg, Expr *e, int append, int line)
{
    Eqn *q = eqn_find(s, ext);
    Expr *ge = e;
    if (s->kind == SK_FIELD) return;
    if (!q || !q->e) {     /* new, or only a MIN placeholder so far */
        if (!q) q = eqn_new(s, ext, line);
        q->line = line;
        q->neg = neg;
        q->e = e;
        q->assigned = !append;
        q->npieces = 1;
        q->grp[g_group] = e;
        return;
    }
    if (!append) {
        char nm[128];
        snprintf(nm, sizeof nm, "%s%s%s", s->name, ext ? "." : "", ext ? ext_names[ext] : "");
        diag(DG_ERROR, "0011ca", line, "expression already assigned to:  %s", nm);
        return;
    }
    if (q->e->empty || e->empty) {
        /* BUG_BARE_EQN: terms appended to an empty equation replace it,
           an empty one appended to terms stops the original */
        if (!e->empty) {
            q->e = e;
            q->neg = neg;
            q->grp[g_group] = e;
        } else if (!q->e->empty) g_empty_err = 1;
        q->npieces++;
        return;
    }
    if (neg != q->neg) e = mk_not(e);
    q->e = mk_bin(E_OR, q->e, e);
    q->npieces++;
    ge = e;
    q->grp[g_group] = q->grp[g_group] ? mk_bin(E_OR, q->grp[g_group], ge) : ge;
}

static void assign(Target *t, int nt, int lhsneg, PNode *rhs, int append, int line)
{
    const char *bad = NULL;
    int w = pn_width(rhs, &bad), i;
    if (bad) { diag(DG_ERROR, "0012ca", line, "vector size mismatch:  %s", bad); return; }
    if (w > 1 && w != nt) {
        PNode **el;
        int k = elements(rhs, &el);
        const char *nm = "";
        if (k > nt && el) nm = el[nt]->sym->name;
        else if (k > 0 && el) nm = el[k - 1]->sym->name;
        diag(DG_ERROR, "0012ca", line, "vector size mismatch:  %s", nm);
        return;
    }
    for (i = 0; i < nt; i++) {
        Expr *e = lane(rhs, i, t[i].pos);
        assign_one(t[i].sym, t[i].ext, lhsneg ^ t[i].neg, e, append, line);
    }
}

static void parse_statement(void);

static void st_pin(int node)
{
    int *nums = NULL, nn = 0, i, neg = 0;
    LList l;
    Tok *t0 = cur();
    memset(&l, 0, sizeof l);
    if (node == 2) { /* NODE [!]var ; */
        if (at(T_NOT)) { neg = 1; P++; }
        if (at(T_SEMI)) { P++; return; }   /* 'NODE;' is accepted */
        if (at(T_LBRK)) { if (!parse_varlist(&l)) { recover(); return; } }
        else if (at_name()) { ll_add(&l, cur()->s, 0, 0); P++; }
        else { missing(";"); recover(); return; }
        for (i = 0; i < l.n; i++) {
            Sym *s = sym_get(l.v[i].name);
            if (s->kind != SK_NONE) diag(DG_ERROR, "0014ca", line_of(t0), "variable already declared:  %s", s->name);
            s->kind = SK_NODE;
            s->neg = neg ^ l.v[i].neg;
            s->line = line_of(t0);
        }
        expect(T_SEMI, ";");
        return;
    }
    if (at(T_EQ)) { /* virtual device: pin = var */
        nn = 0;
    } else if (!parse_numlist(&nums, &nn)) { recover(); return; }
    if (!expect(T_EQ, "=")) { recover(); return; }
    if (at(T_NOT)) { neg = 1; P++; }
    if (at(T_LBRK)) { if (!parse_varlist(&l)) { recover(); return; } }
    else if (at(T_WORD) && !is_reserved(cur()->s)) { ll_add(&l, cur()->s, 0, 0); P++; }
    else { syntax_error(cur()); recover(); return; }
    if (nn && nn != l.n) {
        diag(DG_ERROR, "0012ca", line_of(t0), "vector size mismatch:  %s", l.n ? l.v[l.n - 1].name : "");
    }
    for (i = 0; i < l.n; i++) {
        Sym *s = sym_get(l.v[i].name);
        if (s->kind != SK_NONE) {
            diag(DG_ERROR, "0014ca", line_of(t0), "variable already declared:  %s", s->name);
            continue;
        }
        s->kind = node ? SK_NODE : SK_PIN;
        s->pin = i < nn ? nums[i] : 0;
        s->neg = neg ^ l.v[i].neg;
        s->line = line_of(t0);
    }
    if (!at(T_SEMI)) { missing(";"); recover(); return; }
    P++;
}

static void st_field(void)
{
    Tok *t = cur();
    Sym *f;
    LList l;
    int i;
    memset(&l, 0, sizeof l);
    if (!at_name()) { missing("keyword or operator"); recover(); return; }
    f = sym_get(t->s);
    P++;
    if (!expect(T_EQ, "=")) { recover(); return; }
    if (!parse_varlist_x(&l, 1)) { recover(); return; }
    if (f->kind != SK_NONE && f->kind != SK_FIELD) diag(DG_ERROR, "0014ca", line_of(t), "variable already declared:  %s", f->name);
    f->kind = SK_FIELD;
    f->nmem = 0;
    f->line = line_of(t);
    for (i = 0; i < l.n; i++) {
        Sym *m = sym_get(l.v[i].name);
        if (m->kind == SK_FIELD) {
            int k;
            for (k = 0; k < m->nmem; k++) {
                f->mem = (Sym **)xrealloc(f->mem, sizeof(Sym *) * (size_t)(f->nmem + 1));
                f->mem_ext = (int *)xrealloc(f->mem_ext, sizeof(int) * (size_t)(f->nmem + 1));
                f->mem[f->nmem] = m->mem[k];
                f->mem_ext[f->nmem] = m->mem_ext[k];
                f->nmem++;
            }
            continue;
        }
        f->mem = (Sym **)xrealloc(f->mem, sizeof(Sym *) * (size_t)(f->nmem + 1));
        f->mem_ext = (int *)xrealloc(f->mem_ext, sizeof(int) * (size_t)(f->nmem + 1));
        f->mem[f->nmem] = m;
        f->mem_ext[f->nmem] = l.v[i].ext;
        f->nmem++;
    }
    if (!at(T_SEMI)) { missing(";"); recover(); return; }
    P++;
}

static void st_min(void)
{
    LList l;
    int i, ext = 0;
    Num n;
    Tok *t0 = cur();
    memset(&l, 0, sizeof l);
    if (at(T_LBRK)) { if (!parse_varlist(&l)) { recover(); return; } }
    else if (at_name()) { ll_add(&l, cur()->s, 0, 0); P++; }
    else { syntax_error(cur()); recover(); return; }
    ext = parse_ext();
    if (!expect(T_EQ, "=")) { recover(); return; }
    if (!at(T_WORD)) { syntax_error(cur()); recover(); return; }
    n = conv_num(cur()->s, 10, line_of(cur()));
    P++;
    if (!n.ok || n.val > 4) diag(DG_ERROR, "0031ca", line_of(t0), "invalid minimization level:  %lu", n.val);
    for (i = 0; i < l.n; i++) {
        Target *t;
        int nt, k;
        LList one;
        memset(&one, 0, sizeof one);
        ll_add(&one, l.v[i].name, 0, l.v[i].ext ? l.v[i].ext : ext);
        nt = targets_of(&one, &t);
        for (k = 0; k < nt; k++) {
            /* remember the level; applied when the equation exists */
            Eqn *q = eqn_find(t[k].sym, t[k].ext);
            if (!q) { q = eqn_new(t[k].sym, t[k].ext, line_of(t0)); q->e = NULL; }
            q->minlvl = (int)n.val;
        }
    }
    expect(T_SEMI, ";");
}

/* equation: [APPEND] [!]lhs[.ext] = expr ; */
static void st_equation(int append)
{
    Tok *t0 = cur();
    int neg = 0, nt;
    LList l;
    Target *t;
    PNode *rhs;
    memset(&l, 0, sizeof l);
    if (at(T_NOT)) { neg = 1; P++; }
    if (g_pendnot) { neg = 1; g_pendnot = 0; }   /* BUG_NOT_NUMBER */
    if (at(T_LBRK)) {
        int ok;
        g_lhs_list = 1;
        ok = parse_varlist(&l);
        g_lhs_list = 0;
        if (!ok) { recover(); return; }
        if (at(T_DOT)) {
            int x = parse_ext(), i;
            for (i = 0; i < l.n; i++) if (!l.v[i].ext) l.v[i].ext = x;
        }
    } else if (at(T_BASE) && peekn(1)->t == T_WORD) {
        /* as the original: a number on the left is accepted and the
           equation ignored */
        Num nn;
        if (!parse_number(&nn, 16)) { recover(); return; }
        ll_add(&l, LHS_NUM, 0, 0);
    } else if (at(T_WORD)) {
        Tok *w = cur();
        int x;
        const char *c = w->s;
        while (*c && !isalpha((unsigned char)*c) && *c != '_') c++;
        if (is_reserved(w->s) || !*c) { syntax_error(w); recover(); return; }
        P++;
        x = parse_ext();
        ll_add(&l, w->s, 0, x);
    } else { syntax_error(cur()); recover(); return; }
    if (at(T_SEMI) && !g_fix) {
        /* BUG_BARE_EQN: 'name;' is taken as an equation without an
           expression, which reads as true */
        int i;
        P++;
        diag_bug(DG_NOTE, "0009ca", line_of(t0), BUG_BARE_EQN, "a statement without '=' is taken as an equation that sets the output true");
        nt = targets_of(&l, &t);
        for (i = 0; i < nt; i++) {
            Expr *e = mk_const(1);
            e->empty = 1;
            assign_one(t[i].sym, t[i].ext, neg ^ t[i].neg, e, append, line_of(t0));
        }
        return;
    }
    if (!at(T_EQ)) {
        if (at(T_SEMI) || at(T_EOF)) {
            missing("=");
            if (at(T_SEMI)) diag_bug(DG_NOTE, "0009ca", line_of(t0), BUG_BARE_EQN, "a statement without '=' is taken as an equation that sets the output true");
        } else missing(";");
        recover();
        return;
    }
    P++;
    rhs = parse_expr();
    if (!rhs) { recover(); return; }
    if (!at(T_SEMI)) { missing(";"); recover(); return; }
    P++;
    nt = targets_of(&l, &t);
    assign(t, nt, neg, rhs, append, line_of(t0));
}

/* TABLE inlist => outlist { values => value ; ... } */
static void st_table(void)
{
    int save_group = g_group;
    Tok *t0 = cur();
    LList li, lo;
    PNode *in;
    Target *out;
    int nout, i;
    Expr **acc;
    int plast = -1;
    memset(&li, 0, sizeof li);
    memset(&lo, 0, sizeof lo);
    if (at(T_LBRK)) { if (!parse_varlist(&li)) { recover(); return; } }
    else if (at_name()) { int x; Tok *w = cur(); P++; x = parse_ext(); ll_add(&li, w->s, 0, x); }
    else { syntax_error(cur()); recover(); return; }
    if (!expect(T_ARROW, "=>")) { recover(); return; }
    if (at(T_LBRK)) { if (!parse_varlist(&lo)) { recover(); return; } }
    else if (at_name()) { int x; Tok *w = cur(); P++; x = parse_ext(); ll_add(&lo, w->s, 0, x); }
    else { syntax_error(cur()); recover(); return; }
    in = pn(PN_LIST, line_of(t0));
    for (i = 0; i < li.n; i++) {
        Sym *s = sym_get(li.v[i].name);
        s->used = 1;
        if (s->kind == SK_FIELD) {
            int k;
            for (k = 0; k < s->nmem; k++) {
                PNode *e = pn(PN_VAR, line_of(t0));
                e->sym = s->mem[k];
                e->ext = s->mem_ext[k];
                in->items = (PNode **)xrealloc(in->items, sizeof(PNode *) * (size_t)(in->nitems + 1));
                in->items[in->nitems++] = e;
            }
        } else {
            PNode *e = pn(PN_VAR, line_of(t0));
            e->sym = s;
            e->ext = li.v[i].ext;
            e->neg = li.v[i].neg;
            in->items = (PNode **)xrealloc(in->items, sizeof(PNode *) * (size_t)(in->nitems + 1));
            in->items[in->nitems++] = e;
        }
    }
    nout = targets_of(&lo, &out);
    acc = (Expr **)xcalloc((size_t)nout + 1, sizeof(Expr *));
    if (!expect(T_LBRACE, "{")) { recover(); return; }
    while (!at(T_RBRACE) && !at(T_EOF)) {
        PNode *m;
        if (P == plast) { P++; continue; }   /* no progress: skip a token */
        plast = P;
        m = pn(PN_EQ, line_of(cur()));
        Num ov;
        Expr *me;
        m->a = in;
        if (!parse_vals(m, 16)) { recover(); continue; }
        if (!expect(T_ARROW, "=>")) { recover(); continue; }
        if (!parse_number(&ov, 16)) { recover(); continue; }
        if (ov.dc) diag(DG_ERROR, "0026ca", line_of(cur()), "invalid table statement");
        if (!expect(T_SEMI, ";")) { recover(); continue; }
        me = eval_eq(m);
        /* each entry appends its match (bit 1) or 'b'0 (bit 0) */
        for (i = 0; i < nout; i++) {
            int p = nout == 1 ? 0 : out[i].pos;
            acc[i] = or2(acc[i], (p <= 31 && ((ov.val >> p) & 1)) ? me : mk_const(0));
        }
    }
    expect(T_RBRACE, "}");
    g_group = 1;
    for (i = 0; i < nout; i++)
        assign_one(out[i].sym, out[i].ext, out[i].neg, acc[i] ? acc[i] : mk_const(0), 1, line_of(t0));
    g_group = save_group;
}

typedef struct { Sym *sym; int ext; int neg; } OutRef;

static int parse_outs(OutRef **o)
{
    int n = 0;
    *o = NULL;
    while (kw("OUT")) {
        P++;
        if (at(T_LBRK)) {
            LList l;
            Target *t;
            int nt, k, neg = 0;
            memset(&l, 0, sizeof l);
            if (!parse_varlist(&l)) return -1;
            nt = targets_of(&l, &t);
            for (k = 0; k < nt; k++) {
                *o = (OutRef *)xrealloc(*o, sizeof(OutRef) * (size_t)(n + 1));
                (*o)[n].sym = t[k].sym; (*o)[n].ext = t[k].ext; (*o)[n].neg = t[k].neg ^ neg;
                n++;
            }
            continue;
        }
        {
            int neg = 0, x;
            Tok *w;
            if (at(T_NOT)) { neg = 1; P++; }
            w = cur();
            if (w->t != T_WORD || is_reserved(w->s)) { syntax_error(w); return -1; }
            P++;
            x = parse_ext();
            *o = (OutRef *)xrealloc(*o, sizeof(OutRef) * (size_t)(n + 1));
            (*o)[n].sym = sym_get(w->s);
            (*o)[n].ext = x;
            (*o)[n].neg = neg;
            n++;
        }
    }
    return n;
}

static int g_seq_type; /* 'D', 'J', 'S', 'T' */
static unsigned long g_next[256];
static int g_nnext;

static void seq_next(PNode **sv, int ns, unsigned long cur_state, unsigned long next, Expr *cond, int line)
{
    int k;
    for (k = 0; k < g_nnext; k++) if (g_next[k] == next) break;
    if (k == g_nnext && g_nnext < 256) g_next[g_nnext++] = next;
    for (k = 0; k < ns; k++) {
        /* the next state is written as an assignment, the present state
           matched as an equality: they differ for mixed names (0019ca) */
        int p = tgt_pos(sv, ns, k), q = elem_pos(sv, ns, k);
        int bit = p <= 31 ? (int)((next >> p) & 1) : 0;
        int pbit = q <= 31 ? (int)((cur_state >> q) & 1) : 0;
        Sym *s = sv[k]->sym;
        switch (g_seq_type) {
        case 'D':
            /* a 0 bit appends 'b'0 (the register resets implicitly); the
               reference keeps such constant terms at -m0 */
            assign_one(s, X_D, 0, bit ? cond : mk_const(0), 1, line);
            break;
        case 'J':
            if (bit && !pbit) assign_one(s, X_J, 0, cond, 1, line);
            if (!bit && pbit) assign_one(s, X_K, 0, cond, 1, line);
            if (!eqn_find(s, X_J)) assign_one(s, X_J, 0, mk_const(0), 1, line);
            if (!eqn_find(s, X_K)) assign_one(s, X_K, 0, mk_const(0), 1, line);
            break;
        case 'S':
            if (bit && !pbit) assign_one(s, X_S, 0, cond, 1, line);
            if (!bit && pbit) assign_one(s, X_R, 0, cond, 1, line);
            if (!eqn_find(s, X_S)) assign_one(s, X_S, 0, mk_const(0), 1, line);
            if (!eqn_find(s, X_R)) assign_one(s, X_R, 0, mk_const(0), 1, line);
            break;
        case 'T':
            if (bit != pbit) assign_one(s, X_T, 0, cond, 1, line);
            break;
        }
    }
}

static void seq_out(OutRef *o, int no, Expr *cond, int sync, int line)
{
    int i;
    for (i = 0; i < no; i++) {
        if (!sync) {
            if (!o[i].neg) assign_one(o[i].sym, o[i].ext, 0, cond, 1, line);
            continue;
        }
        switch (g_seq_type) {
        case 'D':
            /* a negated output just resets: it appends 'b'0 */
            assign_one(o[i].sym, X_D, 0, o[i].neg ? mk_const(0) : cond, 1, line);
            break;
        case 'J':
            assign_one(o[i].sym, o[i].neg ? X_K : X_J, 0, cond, 1, line);
            if (!eqn_find(o[i].sym, X_J)) assign_one(o[i].sym, X_J, 0, mk_const(0), 1, line);
            if (!eqn_find(o[i].sym, X_K)) assign_one(o[i].sym, X_K, 0, mk_const(0), 1, line);
            break;
        case 'S':
            assign_one(o[i].sym, o[i].neg ? X_R : X_S, 0, cond, 1, line);
            if (!eqn_find(o[i].sym, X_S)) assign_one(o[i].sym, X_S, 0, mk_const(0), 1, line);
            if (!eqn_find(o[i].sym, X_R)) assign_one(o[i].sym, X_R, 0, mk_const(0), 1, line);
            break;
        case 'T':
            assign_one(o[i].sym, X_T, 0, cond, 1, line);
            break;
        }
    }
}

static int parse_state_num(unsigned long *v)
{
    Num n;
    if (!parse_number(&n, 16)) return 0;
    *v = n.val;
    return 1;
}

static void st_sequence(int type)
{
    Tok *t0 = cur();
    LList l;
    PNode *svn;
    PNode **sv;
    int ns, i;
    unsigned long *seen = NULL;
    int nseen = 0, plast = -1;
    int save_group = g_group;
    memset(&l, 0, sizeof l);
    g_seq_type = type;
    g_nnext = 0;
    g_des.nseq++;
    g_group = 2;
    if (at(T_LBRK)) { if (!parse_varlist(&l)) { recover(); return; } }
    else if (at_name()) { ll_add(&l, cur()->s, 0, 0); P++; }
    else { syntax_error(cur()); recover(); return; }
    svn = pn(PN_LIST, line_of(t0));
    for (i = 0; i < l.n; i++) {
        Sym *s = sym_get(l.v[i].name);
        s->used = 1;
        if (s->kind == SK_FIELD) {
            int k;
            for (k = 0; k < s->nmem; k++) {
                PNode *e = pn(PN_VAR, line_of(t0));
                e->sym = s->mem[k];
                svn->items = (PNode **)xrealloc(svn->items, sizeof(PNode *) * (size_t)(svn->nitems + 1));
                svn->items[svn->nitems++] = e;
            }
        } else {
            PNode *e = pn(PN_VAR, line_of(t0));
            e->sym = s;
            svn->items = (PNode **)xrealloc(svn->items, sizeof(PNode *) * (size_t)(svn->nitems + 1));
            svn->items[svn->nitems++] = e;
        }
    }
    sv = svn->items;
    ns = svn->nitems;
    if (!expect(T_LBRACE, "{")) { recover(); return; }
    while (!at(T_RBRACE) && !at(T_EOF)) {
        unsigned long st;
        Expr *present;
        if (P == plast) { P++; continue; }
        plast = P;
        Expr *ifsum = NULL, *ifsum_out = NULL; /* IF..NEXT and IF..OUT conditions */
        int line, have_uncond_next = 0;
        if (!kw("PRESENT")) { syntax_error(cur()); recover(); continue; }
        line = line_of(cur());
        P++;
        if (!parse_state_num(&st)) { recover(); continue; }
        for (i = 0; i < nseen; i++)
            if (seen[i] == st) diag(DG_ERROR, "0037ca", line, "duplicate present state:  %lX", st);
        seen = (unsigned long *)xrealloc(seen, sizeof(unsigned long) * (size_t)(nseen + 1));
        seen[nseen++] = st;
        present = match_value(sv, ns, st, 0, line, &i);
        if (!present) present = mk_const(1);
        for (;;) {
            if (kw("IF")) {
                PNode *c;
                Expr *ce, *cond;
                OutRef *o;
                int no;
                P++;
                c = parse_expr();
                if (!c) { recover(); break; }
                ce = scalar(c);
                cond = mk_bin(E_AND, present, ce);
                if (kw("NEXT")) ifsum = or2(ifsum, ce);
                else ifsum_out = or2(ifsum_out, ce);
                if (kw("NEXT")) {
                    unsigned long nx;
                    P++;
                    if (!parse_state_num(&nx)) { recover(); break; }
                    seq_next(sv, ns, st, nx, cond, line_of(t0));
                    no = parse_outs(&o);
                    if (no < 0) { recover(); break; }
                    seq_out(o, no, cond, 1, line);
                } else {
                    no = parse_outs(&o);
                    if (no < 0) { recover(); break; }
                    seq_out(o, no, cond, 0, line);
                }
                if (!expect(T_SEMI, ";")) { recover(); break; }
            } else if (kw("DEFAULT")) {
                Expr *cond, *dc;
                OutRef *o;
                int no;
                P++;
                /* DEFAULT NEXT complements the IF..NEXT conditions, DEFAULT OUT
                   the conditions of the IF..OUT statements */
                if (kw("NEXT")) dc = ifsum ? mk_not(ifsum) : mk_const(1);
                else dc = ifsum_out ? mk_not(ifsum_out) : mk_const(1);
                cond = mk_bin(E_AND, present, dc);
                if (kw("NEXT")) {
                    unsigned long nx;
                    P++;
                    if (!parse_state_num(&nx)) { recover(); break; }
                    seq_next(sv, ns, st, nx, cond, line);
                    no = parse_outs(&o);
                    if (no < 0) { recover(); break; }
                    seq_out(o, no, cond, 1, line);
                } else {
                    no = parse_outs(&o);
                    if (no < 0) { recover(); break; }
                    seq_out(o, no, cond, 0, line);
                }
                if (!expect(T_SEMI, ";")) { recover(); break; }
            } else if (kw("NEXT")) {
                unsigned long nx;
                OutRef *o;
                int no;
                P++;
                if (have_uncond_next) diag(DG_ERROR, "0035ca", line, "multiple unconditional statements for state:  %lX", st);
                have_uncond_next = 1;
                if (!parse_state_num(&nx)) { recover(); break; }
                seq_next(sv, ns, st, nx, present, line);
                no = parse_outs(&o);
                if (no < 0) { recover(); break; }
                seq_out(o, no, present, 1, line);
                if (!expect(T_SEMI, ";")) { recover(); break; }
            } else if (kw("OUT")) {
                OutRef *o;
                int no = parse_outs(&o);
                if (no < 0) { recover(); break; }
                seq_out(o, no, present, 0, line);
                if (!expect(T_SEMI, ";")) { recover(); break; }
            } else break;
        }
    }
    expect(T_RBRACE, "}");
    /* every state bit gets a next-state equation (0 when never set); an
       empty sequence gives none (as the reference) */
    if (type == 'D' && nseen > 0)
        for (i = 0; i < ns; i++)
            if (!eqn_find(sv[i]->sym, X_D)) assign_one(sv[i]->sym, X_D, 0, mk_const(0), 1, line_of(t0));
    for (i = 0; i < g_nnext; i++) {
        int k, found = 0;
        for (k = 0; k < nseen; k++) if (seen[k] == g_next[i]) found = 1;
        if (!found) diag(DG_ERROR, "0032ca", line_of(t0), "invalid next state:  'h'%lX", g_next[i]);
    }
    g_group = save_group;
}

static void st_condition(void)
{
    Expr *sum = NULL;
    int plast = -1, save_group = g_group;
    if (!at(T_LBRACE)) { syntax_error(cur()); recover(); return; }
    P++;
    while (!at(T_RBRACE) && !at(T_EOF)) {
        if (P == plast) { P++; continue; }
        plast = P;
        if (kw("IF")) {
            PNode *c;
            Expr *ce;
            OutRef *o;
            int no, line = line_of(cur());
            P++;
            c = parse_expr();
            if (!c) { recover(); continue; }
            ce = scalar(c);
            sum = or2(sum, ce);
            no = parse_outs(&o);
            if (no < 0) { recover(); continue; }
            g_group = 2;
            seq_out(o, no, ce, 0, line);
            g_group = save_group;
            expect(T_SEMI, ";");
        } else if (kw("DEFAULT")) {
            OutRef *o;
            int no, line = line_of(cur());
            P++;
            no = parse_outs(&o);
            if (no < 0) { recover(); continue; }
            g_group = 3;
            seq_out(o, no, sum ? mk_not(sum) : mk_const(1), 0, line);
            g_group = save_group;
            expect(T_SEMI, ";");
        } else { syntax_error(cur()); recover(); }
    }
    expect(T_RBRACE, "}");
}

static void st_function(void)
{
    Tok *t = cur();
    Sym *f;
    int depth;
    if (!at_name()) { missing("keyword or operator"); recover(); return; }
    f = sym_get(t->s);
    P++;
    if (f->kind != SK_NONE) diag(DG_ERROR, "0014ca", line_of(t), "variable already declared:  %s", f->name);
    f->kind = SK_FUNC;
    if (!expect(T_LPAR, "(")) { recover(); return; }
    while (!at(T_RPAR)) {
        if (at(T_BASE) && peekn(1)->t == T_WORD) {
            /* as the original: a number is accepted as a parameter; it
               matches no name in the body */
            Num nn;
            if (!parse_number(&nn, 16)) { recover(); return; }
            if (at(T_RPAR)) continue;   /* a last one is not counted */
            f->params = (char **)xrealloc(f->params, sizeof(char *) * (size_t)(f->nparams + 1));
            f->params[f->nparams++] = xstrdup(LHS_NUM);
            if (at(T_COMMA)) P++;
            else if (!at(T_RPAR)) { missing(")"); recover(); return; }
            continue;
        }
        if (!at_name() || !has_alpha(cur()->s)) { syntax_error(cur()); recover(); return; }
        f->params = (char **)xrealloc(f->params, sizeof(char *) * (size_t)(f->nparams + 1));
        f->params[f->nparams++] = xstrdup(cur()->s);
        P++;
        if (at(T_COMMA)) P++;
        else if (!at(T_RPAR)) { missing(")"); recover(); return; }
    }
    P++;
    if (!expect(T_LBRACE, "{")) { recover(); return; }
    f->body0 = P;
    depth = 0;
    while (!at(T_EOF)) {
        if (at(T_LBRACE)) depth++;
        else if (at(T_RBRACE)) { if (!depth) break; depth--; }
        P++;
    }
    f->body1 = P;
    expect(T_RBRACE, "}");
}

static Tok *g_base_tok;
static int g_base_ntok;

static Sym *call_function(Sym *f, int line)
{
    Tok **argv = NULL;
    int *argn = NULL, na = 0, depth = 0, start, i, k;
    Tok *nt = NULL;
    int nnt = 0;
    char hname[160];
    Sym *h;
    Tok *saveT = T;
    int saveNT = NT, saveP;
    P++;
    start = P;
    while (!at(T_EOF)) {
        if (at(T_LPAR)) depth++;
        else if (at(T_RPAR) && depth > 0) depth--;
        else if ((at(T_COMMA) || at(T_RPAR)) && depth == 0) {
            argv = (Tok **)xrealloc(argv, sizeof(Tok *) * (size_t)(na + 1));
            argn = (int *)xrealloc(argn, sizeof(int) * (size_t)(na + 1));
            argv[na] = &T[start];
            argn[na] = P - start;
            na++;
            if (at(T_RPAR)) break;
            start = P + 1;
        }
        P++;
    }
    if (!at(T_RPAR)) { missing(")"); return NULL; }
    P++;
    if (na == 1 && argn[0] == 0 && f->nparams == 0) na = 0;
    if (na != f->nparams) {
        diag(DG_ERROR, "0016ca", line, "invalid number of function arguments:  %d", na);
        return NULL;
    }
    snprintf(hname, sizeof hname, "%s(%d)", f->name, ++g_des.nhidden);
    h = sym_get(hname);
    h->hidden = 1;
    for (i = f->body0; i < f->body1; i++) {
        Tok *b = &g_base_tok[i];
        int pi = -1;
        if (b->t == T_WORD) {
            for (k = 0; k < f->nparams; k++) if (!strcmp(f->params[k], b->s)) pi = k;
        }
        if (pi >= 0) {
            int wrap = argn[pi] > 1;
            if (wrap) { nt = (Tok *)xrealloc(nt, sizeof(Tok) * (size_t)(nnt + 1)); nt[nnt] = *b; nt[nnt].t = T_LPAR; nt[nnt].s = (char *)"("; nnt++; }
            for (k = 0; k < argn[pi]; k++) {
                nt = (Tok *)xrealloc(nt, sizeof(Tok) * (size_t)(nnt + 1));
                nt[nnt++] = argv[pi][k];
            }
            if (wrap) { nt = (Tok *)xrealloc(nt, sizeof(Tok) * (size_t)(nnt + 1)); nt[nnt] = *b; nt[nnt].t = T_RPAR; nt[nnt].s = (char *)")"; nnt++; }
        } else if (b->t == T_WORD && !strcmp(b->s, f->name)) {
            nt = (Tok *)xrealloc(nt, sizeof(Tok) * (size_t)(nnt + 1));
            nt[nnt] = *b;
            nt[nnt].s = h->name;
            nnt++;
        } else {
            nt = (Tok *)xrealloc(nt, sizeof(Tok) * (size_t)(nnt + 1));
            nt[nnt++] = *b;
        }
    }
    nt = (Tok *)xrealloc(nt, sizeof(Tok) * (size_t)(nnt + 1));
    memset(&nt[nnt], 0, sizeof(Tok));
    nt[nnt].t = T_EOF;
    nt[nnt].pl = nnt ? nt[nnt - 1].pl : 0;
    nnt++;
    saveP = P;
    T = nt;
    NT = nnt;
    P = 0;
    while (!at(T_EOF)) {
        int p0 = P;
        parse_statement();
        if (P == p0) P++;
    }
    T = saveT;
    NT = saveNT;
    P = saveP;
    if (!eqn_find(h, X_NONE)) assign_one(h, X_NONE, 0, mk_const(0), 0, line);
    else {
        /* as the reference: the value of a function call is its body ORed
           with a constant 0 (-m0 keeps one constant-false term for it) */
        Eqn *q = eqn_find(h, X_NONE);
        int g;
        if (q->e) q->e = mk_bin(E_OR, q->e, mk_const(0));
        for (g = 0; g < 4; g++) if (q->grp[g]) { q->grp[g] = mk_bin(E_OR, q->grp[g], mk_const(0)); break; }
    }
    return h;
}

/* PROPERTY name { ... } ;  Without a name and a body the original crashes
   (B031); the default mode stops there, as it does. */
static void prop_crash(int said)
{
    if (!said) syntax_error(cur());
    diag_bug(DG_NOTE, "0007ca", line_of(cur()), BUG_PROP_CRASH, "a PROPERTY statement without a name and a { } body makes WinCUPL crash");
    if (!g_fix) { while (!at(T_EOF)) P++; return; }
    recover();
}

static void st_property(void)
{
    int depth = 0;
    if (!at_name() || isdigit((unsigned char)cur()->s[0])) { prop_crash(0); return; }
    P++;
    if (!at(T_LBRACE)) { syntax_error(cur()); prop_crash(1); return; }
    while (!at(T_EOF)) {
        if (at(T_LBRACE)) depth++;
        else if (at(T_RBRACE) && --depth == 0) break;
        P++;
    }
    if (at(T_EOF)) { prop_crash(0); return; }
    P++;
    if (!at(T_SEMI)) { missing(";"); return; }
    P++;
}

static void st_fuse(void)
{
    Num a, v;
    int line = line_of(cur());
    if (!expect(T_LPAR, "(")) { recover(); return; }
    if (!at(T_WORD)) { syntax_error(cur()); recover(); return; }
    a = conv_num(cur()->s, 10, line);
    P++;
    if (!expect(T_COMMA, ",")) { recover(); return; }
    if (!at(T_WORD)) { syntax_error(cur()); recover(); return; }
    v = conv_num(cur()->s, 10, line);
    P++;
    if (!expect(T_RPAR, ")")) { recover(); return; }
    if (at(T_SEMI)) P++;
    else diag(DG_ERROR, "0025cx", line, "unexpected end-of-file");
    diag(DG_WARN, "0904ca", line, "FUSE statement: fuse %lu set to %lu", a.val, v.val);
    if (g_des.nfuse < 64) {
        g_des.fuse_addr[g_des.nfuse] = (int)a.val;
        g_des.fuse_val[g_des.nfuse] = v.val > 1 ? 2 : (int)v.val;
        g_des.nfuse++;
    }
}

int g_name_twice;
static char *g_hkw[13];     /* header values by spelling (hdr_kw) */
static int g_hdr_shift;

/* the other spelling of REVISION/REV, ASSEMBLY/ASSY, LOCATION/LOC */
static int hdr_alias(int k)
{
    switch (k) {
    case 2: return 3; case 3: return 2;
    case 7: return 8; case 8: return 7;
    case 9: return 10; case 10: return 9;
    }
    return -1;
}

/* the reference points at the ';' that ends a header statement */
static void hdr_context(const Tok *t)
{
    if (t->epl >= 0 && t->epl < g_pp->n) diag_context(src_line(t->epl), g_pp->v[t->epl].text, t->ecol);
}

/* BUG_HDR_ALIAS: the original keeps both spellings as separate entries,
   REV before REVISION, ASSEMBLY before ASSY, LOC before LOCATION, and fills
   the eight header fields from that list, so the fields after a doubled
   item take the value of the one before. */
static void hdr_fields(void)
{
    static const int ord[8][2] = { {0, -1}, {1, -1}, {3, 2}, {4, -1}, {5, -1}, {6, -1}, {7, 8}, {10, 9} };
    char *v[16];
    int n = 0, i, j;
    for (i = 0; i < 8; i++) {
        int c = 0;
        for (j = 0; j < 2; j++) if (ord[i][j] >= 0 && g_hkw[ord[i][j]]) { v[n++] = g_hkw[ord[i][j]]; c++; }
        if (!c) v[n++] = NULL;
    }
    for (i = 0; i < 8; i++) g_des.hdr[i] = v[i];
}


static void parse_statement(void)
{
    Tok *t = cur();
    g_errs_here = 0;
    if (t->t == T_SEMI) { P++; return; }
    if (t->t == T_HDR) {
        int h = t->hdr, k = t->hkw, al = hdr_alias(k);
        if (h == H_DEVICE && !t->s[0]) {
            hdr_context(t);
            diag(DG_ERROR, "0007ca", line_of(t), "invalid syntax:  ;");
            diag_bug(DG_NOTE, "0007ca", line_of(t), BUG_PROP_CRASH, "an empty Device statement makes WinCUPL crash");
            if (!g_fix) { while (!at(T_EOF)) P++; return; }
        } else if (h == H_NAME && !t->s[0]) {
            hdr_context(t);
            diag(DG_ERROR, "0040ca", line_of(t), "invalid or duplicate header name:  ");
        } else if (g_des.hdr[h] && (h == H_DEVICE || h == H_FORMAT)) {
            /* as the reference: a second Device or Format is accepted, the
               first one counts */
        } else if (g_hkw[k] && h == H_NAME && !g_fix) {
            /* BUG_HEADER_LATE: a second Name loses the device */
            diag_bug(DG_NOTE, "0038ca", line_of(t), BUG_HEADER_LATE, "a second Name statement makes WinCUPL lose the target device");
            g_name_twice = 1;
        } else if (g_hkw[k] || (g_fix && al >= 0 && g_hkw[al])) {
            hdr_context(t);
            diag(DG_ERROR, "0021ca", line_of(t), "header item already declared");
            if (h == H_NAME) diag_bug(DG_NOTE, "0021ca", line_of(t), BUG_HEADER_LATE, "a second Name statement is reported as a repeated header item");
            if (!g_hkw[k]) diag_bug(DG_NOTE, "0021ca", line_of(t), BUG_HDR_ALIAS, "a header item given under both of its names shifts the JED header fields");
        } else {
            if (al >= 0 && g_hkw[al]) {
                diag_bug(DG_NOTE, "0021ca", line_of(t), BUG_HDR_ALIAS, "a header item given under both of its names shifts the JED header fields");
                g_hdr_shift = 1;
            }
            g_hkw[k] = t->s;
            if (!g_des.hdr[h]) { g_des.hdr[h] = t->s; g_des.hdr_line[h] = line_of(t); }
        }
        P++;
        return;
    }
    if (t->t == T_WORD) {
        if (kw("PIN")) { P++; st_pin(0); return; }
        if (kw("PINNODE")) { P++; st_pin(1); return; }
        if (kw("NODE")) { P++; st_pin(2); return; }
        if (kw("FIELD") || kw("FLD") || kw("GROUP")) { P++; st_field(); return; }
        if (kw("MIN")) { P++; st_min(); return; }
        if (kw("APPEND")) { P++; st_equation(1); return; }
        if (kw("TABLE")) { P++; st_table(); return; }
        if (kw("SEQUENCE") || kw("SEQUENCED")) { P++; st_sequence('D'); return; }
        if (kw("SEQUENCEJK")) { P++; st_sequence('J'); return; }
        if (kw("SEQUENCERS")) { P++; st_sequence('S'); return; }
        if (kw("SEQUENCET")) { P++; st_sequence('T'); return; }
        if (kw("CONDITION")) { P++; st_condition(); return; }
        if (kw("FUNCTION")) { P++; st_function(); return; }
        if (kw("PROPERTY")) { P++; st_property(); return; }
        if (kw("FUSE") && peekn(1)->t == T_LPAR) { P++; st_fuse(); return; }
        {
            Sym *s = sym_find(t->s);
            if (s && s->kind == SK_FUNC && peekn(1)->t == T_LPAR) {
                P++;
                call_function(s, line_of(t));
                if (at(T_SEMI)) P++;
                return;
            }
        }
    }
    if (t->t == T_WORD || t->t == T_NOT || t->t == T_LBRK || t->t == T_BASE) { st_equation(0); return; }
    syntax_error(t);
    recover();
}

static int empty_sym(const Sym *s)
{
    Eqn *q;
    if (s->kind != SK_NONE) return 0;
    q = eqn_find(s, X_NONE);
    return q && q->e && q->e->empty;
}

static int has_empty(const Expr *e)
{
    if (!e) return 0;
    if (e->op == E_VAR) return empty_sym(e->sym);
    return has_empty(e->a) || has_empty(e->b);
}

/* BUG_BARE_EQN: an intermediate without an expression may only be read
   alone (it is then true); inside an operator the original stops with
   1014cb. */
static void empty_pass(void)
{
    int i, g, changed = 1;
    while (changed) {
        changed = 0;
        for (i = 0; i < g_des.neqns; i++) {
            Eqn *q = g_des.eqns[i];
            Expr *e = q->e;
            if (e && e->op == E_VAR && !e->neg && !e->ext && empty_sym(e->sym)) {
                Expr *c = mk_const(1);
                c->empty = 1;
                for (g = 0; g < 4; g++) if (q->grp[g] == e) q->grp[g] = c;
                q->e = c;
                changed = 1;
            }
        }
    }
    for (i = 0; i < g_des.neqns; i++) {
        Eqn *q = g_des.eqns[i];
        if (q->e && !q->e->empty && has_empty(q->e)) g_empty_err = 1;
    }
    if (g_empty_err) diag(DG_ERROR, "1014cb", 0, "program  error:  ");
}

int parse_design(PPOut *pp)
{
    g_pp = pp;
    g_ntok = 0;
    memset(g_hkw, 0, sizeof g_hkw);
    g_hdr_shift = g_empty_err = 0;
    {
        /* the reference cannot handle (preprocessed) lines longer than
           PP_MAXLINE characters */
        int i;
        for (i = 0; i < pp->n; i++)
            if (strlen(pp->v[i].text) > PP_MAXLINE)
                diag(DG_ERROR, "0039ca", pp->v[i].line, "line exceeds maximum length");
    }
    lex_all();
    g_base_tok = g_tok;
    g_base_ntok = g_ntok;
    T = g_tok;
    NT = g_ntok;
    P = 0;
    while (!at(T_EOF)) {
        int p0 = P;
        g_mixed = 0;
        parse_statement();
        if (g_mixed && P > 0) {
            Tok *e = &T[P - 1];
            if (e->pl >= 0 && e->pl < g_pp->n) diag_context(src_line(e->pl), g_pp->v[e->pl].text, e->col);
            diag(DG_NOTE, "0019ca", line_of(e), "indexed and non-indexed vars in range or match expression");
        }
        if (P == p0) P++;   /* always make progress after an error */
    }
    {
        int k;
        g_des.hdr_missing = 0;
        for (k = 0; k < 8; k++) if (!g_des.hdr[k]) g_des.hdr_missing = 1;
    }
    if (g_hdr_shift && !g_fix) hdr_fields();
    empty_pass();
    return g_diag.nerr ? -1 : 0;
}
