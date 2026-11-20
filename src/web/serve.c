/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl serve - the editor page for a browser on this machine.  One
   thread; listens on 127.0.0.1 only.  DARKSOFT. */
#include "web.h"
#include "version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXCONN 32
#define IDLE_SEC 15

typedef struct {
    SysSock sock;
    char *in;               /* the request so far */
    size_t nin, capin;
    char *out;              /* the answer; NULL while reading */
    size_t nout, sent;
    long long since;        /* last progress */
    unsigned long seq;      /* order of arrival */
    int checked;            /* the header passed web_check */
    int drain;              /* refused: the rest of the request is read
                               and dropped, so that the answer arrives */
    size_t dropped;
    int quit;
} Conn;

static Conn conns[MAXCONN];

static void conn_close(Conn *c)
{
    sys_close(c->sock);
    free(c->in);
    free(c->out);
    memset(c, 0, sizeof *c);
    c->sock = SYS_NOSOCK;
}

/* header and body of the answer in one block */
static void answer(Conn *c, WebResp *r)
{
    WBuf h = { NULL, 0, 0 };
    wb_printf(&h, "HTTP/1.1 %d %s\r\n", r->status, http_reason(r->status));
    wb_printf(&h, "Content-Type: %s\r\n", r->ctype);
    wb_printf(&h, "Content-Length: %lu\r\n", (unsigned long)r->body.n);
    wb_puts(&h, "Connection: close\r\nCache-Control: no-store\r\n"
                "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\n"
                "Referrer-Policy: no-referrer\r\n"
                "Cross-Origin-Resource-Policy: same-origin\r\n"
                "Content-Security-Policy: default-src 'none'; script-src 'self'; "
                "style-src 'self'; img-src 'self'; connect-src 'self'; "
                "base-uri 'none'; form-action 'none'; frame-ancestors 'none'\r\n");
    wb_puts(&h, r->extra);
    wb_puts(&h, "\r\n");
    if (r->body.n) wb_add(&h, r->body.p, r->body.n);
    c->out = h.p;
    c->nout = h.n;
    c->sent = 0;
    c->quit = r->quit;
    if (!c->out) conn_close(c);
}

static void conn_read(Conn *c, WebSrv *srv)
{
    HttpReq req;
    WebResp resp;
    int got, r;
    if (c->drain == 2) {
        char junk[16384];
        got = sys_recv(c->sock, junk, sizeof junk);
        if (got > 0) c->dropped += (size_t)got;
        if (got < 0 || c->dropped > HTTP_MAX_HEAD + 2 * HTTP_MAX_BODY) conn_close(c);
        return;
    }
    if (c->nin + 16384 > c->capin) {
        size_t cap = c->capin ? c->capin * 2 : 32768;
        char *np = realloc(c->in, cap);
        if (!np) {
            conn_close(c);
            return;
        }
        c->in = np;
        c->capin = cap;
    }
    got = sys_recv(c->sock, c->in + c->nin, 16384);
    if (got < 0) {
        conn_close(c);
        return;
    }
    c->nin += (size_t)got;
    if (got > 0) {
        SysTime now;
        sys_now(&now);
        c->since = now.sec;
    }
    memset(&resp, 0, sizeof resp);
    r = 0;
    if (!c->checked) {
        /* refused before its body is taken */
        size_t hl, bl;
        r = http_head(c->in, c->nin, &req, &hl, &bl);
        if (r > 0) {
            int st = web_check(srv, &req);
            c->checked = 1;
            r = st ? -st : 0;
        }
    }
    if (!r) r = http_parse(c->in, c->nin, &req);
    if (!r) {
        if (c->nin > HTTP_MAX_HEAD + HTTP_MAX_BODY) conn_close(c);
        return;
    }
    if (r < 0) c->drain = 1;
    if (r < 0) web_error(&resp, -r, r == -403 ? "Forbidden.  Open the address freecupl serve printed." : NULL);
    else web_handle(srv, &req, &resp);
    answer(c, &resp);
    free(resp.body.p);
}

static void conn_write(Conn *c)
{
    int n = sys_send(c->sock, c->out + c->sent,
                     c->nout - c->sent > 65536 ? 65536 : (int)(c->nout - c->sent));
    if (n < 0) {
        conn_close(c);
        return;
    }
    c->sent += (size_t)n;
    if (n > 0) {
        SysTime now;
        sys_now(&now);
        c->since = now.sec;
    }
    if (c->sent == c->nout) {
        if (c->quit) sys_stop = 1;
        if (c->drain) {
            free(c->out);
            c->out = NULL;
            c->drain = 2;
        } else {
            conn_close(c);
        }
    }
}

static void usage(void)
{
    printf("usage: freecupl serve [--port N] [--no-browser] [directory]\n"
           "  Serves an editor page for the .pld and .si files of the directory\n"
           "  (default: the current one) to a browser on this machine, at\n"
           "  http://127.0.0.1:N/ (default: a free port), and opens it.\n"
           "  Ends with Ctrl+C or the Quit button of the page.\n");
}

