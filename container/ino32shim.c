/*
 * ino32shim - LD_PRELOAD for old (non-LFS) i386 binaries on filesystems with
 * 64-bit inode numbers (XFS, large ext4, overlayfs...).
 *
 * 32-bit stat()/readdir() fail with EOVERFLOW when st_ino/d_ino does not fit
 * in 32 bits. These wrappers call the *64 variants and fold the inode number
 * into 32 bits (hi ^ lo). Files larger than 2 GiB still give EOVERFLOW.
 *
 * Symbols used by Applixware 6.0: __xstat __lxstat __fxstat readdir scandir
 *
 * Part of applixware-wrapper.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

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
    struct stat64 s;
    if (__xstat64(_STAT_VER, path, &s) != 0)
        return -1;
    return conv_stat(&s, buf);
}

int __lxstat(int ver, const char *path, struct stat *buf)
{
    struct stat64 s;
    if (__lxstat64(_STAT_VER, path, &s) != 0)
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
