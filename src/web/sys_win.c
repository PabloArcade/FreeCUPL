/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl serve - Windows.  ws2_32, advapi32 and shell32 are looked up
   at run time, so that the program imports kernel32 and msvcrt only.
   DARKSOFT. */
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include "sys.h"
#include "gui/core.h"
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char sys_sep = '\\';
const char *const sys_exe_suffix = ".exe";
const char *const sys_eol = "\r\n";
volatile int sys_stop;

static int (WSAAPI *w_startup)(WORD, WSADATA *);
static SOCKET (WSAAPI *w_socket)(int, int, int);
static int (WSAAPI *w_bind)(SOCKET, const struct sockaddr *, int);
static int (WSAAPI *w_listen)(SOCKET, int);
static SOCKET (WSAAPI *w_accept)(SOCKET, struct sockaddr *, int *);
static int (WSAAPI *w_getsockname)(SOCKET, struct sockaddr *, int *);
static int (WSAAPI *w_setsockopt)(SOCKET, int, int, const char *, int);
static int (WSAAPI *w_ioctlsocket)(SOCKET, long, u_long *);
static int (WSAAPI *w_recv)(SOCKET, char *, int, int);
static int (WSAAPI *w_send)(SOCKET, const char *, int, int);
static int (WSAAPI *w_closesocket)(SOCKET);
static int (WSAAPI *w_select)(int, fd_set *, fd_set *, fd_set *, const struct timeval *);
static int (WSAAPI *w_lasterror)(void);

static void on_signal(int sig)
{
    (void)sig;
    sys_stop = 1;
}

int sys_init(void)
{
    static const struct {
        const char *name;
        FARPROC *to;
    } fn[] = {
        { "WSAStartup", (FARPROC *)&w_startup }, { "socket", (FARPROC *)&w_socket },
        { "bind", (FARPROC *)&w_bind }, { "listen", (FARPROC *)&w_listen },
        { "accept", (FARPROC *)&w_accept }, { "getsockname", (FARPROC *)&w_getsockname },
        { "setsockopt", (FARPROC *)&w_setsockopt }, { "ioctlsocket", (FARPROC *)&w_ioctlsocket },
        { "recv", (FARPROC *)&w_recv }, { "send", (FARPROC *)&w_send },
        { "closesocket", (FARPROC *)&w_closesocket }, { "select", (FARPROC *)&w_select },
        { "WSAGetLastError", (FARPROC *)&w_lasterror }
    };
    HMODULE ws = LoadLibraryA("ws2_32.dll");
    WSADATA wd;
    size_t i;
    if (!ws) return -1;
    for (i = 0; i < sizeof fn / sizeof fn[0]; i++) {
        *fn[i].to = GetProcAddress(ws, fn[i].name);
        if (!*fn[i].to) return -1;
    }
    if (w_startup(MAKEWORD(2, 2), &wd)) return -1;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifdef SIGBREAK
    signal(SIGBREAK, on_signal);
#endif
    return 0;
}

int sys_exe_dir(const char *argv0, char *out, size_t n)
{
    char buf[MAX_PATH];
    DWORD l = GetModuleFileNameA(NULL, buf, sizeof buf);
    (void)argv0;
    if (!l || l >= sizeof buf) return -1;
    gui_dirname(buf, out, n);
    return out[0] ? 0 : -1;
}

int sys_full_dir(const char *dir, char *out, size_t n)
{
    char buf[MAX_PATH];
    DWORD l = GetFullPathNameA(dir, sizeof buf, buf, NULL), a;
    if (!l || l >= sizeof buf || l >= n) return -1;
    a = GetFileAttributesA(buf);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return -1;
    strcpy(out, buf);
    return 0;
}

int sys_random(unsigned char *buf, size_t n)
{
    /* RtlGenRandom */
    HMODULE adv = LoadLibraryA("advapi32.dll");
    BOOLEAN (WINAPI *gen)(void *, ULONG);
    if (!adv) return -1;
    gen = (BOOLEAN (WINAPI *)(void *, ULONG))(void (*)(void))GetProcAddress(adv, "SystemFunction036");
    return gen && gen(buf, (ULONG)n) ? 0 : -1;
}

