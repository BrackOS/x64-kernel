/* BRKF core: see brkf.h and docs/BRKF_SPEC.txt */
#include "brkf.h"

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "BRKF v1 stores structures in memory layout; build on a little-endian machine"
#endif

/* ---------------------------------------------------------------- on-disk */

typedef struct {
    uint32_t magic;                  /*   0 */
    uint16_t ver_major, ver_minor;   /*   4 */
    uint32_t block_size;             /*   8 */
    uint32_t inode_size;             /*  12 */
    uint64_t block_count;            /*  16 */
    uint32_t inode_count;            /*  24 */
    uint32_t root_ino;               /*  28 */
    uint64_t free_blocks;            /*  32 */
    uint32_t free_inodes;            /*  40 */
    uint32_t state;                  /*  44 */
    uint32_t incompat, compat, rocompat;   /* 48 52 56 */
    uint32_t mount_count;            /*  60 */
    uint64_t inode_bitmap_start;     /*  64 */
    uint64_t block_bitmap_start;     /*  72 */
    uint64_t inode_table_start;      /*  80 */
    uint64_t data_start;             /*  88 */
    uint32_t inode_bitmap_blocks;    /*  96 */
    uint32_t block_bitmap_blocks;    /* 100 */
    uint32_t inode_table_blocks;     /* 104 */
    uint32_t reserved0;              /* 108 */
    uint64_t created_ns;             /* 112 */
    uint64_t last_mount_ns;          /* 120 */
    uint8_t  uuid[16];               /* 128 */
    char     label[32];              /* 144 */
    uint8_t  reserved[76];           /* 176 */
    uint32_t crc32;                  /* 252: CRC-32 of bytes 0..251 */
} sb_t;

typedef struct {
    uint16_t mode, flags;            /*   0 */
    uint32_t uid, gid, nlink;        /*   4 */
    uint64_t size, blocks;           /*  16 */
    uint64_t atime, mtime, ctime, crtime;   /* 32 */
    uint32_t direct[BRKF_NDIRECT];   /*  64 */
    uint32_t indirect, dindirect, tindirect;   /* 112 */
    uint32_t generation;             /* 124 */
    uint8_t  pad[124];               /* 128 */
    uint32_t crc32;                  /* 252: CRC-32 of bytes 0..251 */
} inode_t;

typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];
} dent_t;

_Static_assert(sizeof(sb_t) == 256 && __builtin_offsetof(sb_t, crc32) == 252, "superblock layout");
_Static_assert(__builtin_offsetof(inode_t, direct) == 64 && __builtin_offsetof(inode_t, crc32) == 252, "inode layout");
_Static_assert(sizeof(inode_t) == BRKF_INODE_SIZE, "inode size");

#define IF_INLINE_SYMLINK  0x0001    /* the target is stored in the block pointer area (<= 60 bytes) */
#define INLINE_MAX         60
#define PTRS               1024u     /* block pointers per block */
#define INODES_PER_BLOCK   (BRKF_BLOCK_SIZE / BRKF_INODE_SIZE)
#define BITS_PER_BLOCK     (BRKF_BLOCK_SIZE * 8u)
#define SB_BLOCK           1
#define MAX_LINKS          65000

/* ---------------------------------------------------------------- state */

#define CACHE_SLOTS 64

typedef struct { uint64_t blk; int valid; uint8_t *data; } cslot_t;

struct brkf {
    brkf_io_t io;
    sb_t      sb;
    uint64_t  block_count;
    uint32_t  inode_count;
    uint64_t  blk_hint;
    uint32_t  ino_hint;
    int       sb_dirty;
    int       mark_dirty;            /* this mount set the volume dirty (and must clear it) */
    int       readonly;              /* mounted read-only: nothing is ever written */
    uint8_t  *cache_mem;
    cslot_t   slots[CACHE_SLOTS];
};

/* ---------------------------------------------------------------- helpers */

static uint32_t crc_table[256];
static int crc_ready;

