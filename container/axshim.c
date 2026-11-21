/*
 * axshim - LD_PRELOAD compatibility shim for Applixware 6.0 (i386, 2006)
 * on a modern host. Two independent jobs:
 *
 * 1. 64-bit inode numbers (always on)
 *    32-bit stat()/readdir() fail with EOVERFLOW when st_ino/d_ino does not
 *    fit in 32 bits (XFS, large ext4, overlayfs...). The wrappers call the
 *    *64 variants and fold the inode number into 32 bits (hi ^ lo). Files
 *    larger than 2 GiB still give EOVERFLOW.
 *
 * 2. UTF-8 file names (only when AX_FNCONV is set in the environment)
 *    Applixware lives in ISO-8859-1; host file names are UTF-8. File names
 *    are translated at the libc boundary, one path component at a time:
 *
 *    out (readdir, getcwd, realpath, readlink): a component that is valid
 *        UTF-8 and fits entirely in Latin-1 is converted to Latin-1; any
 *        other component (e.g. with "€" or CJK) is passed through raw.
 *    in  (open, fopen, stat, rename, mkdir, chdir, ...): a component that is
 *        valid UTF-8 containing a character above U+00FF can only be a raw
 *        pass-through from the "out" rule and is kept; anything else is
 *        Latin-1 and is converted to UTF-8.
 *
 *    The "out" rule never hands Applixware UTF-8 that fits in Latin-1, so the
 *    two rules round-trip. Programs Applixware runs from outside its own
 *    tree (gs, lpr, /bin/sh via system/popen) get UTF-8 command lines and run
 *    without AX_FNCONV; its own helpers (…/axdata/…) keep it.
 *
 * Part of applixware-wrapper.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PBUF (PATH_MAX * 2 + 16)    /* Latin-1 -> UTF-8 at most doubles */
#define CBUF 16384                  /* shell command lines */
#define MAXARGS 512

/* ------------------------------------------------------------------ */
/* real libc entry points                                              */

