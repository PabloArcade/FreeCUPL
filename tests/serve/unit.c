/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Tests of freecupl serve without a socket: request parsing, the checks
   on Host, Origin and token, file names, and the request handlers on a
   scratch directory.  DARKSOFT.
   usage: unit WORKDIR BINDIR   (WORKDIR empty; BINDIR has freecupl) */
#include "web/web.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failed;

#define CHECK(c) do { checks++; if (!(c)) { failed++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define TOKEN "0123456789abcdef0123456789abcdef"
#define PORT 8477
#define HOST "Host: 127.0.0.1:8477\r\n"
#define AUTH HOST "X-FreeCUPL-Token: " TOKEN "\r\n"

static WebSrv srv;
static WebResp resp;
static char reqbuf[70000];

/* ---- http.c ---- */

static int parse(const char *text, HttpReq *r)
{
    return http_parse(text, strlen(text), r);
}

static void test_parse(void)
{
    HttpReq r;
    char big[HTTP_MAX_HEAD + 200];
    const char *t;
    size_t i;
    int n;

    t = "GET /api/file?name=a.pld&t=x HTTP/1.1\r\nhOsT:  127.0.0.1:8477 \r\n"
        "Cookie: a=1\r\nOrigin: http://127.0.0.1:8477\r\n"
        "X-FreeCUPL-Token: abc\r\nAccept: */*\r\n\r\n";
    n = parse(t, &r);
    CHECK(n == (int)strlen(t));
    CHECK(!strcmp(r.method, "GET"));
    CHECK(!strcmp(r.path, "/api/file"));
    CHECK(!strcmp(r.query, "name=a.pld&t=x"));
    CHECK(!strcmp(r.host, "127.0.0.1:8477"));
    CHECK(!strcmp(r.origin, "http://127.0.0.1:8477"));
    CHECK(!strcmp(r.token, "abc"));
    CHECK(r.body_len == 0);
    /* every shorter piece is incomplete, never an error */
    for (i = 0; i < strlen(t); i++) CHECK(http_parse(t, i, &r) == 0);

    t = "POST /api/file?name=a HTTP/1.1\r\n" HOST "Content-Length: 5\r\n\r\nhello";
    n = parse(t, &r);
    CHECK(n == (int)strlen(t) && r.body_len == 5 && !memcmp(r.body, "hello", 5));
    CHECK(!strcmp(r.method, "POST") && !r.origin[0] && !r.token[0]);
    CHECK(http_parse(t, strlen(t) - 1, &r) == 0);
    /* a second request behind the first is not part of it */
    t = "POST / HTTP/1.1\r\n" HOST "Content-Length: 2\r\n\r\nabGET / HTTP/1.1\r\n\r\n";
    CHECK(parse(t, &r) == (int)(strstr(t, "abGET") - t) + 2);
    CHECK(parse("GET / HTTP/1.0\r\n" HOST "\r\n", &r) > 0);
    CHECK(parse("POST / HTTP/1.1\r\n" HOST "Content-Length: 0\r\n\r\n", &r) > 0);

    CHECK(parse("GET / HTTP/1.1\r\n\r\n", &r) == -400);                 /* no Host */
    CHECK(parse("GET / HTTP/1.1\r\n" HOST HOST "\r\n", &r) == -400);
    CHECK(parse("GET / HTTP/2.0\r\n" HOST "\r\n", &r) == -505);
    CHECK(parse("GET / HTTP/1.2\r\n" HOST "\r\n", &r) == -505);
    CHECK(parse("GET / FTP/1.1\r\n" HOST "\r\n", &r) == -400);
    CHECK(parse("GET /\r\n" HOST "\r\n", &r) == -400);
    CHECK(parse("GET\r\n" HOST "\r\n", &r) == -400);
    CHECK(parse("\r\n\r\n", &r) == -400);
    CHECK(parse("PUT / HTTP/1.1\r\n" HOST "\r\n", &r) == -405);
    CHECK(parse("DELETE / HTTP/1.1\r\n" HOST "\r\n", &r) == -405);
    CHECK(parse("OPTIONS / HTTP/1.1\r\n" HOST "\r\n", &r) == -405);
    CHECK(parse("get / HTTP/1.1\r\n" HOST "\r\n", &r) == -405);
    CHECK(parse("GET http://127.0.0.1:8477/ HTTP/1.1\r\n" HOST "\r\n", &r) == -400);
    CHECK(parse("GET * HTTP/1.1\r\n" HOST "\r\n", &r) == -400);
    CHECK(parse("GET /a\tb HTTP/1.1\r\n" HOST "\r\n", &r) == -400);
    CHECK(parse("POST / HTTP/1.1\r\n" HOST "\r\n", &r) == -411);
    CHECK(parse("POST / HTTP/1.1\r\n" HOST "Content-Length: 4194305\r\n\r\n", &r) == -413);
    CHECK(parse("POST / HTTP/1.1\r\n" HOST "Content-Length: 4194304\r\n\r\n", &r) == 0);
    CHECK(parse("POST / HTTP/1.1\r\n" HOST "Content-Length: 99999999999\r\n\r\n", &r) == -400);
    CHECK(parse("POST / HTTP/1.1\r\n" HOST "Content-Length: -1\r\n\r\n", &r) == -400);
    CHECK(parse("POST / HTTP/1.1\r\n" HOST "Content-Length: 1x\r\n\r\n", &r) == -400);
    CHECK(parse("POST / HTTP/1.1\r\n" HOST "Content-Length: 1\r\nContent-Length: 1\r\n\r\na", &r) == -400);
    CHECK(parse("POST / HTTP/1.1\r\n" HOST "Content-Length:\r\n\r\n", &r) == -400);
    CHECK(parse("POST / HTTP/1.1\r\n" HOST "Transfer-Encoding: chunked\r\n\r\n", &r) == -501);
    CHECK(parse("GET / HTTP/1.1\r\n" HOST "Content-Length: 3\r\n\r\nabc", &r) == -400);
    CHECK(parse("GET / HTTP/1.1\r\n" HOST "Origin: http://a\r\nOrigin: http://b\r\n\r\n", &r) == -400);
    CHECK(parse("GET / HTTP/1.1\r\n" HOST " folded: x\r\n\r\n", &r) == -400);
    CHECK(parse("GET / HTTP/1.1\r\n" HOST "NoColon\r\n\r\n", &r) == -400);
    CHECK(parse("GET / HTTP/1.1\r\n" HOST ": empty\r\n\r\n", &r) == -400);
    CHECK(parse("GET / HTTP/1.1\r\n" HOST "Name : x\r\n\r\n", &r) == -400);
    CHECK(parse("GET / HTTP/1.1\r\n" HOST "A: b\rc\r\n\r\n", &r) == -400);
    /* a NUL in the header */
    {
        char z[] = "GET / HTTP/1.1\r\nHost: 127.0.0.1:8477\r\nA: b_c\r\n\r\n";
        *strchr(z, '_') = 0;
        CHECK(http_parse(z, sizeof z - 1, &r) == -400);
    }
    /* limits */
    memset(big, 'a', sizeof big);
    memcpy(big, "GET / HTTP/1.1\r\nX: ", 19);
    CHECK(http_parse(big, HTTP_MAX_HEAD - 1, &r) == 0);
    CHECK(http_parse(big, HTTP_MAX_HEAD, &r) == -431);
    CHECK(http_parse(big, sizeof big, &r) == -431);
    memset(big, 'a', sizeof big);
    memcpy(big, "GET /", 5);
    strcpy(big + 300, " HTTP/1.1\r\n" HOST "\r\n");
    CHECK(parse(big, &r) == -414);
    memset(big, 'a', sizeof big);
    memcpy(big, "GET /?", 6);
    strcpy(big + 1100, " HTTP/1.1\r\n" HOST "\r\n");
    CHECK(parse(big, &r) == -414);
    memset(big, 'a', sizeof big);
    memcpy(big, "GET / HTTP/1.1\r\n" HOST "X-FreeCUPL-Token: ", 56);
    strcpy(big + 200, "\r\n\r\n");
    CHECK(parse(big, &r) == -400);
}

