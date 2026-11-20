/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl serve - request handlers.  DARKSOFT. */
#include "web.h"
#include "version.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILE HTTP_MAX_BODY
#define MAX_MSGS 20000

/* snprintf where a cut-off text is acceptable */
static void sput(char *out, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, n, fmt, ap);
    va_end(ap);
}

/* ---- buffers ---- */

void wb_add(WBuf *b, const void *data, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1024;
        char *np;
        while (cap < b->n + n + 1) cap *= 2;
        np = realloc(b->p, cap);
        if (!np) return;
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->n, data, n);
    b->n += n;
    b->p[b->n] = 0;
}

void wb_puts(WBuf *b, const char *s)
{
    wb_add(b, s, strlen(s));
}

void wb_printf(WBuf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    wb_puts(b, tmp);
}

void wb_json(WBuf *b, const char *s)
{
    wb_add(b, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            char e[2];
            e[0] = '\\';
            e[1] = (char)c;
            wb_add(b, e, 2);
        } else if (c < 32 || c == 127) {
            wb_printf(b, "\\u%04x", c);
        } else {
            wb_add(b, s, 1);
        }
    }
    wb_add(b, "\"", 1);
}

/* ---- names ---- */

int web_name_ok(const char *name)
{
    static const char *const dev[] = { "con", "prn", "aux", "nul", "com", "lpt" };
    size_t l = strlen(name), i, stem;
    const char *dot;
    /* '-': the tools would take it for a flag */
    if (!l || l >= WEB_NAME - 56 || name[0] == '.' || name[0] == ' ' || name[0] == '-')
        return 0;
    if (name[l - 1] == '.' || name[l - 1] == ' ') return 0;
    for (i = 0; i < l; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 32 || c == 127 || strchr("/\\:*?\"<>|", c)) return 0;
        /* Windows: the name must be the same in UTF-8 and the code page */
        if (c > 127 && sys_sep == '\\') return 0;
    }
    /* device names of Windows, with any extension */
    dot = strchr(name, '.');
    stem = dot ? (size_t)(dot - name) : l;
    /* Windows drops blanks before the dot */
    while (stem && name[stem - 1] == ' ') stem--;
    for (i = 0; i < 6; i++) {
        char b[4];
        if (stem != (i < 4 ? 3u : 4u)) continue;
        memcpy(b, name, 3);
        b[3] = 0;
        if (gui_stricmp(b, dev[i])) continue;
        if (i < 4 || (name[3] >= '1' && name[3] <= '9')) return 0;
    }
    return 1;
}

static void full_path(const WebSrv *s, const char *name, char *out, size_t n)
{
    size_t l = strlen(s->dir);
    sput(out, n, "%s%s%s", s->dir,
             l && s->dir[l - 1] == sys_sep ? "" : (sys_sep == '/' ? "/" : "\\"), name);
}

/* ---- answers ---- */

static void json_begin(WebResp *out, int status)
{
    out->status = status;
    out->ctype = "application/json";
    out->body.n = 0;
}

