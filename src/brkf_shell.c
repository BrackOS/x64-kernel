#include "brkf_shell.h"
#include "brkf_kernel.h"
#include "blockdev.h"
#include "sched.h"
#include "heap.h"
#include "string.h"
#include "vga.h"
#include "fat32.h"
#include "fs_shell.h"

#define CWD (sched_current()->cwd ? sched_current()->cwd : BRKF_ROOT_INO)

static void print_err(const char *what, int err) {
    vga_print("brkf: ");
    if (what && what[0]) { vga_print(what); vga_print(": "); }
    vga_print(kbrkf_strerror(err));
    vga_print("\n");
}

static void usage(void) {
    vga_print("BRKF - the BrackOS filesystem (second disk)\n"
              "  brkf info                    disk and volume status\n"
              "  brkf format yes [label]      ERASE the second disk and create a BRKF volume\n"
              "  brkf mount | umount | sync\n"
              "  brkf ls [path]   cat path   stat path   df   pwd   cd path\n"
              "  brkf write path text...      brkf append path text...\n"
              "  brkf mkdir path   rm path   rmdir path   mv old new\n"
              "  brkf ln [-s] target path     hard link, or symbolic link with -s\n"
              "  brkf import <fat32 file> <path>     copy a file from the FAT32 disk\n"
              "  brkf export <path> <fat32 file>     copy a file to the FAT32 disk\n"
              "  brkf fsck [repair]           check the volume (must be unmounted)\n"
              "Programs on the volume run with: run /bin/name   (or just: run name)\n");
}

/* Split off the next space-separated word; returns the rest (or NULL) */
static char *next_word(char **s) {
    char *p = *s;
    while (*p == ' ') p++;
    if (!*p) { *s = p; return NULL; }
    char *w = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = '\0';
    *s = p;
    return w;
}

static int need_mount(void) {
    if (kbrkf_mounted()) return 1;
    vga_print(kbrkf_available() ? "brkf: not mounted (try: brkf mount)\n" : "brkf: no second disk attached\n");
    return 0;
}

static void pad2(int v) {
    if (v < 10) vga_print("0");
    vga_print_int(v);
}

static void print_date(uint64_t ns) {
    uint64_t secs = ns / 1000000000ULL;
    int64_t days = (int64_t)(secs / 86400);
    uint32_t rem = (uint32_t)(secs % 86400);
    int64_t z = days + 719468;                          /* civil-from-days */
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    vga_print_int((int)y); vga_print("-");
    pad2((int)m); vga_print("-");
    pad2((int)d); vga_print(" ");
    pad2((int)(rem / 3600)); vga_print(":");
    pad2((int)((rem / 60) % 60));
}

static void print_mode(uint16_t mode) {
    char s[11];
    s[0] = BRKF_S_ISDIR(mode) ? 'd' : BRKF_S_ISLNK(mode) ? 'l' : '-';
    for (int i = 0; i < 9; i++) s[1 + i] = (mode & (0400 >> i)) ? "rwx"[i % 3] : '-';
    s[10] = '\0';
    vga_print(s);
}

static void cmd_ls(const char *path) {
    uint32_t ino;
    int r = brkf_resolve(kbrkf(), CWD, path[0] ? path : ".", 1, &ino);
    if (r != 0) { print_err(path, r); return; }
    brkf_stat_t st;
    brkf_getattr(kbrkf(), ino, &st);
    if (!BRKF_S_ISDIR(st.mode)) { print_mode(st.mode); vga_print(" "); vga_print_int((int)st.size); vga_print("  "); vga_print(path); vga_print("\n"); return; }
    uint64_t pos = 0;
    brkf_dirent_t e;
    int count = 0;
    while (brkf_readdir(kbrkf(), ino, &pos, &e) == 1) {
        if (!strcmp(e.name, ".") || !strcmp(e.name, "..")) continue;
        brkf_stat_t s;
        brkf_getattr(kbrkf(), e.ino, &s);
        print_mode(s.mode);
        vga_print("  ");
        vga_print_int((int)s.size);
        vga_print("\t");
        print_date(s.mtime_ns);
        vga_print("  ");
        vga_print(e.name);
        if (e.type == BRKF_FT_DIR) vga_print("/");
        vga_print("\n");
        count++;
    }
    if (count == 0) vga_print("(empty)\n");
}

