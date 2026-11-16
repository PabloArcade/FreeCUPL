/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - diagnostics (error / warning messages in the reference style).
   DARKSOFT. */
#ifndef FREECUPL_DIAG_H
#define FREECUPL_DIAG_H
#include "util.h"

enum { DG_NOTE = 0, DG_WARN, DG_ERROR, DG_FATAL };

typedef struct {
    int nerr, nwarn, nfatal;
    Buf list;          /* messages collected for the .lst file */
    int quiet;
} Diag;

extern Diag g_diag;

/* code like "0011cb"; line <= 0 means no line reference.  The message
   text is printed after the code, "[0011cb] unknown extension:  y.d". */
void diag(int level, const char *code, int line, const char *fmt, ...);
/* a message about a WinCUPL bug (bugs.h): "WinCUPL bug #Bnnn: TEXT; use
   -fix for the corrected result", with -fix "deviates from WinCUPL (bug
   #Bnnn fixed): TEXT" */
void diag_bug(int level, const char *code, int line, int bug, const char *fmt, ...);
/* show a source line with a caret before a syntax error */
void diag_context(int lineno, const char *text, int col);
void diag_reset(void);

#endif
