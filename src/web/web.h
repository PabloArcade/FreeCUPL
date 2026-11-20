/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl serve - the requests of the page: files of the working
   directory, options, compile and simulate, devices.  DARKSOFT. */
#ifndef FREECUPL_WEB_H
#define FREECUPL_WEB_H
#include "http.h"
#include "sys.h"
#include "gui/core.h"

typedef struct {
    char *p;
    size_t n, cap;
} WBuf;
void wb_add(WBuf *b, const void *data, size_t n);
void wb_puts(WBuf *b, const char *s);
void wb_printf(WBuf *b, const char *fmt, ...);
/* s as a JSON string, quotes included */
void wb_json(WBuf *b, const char *s);

typedef struct {
    int status;
    const char *ctype;
    WBuf body;
    char extra[256];        /* more header lines, each with CR LF */
    int quit;               /* end the server after this answer */
} WebResp;

#define WEB_NAME 256
typedef struct {
    int kind, line;
    char *file;             /* file the line number counts in, or NULL */
    char *text;
} WebMsg;

typedef struct {
    char path[SYS_PATH];
    int existed;
    SysTime t;
} WebSnap;

typedef struct {
    char dir[SYS_PATH];     /* working directory, absolute */
    char exedir[SYS_PATH];  /* freecupl and freecsim are run from here */
    char ini[SYS_PATH];     /* settings file, "" for none */
    char token[40];
    char ticket[40];        /* given to the browser that is opened and
                               traded for the token; "" when used */
    int port;
    /* the run */
    int active, stopped, nq, qi, queue[2], flags;
    unsigned gen;           /* counts the runs */
    char pld[WEB_NAME], si[WEB_NAME];
    GuiOpts o;
    SysProc *proc;
    char line[2048];
    size_t linelen;
    int nerr, nwarn;
    WebSnap snap[16];
    int nsnap;
    SysTime start;
    WebMsg *msg;
    int nmsg, capmsg;
    char status[200];
} WebSrv;

/* A file name the page may use: one component, no dot file, nothing
   Windows gives a meaning to. */
int web_name_ok(const char *name);
/* 0 when the request may be served, else the status that refuses it */
int web_check(const WebSrv *s, const HttpReq *r);
void web_handle(WebSrv *s, const HttpReq *r, WebResp *out);
/* an answer without a request behind it */
void web_error(WebResp *out, int status, const char *text);
/* reads the output of the running tool, starts the next step */
void web_tick(WebSrv *s);
void web_end(WebSrv *s);

/* the page (build/gen/webpage.c) */
extern const unsigned char web_index_html[], web_app_css[], web_app_js[],
                           web_kw_js[], web_favicon_ico[], web_userguide_txt[];
extern const unsigned web_index_html_len, web_app_css_len, web_app_js_len,
                      web_kw_js_len, web_favicon_ico_len, web_userguide_txt_len;

int serve_main(int argc, char **argv);

#endif