static void cmd_cat(const char *path) {
    uint32_t ino;
    int r = brkf_resolve(kbrkf(), CWD, path, 1, &ino);
    if (r != 0) { print_err(path, r); return; }
    uint8_t *buf = (uint8_t *)kmalloc(1024);
    if (!buf) { print_err("", BRKF_ENOMEM); return; }
    uint64_t off = 0;
    int64_t n;
    while ((n = brkf_read(kbrkf(), ino, off, buf, 1024)) > 0) {
        for (int64_t i = 0; i < n; i++) {
            unsigned char c = buf[i];
            if (c == '\n' || c == '\t' || (c >= 32 && c < 127)) vga_putchar((char)c); else vga_putchar('.');
        }
        off += (uint64_t)n;
        if (off > (1u << 20)) { vga_print("\n[stopped after 1 MB]"); break; }
    }
    if (n < 0) print_err(path, (int)n);
    vga_print("\n");
    kfree(buf);
}

static void cmd_stat(const char *path) {
    uint32_t ino;
    int r = brkf_resolve(kbrkf(), CWD, path, 0, &ino);
    if (r != 0) { print_err(path, r); return; }
    brkf_stat_t st;
    if ((r = brkf_getattr(kbrkf(), ino, &st)) != 0) { print_err(path, r); return; }
    vga_print("  inode "); vga_print_int((int)st.ino);
    vga_print("   links "); vga_print_int((int)st.nlink);
    vga_print("   "); print_mode(st.mode);
    vga_print("\n  size "); vga_print_int((int)st.size);
    vga_print(" bytes, "); vga_print_int((int)st.blocks); vga_print(" block(s)\n  modified ");
    print_date(st.mtime_ns); vga_print("   created "); print_date(st.crtime_ns); vga_print("\n");
}

/* Write or append text to a file, creating it if needed */
static void cmd_write(const char *path, const char *text, int append) {
    uint32_t dir, ino; char name[BRKF_NAME_MAX + 1];
    int r = brkf_resolve_parent(kbrkf(), CWD, path, &dir, name);
    if (r != 0) { print_err(path, r); return; }
    r = brkf_lookup(kbrkf(), dir, name, &ino);
    if (r == BRKF_ENOENT) r = brkf_create(kbrkf(), dir, name, 0644, 0, 0, &ino);
    else if (r == 0 && !append) r = brkf_truncate(kbrkf(), ino, 0);
    if (r != 0) { print_err(path, r); return; }
    brkf_stat_t st;
    brkf_getattr(kbrkf(), ino, &st);
    size_t n = strlen(text);
    int64_t w = brkf_write(kbrkf(), ino, append ? st.size : 0, text, n);
    if (w >= 0) w = brkf_write(kbrkf(), ino, (append ? st.size : 0) + n, "\n", 1) >= 0 ? w : -1;
    if (w < 0) print_err(path, (int)w);
}

static void cmd_import(const char *src, const char *dst) {
    dir_entry_t e;
    if (fat32_find_in_dir(current_dir.cluster, src, &e) != 0) { vga_print("brkf: no such FAT32 file\n"); return; }
    if (e.file_size == 0) { vga_print("brkf: that file is empty\n"); return; }
    uint8_t *buf = (uint8_t *)kmalloc(e.file_size);
    if (!buf) { print_err("", BRKF_ENOMEM); return; }
    int got = fat32_read_file(current_dir.cluster, src, buf, e.file_size);
    if (got <= 0) { vga_print("brkf: cannot read the FAT32 file\n"); kfree(buf); return; }

    uint32_t dir, ino; char name[BRKF_NAME_MAX + 1];
    int r = brkf_resolve_parent(kbrkf(), CWD, dst, &dir, name);
    if (r == 0) {
        r = brkf_lookup(kbrkf(), dir, name, &ino);
        if (r == BRKF_ENOENT) r = brkf_create(kbrkf(), dir, name, 0755, 0, 0, &ino);
        else if (r == 0) r = brkf_truncate(kbrkf(), ino, 0);
    }
    if (r == 0) {
        int64_t w = brkf_write(kbrkf(), ino, 0, buf, (size_t)got);
        if (w != got) r = w < 0 ? (int)w : BRKF_ENOSPC;
    }
    kfree(buf);
    if (r != 0) print_err(dst, r);
    else { vga_print("copied "); vga_print_int(got); vga_print(" bytes\n"); }
}

