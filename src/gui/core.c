/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecuplw - the parts that do not need Windows.  DARKSOFT. */
#include "core.h"
#include "../namebase.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int gui_stricmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int d = lower((unsigned char)*a) - lower((unsigned char)*b);
        if (d || !*a) return d;
    }
}

static int strnicmp_(const char *a, const char *b, size_t n)
{
    for (; n; a++, b++, n--) {
        int d = lower((unsigned char)*a) - lower((unsigned char)*b);
        if (d || !*a) return d;
    }
    return 0;
}

static void copy(char *out, size_t n, const char *s)
{
    size_t l = strlen(s);
    if (!n) return;
    if (l >= n) l = n - 1;
    memcpy(out, s, l);
    out[l] = 0;
}

/* ---- devices ---- */

static int contains(const char *hay, const char *w, size_t wl)
{
    for (; *hay; hay++)
        if (!strnicmp_(hay, w, wl)) return 1;
    return wl == 0;
}

int gui_dev_match(const char *filter, const GuiDev *d)
{
    const char *p = filter;
    for (;;) {
        const char *w;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) return 1;
        w = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (!contains(d->mn, w, (size_t)(p - w)) &&
            !contains(d->part, w, (size_t)(p - w)))
            return 0;
    }
}

static int text_cmp(const char *a, const char *b)
{
    for (;;) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= '0' && ca <= '9' && cb >= '0' && cb <= '9') {
            const char *ea, *eb;
            while (*a == '0') a++;
            while (*b == '0') b++;
            for (ea = a; *ea >= '0' && *ea <= '9'; ea++) ;
            for (eb = b; *eb >= '0' && *eb <= '9'; eb++) ;
            if (ea - a != eb - b) return ea - a < eb - b ? -1 : 1;
            for (; a < ea; a++, b++) if (*a != *b) return *a < *b ? -1 : 1;
            continue;
        }
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return ca < cb ? -1 : 1;
        if (!ca) return 0;
        a++; b++;
    }
}

int gui_dev_cmp(const GuiDev *a, const GuiDev *b, int col)
{
    int r = 0;
    if (col == 1) r = a->pins < b->pins ? -1 : a->pins > b->pins;
    else if (col == 2) r = text_cmp(a->part, b->part);
    if (!r) r = text_cmp(a->mn, b->mn);
    if (!r) r = strcmp(a->mn, b->mn);
    return r;
}

int gui_dev_list(const char *filter, int col, int desc, int *idx)
{
    int i, j, n = 0;
    for (i = 0; i < gui_ndevs; i++)
        if (gui_dev_match(filter, &gui_devs[i])) idx[n++] = i;
    if (col < 0 || col > 2) return n;
    for (i = 1; i < n; i++) {
        int v = idx[i];
        for (j = i; j > 0; j--) {
            int c = gui_dev_cmp(&gui_devs[idx[j - 1]], &gui_devs[v], col);
            if (desc ? c >= 0 : c <= 0) break;
            idx[j] = idx[j - 1];
        }
        idx[j] = v;
    }
    return n;
}

/* ---- tool messages ---- */

void gui_parse_msg(const char *text, GuiMsg *m)
{
    const char *p = text;
    int i;
    m->kind = GM_TEXT;
    m->line = 0;
    memset(m->code, 0, sizeof m->code);
    if (p[0] == '[') {
        for (i = 1; i <= 4; i++)
            if (!isdigit((unsigned char)p[i])) return;
        if (!isalpha((unsigned char)p[5]) || !isalpha((unsigned char)p[6]) ||
            p[7] != ']')
            return;
        memcpy(m->code, p + 1, 6);
        m->code[6] = 0;
        p += 8;
        while (*p == ' ') p++;
        if (!strncmp(p, "line(", 5) && isdigit((unsigned char)p[5])) {
            char *e;
            long v = strtol(p + 5, &e, 10);
            if (*e == ')') {
                m->line = (int)v;
                p = e + 1;
                while (*p == ' ') p++;
            }
        }
        if (!strncmp(p, "Please note:", 12)) m->kind = GM_NOTE;
        else if (!strncmp(p, "Warning:", 8)) m->kind = GM_WARN;
        else m->kind = GM_ERROR;
        return;
    }
    if (!strncmp(p, "Fatal errors encountered", 24) ||
        !strncmp(p, "Simulation errors encountered", 29)) {
        m->kind = GM_ABORT;
        return;
    }
    /* "%3d:" and the source line */
    while (*p == ' ') p++;
    if (isdigit((unsigned char)*p) && p - text <= 2) {
        char *e;
        long v = strtol(p, &e, 10);
        if (*e == ':' && e - text >= 3 && v > 0) {
            m->kind = GM_CONTEXT;
            m->line = (int)v;
        }
    }
}

