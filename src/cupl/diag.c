/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - diagnostics.  DARKSOFT. */
#include "diag.h"
#include "bugs.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

Diag g_diag;

void diag_reset(void)
{
    buf_free(&g_diag.list);
    memset(&g_diag, 0, sizeof g_diag);
}

void diag(int level, const char *code, int line, const char *fmt, ...)
{
    char msg[1024];
    Buf b = {0, 0, 0};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    buf_printf(&b, "[%s] ", code);
    if (line > 0) buf_printf(&b, "line(%d) ", line);
    if (level == DG_NOTE) buf_puts(&b, "Please note: ");
    else if (level == DG_WARN) buf_puts(&b, "Warning: ");
    buf_puts(&b, msg);
    if (level >= DG_ERROR) g_diag.nerr++;
    if (level == DG_FATAL) g_diag.nfatal++;
    if (level == DG_WARN || level == DG_NOTE) g_diag.nwarn++;
    if (!g_diag.quiet) printf("%s\n", b.p);
    buf_puts(&g_diag.list, b.p);
    buf_putc(&g_diag.list, '\n');
    buf_free(&b);
}

void diag_bug(int level, const char *code, int line, int bug, const char *fmt, ...)
{
    char msg[900], out[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    bug_text(out, sizeof out, bug, msg);
    diag(level, code, line, "%s", out);
}

void diag_context(int lineno, const char *text, int col)
{
    Buf b = {0, 0, 0};
    int i, w;
    w = (int)strlen(text);
    buf_printf(&b, "%3d:", lineno);
    buf_puts(&b, text);
    buf_putc(&b, '\n');
    for (i = 0; i < 4 + col && i < 4 + w + 1; i++) buf_putc(&b, ' ');
    buf_puts(&b, "^\n");
    if (!g_diag.quiet) fputs(b.p, stdout);
    buf_puts(&g_diag.list, b.p);
    buf_free(&b);
}