uint32_t brkf_crc32(const void *data, size_t len) {
    if (!crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
        crc_ready = 1;
    }
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static uint64_t now_ns(brkf_t *fs) { return fs->io.now_ns ? fs->io.now_ns(fs->io.ctx) : 0; }

static uint8_t *bnew(brkf_t *fs) { return (uint8_t *)fs->io.alloc(fs->io.ctx, BRKF_BLOCK_SIZE); }
static void bdel(brkf_t *fs, void *p) { if (p) fs->io.release(fs->io.ctx, p); }

static int bread(brkf_t *fs, uint64_t blk, void *buf) {
    if (blk >= fs->block_count) return BRKF_EUCLEAN;
    cslot_t *s = fs->cache_mem ? &fs->slots[blk % CACHE_SLOTS] : NULL;
    if (s && s->valid && s->blk == blk) { memcpy(buf, s->data, BRKF_BLOCK_SIZE); return 0; }
    if (fs->io.read_block(fs->io.ctx, blk, buf) != 0) return BRKF_EIO;
    if (s) { memcpy(s->data, buf, BRKF_BLOCK_SIZE); s->blk = blk; s->valid = 1; }
    return 0;
}

static int bwrite(brkf_t *fs, uint64_t blk, const void *buf) {
    if (fs->readonly) return BRKF_EROFS;
    if (blk >= fs->block_count) return BRKF_EUCLEAN;
    cslot_t *s = fs->cache_mem ? &fs->slots[blk % CACHE_SLOTS] : NULL;
    if (fs->io.write_block(fs->io.ctx, blk, buf) != 0) {
        if (s && s->valid && s->blk == blk) s->valid = 0;
        return BRKF_EIO;
    }
    if (s) { memcpy(s->data, buf, BRKF_BLOCK_SIZE); s->blk = blk; s->valid = 1; }
    return 0;
}

static size_t align4(size_t n) { return (n + 3) & ~(size_t)3; }

static int valid_name(const char *name) {
    size_t n = strlen(name);
    if (n == 0) return BRKF_ENOENT;
    if (n > BRKF_NAME_MAX) return BRKF_ENAMETOOLONG;
    for (size_t i = 0; i < n; i++) if (name[i] == '/') return BRKF_EINVAL;
    return 0;
}
static int is_dots(const char *name) {
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

/* ---------------------------------------------------------------- superblock */

static int sb_store(brkf_t *fs) {
    uint8_t *buf = bnew(fs);
    if (!buf) return BRKF_ENOMEM;
    memset(buf, 0, BRKF_BLOCK_SIZE);
    fs->sb.crc32 = brkf_crc32(&fs->sb, sizeof(sb_t) - 4);
    memcpy(buf, &fs->sb, sizeof(sb_t));
    int r = bwrite(fs, SB_BLOCK, buf);
    if (r == 0) r = bwrite(fs, fs->block_count - 1, buf);       /* backup copy in the last block */
    bdel(fs, buf);
    if (r == 0) fs->sb_dirty = 0;
    return r;
}

/* ---------------------------------------------------------------- bitmaps */

static int bm_get(brkf_t *fs, uint64_t base, uint64_t bit, int *value) {
    uint8_t *buf = bnew(fs);
    if (!buf) return BRKF_ENOMEM;
    int r = bread(fs, base + bit / BITS_PER_BLOCK, buf);
    if (r == 0) {
        uint32_t b = (uint32_t)(bit % BITS_PER_BLOCK);
        *value = (buf[b / 8] >> (b % 8)) & 1;
    }
    bdel(fs, buf);
    return r;
}

static int bm_put(brkf_t *fs, uint64_t base, uint64_t bit, int value) {
    uint8_t *buf = bnew(fs);
    if (!buf) return BRKF_ENOMEM;
    uint64_t blk = base + bit / BITS_PER_BLOCK;
    int r = bread(fs, blk, buf);
    if (r == 0) {
        uint32_t b = (uint32_t)(bit % BITS_PER_BLOCK);
        if (value) buf[b / 8] |= (uint8_t)(1u << (b % 8));
        else       buf[b / 8] &= (uint8_t)~(1u << (b % 8));
        r = bwrite(fs, blk, buf);
    }
    bdel(fs, buf);
    return r;
}

/* Find a clear bit (starting near `hint`), set it, return its index */
static int bm_alloc(brkf_t *fs, uint64_t base, uint64_t nbits, uint64_t hint, uint64_t *out) {
    uint64_t nblocks = (nbits + BITS_PER_BLOCK - 1) / BITS_PER_BLOCK;
    uint8_t *buf = bnew(fs);
    if (!buf) return BRKF_ENOMEM;
    uint64_t first = (hint < nbits ? hint : 0) / BITS_PER_BLOCK;
    int result = BRKF_ENOSPC;

    for (uint64_t n = 0; n < nblocks && result == BRKF_ENOSPC; n++) {
        uint64_t bi = (first + n) % nblocks;
        int r = bread(fs, base + bi, buf);
        if (r != 0) { result = r; break; }
        for (uint32_t byte = 0; byte < BRKF_BLOCK_SIZE; byte++) {
            if (buf[byte] == 0xFF) continue;
            for (int bit = 0; bit < 8; bit++) {
                if (buf[byte] & (1u << bit)) continue;
                uint64_t idx = bi * BITS_PER_BLOCK + (uint64_t)byte * 8 + (uint64_t)bit;
                if (idx >= nbits) break;
                buf[byte] |= (uint8_t)(1u << bit);
                r = bwrite(fs, base + bi, buf);
                if (r == 0) { *out = idx; result = 0; } else result = r;
                goto done;
            }
        }
    }
done:
    bdel(fs, buf);
    return result;
}

/* ---------------------------------------------------------------- blocks and inodes */

static int balloc(brkf_t *fs, uint32_t *out) {
    if (fs->sb.free_blocks == 0) return BRKF_ENOSPC;
    uint64_t b;
    int r = bm_alloc(fs, fs->sb.block_bitmap_start, fs->block_count, fs->blk_hint, &b);
    if (r != 0) return r;
    if (b < fs->sb.data_start || b >= fs->block_count - 1 || b > 0xFFFFFFFEu) {   /* bitmap damaged */
        bm_put(fs, fs->sb.block_bitmap_start, b, 1);
        return BRKF_EUCLEAN;
    }
    uint8_t *z = bnew(fs);
    if (!z) { bm_put(fs, fs->sb.block_bitmap_start, b, 0); return BRKF_ENOMEM; }
    memset(z, 0, BRKF_BLOCK_SIZE);
    r = bwrite(fs, b, z);                                  /* new blocks always start zeroed */
    bdel(fs, z);
    if (r != 0) { bm_put(fs, fs->sb.block_bitmap_start, b, 0); return r; }
    fs->blk_hint = b + 1;
    fs->sb.free_blocks--;
    fs->sb_dirty = 1;
    *out = (uint32_t)b;
    return 0;
}

static int bfree(brkf_t *fs, uint32_t b) {
    if (b < fs->sb.data_start || b >= fs->block_count - 1) return BRKF_EUCLEAN;
    int r = bm_put(fs, fs->sb.block_bitmap_start, b, 0);
    if (r != 0) return r;
    fs->sb.free_blocks++;
    fs->sb_dirty = 1;
    if (b < fs->blk_hint) fs->blk_hint = b;
    return 0;
}

static int ino_loc(brkf_t *fs, uint32_t ino, uint64_t *blk, uint32_t *off) {
    if (ino == 0 || ino > fs->inode_count) return BRKF_EINVAL;
    uint32_t idx = ino - 1;
    *blk = fs->sb.inode_table_start + idx / INODES_PER_BLOCK;
    *off = (idx % INODES_PER_BLOCK) * BRKF_INODE_SIZE;
    return 0;
}

static int iget(brkf_t *fs, uint32_t ino, inode_t *out) {
    uint64_t blk; uint32_t off;
    int r = ino_loc(fs, ino, &blk, &off);
    if (r != 0) return r;
    uint8_t *buf = bnew(fs);
    if (!buf) return BRKF_ENOMEM;
    r = bread(fs, blk, buf);
    if (r == 0) {
        memcpy(out, buf + off, sizeof(inode_t));
        if (out->mode == 0 && out->crc32 == 0) r = BRKF_ENOENT;                   /* free slot */
        else if (brkf_crc32(out, sizeof(inode_t) - 4) != out->crc32) r = BRKF_EUCLEAN;
    }
    bdel(fs, buf);
    return r;
}

static int iput(brkf_t *fs, uint32_t ino, inode_t *in) {
    uint64_t blk; uint32_t off;
    int r = ino_loc(fs, ino, &blk, &off);
    if (r != 0) return r;
    uint8_t *buf = bnew(fs);
    if (!buf) return BRKF_ENOMEM;
    r = bread(fs, blk, buf);
    if (r == 0) {
        in->crc32 = brkf_crc32(in, sizeof(inode_t) - 4);
        memcpy(buf + off, in, sizeof(inode_t));
        r = bwrite(fs, blk, buf);
    }
    bdel(fs, buf);
    return r;
}

/* Allocate an inode, initialise it and write it out */
static int ialloc(brkf_t *fs, uint16_t mode, uint32_t uid, uint32_t gid, uint32_t *ino, inode_t *in) {
    if (fs->sb.free_inodes == 0) return BRKF_ENOSPC;
    uint64_t bit;
    int r = bm_alloc(fs, fs->sb.inode_bitmap_start, fs->inode_count, fs->ino_hint, &bit);
    if (r != 0) return r;
    memset(in, 0, sizeof(*in));
    in->mode = mode;
    in->uid = uid;
    in->gid = gid;
    in->nlink = 1;
    in->atime = in->mtime = in->ctime = in->crtime = now_ns(fs);
    in->generation = (uint32_t)(now_ns(fs) >> 10) ^ (uint32_t)bit;
    *ino = (uint32_t)bit + 1;
    r = iput(fs, *ino, in);
    if (r != 0) { bm_put(fs, fs->sb.inode_bitmap_start, bit, 0); return r; }
    fs->ino_hint = (uint32_t)bit + 1;
    fs->sb.free_inodes--;
    fs->sb_dirty = 1;
    return 0;
}

static int ifree(brkf_t *fs, uint32_t ino) {
    uint64_t blk; uint32_t off;
    int r = ino_loc(fs, ino, &blk, &off);
    if (r != 0) return r;
    uint8_t *buf = bnew(fs);
    if (!buf) return BRKF_ENOMEM;
    r = bread(fs, blk, buf);
    if (r == 0) { memset(buf + off, 0, BRKF_INODE_SIZE); r = bwrite(fs, blk, buf); }
    bdel(fs, buf);
    if (r != 0) return r;
    r = bm_put(fs, fs->sb.inode_bitmap_start, ino - 1, 0);
    if (r != 0) return r;
    fs->sb.free_inodes++;
    fs->sb_dirty = 1;
    if (ino - 1 < fs->ino_hint) fs->ino_hint = ino - 1;
    return 0;
}

/* ---------------------------------------------------------------- block map */

static uint64_t pow_p(int e) { uint64_t v = 1; while (e-- > 0) v *= PTRS; return v; }

/* Map logical block lbn of a file to a disk block. With alloc, missing blocks
 * (and the tables leading to them) are created and *dirty is set if the inode
 * changed. A result of 0 means "hole". */
static int bmap(brkf_t *fs, inode_t *in, uint64_t lbn, int alloc, uint32_t *out, int *dirty) {
    int r;
    *out = 0;
    if (lbn < BRKF_NDIRECT) {
        uint32_t b = in->direct[lbn];
        if (!b && alloc) {
            if ((r = balloc(fs, &b)) != 0) return r;
            in->direct[lbn] = b; in->blocks++; *dirty = 1;
        }
        *out = b;
        return 0;
    }
    lbn -= BRKF_NDIRECT;
    int depth;
    uint32_t *root;
    if (lbn < PTRS)                       { depth = 1; root = &in->indirect; }
    else if ((lbn -= PTRS) < pow_p(2))    { depth = 2; root = &in->dindirect; }
    else if ((lbn -= pow_p(2)) < pow_p(3)) { depth = 3; root = &in->tindirect; }
    else return BRKF_EFBIG;

    uint32_t cur = *root;
    if (!cur) {
        if (!alloc) return 0;
        if ((r = balloc(fs, &cur)) != 0) return r;
        *root = cur; in->blocks++; *dirty = 1;
    }
    uint8_t *buf = bnew(fs);
    if (!buf) return BRKF_ENOMEM;
    for (int level = depth; level >= 1; level--) {
        uint64_t per = pow_p(level - 1);
        uint32_t i = (uint32_t)(lbn / per);
        lbn %= per;
        if ((r = bread(fs, cur, buf)) != 0) { bdel(fs, buf); return r; }
        uint32_t *tab = (uint32_t *)buf;
        uint32_t next = tab[i];
        if (!next) {
            if (!alloc) { bdel(fs, buf); return 0; }
            if ((r = balloc(fs, &next)) != 0) { bdel(fs, buf); return r; }
            tab[i] = next;
            if ((r = bwrite(fs, cur, buf)) != 0) { bdel(fs, buf); return r; }
            in->blocks++; *dirty = 1;
        }
        cur = next;
    }
    bdel(fs, buf);
    *out = cur;
    return 0;
}

/* Free the part of an indirect tree that lies at or beyond block `keep`.
 * `*slot` is the table's block number; `base` is the first file block it maps. */
static int free_tree(brkf_t *fs, uint32_t *slot, int depth, uint64_t base, uint64_t keep, uint64_t *freed) {
    if (!*slot) return 0;
    uint8_t *buf = bnew(fs);
    if (!buf) return BRKF_ENOMEM;
    int r = bread(fs, *slot, buf);
    if (r != 0) { bdel(fs, buf); return r; }
    uint32_t *tab = (uint32_t *)buf;
    uint64_t span = pow_p(depth - 1);
    int changed = 0;

    for (uint32_t i = 0; i < PTRS && r == 0; i++) {
        if (!tab[i]) continue;
        uint64_t start = base + (uint64_t)i * span;
        if (start + span <= keep) continue;                  /* entirely kept */
        if (depth == 1) {
            if ((r = bfree(fs, tab[i])) == 0) { tab[i] = 0; changed = 1; (*freed)++; }
        } else {
            uint32_t child = tab[i];
            r = free_tree(fs, &child, depth - 1, start, keep, freed);
            if (child != tab[i]) { tab[i] = child; changed = 1; }
        }
    }
    if (r == 0 && base >= keep) {                            /* the whole table is gone */
        r = bfree(fs, *slot);
        if (r == 0) { *slot = 0; (*freed)++; changed = 0; }
    } else if (r == 0 && changed) {
        r = bwrite(fs, *slot, buf);
    }
    bdel(fs, buf);
    return r;
}

/* Release every block of the file beyond the first `keep` blocks */
static int free_blocks_from(brkf_t *fs, inode_t *in, uint64_t keep) {
    uint64_t freed = 0;
    int r = 0;
    for (uint64_t i = keep; i < BRKF_NDIRECT && r == 0; i++) {
        if (in->direct[i]) { r = bfree(fs, in->direct[i]); if (r == 0) { in->direct[i] = 0; freed++; } }
    }
    if (r == 0) r = free_tree(fs, &in->indirect, 1, BRKF_NDIRECT, keep, &freed);
    if (r == 0) r = free_tree(fs, &in->dindirect, 2, BRKF_NDIRECT + PTRS, keep, &freed);
    if (r == 0) r = free_tree(fs, &in->tindirect, 3, BRKF_NDIRECT + PTRS + pow_p(2), keep, &freed);
    in->blocks = in->blocks >= freed ? in->blocks - freed : 0;
    return r;
}

/* ---------------------------------------------------------------- file data */

static int64_t file_read(brkf_t *fs, inode_t *in, uint64_t off, void *dst, size_t len) {
    if (off >= in->size) return 0;
    if (len > in->size - off) len = (size_t)(in->size - off);
    uint8_t *out = (uint8_t *)dst;
    uint8_t *buf = bnew(fs);
    if (!buf) return BRKF_ENOMEM;
    size_t done = 0;
    int r = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t boff = (uint32_t)(pos % BRKF_BLOCK_SIZE);
        size_t chunk = BRKF_BLOCK_SIZE - boff;
        if (chunk > len - done) chunk = len - done;
        uint32_t phys; int dirty = 0;
        if ((r = bmap(fs, in, pos / BRKF_BLOCK_SIZE, 0, &phys, &dirty)) != 0) break;
        if (phys == 0) memset(out + done, 0, chunk);          /* a hole reads as zeros */
        else {
            if ((r = bread(fs, phys, buf)) != 0) break;
            memcpy(out + done, buf + boff, chunk);
        }
        done += chunk;
    }
    bdel(fs, buf);
    return done > 0 ? (int64_t)done : (r != 0 ? r : 0);
}

/* Writes data and updates `in` (size, blocks, times); the caller stores the inode. */
static int64_t file_write(brkf_t *fs, inode_t *in, uint64_t off, const void *src, size_t len) {
    if (len == 0) return 0;
    if (off + len < off) return BRKF_EFBIG;
    const uint8_t *data = (const uint8_t *)src;
    uint8_t *buf = bnew(fs);
    if (!buf) return BRKF_ENOMEM;
    size_t done = 0;
    int r = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t boff = (uint32_t)(pos % BRKF_BLOCK_SIZE);
        size_t chunk = BRKF_BLOCK_SIZE - boff;
        if (chunk > len - done) chunk = len - done;
        uint32_t phys; int dirty = 0;
        if ((r = bmap(fs, in, pos / BRKF_BLOCK_SIZE, 1, &phys, &dirty)) != 0) break;
        if (chunk == BRKF_BLOCK_SIZE) {
            memcpy(buf, data + done, chunk);
        } else {
            if ((r = bread(fs, phys, buf)) != 0) break;
            memcpy(buf + boff, data + done, chunk);
        }
        if ((r = bwrite(fs, phys, buf)) != 0) break;
        done += chunk;
        if (off + done > in->size) in->size = off + done;    /* grow as we go: a failure keeps what was written */
    }
    bdel(fs, buf);
    if (done > 0) in->mtime = in->ctime = now_ns(fs);
    return done > 0 ? (int64_t)done : (r != 0 ? r : 0);
}