static void json_error(WebResp *out, int status, const char *fmt, ...)
{
    char tmp[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    json_begin(out, status);
    wb_puts(&out->body, "{\"error\":");
    wb_json(&out->body, tmp);
    wb_puts(&out->body, "}");
}

void web_error(WebResp *out, int status, const char *text)
{
    out->status = status;
    out->ctype = "text/plain";
    out->body.n = 0;
    wb_printf(&out->body, "%d %s\n", status, text ? text : http_reason(status));
}

/* ---- the checks every request passes ---- */

/* Host and Origin for everything.  The page and its files hold no
   secret and need no token; every request under /api/ needs it in the
   header, which a page of another site cannot send.  The one exception
   is the request that trades the ticket for the token. */
int web_check(const WebSrv *s, const HttpReq *r)
{
    if (!http_host_ok(r->host, s->port)) return 403;
    if (r->origin[0] && !http_origin_ok(r->origin, s->port)) return 403;
    if (strncmp(r->path, "/api/", 5)) return 0;
    if (!strcmp(r->path, "/api/session") && !strcmp(r->method, "POST")) return 0;
    return http_token_eq(r->token, s->token) ? 0 : 403;
}

/* the ticket the opened browser was given, good once */
static void post_session(WebSrv *s, const HttpReq *r, WebResp *out)
{
    char v[80];
    if (!s->ticket[0] || http_query_get(r->query, "k", v, sizeof v) ||
        !http_token_eq(v, s->ticket)) {
        web_error(out, 403, "Forbidden.  Open the address freecupl serve printed.");
        return;
    }
    s->ticket[0] = 0;
    json_begin(out, 200);
    wb_printf(&out->body, "{\"token\":\"%s\"}", s->token);
}

/* ---- settings ---- */

/* the settings file takes sections of up to 500 characters: of a longer
   path, the end */
#define SEC_MAX 480

static void file_section(const WebSrv *s, const char *name, char *sec, size_t n)
{
    char path[SYS_PATH];
    size_t l;
    full_path(s, name, path, sizeof path);
    l = strlen(path);
    gui_file_section(l > SEC_MAX ? path + l - SEC_MAX : path, sec, n);
}

/* options of a source: its own section, else the common one */
static int opts_for(const WebSrv *s, const char *pld, GuiOpts *o)
{
    GuiIni *ini = gui_ini_load(s->ini[0] ? s->ini : "");
    char sec[SYS_PATH + 8];
    int own = 0;
    gui_opts_default(o);
    if (!ini) return 0;
    if (pld && pld[0]) {
        file_section(s, pld, sec, sizeof sec);
        own = gui_ini_has_section(ini, sec);
    }
    gui_opts_load(ini, own ? sec : "options", o);
    gui_ini_free(ini);
    return own;
}

static void ini_store(const WebSrv *s, GuiIni *ini)
{
    if (!s->ini[0]) return;
    sys_make_parent(s->ini);
    gui_ini_save(ini, s->ini);
}

static const struct {
    const char *key;
    size_t off;
} opt_keys[] = {
    { "jed", offsetof(GuiOpts, jed) }, { "doc", offsetof(GuiOpts, doc) },
    { "fuseplot", offsetof(GuiOpts, fuseplot) }, { "lst", offsetof(GuiOpts, lst) },
    { "abs", offsetof(GuiOpts, abs) }, { "mx", offsetof(GuiOpts, mx) },
    { "namesrc", offsetof(GuiOpts, namesrc) }, { "secure", offsetof(GuiOpts, secure) },
    { "kb", offsetof(GuiOpts, kb) }, { "min", offsetof(GuiOpts, min) },
    { "simafter", offsetof(GuiOpts, simafter) }, { "compat", offsetof(GuiOpts, compat) },
    { "simlatch", offsetof(GuiOpts, simlatch) }, { "simlst", offsetof(GuiOpts, simlst) },
    { "simjed", offsetof(GuiOpts, simjed) }
};
#define NOPTKEYS (sizeof opt_keys / sizeof opt_keys[0])

static int known_device(const char *mn)
{
    int i;
    for (i = 0; i < gui_ndevs; i++)
        if (!strcmp(gui_devs[i].mn, mn)) return 1;
    return 0;
}

/* the source a tab stands for: name.si -> name.pld */
static void target(const char *name, char *pld, char *si)
{
    if (gui_has_ext(name, ".si")) {
        sput(si, WEB_NAME, "%s", name);
        gui_set_ext(name, ".pld", pld, WEB_NAME);
    } else {
        sput(pld, WEB_NAME, "%s", name);
        gui_set_ext(name, ".si", si, WEB_NAME);
    }
}

static void get_options(WebSrv *s, const HttpReq *r, WebResp *out)
{
    char name[WEB_NAME], pld[WEB_NAME] = "", si[WEB_NAME];
    GuiOpts o;
    GuiIni *ini;
    size_t i;
    int own;
    if (!http_query_get(r->query, "name", name, sizeof name)) {
        if (!web_name_ok(name)) {
            json_error(out, 400, "Bad file name");
            return;
        }
        target(name, pld, si);
    }
    own = opts_for(s, pld, &o);
    json_begin(out, 200);
    wb_puts(&out->body, "{\"source\":");
    wb_json(&out->body, pld);
    wb_printf(&out->body, ",\"perfile\":%d,\"opts\":{", own);
    for (i = 0; i < NOPTKEYS; i++)
        wb_printf(&out->body, "\"%s\":%d,", opt_keys[i].key,
                  *(const int *)((const char *)&o + opt_keys[i].off));
    wb_puts(&out->body, "\"device\":");
    wb_json(&out->body, o.device);
    ini = gui_ini_load(s->ini[0] ? s->ini : "");
    wb_printf(&out->body, "},\"tab\":%d,\"colour\":%d}",
              ini ? gui_ini_int(ini, "editor", "tab", 8) : 8,
              ini ? gui_ini_int(ini, "editor", "colour", 1) != 0 : 1);
    gui_ini_free(ini);
}

/* body: jed=1&doc=0&...&device=g16v8; query: name, perfile */
static void post_options(WebSrv *s, const HttpReq *r, WebResp *out)
{
    char name[WEB_NAME], pld[WEB_NAME] = "", si[WEB_NAME], v[64], sec[SYS_PATH + 8];
    char *form;
    GuiOpts o;
    GuiIni *ini;
    size_t i;
    int perfile = 0;
    if (r->body_len > 2000) {
        json_error(out, 400, "Bad options");
        return;
    }
    if (!http_query_get(r->query, "name", name, sizeof name)) {
        if (!web_name_ok(name)) {
            json_error(out, 400, "Bad file name");
            return;
        }
        target(name, pld, si);
    }
    if (!http_query_get(r->query, "perfile", v, sizeof v)) perfile = !strcmp(v, "1");
    form = malloc(r->body_len + 1);
    if (!form) {
        json_error(out, 500, "Out of memory");
        return;
    }
    memcpy(form, r->body, r->body_len);
    form[r->body_len] = 0;
    gui_opts_default(&o);
    for (i = 0; i < NOPTKEYS; i++) {
        int *p = (int *)((char *)&o + opt_keys[i].off);
        if (http_query_get(form, opt_keys[i].key, v, sizeof v)) continue;
        if (!strcmp(opt_keys[i].key, "min")) {
            if (v[0] >= '0' && v[0] <= '4' && !v[1]) *p = v[0] - '0';
        } else {
            *p = !strcmp(v, "1");
        }
    }
    if (!http_query_get(form, "device", v, sizeof v) && v[0]) {
        if (!known_device(v)) {
            free(form);
            json_error(out, 400, "Unknown device %s", v);
            return;
        }
        sput(o.device, sizeof o.device, "%s", v);
    }
    free(form);
    ini = gui_ini_load(s->ini[0] ? s->ini : "");
    if (!ini) {
        json_error(out, 500, "Out of memory");
        return;
    }
    if (pld[0]) file_section(s, pld, sec, sizeof sec);
    if (pld[0] && perfile) {
        gui_opts_save(ini, sec, &o);
    } else {
        if (pld[0]) gui_ini_del_section(ini, sec);
        gui_opts_save(ini, "options", &o);
    }
    ini_store(s, ini);
    gui_ini_free(ini);
    json_begin(out, 200);
    wb_puts(&out->body, "{\"ok\":1}");
}

static void post_prefs(WebSrv *s, const HttpReq *r, WebResp *out)
{
    GuiIni *ini = gui_ini_load(s->ini[0] ? s->ini : "");
    char v[16];
    if (!ini) {
        json_error(out, 500, "Out of memory");
        return;
    }
    if (!http_query_get(r->query, "tab", v, sizeof v) &&
        (!strcmp(v, "2") || !strcmp(v, "4") || !strcmp(v, "8")))
        gui_ini_set(ini, "editor", "tab", v);
    if (!http_query_get(r->query, "colour", v, sizeof v))
        gui_ini_set_int(ini, "editor", "colour", !strcmp(v, "1"));
    ini_store(s, ini);
    gui_ini_free(ini);
    json_begin(out, 200);
    wb_puts(&out->body, "{\"ok\":1}");
}

/* ---- files ---- */

typedef struct {
    char **v;
    int n, cap;
} Names;

static void list_add(void *ctx, const char *name)
{
    Names *l = ctx;
    size_t len = strlen(name) + 1;
    if (!web_name_ok(name) || l->n >= 5000) return;
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 64;
        char **nv = realloc(l->v, (size_t)cap * sizeof *nv);
        if (!nv) return;
        l->v = nv;
        l->cap = cap;
    }
    l->v[l->n] = malloc(len);
    if (l->v[l->n]) memcpy(l->v[l->n++], name, len);
}