static void test_head(void)
{
    HttpReq r;
    size_t hl = 0, bl = 0;
    const char *t = "POST /api/file?name=a HTTP/1.1\r\n" HOST "Content-Length: 500\r\n\r\nab";
    /* the header is known before the body is there */
    CHECK(http_head(t, strlen(t), &r, &hl, &bl) == 1 && bl == 500 && hl == strlen(t) - 2);
    CHECK(!strcmp(r.path, "/api/file") && !strcmp(r.host, "127.0.0.1:8477") && r.body_len == 0);
    CHECK(http_parse(t, strlen(t), &r) == 0);
    CHECK(http_head(t, 20, &r, &hl, &bl) == 0);
    CHECK(http_head("PUT / HTTP/1.1\r\n" HOST "\r\n", 40, &r, &hl, &bl) == -405);
}

static void test_fields(void)
{
    char v[16];
    CHECK(!http_query_get("a=1&name=x%20y+z&b=", "name", v, sizeof v) && !strcmp(v, "x y z"));
    CHECK(!http_query_get("a=1&name=x", "a", v, sizeof v) && !strcmp(v, "1"));
    CHECK(!http_query_get("a=1&b=", "b", v, sizeof v) && !v[0]);
    CHECK(!http_query_get("name=%2e%2E%2f", "name", v, sizeof v) && !strcmp(v, "../"));
    CHECK(http_query_get("a=1", "name", v, sizeof v) < 0);
    CHECK(http_query_get("xname=1", "name", v, sizeof v) < 0);
    CHECK(http_query_get("name", "name", v, sizeof v) < 0);
    CHECK(http_query_get("", "name", v, sizeof v) < 0);
    CHECK(http_query_get("name=%zz", "name", v, sizeof v) < 0);
    CHECK(http_query_get("name=%2", "name", v, sizeof v) < 0);
    CHECK(http_query_get("name=%", "name", v, sizeof v) < 0);
    CHECK(http_query_get("name=a%00b", "name", v, sizeof v) < 0);
    CHECK(http_query_get("name=0123456789abcdef", "name", v, sizeof v) < 0);
    CHECK(!http_query_get("name=0123456789abcde", "name", v, sizeof v));
    /* the first one counts */
    CHECK(!http_query_get("name=a&name=b", "name", v, sizeof v) && !strcmp(v, "a"));

    CHECK(http_host_ok("127.0.0.1:8477", 8477));
    CHECK(http_host_ok("localhost:8477", 8477));
    CHECK(http_host_ok("LocalHost:8477", 8477));
    CHECK(http_host_ok("127.0.0.1", 80) && http_host_ok("localhost", 80));
    CHECK(!http_host_ok("127.0.0.1", 8477));
    CHECK(!http_host_ok("127.0.0.1:8478", 8477));
    CHECK(!http_host_ok("127.0.0.1:84770", 8477));
    CHECK(!http_host_ok("127.0.0.1:8477 ", 8477));
    CHECK(!http_host_ok("127.0.0.1:08477", 8477));
    CHECK(!http_host_ok("127.0.0.1:", 8477));
    CHECK(!http_host_ok("", 8477));
    CHECK(!http_host_ok("attacker.example:8477", 8477));
    CHECK(!http_host_ok("attacker.example", 80));
    CHECK(!http_host_ok("127.0.0.1.attacker.example:8477", 8477));
    CHECK(!http_host_ok("localhost.attacker.example:8477", 8477));
    CHECK(!http_host_ok("127.0.0.1:8477@attacker.example", 8477));
    CHECK(!http_host_ok("attacker.example:8477@127.0.0.1:8477", 8477));
    CHECK(!http_host_ok("127.0.0.2:8477", 8477));
    CHECK(!http_host_ok("0.0.0.0:8477", 8477));
    CHECK(!http_host_ok("[::1]:8477", 8477));
    CHECK(!http_host_ok("192.168.1.10:8477", 8477));

    CHECK(http_origin_ok("http://127.0.0.1:8477", 8477));
    CHECK(http_origin_ok("http://localhost:8477", 8477));
    CHECK(!http_origin_ok("https://127.0.0.1:8477", 8477));
    CHECK(!http_origin_ok("http://127.0.0.1:8477/", 8477));
    CHECK(!http_origin_ok("http://attacker.example", 8477));
    CHECK(!http_origin_ok("http://127.0.0.1:8477.attacker.example", 8477));
    CHECK(!http_origin_ok("null", 8477));
    CHECK(!http_origin_ok("127.0.0.1:8477", 8477));

    CHECK(http_token_eq(TOKEN, TOKEN));
    CHECK(!http_token_eq(TOKEN, "0123456789abcdef0123456789abcdeF"));
    CHECK(!http_token_eq(TOKEN, "0123456789abcdef0123456789abcde"));
    CHECK(!http_token_eq(TOKEN, TOKEN "0"));
    CHECK(!http_token_eq("", TOKEN) && !http_token_eq(TOKEN, ""));
    /* an empty token never matches, not even an empty one */
    CHECK(!http_token_eq("", ""));
}

