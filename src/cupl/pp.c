/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - CUPL preprocessor.  DARKSOFT.
   As the original: directives are recognised even inside comments;
   $DEFINE replaces text inside longer words, longest name first. */
#include "pp.h"
#include "diag.h"
#include "bugs.h"
#include "util.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *name, *val; } Def;
typedef struct { char *text; int file, line; } SLine;
typedef struct {
    char *name;
    char **params; int np;
    SLine *body; int nbody;
} Macro;
typedef struct { char *name; long val; } EnvVar;

static Def *g_defs; static int g_ndefs;
static Macro *g_mac; static int g_nmac;
static EnvVar g_env[64]; static int g_nenv;
static int g_expand_depth;      /* >0 inside $REPEAT / macro text */
static PPOut *g_out;
static char g_dir[1024];
/* conditional stack: 1 = active, 0 = skipping this branch, 2 = skipping an
   enclosing branch (nested) */
static int g_cond[64]; static int g_ncond;
static char **g_src0; static int g_nsrc0;
static int g_last_line;     /* processing the last line of the source file */
static int g_depth;
static struct {
    int incomment, stmt, inpin, st, line;
    char prev[64];
} g_fs;
static int g_stop;
static int g_rep_line;      /* $REPEAT body: faults are reported at $REPEND */

#define MAX_IFDEF 12

static int skipping(void)
{
    int i;
    for (i = 0; i < g_ncond; i++) if (g_cond[i] != 1) return 1;
    return 0;
}

static void emit(const char *text, int file, int line)
{
    PPOut *o = g_out;
    if (o->n == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 256;
        o->v = (PLine *)xrealloc(o->v, sizeof(PLine) * (size_t)o->cap);
    }
    o->v[o->n].text = xstrdup(text);
    o->v[o->n].file = file;
    o->v[o->n].line = line;
    o->n++;
}

typedef struct { const char *p; int ok; } AP;

static void ap_ws(AP *a) { while (*a->p == ' ' || *a->p == '\t') a->p++; }
static long ap_sum(AP *a);

static long ilog(long base, long v)
{
    long r = 0, p = 1;
    if (v <= 1) return 0;
    while (p < v) { p *= base; r++; }
    return r;
}

static long ap_prim(AP *a)
{
    ap_ws(a);
    if (*a->p == '(') {
        long v;
        a->p++;
        v = ap_sum(a);
        ap_ws(a);
        if (*a->p == ')') a->p++; else a->ok = 0;
        return v;
    }
    if (*a->p == '-') { a->p++; return -ap_prim(a); }
    if (*a->p == '+') { a->p++; return ap_prim(a); }
    if (isdigit((unsigned char)*a->p)) {
        long v = 0;
        while (isdigit((unsigned char)*a->p)) v = v * 10 + (*a->p++ - '0');
        if (isalpha((unsigned char)*a->p) || *a->p == '_') a->ok = 0;
        return v;
    }
    if (isalpha((unsigned char)*a->p) || *a->p == '_') {
        char nm[64];
        int n = 0, i;
        while ((isalnum((unsigned char)*a->p) || *a->p == '_') && n < 63) nm[n++] = *a->p++;
        nm[n] = 0;
        for (i = g_nenv - 1; i >= 0; i--)
            if (!strcmp(g_env[i].name, nm)) return g_env[i].val;
        if (str_ieq(nm, "LOG2") || str_ieq(nm, "LOG8") || str_ieq(nm, "LOG16") || str_ieq(nm, "LOG")) {
            long base = str_ieq(nm, "LOG2") ? 2 : str_ieq(nm, "LOG8") ? 8 : str_ieq(nm, "LOG16") ? 16 : 10;
            long v;
            ap_ws(a);
            if (*a->p != '(') { a->ok = 0; return 0; }
            v = ap_prim(a);
            return ilog(base, v);
        }
        a->ok = 0;
        return 0;
    }
    a->ok = 0;
    return 0;
}