static int name_cmp(const void *a, const void *b)
{
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    int d = gui_stricmp(x, y);
    return d ? d : strcmp(x, y);
}

static void get_files(WebSrv *s, WebResp *out)
{
    Names l = { NULL, 0, 0 };
    int i, first = 1;
    sys_list(s->dir, list_add, &l);
    if (l.n) qsort(l.v, (size_t)l.n, sizeof l.v[0], name_cmp);
    json_begin(out, 200);
    wb_puts(&out->body, "{\"files\":[");
    for (i = 0; i < l.n; i++) {
        char path[SYS_PATH];
        long size = 0;
        SysTime t;
        full_path(s, l.v[i], path, sizeof path);
        if (sys_stat(path, &size, &t) == 1) {
            wb_puts(&out->body, first ? "{\"name\":" : ",{\"name\":");
            wb_json(&out->body, l.v[i]);
            wb_printf(&out->body, ",\"size\":%ld,\"ro\":%d}", size, gui_is_output(l.v[i]));
            first = 0;
        }
        free(l.v[i]);
    }
    free(l.v);
    wb_puts(&out->body, "]}");
}

/* the name parameter and its path; 0, or the answer is written */
static int file_arg(WebSrv *s, const HttpReq *r, WebResp *out, char *name, char *path)
{
    if (http_query_get(r->query, "name", name, WEB_NAME) || !web_name_ok(name)) {
        json_error(out, 400, "Bad file name");
        return -1;
    }
    full_path(s, name, path, SYS_PATH);
    return 0;
}

static void time_text(const SysTime *t, char *out, size_t n)
{
    sput(out, n, "%lld.%09ld", t->sec, t->nsec);
}