static int file_truncate(brkf_t *fs, inode_t *in, uint64_t size) {
    int r = 0;
    if (size < in->size) {
        uint64_t keep = (size + BRKF_BLOCK_SIZE - 1) / BRKF_BLOCK_SIZE;
        if ((r = free_blocks_from(fs, in, keep)) != 0) return r;
        uint32_t tail = (uint32_t)(size % BRKF_BLOCK_SIZE);
        if (tail) {                                           /* zero the rest of the last block */
            uint32_t phys; int dirty = 0;
            if ((r = bmap(fs, in, size / BRKF_BLOCK_SIZE, 0, &phys, &dirty)) != 0) return r;
            if (phys) {
                uint8_t *buf = bnew(fs);
                if (!buf) return BRKF_ENOMEM;
                if ((r = bread(fs, phys, buf)) == 0) {
                    memset(buf + tail, 0, BRKF_BLOCK_SIZE - tail);
                    r = bwrite(fs, phys, buf);
                }
                bdel(fs, buf);
                if (r != 0) return r;
            }
        }
    }
    in->size = size;
    in->mtime = in->ctime = now_ns(fs);
    return 0;
}

/* ---------------------------------------------------------------- directories */

#define DENT_MIN 8

/* Visit every slot of a directory block; returns via pointers. Entries are
 * validated so a damaged block cannot send us outside the buffer. */
static int dent_ok(const uint8_t *blk, uint32_t off) {
    if (off + DENT_MIN > BRKF_BLOCK_SIZE) return 0;
    const dent_t *e = (const dent_t *)(blk + off);
    if (e->rec_len < DENT_MIN || (e->rec_len & 3) || off + e->rec_len > BRKF_BLOCK_SIZE) return 0;
    if (e->inode && DENT_MIN + (uint32_t)e->name_len > e->rec_len) return 0;
    return 1;
}

static int dir_get(brkf_t *fs, uint32_t dir, inode_t *din) {
    int r = iget(fs, dir, din);
    if (r != 0) return r;
    if (!BRKF_S_ISDIR(din->mode)) return BRKF_ENOTDIR;
    return 0;
}

static int dir_find(brkf_t *fs, inode_t *din, const char *name, uint32_t *ino, uint8_t *type) {
    size_t nlen = strlen(name);
    uint8_t *buf = bnew(fs);
    if (!buf) return BRKF_ENOMEM;
    int r = BRKF_ENOENT;
    uint64_t nblocks = din->size / BRKF_BLOCK_SIZE;
    for (uint64_t lbn = 0; lbn < nblocks && r == BRKF_ENOENT; lbn++) {
        uint32_t phys; int dirty = 0;
        int e = bmap(fs, din, lbn, 0, &phys, &dirty);
        if (e != 0) { r = e; break; }
        if (!phys) continue;
        if ((e = bread(fs, phys, buf)) != 0) { r = e; break; }
        for (uint32_t off = 0; off < BRKF_BLOCK_SIZE; ) {
            if (!dent_ok(buf, off)) { r = BRKF_EUCLEAN; break; }
            const dent_t *d = (const dent_t *)(buf + off);
            if (d->inode && d->name_len == nlen && memcmp(d->name, name, nlen) == 0) {
                *ino = d->inode;
                if (type) *type = d->file_type;
                r = 0;
                break;
            }
            off += d->rec_len;
        }
    }
    bdel(fs, buf);
    return r;
}

static void touch(inode_t *in, uint64_t t) { in->mtime = in->ctime = t; }

/* Add an entry to a directory (the entry must not exist yet) */
static int dir_add(brkf_t *fs, uint32_t dir, const char *name, uint32_t ino, uint8_t type) {
    inode_t din;
    int r = dir_get(fs, dir, &din);
    if (r != 0) return r;
    size_t nlen = strlen(name);
    uint32_t need = (uint32_t)align4(DENT_MIN + nlen);
    uint8_t *buf = bnew(fs);
    if (!buf) return BRKF_ENOMEM;
    uint64_t nblocks = din.size / BRKF_BLOCK_SIZE;
    int placed = 0;

    for (uint64_t lbn = 0; lbn < nblocks && !placed; lbn++) {
        uint32_t phys; int dirty = 0;
        if ((r = bmap(fs, &din, lbn, 0, &phys, &dirty)) != 0) goto out;
        if (!phys) continue;
        if ((r = bread(fs, phys, buf)) != 0) goto out;
        for (uint32_t off = 0; off < BRKF_BLOCK_SIZE; ) {
            if (!dent_ok(buf, off)) { r = BRKF_EUCLEAN; goto out; }
            dent_t *d = (dent_t *)(buf + off);
            uint32_t used = d->inode ? (uint32_t)align4(DENT_MIN + d->name_len) : 0;
            if (d->rec_len - used >= need) {
                dent_t *n = d;
                if (d->inode) {                               /* split off the unused tail */
                    n = (dent_t *)(buf + off + used);
                    n->rec_len = (uint16_t)(d->rec_len - used);
                    d->rec_len = (uint16_t)used;
                }
                n->inode = ino; n->name_len = (uint8_t)nlen; n->file_type = type;
                memcpy(n->name, name, nlen);
                if ((r = bwrite(fs, phys, buf)) != 0) goto out;
                placed = 1;
                break;
            }
            off += d->rec_len;
        }
    }
    if (!placed) {                                            /* directory is full: add a block */
        uint32_t phys; int dirty = 0;
        if ((r = bmap(fs, &din, nblocks, 1, &phys, &dirty)) != 0) goto out;
        memset(buf, 0, BRKF_BLOCK_SIZE);
        dent_t *n = (dent_t *)buf;
        n->inode = ino; n->rec_len = BRKF_BLOCK_SIZE; n->name_len = (uint8_t)nlen; n->file_type = type;
        memcpy(n->name, name, nlen);
        if ((r = bwrite(fs, phys, buf)) != 0) goto out;
        din.size += BRKF_BLOCK_SIZE;
    }
    touch(&din, now_ns(fs));
    r = iput(fs, dir, &din);
out:
    bdel(fs, buf);
    return r;
}