void gui_summary(char *out, size_t n, const char *what, int nerr, int nwarn, int rc)
{
    char num[64];
    /* a failure the tool gave no coded message for */
    if (rc != 0 && nerr == 0) nerr = 1;
    snprintf(num, sizeof num, "%d error%s, %d warning%s",
             nerr, nerr == 1 ? "" : "s", nwarn, nwarn == 1 ? "" : "s");
    snprintf(out, n, "%s: %s - %s", what, nerr ? "FAILED" : "OK", num);
}

/* ---- options ---- */

void gui_opts_default(GuiOpts *o)
{
    memset(o, 0, sizeof *o);
    o->jed = o->doc = o->lst = 1;
    o->min = 1;
    o->simlst = 1;
}

int gui_quote_arg(const char *arg, char *out, size_t n)
{
    size_t k = 0;
    const char *p;
    int bs = 0;
    if (*arg && !strpbrk(arg, " \t\"")) {
        if (strlen(arg) >= n) return -1;
        strcpy(out, arg);
        return 0;
    }
#define PUT(c) do { if (k + 1 >= n) return -1; out[k++] = (c); } while (0)
    PUT('"');
    for (p = arg; *p; p++) {
        if (*p == '\\') {
            bs++;
        } else if (*p == '"') {
            /* backslashes before a quote are doubled, the quote escaped */
            for (; bs >= 0; bs--) PUT('\\');
            bs = 0;
        } else {
            bs = 0;
        }
        PUT(*p);
    }
    /* and those before the closing quote */
    for (; bs > 0; bs--) PUT('\\');
    PUT('"');
#undef PUT
    out[k] = 0;
    return 0;
}

static void arg_push(GuiArgv *a, const char *s)
{
    if (a->n < GUI_MAXARGS) a->v[a->n++] = s;
    a->v[a->n] = NULL;
}

void gui_compile_argv(const GuiOpts *o, const char *exe, const char *file,
                      int flags, GuiArgv *a)
{
    char *f = a->flags;
    int k = 0;
    a->n = 0;
    f[k++] = '-';
    if (o->jed) f[k++] = 'j';
    if (o->doc) f[k++] = 'x';
    if (o->fuseplot) f[k++] = 'f';
    if (o->lst) f[k++] = 'l';
    if (o->mx) f[k++] = 'e';
    if (o->abs || (flags & GC_ABS)) f[k++] = 'a';
    if (flags & GC_SIM) f[k++] = 's';
    if (o->namesrc) f[k++] = 'n';
    if (o->secure) f[k++] = 'g';
    f[k] = 0;
    arg_push(a, exe);
    if (k > 1) arg_push(a, f);
    if (o->min != 1 && o->min >= 0 && o->min <= 4) {
        snprintf(a->min, sizeof a->min, "-m%d", o->min);
        arg_push(a, a->min);
    }
    if (o->kb) arg_push(a, "-kb");
    if (o->compat) arg_push(a, "-compat-refdefect");
    if (o->simlatch && (flags & GC_SIM)) arg_push(a, "-simlatch");
    if (o->device[0]) arg_push(a, o->device);
    arg_push(a, file);
}

void gui_sim_argv(const GuiOpts *o, const char *exe, const char *file,
                  GuiArgv *a)
{
    char *f = a->flags;
    int k = 0;
    a->n = 0;
    f[k++] = '-';
    if (o->simlst) f[k++] = 'l';
    if (o->simjed) f[k++] = 'j';
    if (o->simjed && o->namesrc) f[k++] = 'n';
    f[k] = 0;
    arg_push(a, exe);
    if (k > 1) arg_push(a, f);
    if (o->compat) arg_push(a, "-compat-refdefect");
    if (o->simlatch) arg_push(a, "-simlatch");
    arg_push(a, file);
}

