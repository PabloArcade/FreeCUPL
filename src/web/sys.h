/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl serve - what differs between POSIX and Windows: sockets on
   the loopback address, files, child processes.  DARKSOFT. */
#ifndef FREECUPL_WEBSYS_H
#define FREECUPL_WEBSYS_H
#include <stddef.h>

#define SYS_PATH 1024

typedef struct {
    long long sec;          /* since 1970 */
    long nsec;
} SysTime;

/* ---- start-up ---- */
int sys_init(void);                              /* 0, or -1 */
/* directory of the running program */
int sys_exe_dir(const char *argv0, char *out, size_t n);
/* absolute name of an existing directory */
int sys_full_dir(const char *dir, char *out, size_t n);
int sys_random(unsigned char *buf, size_t n);    /* 0, or -1 */
/* settings file; "" when there is no place for one */
void sys_config_path(const char *exedir, char *out, size_t n);
/* creates the directory of path */
void sys_make_parent(const char *path);
void sys_open_browser(const char *url);
extern const char sys_sep;                       /* '/' or '\\' */
extern const char *const sys_exe_suffix;         /* "" or ".exe" */
extern const char *const sys_eol;                /* of a new file */
/* set by Ctrl+C and by a request to end */
extern volatile int sys_stop;

/* ---- files ---- */
/* 1: a regular file (size and time filled in), 0: nothing of that
   name, -1: anything else, a link included */
int sys_stat(const char *path, long *size, SysTime *mtime);
void sys_now(SysTime *t);
typedef void (*SysDirFn)(void *ctx, const char *name);
int sys_list(const char *dir, SysDirFn fn, void *ctx);

/* ---- sockets, 127.0.0.1 only ---- */
typedef long SysSock;
#define SYS_NOSOCK (-1L)
/* *port 0 takes a free one and returns it */
SysSock sys_listen(int *port);
SysSock sys_accept(SysSock l);
/* > 0 bytes; 0 nothing now; -1 closed or failed */
int sys_recv(SysSock s, char *buf, int n);
/* >= 0 bytes taken; -1 failed */
int sys_send(SysSock s, const char *buf, int n);
void sys_close(SysSock s);

/* ---- child process: output and errors through one pipe ---- */
typedef struct SysProc SysProc;
/* argv ends with NULL; NULL and a text in err when it cannot run */
SysProc *sys_spawn(const char *const argv[], const char *dir, char *err, size_t errn);
/* > 0 bytes; 0 nothing now; -1 the pipe is closed */
int sys_proc_read(SysProc *p, char *buf, int n);
void sys_proc_kill(SysProc *p);
/* waits, frees p, returns the exit status */
int sys_proc_finish(SysProc *p);

/* Waits up to ms for a socket (rd[i] readable, wr[i] writable) or for
   output of p (may be NULL); rdy and wdy get 1 where ready.  Entries
   SYS_NOSOCK are skipped. */
void sys_wait(const SysSock *rd, unsigned char *rdy, int nrd,
              const SysSock *wr, unsigned char *wdy, int nwr,
              const SysProc *p, int ms);

#endif