static long ap_pow(AP *a)
{
    long b = ap_prim(a);
    ap_ws(a);
    if (a->p[0] == '*' && a->p[1] == '*') {
        long e, r = 1;
        a->p += 2;
        e = ap_pow(a);
        while (e-- > 0) r *= b;
        return r;
    }
    return b;
}

static long ap_mul(AP *a)
{
    long v = ap_pow(a);
    for (;;) {
        ap_ws(a);
        if (a->p[0] == '*' && a->p[1] != '*') { a->p++; v *= ap_pow(a); }
        else if (a->p[0] == '/') { long d; a->p++; d = ap_pow(a); if (d) v /= d; else a->ok = 0; }
        else if (a->p[0] == '%') { long d; a->p++; d = ap_pow(a); if (d) v %= d; else a->ok = 0; }
        else return v;
    }
}

static long ap_sum(AP *a)
{
    long v = ap_mul(a);
    for (;;) {
        ap_ws(a);
        if (a->p[0] == '+') { a->p++; v += ap_mul(a); }
        else if (a->p[0] == '-') { a->p++; v -= ap_mul(a); }
        else return v;
    }
}

/* evaluate the text between braces; returns 1 and the value if it is a
   complete arithmetic expression */
static int eval_arith(const char *s, size_t n, long *val)
{
    char *t = xstrndup(s, n);
    AP a;
    a.p = t;
    a.ok = 1;
    *val = ap_sum(&a);
    ap_ws(&a);
    if (*a.p) a.ok = 0;
    free(t);
    return a.ok;
}

/* replace {expr} by its value where it evaluates */
static char *eval_braces(const char *s)
{
    Buf b = {0, 0, 0};
    while (*s) {
        if (*s == '{') {
            const char *e = strchr(s + 1, '}');
            long v;
            if (e && !memchr(s + 1, '{', (size_t)(e - s - 1)) && eval_arith(s + 1, (size_t)(e - s - 1), &v)) {
                buf_printf(&b, "%ld", v);
                s = e + 1;
                continue;
            }
        }
        buf_putc(&b, *s++);
    }
    buf_putc(&b, 0);
    return b.p;
}

static char *subst_defines(const char *s)
{
    Buf b = {0, 0, 0};
    if (!g_ndefs) return xstrdup(s);
    while (*s) {
        int i, best = -1;
        size_t bl = 0;
        for (i = 0; i < g_ndefs; i++) {
            size_t l = strlen(g_defs[i].name);
            if (l > bl && !strncmp(s, g_defs[i].name, l)) { best = i; bl = l; }
        }
        if (best >= 0) {
            buf_puts(&b, g_defs[best].val);
            s += bl;
        } else buf_putc(&b, *s++);
    }
    buf_putc(&b, 0);
    return b.p;
}

static int find_def(const char *n)
{
    int i;
    for (i = 0; i < g_ndefs; i++) if (!strcmp(g_defs[i].name, n)) return i;
    return -1;
}

static SLine *split_lines(const char *buf, int file, int *n)
{
    SLine *v = NULL;
    int cnt = 0, cap = 0, ln = 1;
    const char *p = buf;
    while (*p) {
        const char *e = p;
        size_t l;
        while (*e && *e != '\n') e++;
        l = (size_t)(e - p);
        if (l && p[l - 1] == '\r') l--;
        if (cnt == cap) { cap = cap ? cap * 2 : 128; v = (SLine *)xrealloc(v, sizeof(SLine) * (size_t)cap); }
        v[cnt].text = xstrndup(p, l);
        v[cnt].file = file;
        v[cnt].line = ln++;
        cnt++;
        p = *e ? e + 1 : e;
    }
    *n = cnt;
    return v;
}

