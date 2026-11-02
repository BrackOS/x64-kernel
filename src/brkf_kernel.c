#include "brkf_kernel.h"
#include "blockdev.h"
#include "heap.h"
#include "rtc.h"
#include "string.h"
#include "vga.h"

#define SECTORS_PER_BLOCK (BRKF_BLOCK_SIZE / BLK_SECTOR_SIZE)
#define BRKF_DRIVE 1

static brkf_t *fs = NULL;

static int io_read(void *ctx, uint64_t block, void *buf) {
    (void)ctx;
    return blk_read(BRKF_DRIVE, block * SECTORS_PER_BLOCK, SECTORS_PER_BLOCK, buf);
}
static int io_write(void *ctx, uint64_t block, const void *buf) {
    (void)ctx;
    return blk_write(BRKF_DRIVE, block * SECTORS_PER_BLOCK, SECTORS_PER_BLOCK, buf);
}
static void *io_alloc(void *ctx, size_t size) { (void)ctx; return kmalloc(size); }
static void io_release(void *ctx, void *ptr)  { (void)ctx; kfree(ptr); }

/* Seconds since 1970 from the RTC (days-from-civil algorithm) */
static uint64_t io_now(void *ctx) {
    (void)ctx;
    rtc_time_t t = rtc_get_time();
    int64_t y = t.year < 100 ? 2000 + t.year : t.year;
    int64_t m = t.month ? t.month : 1;
    int64_t d = t.day ? t.day : 1;
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    uint64_t secs = (uint64_t)days * 86400 + (uint64_t)t.hour * 3600 + (uint64_t)t.minute * 60 + t.second;
    return secs * 1000000000ULL;
}

static brkf_io_t make_io(void) {
    brkf_io_t io = { io_read, io_write, io_alloc, io_release, io_now, NULL };
    return io;
}

int kbrkf_available(void) { return blk_present(BRKF_DRIVE); }
int kbrkf_mounted(void)   { return fs != NULL; }
brkf_t *kbrkf(void)       { return fs; }

int kbrkf_mount(void) {
    if (fs) return 0;
    if (!kbrkf_available()) return BRKF_ENOENT;
    brkf_io_t io = make_io();
    return brkf_mount(&fs, &io);
}

int kbrkf_umount(void) {
    if (!fs) return BRKF_EINVAL;
    int r = brkf_unmount(fs);
    fs = NULL;
    return r;
}

int kbrkf_format(const char *label) {
    if (fs) return BRKF_EINVAL;                           /* unmount first */
    if (!kbrkf_available()) return BRKF_ENOENT;
    uint64_t blocks = blk_sectors(BRKF_DRIVE) / SECTORS_PER_BLOCK;
    if (blocks > 0xFFFFFFF0ULL) blocks = 0xFFFFFFF0ULL;
    brkf_io_t io = make_io();
    uint8_t uuid[16];
    uint64_t seed = io_now(NULL) ^ (blocks * 0x9E3779B97F4A7C15ULL);   /* no real entropy source yet */
    for (int i = 0; i < 16; i++) { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; uuid[i] = (uint8_t)(seed >> 33); }
    uuid[6] = (uint8_t)((uuid[6] & 0x0F) | 0x40);
    uuid[8] = (uint8_t)((uuid[8] & 0x3F) | 0x80);
    return brkf_mkfs(&io, blocks, label, 0, uuid);
}

static void fsck_report(void *ctx, const char *msg) {
    (void)ctx;
    vga_print("  ");
    vga_print(msg);
    vga_print("\n");
}