static void get_file(WebSrv *s, const HttpReq *r, WebResp *out)
{
    char name[WEB_NAME], path[SYS_PATH], tt[40];
    long size = 0;
    SysTime t;
    FILE *f;
    int st;
    char v[8];
    if (file_arg(s, r, out, name, path)) return;
    st = sys_stat(path, &size, &t);
    if (st != 1) {
        json_error(out, st ? 403 : 404, st ? "Not a regular file: %s" : "No file %s", name);
        return;
    }
    time_text(&t, tt, sizeof tt);
    /* time only: the page asks whether a viewer is stale */
    if (!http_query_get(r->query, "stat", v, sizeof v)) {
        json_begin(out, 200);
        wb_printf(&out->body, "{\"mtime\":\"%s\",\"size\":%ld}", tt, size);
        return;
    }
    if ((unsigned long)size > MAX_FILE) {
        json_error(out, 413, "%s is too large", name);
        return;
    }
    f = fopen(path, "rb");
    if (!f) {
        json_error(out, 403, "Cannot read %s", name);
        return;
    }
    out->status = 200;
    out->ctype = "application/octet-stream";
    out->body.n = 0;
    for (;;) {
        char buf[16384];
        size_t got = fread(buf, 1, sizeof buf, f);
        if (!got) break;
        wb_add(&out->body, buf, got);
        if (out->body.n > MAX_FILE) break;
    }
    fclose(f);
    sput(out->extra, sizeof out->extra,
             "X-FreeCUPL-Eol: %s\r\nX-FreeCUPL-Mtime: %s\r\nX-FreeCUPL-Readonly: %d\r\n",
             gui_detect_lf(out->body.p ? out->body.p : "", out->body.n) ? "lf" : "crlf",
             tt, gui_is_output(name));
}

static void post_file(WebSrv *s, const HttpReq *r, WebResp *out)
{
    char name[WEB_NAME], path[SYS_PATH], tt[40], v[8];
    SysTime t;
    long size = 0;
    FILE *f;
    int st, ok;
    if (file_arg(s, r, out, name, path)) return;
    if (gui_is_output(name)) {
        json_error(out, 403, "%s is an output file", name);
        return;
    }
    st = sys_stat(path, NULL, NULL);
    if (st < 0) {
        json_error(out, 403, "Not a regular file: %s", name);
        return;
    }
    /* new=1: the name must be free */
    if (st == 1 && !http_query_get(r->query, "new", v, sizeof v) && !strcmp(v, "1")) {
        json_error(out, 409, "%s exists", name);
        return;
    }
    f = fopen(path, "wb");
    if (!f) {
        json_error(out, 403, "Cannot write %s", name);
        return;
    }
    ok = fwrite(r->body, 1, r->body_len, f) == r->body_len;
    if (fclose(f)) ok = 0;
    if (!ok || sys_stat(path, &size, &t) != 1) {
        json_error(out, 500, "Cannot write %s", name);
        return;
    }
    time_text(&t, tt, sizeof tt);
    json_begin(out, 200);
    wb_printf(&out->body, "{\"mtime\":\"%s\",\"size\":%ld}", tt, size);
}

/* names the outputs of pld can have: the Name item, the source name,
   no_name; without extension */
static int out_names(const WebSrv *s, const char *pld, char names[3][WEB_NAME])
{
    static char head[65536];
    char path[SYS_PATH], nm[80];
    size_t got = 0;
    int n = 0;
    FILE *f;
    full_path(s, pld, path, sizeof path);
    f = sys_stat(path, NULL, NULL) == 1 ? fopen(path, "rb") : NULL;
    if (f) {
        got = fread(head, 1, sizeof head, f);
        fclose(f);
        if (gui_design_name(head, got, nm, sizeof nm) && web_name_ok(nm) &&
            !strpbrk(nm, " \t"))
            sput(names[n++], WEB_NAME, "%s", nm);
    }
    gui_set_ext(pld, "", names[n], WEB_NAME);
    if (!n || (sys_sep == '/' ? strcmp(names[0], names[n])
                              : gui_stricmp(names[0], names[n])))
        n++;
    strcpy(names[n++], "no_name");
    return n;
}

/* the JEDEC file a run of pld writes, without extension: named after the
   source with -n, else after the Name item, else no_name */
static void jed_name(const WebSrv *s, const char *pld, int namesrc, char *out)
{
    static char head[65536];
    char path[SYS_PATH], nm[80];
    size_t got = 0;
    FILE *f;
    if (namesrc) { gui_set_ext(pld, "", out, WEB_NAME); return; }
    full_path(s, pld, path, sizeof path);
    f = sys_stat(path, NULL, NULL) == 1 ? fopen(path, "rb") : NULL;
    if (f) { got = fread(head, 1, sizeof head, f); fclose(f); }
    sput(out, WEB_NAME, "%s", gui_design_name(head, got, nm, sizeof nm) ? nm : "no_name");
}