static int add_file(const char *name)
{
    g_out->files = (char **)xrealloc(g_out->files, sizeof(char *) * (size_t)(g_out->nfiles + 1));
    g_out->files[g_out->nfiles] = xstrdup(name);
    return g_out->nfiles++;
}

static char *read_source(const char *path, size_t *len)
{
    char *b = read_file(path, len);
    if (!b && g_dir[0]) {
        char full[2048];
        snprintf(full, sizeof full, "%s%s", g_dir, path);
        b = read_file(full, len);
    }
    return b;
}

static void process_block(SLine *L, int n);

static const char *skip_ws(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }

static char *word(const char **ps)
{
    const char *s = skip_ws(*ps), *e = s;
    while (*e && *e != ' ' && *e != '\t') e++;
    *ps = e;
    return xstrndup(s, (size_t)(e - s));
}

/* index of the matching end directive for a block starting at i */
static int find_end(SLine *L, int n, int i, const char *open, const char *close)
{
    int depth = 0, k;
    for (k = i + 1; k < n; k++) {
        const char *t = L[k].text;
        if (t[0] != '$') continue;
        if (str_nieq(t + 1, open, strlen(open)) && !isalpha((unsigned char)t[1 + strlen(open)])) depth++;
        else if (str_nieq(t + 1, close, strlen(close)) && !isalpha((unsigned char)t[1 + strlen(close)])) {
            if (depth == 0) return k;
            depth--;
        }
    }
    return -1;
}

/* parse "[a,b..c,...]" into values; returns count or -1 */
static int parse_repeat_list(const char *s, long **vals)
{
    long *v = NULL;
    int n = 0;
    s = skip_ws(s);
    if (*s != '[') return -1;
    s++;
    for (;;) {
        long a, b;
        char *e;
        s = skip_ws(s);
        if (*s == '{') { /* allow {expr} */
            const char *c = strchr(s, '}');
            if (!c || !eval_arith(s + 1, (size_t)(c - s - 1), &a)) return -1;
            s = c + 1;
        } else {
            a = strtol(s, &e, 10);
            if (e == s) return -1;
            s = e;
        }
        s = skip_ws(s);
        b = a;
        if (s[0] == '.' && s[1] == '.') {
            s = skip_ws(s + 2);
            if (*s == '{') {
                const char *c = strchr(s, '}');
                if (!c || !eval_arith(s + 1, (size_t)(c - s - 1), &b)) return -1;
                s = c + 1;
            } else {
                b = strtol(s, &e, 10);
                if (e == s) return -1;
                s = e;
            }
            s = skip_ws(s);
        }
        if (a < 0 || b < 0 || a > 1023 || b > 1023) { free(v); return -1; }
        if (a <= b) { long k; for (k = a; k <= b; k++) { v = (long *)xrealloc(v, sizeof(long) * (size_t)(n + 1)); v[n++] = k; } }
        else { long k; for (k = a; k >= b; k--) { v = (long *)xrealloc(v, sizeof(long) * (size_t)(n + 1)); v[n++] = k; } }
        if (*s == ',') { s++; continue; }
        if (*s == ']') break;
        return -1;
    }
    *vals = v;
    return n;
}

static int is_wc(int c) { return isalnum(c) || c == '_'; }
static int stmt_word(const char *s, size_t n);

/* substitute macro parameters (whole identifiers) */
static char *subst_params(const char *s, const Macro *m, char **args)
{
    Buf b = {0, 0, 0};
    while (*s) {
        if (is_wc((unsigned char)*s)) {
            const char *e = s;
            int i, hit = -1;
            while (is_wc((unsigned char)*e)) e++;
            for (i = 0; i < m->np; i++)
                if (strlen(m->params[i]) == (size_t)(e - s) && !strncmp(m->params[i], s, (size_t)(e - s))) hit = i;
            if (hit >= 0) buf_puts(&b, args[hit]);
            else { size_t k; for (k = 0; k < (size_t)(e - s); k++) buf_putc(&b, s[k]); }
            s = e;
        } else buf_putc(&b, *s++);
    }
    buf_putc(&b, 0);
    return b.p;
}