/* Remove an entry; reports what it pointed to */
static int dir_remove(brkf_t *fs, uint32_t dir, const char *name, uint32_t *ino, uint8_t *type) {
    inode_t din;
    int r = dir_get(fs, dir, &din);
    if (r != 0) return r;
    size_t nlen = strlen(name);
    uint8_t *buf = bnew(fs);
    if (!buf) return BRKF_ENOMEM;
    uint64_t nblocks = din.size / BRKF_BLOCK_SIZE;
    r = BRKF_ENOENT;

    for (uint64_t lbn = 0; lbn < nblocks && r == BRKF_ENOENT; lbn++) {
        uint32_t phys; int dirty = 0;
        int e = bmap(fs, &din, lbn, 0, &phys, &dirty);
        if (e != 0) { r = e; break; }
        if (!phys) continue;
        if ((e = bread(fs, phys, buf)) != 0) { r = e; break; }
        dent_t *prev = NULL;
        for (uint32_t off = 0; off < BRKF_BLOCK_SIZE; ) {
            if (!dent_ok(buf, off)) { r = BRKF_EUCLEAN; break; }
            dent_t *d = (dent_t *)(buf + off);
            if (d->inode && d->name_len == nlen && memcmp(d->name, name, nlen) == 0) {
                if (ino) *ino = d->inode;
                if (type) *type = d->file_type;
                if (prev) prev->rec_len = (uint16_t)(prev->rec_len + d->rec_len);   /* merge into the previous entry */
                else d->inode = 0;
                r = bwrite(fs, phys, buf);
                if (r == 0) { touch(&din, now_ns(fs)); r = iput(fs, dir, &din); }
                break;
            }
            prev = d;
            off += d->rec_len;
        }
    }
    bdel(fs, buf);
    return r;
}

/* Only "." and ".." left? */
static int dir_is_empty(brkf_t *fs, uint32_t dir) {
    inode_t din;
    int r = dir_get(fs, dir, &din);
    if (r != 0) return r;
    uint8_t *buf = bnew(fs);
    if (!buf) return BRKF_ENOMEM;
    int result = 1;
    uint64_t nblocks = din.size / BRKF_BLOCK_SIZE;
    for (uint64_t lbn = 0; lbn < nblocks && result == 1; lbn++) {
        uint32_t phys; int dirty = 0;
        if ((r = bmap(fs, &din, lbn, 0, &phys, &dirty)) != 0) { result = r; break; }
        if (!phys) continue;
        if ((r = bread(fs, phys, buf)) != 0) { result = r; break; }
        for (uint32_t off = 0; off < BRKF_BLOCK_SIZE; ) {
            if (!dent_ok(buf, off)) { result = BRKF_EUCLEAN; break; }
            const dent_t *d = (const dent_t *)(buf + off);
            if (d->inode && !((d->name_len == 1 && d->name[0] == '.') ||
                              (d->name_len == 2 && d->name[0] == '.' && d->name[1] == '.'))) {
                result = 0;
                break;
            }
            off += d->rec_len;
        }
    }
    bdel(fs, buf);
    return result;
}

/* Point a directory's ".." entry at a new parent */
static int dir_set_parent(brkf_t *fs, uint32_t dir, uint32_t parent) {
    inode_t din;
    int r = dir_get(fs, dir, &din);
    if (r != 0) return r;
    uint32_t phys; int dirty = 0;
    if ((r = bmap(fs, &din, 0, 0, &phys, &dirty)) != 0 || !phys) return r ? r : BRKF_EUCLEAN;
    uint8_t *buf = bnew(fs);
    if (!buf) return BRKF_ENOMEM;
    if ((r = bread(fs, phys, buf)) == 0) {
        r = BRKF_EUCLEAN;
        for (uint32_t off = 0; off < BRKF_BLOCK_SIZE && dent_ok(buf, off); ) {
            dent_t *d = (dent_t *)(buf + off);
            if (d->inode && d->name_len == 2 && d->name[0] == '.' && d->name[1] == '.') {
                d->inode = parent;
                r = bwrite(fs, phys, buf);
                break;
            }
            off += d->rec_len;
        }
    }
    bdel(fs, buf);
    return r;
}

/* Adjust a link count (and touch ctime/mtime) */
static int adj_nlink(brkf_t *fs, uint32_t ino, int delta, int touch_dir) {
    inode_t in;
    int r = iget(fs, ino, &in);
    if (r != 0) return r;
    if (delta < 0 && in.nlink < (uint32_t)(-delta)) in.nlink = 0; else in.nlink = (uint32_t)((int64_t)in.nlink + delta);
    if (touch_dir) touch(&in, now_ns(fs)); else in.ctime = now_ns(fs);
    return iput(fs, ino, &in);
}

/* Release an inode and everything it owns */
static int destroy_inode(brkf_t *fs, uint32_t ino) {
    inode_t in;
    int r = iget(fs, ino, &in);
    if (r != 0) return r;
    if (!(in.flags & IF_INLINE_SYMLINK)) {
        if ((r = free_blocks_from(fs, &in, 0)) != 0) return r;
    }
    return ifree(fs, ino);
}

/* Build the first block of a directory: "." and ".." */
static int dir_init_block(brkf_t *fs, inode_t *in, uint32_t self, uint32_t parent) {
    uint32_t phys; int dirty = 0;
    int r = bmap(fs, in, 0, 1, &phys, &dirty);
    if (r != 0) return r;
    uint8_t *buf = bnew(fs);
    if (!buf) return BRKF_ENOMEM;
    memset(buf, 0, BRKF_BLOCK_SIZE);
    dent_t *d = (dent_t *)buf;
    d->inode = self; d->rec_len = 12; d->name_len = 1; d->file_type = BRKF_FT_DIR; d->name[0] = '.';
    d = (dent_t *)(buf + 12);
    d->inode = parent; d->rec_len = BRKF_BLOCK_SIZE - 12; d->name_len = 2; d->file_type = BRKF_FT_DIR;
    d->name[0] = '.'; d->name[1] = '.';
    r = bwrite(fs, phys, buf);
    bdel(fs, buf);
    in->size = BRKF_BLOCK_SIZE;
    return r;
}

/* ---------------------------------------------------------------- public: names */

int brkf_lookup(brkf_t *fs, uint32_t dir, const char *name, uint32_t *ino) {
    inode_t din;
    int r = dir_get(fs, dir, &din);
    if (r != 0) return r;
    if (strlen(name) > BRKF_NAME_MAX) return BRKF_ENAMETOOLONG;
    return dir_find(fs, &din, name, ino, NULL);
}

static void to_stat(uint32_t ino, const inode_t *in, brkf_stat_t *st) {
    st->ino = ino; st->mode = in->mode; st->uid = in->uid; st->gid = in->gid; st->nlink = in->nlink;
    st->size = in->size; st->blocks = in->blocks;
    st->atime_ns = in->atime; st->mtime_ns = in->mtime; st->ctime_ns = in->ctime; st->crtime_ns = in->crtime;
}

int brkf_getattr(brkf_t *fs, uint32_t ino, brkf_stat_t *st) {
    inode_t in;
    int r = iget(fs, ino, &in);
    if (r == 0) to_stat(ino, &in, st);
    return r;
}

int brkf_setattr(brkf_t *fs, uint32_t ino, const brkf_stat_t *s, uint32_t valid) {
    inode_t in;
    int r = iget(fs, ino, &in);
    if (r != 0) return r;
    if (valid & BRKF_SET_MODE)  in.mode = (uint16_t)((in.mode & BRKF_S_IFMT) | (s->mode & 07777));
    if (valid & BRKF_SET_UID)   in.uid = s->uid;
    if (valid & BRKF_SET_GID)   in.gid = s->gid;
    if (valid & BRKF_SET_ATIME) in.atime = s->atime_ns;
    if (valid & BRKF_SET_MTIME) in.mtime = s->mtime_ns;
    in.ctime = now_ns(fs);
    return iput(fs, ino, &in);
}

int brkf_create(brkf_t *fs, uint32_t dir, const char *name, uint16_t mode, uint32_t uid, uint32_t gid, uint32_t *ino) {
    int r = valid_name(name);
    if (r != 0) return r;
    if (is_dots(name)) return BRKF_EEXIST;
    inode_t din, in;
    if ((r = dir_get(fs, dir, &din)) != 0) return r;
    uint32_t existing;
    if ((r = dir_find(fs, &din, name, &existing, NULL)) == 0) return BRKF_EEXIST;
    if (r != BRKF_ENOENT) return r;

    uint32_t n;
    if ((r = ialloc(fs, (uint16_t)(BRKF_S_IFREG | (mode & 07777)), uid, gid, &n, &in)) != 0) return r;
    if ((r = dir_add(fs, dir, name, n, BRKF_FT_REG)) != 0) { ifree(fs, n); return r; }
    if (ino) *ino = n;
    return 0;
}

int brkf_mkdir(brkf_t *fs, uint32_t dir, const char *name, uint16_t mode, uint32_t uid, uint32_t gid, uint32_t *ino) {
    int r = valid_name(name);
    if (r != 0) return r;
    if (is_dots(name)) return BRKF_EEXIST;
    inode_t din, in;
    if ((r = dir_get(fs, dir, &din)) != 0) return r;
    if (din.nlink >= MAX_LINKS) return BRKF_EMLINK;
    uint32_t existing;
    if ((r = dir_find(fs, &din, name, &existing, NULL)) == 0) return BRKF_EEXIST;
    if (r != BRKF_ENOENT) return r;

    uint32_t n;
    if ((r = ialloc(fs, (uint16_t)(BRKF_S_IFDIR | (mode & 07777)), uid, gid, &n, &in)) != 0) return r;
    in.nlink = 2;                                             /* its entry in the parent + "." */
    if ((r = dir_init_block(fs, &in, n, dir)) == 0) r = iput(fs, n, &in);
    if (r != 0) { destroy_inode(fs, n); return r; }
    if ((r = dir_add(fs, dir, name, n, BRKF_FT_DIR)) != 0) { destroy_inode(fs, n); return r; }
    adj_nlink(fs, dir, +1, 1);                                /* the new ".." points at the parent */
    if (ino) *ino = n;
    return 0;
}