void sys_config_path(const char *exedir, char *out, size_t n)
{
    if (strlen(exedir) + 24 >= n) {
        out[0] = 0;
        return;
    }
    sprintf(out, "%s\\freecupl-serve.ini", exedir);
}

void sys_make_parent(const char *path)
{
    (void)path;
}

void sys_open_browser(const char *url)
{
    HMODULE sh = LoadLibraryA("shell32.dll");
    HINSTANCE (WINAPI *exec)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT);
    if (!sh) return;
    exec = (HINSTANCE (WINAPI *)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT))(void (*)(void))
           GetProcAddress(sh, "ShellExecuteA");
    if (exec) exec(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
}

/* ---- files ---- */

static void from_filetime(const FILETIME *ft, SysTime *t)
{
    ULARGE_INTEGER u;
    u.LowPart = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    /* 100 ns since 1601 */
    t->sec = (long long)(u.QuadPart / 10000000u) - 11644473600LL;
    t->nsec = (long)(u.QuadPart % 10000000u) * 100;
}

int sys_stat(const char *path, long *size, SysTime *mtime)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) {
        DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? 0 : -1;
    }
    if (d.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
                              FILE_ATTRIBUTE_DEVICE))
        return -1;
    if (size) *size = d.nFileSizeHigh || d.nFileSizeLow > (DWORD)LONG_MAX ? LONG_MAX
                                                                          : (long)d.nFileSizeLow;
    if (mtime) from_filetime(&d.ftLastWriteTime, mtime);
    return 1;
}

void sys_now(SysTime *t)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    from_filetime(&ft, t);
}

int sys_list(const char *dir, SysDirFn fn, void *ctx)
{
    char pat[SYS_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    if (strlen(dir) + 3 >= sizeof pat) return -1;
    sprintf(pat, "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        const unsigned char *p = (const unsigned char *)fd.cFileName;
        /* the page names files in UTF-8, the system in its code page:
           only names that are the same in both */
        while (*p && *p < 128) p++;
        if (!*p) fn(ctx, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

/* ---- sockets ---- */

SysSock sys_listen(int *port)
{
    struct sockaddr_in a;
    int al = sizeof a;
    u_long one = 1;
    BOOL excl = TRUE;
    SOCKET s = w_socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return SYS_NOSOCK;
    /* nobody else on this port */
    w_setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof excl);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = (u_short)(((*port & 255) << 8) | ((*port >> 8) & 255));
    /* 127.0.0.1 in network order */
    a.sin_addr.s_addr = 0x0100007fUL;
    if (w_bind(s, (struct sockaddr *)&a, sizeof a) || w_listen(s, 16) ||
        w_getsockname(s, (struct sockaddr *)&a, &al)) {
        w_closesocket(s);
        return SYS_NOSOCK;
    }
    *port = ((a.sin_port & 255) << 8) | ((a.sin_port >> 8) & 255);
    w_ioctlsocket(s, FIONBIO, &one);
    /* not for the tools that are started */
    SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
    return (SysSock)s;
}

SysSock sys_accept(SysSock l)
{
    u_long one = 1;
    SOCKET s = w_accept((SOCKET)l, NULL, NULL);
    if (s == INVALID_SOCKET) return SYS_NOSOCK;
    w_ioctlsocket(s, FIONBIO, &one);
    SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
    return (SysSock)s;
}

int sys_recv(SysSock s, char *buf, int n)
{
    int r = w_recv((SOCKET)s, buf, n, 0);
    if (r > 0) return r;
    if (r < 0 && w_lasterror() == WSAEWOULDBLOCK) return 0;
    return -1;
}

int sys_send(SysSock s, const char *buf, int n)
{
    int r = w_send((SOCKET)s, buf, n, 0);
    if (r >= 0) return r;
    return w_lasterror() == WSAEWOULDBLOCK ? 0 : -1;
}

void sys_close(SysSock s)
{
    w_closesocket((SOCKET)s);
}

/* ---- child process ---- */

struct SysProc {
    HANDLE process, out;
};

static void errtext(DWORD err, char *out, size_t n)
{
    DWORD l = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, err, 0, out, (DWORD)n, NULL);
    while (l && (out[l - 1] == '\r' || out[l - 1] == '\n' || out[l - 1] == ' ')) out[--l] = 0;
    if (!l) _snprintf(out, n, "error %lu", (unsigned long)err);
}

SysProc *sys_spawn(const char *const argv[], const char *dir, char *err, size_t errn)
{
    static char cmd[4096];
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE rd = NULL, wr = NULL, out = NULL, nul, me = GetCurrentProcess();
    SysProc *p;
    size_t l = 0;
    DWORD e = 0;
    int i;

    for (i = 0; argv[i]; i++) {
        if (l + 2 >= sizeof cmd || gui_quote_arg(argv[i], cmd + l + (i > 0), sizeof cmd - l - 1)) {
            _snprintf(err, errn, "command line too long");
            return NULL;
        }
        if (i > 0) cmd[l] = ' ';
        l += strlen(cmd + l);
    }
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        errtext(GetLastError(), err, errn);
        return NULL;
    }
    /* the tool must not inherit the read end, or the pipe never closes */
    if (!DuplicateHandle(me, rd, me, &out, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        errtext(GetLastError(), err, errn);
        CloseHandle(rd);
        CloseHandle(wr);
        return NULL;
    }
    CloseHandle(rd);
    nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                      OPEN_EXISTING, 0, NULL);
    if (nul == INVALID_HANDLE_VALUE) nul = NULL;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, dir, &si, &pi)) e = GetLastError();
    if (nul) CloseHandle(nul);
    CloseHandle(wr);
    p = e ? NULL : malloc(sizeof *p);
    if (!p) {
        if (e) {
            errtext(e, err, errn);
        } else {
            _snprintf(err, errn, "out of memory");
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        CloseHandle(out);
        return NULL;
    }
    CloseHandle(pi.hThread);
    p->process = pi.hProcess;
    p->out = out;
    return p;
}

