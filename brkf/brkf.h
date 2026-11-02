#ifndef BRKF_H
#define BRKF_H

/* BRKF - the BrackOS filesystem, version 1.0. See docs/BRKF_SPEC.txt.
 *
 * This is a portable library: it talks to the disk through brkf_io_t and
 * never calls the operating system. All numbers on disk are little endian.
 * Functions return >= 0 on success and a negative Linux errno on failure. */

#include "brkf_port.h"

#define BRKF_MAGIC          0x464B5242u      /* bytes 'B' 'R' 'K' 'F' on disk */
#define BRKF_VERSION_MAJOR  1
#define BRKF_VERSION_MINOR  0
#define BRKF_BLOCK_SIZE     4096
#define BRKF_INODE_SIZE     256
#define BRKF_ROOT_INO       1
#define BRKF_NAME_MAX       255
#define BRKF_SYMLINK_MAX    4095
#define BRKF_NDIRECT        12

/* Inode mode bits (same values as POSIX) */
#define BRKF_S_IFMT   0xF000
#define BRKF_S_IFREG  0x8000
#define BRKF_S_IFDIR  0x4000
#define BRKF_S_IFLNK  0xA000
#define BRKF_S_ISDIR(m)  (((m) & BRKF_S_IFMT) == BRKF_S_IFDIR)
#define BRKF_S_ISREG(m)  (((m) & BRKF_S_IFMT) == BRKF_S_IFREG)
#define BRKF_S_ISLNK(m)  (((m) & BRKF_S_IFMT) == BRKF_S_IFLNK)

/* Directory entry types */
#define BRKF_FT_REG  1
#define BRKF_FT_DIR  2
#define BRKF_FT_LNK  7

/* Errors (negative Linux errno values) */
#define BRKF_EPERM        (-1)
#define BRKF_ENOENT       (-2)
#define BRKF_EIO          (-5)
#define BRKF_EROFS        (-30)
#define BRKF_ENOMEM       (-12)
#define BRKF_EEXIST       (-17)
#define BRKF_ENOTDIR      (-20)
#define BRKF_EISDIR       (-21)
#define BRKF_EINVAL       (-22)
#define BRKF_EFBIG        (-27)
#define BRKF_ENOSPC       (-28)
#define BRKF_EMLINK       (-31)
#define BRKF_ENAMETOOLONG (-36)
#define BRKF_ENOTEMPTY    (-39)
#define BRKF_ELOOP        (-40)
#define BRKF_EUCLEAN      (-117)             /* structure needs cleaning (bad checksum etc.) */

/* How the library reaches the disk and the outside world */
typedef struct brkf_io {
    int      (*read_block)(void *ctx, uint64_t block, void *buf);          /* 4096 bytes */
    int      (*write_block)(void *ctx, uint64_t block, const void *buf);   /* 4096 bytes */
    void    *(*alloc)(void *ctx, size_t size);
    void     (*release)(void *ctx, void *ptr);
    uint64_t (*now_ns)(void *ctx);                                         /* nanoseconds since 1970 */
    void     *ctx;
} brkf_io_t;

typedef struct brkf brkf_t;

typedef struct {
    uint32_t ino;
    uint16_t mode;
    uint32_t uid, gid, nlink;
    uint64_t size;
    uint64_t blocks;                         /* 4 KB blocks in use */
    uint64_t atime_ns, mtime_ns, ctime_ns, crtime_ns;
} brkf_stat_t;

typedef struct {
    uint64_t block_count, free_blocks;
    uint32_t inode_count, free_inodes;
    uint32_t state;                          /* BRKF_STATE_* */
    char     label[33];
    uint8_t  uuid[16];
} brkf_statfs_t;

typedef struct {
    uint32_t ino;
    uint8_t  type;                           /* BRKF_FT_* */
    char     name[BRKF_NAME_MAX + 1];
} brkf_dirent_t;

#define BRKF_STATE_CLEAN  1
#define BRKF_STATE_DIRTY  2