int brkf_symlink(brkf_t *fs, uint32_t dir, const char *name, const char *target, uint32_t uid, uint32_t gid, uint32_t *ino) {
    int r = valid_name(name);
    if (r != 0) return r;
    if (is_dots(name)) return BRKF_EEXIST;
    size_t tlen = strlen(target);
    if (tlen == 0) return BRKF_EINVAL;
    if (tlen > BRKF_SYMLINK_MAX) return BRKF_ENAMETOOLONG;
    inode_t din, in;
    if ((r = dir_get(fs, dir, &din)) != 0) return r;
    uint32_t existing;
    if ((r = dir_find(fs, &din, name, &existing, NULL)) == 0) return BRKF_EEXIST;
    if (r != BRKF_ENOENT) return r;

    uint32_t n;
    if ((r = ialloc(fs, (uint16_t)(BRKF_S_IFLNK | 0777), uid, gid, &n, &in)) != 0) return r;
    if (tlen <= INLINE_MAX) {
        memcpy(in.direct, target, tlen);                      /* stored in the pointer area */
        in.flags |= IF_INLINE_SYMLINK;
        in.size = tlen;
    } else {
        int64_t w = file_write(fs, &in, 0, target, tlen);
        if (w < 0 || (size_t)w != tlen) { destroy_inode(fs, n); return w < 0 ? (int)w : BRKF_EIO; }
    }
    if ((r = iput(fs, n, &in)) != 0 || (r = dir_add(fs, dir, name, n, BRKF_FT_LNK)) != 0) {
        destroy_inode(fs, n);
        return r;
    }
    if (ino) *ino = n;
    return 0;
}

int brkf_readlink(brkf_t *fs, uint32_t ino, char *buf, size_t cap) {
    inode_t in;
    int r = iget(fs, ino, &in);
    if (r != 0) return r;
    if (!BRKF_S_ISLNK(in.mode)) return BRKF_EINVAL;
    size_t len = (size_t)in.size < cap ? (size_t)in.size : cap;
    if (in.flags & IF_INLINE_SYMLINK) { memcpy(buf, in.direct, len); return (int)len; }
    int64_t n = file_read(fs, &in, 0, buf, len);
    return n < 0 ? (int)n : (int)n;
}

int brkf_link(brkf_t *fs, uint32_t dir, const char *name, uint32_t target) {
    int r = valid_name(name);
    if (r != 0) return r;
    if (is_dots(name)) return BRKF_EEXIST;
    inode_t din, tin;
    if ((r = dir_get(fs, dir, &din)) != 0) return r;
    if ((r = iget(fs, target, &tin)) != 0) return r;
    if (BRKF_S_ISDIR(tin.mode)) return BRKF_EPERM;           /* no hard links to directories */
    if (tin.nlink >= MAX_LINKS) return BRKF_EMLINK;
    uint32_t existing;
    if ((r = dir_find(fs, &din, name, &existing, NULL)) == 0) return BRKF_EEXIST;
    if (r != BRKF_ENOENT) return r;

    uint8_t ft = BRKF_S_ISLNK(tin.mode) ? BRKF_FT_LNK : BRKF_FT_REG;
    tin.nlink++;
    tin.ctime = now_ns(fs);
    if ((r = iput(fs, target, &tin)) != 0) return r;         /* count first: a crash leaks a link, never loses data */
    if ((r = dir_add(fs, dir, name, target, ft)) != 0) { adj_nlink(fs, target, -1, 0); return r; }
    return 0;
}

int brkf_unlink(brkf_t *fs, uint32_t dir, const char *name) {
    int r = valid_name(name);
    if (r != 0) return r;
    if (is_dots(name)) return BRKF_EINVAL;
    inode_t din, in;
    if ((r = dir_get(fs, dir, &din)) != 0) return r;
    uint32_t ino;
    if ((r = dir_find(fs, &din, name, &ino, NULL)) != 0) return r;
    if ((r = iget(fs, ino, &in)) != 0) return r;
    if (BRKF_S_ISDIR(in.mode)) return BRKF_EISDIR;
    if ((r = dir_remove(fs, dir, name, NULL, NULL)) != 0) return r;      /* entry first, then the inode */
    if (in.nlink <= 1) return destroy_inode(fs, ino);
    in.nlink--;
    in.ctime = now_ns(fs);
    return iput(fs, ino, &in);
}

int brkf_rmdir(brkf_t *fs, uint32_t dir, const char *name) {
    int r = valid_name(name);
    if (r != 0) return r;
    if (is_dots(name)) return BRKF_EINVAL;
    inode_t din, in;
    if ((r = dir_get(fs, dir, &din)) != 0) return r;
    uint32_t ino;
    if ((r = dir_find(fs, &din, name, &ino, NULL)) != 0) return r;
    if ((r = iget(fs, ino, &in)) != 0) return r;
    if (!BRKF_S_ISDIR(in.mode)) return BRKF_ENOTDIR;
    if (ino == BRKF_ROOT_INO) return BRKF_EPERM;
    int empty = dir_is_empty(fs, ino);
    if (empty < 0) return empty;
    if (!empty) return BRKF_ENOTEMPTY;
    if ((r = dir_remove(fs, dir, name, NULL, NULL)) != 0) return r;
    adj_nlink(fs, dir, -1, 1);                                /* its ".." is gone */
    return destroy_inode(fs, ino);
}

/* Is `maybe_child` inside the tree below `ancestor`? (walks ".." upwards) */
static int is_inside(brkf_t *fs, uint32_t maybe_child, uint32_t ancestor) {
    uint32_t cur = maybe_child;
    for (int guard = 0; guard < 4096; guard++) {
        if (cur == ancestor) return 1;
        if (cur == BRKF_ROOT_INO) return 0;
        inode_t in;
        if (dir_get(fs, cur, &in) != 0) return 0;
        uint32_t parent;
        if (dir_find(fs, &in, "..", &parent, NULL) != 0 || parent == cur) return 0;
        cur = parent;
    }
    return 1;                                                 /* a loop in the tree: refuse */
}

int brkf_rename(brkf_t *fs, uint32_t odir, const char *oname, uint32_t ndir, const char *nname) {
    int r;
    if ((r = valid_name(oname)) != 0 || (r = valid_name(nname)) != 0) return r;
    if (is_dots(oname) || is_dots(nname)) return BRKF_EINVAL;
    if (odir == ndir && strcmp(oname, nname) == 0) return 0;

    inode_t od, nd, src;
    if ((r = dir_get(fs, odir, &od)) != 0) return r;
    if ((r = dir_get(fs, ndir, &nd)) != 0) return r;
    uint32_t sino; uint8_t stype;
    if ((r = dir_find(fs, &od, oname, &sino, &stype)) != 0) return r;
    if ((r = iget(fs, sino, &src)) != 0) return r;
    int src_dir = BRKF_S_ISDIR(src.mode);
    if (src_dir && is_inside(fs, ndir, sino)) return BRKF_EINVAL;     /* a directory into itself */

    uint32_t dino; uint8_t dtype;
    r = dir_find(fs, &nd, nname, &dino, &dtype);
    if (r == 0) {
        if (dino == sino) return 0;                           /* same file under two names: nothing to do */
        inode_t dst;
        if ((r = iget(fs, dino, &dst)) != 0) return r;
        if (BRKF_S_ISDIR(dst.mode)) {
            if (!src_dir) return BRKF_EISDIR;
            int empty = dir_is_empty(fs, dino);
            if (empty < 0) return empty;
            if (!empty) return BRKF_ENOTEMPTY;
            if ((r = dir_remove(fs, ndir, nname, NULL, NULL)) != 0) return r;
            adj_nlink(fs, ndir, -1, 1);
            if ((r = destroy_inode(fs, dino)) != 0) return r;
        } else {
            if (src_dir) return BRKF_ENOTDIR;
            if ((r = dir_remove(fs, ndir, nname, NULL, NULL)) != 0) return r;
            if (dst.nlink <= 1) { if ((r = destroy_inode(fs, dino)) != 0) return r; }
            else adj_nlink(fs, dino, -1, 0);
        }
    } else if (r != BRKF_ENOENT) {
        return r;
    }

    if ((r = dir_add(fs, ndir, nname, sino, stype)) != 0) return r;     /* new name first ... */
    if ((r = dir_remove(fs, odir, oname, NULL, NULL)) != 0) return r;   /* ... then drop the old one */
    if (src_dir && odir != ndir) {
        if ((r = dir_set_parent(fs, sino, ndir)) != 0) return r;
        adj_nlink(fs, odir, -1, 1);
        adj_nlink(fs, ndir, +1, 1);
    }
    adj_nlink(fs, sino, 0, 0);                                /* ctime */
    return 0;
}

/* ---------------------------------------------------------------- public: data */

int64_t brkf_read(brkf_t *fs, uint32_t ino, uint64_t off, void *buf, size_t len) {
    inode_t in;
    int r = iget(fs, ino, &in);
    if (r != 0) return r;
    if (BRKF_S_ISDIR(in.mode)) return BRKF_EISDIR;
    return file_read(fs, &in, off, buf, len);
}

