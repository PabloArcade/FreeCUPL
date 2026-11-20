/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl serve - POSIX (Linux, macOS).  DARKSOFT. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE 1
#include "sys.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char **environ;

const char sys_sep = '/';
const char *const sys_exe_suffix = "";
const char *const sys_eol = "\n";
volatile int sys_stop;

static void on_signal(int sig)
{
    (void)sig;
    sys_stop = 1;
}

int sys_init(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    /* a browser that went away must not end the program */
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
    return 0;
}

static int copy_path(char *out, size_t n, const char *s)
{
    size_t l = strlen(s);
    if (l >= n) return -1;
    memcpy(out, s, l + 1);
    return 0;
}

static int dir_part(char *path)
{
    char *s = strrchr(path, '/');
    if (!s) return -1;
    if (s == path) s++;
    *s = 0;
    return 0;
}

int sys_exe_dir(const char *argv0, char *out, size_t n)
{
    char buf[PATH_MAX], real[PATH_MAX];
    buf[0] = 0;
#ifdef __APPLE__
    {
        uint32_t sz = sizeof buf;
        if (_NSGetExecutablePath(buf, &sz)) buf[0] = 0;
    }
#else
    {
        ssize_t l = readlink("/proc/self/exe", buf, sizeof buf - 1);
        buf[l > 0 ? l : 0] = 0;
    }
#endif
    if (!buf[0] && strchr(argv0, '/')) copy_path(buf, sizeof buf, argv0);
    if (!buf[0]) {
        /* found through PATH */
        const char *p = getenv("PATH");
        while (p && *p) {
            const char *e = strchr(p, ':');
            size_t l = e ? (size_t)(e - p) : strlen(p);
            if (l && l + strlen(argv0) + 2 < sizeof buf) {
                memcpy(buf, p, l);
                buf[l] = '/';
                strcpy(buf + l + 1, argv0);
                if (!access(buf, X_OK)) break;
            }
            buf[0] = 0;
            p = e ? e + 1 : NULL;
        }
    }
    if (!buf[0] || !realpath(buf, real) || dir_part(real)) return -1;
    return copy_path(out, n, real);
}

int sys_full_dir(const char *dir, char *out, size_t n)
{
    char real[PATH_MAX];
    struct stat st;
    if (!realpath(dir, real) || stat(real, &st) || !S_ISDIR(st.st_mode)) return -1;
    return copy_path(out, n, real);
}

int sys_random(unsigned char *buf, size_t n)
{
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got;
    if (!f) return -1;
    got = fread(buf, 1, n, f);
    fclose(f);
    return got == n ? 0 : -1;
}

void sys_config_path(const char *exedir, char *out, size_t n)
{
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    int l = -1;
    (void)exedir;
    out[0] = 0;
    if (x && x[0] == '/') l = snprintf(out, n, "%s/freecupl/serve.ini", x);
    else if (h && h[0] == '/') l = snprintf(out, n, "%s/.config/freecupl/serve.ini", h);
    if (l < 0 || (size_t)l >= n) out[0] = 0;
}

void sys_make_parent(const char *path)
{
    char buf[SYS_PATH];
    char *p;
    if (copy_path(buf, sizeof buf, path)) return;
    /* the last two levels: .config and freecupl */
    p = strrchr(buf, '/');
    if (!p || p == buf) return;
    *p = 0;
    p = strrchr(buf, '/');
    if (p && p != buf) {
        *p = 0;
        mkdir(buf, 0700);
        *p = '/';
    }
    mkdir(buf, 0700);
}

static pid_t browser_pid;

void sys_open_browser(const char *url)
{
#ifdef __APPLE__
    const char *prog = "open";
#else
    const char *prog = "xdg-open";
#endif
    char *argv[3];
    posix_spawn_file_actions_t fa;
    pid_t pid;
    argv[0] = (char *)prog;
    argv[1] = (char *)url;
    argv[2] = NULL;
    if (posix_spawn_file_actions_init(&fa)) return;
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    /* collected in sys_wait */
    if (!posix_spawnp(&pid, prog, &fa, NULL, argv, environ)) browser_pid = pid;
    posix_spawn_file_actions_destroy(&fa);
}

/* ---- files ---- */

int sys_stat(const char *path, long *size, SysTime *mtime)
{
    struct stat st;
    if (lstat(path, &st)) return errno == ENOENT ? 0 : -1;
    if (!S_ISREG(st.st_mode)) return -1;
    if (size) *size = st.st_size > LONG_MAX ? LONG_MAX : (long)st.st_size;
    if (mtime) {
        mtime->sec = (long long)st.st_mtime;
#ifdef __APPLE__
        mtime->nsec = st.st_mtimespec.tv_nsec;
#else
        mtime->nsec = st.st_mtim.tv_nsec;
#endif
    }
    return 1;
}

void sys_now(SysTime *t)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    t->sec = (long long)tv.tv_sec;
    t->nsec = (long)tv.tv_usec * 1000;
}

int sys_list(const char *dir, SysDirFn fn, void *ctx)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return -1;
    while ((e = readdir(d)) != NULL) fn(ctx, e->d_name);
    closedir(d);
    return 0;
}

/* ---- sockets ---- */

static void nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