/* the files the View buttons open */
static void get_outputs(WebSrv *s, const HttpReq *r, WebResp *out)
{
    static const char *const ext[] = { ".jed", ".doc", ".lst", ".so" };
    char name[WEB_NAME], path[SYS_PATH], pld[WEB_NAME], si[WEB_NAME];
    char names[3][WEB_NAME], base[WEB_NAME], cand[WEB_NAME + 8];
    int n, i, k;
    if (file_arg(s, r, out, name, path)) return;
    target(name, pld, si);
    n = out_names(s, pld, names);
    json_begin(out, 200);
    wb_puts(&out->body, "{\"pld\":");
    wb_json(&out->body, pld);
    wb_puts(&out->body, ",\"si\":");
    wb_json(&out->body, si);
    for (k = 0; k < 4; k++) {
        const char *found = "";
        for (i = 0; i < n && !found[0]; i++) {
            /* only the JEDEC file is named after the Name item */
            if (k >= 1) {
                gui_set_ext(k == 3 ? si : pld, "", base, sizeof base);
                sput(cand, sizeof cand, "%s%s", base, ext[k]);
            } else {
                sput(cand, sizeof cand, "%s%s", names[i], ext[k]);
            }
            full_path(s, cand, path, sizeof path);
            if (web_name_ok(cand) && sys_stat(path, NULL, NULL) == 1) found = cand;
        }
        wb_printf(&out->body, ",\"%s\":", ext[k] + 1);
        wb_json(&out->body, found);
    }
    wb_puts(&out->body, "}");
}

/* ---- messages of a run ---- */

static const char *const kind_names[] = {
    "text", "note", "warn", "error", "context", "abort", "file", "cmd", "summary"
};

static char *sdup(const char *s)
{
    size_t l = strlen(s) + 1;
    char *p = malloc(l);
    if (p) memcpy(p, s, l);
    return p;
}

static void msg_clear(WebSrv *s)
{
    int i;
    for (i = 0; i < s->nmsg; i++) {
        free(s->msg[i].file);
        free(s->msg[i].text);
    }
    s->nmsg = 0;
}

static void msg_add(WebSrv *s, const char *text, int kind, const char *file, int line)
{
    WebMsg *m;
    if (s->nmsg >= MAX_MSGS) return;
    if (s->nmsg == MAX_MSGS - 1) {
        text = "(more output not shown)";
        kind = GM_TEXT;
        file = NULL;
        line = 0;
    }
    if (s->nmsg == s->capmsg) {
        int cap = s->capmsg ? s->capmsg * 2 : 128;
        WebMsg *nm = realloc(s->msg, (size_t)cap * sizeof *nm);
        if (!nm) return;
        s->msg = nm;
        s->capmsg = cap;
    }
    m = &s->msg[s->nmsg];
    m->kind = kind;
    m->line = line;
    m->file = file ? sdup(file) : NULL;
    m->text = sdup(text);
    if (m->text) s->nmsg++;
    else free(m->file);
}

static void msg_tool_line(WebSrv *s, const char *line)
{
    GuiMsg m;
    gui_parse_msg(line, &m);
    if (m.kind == GM_ERROR) s->nerr++;
    else if (m.kind == GM_WARN || m.kind == GM_NOTE) s->nwarn++;
    /* simulator messages count lines of the .si file */
    msg_add(s, line, m.kind, m.line ? (m.code[4] == 's' ? s->si : s->pld) : NULL, m.line);
}

/* The simulator writes its messages into the listing only. */
static void msg_scan_listing(WebSrv *s, const char *so)
{
    char path[SYS_PATH], line[1024], b[1024 + WEB_NAME + 32];
    FILE *f;
    int n = 0;
    full_path(s, so, path, sizeof path);
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        GuiMsg m;
        n++;
        if (line[0] != '[') continue;
        line[strcspn(line, "\r\n")] = 0;
        gui_parse_msg(line, &m);
        if (m.kind != GM_ERROR && m.kind != GM_WARN && m.kind != GM_NOTE) continue;
        if (m.kind == GM_ERROR) s->nerr++;
        else s->nwarn++;
        sput(b, sizeof b, "%s(%d): %s", so, n, line);
        msg_add(s, b, m.kind, so, n);
    }
    fclose(f);
}

/* ---- compile and simulate ---- */

enum { STEP_COMPILE, STEP_SIM };

static void snap_add(WebSrv *s, const char *base, const char *ext)
{
    WebSnap *sn;
    char name[WEB_NAME + 8], path[SYS_PATH];
    if (s->nsnap == (int)(sizeof s->snap / sizeof s->snap[0])) return;
    sput(name, sizeof name, "%s%s", base, ext);
    if (!web_name_ok(name)) return;
    sn = &s->snap[s->nsnap++];
    sput(sn->path, sizeof sn->path, "%s", name);
    full_path(s, name, path, sizeof path);
    sn->existed = sys_stat(path, NULL, &sn->t) == 1;
}

static void step_done(WebSrv *s, int rc);