static void test_names(void)
{
    static const char *const good[] = {
        "a.pld", "counter4.si", "A B.pld", "x", "x.y.z", "no_name.jed", "a-b.pld",
        "console.pld", "com.pld", "com0.pld", "lpt10.pld", "nulls.pld", "a..b"
    };
    static const char *const bad[] = {
        "", ".", "..", ".hidden", "../a.pld", "..\\a.pld", "a/b.pld", "a\\b.pld",
        "/etc/passwd", "C:\\x.pld", "c:x.pld", "a.pld:stream", "a*.pld", "a?.pld",
        "a\"b.pld", "a<b.pld", "a>b.pld", "a|b.pld", "a\nb.pld", "a\tb.pld", "a\177.pld",
        "a.pld.", "a.pld ", " a.pld", "-j.pld", "-", "nul", "NUL", "nul.pld", "CON.txt",
        "prn", "aux.c", "com1", "COM9.pld", "lpt1.txt", "Lpt5", "con .pld", "nul  .txt",
        "com1 .x", "lpt9 "
    };
    char longname[300];
    size_t i;
    for (i = 0; i < sizeof good / sizeof good[0]; i++) {
        checks++;
        if (!web_name_ok(good[i])) { failed++; printf("FAIL name refused: %s\n", good[i]); }
    }
    for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        checks++;
        if (web_name_ok(bad[i])) { failed++; printf("FAIL name accepted: %s\n", bad[i]); }
    }
    memset(longname, 'a', sizeof longname);
    longname[199] = 0;
    CHECK(web_name_ok(longname));
    longname[199] = 'a';
    longname[200] = 0;
    CHECK(!web_name_ok(longname));
    if (sys_sep == '/') CHECK(web_name_ok("caf\303\251.pld"));
    else CHECK(!web_name_ok("caf\303\251.pld"));
}

/* ---- api.c ---- */

/* sends one request through the parser and the handlers */
static int call(const char *head, const char *body, size_t blen)
{
    HttpReq r;
    size_t hl = strlen(head);
    int n;
    if (hl + blen >= sizeof reqbuf) return -1;
    memcpy(reqbuf, head, hl);
    if (blen) memcpy(reqbuf + hl, body, blen);
    resp.body.n = 0;
    resp.extra[0] = 0;
    resp.status = 0;
    n = http_parse(reqbuf, hl + blen, &r);
    if (n <= 0) return n;
    web_handle(&srv, &r, &resp);
    if (resp.body.p) resp.body.p[resp.body.n] = 0;
    return resp.status;
}

static int get(const char *target)
{
    char head[2048];
    sprintf(head, "GET %s HTTP/1.1\r\n" AUTH "\r\n", target);
    return call(head, NULL, 0);
}

static int post(const char *target, const char *body)
{
    char head[2048];
    sprintf(head, "POST %s HTTP/1.1\r\n" AUTH "Content-Length: %lu\r\n\r\n", target,
            (unsigned long)strlen(body));
    return call(head, body, strlen(body));
}

static int body_has(const char *s)
{
    return resp.body.p && strstr(resp.body.p, s) != NULL;
}