/* Which fields brkf_setattr should change */
#define BRKF_SET_MODE   0x01
#define BRKF_SET_UID    0x02
#define BRKF_SET_GID    0x04
#define BRKF_SET_ATIME  0x08
#define BRKF_SET_MTIME  0x10

/* ---- Creating, mounting ---- */
/* Format a volume of block_count blocks. inode_count 0 = one inode per 16 KB.
 * uuid may be NULL (a zero uuid is stored). */
int  brkf_mkfs(const brkf_io_t *io, uint64_t block_count, const char *label,
               uint32_t inode_count, const uint8_t *uuid);
int  brkf_mount(brkf_t **out, const brkf_io_t *io);
int  brkf_mount_ro(brkf_t **out, const brkf_io_t *io);   /* never writes; works on read-only media */
int  brkf_unmount(brkf_t *fs);               /* writes everything, marks the volume clean */
int  brkf_sync(brkf_t *fs);                  /* flush counters; the volume stays dirty-marked until unmount */
int  brkf_statfs(brkf_t *fs, brkf_statfs_t *out);

/* ---- Names and attributes (by inode number; the root is BRKF_ROOT_INO) ---- */
int  brkf_lookup(brkf_t *fs, uint32_t dir, const char *name, uint32_t *ino);
int  brkf_getattr(brkf_t *fs, uint32_t ino, brkf_stat_t *st);
int  brkf_setattr(brkf_t *fs, uint32_t ino, const brkf_stat_t *in, uint32_t valid);
int  brkf_resolve(brkf_t *fs, uint32_t cwd, const char *path, int follow_last, uint32_t *ino);
int  brkf_resolve_parent(brkf_t *fs, uint32_t cwd, const char *path, uint32_t *dir,
                         char *name /* BRKF_NAME_MAX+1 */);

/* ---- Creating and removing ---- */
int  brkf_create(brkf_t *fs, uint32_t dir, const char *name, uint16_t mode,
                 uint32_t uid, uint32_t gid, uint32_t *ino);
int  brkf_mkdir(brkf_t *fs, uint32_t dir, const char *name, uint16_t mode,
                uint32_t uid, uint32_t gid, uint32_t *ino);
int  brkf_symlink(brkf_t *fs, uint32_t dir, const char *name, const char *target,
                  uint32_t uid, uint32_t gid, uint32_t *ino);
int  brkf_readlink(brkf_t *fs, uint32_t ino, char *buf, size_t cap);   /* length, not NUL terminated */
int  brkf_link(brkf_t *fs, uint32_t dir, const char *name, uint32_t target);
int  brkf_unlink(brkf_t *fs, uint32_t dir, const char *name);
int  brkf_rmdir(brkf_t *fs, uint32_t dir, const char *name);
int  brkf_rename(brkf_t *fs, uint32_t old_dir, const char *old_name,
                 uint32_t new_dir, const char *new_name);

/* ---- File data ---- */
int64_t brkf_read(brkf_t *fs, uint32_t ino, uint64_t off, void *buf, size_t len);
int64_t brkf_write(brkf_t *fs, uint32_t ino, uint64_t off, const void *buf, size_t len);
int     brkf_truncate(brkf_t *fs, uint32_t ino, uint64_t size);

/* Iterate a directory: *pos starts at 0. Returns 1 and fills *ent, 0 at the end. */
int  brkf_readdir(brkf_t *fs, uint32_t dir, uint64_t *pos, brkf_dirent_t *ent);

/* ---- Checking ---- */
typedef void (*brkf_report_fn)(void *ctx, const char *message);
typedef struct {
    uint64_t inodes_used, blocks_used, dirs, files, symlinks;
    uint32_t problems_found, problems_fixed;
} brkf_fsck_result_t;
/* Check the volume. With repair != 0, fixes what can be fixed safely (bitmaps,
 * counters, link counts, leaked inodes). The volume must not be mounted. */
int  brkf_fsck(const brkf_io_t *io, int repair, brkf_report_fn report, void *report_ctx,
               brkf_fsck_result_t *result);

uint32_t brkf_crc32(const void *data, size_t len);

#endif
