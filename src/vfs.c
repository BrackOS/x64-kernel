#include "vfs.h"
#include "brkf_kernel.h"
#include "sched.h"
#include "vmm.h"
#include "heap.h"
#include "string.h"

#define ENOENT   2
#define EBADF    9
#define ENOMEM  12
#define EFAULT  14
#define EINVAL  22
#define EMFILE  24
#define ERANGE  34

#define O_ACCMODE   3
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0x40
#define O_EXCL      0x80
#define O_TRUNC     0x200
#define O_APPEND    0x400
#define O_DIRECTORY 0x10000

#define PATH_MAX_USER 512
#define IO_CHUNK 4096

int vfs_is_file_fd(int fd) { return fd >= 3 && fd < TASK_MAX_FDS; }

static task_file_t *get_file(int fd) {
    if (!vfs_is_file_fd(fd)) return NULL;
    task_file_t *f = &sched_current()->files[fd];
    return f->used ? f : NULL;
}

static uint32_t cwd_of(task_t *t) { return t->cwd ? t->cwd : BRKF_ROOT_INO; }

/* Copy a path in from user space and find what it names */
static long get_path(uint64_t upath, char *path) {
    if (!kbrkf_mounted()) return -ENOENT;
    int n = strncpy_from_user(path, upath, PATH_MAX_USER);
    if (n < 0) return -EFAULT;
    if (n == 0) return -ENOENT;
    return 0;
}

long vfs_open(uint64_t upath, int flags, int mode) {
    char path[PATH_MAX_USER];
    long r = get_path(upath, path);
    if (r) return r;
    task_t *t = sched_current();
    brkf_t *fs = kbrkf();
    int slot = -1;
    for (int i = 3; i < TASK_MAX_FDS; i++) if (!t->files[i].used) { slot = i; break; }
    if (slot < 0) return -EMFILE;

    uint32_t ino;
    int e = brkf_resolve(fs, cwd_of(t), path, 1, &ino);
    if (e == BRKF_ENOENT && (flags & O_CREAT)) {
        uint32_t dir; char name[BRKF_NAME_MAX + 1];
        if ((e = brkf_resolve_parent(fs, cwd_of(t), path, &dir, name)) != 0) return e;
        if ((e = brkf_create(fs, dir, name, (uint16_t)(mode ? mode & 07777 : 0644), 0, 0, &ino)) != 0) return e;
    } else if (e != 0) {
        return e;
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        return BRKF_EEXIST;
    }

    brkf_stat_t st;
    if ((e = brkf_getattr(fs, ino, &st)) != 0) return e;
    if (BRKF_S_ISDIR(st.mode)) {
        if ((flags & O_ACCMODE) != 0) return BRKF_EISDIR;
    } else {
        if (flags & O_DIRECTORY) return BRKF_ENOTDIR;
        if ((flags & O_TRUNC) && (flags & O_ACCMODE) != 0 && BRKF_S_ISREG(st.mode)) {
            if ((e = brkf_truncate(fs, ino, 0)) != 0) return e;
        }
    }
    t->files[slot].used = 1;
    t->files[slot].ino = ino;
    t->files[slot].flags = (uint32_t)flags;
    t->files[slot].pos = 0;
    return slot;
}

long vfs_close(int fd) {
    task_file_t *f = get_file(fd);
    if (!f) return -EBADF;
    f->used = 0;
    return 0;
}