int gui_join_args(const GuiArgv *a, char *out, size_t n)
{
    int i, r = 0;
    if (!n) return -1;
    out[0] = 0;
    for (i = 0; i < a->n; i++) {
        size_t l = strlen(out);
        if (l) {
            if (l + 2 >= n) return -1;
            out[l++] = ' ';
            out[l] = 0;
        }
        r |= gui_quote_arg(a->v[i], out + l, n - l);
    }
    return r ? -1 : 0;
}

int gui_compile_cmd(const GuiOpts *o, const char *exe, const char *file,
                    int flags, char *out, size_t n)
{
    GuiArgv a;
    gui_compile_argv(o, exe, file, flags, &a);
    return gui_join_args(&a, out, n);
}

int gui_sim_cmd(const GuiOpts *o, const char *exe, const char *file,
                char *out, size_t n)
{
    GuiArgv a;
    gui_sim_argv(o, exe, file, &a);
    return gui_join_args(&a, out, n);
}

/* ---- settings file ---- */

typedef struct {
    char *sec, *key, *val;
} IniEnt;

struct GuiIni {
    IniEnt *e;
    int n, cap;
};

static char *sdup(const char *s)
{
    size_t l = strlen(s) + 1;
    char *p = malloc(l);
    if (p) memcpy(p, s, l);
    return p;
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        e--;
    *e = 0;
    return s;
}

static IniEnt *ini_find(const GuiIni *ini, const char *sec, const char *key)
{
    int i;
    for (i = 0; i < ini->n; i++)
        if (!gui_stricmp(ini->e[i].sec, sec) && !gui_stricmp(ini->e[i].key, key))
            return &ini->e[i];
    return NULL;
}

GuiIni *gui_ini_load(const char *path)
{
    GuiIni *ini = calloc(1, sizeof *ini);
    FILE *f;
    char line[1024], sec[512];
    if (!ini) return NULL;
    f = fopen(path, "r");
    if (!f) return ini;
    sec[0] = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line), *eq;
        if (*s == '[') {
            char *e = strrchr(s, ']');
            if (e) {
                *e = 0;
                copy(sec, sizeof sec, s + 1);
            }
            continue;
        }
        if (!*s || *s == ';' || *s == '#') continue;
        eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        gui_ini_set(ini, sec, trim(s), trim(eq + 1));
    }
    fclose(f);
    return ini;
}

int gui_ini_save(const GuiIni *ini, const char *path)
{
    FILE *f = fopen(path, "w");
    int i, j, r;
    if (!f) return -1;
    for (i = 0; i < ini->n; i++) {
        /* a section is written where its first entry is */
        for (j = 0; j < i; j++)
            if (!gui_stricmp(ini->e[j].sec, ini->e[i].sec)) break;
        if (j < i) continue;
        fprintf(f, "%s[%s]\n", i ? "\n" : "", ini->e[i].sec);
        for (j = i; j < ini->n; j++)
            if (!gui_stricmp(ini->e[j].sec, ini->e[i].sec))
                fprintf(f, "%s=%s\n", ini->e[j].key, ini->e[j].val);
    }
    r = ferror(f) ? -1 : 0;
    if (fclose(f)) r = -1;
    return r;
}

void gui_ini_free(GuiIni *ini)
{
    int i;
    if (!ini) return;
    for (i = 0; i < ini->n; i++) {
        free(ini->e[i].sec);
        free(ini->e[i].key);
        free(ini->e[i].val);
    }
    free(ini->e);
    free(ini);
}

const char *gui_ini_get(const GuiIni *ini, const char *sec, const char *key,
                        const char *def)
{
    IniEnt *e = ini_find(ini, sec, key);
    return e ? e->val : def;
}

int gui_ini_int(const GuiIni *ini, const char *sec, const char *key, int def)
{
    const char *v = gui_ini_get(ini, sec, key, NULL);
    char *e;
    long x;
    if (!v || !*v) return def;
    x = strtol(v, &e, 10);
    return *e ? def : (int)x;
}

