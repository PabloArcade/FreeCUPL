/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - CUPL preprocessor ($DEFINE, $INCLUDE, $IFDEF, $REPEAT, $MACRO).
   DARKSOFT. */
#ifndef FREECUPL_PP_H
#define FREECUPL_PP_H

typedef struct {
    char *text;     /* expanded text of one line (no newline) */
    int file;       /* index into PPOut.files */
    int line;       /* 1-based line in that file */
} PLine;

typedef struct PPOut {
    PLine *v;
    int n, cap;
    char **files;
    int nfiles;
} PPOut;

/* Runs the preprocessor on a file.  Messages go through diag().
   Returns 0 when no error was reported. */
int pp_run(const char *path, PPOut *out);
/* source lines of file 0 (for listings); NULL-terminated */
char **pp_source_lines(int *n);
/* longest line accepted by the parser (reference limit) */
#define PP_MAXLINE 256

#endif