int sys_proc_read(SysProc *p, char *buf, int n)
{
    DWORD avail = 0, got = 0;
    if (!PeekNamedPipe(p->out, NULL, 0, NULL, &avail, NULL)) return -1;
    if (!avail) return 0;
    if (avail > (DWORD)n) avail = (DWORD)n;
    if (!ReadFile(p->out, buf, avail, &got, NULL) || !got) return -1;
    return (int)got;
}

void sys_proc_kill(SysProc *p)
{
    TerminateProcess(p->process, 1);
}

int sys_proc_finish(SysProc *p)
{
    DWORD rc = 1;
    WaitForSingleObject(p->process, INFINITE);
    if (!GetExitCodeProcess(p->process, &rc)) rc = 1;
    CloseHandle(p->process);
    CloseHandle(p->out);
    free(p);
    return (int)rc;
}

static int in_set(const fd_set *s, SOCKET x)
{
    u_int i;
    for (i = 0; i < s->fd_count; i++)
        if (s->fd_array[i] == x) return 1;
    return 0;
}

void sys_wait(const SysSock *rd, unsigned char *rdy, int nrd,
              const SysSock *wr, unsigned char *wdy, int nwr,
              const SysProc *p, int ms)
{
    fd_set r, w;
    struct timeval tv;
    int i;
    r.fd_count = w.fd_count = 0;
    for (i = 0; i < nrd; i++) {
        rdy[i] = 0;
        if (rd[i] != SYS_NOSOCK && r.fd_count < FD_SETSIZE) r.fd_array[r.fd_count++] = (SOCKET)rd[i];
    }
    for (i = 0; i < nwr; i++) {
        wdy[i] = 0;
        if (wr[i] != SYS_NOSOCK && w.fd_count < FD_SETSIZE) w.fd_array[w.fd_count++] = (SOCKET)wr[i];
    }
    /* a pipe cannot be waited for with the sockets: look often */
    if (p && ms > 20) ms = 20;
    if (!r.fd_count && !w.fd_count) {
        Sleep((DWORD)ms);
        return;
    }
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    if (w_select(0, &r, &w, NULL, &tv) <= 0) return;
    for (i = 0; i < nrd; i++)
        if (rd[i] != SYS_NOSOCK && in_set(&r, (SOCKET)rd[i])) rdy[i] = 1;
    for (i = 0; i < nwr; i++)
        if (wr[i] != SYS_NOSOCK && in_set(&w, (SOCKET)wr[i])) wdy[i] = 1;
}

#else
typedef int freecupl_sys_win_unused;
#endif