void gui_ini_set(GuiIni *ini, const char *sec, const char *key, const char *val)
{
    IniEnt *e = ini_find(ini, sec, key);
    char *v = sdup(val), *p;
    if (!v) return;
    for (p = v; *p; p++)
        if (*p == '\r' || *p == '\n') *p = ' ';
    if (e) {
        free(e->val);
        e->val = v;
        return;
    }
    if (ini->n == ini->cap) {
        int cap = ini->cap ? ini->cap * 2 : 32;
        IniEnt *ne = realloc(ini->e, (size_t)cap * sizeof *ne);
        if (!ne) {
            free(v);
            return;
        }
        ini->e = ne;
        ini->cap = cap;
    }
    e = &ini->e[ini->n];
    e->sec = sdup(sec);
    e->key = sdup(key);
    e->val = v;
    if (!e->sec || !e->key) {
        free(e->sec);
        free(e->key);
        free(v);
        return;
    }
    ini->n++;
}

void gui_ini_set_int(GuiIni *ini, const char *sec, const char *key, int val)
{
    char b[24];
    snprintf(b, sizeof b, "%d", val);
    gui_ini_set(ini, sec, key, b);
}

void gui_ini_del_section(GuiIni *ini, const char *sec)
{
    int i, k = 0;
    for (i = 0; i < ini->n; i++) {
        if (!gui_stricmp(ini->e[i].sec, sec)) {
            free(ini->e[i].sec);
            free(ini->e[i].key);
            free(ini->e[i].val);
        } else {
            ini->e[k++] = ini->e[i];
        }
    }
    ini->n = k;
}

int gui_ini_has_section(const GuiIni *ini, const char *sec)
{
    int i;
    for (i = 0; i < ini->n; i++)
        if (!gui_stricmp(ini->e[i].sec, sec)) return 1;
    return 0;
}

static const struct {
    const char *key;
    size_t off;
} opt_keys[] = {
    { "jed", offsetof(GuiOpts, jed) },
    { "doc", offsetof(GuiOpts, doc) },
    { "fuseplot", offsetof(GuiOpts, fuseplot) },
    { "listing", offsetof(GuiOpts, lst) },
    { "abs", offsetof(GuiOpts, abs) },
    { "expand", offsetof(GuiOpts, mx) },
    { "namesrc", offsetof(GuiOpts, namesrc) },
    { "secure", offsetof(GuiOpts, secure) },
    { "polarity", offsetof(GuiOpts, kb) },
    { "min", offsetof(GuiOpts, min) },
    { "simafter", offsetof(GuiOpts, simafter) },
    { "compat", offsetof(GuiOpts, compat) },
    { "simlatch", offsetof(GuiOpts, simlatch) },
    { "simlisting", offsetof(GuiOpts, simlst) },
    { "simjed", offsetof(GuiOpts, simjed) }
};
#define NOPTKEYS (sizeof opt_keys / sizeof opt_keys[0])

void gui_opts_load(const GuiIni *ini, const char *sec, GuiOpts *o)
{
    size_t i;
    const char *d;
    gui_opts_default(o);
    for (i = 0; i < NOPTKEYS; i++) {
        int *p = (int *)((char *)o + opt_keys[i].off);
        *p = gui_ini_int(ini, sec, opt_keys[i].key, *p);
        if (strcmp(opt_keys[i].key, "min")) *p = *p != 0;
    }
    if (o->min < 0 || o->min > 4) o->min = 1;
    d = gui_ini_get(ini, sec, "device", "");
    /* a mnemonic, never a flag */
    if (*d == '-' || strpbrk(d, " \t\"")) d = "";
    copy(o->device, sizeof o->device, d);
}

void gui_opts_save(GuiIni *ini, const char *sec, const GuiOpts *o)
{
    size_t i;
    for (i = 0; i < NOPTKEYS; i++)
        gui_ini_set_int(ini, sec, opt_keys[i].key,
                        *(const int *)((const char *)o + opt_keys[i].off));
    gui_ini_set(ini, sec, "device", o->device);
}