static void step_start(WebSrv *s)
{
    int step = s->queue[s->qi];
    const char *tool = step == STEP_COMPILE ? "freecupl" : "freecsim";
    const char *file = step == STEP_COMPILE ? s->pld : s->si;
    int flags = step == STEP_COMPILE ? s->flags : 0;
    char exe[SYS_PATH], shown[SYS_PATH], path[SYS_PATH], err[200];
    char jn[WEB_NAME], base[WEB_NAME];
    GuiArgv run, show;

    s->nerr = s->nwarn = 0;
    s->nsnap = 0;
    s->linelen = 0;
    sput(exe, sizeof exe, "%s%c%s%s", s->exedir, sys_sep, tool, sys_exe_suffix);
    if (step == STEP_COMPILE) {
        gui_compile_argv(&s->o, exe, file, flags, &run);
        gui_compile_argv(&s->o, tool, file, flags, &show);
    } else {
        gui_sim_argv(&s->o, exe, file, &run);
        gui_sim_argv(&s->o, tool, file, &show);
    }
    if (gui_join_args(&show, shown, sizeof shown)) sput(shown, sizeof shown, "%s", tool);
    msg_add(s, shown, GM_CMD, NULL, 0);
    sput(s->status, sizeof s->status, "%s %s...",
             step == STEP_COMPILE ? "Compiling" : "Simulating", file);

    full_path(s, s->pld, path, sizeof path);
    gui_set_ext(s->pld, "", base, sizeof base);
    jn[0] = 0;
    if (sys_stat(path, NULL, NULL) == 1) jed_name(s, s->pld, s->o.namesrc, jn);
    if (step == STEP_COMPILE) {
        if (jn[0]) snap_add(s, jn, ".jed");
        snap_add(s, base, ".doc");
        snap_add(s, base, ".abs");
        snap_add(s, base, ".lst");
        snap_add(s, base, ".mx");
    } else if (s->o.simjed) {
        if (jn[0]) snap_add(s, jn, ".jed");
    }
    if (step == STEP_SIM || (flags & GC_SIM)) {
        gui_set_ext(s->si, "", base, sizeof base);
        snap_add(s, base, ".so");
        snap_add(s, base, ".wo");
    }
    sys_now(&s->start);

    s->proc = sys_spawn(run.v, s->dir, err, sizeof err);
    if (!s->proc) {
        char b[SYS_PATH];
        sput(b, sizeof b, "Cannot run %s%s: %s", tool, sys_exe_suffix, err);
        msg_add(s, b, GM_ABORT, NULL, 0);
        step_done(s, 1);
    }
}

static void step_done(WebSrv *s, int rc)
{
    int step = s->queue[s->qi];
    char sum[128], b[WEB_NAME + 16];
    int i;
    for (i = 0; i < s->nsnap; i++) {
        WebSnap *sn = &s->snap[i];
        char path[SYS_PATH];
        SysTime t;
        full_path(s, sn->path, path, sizeof path);
        if (sys_stat(path, NULL, &t) != 1) continue;
        /* some file systems keep two-second times */
        if (!sn->existed || t.sec != sn->t.sec || t.nsec != sn->t.nsec ||
            t.sec + 2 >= s->start.sec) {
            if (gui_has_ext(sn->path, ".so")) msg_scan_listing(s, sn->path);
            sput(b, sizeof b, "    %s", sn->path);
            msg_add(s, b, GM_FILE, sn->path, 0);
        }
    }
    if (s->stopped) msg_add(s, "Stopped.", GM_ABORT, NULL, 0);
    gui_summary(sum, sizeof sum, step == STEP_COMPILE ? "Compile" : "Simulate",
                s->nerr, s->nwarn, rc);
    msg_add(s, sum, rc || s->nerr ? GM_ABORT : GM_SUMMARY, NULL, 0);
    sput(s->status, sizeof s->status, "%s", sum);
    s->qi++;
    if (rc == 0 && !s->stopped && s->qi < s->nq) step_start(s);
    else s->active = 0;
}

void web_tick(WebSrv *s)
{
    int budget = 16;
    while (s->active && s->proc && budget-- > 0) {
        char buf[4096];
        int got = sys_proc_read(s->proc, buf, sizeof buf), i;
        if (got == 0) return;
        if (got < 0) {
            SysProc *p = s->proc;
            if (s->linelen) {
                s->line[s->linelen] = 0;
                msg_tool_line(s, s->line);
                s->linelen = 0;
            }
            s->proc = NULL;
            step_done(s, sys_proc_finish(p));
            return;
        }
        for (i = 0; i < got; i++) {
            if (buf[i] == '\n') {
                if (s->linelen && s->line[s->linelen - 1] == '\r') s->linelen--;
                s->line[s->linelen] = 0;
                msg_tool_line(s, s->line);
                s->linelen = 0;
            } else if (s->linelen + 1 < sizeof s->line && buf[i]) {
                s->line[s->linelen++] = buf[i];
            }
        }
    }
}

void web_end(WebSrv *s)
{
    if (s->proc) {
        sys_proc_kill(s->proc);
        sys_proc_finish(s->proc);
        s->proc = NULL;
    }
    s->active = 0;
    msg_clear(s);
    free(s->msg);
    s->msg = NULL;
    s->capmsg = 0;
}