static int find_macro(const char *s, size_t n)
{
    int i;
    for (i = 0; i < g_nmac; i++)
        if (strlen(g_mac[i].name) == n && !strncmp(g_mac[i].name, s, n)) return i;
    return -1;
}

/* text line after define substitution: expand macro calls, emit */
static void emit_text(const char *text, int file, int line)
{
    const char *s = text, *start = text;
    Buf pre = {0, 0, 0};
    if (g_nmac) {
        while (*s) {
            if (is_wc((unsigned char)*s) && (s == text || !is_wc((unsigned char)s[-1]))) {
                const char *e = s, *q;
                int mi;
                while (is_wc((unsigned char)*e)) e++;
                mi = stmt_word(s, (size_t)(e - s)) ? -1 : find_macro(s, (size_t)(e - s));
                q = skip_ws(e);
                if (mi >= 0 && *q == '(') {
                    char **args = NULL;
                    int na = 0, depth = 0;
                    const char *a = q + 1, *p = q + 1;
                    for (;; p++) {
                        if (!*p) break;
                        if (*p == '(') depth++;
                        else if (*p == ')' && depth > 0) depth--;
                        else if ((*p == ',' || *p == ')') && depth == 0) {
                            const char *x = skip_ws(a), *y = p;
                            while (y > x && (y[-1] == ' ' || y[-1] == '\t')) y--;
                            args = (char **)xrealloc(args, sizeof(char *) * (size_t)(na + 1));
                            args[na++] = xstrndup(x, (size_t)(y - x));
                            a = p + 1;
                            if (*p == ')') break;
                        }
                    }
                    if (*p == ')') {
                        const Macro *m = &g_mac[mi];
                        SLine *body;
                        int k;
                        if (na != m->np && !(na == 1 && m->np == 0 && !args[0][0]))
                            diag(DG_ERROR, "0018cx", line, "incorrect number of parameters");
                        { size_t k2; for (k2 = 0; k2 < (size_t)(s - start); k2++) buf_putc(&pre, start[k2]); }
                        buf_putc(&pre, 0);
                        { const char *t = pre.p; while (*t == ' ' || *t == '\t') t++; if (*t) emit(pre.p, file, line); }
                        buf_free(&pre);
                        body = (SLine *)xcalloc((size_t)m->nbody + 1, sizeof(SLine));
                        for (k = 0; k < m->nbody; k++) {
                            char **aa = (char **)xcalloc((size_t)m->np + 1, sizeof(char *));
                            int j;
                            for (j = 0; j < m->np; j++) aa[j] = j < na ? args[j] : (char *)"";
                            body[k].text = subst_params(m->body[k].text, m, aa);
                            body[k].file = file;
                            body[k].line = line;
                            free(aa);
                        }
                        g_expand_depth++;
                        process_block(body, m->nbody);
                        g_expand_depth--;
                        p++;
                        q = skip_ws(p);
                        if (*q == ';') p = q + 1;
                        else if (g_last_line) diag(DG_ERROR, "0025cx", line, "unexpected end-of-file");
                        else diag(DG_ERROR, "0014cx", line, "unexpected symbol:  )");
                        s = start = p;
                        continue;
                    }
                }
                s = e;
                continue;
            }
            s++;
        }
    }
    if (start != text) {
        const char *t = start;
        while (*t == ' ' || *t == '\t') t++;
        if (*t) emit(start, file, line);
    } else emit(text, file, line);
}

static int g_defskip;   /* BUG_DEFINE_LF: the next line was the value */