void gui_file_section(const char *path, char *out, size_t n)
{
    size_t k = 5;
    if (n < 8) {
        if (n) out[0] = 0;
        return;
    }
    memcpy(out, "file ", 5);
    for (; *path && k + 1 < n; path++) {
        int c = lower((unsigned char)*path);
        /* the two characters a section name cannot hold */
        out[k++] = (c == '[') ? '(' : (c == ']') ? ')' : (char)c;
    }
    out[k] = 0;
}

void gui_recent_add(char list[GUI_NRECENT][GUI_PATH], const char *path)
{
    char tmp[GUI_PATH];
    int i, at = GUI_NRECENT - 1;
    copy(tmp, sizeof tmp, path);
    for (i = 0; i < GUI_NRECENT; i++)
        if (!gui_stricmp(list[i], tmp)) {
            at = i;
            break;
        }
    for (i = at; i > 0; i--) memcpy(list[i], list[i - 1], GUI_PATH);
    memcpy(list[0], tmp, GUI_PATH);
}

/* ---- source text ---- */

const char *const gui_keywords[] = {
    "append", "assembly", "base", "company", "condition", "date", "default",
    "designer", "device", "else", "field", "fld", "format", "function",
    "fuse", "group", "if", "jump", "location", "macro", "min", "name",
    "next", "node", "order", "out", "partno", "pin", "pinnode", "present",
    "property", "revision", "sequence", "sequenced", "sequencejk",
    "sequencers", "sequencet", "table", "vectors"
};
const int gui_nkeywords = (int)(sizeof gui_keywords / sizeof gui_keywords[0]);

static int is_keyword(const unsigned char *s, size_t l)
{
    size_t lo = 0, hi = (size_t)gui_nkeywords;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2, i;
        const char *k = gui_keywords[mid];
        int d = 0;
        for (i = 0; i < l && !d; i++) d = lower(s[i]) - (unsigned char)k[i];
        if (!d && k[l]) d = -1;
        if (!d) return 1;
        if (d < 0) hi = mid;
        else lo = mid + 1;
    }
    return 0;
}