static void post_run(WebSrv *s, const HttpReq *r, WebResp *out)
{
    char name[WEB_NAME], path[SYS_PATH], action[16], b[WEB_NAME + 40];
    int what, havepld, havesi;
    if (s->active) {
        json_error(out, 409, "A run is in progress");
        return;
    }
    if (http_query_get(r->query, "action", action, sizeof action)) action[0] = 0;
    if (!strcmp(action, "compile")) what = STEP_COMPILE;
    else if (!strcmp(action, "simulate")) what = STEP_SIM;
    else {
        json_error(out, 400, "Bad action");
        return;
    }
    if (file_arg(s, r, out, name, path)) return;
    target(name, s->pld, s->si);
    if (!web_name_ok(s->pld) || !web_name_ok(s->si)) {
        json_error(out, 400, "Bad file name");
        return;
    }
    full_path(s, s->pld, path, sizeof path);
    havepld = sys_stat(path, NULL, NULL) == 1;
    full_path(s, s->si, path, sizeof path);
    havesi = sys_stat(path, NULL, NULL) == 1;
    if (what == STEP_COMPILE && !havepld) {
        json_error(out, 404, "No source file %s", s->pld);
        return;
    }
    if (what == STEP_SIM && !havesi) {
        json_error(out, 404, "No simulation input %s", s->si);
        return;
    }
    opts_for(s, s->pld, &s->o);
    s->nq = s->qi = 0;
    s->flags = 0;
    s->stopped = 0;
    s->gen++;
    msg_clear(s);
    if (what == STEP_COMPILE) {
        s->queue[s->nq++] = STEP_COMPILE;
        if (s->o.simafter) {
            if (havesi) {
                s->flags = GC_SIM;
            } else {
                sput(b, sizeof b, "No %s: not simulated.", s->si);
                msg_add(s, b, GM_TEXT, NULL, 0);
            }
        }
    } else {
        /* the simulator reads the absolute file of the compiler */
        if (havepld) {
            s->queue[s->nq++] = STEP_COMPILE;
            s->flags = GC_ABS;
        }
        s->queue[s->nq++] = STEP_SIM;
    }
    s->active = 1;
    step_start(s);
    json_begin(out, 200);
    wb_printf(&out->body, "{\"gen\":%u}", s->gen);
}

static void get_job(WebSrv *s, const HttpReq *r, WebResp *out)
{
    char v[16];
    int from = 0, i;
    if (!http_query_get(r->query, "from", v, sizeof v)) from = atoi(v);
    if (from < 0 || from > s->nmsg) from = s->nmsg;
    json_begin(out, 200);
    wb_printf(&out->body, "{\"gen\":%u,\"active\":%d,\"total\":%d,\"status\":",
              s->gen, s->active, s->nmsg);
    wb_json(&out->body, s->status);
    wb_puts(&out->body, ",\"msgs\":[");
    for (i = from; i < s->nmsg; i++) {
        const WebMsg *m = &s->msg[i];
        wb_printf(&out->body, "%s{\"k\":\"%s\",\"line\":%d,\"file\":", i > from ? "," : "",
                  kind_names[m->kind], m->line);
        wb_json(&out->body, m->file ? m->file : "");
        wb_puts(&out->body, ",\"text\":");
        wb_json(&out->body, m->text);
        wb_puts(&out->body, "}");
    }
    wb_puts(&out->body, "]}");
}

/* ---- devices ---- */

static void get_devices(const HttpReq *r, WebResp *out)
{
    char q[128], v[8];
    int k, n, col = -1, desc = 0, *idx;
    if (http_query_get(r->query, "q", q, sizeof q)) q[0] = 0;
    /* s: column to sort by (0 mnemonic, 1 pins, 2 part), d=1: downwards */
    if (!http_query_get(r->query, "s", v, sizeof v) && v[0] >= '0' && v[0] <= '2' && !v[1]) col = v[0] - '0';
    if (!http_query_get(r->query, "d", v, sizeof v) && !strcmp(v, "1")) desc = 1;
    idx = (int *)malloc(sizeof(int) * (size_t)(gui_ndevs + 1));
    if (!idx) { json_error(out, 500, "out of memory"); return; }
    n = gui_dev_list(q, col, desc, idx);
    json_begin(out, 200);
    wb_printf(&out->body, "{\"total\":%d,\"devs\":[", gui_ndevs);
    for (k = 0; k < n; k++) {
        const GuiDev *d = &gui_devs[idx[k]];
        wb_puts(&out->body, k ? ",[" : "[");
        wb_json(&out->body, d->mn);
        wb_printf(&out->body, ",%d,", d->pins);
        wb_json(&out->body, d->part);
        wb_puts(&out->body, "]");
    }
    wb_puts(&out->body, "]}");
    free(idx);
}

