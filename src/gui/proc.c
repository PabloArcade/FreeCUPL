/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecuplw - running a console tool.  DARKSOFT. */
#include "proc.h"
#include <string.h>

DWORD proc_start(Proc *p, const char *cmdline, const char *dir)
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE rd = NULL, wr = NULL, nul, me = GetCurrentProcess();
    char *cmd;
    DWORD err = 0;
    size_t l = strlen(cmdline) + 1;

    p->process = p->out = NULL;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return GetLastError();
    /* the tool must not inherit the read end, or the pipe never closes */
    if (!DuplicateHandle(me, rd, me, &p->out, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        err = GetLastError();
        CloseHandle(rd);
        CloseHandle(wr);
        return err;
    }
    CloseHandle(rd);
    nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      &sa, OPEN_EXISTING, 0, NULL);
    if (nul == INVALID_HANDLE_VALUE) nul = NULL;

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    /* CreateProcess may write to the command line */
    cmd = HeapAlloc(GetProcessHeap(), 0, l);
    if (!cmd) {
        err = ERROR_NOT_ENOUGH_MEMORY;
    } else {
        memcpy(cmd, cmdline, l);
        if (CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL,
                           dir, &si, &pi)) {
            CloseHandle(pi.hThread);
            p->process = pi.hProcess;
        } else {
            err = GetLastError();
        }
        HeapFree(GetProcessHeap(), 0, cmd);
    }
    if (nul) CloseHandle(nul);
    CloseHandle(wr);
    if (err) {
        CloseHandle(p->out);
        p->out = NULL;
    }
    return err;
}

void proc_read(Proc *p, ProcLine fn, void *ctx)
{
    char buf[4096], line[2048];
    DWORD got, i;
    size_t n = 0;
    while (ReadFile(p->out, buf, sizeof buf, &got, NULL) && got) {
        for (i = 0; i < got; i++) {
            if (buf[i] == '\n') {
                if (n && line[n - 1] == '\r') n--;
                line[n] = 0;
                fn(ctx, line);
                n = 0;
            } else if (n + 1 < sizeof line) {
                line[n++] = buf[i];
            }
        }
    }
    if (n) {
        line[n] = 0;
        fn(ctx, line);
    }
}

DWORD proc_finish(Proc *p)
{
    DWORD rc = 1;
    if (p->process) {
        WaitForSingleObject(p->process, INFINITE);
        if (!GetExitCodeProcess(p->process, &rc)) rc = 1;
        CloseHandle(p->process);
    }
    if (p->out) CloseHandle(p->out);
    p->process = p->out = NULL;
    return rc;
}