#define REAL(name) \
    static __typeof__(name) *real_##name; \
    if (!real_##name) real_##name = (__typeof__(name) *)dlsym(RTLD_NEXT, #name)

static int fnconv = -1;

static int conv_on(void)
{
    if (fnconv < 0) {
        const char *v = getenv("AX_FNCONV");
        fnconv = (v != NULL && *v != '\0' && strcmp(v, "0") != 0);
    }
    return fnconv;
}

/* ------------------------------------------------------------------ */
/* UTF-8 <-> Latin-1, one component at a time                          */

/* Decode one UTF-8 sequence; returns its length (cp in *cp), or 0 if invalid. */
static int u8dec(const unsigned char *s, size_t n, unsigned *cp)
{
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    if ((s[0] & 0xe0) == 0xc0 && n >= 2 && (s[1] & 0xc0) == 0x80) {
        *cp = ((s[0] & 0x1f) << 6) | (s[1] & 0x3f);
        return *cp >= 0x80 ? 2 : 0;
    }
    if ((s[0] & 0xf0) == 0xe0 && n >= 3 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) {
        *cp = ((s[0] & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f);
        return *cp >= 0x800 ? 3 : 0;
    }
    if ((s[0] & 0xf8) == 0xf0 && n >= 4 && (s[1] & 0xc0) == 0x80 &&
        (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80) {
        *cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3f) << 12) | ((s[2] & 0x3f) << 6) | (s[3] & 0x3f);
        return (*cp >= 0x10000 && *cp <= 0x10ffff) ? 4 : 0;
    }
    return 0;
}

/* Classify a component: 0 = ASCII only, 1 = valid UTF-8 within Latin-1,
 * 2 = valid UTF-8 with a character above U+00FF, 3 = not valid UTF-8. */
static int classify(const unsigned char *s, size_t n)
{
    int r = 0;
    size_t i = 0;
    while (i < n) {
        unsigned cp;
        int l = u8dec(s + i, n - i, &cp);
        if (l == 0)
            return 3;
        if (cp > 0xff)
            r = 2;
        else if (cp >= 0x80 && r < 1)
            r = 1;
        i += l;
    }
    return r;
}

typedef size_t (*compfn)(const unsigned char *, size_t, char *, size_t);

/* UTF-8 -> Latin-1 (out rule) */
static size_t comp_out(const unsigned char *s, size_t n, char *o, size_t room)
{
    size_t i = 0, k = 0;
    if (classify(s, n) != 1) {
        if (n > room) n = room;
        memcpy(o, s, n);
        return n;
    }
    while (i < n && k < room) {
        unsigned cp;
        i += u8dec(s + i, n - i, &cp);
        o[k++] = (char)cp;
    }
    return k;
}

/* Latin-1 -> UTF-8 (in rule) */
static size_t comp_in(const unsigned char *s, size_t n, char *o, size_t room)
{
    size_t i, k = 0;
    int c = classify(s, n);
    if (c == 0 || c == 2) {
        if (n > room) n = room;
        memcpy(o, s, n);
        return n;
    }
    for (i = 0; i < n && k + 2 <= room; i++) {
        if (s[i] < 0x80) {
            o[k++] = (char)s[i];
        } else {
            o[k++] = (char)(0xc0 | (s[i] >> 6));
            o[k++] = (char)(0x80 | (s[i] & 0x3f));
        }
    }
    return k;
}

/* Apply fn to each component of src (split at any char in seps) into dst. */
static char *xlate(const char *src, char *dst, size_t size, const char *seps, compfn fn)
{
    const unsigned char *p = (const unsigned char *)src;
    size_t k = 0;
    if (src == NULL)
        return NULL;
    while (*p && k + 1 < size) {
        size_t n = 0;
        if (strchr(seps, *p)) {
            dst[k++] = (char)*p++;
            continue;
        }
        while (p[n] && !strchr(seps, p[n]))
            n++;
        k += fn(p, n, dst + k, size - 1 - k);
        p += n;
    }
    dst[k] = '\0';
    return dst;
}

static int has_high(const char *s)
{
    for (; s && *s; s++)
        if ((unsigned char)*s >= 0x80)
            return 1;
    return 0;
}

/* Path as given by Applixware -> path for the kernel. */
static const char *pin(const char *path, char *buf)
{
    if (!conv_on() || !has_high(path))
        return path;
    return xlate(path, buf, PBUF, "/", comp_in);
}

/* Path from the kernel -> Applixware, converted in place (never grows). */
static void pout_inplace(char *path)
{
    char tmp[PBUF];
    if (!conv_on() || !has_high(path))
        return;
    xlate(path, tmp, sizeof(tmp), "/", comp_out);
    strcpy(path, tmp);
}

/* ------------------------------------------------------------------ */
/* 1. inode folding (+ path conversion)                                */

static ino_t fold_ino(ino64_t i)
{
    return (ino_t)((i & 0xffffffffULL) ^ (i >> 32));
}

static int conv_stat(const struct stat64 *s, struct stat *d)
{
    if (s->st_size > 0x7fffffffLL) {
        errno = EOVERFLOW;
        return -1;
    }
    memset(d, 0, sizeof(*d));
    d->st_dev = s->st_dev;
    d->st_ino = fold_ino(s->st_ino);
    d->st_mode = s->st_mode;
    d->st_nlink = s->st_nlink;
    d->st_uid = s->st_uid;
    d->st_gid = s->st_gid;
    d->st_rdev = s->st_rdev;
    d->st_size = (off_t)s->st_size;
    d->st_blksize = s->st_blksize;
    d->st_blocks = (blkcnt_t)s->st_blocks;
    d->st_atime = s->st_atime;
    d->st_mtime = s->st_mtime;
    d->st_ctime = s->st_ctime;
    return 0;
}

int __xstat(int ver, const char *path, struct stat *buf)
{
    char b[PBUF];
    struct stat64 s;
    if (__xstat64(_STAT_VER, pin(path, b), &s) != 0)
        return -1;
    return conv_stat(&s, buf);
}

int __lxstat(int ver, const char *path, struct stat *buf)
{
    char b[PBUF];
    struct stat64 s;
    if (__lxstat64(_STAT_VER, pin(path, b), &s) != 0)
        return -1;
    return conv_stat(&s, buf);
}

int __fxstat(int ver, int fd, struct stat *buf)
{
    struct stat64 s;
    if (__fxstat64(_STAT_VER, fd, &s) != 0)
        return -1;
    return conv_stat(&s, buf);
}

/* One buffer per process is enough for (single-threaded) Applixware. */
static struct dirent ent32;

struct dirent *readdir(DIR *dir)
{
    struct dirent64 *e = readdir64(dir);
    if (e == NULL)
        return NULL;
    memset(&ent32, 0, sizeof(ent32));
    ent32.d_ino = fold_ino(e->d_ino);
    ent32.d_off = (off_t)e->d_off;
    ent32.d_type = e->d_type;
    strncpy(ent32.d_name, e->d_name, sizeof(ent32.d_name) - 1);
    pout_inplace(ent32.d_name);
    ent32.d_reclen = sizeof(ent32);
    return &ent32;
}

/* glibc's scandir calls its internal readdir, which cannot be interposed. */
int scandir(const char *path, struct dirent ***list,
            int (*sel)(const struct dirent *),
            int (*cmp)(const void *, const void *))
{
    DIR *d = opendir(path);
    struct dirent **v = NULL, *e;
    size_t n = 0, cap = 0;
    int save;

    if (d == NULL)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (sel != NULL && !sel(e))
            continue;
        if (n == cap) {
            struct dirent **nv;
            cap = cap ? cap * 2 : 32;
            nv = realloc(v, cap * sizeof(*v));
            if (nv == NULL)
                goto fail;
            v = nv;
        }
        v[n] = malloc(sizeof(**v));
        if (v[n] == NULL)
            goto fail;
        memcpy(v[n], e, sizeof(**v));
        n++;
    }
    closedir(d);
    if (cmp != NULL && n > 0)
        qsort(v, n, sizeof(*v), cmp);
    *list = v;
    return (int)n;

fail:
    save = errno;
    while (n > 0)
        free(v[--n]);
    free(v);
    closedir(d);
    errno = save ? save : ENOMEM;
    return -1;
}

