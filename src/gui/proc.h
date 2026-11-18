/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecuplw - running a console tool without a console window, its
   output read through a pipe.  DARKSOFT. */
#ifndef FREECUPLW_PROC_H
#define FREECUPLW_PROC_H
#include <windows.h>

typedef struct {
    HANDLE process;
    HANDLE out;            /* read end: stdout and stderr of the tool */
} Proc;

/* Start cmdline in directory dir (NULL: the current one).  Returns 0, or
   the Windows error code. */
DWORD proc_start(Proc *p, const char *cmdline, const char *dir);
/* Read the output until the tool closes it; fn gets complete lines
   without the line end. */
typedef void (*ProcLine)(void *ctx, const char *line);
void proc_read(Proc *p, ProcLine fn, void *ctx);
/* Wait for the end, close the handles, return the exit code. */
DWORD proc_finish(Proc *p);

#endif