int kbrkf_check(int repair) {
    if (fs) return BRKF_EINVAL;
    if (!kbrkf_available()) return BRKF_ENOENT;
    brkf_io_t io = make_io();
    brkf_fsck_result_t res;
    int r = brkf_fsck(&io, repair, fsck_report, NULL, &res);
    vga_print_int((int)res.inodes_used);
    vga_print(" inodes (");
    vga_print_int((int)res.files);
    vga_print(" files, ");
    vga_print_int((int)res.dirs);
    vga_print(" directories), ");
    vga_print_int((int)res.blocks_used);
    vga_print(" blocks in use\n");
    if (r < 0) return r;
    vga_print_int((int)res.problems_found);
    vga_print(" problem(s) found, ");
    vga_print_int((int)res.problems_fixed);
    vga_print(" fixed\n");
    return 0;
}

const char *kbrkf_strerror(int err) {
    switch (err) {
        case BRKF_ENOENT: return "no such file or directory";
        case BRKF_EEXIST: return "already exists";
        case BRKF_ENOTDIR: return "not a directory";
        case BRKF_EISDIR: return "is a directory";
        case BRKF_ENOSPC: return "no space left";
        case BRKF_ENOTEMPTY: return "directory not empty";
        case BRKF_ENAMETOOLONG: return "name too long";
        case BRKF_EINVAL: return "invalid (or not a BRKF volume)";
        case BRKF_EUCLEAN: return "damaged structure (run: brkf fsck)";
        case BRKF_EIO: return "I/O error";
        case BRKF_ELOOP: return "too many symbolic links";
        case BRKF_ENOMEM: return "out of memory";
        case BRKF_EROFS: return "read-only";
        case BRKF_EPERM: return "not permitted";
        default: return "error";
    }
}

int kbrkf_read_file(uint32_t cwd, const char *path, uint8_t **data, size_t *len) {
    if (!fs) return BRKF_ENOENT;
    uint32_t ino;
    int r = brkf_resolve(fs, cwd, path, 1, &ino);
    if (r != 0) return r;
    brkf_stat_t st;
    if ((r = brkf_getattr(fs, ino, &st)) != 0) return r;
    if (!BRKF_S_ISREG(st.mode)) return BRKF_EISDIR;
    if (st.size == 0 || st.size > (16u << 20)) return BRKF_EINVAL;
    uint8_t *buf = (uint8_t *)kmalloc((size_t)st.size);
    if (!buf) return BRKF_ENOMEM;
    int64_t n = brkf_read(fs, ino, 0, buf, (size_t)st.size);
    if (n < 0 || (uint64_t)n != st.size) { kfree(buf); return n < 0 ? (int)n : BRKF_EIO; }
    *data = buf;
    *len = (size_t)st.size;
    return 0;
}

/* Walk ".." upwards, finding each directory's name in its parent */
int kbrkf_path_of(uint32_t dir, char *buf, size_t cap) {
    if (!fs || cap < 2) return BRKF_EINVAL;
    char parts[16][BRKF_NAME_MAX + 1];
    int depth = 0;
    uint32_t cur = dir;
    while (cur != BRKF_ROOT_INO) {
        if (depth >= 16) return BRKF_ENAMETOOLONG;
        uint32_t parent;
        int r = brkf_lookup(fs, cur, "..", &parent);
        if (r != 0) return r;
        uint64_t pos = 0; brkf_dirent_t e; int found = 0;
        while (brkf_readdir(fs, parent, &pos, &e) == 1) {
            if (e.ino == cur && strcmp(e.name, ".") != 0 && strcmp(e.name, "..") != 0) {
                strncpy_safe(parts[depth++], e.name, sizeof(parts[0]));
                found = 1;
                break;
            }
        }
        if (!found) return BRKF_EUCLEAN;
        cur = parent;
    }
    size_t n = 0;
    buf[n++] = '/';
    for (int i = depth - 1; i >= 0; i--) {
        size_t l = strlen(parts[i]);
        if (n + l + 2 > cap) return BRKF_ENAMETOOLONG;
        memcpy(buf + n, parts[i], l);
        n += l;
        if (i > 0) buf[n++] = '/';
    }
    buf[n] = '\0';
    return 0;
}