/* ------------------------------------------------------------------ */
/* 2. file name conversion: paths in                                   */

int open(const char *path, int flags, ...)
{
    char b[PBUF];
    mode_t mode = 0;
    REAL(open);
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return real_open(pin(path, b), flags, mode);
}

int creat(const char *path, mode_t mode)
{
    char b[PBUF];
    REAL(creat);
    return real_creat(pin(path, b), mode);
}

FILE *fopen(const char *path, const char *mode)
{
    char b[PBUF];
    REAL(fopen);
    return real_fopen(pin(path, b), mode);
}

FILE *freopen(const char *path, const char *mode, FILE *f)
{
    char b[PBUF];
    REAL(freopen);
    return real_freopen(pin(path, b), mode, f);
}

int access(const char *path, int m)
{
    char b[PBUF];
    REAL(access);
    return real_access(pin(path, b), m);
}

int chdir(const char *path)
{
    char b[PBUF];
    REAL(chdir);
    return real_chdir(pin(path, b));
}

int chmod(const char *path, mode_t m)
{
    char b[PBUF];
    REAL(chmod);
    return real_chmod(pin(path, b), m);
}

int mkdir(const char *path, mode_t m)
{
    char b[PBUF];
    REAL(mkdir);
    return real_mkdir(pin(path, b), m);
}

int rmdir(const char *path)
{
    char b[PBUF];
    REAL(rmdir);
    return real_rmdir(pin(path, b));
}

int unlink(const char *path)
{
    char b[PBUF];
    REAL(unlink);
    return real_unlink(pin(path, b));
}

int remove(const char *path)
{
    char b[PBUF];
    REAL(remove);
    return real_remove(pin(path, b));
}