SysSock sys_listen(int *port)
{
    struct sockaddr_in a;
    socklen_t al = sizeof a;
    int one = 1, fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return SYS_NOSOCK;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)*port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 16) ||
        getsockname(fd, (struct sockaddr *)&a, &al) || fd >= FD_SETSIZE) {
        close(fd);
        return SYS_NOSOCK;
    }
    *port = ntohs(a.sin_port);
    nonblock(fd);
    return fd;
}

SysSock sys_accept(SysSock l)
{
    int fd = accept((int)l, NULL, NULL);
    if (fd < 0) return SYS_NOSOCK;
    if (fd >= FD_SETSIZE) {
        close(fd);
        return SYS_NOSOCK;
    }
    nonblock(fd);
#ifdef SO_NOSIGPIPE
    {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    }
#endif
    return fd;
}

int sys_recv(SysSock s, char *buf, int n)
{
    ssize_t r = recv((int)s, buf, (size_t)n, 0);
    if (r > 0) return (int)r;
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    return -1;
}

int sys_send(SysSock s, const char *buf, int n)
{
    ssize_t r = send((int)s, buf, (size_t)n, 0);
    if (r >= 0) return (int)r;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

void sys_close(SysSock s)
{
    close((int)s);
}

/* ---- child process ---- */

struct SysProc {
    pid_t pid;
    int fd;
};

SysProc *sys_spawn(const char *const argv[], const char *dir, char *err, size_t errn)
{
    SysProc *p = malloc(sizeof *p);
    int fds[2], st[2], e = 0;
    pid_t pid;
    if (!p || pipe(fds)) {
        snprintf(err, errn, "%s", strerror(errno));
        free(p);
        return NULL;
    }
    /* posix_spawn cannot change the directory everywhere, so fork; the
       second pipe closes on exec and carries errno when exec fails */
    if (pipe(st)) {
        snprintf(err, errn, "%s", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        free(p);
        return NULL;
    }
    fcntl(st[1], F_SETFD, FD_CLOEXEC);
    pid = fork();
    if (pid == 0) {
        int nul = open("/dev/null", O_RDONLY);
        signal(SIGPIPE, SIG_DFL);
        if (nul >= 0) dup2(nul, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        close(st[0]);
        if (!dir || !chdir(dir)) execv(argv[0], (char *const *)argv);
        e = errno;
        if (write(st[1], &e, sizeof e) < 0) _exit(127);
        _exit(127);
    }
    close(fds[1]);
    close(st[1]);
    if (pid < 0) {
        e = errno;
    } else {
        ssize_t r;
        do r = read(st[0], &e, sizeof e); while (r < 0 && errno == EINTR);
        if (r != (ssize_t)sizeof e) e = 0;
        if (e) waitpid(pid, NULL, 0);
    }
    close(st[0]);
    if (e) {
        snprintf(err, errn, "%s", strerror(e));
        close(fds[0]);
        free(p);
        return NULL;
    }
    nonblock(fds[0]);
    p->pid = pid;
    p->fd = fds[0];
    return p;
}

int sys_proc_read(SysProc *p, char *buf, int n)
{
    ssize_t r = read(p->fd, buf, (size_t)n);
    if (r > 0) return (int)r;
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    return -1;
}

void sys_proc_kill(SysProc *p)
{
    kill(p->pid, SIGKILL);
}

int sys_proc_finish(SysProc *p)
{
    int st = 0, rc = 1;
    pid_t r;
    do r = waitpid(p->pid, &st, 0); while (r < 0 && errno == EINTR);
    if (r == p->pid) rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    close(p->fd);
    free(p);
    return rc;
}

void sys_wait(const SysSock *rd, unsigned char *rdy, int nrd,
              const SysSock *wr, unsigned char *wdy, int nwr,
              const SysProc *p, int ms)
{
    fd_set r, w;
    struct timeval tv;
    int i, max = -1, n;
    if (browser_pid > 0 && waitpid(browser_pid, NULL, WNOHANG) != 0) browser_pid = 0;
    FD_ZERO(&r);
    FD_ZERO(&w);
    for (i = 0; i < nrd; i++) {
        rdy[i] = 0;
        if (rd[i] == SYS_NOSOCK) continue;
        FD_SET((int)rd[i], &r);
        if (rd[i] > max) max = (int)rd[i];
    }
    for (i = 0; i < nwr; i++) {
        wdy[i] = 0;
        if (wr[i] == SYS_NOSOCK) continue;
        FD_SET((int)wr[i], &w);
        if (wr[i] > max) max = (int)wr[i];
    }
    if (p && p->fd < FD_SETSIZE) {
        FD_SET(p->fd, &r);
        if (p->fd > max) max = p->fd;
    }
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    n = select(max + 1, &r, &w, NULL, &tv);
    if (n <= 0) return;
    for (i = 0; i < nrd; i++)
        if (rd[i] != SYS_NOSOCK && FD_ISSET((int)rd[i], &r)) rdy[i] = 1;
    for (i = 0; i < nwr; i++)
        if (wr[i] != SYS_NOSOCK && FD_ISSET((int)wr[i], &w)) wdy[i] = 1;
}

#else
typedef int freecupl_sys_posix_unused;
#endif