int64_t brkf_write(brkf_t *fs, uint32_t ino, uint64_t off, const void *buf, size_t len) {
    inode_t in;
    int r = iget(fs, ino, &in);
    if (r != 0) return r;
    if (BRKF_S_ISDIR(in.mode)) return BRKF_EISDIR;
    if (!BRKF_S_ISREG(in.mode)) return BRKF_EINVAL;
    int64_t w = file_write(fs, &in, off, buf, len);
    int s = iput(fs, ino, &in);
    return s != 0 ? s : w;
}

int brkf_truncate(brkf_t *fs, uint32_t ino, uint64_t size) {
    inode_t in;
    int r = iget(fs, ino, &in);
    if (r != 0) return r;
    if (BRKF_S_ISDIR(in.mode)) return BRKF_EISDIR;
    if (!BRKF_S_ISREG(in.mode)) return BRKF_EINVAL;
    if (size > (uint64_t)(BRKF_NDIRECT + PTRS + pow_p(2) + pow_p(3)) * BRKF_BLOCK_SIZE) return BRKF_EFBIG;
    r = file_truncate(fs, &in, size);
    int s = iput(fs, ino, &in);
    return r != 0 ? r : s;
}

int brkf_readdir(brkf_t *fs, uint32_t dir, uint64_t *pos, brkf_dirent_t *ent) {
    inode_t din;
    int r = dir_get(fs, dir, &din);
    if (r != 0) return r;
    uint8_t *buf = bnew(fs);
    if (!buf) return BRKF_ENOMEM;
    r = 0;
    uint64_t nblocks = din.size / BRKF_BLOCK_SIZE;
    for (uint64_t lbn = *pos / BRKF_BLOCK_SIZE; lbn < nblocks && r == 0; lbn++) {
        uint32_t phys; int dirty = 0;
        if ((r = bmap(fs, &din, lbn, 0, &phys, &dirty)) != 0) break;
        if (!phys) { *pos = (lbn + 1) * BRKF_BLOCK_SIZE; continue; }
        if ((r = bread(fs, phys, buf)) != 0) break;
        uint32_t off = (lbn == *pos / BRKF_BLOCK_SIZE) ? (uint32_t)(*pos % BRKF_BLOCK_SIZE) : 0;
        /* entries start only at offsets reachable from 0; walk from the beginning to stay aligned */
        uint32_t walk = 0;
        while (walk < BRKF_BLOCK_SIZE) {
            if (!dent_ok(buf, walk)) { r = BRKF_EUCLEAN; break; }
            const dent_t *d = (const dent_t *)(buf + walk);
            if (walk >= off && d->inode) {
                ent->ino = d->inode; ent->type = d->file_type;
                memcpy(ent->name, d->name, d->name_len);
                ent->name[d->name_len] = '\0';
                *pos = lbn * BRKF_BLOCK_SIZE + walk + d->rec_len;
                bdel(fs, buf);
                return 1;
            }
            walk += d->rec_len;
        }
        if (r == 0) *pos = (lbn + 1) * BRKF_BLOCK_SIZE;
    }
    bdel(fs, buf);
    return r;
}

/* ---------------------------------------------------------------- paths */

#define MAX_SYMLINK_DEPTH 16

static int walk_path(brkf_t *fs, uint32_t start, const char *path, int follow_last, int *links, uint32_t *out) {
    uint32_t cur = (path[0] == '/') ? BRKF_ROOT_INO : start;
    const char *p = path;
    char comp[BRKF_NAME_MAX + 1];

    for (;;) {
        while (*p == '/') p++;
        if (!*p) break;
        size_t n = 0;
        while (p[n] && p[n] != '/') n++;
        if (n > BRKF_NAME_MAX) return BRKF_ENAMETOOLONG;
        memcpy(comp, p, n);
        comp[n] = '\0';
        p += n;
        const char *q = p;
        while (*q == '/') q++;
        int last = (*q == '\0');

        uint32_t child;
        int r = brkf_lookup(fs, cur, comp, &child);       /* fails with ENOTDIR if cur is not a directory */
        if (r != 0) return r;

        inode_t in;
        if ((r = iget(fs, child, &in)) != 0) return r;
        if (BRKF_S_ISLNK(in.mode) && (!last || follow_last)) {
            if (++*links > MAX_SYMLINK_DEPTH) return BRKF_ELOOP;
            char *target = (char *)bnew(fs);              /* heap, not stack: this recurses and the kernel stack is small */
            if (!target) return BRKF_ENOMEM;
            int tl = brkf_readlink(fs, child, target, BRKF_SYMLINK_MAX);
            if (tl < 0) { bdel(fs, target); return tl; }
            target[tl] = '\0';
            r = walk_path(fs, cur, target, 1, links, &child);
            bdel(fs, target);
            if (r != 0) return r;
        }
        cur = child;
        if (!last) {
            if ((r = iget(fs, cur, &in)) != 0) return r;
            if (!BRKF_S_ISDIR(in.mode)) return BRKF_ENOTDIR;
        }
    }
    *out = cur;
    return 0;
}

int brkf_resolve(brkf_t *fs, uint32_t cwd, const char *path, int follow_last, uint32_t *ino) {
    if (!path || !*path) return BRKF_ENOENT;
    int links = 0;
    return walk_path(fs, cwd, path, follow_last, &links, ino);
}

int brkf_resolve_parent(brkf_t *fs, uint32_t cwd, const char *path, uint32_t *dir, char *name) {
    if (!path || !*path) return BRKF_ENOENT;
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;            /* ignore trailing slashes */
    size_t slash = len;
    while (slash > 0 && path[slash - 1] != '/') slash--;
    size_t nlen = len - slash;
    if (nlen == 0) return BRKF_EINVAL;                        /* "/" has no parent entry */
    if (nlen > BRKF_NAME_MAX) return BRKF_ENAMETOOLONG;
    memcpy(name, path + slash, nlen);
    name[nlen] = '\0';

    char *dirpath = (char *)bnew(fs);
    if (!dirpath) return BRKF_ENOMEM;
    if (slash == 0) { dirpath[0] = '.'; dirpath[1] = '\0'; }
    else {
        size_t dl = slash;
        while (dl > 1 && path[dl - 1] == '/') dl--;
        if (dl > BRKF_SYMLINK_MAX) { bdel(fs, dirpath); return BRKF_ENAMETOOLONG; }
        memcpy(dirpath, path, dl);
        dirpath[dl] = '\0';
    }
    uint32_t d;
    int links = 0;
    int r = walk_path(fs, cwd, dirpath, 1, &links, &d);
    bdel(fs, dirpath);
    if (r != 0) return r;
    inode_t in;
    if ((r = iget(fs, d, &in)) != 0) return r;
    if (!BRKF_S_ISDIR(in.mode)) return BRKF_ENOTDIR;
    *dir = d;
    return 0;
}

/* ---------------------------------------------------------------- mount / mkfs */

/* mode: 0 = open for checking (no dirty mark), 1 = read-write mount, 2 = read-only mount */
static int fs_open(const brkf_io_t *io, brkf_t **out, int mode) {
    int set_dirty = (mode == 1);
    if (!io || !io->read_block || !io->write_block || !io->alloc || !io->release) return BRKF_EINVAL;
    brkf_t *fs = (brkf_t *)io->alloc(io->ctx, sizeof(brkf_t));
    if (!fs) return BRKF_ENOMEM;
    memset(fs, 0, sizeof(*fs));
    fs->io = *io;

    /* The superblock is in block 1, with a backup in the last block. We do not
     * know the size yet, so the first read goes straight to the device. */
    uint8_t *buf = (uint8_t *)io->alloc(io->ctx, BRKF_BLOCK_SIZE);
    if (!buf) { io->release(io->ctx, fs); return BRKF_ENOMEM; }
    int r = BRKF_EUCLEAN;
    if (io->read_block(io->ctx, SB_BLOCK, buf) == 0) {
        memcpy(&fs->sb, buf, sizeof(sb_t));
        if (fs->sb.magic == BRKF_MAGIC && brkf_crc32(&fs->sb, sizeof(sb_t) - 4) == fs->sb.crc32) r = 0;
    }
    if (r != 0 && fs->sb.magic == BRKF_MAGIC && fs->sb.block_count > 2) {   /* try the backup copy */
        if (io->read_block(io->ctx, fs->sb.block_count - 1, buf) == 0) {
            sb_t b; memcpy(&b, buf, sizeof(b));
            if (b.magic == BRKF_MAGIC && brkf_crc32(&b, sizeof(b) - 4) == b.crc32) { fs->sb = b; r = 0; }
        }
    }
    io->release(io->ctx, buf);

    sb_t *sb = &fs->sb;
    if (r == 0) {
        if (sb->magic != BRKF_MAGIC) r = BRKF_EINVAL;
        else if (sb->ver_major != BRKF_VERSION_MAJOR || sb->block_size != BRKF_BLOCK_SIZE ||
                 sb->inode_size != BRKF_INODE_SIZE || sb->incompat != 0) r = BRKF_EINVAL;
        else if (sb->block_count < 16 || sb->block_count > 0xFFFFFFF0ULL || sb->inode_count == 0 ||
                 sb->root_ino != BRKF_ROOT_INO || sb->data_start >= sb->block_count - 1 ||
                 sb->inode_table_start + sb->inode_table_blocks > sb->data_start ||
                 (uint64_t)sb->inode_table_blocks * INODES_PER_BLOCK < sb->inode_count) r = BRKF_EUCLEAN;
    } else if (r == BRKF_EUCLEAN && fs->sb.magic != BRKF_MAGIC) {
        r = BRKF_EINVAL;                                      /* not a BRKF volume at all */
    }
    if (r != 0) { io->release(io->ctx, fs); return r; }

    fs->block_count = sb->block_count;
    fs->inode_count = sb->inode_count;
    fs->blk_hint = sb->data_start;
    fs->readonly = (mode == 2);
    fs->cache_mem = (uint8_t *)io->alloc(io->ctx, (size_t)CACHE_SLOTS * BRKF_BLOCK_SIZE);
    if (fs->cache_mem) {
        for (int i = 0; i < CACHE_SLOTS; i++) fs->slots[i].data = fs->cache_mem + (size_t)i * BRKF_BLOCK_SIZE;
    }
    if (set_dirty) {
        sb->state = BRKF_STATE_DIRTY;
        sb->mount_count++;
        sb->last_mount_ns = now_ns(fs);
        fs->mark_dirty = 1;
        if ((r = sb_store(fs)) != 0) {
            if (fs->cache_mem) io->release(io->ctx, fs->cache_mem);
            io->release(io->ctx, fs);
            return r;
        }
    }
    *out = fs;
    return 0;
}