int rename(const char *from, const char *to)
{
    char b1[PBUF], b2[PBUF];
    REAL(rename);
    return real_rename(pin(from, b1), pin(to, b2));
}

DIR *opendir(const char *path)
{
    char b[PBUF];
    REAL(opendir);
    return real_opendir(pin(path, b));
}

/* ------------------------------------------------------------------ */
/* 2. file name conversion: paths out                                  */

char *getcwd(char *buf, size_t size)
{
    char *r;
    REAL(getcwd);
    r = real_getcwd(buf, size);
    if (r != NULL)
        pout_inplace(r);
    return r;
}

char *realpath(const char *path, char *resolved)
{
    char b[PBUF];
    char *r;
    REAL(realpath);
    r = real_realpath(pin(path, b), resolved);
    if (r != NULL)
        pout_inplace(r);
    return r;
}

ssize_t readlink(const char *path, char *buf, size_t size)
{
    char b[PBUF], tmp[PBUF];
    ssize_t n;
    REAL(readlink);
    n = real_readlink(pin(path, b), buf, size);
    if (n > 0 && conv_on() && (size_t)n < sizeof(tmp)) {
        memcpy(tmp, buf, n);
        tmp[n] = '\0';
        pout_inplace(tmp);
        n = (ssize_t)strlen(tmp);
        memcpy(buf, tmp, n);
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* 2. file name conversion: other programs                             */

/* Applixware's own helpers keep Latin-1; anything else gets UTF-8. */
static int is_applix_prog(const char *file)
{
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    return strstr(file, "/axdata/") != NULL || strcmp(base, "applix") == 0 ||
           strcmp(base, "builder") == 0 || strncmp(base, "ax", 2) == 0 ||
           strcmp(base, "elfmain") == 0 || strcmp(base, "vistasrc") == 0;
}

static const char *cmd_in(const char *cmd, char *buf, size_t size)
{
    if (!conv_on() || !has_high(cmd))
        return cmd;
    return xlate(cmd, buf, size, "/ \t\n'\"=:;<>|&()", comp_in);
}

int execvp(const char *file, char *const argv[])
{
    static char store[CBUF];
    char *nargv[MAXARGS];
    size_t used = 0;
    int i;
    REAL(execvp);
    if (!conv_on() || is_applix_prog(file))
        return real_execvp(file, argv);
    for (i = 0; argv[i] != NULL && i < MAXARGS - 1; i++) {
        if (has_high(argv[i]) && used < sizeof(store)) {
            nargv[i] = store + used;
            xlate(argv[i], nargv[i], sizeof(store) - used, "/", comp_in);
            used += strlen(nargv[i]) + 1;
        } else {
            nargv[i] = argv[i];
        }
    }
    nargv[i] = NULL;
    unsetenv("AX_FNCONV");
    return real_execvp(file, nargv);
}

int execlp(const char *file, const char *arg, ...)
{
    char *argv[MAXARGS];
    va_list ap;
    int i = 0;
    argv[i++] = (char *)arg;
    va_start(ap, arg);
    while (i < MAXARGS - 1 && (argv[i] = va_arg(ap, char *)) != NULL)
        i++;
    va_end(ap);
    argv[i] = NULL;
    return execvp(file, argv);
}

int system(const char *cmd)
{
    static char buf[CBUF];
    int r;
    REAL(system);
    if (cmd == NULL || !conv_on())
        return real_system(cmd);
    unsetenv("AX_FNCONV");
    r = real_system(cmd_in(cmd, buf, sizeof(buf)));
    setenv("AX_FNCONV", "1", 1);
    return r;
}

FILE *popen(const char *cmd, const char *type)
{
    static char buf[CBUF];
    FILE *f;
    REAL(popen);
    if (!conv_on())
        return real_popen(cmd, type);
    unsetenv("AX_FNCONV");
    f = real_popen(cmd_in(cmd, buf, sizeof(buf)), type);
    setenv("AX_FNCONV", "1", 1);
    return f;
}