static void directive(SLine *L, int n, int *pi)
{
    int i = *pi;
    const char *t = L[i].text + 1, *rest;
    char cmd[16];
    int k = 0, line = L[i].line;
    while (isalpha((unsigned char)t[k]) && k < 15) { cmd[k] = (char)toupper((unsigned char)t[k]); k++; }
    cmd[k] = 0;
    rest = t + k;
    if (skipping()) {
        if (!strcmp(cmd, "IFDEF") || !strcmp(cmd, "IFNDEF")) {
            if (g_ncond < 64) g_cond[g_ncond++] = 2;
        } else if (!strcmp(cmd, "ELSE")) {
            if (g_ncond && g_cond[g_ncond - 1] != 2) g_cond[g_ncond - 1] = !g_cond[g_ncond - 1];
        } else if (!strcmp(cmd, "ENDIF")) {
            if (g_ncond) g_ncond--;
        }
        *pi = i + 1;
        return;
    }
    if (g_expand_depth) {
        char *r = eval_braces(rest);
        rest = r;
    }
    if (!strcmp(cmd, "DEFINE")) {
        const char *p = rest;
        char *name = word(&p);
        const char *v = skip_ws(p);
        size_t vl = strlen(v);
        while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
        if (name[0] && !*p && i + 1 < n) {
            /* BUG_DEFINE_LF: the name ends the line: WinCUPL takes the
               whole next line as the value */
            diag_bug(DG_WARN, "0901cx", line, BUG_DEFINE_LF, g_fix ? "$DEFINE %s at the end of the line: empty value" : "$DEFINE %s at the end of the line takes the next line as its value", name);
            if (!g_fix) {
                v = skip_ws(L[i + 1].text);
                vl = strlen(v);
                while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;
                g_defskip = 1;
            }
        }
        if (!name[0]) diag(DG_ERROR, "0003cx", line, "no label given for command");
        else if (find_def(name) >= 0) diag(DG_ERROR, "0004cx", line, "already defined:  %s", name);
        else {
            g_defs = (Def *)xrealloc(g_defs, sizeof(Def) * (size_t)(g_ndefs + 1));
            g_defs[g_ndefs].name = name;
            g_defs[g_ndefs].val = xstrndup(v, vl);
            g_ndefs++;
        }
    } else if (!strcmp(cmd, "UNDEF")) {
        const char *p = rest;
        char *name = word(&p);
        int d = find_def(name);
        if (!name[0]) diag(DG_ERROR, "0003cx", line, "no label given for command");
        else if (d >= 0) { g_defs[d] = g_defs[--g_ndefs]; }
        free(name);
    } else if (!strcmp(cmd, "IFDEF") || !strcmp(cmd, "IFNDEF")) {
        const char *p = rest;
        char *name = word(&p);
        int def = find_def(name) >= 0;
        if (!name[0]) diag(DG_ERROR, "0003cx", line, "no label given for command");
        if (g_ncond >= MAX_IFDEF) diag(DG_FATAL, "0008cx", line, "$ifdef nesting too deep");
        else g_cond[g_ncond++] = cmd[2] == 'N' ? !def : def;
        free(name);
    } else if (!strcmp(cmd, "ELSE")) {
        if (!g_ncond) diag(DG_ERROR, "0006cx", line, "$else without $ifdef");
        else g_cond[g_ncond - 1] = !g_cond[g_ncond - 1];
    } else if (!strcmp(cmd, "ENDIF")) {
        if (!g_ncond) diag(DG_ERROR, "0007cx", line, "$endif without $ifdef");
        else g_ncond--;
    } else if (!strcmp(cmd, "INCLUDE")) {
        const char *p = rest;
        char *name = word(&p);
        size_t len;
        char *buf;
        size_t nl = strlen(name);
        if (nl >= 2 && (name[0] == '"' || name[0] == '<')) { memmove(name, name + 1, nl); name[nl - 2] = 0; }
        buf = read_source(name, &len);
        if (!buf) diag(DG_FATAL, "0001cx", line, "could not open:  %s", name);
        else {
            int fi = add_file(name), nn;
            SLine *ls = split_lines(buf, fi, &nn);
            process_block(ls, nn);
            free(buf);
        }
        free(name);
    } else if (!strcmp(cmd, "REPEAT")) {
        int e = find_end(L, n, i, "REPEAT", "REPEND");
        const char *p = skip_ws(rest), *q;
        char var[64];
        int vn = 0, nv, j;
        long *vals = NULL;
        while ((isalnum((unsigned char)*p) || *p == '_') && vn < 63) var[vn++] = *p++;
        var[vn] = 0;
        q = skip_ws(p);
        if (vn && stmt_word(var, (size_t)vn)) {
            /* the original stops: a statement word cannot be the variable */
            diag(DG_ERROR, "0014cx", line, "unexpected symbol: $%.*s", k, t);
            g_stop = 1;
            *pi = n;
            return;
        }
        if (e < 0) { diag(DG_ERROR, "0027cx", line, "invalid syntax for preprocessor  command:  $repeat"); *pi = n; return; }
        if (!vn || *q != '=' || (nv = parse_repeat_list(q + 1, &vals)) < 0) {
            diag(DG_ERROR, "0027cx", line, "invalid syntax for preprocessor  command:  $repeat");
        } else if (g_nenv >= 64) {
            diag(DG_FATAL, "0015cx", line, "Repeat nesting too deep");
        } else {
            int save = g_rep_line;
            if (!g_rep_line) g_rep_line = L[e].line;
            for (j = 0; j < nv && !g_stop; j++) {
                g_env[g_nenv].name = var;
                g_env[g_nenv].val = vals[j];
                g_nenv++;
                g_expand_depth++;
                process_block(L + i + 1, e - i - 1);
                g_expand_depth--;
                g_nenv--;
            }
            g_rep_line = save;
        }
        free(vals);
        *pi = e + 1;
        return;
    } else if (!strcmp(cmd, "MACRO")) {
        int e = find_end(L, n, i, "MACRO", "MEND"), j;
        const char *p = rest;
        Macro m;
        memset(&m, 0, sizeof m);
        if (e < 0) { diag(DG_ERROR, "0027cx", line, "invalid syntax for preprocessor  command:  $macro"); *pi = n; return; }
        for (;;) {
            char *w;
            p = skip_ws(p);
            if (!*p || *p == ';') break;
            w = word(&p);
            { size_t wl = strlen(w); if (wl && w[wl - 1] == ';') w[wl - 1] = 0; }
            if (!w[0]) { free(w); break; }
            if (!m.name) m.name = w;
            else {
                m.params = (char **)xrealloc(m.params, sizeof(char *) * (size_t)(m.np + 1));
                m.params[m.np++] = w;
            }
        }
        if (!m.name) diag(DG_ERROR, "0017cx", line, "missing Macro name");
        else if (find_macro(m.name, strlen(m.name)) >= 0) diag(DG_ERROR, "0016cx", line, "duplicate Macro function name:  %s", m.name);
        else {
            m.nbody = e - i - 1;
            m.body = (SLine *)xcalloc((size_t)m.nbody + 1, sizeof(SLine));
            for (j = 0; j < m.nbody; j++) m.body[j] = L[i + 1 + j];
            g_mac = (Macro *)xrealloc(g_mac, sizeof(Macro) * (size_t)(g_nmac + 1));
            g_mac[g_nmac++] = m;
        }
        *pi = e + 1;
        return;
    } else if (!strcmp(cmd, "REPEND") || !strcmp(cmd, "MEND")) {
        diag(DG_ERROR, "0014cx", line, "unexpected symbol:  $%s", str_lower(xstrdup(cmd)));
    } else {
        char *lc = str_lower(xstrdup(L[i].text));
        char *sp = lc;
        while (*sp && *sp != ' ' && *sp != '\t') sp++;
        *sp = 0;
        diag(DG_ERROR, "0010cx", line, "invalid preprocessor command:  %s", lc);
        free(lc);
    }
    *pi = i + 1 + g_defskip;
    g_defskip = 0;
}