static void fs_close(brkf_t *fs) {
    brkf_io_t io = fs->io;
    if (fs->cache_mem) io.release(io.ctx, fs->cache_mem);
    io.release(io.ctx, fs);
}

int brkf_mount(brkf_t **out, const brkf_io_t *io) {
    return fs_open(io, out, 1);
}

int brkf_mount_ro(brkf_t **out, const brkf_io_t *io) {
    return fs_open(io, out, 2);
}

int brkf_sync(brkf_t *fs) {
    return (fs->sb_dirty && !fs->readonly) ? sb_store(fs) : 0;
}

int brkf_unmount(brkf_t *fs) {
    if (!fs) return BRKF_EINVAL;
    int r = 0;
    if (!fs->readonly) {
        fs->sb.state = BRKF_STATE_CLEAN;
        r = sb_store(fs);
    }
    fs_close(fs);
    return r;
}

int brkf_statfs(brkf_t *fs, brkf_statfs_t *out) {
    out->block_count = fs->sb.block_count;
    out->free_blocks = fs->sb.free_blocks;
    out->inode_count = fs->sb.inode_count;
    out->free_inodes = fs->sb.free_inodes;
    out->state = fs->sb.state;
    memcpy(out->label, fs->sb.label, 32);
    out->label[32] = '\0';
    memcpy(out->uuid, fs->sb.uuid, 16);
    return 0;
}

int brkf_mkfs(const brkf_io_t *io, uint64_t block_count, const char *label, uint32_t inode_count, const uint8_t *uuid) {
    if (!io || block_count < 64 || block_count > 0xFFFFFFF0ULL) return BRKF_EINVAL;
    if (inode_count == 0) inode_count = (uint32_t)(block_count / 4);
    if (inode_count < 64) inode_count = 64;
    inode_count = (inode_count + 15) & ~15u;                  /* whole inode-table blocks */

    sb_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = BRKF_MAGIC;
    sb.ver_major = BRKF_VERSION_MAJOR;
    sb.ver_minor = BRKF_VERSION_MINOR;
    sb.block_size = BRKF_BLOCK_SIZE;
    sb.inode_size = BRKF_INODE_SIZE;
    sb.block_count = block_count;
    sb.inode_count = inode_count;
    sb.root_ino = BRKF_ROOT_INO;
    sb.inode_bitmap_start = 2;
    sb.inode_bitmap_blocks = (inode_count + BITS_PER_BLOCK - 1) / BITS_PER_BLOCK;
    sb.block_bitmap_start = sb.inode_bitmap_start + sb.inode_bitmap_blocks;
    sb.block_bitmap_blocks = (uint32_t)((block_count + BITS_PER_BLOCK - 1) / BITS_PER_BLOCK);
    sb.inode_table_start = sb.block_bitmap_start + sb.block_bitmap_blocks;
    sb.inode_table_blocks = inode_count / INODES_PER_BLOCK;
    sb.data_start = sb.inode_table_start + sb.inode_table_blocks;
    if (sb.data_start + 4 >= block_count) return BRKF_ENOSPC;                 /* too small for its own metadata */
    sb.free_blocks = block_count - sb.data_start - 1;                         /* minus the backup superblock */
    sb.free_inodes = inode_count;
    sb.state = BRKF_STATE_DIRTY;
    if (uuid) memcpy(sb.uuid, uuid, 16);
    if (label) { size_t n = strlen(label); memcpy(sb.label, label, n > 31 ? 31 : n); }

    brkf_t *fs = (brkf_t *)io->alloc(io->ctx, sizeof(brkf_t));
    if (!fs) return BRKF_ENOMEM;
    memset(fs, 0, sizeof(*fs));
    fs->io = *io;
    fs->sb = sb;
    fs->block_count = block_count;
    fs->inode_count = inode_count;
    fs->blk_hint = sb.data_start;
    sb.created_ns = sb.last_mount_ns = now_ns(fs);
    fs->sb.created_ns = sb.created_ns;
    fs->sb.last_mount_ns = sb.last_mount_ns;

    uint8_t *buf = bnew(fs);
    if (!buf) { io->release(io->ctx, fs); return BRKF_ENOMEM; }
    int r = 0;

    /* Inode table: all free (zeros) */
    memset(buf, 0, BRKF_BLOCK_SIZE);
    for (uint64_t b = 0; b < sb.inode_table_blocks && r == 0; b++) r = bwrite(fs, sb.inode_table_start + b, buf);

    /* Inode bitmap: bits past the last inode are marked used */
    for (uint64_t b = 0; b < sb.inode_bitmap_blocks && r == 0; b++) {
        memset(buf, 0, BRKF_BLOCK_SIZE);
        for (uint64_t bit = 0; bit < BITS_PER_BLOCK; bit++) {
            if (b * BITS_PER_BLOCK + bit >= inode_count) buf[bit / 8] |= (uint8_t)(1u << (bit % 8));
        }
        r = bwrite(fs, sb.inode_bitmap_start + b, buf);
    }
    /* Block bitmap: metadata, the backup superblock and padding bits are used */
    for (uint64_t b = 0; b < sb.block_bitmap_blocks && r == 0; b++) {
        memset(buf, 0, BRKF_BLOCK_SIZE);
        for (uint64_t bit = 0; bit < BITS_PER_BLOCK; bit++) {
            uint64_t blk = b * BITS_PER_BLOCK + bit;
            if (blk < sb.data_start || blk >= block_count - 1) buf[bit / 8] |= (uint8_t)(1u << (bit % 8));
        }
        r = bwrite(fs, sb.block_bitmap_start + b, buf);
    }
    bdel(fs, buf);

    /* Root directory = inode 1 */
    if (r == 0) {
        inode_t root; uint32_t ino;
        if ((r = ialloc(fs, (uint16_t)(BRKF_S_IFDIR | 0755), 0, 0, &ino, &root)) == 0) {
            root.nlink = 2;
            if ((r = dir_init_block(fs, &root, ino, ino)) == 0) r = iput(fs, ino, &root);
            if (r == 0 && ino != BRKF_ROOT_INO) r = BRKF_EUCLEAN;
        }
    }
    if (r == 0) { fs->sb.state = BRKF_STATE_CLEAN; r = sb_store(fs); }
    io->release(io->ctx, fs);
    return r;
}

/* ---------------------------------------------------------------- fsck */

typedef struct {
    brkf_t *fs;
    brkf_report_fn report;
    void *rctx;
    brkf_fsck_result_t *res;
    int repair;
    uint32_t *counted;       /* directory entries pointing at each inode */
    uint32_t *parent_of;     /* the directory each directory was reached from */
    uint8_t  *seen;          /* inode reachable from the root */
    uint8_t  *blk_used;      /* block referenced by something */
    uint64_t  claimed;       /* blocks claimed by the inode being scanned */
} fck_t;

static inline int bit_get(const uint8_t *a, uint64_t i) { return (a[i >> 3] >> (i & 7)) & 1; }
static inline void bit_set(uint8_t *a, uint64_t i) { a[i >> 3] |= (uint8_t)(1u << (i & 7)); }