/* Where the Device statement of the posted text is, and the statement
   for mnemonic mn.  The page sends one byte for every character. */
static void post_devstmt(const HttpReq *r, WebResp *out)
{
    char mn[40];
    size_t a = 0, b = 0;
    int found;
    if (http_query_get(r->query, "mn", mn, sizeof mn) || !known_device(mn)) {
        json_error(out, 400, "Unknown device");
        return;
    }
    found = gui_device_stmt(r->body, r->body_len, &a, &b);
    json_begin(out, 200);
    wb_printf(&out->body, "{\"found\":%d,\"start\":%lu,\"end\":%lu,\"text\":\"Device   %s ;\"}",
              found, (unsigned long)a, (unsigned long)b, mn);
}

/* ---- the page ---- */

static void get_info(WebSrv *s, WebResp *out)
{
    char skel[400];
    const char *e = sys_eol;
    sput(skel, sizeof skel,
             "Name     XXXXX ;%sPartNo   XXXXX ;%sDate     XX/XX/XX ;%s"
             "Revision XX ;%sDesigner XXXXX ;%sCompany  XXXXX ;%s"
             "Assembly XXXXX ;%sLocation XXXXX ;%sDevice   XXXXX ;%s%s",
             e, e, e, e, e, e, e, e, e, e);
    json_begin(out, 200);
    wb_puts(&out->body, "{\"version\":");
    wb_json(&out->body, FREECUPL_VERSION_STRING);
    wb_puts(&out->body, ",\"dir\":");
    wb_json(&out->body, s->dir);
    wb_printf(&out->body, ",\"devices\":%d,\"eol\":\"%s\",\"skeleton\":", gui_ndevs,
              sys_eol[0] == '\r' ? "crlf" : "lf");
    wb_json(&out->body, skel);
    wb_puts(&out->body, "}");
}

static void fixed(WebResp *out, const char *ctype, const unsigned char *data, unsigned len)
{
    out->status = 200;
    out->ctype = ctype;
    out->body.n = 0;
    wb_add(&out->body, data, len);
}

void web_handle(WebSrv *s, const HttpReq *r, WebResp *out)
{
    const char *p = r->path;
    int post = !strcmp(r->method, "POST");
    int st = web_check(s, r);

    out->extra[0] = 0;
    out->quit = 0;
    if (st) {
        web_error(out, st, "Forbidden.  Open the address freecupl serve printed.");
        return;
    }
    if (!strcmp(p, "/") && !post) {
        fixed(out, "text/html; charset=utf-8", web_index_html, web_index_html_len);
    } else if (!strcmp(p, "/app.css") && !post) {
        fixed(out, "text/css; charset=utf-8", web_app_css, web_app_css_len);
    } else if (!strcmp(p, "/app.js") && !post) {
        fixed(out, "text/javascript; charset=utf-8", web_app_js, web_app_js_len);
    } else if (!strcmp(p, "/kw.js") && !post) {
        fixed(out, "text/javascript; charset=utf-8", web_kw_js, web_kw_js_len);
    } else if (!strcmp(p, "/favicon.ico") && !post) {
        fixed(out, "image/x-icon", web_favicon_ico, web_favicon_ico_len);
    } else if (!strcmp(p, "/api/guide") && !post) {
        fixed(out, "text/plain; charset=utf-8", web_userguide_txt, web_userguide_txt_len);
    } else if (!strcmp(p, "/api/session") && post) {
        post_session(s, r, out);
    } else if (!strcmp(p, "/api/info") && !post) {
        get_info(s, out);
    } else if (!strcmp(p, "/api/files") && !post) {
        get_files(s, out);
    } else if (!strcmp(p, "/api/file")) {
        if (post) post_file(s, r, out);
        else get_file(s, r, out);
    } else if (!strcmp(p, "/api/outputs") && !post) {
        get_outputs(s, r, out);
    } else if (!strcmp(p, "/api/options")) {
        if (post) post_options(s, r, out);
        else get_options(s, r, out);
    } else if (!strcmp(p, "/api/prefs") && post) {
        post_prefs(s, r, out);
    } else if (!strcmp(p, "/api/run") && post) {
        post_run(s, r, out);
    } else if (!strcmp(p, "/api/job") && !post) {
        get_job(s, r, out);
    } else if (!strcmp(p, "/api/stop") && post) {
        if (s->active && s->proc) {
            s->stopped = 1;
            sys_proc_kill(s->proc);
        }
        json_begin(out, 200);
        wb_puts(&out->body, "{\"ok\":1}");
    } else if (!strcmp(p, "/api/devices") && !post) {
        get_devices(r, out);
    } else if (!strcmp(p, "/api/devstmt") && post) {
        post_devstmt(r, out);
    } else if (!strcmp(p, "/api/quit") && post) {
        json_begin(out, 200);
        wb_puts(&out->body, "{\"ok\":1}");
        out->quit = 1;
    } else {
        json_error(out, 404, "Not found");
    }
}