/* A macro named like one of these statements is never expanded: the
   statement wins (probed). */
static int stmt_word(const char *s, size_t n)
{
    static const char *const w[] = {
        "CONDITION", "FIELD", "FUNCTION", "PIN", "PINNODE", "PROPERTY", "SEQUENCE",
        "SEQUENCED", "SEQUENCEJK", "SEQUENCERS", "SEQUENCET", "TABLE", "FUSE", NULL
    };
    char b[16];
    int i;
    if (n >= sizeof b) return 0;
    memcpy(b, s, n);
    b[n] = 0;
    for (i = 0; w[i]; i++) if (str_ieq(b, w[i])) return 1;
    return 0;
}

/* The original checks every FUSE on the source text (after macro
   parameters, before $DEFINE): outside a PIN or PINNODE statement it must
   start FUSE(n,n); and the first fault stops the run (0014cx).  Line
   starts and ; { } begin a statement. */

static int strncasecmp_(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int d = toupper((unsigned char)*a) - toupper((unsigned char)*b);
        if (d || !*a) return d;
    }
    return 0;
}

static int fuse_scan(const char *t, int line)
{
    const char *s = t;
    if (g_rep_line) line = g_rep_line;
    if (!g_fs.st) { g_fs.stmt = 1; g_fs.inpin = 0; }
    while (*s) {
        const char *b;
        size_t n;
        if (g_fs.incomment) {
            if (s[0] == '*' && s[1] == '/') { g_fs.incomment = 0; s += 2; }
            else s++;
            continue;
        }
        if (s[0] == '/' && s[1] == '*') { g_fs.incomment = 1; s += 2; continue; }
        if (isspace((unsigned char)*s)) { s++; continue; }
        b = s;
        if (is_wc((unsigned char)*s)) while (is_wc((unsigned char)*s)) s++;
        else s++;
        n = (size_t)(s - b);
        if (g_fs.st) {
            int ok = 0;
            size_t k;
            switch (g_fs.st) {
            case 1: ok = *b == '('; break;
            case 3: ok = *b == ','; break;
            case 5: ok = *b == ')'; break;
            case 6: ok = *b == ';'; break;
            default:
                ok = 1;
                for (k = 0; k < n; k++) if (!isdigit((unsigned char)b[k])) ok = 0;
            }
            if (!ok) {
                diag(DG_ERROR, "0014cx", line, "unexpected symbol: %s", g_fs.prev);
                g_stop = 1;
                return -1;
            }
            snprintf(g_fs.prev, sizeof g_fs.prev, "%.*s", (int)(n < 63 ? n : 63), b);
            if (++g_fs.st == 7) { g_fs.st = 0; g_fs.stmt = 1; g_fs.inpin = 0; }
            continue;
        }
        if (n == 1 && (*b == ';' || *b == '{' || *b == '}')) { g_fs.stmt = 1; g_fs.inpin = 0; continue; }
        if (is_wc((unsigned char)*b)) {
            char w[16];
            snprintf(w, sizeof w, "%.*s", (int)(n < 15 ? n : 15), b);
            if (g_fs.stmt) g_fs.inpin = n < 15 && (str_ieq(w, "PIN") || str_ieq(w, "PINNODE"));
            if (!g_fs.inpin && n == 4 && str_ieq(w, "FUSE")) {
                g_fs.st = 1;
                g_fs.line = line;
                snprintf(g_fs.prev, sizeof g_fs.prev, "%s", w);
            }
        }
        g_fs.stmt = 0;
    }
    return 0;
}