static int idchar(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

void gui_scan(const unsigned char *t, size_t n, unsigned char *attr)
{
    size_t i = 0;
    while (i < n) {
        int c = t[i];
        size_t j;
        if (c == '/' && i + 1 < n && t[i + 1] == '*') {
            j = i + 2;
            while (j < n && !(t[j] == '*' && j + 1 < n && t[j + 1] == '/')) j++;
            j = (j < n) ? j + 2 : n;
            memset(attr + i, GA_COMMENT, j - i);
            i = j;
        } else if (c == '\'' && i + 2 < n && t[i + 2] == '\'' &&
                   strchr("bBoOdDhH", t[i + 1])) {
            /* 'h'1F */
            j = i + 3;
            while (j < n && idchar(t[j])) j++;
            memset(attr + i, GA_NUMBER, j - i);
            i = j;
        } else if (c == '$' && i + 1 < n && isalpha(t[i + 1])) {
            j = i + 1;
            while (j < n && idchar(t[j])) j++;
            memset(attr + i, GA_KEYWORD, j - i);
            i = j;
        } else if (idchar(c)) {
            int num = 1;
            j = i;
            while (j < n && idchar(t[j])) {
                if (t[j] < '0' || t[j] > '9') num = 0;
                j++;
            }
            memset(attr + i, num ? GA_NUMBER :
                   is_keyword(t + i, j - i) ? GA_KEYWORD : GA_PLAIN, j - i);
            i = j;
        } else {
            attr[i++] = GA_PLAIN;
        }
    }
}

void gui_diff(const unsigned char *a, size_t na, const unsigned char *b,
              size_t nb, size_t *prefix, size_t *suffix)
{
    size_t m = na < nb ? na : nb, p = 0, s = 0;
    while (p < m && a[p] == b[p]) p++;
    while (s < m - p && a[na - 1 - s] == b[nb - 1 - s]) s++;
    *prefix = p;
    *suffix = s;
}

/* Find the header item word at the start of a statement; returns the
   offset of the word or n. */
static size_t find_item(const char *t, size_t n, const char *word)
{
    size_t i = 0, wl = strlen(word);
    int start = 1;
    while (i < n) {
        int c = (unsigned char)t[i];
        if (c == '/' && i + 1 < n && t[i + 1] == '*') {
            i += 2;
            while (i < n && !(t[i] == '*' && i + 1 < n && t[i + 1] == '/')) i++;
            i = (i < n) ? i + 2 : n;
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            i++;
        } else if (c == ';') {
            start = 1;
            i++;
        } else if (idchar(c) || c == '$') {
            size_t j = i + 1;
            while (j < n && idchar((unsigned char)t[j])) j++;
            if (start && j - i == wl && !strnicmp_(t + i, word, wl)) return i;
            start = 0;
            i = j;
        } else {
            start = 0;
            i++;
        }
    }
    return n;
}

int gui_design_name(const char *t, size_t n, char *out, size_t outn)
{
    size_t i = find_item(t, n, "name"), j, k = 0;
    if (i >= n || !outn) return 0;
    i += 4;
    while (i < n && (t[i] == ' ' || t[i] == '\t')) i++;
    j = i;
    while (j < n && t[j] != ';' && t[j] != '\r' && t[j] != '\n') j++;
    while (j > i && (t[j - 1] == ' ' || t[j - 1] == '\t')) j--;
    {
        /* the JEDEC file is named after the item as freecupl forms the
           name (namebase.h); no name when freecupl refuses it */
        char raw[256], nb[64];
        for (; i < j && k + 1 < sizeof raw; i++) raw[k++] = t[i];
        raw[k] = 0;
        if (!k || name_file_base(raw, nb, sizeof nb)) { out[0] = 0; return 0; }
        snprintf(out, outn, "%s", nb);
    }
    return out[0] != 0;
}

int gui_device_stmt(const char *t, size_t n, size_t *start, size_t *end)
{
    size_t i = find_item(t, n, "device"), j;
    if (i >= n) return 0;
    j = i;
    while (j < n && t[j] != ';' && t[j] != '\r' && t[j] != '\n') j++;
    if (j < n && t[j] == ';') j++;
    *start = i;
    *end = j;
    return 1;
}

int gui_detect_lf(const char *buf, size_t n)
{
    size_t i, crlf = 0, lf = 0;
    for (i = 0; i < n; i++)
        if (buf[i] == '\n') {
            if (i && buf[i - 1] == '\r') crlf++;
            else lf++;
        }
    return lf > crlf;
}

/* ---- paths ---- */

const char *gui_basename(const char *path)
{
    const char *p, *b = path;
    for (p = path; *p; p++)
        if (*p == '\\' || *p == '/' || (*p == ':' && p == path + 1)) b = p + 1;
    return b;
}

void gui_set_ext(const char *path, const char *ext, char *out, size_t n)
{
    const char *b = gui_basename(path), *dot = strrchr(b, '.');
    size_t l = dot ? (size_t)(dot - path) : strlen(path);
    if (!n) return;
    if (l + strlen(ext) >= n) {
        out[0] = 0;
        return;
    }
    memcpy(out, path, l);
    strcpy(out + l, ext);
}

void gui_dirname(const char *path, char *out, size_t n)
{
    const char *b = gui_basename(path);
    size_t l = (size_t)(b - path);
    if (!n) return;
    /* keep the separator of a root: "C:\" */
    if (l > 1 && (path[l - 1] == '\\' || path[l - 1] == '/') && path[l - 2] != ':')
        l--;
    if (!l) {
        copy(out, n, ".");
        return;
    }
    if (l >= n) l = n - 1;
    memcpy(out, path, l);
    out[l] = 0;
}

int gui_has_ext(const char *path, const char *ext)
{
    const char *dot = strrchr(gui_basename(path), '.');
    return dot && !gui_stricmp(dot, ext);
}

int gui_is_output(const char *path)
{
    static const char *const ext[] = { ".jed", ".doc", ".lst", ".so", ".wo", ".abs", ".mx" };
    size_t i;
    for (i = 0; i < sizeof ext / sizeof ext[0]; i++)
        if (gui_has_ext(path, ext[i])) return 1;
    return 0;
}