int serve_main(int argc, char **argv)
{
    static WebSrv srv;
    static const char hex[] = "0123456789abcdef";
    const char *dir = NULL, *env;
    unsigned char rnd[32];
    char url[128], first[128];
    unsigned long seq = 0;
    SysSock ls;
    int i, port = 0, browser = 1;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            char *e;
            long v = strtol(argv[++i], &e, 10);
            if (*e || v < 0 || v > 65535) {
                fprintf(stderr, "freecupl serve: bad port %s\n", argv[i]);
                return 2;
            }
            port = (int)v;
        } else if (!strcmp(argv[i], "--no-browser")) {
            browser = 0;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage();
            return 0;
        } else if (argv[i][0] == '-' || dir) {
            usage();
            return 2;
        } else {
            dir = argv[i];
        }
    }
    if (sys_init()) {
        fprintf(stderr, "freecupl serve: no network sockets\n");
        return 1;
    }
    if (sys_full_dir(dir ? dir : ".", srv.dir, sizeof srv.dir)) {
        fprintf(stderr, "freecupl serve: no directory %s\n", dir ? dir : ".");
        return 1;
    }
    if (strlen(srv.dir) > SYS_PATH - WEB_NAME - 16) {
        fprintf(stderr, "freecupl serve: the name of the directory is too long\n");
        return 1;
    }
    if (sys_exe_dir(argv[0], srv.exedir, sizeof srv.exedir)) {
        fprintf(stderr, "freecupl serve: cannot find the directory of the program\n");
        return 1;
    }
    env = getenv("FREECUPL_SERVE_INI");
    if (env && *env && strlen(env) < sizeof srv.ini) strcpy(srv.ini, env);
    else sys_config_path(srv.exedir, srv.ini, sizeof srv.ini);
    if (sys_random(rnd, sizeof rnd)) {
        fprintf(stderr, "freecupl serve: no random numbers for the session token\n");
        return 1;
    }
    for (i = 0; i < 16; i++) {
        srv.token[2 * i] = hex[rnd[i] >> 4];
        srv.token[2 * i + 1] = hex[rnd[i] & 15];
        srv.ticket[2 * i] = hex[rnd[16 + i] >> 4];
        srv.ticket[2 * i + 1] = hex[rnd[16 + i] & 15];
    }
    srv.token[32] = srv.ticket[32] = 0;
    ls = sys_listen(&port);
    if (ls == SYS_NOSOCK) {
        fprintf(stderr, "freecupl serve: cannot listen on 127.0.0.1 port %d\n", port);
        return 1;
    }
    srv.port = port;
    for (i = 0; i < MAXCONN; i++) conns[i].sock = SYS_NOSOCK;
    /* behind '#': not part of any request */
    snprintf(url, sizeof url, "http://127.0.0.1:%d/#t=%s", port, srv.token);
    printf("%s\n", FREECUPL_VERSION_STRING);
    printf("Directory: %s\n", srv.dir);
    printf("Open %s\n", url);
    printf("Ctrl+C ends.\n");
    fflush(stdout);
    /* the browser is given a ticket, which the page trades for the token
       once: its command line can be read by other users of the machine */
    if (browser) {
        snprintf(first, sizeof first, "http://127.0.0.1:%d/#k=%s", port, srv.ticket);
        sys_open_browser(first);
    } else {
        srv.ticket[0] = 0;
    }

    while (!sys_stop) {
        SysSock rd[MAXCONN + 1], wr[MAXCONN];
        unsigned char rdy[MAXCONN + 1], wdy[MAXCONN];
        SysTime now;
        for (i = 0; i < MAXCONN; i++) {
            Conn *c = &conns[i];
            rd[i] = c->sock != SYS_NOSOCK && !c->out ? c->sock : SYS_NOSOCK;
            wr[i] = c->sock != SYS_NOSOCK && c->out ? c->sock : SYS_NOSOCK;
        }
        rd[MAXCONN] = ls;
        sys_wait(rd, rdy, MAXCONN + 1, wr, wdy, MAXCONN, srv.proc, srv.active ? 50 : 500);
        web_tick(&srv);
        sys_now(&now);
        if (rdy[MAXCONN]) {
            SysSock s = sys_accept(ls);
            int at = -1, old = -1;
            for (i = 0; s != SYS_NOSOCK && i < MAXCONN; i++) {
                if (conns[i].sock == SYS_NOSOCK) {
                    at = i;
                    break;
                }
                if (!conns[i].out && (old < 0 || conns[i].seq < conns[old].seq)) old = i;
            }
            /* all taken: the oldest one that has not finished its request
               goes, so that silent connections cannot keep the page out */
            if (s != SYS_NOSOCK && at < 0 && old >= 0) {
                conn_close(&conns[old]);
                at = old;
            }
            if (at >= 0) {
                conns[at].sock = s;
                conns[at].since = now.sec;
                conns[at].seq = ++seq;
                /* not read before the next round */
                rdy[at] = wdy[at] = 0;
            } else if (s != SYS_NOSOCK) {
                sys_close(s);
            }
        }
        for (i = 0; i < MAXCONN; i++) {
            Conn *c = &conns[i];
            if (c->sock == SYS_NOSOCK) continue;
            if (rdy[i] && !c->out) conn_read(c, &srv);
            else if (wdy[i] && c->out) conn_write(c);
            else if (now.sec - c->since > (c->drain == 2 ? 2 : IDLE_SEC)) conn_close(c);
        }
    }
    for (i = 0; i < MAXCONN; i++)
        if (conns[i].sock != SYS_NOSOCK) conn_close(&conns[i]);
    sys_close(ls);
    web_end(&srv);
    printf("freecupl serve: ended\n");
    return 0;
}