/* a macro call whose argument list is not closed on this line */
static int open_call(const char *t)
{
    const char *s = t;
    if (!g_nmac) return 0;
    while (*s) {
        if (is_wc((unsigned char)*s) && (s == t || !is_wc((unsigned char)s[-1]))) {
            const char *e = s, *q;
            while (is_wc((unsigned char)*e)) e++;
            q = skip_ws(e);
            if (!stmt_word(s, (size_t)(e - s)) && find_macro(s, (size_t)(e - s)) >= 0 && *q == '(') {
                int depth = 0;
                for (; *q; q++) {
                    if (*q == '(') depth++;
                    else if (*q == ')' && --depth == 0) break;
                }
                if (!*q) return 1;
                s = q;
                continue;
            }
            s = e;
            continue;
        }
        s++;
    }
    return 0;
}

static void process_block(SLine *L, int n)
{
    int i = 0;
    g_depth++;
    while (i < n) {
        if (g_depth == 1) {
            int k = i + 1;
            while (k < n && !L[k].text[0]) k++;
            g_last_line = k >= n;
        }
        if (g_stop) break;
        if (L[i].text[0] == '$') {
            /* the FUSE check reads every directive line except these */
            static const char *const noscan[] = { "DEFINE", "MACRO", "REPEAT", "INCLUDE", NULL };
            const char *c = L[i].text + 1;
            int k;
            for (k = 0; noscan[k]; k++) {
                size_t m = strlen(noscan[k]);
                if (!strncasecmp_(c, noscan[k], m) && !isalpha((unsigned char)c[m])) break;
            }
            if (!noscan[k] && fuse_scan(L[i].text + 1, L[i].line) < 0) break;
            directive(L, n, &i);
            continue;
        }
        if (fuse_scan(L[i].text, L[i].line) < 0) break;
        if (!skipping()) {
            char *a;
            a = subst_defines(L[i].text);
            while (open_call(a) && i + 1 < n && L[i + 1].text[0] != '$') {
                char *b = subst_defines(L[i + 1].text);
                Buf j = {0, 0, 0};
                buf_printf(&j, "%s %s", a, b);
                free(a);
                free(b);
                a = j.p;
                i++;
            }
            if (g_expand_depth) {
                char *b = eval_braces(a);
                free(a);
                a = b;
            }
            emit_text(a, L[i].file, L[i].line);
            free(a);
        }
        i++;
    }
    g_depth--;
}