long vfs_read(int fd, uint64_t ubuf, uint64_t count) {
    task_file_t *f = get_file(fd);
    if (!f || (f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    uint8_t *buf = (uint8_t *)kmalloc(IO_CHUNK);
    if (!buf) return -ENOMEM;
    uint64_t done = 0;
    long result = 0;
    while (done < count) {
        size_t chunk = (count - done > IO_CHUNK) ? IO_CHUNK : (size_t)(count - done);
        int64_t n = brkf_read(kbrkf(), f->ino, f->pos, buf, chunk);
        if (n < 0) { result = done ? (long)done : (long)n; goto out; }
        if (n == 0) break;
        if (copy_to_user(ubuf + done, buf, (size_t)n) != 0) { result = done ? (long)done : -EFAULT; goto out; }
        f->pos += (uint64_t)n;
        done += (uint64_t)n;
        if ((size_t)n < chunk) break;
    }
    result = (long)done;
out:
    kfree(buf);
    return result;
}

long vfs_write(int fd, uint64_t ubuf, uint64_t count) {
    task_file_t *f = get_file(fd);
    if (!f || (f->flags & O_ACCMODE) == 0) return -EBADF;
    brkf_t *fs = kbrkf();
    if (f->flags & O_APPEND) {
        brkf_stat_t st;
        if (brkf_getattr(fs, f->ino, &st) == 0) f->pos = st.size;
    }
    uint8_t *buf = (uint8_t *)kmalloc(IO_CHUNK);
    if (!buf) return -ENOMEM;
    uint64_t done = 0;
    long result = 0;
    while (done < count) {
        size_t chunk = (count - done > IO_CHUNK) ? IO_CHUNK : (size_t)(count - done);
        if (copy_from_user(buf, ubuf + done, chunk) != 0) { result = done ? (long)done : -EFAULT; goto out; }
        int64_t n = brkf_write(fs, f->ino, f->pos, buf, chunk);
        if (n < 0) { result = done ? (long)done : (long)n; goto out; }
        f->pos += (uint64_t)n;
        done += (uint64_t)n;
        if ((size_t)n < chunk) break;
    }
    result = (long)done;
out:
    kfree(buf);
    return result;
}

long vfs_lseek(int fd, int64_t off, int whence) {
    task_file_t *f = get_file(fd);
    if (!f) return -EBADF;
    int64_t base;
    if (whence == 0) base = 0;
    else if (whence == 1) base = (int64_t)f->pos;
    else if (whence == 2) {
        brkf_stat_t st;
        int e = brkf_getattr(kbrkf(), f->ino, &st);
        if (e != 0) return e;
        base = (int64_t)st.size;
    } else return -EINVAL;
    if (base + off < 0) return -EINVAL;
    f->pos = (uint64_t)(base + off);
    return (long)f->pos;
}

/* Fill a Linux x86-64 struct stat (144 bytes) */
static int put_stat(uint64_t ustat, const brkf_stat_t *s) {
    uint64_t v[18];
    memset(v, 0, sizeof(v));
    v[1] = s->ino;
    v[2] = s->nlink;
    ((uint32_t *)v)[6] = s->mode;
    ((uint32_t *)v)[7] = s->uid;
    ((uint32_t *)v)[8] = s->gid;
    v[6] = s->size;
    v[7] = BRKF_BLOCK_SIZE;
    v[8] = s->blocks * (BRKF_BLOCK_SIZE / 512);
    v[9]  = s->atime_ns / 1000000000ULL; v[10] = s->atime_ns % 1000000000ULL;
    v[11] = s->mtime_ns / 1000000000ULL; v[12] = s->mtime_ns % 1000000000ULL;
    v[13] = s->ctime_ns / 1000000000ULL; v[14] = s->ctime_ns % 1000000000ULL;
    return copy_to_user(ustat, v, sizeof(v)) == 0 ? 0 : -EFAULT;
}

long vfs_fstat(int fd, uint64_t ustat) {
    task_file_t *f = get_file(fd);
    if (!f) return -EBADF;
    brkf_stat_t st;
    int e = brkf_getattr(kbrkf(), f->ino, &st);
    return e != 0 ? e : put_stat(ustat, &st);
}

long vfs_stat(uint64_t upath, uint64_t ustat, int follow) {
    char path[PATH_MAX_USER];
    long r = get_path(upath, path);
    if (r) return r;
    uint32_t ino;
    int e = brkf_resolve(kbrkf(), cwd_of(sched_current()), path, follow, &ino);
    if (e != 0) return e;
    brkf_stat_t st;
    if ((e = brkf_getattr(kbrkf(), ino, &st)) != 0) return e;
    return put_stat(ustat, &st);
}

long vfs_ftruncate(int fd, uint64_t size) {
    task_file_t *f = get_file(fd);
    if (!f || (f->flags & O_ACCMODE) == 0) return -EBADF;
    return brkf_truncate(kbrkf(), f->ino, size);
}

long vfs_getdents64(int fd, uint64_t ubuf, uint64_t count) {
    task_file_t *f = get_file(fd);
    if (!f) return -EBADF;
    if (count < 32) return -EINVAL;
    if (count > IO_CHUNK) count = IO_CHUNK;
    uint8_t *buf = (uint8_t *)kmalloc((size_t)count);
    if (!buf) return -ENOMEM;
    size_t used = 0;
    long result;
    brkf_dirent_t e;
    int r;
    for (;;) {
        uint64_t pos = f->pos;
        r = brkf_readdir(kbrkf(), f->ino, &pos, &e);
        if (r != 1) break;
        size_t nlen = strlen(e.name);
        size_t reclen = (19 + nlen + 1 + 7) & ~(size_t)7;       /* d_ino, d_off, d_reclen, d_type, name, NUL */
        if (used + reclen > count) break;
        uint8_t *p = buf + used;
        uint64_t ino64 = e.ino, off64 = pos;
        memcpy(p, &ino64, 8);
        memcpy(p + 8, &off64, 8);
        uint16_t rl = (uint16_t)reclen;
        memcpy(p + 16, &rl, 2);
        p[18] = e.type == BRKF_FT_DIR ? 4 : e.type == BRKF_FT_LNK ? 10 : 8;
        memcpy(p + 19, e.name, nlen + 1);
        used += reclen;
        f->pos = pos;
    }
    if (r < 0 && used == 0) result = r;
    else if (copy_to_user(ubuf, buf, used) != 0) result = -EFAULT;
    else result = (long)used;                                   /* 0 = end of directory */
    kfree(buf);
    return result;
}

static long with_parent(uint64_t upath, uint32_t *dir, char *name) {
    char path[PATH_MAX_USER];
    long r = get_path(upath, path);
    if (r) return r;
    return brkf_resolve_parent(kbrkf(), cwd_of(sched_current()), path, dir, name);
}

long vfs_mkdir(uint64_t upath, int mode) {
    uint32_t dir; char name[BRKF_NAME_MAX + 1];
    long r = with_parent(upath, &dir, name);
    if (r) return r;
    return brkf_mkdir(kbrkf(), dir, name, (uint16_t)(mode & 07777), 0, 0, NULL);
}

long vfs_rmdir(uint64_t upath) {
    uint32_t dir; char name[BRKF_NAME_MAX + 1];
    long r = with_parent(upath, &dir, name);
    if (r) return r;
    return brkf_rmdir(kbrkf(), dir, name);
}

long vfs_unlink(uint64_t upath) {
    uint32_t dir; char name[BRKF_NAME_MAX + 1];
    long r = with_parent(upath, &dir, name);
    if (r) return r;
    return brkf_unlink(kbrkf(), dir, name);
}

long vfs_rename(uint64_t uold, uint64_t unew) {
    uint32_t d1, d2; char n1[BRKF_NAME_MAX + 1], n2[BRKF_NAME_MAX + 1];
    long r = with_parent(uold, &d1, n1);
    if (r) return r;
    if ((r = with_parent(unew, &d2, n2)) != 0) return r;
    return brkf_rename(kbrkf(), d1, n1, d2, n2);
}

long vfs_chdir(uint64_t upath) {
    char path[PATH_MAX_USER];
    long r = get_path(upath, path);
    if (r) return r;
    task_t *t = sched_current();
    uint32_t ino;
    int e = brkf_resolve(kbrkf(), cwd_of(t), path, 1, &ino);
    if (e != 0) return e;
    brkf_stat_t st;
    if ((e = brkf_getattr(kbrkf(), ino, &st)) != 0) return e;
    if (!BRKF_S_ISDIR(st.mode)) return BRKF_ENOTDIR;
    t->cwd = ino;
    return 0;
}

long vfs_getcwd(uint64_t ubuf, uint64_t size) {
    if (!kbrkf_mounted()) return -ENOENT;
    char path[PATH_MAX_USER];
    int e = kbrkf_path_of(cwd_of(sched_current()), path, sizeof(path));
    if (e != 0) return e;
    size_t n = strlen(path) + 1;
    if (n > size) return -ERANGE;
    if (copy_to_user(ubuf, path, n) != 0) return -EFAULT;
    return (long)n;
}

long vfs_readlink(uint64_t upath, uint64_t ubuf, uint64_t size) {
    char path[PATH_MAX_USER];
    long r = get_path(upath, path);
    if (r) return r;
    uint32_t ino;
    int e = brkf_resolve(kbrkf(), cwd_of(sched_current()), path, 0, &ino);
    if (e != 0) return e;
    char target[BRKF_NAME_MAX + 1];
    int n = brkf_readlink(kbrkf(), ino, target, sizeof(target) - 1);
    if (n < 0) return n;
    if ((uint64_t)n > size) n = (int)size;
    return copy_to_user(ubuf, target, (size_t)n) == 0 ? n : -EFAULT;
}