static void put_file(const char *name, const char *text)
{
    char path[SYS_PATH + 300];
    FILE *f;
    sprintf(path, "%s%c%.200s", srv.dir, sys_sep, name);
    f = fopen(path, "wb");
    if (!f) { printf("FAIL cannot write %s\n", path); failed++; return; }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static int file_is(const char *name, const char *text)
{
    char path[SYS_PATH + 300], buf[4096];
    size_t n;
    FILE *f;
    sprintf(path, "%s%c%.200s", srv.dir, sys_sep, name);
    f = fopen(path, "rb");
    if (!f) return 0;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return !strcmp(buf, text);
}

static void run_to_end(void)
{
    int i;
    for (i = 0; srv.active && i < 2000; i++) {
        unsigned char rdy;
        SysSock none = SYS_NOSOCK;
        sys_wait(&none, &rdy, 1, &none, &rdy, 0, srv.proc, 10);
        web_tick(&srv);
    }
    CHECK(!srv.active);
}

#define TICKET "fedcba9876543210fedcba9876543210"

static void test_access(void)
{
    static const char *const open_paths[] = { "/", "/app.js", "/app.css", "/kw.js", "/favicon.ico" };
    static const char *const api_get[] = {
        "/api/info", "/api/files", "/api/file?name=a.pld", "/api/outputs?name=a.pld",
        "/api/options", "/api/job", "/api/devices", "/api/guide", "/api/nosuch", "/api/session"
    };
    static const char *const api_post[] = {
        "/api/file?name=x.pld", "/api/options", "/api/prefs?tab=4", "/api/run?action=compile&name=a.pld",
        "/api/stop", "/api/devstmt?mn=g16v8", "/api/quit", "/api/nosuch"
    };
    char head[512];
    size_t i;

    /* the page and its files: no token, and no secret in them */
    for (i = 0; i < sizeof open_paths / sizeof open_paths[0]; i++) {
        sprintf(head, "GET %s HTTP/1.1\r\n" HOST "\r\n", open_paths[i]);
        CHECK(call(head, NULL, 0) == 200);
        CHECK(!body_has(TOKEN) && !body_has(TICKET) && !strstr(resp.extra, "Set-Cookie"));
        /* but not for a page reached under another name */
        sprintf(head, "GET %s HTTP/1.1\r\nHost: attacker.example:8477\r\n\r\n", open_paths[i]);
        CHECK(call(head, NULL, 0) == 403);
    }
    CHECK(call("GET / HTTP/1.1\r\n" HOST "\r\n", NULL, 0) == 200);
    CHECK(body_has("<title>FreeCUPL</title>") && body_has("<script src=\"/app.js\">") && !body_has("@TOKEN@"));
    CHECK(!strncmp(resp.ctype, "text/html", 9));
    CHECK(call("GET /app.js HTTP/1.1\r\n" HOST "\r\n", NULL, 0) == 200 && body_has("fcScan") &&
          !strncmp(resp.ctype, "text/javascript", 15));
    CHECK(call("GET /app.css HTTP/1.1\r\n" HOST "\r\n", NULL, 0) == 200 && !strncmp(resp.ctype, "text/css", 8));
    CHECK(call("GET /kw.js HTTP/1.1\r\n" HOST "\r\n", NULL, 0) == 200 && body_has("\"sequencejk\""));
    CHECK(call("GET /favicon.ico HTTP/1.1\r\n" HOST "\r\n", NULL, 0) == 200 && resp.body.n > 100);

    /* everything under /api/: the header, nothing else */
    for (i = 0; i < sizeof api_get / sizeof api_get[0]; i++) {
        const char *q = strchr(api_get[i], '?') ? "&" : "?";
        sprintf(head, "GET %s HTTP/1.1\r\n" HOST "\r\n", api_get[i]);
        CHECK(call(head, NULL, 0) == 403);
        CHECK(!body_has(TOKEN));
        sprintf(head, "GET %s%st=" TOKEN " HTTP/1.1\r\n" HOST "\r\n", api_get[i], q);
        CHECK(call(head, NULL, 0) == 403);
        sprintf(head, "GET %s HTTP/1.1\r\n" HOST "Cookie: fct8477=" TOKEN "; t=" TOKEN "\r\n\r\n", api_get[i]);
        CHECK(call(head, NULL, 0) == 403);
        sprintf(head, "GET %s HTTP/1.1\r\n" HOST "X-FreeCUPL-Token: 1123456789abcdef0123456789abcdef\r\n\r\n", api_get[i]);
        CHECK(call(head, NULL, 0) == 403);
        sprintf(head, "GET %s HTTP/1.1\r\n" HOST "X-FreeCUPL-Token:\r\n\r\n", api_get[i]);
        CHECK(call(head, NULL, 0) == 403);
    }
    for (i = 0; i < sizeof api_post / sizeof api_post[0]; i++) {
        const char *q = strchr(api_post[i], '?') ? "&" : "?";
        sprintf(head, "POST %s HTTP/1.1\r\n" HOST "Content-Length: 1\r\n\r\nx", api_post[i]);
        CHECK(call(head, NULL, 0) == 403);
        CHECK(!resp.quit && !srv.active);
        sprintf(head, "POST %s%st=" TOKEN " HTTP/1.1\r\n" HOST "Cookie: fct8477=" TOKEN "\r\nContent-Length: 1\r\n\r\nx",
                api_post[i], q);
        CHECK(call(head, NULL, 0) == 403);
        CHECK(!resp.quit && !srv.active);
    }
    CHECK(!file_is("x.pld", "x"));
    /* the right one */
    CHECK(call("GET /api/files HTTP/1.1\r\n" AUTH "\r\n", NULL, 0) == 200);
    CHECK(call("GET /api/files HTTP/1.1\r\nHost: localhost:8477\r\nX-FreeCUPL-Token: " TOKEN "\r\n\r\n", NULL, 0) == 200);
    CHECK(!strstr(resp.extra, "Set-Cookie"));
    /* Host and Origin, token given */
    CHECK(call("GET /api/files HTTP/1.1\r\nHost: attacker.example:8477\r\nX-FreeCUPL-Token: " TOKEN "\r\n\r\n", NULL, 0) == 403);
    CHECK(call("GET /api/files HTTP/1.1\r\nHost: 127.0.0.1:8478\r\nX-FreeCUPL-Token: " TOKEN "\r\n\r\n", NULL, 0) == 403);
    CHECK(call("GET /api/files HTTP/1.1\r\nHost: 127.0.0.1\r\nX-FreeCUPL-Token: " TOKEN "\r\n\r\n", NULL, 0) == 403);
    CHECK(call("GET /api/files HTTP/1.1\r\n" AUTH "Origin: http://attacker.example\r\n\r\n", NULL, 0) == 403);
    CHECK(call("GET /api/files HTTP/1.1\r\n" AUTH "Origin: null\r\n\r\n", NULL, 0) == 403);
    CHECK(call("GET /api/files HTTP/1.1\r\n" AUTH "Origin: http://127.0.0.1:8477\r\n\r\n", NULL, 0) == 200);
    CHECK(call("GET / HTTP/1.1\r\n" HOST "Origin: http://attacker.example\r\n\r\n", NULL, 0) == 403);
    CHECK(call("POST /api/quit HTTP/1.1\r\n" AUTH "Origin: http://attacker.example\r\nContent-Length: 0\r\n\r\n", NULL, 0) == 403);
    CHECK(!resp.quit);

    /* the ticket of the opened browser is traded for the token, once */
    CHECK(!srv.ticket[0]);
    CHECK(call("POST /api/session?k= HTTP/1.1\r\n" HOST "Content-Length: 0\r\n\r\n", NULL, 0) == 403);
    CHECK(call("POST /api/session HTTP/1.1\r\n" HOST "Content-Length: 0\r\n\r\n", NULL, 0) == 403);
    strcpy(srv.ticket, TICKET);
    CHECK(call("POST /api/session HTTP/1.1\r\n" HOST "Content-Length: 0\r\n\r\n", NULL, 0) == 403);
    CHECK(call("POST /api/session?k= HTTP/1.1\r\n" HOST "Content-Length: 0\r\n\r\n", NULL, 0) == 403);
    CHECK(call("POST /api/session?k=fedcba9876543210fedcba987654321f HTTP/1.1\r\n" HOST "Content-Length: 0\r\n\r\n", NULL, 0) == 403);
    CHECK(call("POST /api/session?k=" TOKEN " HTTP/1.1\r\n" HOST "Content-Length: 0\r\n\r\n", NULL, 0) == 403);
    CHECK(call("GET /api/session?k=" TICKET " HTTP/1.1\r\n" HOST "\r\n", NULL, 0) == 403);
    CHECK(call("POST /api/session?k=" TICKET " HTTP/1.1\r\nHost: attacker.example:8477\r\nContent-Length: 0\r\n\r\n", NULL, 0) == 403);
    CHECK(call("POST /api/session?k=" TICKET " HTTP/1.1\r\n" HOST "Origin: http://attacker.example\r\nContent-Length: 0\r\n\r\n", NULL, 0) == 403);
    /* the ticket is not the token */
    CHECK(call("GET /api/files HTTP/1.1\r\n" HOST "X-FreeCUPL-Token: " TICKET "\r\n\r\n", NULL, 0) == 403);
    CHECK(call("GET /api/files?k=" TICKET " HTTP/1.1\r\n" HOST "\r\n", NULL, 0) == 403);
    CHECK(!body_has(TOKEN) && srv.ticket[0]);
    CHECK(call("POST /api/session?k=" TICKET " HTTP/1.1\r\n" HOST "Origin: http://127.0.0.1:8477\r\nContent-Length: 0\r\n\r\n", NULL, 0) == 200);
    CHECK(body_has("{\"token\":\"" TOKEN "\"}") && !strstr(resp.extra, "Set-Cookie"));
    CHECK(!srv.ticket[0]);
    CHECK(call("POST /api/session?k=" TICKET " HTTP/1.1\r\n" HOST "Content-Length: 0\r\n\r\n", NULL, 0) == 403);
    CHECK(!body_has(TOKEN));

    CHECK(get("/api/guide") == 200 && body_has("USER GUIDE"));
    CHECK(get("/nosuch") == 404 && get("/api") == 404 && get("/api/") == 404);
    CHECK(get("/../etc/passwd") == 404 && get("/app.js/") == 404 && get("//") == 404);
    CHECK(post("/", "") == 404 && post("/api/files", "") == 404 && post("/api/guide", "") == 404);
    CHECK(get("/api/quit") == 404 && !resp.quit && get("/api/run?action=compile&name=a.pld") == 404);
    CHECK(get("/api/info") == 200 && body_has("\"version\":\"FreeCUPL ") && body_has("\"skeleton\":\"Name     XXXXX ;"));
}

static void test_files(void)
{
    static const char *const badq[] = {
        "..%2Fa.pld", "%2e%2e%2fa.pld", "..%5Ca.pld", "%2Fetc%2Fpasswd", "sub%2Fa.pld",
        ".hidden", "..", ".", "", "a%00.pld", "C:%5Ca.pld", "a.pld:s", "nul", "com1.pld",
        "a%0Ab.pld", "-j.pld", "a.pld.", "a%2Fb", "%2e"
    };
    char t[256];
    size_t i;

    put_file("a.pld", "Name a;\r\nDevice g16v8;\r\n");
    put_file("b.si", "x\ny\r\nz\n");
    put_file("out.jed", "jed\n");
    put_file(".hidden", "h\n");
    put_file("empty.pld", "");

    CHECK(get("/api/files") == 200);
    CHECK(body_has("{\"name\":\"a.pld\",\"size\":24,\"ro\":0}"));
    CHECK(body_has("{\"name\":\"out.jed\",\"size\":4,\"ro\":1}"));
    CHECK(!body_has("hidden"));
    /* sorted without regard to case */
    CHECK(strstr(resp.body.p, "a.pld") < strstr(resp.body.p, "b.si") &&
          strstr(resp.body.p, "b.si") < strstr(resp.body.p, "out.jed"));

    CHECK(get("/api/file?name=a.pld") == 200);
    CHECK(resp.body.n == 24 && !memcmp(resp.body.p, "Name a;\r\nDevice g16v8;\r\n", 24));
    CHECK(strstr(resp.extra, "X-FreeCUPL-Eol: crlf\r\n") && strstr(resp.extra, "X-FreeCUPL-Readonly: 0\r\n"));
    CHECK(!strcmp(resp.ctype, "application/octet-stream"));
    CHECK(get("/api/file?name=b.si") == 200 && strstr(resp.extra, "X-FreeCUPL-Eol: lf\r\n"));
    CHECK(get("/api/file?name=out.jed") == 200 && strstr(resp.extra, "X-FreeCUPL-Readonly: 1\r\n"));
    CHECK(get("/api/file?name=empty.pld") == 200 && resp.body.n == 0);
    CHECK(get("/api/file?name=a.pld&stat=1") == 200 && body_has("\"size\":24") && body_has("\"mtime\":\""));
    CHECK(get("/api/file?name=none.pld") == 404 && body_has("\"error\":\"No file none.pld\""));
    CHECK(get("/api/file") == 400 && get("/api/file?nam=a.pld") == 400);

    for (i = 0; i < sizeof badq / sizeof badq[0]; i++) {
        sprintf(t, "/api/file?name=%s", badq[i]);
        checks += 2;
        if (get(t) != 400) { failed++; printf("FAIL read not refused: %s\n", badq[i]); }
        if (post(t, "x") != 400) { failed++; printf("FAIL write not refused: %s\n", badq[i]); }
        sprintf(t, "/api/run?action=compile&name=%s", badq[i]);
        checks++;
        if (post(t, "") != 400) { failed++; printf("FAIL run not refused: %s\n", badq[i]); }
    }
    CHECK(!srv.active);

    CHECK(post("/api/file?name=new.pld", "one\r\ntwo\r\n") == 200 && body_has("\"size\":10"));
    CHECK(file_is("new.pld", "one\r\ntwo\r\n"));
    CHECK(post("/api/file?name=new.pld", "three\n") == 200 && file_is("new.pld", "three\n"));
    CHECK(post("/api/file?name=new.pld&new=1", "four\n") == 409 && file_is("new.pld", "three\n"));
    CHECK(post("/api/file?name=other.pld&new=1", "four\n") == 200 && file_is("other.pld", "four\n"));
    CHECK(post("/api/file?name=new.pld", "") == 200 && file_is("new.pld", ""));
    /* output files are read only */
    CHECK(post("/api/file?name=out.jed", "x") == 403 && file_is("out.jed", "jed\n"));
    CHECK(post("/api/file?name=x.DOC", "x") == 403 && post("/api/file?name=x.so", "x") == 403);
    CHECK(post("/api/file?name=x.abs", "x") == 403 && post("/api/file?name=x.mx", "x") == 403);
    CHECK(post("/api/file?name=x.lst", "x") == 403 && post("/api/file?name=x.wo", "x") == 403);
    CHECK(post("/api/file", "x") == 400);
}

static void test_options(void)
{
    CHECK(get("/api/options") == 200);
    CHECK(body_has("\"source\":\"\",\"perfile\":0,\"opts\":{\"jed\":1,\"doc\":1,\"fuseplot\":0,"
                   "\"lst\":1,\"abs\":0,\"mx\":0,\"namesrc\":0,\"secure\":0,\"kb\":0,\"min\":1,"
                   "\"simafter\":0,\"compat\":0,\"simlatch\":0,\"simlst\":1,\"simjed\":0,"
                   "\"device\":\"\"},\"tab\":8,\"colour\":1}"));
    CHECK(get("/api/options?name=b.si") == 200 && body_has("\"source\":\"b.pld\""));
    CHECK(get("/api/options?name=..%2Fx") == 400);
    CHECK(post("/api/options?name=a.pld&perfile=1",
               "jed=0&doc=0&lst=0&fuseplot=1&min=4&compat=1&simlatch=1&device=g22v10") == 200);
    CHECK(get("/api/options?name=a.pld") == 200);
    CHECK(body_has("\"perfile\":1,\"opts\":{\"jed\":0,\"doc\":0,\"fuseplot\":1,\"lst\":0") &&
          body_has("\"min\":4") && body_has("\"compat\":1,\"simlatch\":1") &&
          body_has("\"device\":\"g22v10\""));
    /* a.si has the options of a.pld, other sources the common ones */
    CHECK(get("/api/options?name=a.si") == 200 && body_has("\"perfile\":1"));
    CHECK(get("/api/options?name=new.pld") == 200 && body_has("\"perfile\":0,\"opts\":{\"jed\":1"));
    /* values that are not options are not taken */
    CHECK(post("/api/options", "min=9&jed=yes&device=g16v8") == 200);
    CHECK(get("/api/options") == 200 && body_has("\"jed\":0") && body_has("\"min\":1") &&
          body_has("\"device\":\"g16v8\""));
    CHECK(post("/api/options", "device=-compat-refdefect") == 400);
    CHECK(post("/api/options", "device=g16v8%20-s") == 400);
    CHECK(post("/api/options", "device=nosuchdevice") == 400);
    CHECK(get("/api/options") == 200 && body_has("\"device\":\"g16v8\""));
    CHECK(post("/api/options?name=a.pld&perfile=0", "jed=1&doc=1&lst=1&simlst=1&min=1") == 200);
    CHECK(get("/api/options?name=a.pld") == 200 && body_has("\"perfile\":0,\"opts\":{\"jed\":1,\"doc\":1") &&
          body_has("\"device\":\"\""));
    CHECK(post("/api/prefs?tab=4&colour=0", "") == 200);
    CHECK(get("/api/options") == 200 && body_has("\"tab\":4,\"colour\":0"));
    CHECK(post("/api/prefs?tab=5&colour=1", "") == 200);
    CHECK(get("/api/options") == 200 && body_has("\"tab\":4,\"colour\":1"));
    CHECK(post("/api/prefs?tab=8", "") == 200);
}

static void test_devices(void)
{
    char want[64];
    const char *p;
    int n = 0;
    CHECK(get("/api/devices") == 200);
    sprintf(want, "{\"total\":%d,\"devs\":[[\"", gui_ndevs);
    CHECK(!strncmp(resp.body.p, want, strlen(want)));
    for (p = resp.body.p; (p = strstr(p, "],[")) != NULL; p++) n++;
    CHECK(n + 1 == gui_ndevs && gui_ndevs > 300);
    CHECK(get("/api/devices?q=16v8+complex") == 200);
    CHECK(body_has("[\"g16v8ma\",20,\"GAL16V8 (complex mode)\"]") && !body_has("\"g22v10\""));
    CHECK(get("/api/devices?q=nosuchdevicename") == 200 && body_has("\"devs\":[]}"));
    {
        /* sorted by a column: s=0 mnemonic, 1 pins (as numbers), 2 part; d=1 downwards */
        int *idx = (int *)malloc(sizeof(int) * (size_t)gui_ndevs), nn, col, d;
        char first[160], last[160];
        for (col = 0; col < 3; col++) for (d = 0; d < 2; d++) {
            char url[64];
            nn = gui_dev_list("", col, d, idx);
            sprintf(url, "/api/devices?s=%d&d=%d", col, d);
            CHECK(get(url) == 200 && nn == gui_ndevs);
            sprintf(first, "\"devs\":[[\"%s\",%d,", gui_devs[idx[0]].mn, gui_devs[idx[0]].pins);
            sprintf(last, ",[\"%s\",%d,", gui_devs[idx[nn - 1]].mn, gui_devs[idx[nn - 1]].pins);
            CHECK(body_has(first));
            p = strstr(resp.body.p, last);
            CHECK(p && !strstr(p + 1, "],["));
        }
        nn = gui_dev_list("", 1, 0, idx);
        CHECK(gui_devs[idx[0]].pins == 20 && gui_devs[idx[nn - 1]].pins > gui_devs[idx[0]].pins);
        nn = gui_dev_list("16v8", 1, 1, idx);
        CHECK(get("/api/devices?q=16v8&s=1&d=1") == 200);
        sprintf(first, "\"devs\":[[\"%s\",", gui_devs[idx[0]].mn);
        CHECK(body_has(first) && !body_has("\"g22v10\""));
        /* anything else in s or d: the order of the table */
        CHECK(get("/api/devices?s=3&d=2") == 200);
        sprintf(first, "\"devs\":[[\"%s\",", gui_devs[0].mn);
        CHECK(body_has(first));
        CHECK(get("/api/devices?s=11") == 200 && body_has(first));
        CHECK(get("/api/devices?s=-1&d=1") == 200 && body_has(first));
        free(idx);
    }

    CHECK(post("/api/devstmt?mn=g16v8", "Name x;\nDevice   g22v10;\npin 1 = a;\n") == 200);
    CHECK(body_has("{\"found\":1,\"start\":8,\"end\":24,\"text\":\"Device   g16v8 ;\"}"));
    CHECK(post("/api/devstmt?mn=g16v8", "Name x;\n/* Device g22v10; */\n") == 200);
    CHECK(body_has("\"found\":0"));
    CHECK(post("/api/devstmt?mn=g16v8", "") == 200 && body_has("\"found\":0"));
    CHECK(post("/api/devstmt?mn=nosuch", "Device x;") == 400);
    CHECK(post("/api/devstmt", "Device x;") == 400);
}

static const char good_pld[] =
    "Name     unit;\nPartno   X;\nRevision 01;\nDate     01/01/26;\nDesigner X;\n"
    "Company  X;\nAssembly X;\nLocation X;\nDevice   g16v8;\n\n"
    "pin 1 = a;\npin 2 = b;\npin 19 = y;\n\ny = a & b;\n";
static const char good_si[] =
    "Name     unit;\nPartno   X;\nRevision 01;\nDate     01/01/26;\nDesigner X;\n"
    "Company  X;\nAssembly X;\nLocation X;\nDevice   g16v8;\n\n"
    "ORDER: a, b, %1, y;\n\nVECTORS:\n00 L\n01 L\n10 L\n11 H\n";

static void test_run(void)
{
    char bad[sizeof good_pld + 8], badsi[sizeof good_si];
    unsigned gen;

    CHECK(post("/api/run?action=compile&name=unit.pld", "") == 404 &&
          body_has("\"error\":\"No source file unit.pld\""));
    CHECK(post("/api/run?action=simulate&name=unit.pld", "") == 404 &&
          body_has("\"error\":\"No simulation input unit.si\""));
    CHECK(post("/api/run?action=erase&name=unit.pld", "") == 400);
    CHECK(post("/api/run?name=unit.pld", "") == 400);
    CHECK(post("/api/run?action=compile", "") == 400);
    CHECK(!srv.active && srv.gen == 0);
    CHECK(get("/api/job") == 200 && body_has("\"active\":0,\"total\":0") && body_has("\"msgs\":[]}"));

    put_file("unit.pld", good_pld);
    CHECK(post("/api/options", "jed=1&doc=1&lst=1&simlst=1&min=1") == 200);
    CHECK(post("/api/run?action=compile&name=unit.pld", "") == 200 && body_has("{\"gen\":1}"));
    /* one run at a time */
    if (srv.active) CHECK(post("/api/run?action=compile&name=unit.pld", "") == 409);
    run_to_end();
    CHECK(get("/api/job") == 200);
    CHECK(body_has("{\"k\":\"cmd\",\"line\":0,\"file\":\"\",\"text\":\"freecupl -jxl unit.pld\"}"));
    CHECK(body_has("{\"k\":\"file\",\"line\":0,\"file\":\"unit.jed\",\"text\":\"    unit.jed\"}"));
    CHECK(body_has("{\"k\":\"file\",\"line\":0,\"file\":\"unit.doc\",\"text\":\"    unit.doc\"}"));
    CHECK(body_has("{\"k\":\"file\",\"line\":0,\"file\":\"unit.lst\",\"text\":\"    unit.lst\"}"));
    CHECK(body_has("{\"k\":\"summary\",\"line\":0,\"file\":\"\",\"text\":\"Compile: OK - 0 errors, 0 warnings\"}"));
    CHECK(body_has("\"status\":\"Compile: OK - 0 errors, 0 warnings\""));
    CHECK(get("/api/file?name=unit.jed") == 200 && resp.body.n > 100);
    CHECK(get("/api/outputs?name=unit.pld") == 200 &&
          body_has("{\"pld\":\"unit.pld\",\"si\":\"unit.si\",\"jed\":\"unit.jed\",\"doc\":\"unit.doc\","
                   "\"lst\":\"unit.lst\",\"so\":\"\"}"));
    /* a part of the messages */
    CHECK(get("/api/job?from=1") == 200 && !body_has("\"k\":\"cmd\"") && body_has("\"k\":\"summary\""));
    CHECK(get("/api/job?from=999") == 200 && body_has("\"msgs\":[]}"));
    CHECK(get("/api/job?from=-5") == 200 && body_has("\"msgs\":[]}"));

    /* simulate: compile with -a, then freecsim; from the .si tab too */
    put_file("unit.si", good_si);
    CHECK(post("/api/run?action=simulate&name=unit.si", "") == 200 && body_has("{\"gen\":2}"));
    run_to_end();
    CHECK(get("/api/job") == 200);
    CHECK(body_has("\"text\":\"freecupl -jxla unit.pld\"") && body_has("\"text\":\"freecsim -l unit.si\""));
    CHECK(body_has("\"file\":\"unit.so\",\"text\":\"    unit.so\""));
    CHECK(body_has("\"text\":\"Simulate: OK - 0 errors, 0 warnings\""));
    CHECK(get("/api/outputs?name=unit.si") == 200 && body_has("\"so\":\"unit.so\""));

    /* a syntax error: the line number and the file it counts in */
    strcpy(bad, good_pld);
    strcpy(strstr(bad, "y = a & b;"), "y = a & & b;\n");
    put_file("unit.pld", bad);
    CHECK(post("/api/run?action=compile&name=unit.pld", "") == 200);
    run_to_end();
    CHECK(get("/api/job") == 200);
    CHECK(body_has("{\"k\":\"error\",\"line\":15,\"file\":\"unit.pld\",\"text\":\"["));
    CHECK(body_has("{\"k\":\"context\",\"line\":15,\"file\":\"unit.pld\""));
    CHECK(body_has("{\"k\":\"abort\",\"line\":0,\"file\":\"\",\"text\":\"Compile: FAILED - 1 error, 0 warnings\"}"));
    /* simulate does not go on after a failed compile */
    CHECK(post("/api/run?action=simulate&name=unit.pld", "") == 200);
    run_to_end();
    CHECK(get("/api/job") == 200 && !body_has("freecsim") && body_has("Compile: FAILED"));

    /* a failed vector: message from the listing, with its line there */
    put_file("unit.pld", good_pld);
    strcpy(badsi, good_si);
    memcpy(strstr(badsi, "11 H"), "11 L", 4);
    put_file("unit.si", badsi);
    CHECK(post("/api/run?action=simulate&name=unit.pld", "") == 200);
    run_to_end();
    CHECK(get("/api/job") == 200);
    CHECK(body_has("\"k\":\"error\",\"line\":") && body_has("\"file\":\"unit.so\",\"text\":\"unit.so("));
    CHECK(body_has("\"text\":\"Simulate: FAILED - 1 error, 0 warnings\""));

    /* simulate after compile, flags of the options */
    put_file("unit.si", good_si);
    CHECK(post("/api/options?name=unit.pld&perfile=1",
               "jed=1&simafter=1&compat=1&simlatch=1&min=2&kb=1&device=g16v8&simlst=1") == 200);
    /* an option not sent has its default: -x and -l */
    CHECK(post("/api/run?action=compile&name=unit.pld", "") == 200);
    run_to_end();
    CHECK(get("/api/job") == 200);
    CHECK(body_has("\"text\":\"freecupl -jxls -m2 -kb -compat-refdefect -simlatch g16v8 unit.pld\""));
    CHECK(body_has("\"text\":\"Compile: OK - 0 errors,"));
    CHECK(post("/api/options?name=unit.pld&perfile=0", "jed=1&doc=1&lst=1&simlst=1&min=1") == 200);

    /* stop */
    gen = srv.gen;
    CHECK(post("/api/stop", "") == 200 && !srv.active);
    CHECK(post("/api/run?action=compile&name=unit.pld", "") == 200 && srv.gen == gen + 1);
    CHECK(post("/api/stop", "") == 200);
    run_to_end();
    CHECK(get("/api/job") == 200 && body_has("\"active\":0"));

    /* a program that is not there */
    {
        char keep[SYS_PATH];
        strcpy(keep, srv.exedir);
        strcat(srv.exedir, "-nosuch");
        CHECK(post("/api/run?action=compile&name=unit.pld", "") == 200);
        run_to_end();
        CHECK(get("/api/job") == 200 && body_has("\"k\":\"abort\"") && body_has("Cannot run freecupl"));
        CHECK(body_has("\"text\":\"Compile: FAILED - 1 error, 0 warnings\""));
        strcpy(srv.exedir, keep);
    }

    CHECK(post("/api/quit", "") == 200 && resp.quit);
}

static void test_json(void)
{
    WBuf b = { NULL, 0, 0 };
    wb_json(&b, "a\"b\\c\n\001\177 \303\251");
    CHECK(b.p && !strcmp(b.p, "\"a\\\"b\\\\c\\u000a\\u0001\\u007f \303\251\""));
    free(b.p);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: unit WORKDIR BINDIR\n");
        return 2;
    }
    if (sys_init() || sys_full_dir(argv[1], srv.dir, sizeof srv.dir) ||
        sys_full_dir(argv[2], srv.exedir, sizeof srv.exedir)) {
        fprintf(stderr, "unit: bad directories\n");
        return 2;
    }
    if (strlen(srv.dir) + 24 >= sizeof srv.ini) return 2;
    strcpy(srv.ini, srv.dir);
    srv.ini[strlen(srv.ini) + 1] = 0;
    srv.ini[strlen(srv.ini)] = sys_sep;
    strcat(srv.ini, "serve-settings.txt");
    strcpy(srv.token, TOKEN);
    srv.port = PORT;

    test_parse();
    test_head();
    test_fields();
    test_names();
    test_json();
    test_access();
    test_files();
    test_options();
    test_devices();
    test_run();
    web_end(&srv);
    free(resp.body.p);
    printf("serve unit: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