char **pp_source_lines(int *n)
{
    *n = g_nsrc0;
    return g_src0;
}

int pp_run(const char *path, PPOut *out)
{
    size_t len;
    char *buf;
    SLine *ls;
    int n, i, fi;
    const char *sl;
    memset(out, 0, sizeof *out);
    g_out = out;
    g_ndefs = g_nmac = g_nenv = g_ncond = g_expand_depth = g_depth = 0;
    memset(&g_fs, 0, sizeof g_fs);
    g_stop = g_rep_line = 0;
    g_dir[0] = 0;
    sl = strrchr(path, '/');
    if (!sl) sl = strrchr(path, '\\');
    if (sl && (size_t)(sl - path + 1) < sizeof g_dir) { memcpy(g_dir, path, (size_t)(sl - path + 1)); g_dir[sl - path + 1] = 0; }
    buf = read_file(path, &len);
    if (!buf) { diag(DG_FATAL, "0001ck", 0, "could not open:  \"%s\"", path); return -1; }
    fi = add_file(path);
    ls = split_lines(buf, fi, &n);
    g_src0 = (char **)xcalloc((size_t)n + 1, sizeof(char *));
    for (i = 0; i < n; i++) g_src0[i] = ls[i].text;
    g_nsrc0 = n;
    process_block(ls, n);
    if (g_fs.st && !g_stop) diag(DG_ERROR, "0025cx", n, "unexpected end-of-file");
    if (g_ncond && !g_stop) diag(DG_ERROR, "0009cx", 0, "missing $endif");
    free(buf);
    return g_diag.nerr ? -1 : 0;
}