static void cmd_export(const char *src, const char *dst) {
    uint8_t *buf; size_t n;
    int r = kbrkf_read_file(CWD, src, &buf, &n);
    if (r != 0) { print_err(src, r); return; }
    if (fat32_write_file(current_dir.cluster, dst, buf, (uint32_t)n) != 0) vga_print("brkf: cannot write the FAT32 file\n");
    else { vga_print("copied "); vga_print_int((int)n); vga_print(" bytes\n"); }
    kfree(buf);
}

void brkf_shell_command(const char *args) {
    char line[256];
    strncpy_safe(line, args, sizeof(line));
    char *rest = line;
    char *sub = next_word(&rest);
    if (!sub || !strcmp(sub, "help")) { usage(); return; }

    if (!strcmp(sub, "info")) {
        if (!kbrkf_available()) { vga_print("No second disk. Attach one (QEMU: -drive file=brkf.img,format=raw,if=ide,index=1).\n"); return; }
        vga_print("Second disk: ");
        vga_print_int((int)(blk_sectors(1) / 2048));
        vga_print(" MB\n");
        if (!kbrkf_mounted()) { vga_print("Not mounted. (brkf mount, or brkf format yes to create a volume)\n"); return; }
        brkf_statfs_t s;
        brkf_statfs(kbrkf(), &s);
        vga_print("BRKF volume '"); vga_print(s.label); vga_print("': ");
        vga_print_int((int)(s.free_blocks / 256)); vga_print(" MB free of ");
        vga_print_int((int)(s.block_count / 256)); vga_print(" MB, ");
        vga_print_int((int)s.free_inodes); vga_print(" free inodes\n");
    } else if (!strcmp(sub, "format")) {
        char *yes = next_word(&rest);
        if (!yes || strcmp(yes, "yes") != 0) { vga_print("This ERASES the whole second disk. Type: brkf format yes [label]\n"); return; }
        if (kbrkf_mounted()) { vga_print("brkf: unmount first\n"); return; }
        char *label = next_word(&rest);
        vga_print("Formatting...\n");
        int r = kbrkf_format(label ? label : "BRKF");
        if (r != 0) print_err("format", r); else vga_print("Done. Use: brkf mount\n");
    } else if (!strcmp(sub, "mount")) {
        int r = kbrkf_mount();
        if (r != 0) print_err("mount", r); else vga_print("BRKF mounted at /\n");
    } else if (!strcmp(sub, "umount") || !strcmp(sub, "unmount")) {
        sched_current()->cwd = BRKF_ROOT_INO;
        int r = kbrkf_umount();
        if (r != 0) print_err("umount", r); else vga_print("BRKF unmounted (volume marked clean)\n");
    } else if (!strcmp(sub, "sync")) {
        if (need_mount()) brkf_sync(kbrkf());
    } else if (!strcmp(sub, "fsck")) {
        char *mode = next_word(&rest);
        int r = kbrkf_check(mode && !strcmp(mode, "repair"));
        if (r == BRKF_EINVAL && kbrkf_mounted()) vga_print("brkf: unmount first (brkf umount)\n");
        else if (r != 0) print_err("fsck", r);
    } else if (!kbrkf_mounted()) {
        need_mount();
    } else if (!strcmp(sub, "ls"))      { char *p = next_word(&rest); cmd_ls(p ? p : ""); }
    else if (!strcmp(sub, "cat"))       { char *p = next_word(&rest); if (p) cmd_cat(p); else usage(); }
    else if (!strcmp(sub, "stat"))      { char *p = next_word(&rest); if (p) cmd_stat(p); else usage(); }
    else if (!strcmp(sub, "pwd"))       { char buf[256]; int r = kbrkf_path_of(CWD, buf, sizeof(buf)); if (r) print_err("", r); else { vga_print(buf); vga_print("\n"); } }
    else if (!strcmp(sub, "cd")) {
        char *p = next_word(&rest);
        uint32_t ino; brkf_stat_t st;
        int r = brkf_resolve(kbrkf(), CWD, p ? p : "/", 1, &ino);
        if (r == 0) r = brkf_getattr(kbrkf(), ino, &st);
        if (r == 0 && !BRKF_S_ISDIR(st.mode)) r = BRKF_ENOTDIR;
        if (r != 0) print_err(p, r); else sched_current()->cwd = ino;
    } else if (!strcmp(sub, "df")) {
        brkf_statfs_t s; brkf_statfs(kbrkf(), &s);
        vga_print("blocks (4 KB): "); vga_print_int((int)s.block_count); vga_print(", free "); vga_print_int((int)s.free_blocks);
        vga_print("\ninodes: "); vga_print_int((int)s.inode_count); vga_print(", free "); vga_print_int((int)s.free_inodes); vga_print("\n");
    } else if (!strcmp(sub, "write") || !strcmp(sub, "append")) {
        char *p = next_word(&rest);
        if (!p) { usage(); return; }
        while (*rest == ' ') rest++;
        cmd_write(p, rest, sub[0] == 'a');
    } else if (!strcmp(sub, "mkdir") || !strcmp(sub, "rmdir") || !strcmp(sub, "rm")) {
        char *p = next_word(&rest);
        if (!p) { usage(); return; }
        uint32_t dir; char name[BRKF_NAME_MAX + 1];
        int r = brkf_resolve_parent(kbrkf(), CWD, p, &dir, name);
        if (r == 0) r = sub[1] == 'k' ? brkf_mkdir(kbrkf(), dir, name, 0755, 0, 0, NULL)
                      : !strcmp(sub, "rmdir") ? brkf_rmdir(kbrkf(), dir, name) : brkf_unlink(kbrkf(), dir, name);
        if (r != 0) print_err(p, r);
    } else if (!strcmp(sub, "mv")) {
        char *a = next_word(&rest), *b = next_word(&rest);
        if (!a || !b) { usage(); return; }
        uint32_t d1, d2; char n1[BRKF_NAME_MAX + 1], n2[BRKF_NAME_MAX + 1];
        int r = brkf_resolve_parent(kbrkf(), CWD, a, &d1, n1);
        if (r == 0) r = brkf_resolve_parent(kbrkf(), CWD, b, &d2, n2);
        if (r == 0) r = brkf_rename(kbrkf(), d1, n1, d2, n2);
        if (r != 0) print_err("mv", r);
    } else if (!strcmp(sub, "ln")) {
        char *a = next_word(&rest);
        int symbolic = (a && !strcmp(a, "-s"));
        if (symbolic) a = next_word(&rest);
        char *b = next_word(&rest);
        if (!a || !b) { usage(); return; }
        uint32_t dir, ino; char name[BRKF_NAME_MAX + 1];
        int r = brkf_resolve_parent(kbrkf(), CWD, b, &dir, name);
        if (r == 0) r = symbolic ? brkf_symlink(kbrkf(), dir, name, a, 0, 0, NULL)
                                 : (brkf_resolve(kbrkf(), CWD, a, 0, &ino) ? BRKF_ENOENT : brkf_link(kbrkf(), dir, name, ino));
        if (r != 0) print_err("ln", r);
    } else if (!strcmp(sub, "import")) {
        char *a = next_word(&rest), *b = next_word(&rest);
        if (!a || !b) usage(); else cmd_import(a, b);
    } else if (!strcmp(sub, "export")) {
        char *a = next_word(&rest), *b = next_word(&rest);
        if (!a || !b) usage(); else cmd_export(a, b);
    } else {
        vga_print("brkf: unknown command '"); vga_print(sub); vga_print("'  (brkf help)\n");
    }
}

/* Called once at boot: find the disk and mount it if it holds a BRKF volume */
void brkf_boot_mount(void) {
    if (!kbrkf_available()) return;
    int r = kbrkf_mount();
    if (r == 0) {
        brkf_statfs_t s;
        brkf_statfs(kbrkf(), &s);
        vga_print("BRKF: mounted '");
        vga_print(s.label);
        vga_print("' at /  (");
        vga_print_int((int)(s.free_blocks / 256));
        vga_print(" MB free)\n");
    } else if (r == BRKF_EINVAL) {
        vga_print("BRKF: second disk is not formatted (brkf format yes)\n");
    } else {
        vga_print("BRKF: could not mount the second disk: ");
        vga_print(kbrkf_strerror(r));
        vga_print("\n");
    }
}