static void say(fck_t *f, const char *msg, uint64_t n, int is_problem) {
    if (is_problem) f->res->problems_found++;
    if (!f->report) return;
    char line[160];
    size_t len = strlen(msg);
    if (len > 120) len = 120;
    memcpy(line, msg, len);
    line[len++] = ' ';
    char digits[24]; int d = 0;
    uint64_t v = n;
    do { digits[d++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (d) line[len++] = digits[--d];
    line[len] = '\0';
    f->report(f->rctx, line);
}

static void claim(fck_t *f, uint32_t b, uint32_t ino) {
    brkf_t *fs = f->fs;
    if (b < fs->sb.data_start || b >= fs->block_count - 1) { say(f, "inode points outside the data area:", ino, 1); return; }
    if (bit_get(f->blk_used, b)) say(f, "block claimed twice, inode", ino, 1);
    bit_set(f->blk_used, b);
    f->claimed++;
}

static void claim_tree(fck_t *f, uint32_t table, int depth, uint32_t ino) {
    if (!table) return;
    claim(f, table, ino);
    if (table < f->fs->sb.data_start || table >= f->fs->block_count - 1) return;
    uint8_t *buf = bnew(f->fs);
    if (!buf) return;
    if (bread(f->fs, table, buf) == 0) {
        const uint32_t *tab = (const uint32_t *)buf;
        for (uint32_t i = 0; i < PTRS; i++) {
            if (!tab[i]) continue;
            if (depth == 1) claim(f, tab[i], ino); else claim_tree(f, tab[i], depth - 1, ino);
        }
    }
    bdel(f->fs, buf);
}

static void claim_inode(fck_t *f, uint32_t ino, const inode_t *in) {
    f->claimed = 0;
    if (in->flags & IF_INLINE_SYMLINK) return;
    for (int i = 0; i < BRKF_NDIRECT; i++) if (in->direct[i]) claim(f, in->direct[i], ino);
    claim_tree(f, in->indirect, 1, ino);
    claim_tree(f, in->dindirect, 2, ino);
    claim_tree(f, in->tindirect, 3, ino);
    if (f->claimed != in->blocks) {
        say(f, "block count wrong, inode", ino, 1);
        if (f->repair) {
            inode_t fix = *in;
            fix.blocks = f->claimed;
            if (iput(f->fs, ino, &fix) == 0) f->res->problems_fixed++;
        }
    }
}

int brkf_fsck(const brkf_io_t *io, int repair, brkf_report_fn report, void *rctx, brkf_fsck_result_t *result) {
    memset(result, 0, sizeof(*result));
    brkf_t *fs;
    fck_t f;
    memset(&f, 0, sizeof(f));
    f.report = report; f.rctx = rctx; f.res = result; f.repair = repair;

    int r = fs_open(io, &fs, 0);
    if (r != 0) {
        if (report) report(rctx, r == BRKF_EINVAL ? "not a BRKF volume (or unsupported version)" : "superblock is damaged");
        result->problems_found++;
        return r;
    }
    f.fs = fs;
    uint32_t nino = fs->inode_count;
    uint64_t nblk = fs->block_count;

    f.counted   = (uint32_t *)io->alloc(io->ctx, (size_t)(nino + 1) * 4);
    f.parent_of = (uint32_t *)io->alloc(io->ctx, (size_t)(nino + 1) * 4);
    uint32_t *queue = (uint32_t *)io->alloc(io->ctx, (size_t)(nino + 1) * 4);
    f.seen      = (uint8_t *)io->alloc(io->ctx, nino / 8 + 2);
    f.blk_used  = (uint8_t *)io->alloc(io->ctx, (size_t)(nblk / 8 + 2));
    if (!f.counted || !f.parent_of || !queue || !f.seen || !f.blk_used) {
        r = BRKF_ENOMEM;
        if (report) report(rctx, "not enough memory to check this volume");
        goto cleanup;
    }
    memset(f.counted, 0, (size_t)(nino + 1) * 4);
    memset(f.parent_of, 0, (size_t)(nino + 1) * 4);
    memset(f.seen, 0, nino / 8 + 2);
    memset(f.blk_used, 0, (size_t)(nblk / 8 + 2));

    if (fs->sb.state != BRKF_STATE_CLEAN) say(&f, "volume was not cleanly unmounted (state)", fs->sb.state, 0);
    for (uint64_t b = 0; b < fs->sb.data_start; b++) bit_set(f.blk_used, b);
    bit_set(f.blk_used, nblk - 1);

    /* Walk the tree from the root */
    uint32_t qh = 0, qt = 0;
    queue[qt++] = BRKF_ROOT_INO;
    bit_set(f.seen, BRKF_ROOT_INO - 1);
    f.parent_of[BRKF_ROOT_INO] = BRKF_ROOT_INO;
    inode_t in;
    uint8_t *buf = bnew(fs);
    if (!buf) { r = BRKF_ENOMEM; goto cleanup; }

    while (qh < qt) {
        uint32_t dir = queue[qh++];
        if ((r = iget(fs, dir, &in)) != 0 || !BRKF_S_ISDIR(in.mode)) {
            say(&f, "directory inode unreadable or not a directory:", dir, 1);
            r = 0;
            continue;
        }
        result->dirs++;
        claim_inode(&f, dir, &in);
        if (in.size % BRKF_BLOCK_SIZE) say(&f, "directory size is not a multiple of the block size, inode", dir, 1);

        uint64_t nb = in.size / BRKF_BLOCK_SIZE;
        for (uint64_t lbn = 0; lbn < nb; lbn++) {
            uint32_t phys; int dirty = 0;
            if (bmap(fs, &in, lbn, 0, &phys, &dirty) != 0 || !phys || bread(fs, phys, buf) != 0) {
                say(&f, "unreadable directory block, inode", dir, 1);
                continue;
            }
            for (uint32_t off = 0; off < BRKF_BLOCK_SIZE; ) {
                if (!dent_ok(buf, off)) { say(&f, "damaged directory entry, inode", dir, 1); break; }
                const dent_t *d = (const dent_t *)(buf + off);
                off += d->rec_len;
                if (!d->inode) continue;
                uint32_t t = d->inode;
                if (t > nino) { say(&f, "directory entry points to a bad inode, directory", dir, 1); continue; }
                f.counted[t]++;
                int dot = (d->name_len == 1 && d->name[0] == '.');
                int dotdot = (d->name_len == 2 && d->name[0] == '.' && d->name[1] == '.');
                if (dot) { if (t != dir) say(&f, "\".\" entry wrong in directory", dir, 1); continue; }
                if (dotdot) { if (t != f.parent_of[dir]) say(&f, "\"..\" entry wrong in directory", dir, 1); continue; }

                if (bit_get(f.seen, t - 1)) {
                    inode_t again;
                    if (iget(fs, t, &again) == 0 && BRKF_S_ISDIR(again.mode)) say(&f, "directory has two parents, inode", t, 1);
                    continue;
                }
                bit_set(f.seen, t - 1);
                inode_t child;
                if (iget(fs, t, &child) != 0) { say(&f, "inode unreadable or bad checksum:", t, 1); continue; }
                uint8_t want = BRKF_S_ISDIR(child.mode) ? BRKF_FT_DIR : BRKF_S_ISLNK(child.mode) ? BRKF_FT_LNK : BRKF_FT_REG;
                if (want != d->file_type) say(&f, "entry type disagrees with inode", t, 1);
                result->inodes_used++;
                if (BRKF_S_ISDIR(child.mode)) { f.parent_of[t] = dir; queue[qt++] = t; }
                else {
                    if (BRKF_S_ISLNK(child.mode)) result->symlinks++; else result->files++;
                    claim_inode(&f, t, &child);
                }
            }
        }
    }
    result->inodes_used++;                                    /* the root */
    bdel(fs, buf);
    r = 0;

    /* Link counts */
    uint64_t used_inodes = 0;
    for (uint32_t i = 1; i <= nino; i++) {
        if (!bit_get(f.seen, i - 1)) continue;
        used_inodes++;
        if (iget(fs, i, &in) != 0) continue;
        if (in.nlink != f.counted[i]) {
            say(&f, "link count wrong, inode", i, 1);
            if (repair) {
                in.nlink = f.counted[i];
                if (iput(fs, i, &in) == 0) result->problems_fixed++;
            }
        }
    }

    /* Inode bitmap */
    for (uint32_t i = 1; i <= nino; i++) {
        int bit = 0;
        if (bm_get(fs, fs->sb.inode_bitmap_start, i - 1, &bit) != 0) continue;
        int seen = bit_get(f.seen, i - 1);
        if (seen && !bit) {
            say(&f, "inode in use but marked free:", i, 1);
            if (repair && bm_put(fs, fs->sb.inode_bitmap_start, i - 1, 1) == 0) result->problems_fixed++;
        } else if (!seen && bit) {
            say(&f, "inode allocated but not reachable (leaked):", i, 1);
            if (repair) {
                uint64_t blk; uint32_t off;
                if (ino_loc(fs, i, &blk, &off) == 0) {
                    uint8_t *ib = bnew(fs);
                    if (ib) {
                        if (bread(fs, blk, ib) == 0) { memset(ib + off, 0, BRKF_INODE_SIZE); bwrite(fs, blk, ib); }
                        bdel(fs, ib);
                    }
                }
                if (bm_put(fs, fs->sb.inode_bitmap_start, i - 1, 0) == 0) result->problems_fixed++;
            }
        }
    }

    /* Block bitmap, and the free counters */
    uint64_t used_blocks = 0, bad_bits = 0;
    for (uint64_t b = 0; b < nblk; b++) {
        int bit = 0;
        if (bm_get(fs, fs->sb.block_bitmap_start, b, &bit) != 0) continue;
        int want = bit_get(f.blk_used, b);
        if (want) used_blocks++;
        if (bit != want) {
            bad_bits++;
            if (repair) bm_put(fs, fs->sb.block_bitmap_start, b, want);
        }
    }
    result->blocks_used = used_blocks;
    if (bad_bits) {
        say(&f, "block bitmap differs from the blocks actually in use, blocks:", bad_bits, 1);
        if (repair) result->problems_fixed++;
    }
    uint64_t free_blocks = nblk - used_blocks;
    uint32_t free_inodes = nino - (uint32_t)used_inodes;
    if (fs->sb.free_blocks != free_blocks || fs->sb.free_inodes != free_inodes) {
        say(&f, "free counters wrong; real free blocks:", free_blocks, 0);
    }
    if (repair) {
        fs->sb.free_blocks = free_blocks;
        fs->sb.free_inodes = free_inodes;
        fs->sb.state = BRKF_STATE_CLEAN;
        r = sb_store(fs);
    }

cleanup:
    if (f.counted)   io->release(io->ctx, f.counted);
    if (f.parent_of) io->release(io->ctx, f.parent_of);
    if (queue)       io->release(io->ctx, queue);
    if (f.seen)      io->release(io->ctx, f.seen);
    if (f.blk_used)  io->release(io->ctx, f.blk_used);
    fs_close(fs);
    return r < 0 ? r : (int)(result->problems_found - result->problems_fixed);
}
