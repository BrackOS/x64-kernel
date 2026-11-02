#include "fat32.h"
#include "vga.h"
#include "io.h"
#include "string.h"
#include <stdint.h>

/* Global FAT32 state */
fat32_fs_t fat32_fs = {0};
static uint8_t sector_buffer[SECTOR_SIZE];

/* ATA PIO disk registers (primary bus, master drive) */
#define IDE_DATA        0x1F0
#define IDE_ERROR       0x1F1
#define IDE_SECTOR_CNT  0x1F2
#define IDE_LBA_LOW     0x1F3
#define IDE_LBA_MID     0x1F4
#define IDE_LBA_HIGH    0x1F5
#define IDE_DRIVE_HEAD  0x1F6
#define IDE_COMMAND     0x1F7
#define IDE_STATUS      0x1F7

#define IDE_CMD_READ    0x20
#define IDE_CMD_WRITE   0x30
#define IDE_CMD_FLUSH   0xE7
#define IDE_CMD_READ_EXT   0x24   /* 48-bit LBA variants */
#define IDE_CMD_WRITE_EXT  0x34
#define IDE_CMD_FLUSH_EXT  0xEA
#define IDE_LBA28_LIMIT    0x10000000u

#define IDE_STATUS_ERR  0x01
#define IDE_STATUS_DRQ  0x08
#define IDE_STATUS_BSY  0x80

#define FAT_EOC_MIN     0x0FFFFFF8   /* clusters >= this end a chain */
#define FAT_EOC_MARK    0x0FFFFFFF   /* value written at the end of a chain */
#define FAT_FREE        0x00000000
#define FAT_NO_CLUSTER  0xFFFFFFFF

#define ATTR_ARCHIVE    0x20
#define DIR_ENTRY_SIZE  32

/* ---------- ATA disk I/O ---------- */

static int ide_wait_not_busy(void) {
    for (int t = 100000; t > 0; t--) {
        if (!(inb(IDE_STATUS) & IDE_STATUS_BSY)) return 0;
    }
    return -1;
}

static int ide_wait_drq(void) {
    for (int t = 100000; t > 0; t--) {
        uint8_t s = inb(IDE_STATUS);
        if (s & IDE_STATUS_ERR) return -1;
        if (!(s & IDE_STATUS_BSY) && (s & IDE_STATUS_DRQ)) return 0;
    }
    return -1;
}

/* Select a sector. Sectors below 2^28 use the classic 28-bit LBA commands;
 * higher ones use 48-bit LBA. Returns 1 when 48-bit commands must be used. */
static int ide_select(uint32_t sector) {
    if (sector >= IDE_LBA28_LIMIT) {
        outb(IDE_DRIVE_HEAD, 0x40);                 /* LBA mode, master drive */
        outb(IDE_SECTOR_CNT, 0);                    /* high bytes first ... */
        outb(IDE_LBA_LOW,  (sector >> 24) & 0xFF);
        outb(IDE_LBA_MID,  0);
        outb(IDE_LBA_HIGH, 0);
        outb(IDE_SECTOR_CNT, 1);                    /* ... then the low bytes */
        outb(IDE_LBA_LOW,  sector & 0xFF);
        outb(IDE_LBA_MID,  (sector >> 8) & 0xFF);
        outb(IDE_LBA_HIGH, (sector >> 16) & 0xFF);
        return 1;
    }
    outb(IDE_SECTOR_CNT, 1);
    outb(IDE_LBA_LOW,  sector & 0xFF);
    outb(IDE_LBA_MID,  (sector >> 8) & 0xFF);
    outb(IDE_LBA_HIGH, (sector >> 16) & 0xFF);
    outb(IDE_DRIVE_HEAD, 0xE0 | ((sector >> 24) & 0x0F));  /* LBA mode */
    return 0;
}

/* Read one 512-byte sector straight from the disk */
static int ata_read_sector(uint32_t sector, uint8_t *buffer) {
    if (ide_wait_not_busy() != 0) return -1;
    int ext = ide_select(sector);
    outb(IDE_COMMAND, ext ? IDE_CMD_READ_EXT : IDE_CMD_READ);
    if (ide_wait_drq() != 0) return -1;

    for (int i = 0; i < 256; i++) {
        uint16_t data = inw(IDE_DATA);
        buffer[i * 2]     = data & 0xFF;
        buffer[i * 2 + 1] = (data >> 8) & 0xFF;
    }
    return 0;
}

/* Write one 512-byte sector straight to the disk */
static int ata_write_sector(uint32_t sector, const uint8_t *buffer) {
    if (ide_wait_not_busy() != 0) return -1;
    int ext = ide_select(sector);
    outb(IDE_COMMAND, ext ? IDE_CMD_WRITE_EXT : IDE_CMD_WRITE);
    if (ide_wait_drq() != 0) return -1;

    for (int i = 0; i < 256; i++) {
        outw(IDE_DATA, (uint16_t)(buffer[i * 2] | (buffer[i * 2 + 1] << 8)));
    }

    /* Flush the drive's write cache so the data is really on disk */
    outb(IDE_COMMAND, ext ? IDE_CMD_FLUSH_EXT : IDE_CMD_FLUSH);
    if (ide_wait_not_busy() != 0) return -1;
    return 0;
}

/* ---------- Sector cache ----------
 * Direct-mapped, write-through: reads are served from memory when possible
 * and every write goes to the disk immediately, so the cache never holds
 * data the disk does not have. */
#define CACHE_SLOTS 256                 /* power of two; 256 x 512 B = 128 KB */

typedef struct {
    uint32_t lba;
    int      valid;
    uint8_t  data[SECTOR_SIZE];
} cache_slot_t;

static cache_slot_t cache[CACHE_SLOTS];
static uint32_t cache_hits, cache_misses, cache_writes;

void fat32_cache_clear(void) {
    for (int i = 0; i < CACHE_SLOTS; i++) cache[i].valid = 0;
}

void fat32_cache_stats(uint32_t *hits, uint32_t *misses, uint32_t *writes) {
    *hits = cache_hits;
    *misses = cache_misses;
    *writes = cache_writes;
}

/* Read one 512-byte sector (through the cache) */
int fat32_read_sector(uint32_t sector, uint8_t *buffer) {
    cache_slot_t *slot = &cache[sector & (CACHE_SLOTS - 1)];
    if (slot->valid && slot->lba == sector) {
        cache_hits++;
        memcpy(buffer, slot->data, SECTOR_SIZE);
        return 0;
    }
    cache_misses++;
    slot->valid = 0;
    if (ata_read_sector(sector, slot->data) != 0) return -1;
    slot->lba = sector;
    slot->valid = 1;
    memcpy(buffer, slot->data, SECTOR_SIZE);
    return 0;
}

/* Write one 512-byte sector (to the disk first, then into the cache) */
int fat32_write_sector(uint32_t sector, uint8_t *buffer) {
    cache_slot_t *slot = &cache[sector & (CACHE_SLOTS - 1)];
    cache_writes++;
    if (ata_write_sector(sector, buffer) != 0) {
        if (slot->valid && slot->lba == sector) slot->valid = 0;   /* unknown state */
        return -1;
    }
    memcpy(slot->data, buffer, SECTOR_SIZE);
    slot->lba = sector;
    slot->valid = 1;
    return 0;
}

/* ---------- Cluster and FAT helpers ---------- */

static uint32_t cluster_to_sector(uint32_t cluster) {
    return fat32_fs.data_start_sector +
           (cluster - 2) * fat32_fs.boot.sectors_per_cluster;
}

/* Write a value into a cluster's FAT entry in every FAT copy */
static int fat_set(uint32_t cluster, uint32_t value) {
    uint8_t buf[SECTOR_SIZE];
    uint32_t off = cluster * 4;
    uint32_t idx = off / SECTOR_SIZE;
    uint32_t in_sector = off % SECTOR_SIZE;

    for (uint32_t f = 0; f < fat32_fs.boot.fats; f++) {
        uint32_t sec = fat32_fs.fat_start_sector + f * fat32_fs.boot.sectors_per_fat + idx;
        if (fat32_read_sector(sec, buf) != 0) return -1;
        value &= 0x0FFFFFFF;
        memcpy(&buf[in_sector], &value, 4);
        if (fat32_write_sector(sec, buf) != 0) return -1;
    }
    return 0;
}

/* Does this boot sector describe a FAT32 volume? */
static int bpb_valid(const boot_sector_t *b) {
    return b->bytes_per_sector == SECTOR_SIZE &&
           b->sectors_per_cluster != 0 &&
           b->fats != 0 &&
           b->root_entries == 0 &&            /* FAT12/16 have a fixed root directory */
           b->sectors_per_fat_small == 0 &&
           b->sectors_per_fat != 0 &&
           b->root_cluster >= 2;
}

/* Try to mount the FAT32 volume whose boot sector is at lba.
 * Returns 0 on success, -1 if there is no FAT32 volume there, -2 on a disk error. */
static int mount_volume(uint32_t lba) {
    uint8_t buf[SECTOR_SIZE];
    if (fat32_read_sector(lba, buf) != 0) return -2;
    if (buf[510] != 0x55 || buf[511] != 0xAA) return -1;

    boot_sector_t b;
    memcpy(&b, buf, sizeof(boot_sector_t));
    if (!bpb_valid(&b)) return -1;

    uint32_t fat_sectors = (uint32_t)b.fats * b.sectors_per_fat;
    uint32_t total = b.total_sectors ? b.total_sectors : b.small_sectors;
    if (total <= (uint32_t)b.reserved_sectors + fat_sectors) return -1;

    /* Calculate filesystem layout (all absolute sector numbers) */
    fat32_fs.boot = b;
    fat32_fs.partition_lba = lba;
    fat32_fs.fat_start_sector = lba + b.reserved_sectors;
    fat32_fs.data_start_sector = fat32_fs.fat_start_sector + fat_sectors;
    fat32_fs.root_dir_cluster = b.root_cluster;
    fat32_fs.total_clusters = (total - b.reserved_sectors - fat_sectors) / b.sectors_per_cluster;
    fat32_fs.next_free_hint = 2;
    return 0;
}

/* Initialize FAT32 filesystem. drive_lba is where the volume starts; when
 * it is 0 and sector 0 is not a FAT32 boot sector, the MBR partition table
 * is searched for a FAT32 partition. */
int fat32_init(uint32_t drive_lba) {
    fat32_cache_clear();
    int r = mount_volume(drive_lba);

    if (r == -1 && drive_lba == 0) {
        uint8_t mbr[SECTOR_SIZE];
        if (fat32_read_sector(0, mbr) == 0 && mbr[510] == 0x55 && mbr[511] == 0xAA) {
            for (int i = 0; i < 4 && r != 0; i++) {
                const uint8_t *e = &mbr[446 + 16 * i];
                uint32_t start;
                memcpy(&start, &e[8], 4);
                if ((e[4] == 0x0B || e[4] == 0x0C) && start != 0) {   /* FAT32 CHS / LBA */
                    r = mount_volume(start);
                }
            }
        }
    }

    if (r == -2) {
        vga_print("FAT32: Failed to read boot sector\n");
        return -1;
    }
    if (r != 0) {
        vga_print("FAT32: No FAT32 volume found\n");
        return -1;
    }

    vga_print("FAT32: Initialized (");
    vga_print_int(fat32_fs.boot.bytes_per_sector);
    vga_print(" bytes/sector, cluster=");
    vga_print_int(fat32_fs.boot.sectors_per_cluster);
    vga_print(" sectors");
    if (fat32_fs.partition_lba != 0) {
        vga_print(", partition at LBA ");
        vga_print_int((int)fat32_fs.partition_lba);
    }
    vga_print(")\n");

    return 0;
}

/* Get next cluster in FAT chain. Clusters outside the volume read as end-of-chain,
 * so a damaged chain can never send us reading past the FAT. */
uint32_t fat32_get_next_cluster(uint32_t cluster) {
    if (cluster < 2 || cluster >= fat32_fs.total_clusters + 2) return FAT_EOC_MARK;

    uint8_t buf[SECTOR_SIZE];       /* local: must not clobber callers' sector_buffer */
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat32_fs.fat_start_sector + (fat_offset / SECTOR_SIZE);
    uint32_t offset_in_sector = fat_offset % SECTOR_SIZE;

    if (fat32_read_sector(fat_sector, buf) != 0) {
        return FAT_NO_CLUSTER;
    }

    uint32_t next_cluster;
    memcpy(&next_cluster, &buf[offset_in_sector], 4);
    return next_cluster & 0x0FFFFFFF;  /* Mask to 28 bits */
}

/* Find a free cluster. Searches only inside the volume, starting from where
 * the last search ended (wrapping around once). */
uint32_t fat32_get_free_cluster(void) {
    uint32_t n = fat32_fs.total_clusters;
    if (n == 0) return FAT_NO_CLUSTER;
    uint32_t start = fat32_fs.next_free_hint;
    if (start < 2 || start >= n + 2) start = 2;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t cluster = 2 + (start - 2 + i) % n;
        if (fat32_get_next_cluster(cluster) == FAT_FREE) {
            fat32_fs.next_free_hint = cluster + 1;
            return cluster;
        }
    }
    return FAT_NO_CLUSTER;
}

/* Link current_cluster -> new_cluster and mark new_cluster as end of chain */
int fat32_allocate_cluster(uint32_t current_cluster, uint32_t new_cluster) {
    if (new_cluster == FAT_NO_CLUSTER) return -1;
    if (fat_set(new_cluster, FAT_EOC_MARK) != 0) return -1;
    if (current_cluster >= 2 && fat_set(current_cluster, new_cluster) != 0) return -1;
    return 0;
}

/* Free every cluster in a chain (used when a file is overwritten) */
static void free_chain(uint32_t cluster) {
    while (cluster >= 2 && cluster < FAT_EOC_MIN) {
        uint32_t next = fat32_get_next_cluster(cluster);
        fat_set(cluster, FAT_FREE);
        cluster = next;
    }
}

/* ---------- Directory helpers ---------- */

/* Find an entry by its 8.3 name. Returns 1 if found and sets sector and offset.
 * If not found, returns 0 and sets sector and offset to the first free slot
 * (offset is -1 if the directory is full). last receives the last cluster in the chain. */
static int dir_slot(uint32_t dir_cluster, const uint8_t name83[11],
                    uint32_t *sector, int *offset, uint32_t *last) {
    uint32_t cluster = dir_cluster;
    int have_free = 0;
    uint32_t free_sector = 0;
    int free_offset = 0;

    *last = dir_cluster;
    while (cluster >= 2 && cluster < FAT_EOC_MIN) {
        *last = cluster;
        for (uint32_t s = 0; s < fat32_fs.boot.sectors_per_cluster; s++) {
            uint32_t sec = cluster_to_sector(cluster) + s;
            if (fat32_read_sector(sec, sector_buffer) != 0) return -1;

            for (int i = 0; i < SECTOR_SIZE; i += DIR_ENTRY_SIZE) {
                dir_entry_t *ent = (dir_entry_t *)&sector_buffer[i];
                if (ent->filename[0] == 0x00 || ent->filename[0] == 0xE5) {
                    if (!have_free) {
                        have_free = 1;
                        free_sector = sec;
                        free_offset = i;
                    }
                    if (ent->filename[0] == 0x00) goto done_scanning;
                    continue;
                }
                if (memcmp(ent->filename, name83, 11) == 0) {
                    *sector = sec;
                    *offset = i;
                    return 1;
                }
            }
        }
        cluster = fat32_get_next_cluster(cluster);
    }

done_scanning:
    if (have_free) {
        *sector = free_sector;
        *offset = free_offset;
        return 0;
    }
    *offset = -1;
    return 0;
}

/* Write a directory entry for name83, adding a cluster to the directory if full */
static int dir_store(uint32_t dir_cluster, const uint8_t name83[11],
                     uint32_t first_cluster, uint32_t size) {
    uint32_t sector, last;
    int offset;
    int found = dir_slot(dir_cluster, name83, &sector, &offset, &last);
    if (found < 0) return -1;

    if (offset < 0) {
        /* Directory is full: chain on a fresh, zeroed cluster */
        uint32_t nc = fat32_get_free_cluster();
        if (nc == FAT_NO_CLUSTER) return -1;
        if (fat32_allocate_cluster(last, nc) != 0) return -1;

        uint8_t zero[SECTOR_SIZE];
        memset(zero, 0, SECTOR_SIZE);
        for (uint32_t s = 0; s < fat32_fs.boot.sectors_per_cluster; s++) {
            if (fat32_write_sector(cluster_to_sector(nc) + s, zero) != 0) return -1;
        }
        sector = cluster_to_sector(nc);
        offset = 0;
    }

    if (fat32_read_sector(sector, sector_buffer) != 0) return -1;
    dir_entry_t *e = (dir_entry_t *)&sector_buffer[offset];
    memset(e, 0, DIR_ENTRY_SIZE);
    memcpy(e->filename, name83, 11);
    e->attributes = ATTR_ARCHIVE;
    e->first_cluster_high = (uint16_t)(first_cluster >> 16);
    e->first_cluster_low  = (uint16_t)(first_cluster & 0xFFFF);
    e->file_size = size;
    return fat32_write_sector(sector, sector_buffer);
}

/* Find file in a directory by name. Returns 0 on success. */
int fat32_find_in_dir(uint32_t dir_cluster, const char *filename, dir_entry_t *entry) {
    uint8_t search_name[11];
    uint32_t sector, last;
    int offset;

    fat32_filename_to_8_3(filename, search_name);
    if (dir_slot(dir_cluster, search_name, &sector, &offset, &last) != 1) {
        return -1;
    }
    if (fat32_read_sector(sector, sector_buffer) != 0) return -1;
    memcpy(entry, &sector_buffer[offset], sizeof(dir_entry_t));
    return 0;
}

/* Find file in the root directory */
int fat32_find_file(const char *filename, dir_entry_t *entry) {
    return fat32_find_in_dir(fat32_fs.root_dir_cluster, filename, entry);
}

/* List files in directory */
int fat32_list_directory(uint32_t cluster, dir_entry_t *entries, int max_entries) {
    int count = 0;

    while (cluster >= 2 && cluster < FAT_EOC_MIN && count < max_entries) {
        for (uint32_t s = 0; s < fat32_fs.boot.sectors_per_cluster; s++) {
            if (fat32_read_sector(cluster_to_sector(cluster) + s, sector_buffer) != 0) {
                continue;
            }

            for (int i = 0; i < SECTOR_SIZE && count < max_entries; i += DIR_ENTRY_SIZE) {
                dir_entry_t *ent = (dir_entry_t *)&sector_buffer[i];

                if (ent->filename[0] == 0) {
                    return count;  /* End of directory */
                }
                if (ent->filename[0] == 0xE5 || ent->filename[0] == 0x2E) {
                    continue;      /* Deleted entry or . / .. */
                }
                memcpy(&entries[count], ent, sizeof(dir_entry_t));
                count++;
            }
        }
        cluster = fat32_get_next_cluster(cluster);
    }

    return count;
}

/* ---------- File read / write ---------- */

/* Read up to cap bytes of a file. Returns bytes read, or -1 on error. */
int fat32_read_file(uint32_t dir_cluster, const char *name, uint8_t *buf, uint32_t cap) {
    dir_entry_t e;
    if (fat32_find_in_dir(dir_cluster, name, &e) != 0) return -1;

    uint32_t first = ((uint32_t)e.first_cluster_high << 16) | e.first_cluster_low;
    uint32_t limit = (e.file_size < cap) ? e.file_size : cap;
    uint32_t done = 0;
    uint32_t cluster = first;
    uint8_t tmp[SECTOR_SIZE];

    while (cluster >= 2 && cluster < FAT_EOC_MIN && done < limit) {
        for (uint32_t s = 0; s < fat32_fs.boot.sectors_per_cluster && done < limit; s++) {
            if (fat32_read_sector(cluster_to_sector(cluster) + s, tmp) != 0) return -1;
            for (uint32_t i = 0; i < SECTOR_SIZE && done < limit; i++) {
                buf[done++] = tmp[i];
            }
        }
        cluster = fat32_get_next_cluster(cluster);
    }
    return (int)done;
}

/* Create or overwrite a file in a directory. Returns 0 on success. */
int fat32_write_file(uint32_t dir_cluster, const char *name,
                     const uint8_t *data, uint32_t size) {
    uint8_t name83[11];
    uint32_t sector, last;
    int offset;
    fat32_filename_to_8_3(name, name83);

    /* Release the old data first so its clusters can be reused */
    if (dir_slot(dir_cluster, name83, &sector, &offset, &last) == 1) {
        if (fat32_read_sector(sector, sector_buffer) != 0) return -1;
        dir_entry_t *old = (dir_entry_t *)&sector_buffer[offset];
        free_chain(((uint32_t)old->first_cluster_high << 16) | old->first_cluster_low);
    }

    /* Build the new cluster chain and write the data */
    uint32_t first = 0, prev = 0, done = 0;
    uint32_t spc = fat32_fs.boot.sectors_per_cluster;

    while (done < size) {
        uint32_t c = fat32_get_free_cluster();
        if (c == FAT_NO_CLUSTER) return -1;   /* disk full */
        if (fat32_allocate_cluster(prev, c) != 0) return -1;
        if (prev == 0) first = c;

        for (uint32_t s = 0; s < spc; s++) {
            uint8_t buf[SECTOR_SIZE];
            for (uint32_t i = 0; i < SECTOR_SIZE; i++) {
                uint32_t pos = done + s * SECTOR_SIZE + i;
                buf[i] = (pos < size) ? data[pos] : 0;
            }
            if (fat32_write_sector(cluster_to_sector(c) + s, buf) != 0) return -1;
        }
        done += spc * SECTOR_SIZE;
        prev = c;
    }

    return dir_store(dir_cluster, name83, first, size);
}

/* Delete a file: free its clusters and mark its directory entry deleted.
 * Returns 0 on success, -1 if not found or on error, -2 if it is a directory. */
int fat32_delete_file(uint32_t dir_cluster, const char *name) {
    uint8_t name83[11];
    uint32_t sector, last;
    int offset;
    fat32_filename_to_8_3(name, name83);

    if (dir_slot(dir_cluster, name83, &sector, &offset, &last) != 1) return -1;
    if (fat32_read_sector(sector, sector_buffer) != 0) return -1;

    dir_entry_t *e = (dir_entry_t *)&sector_buffer[offset];
    if (e->attributes & 0x10) return -2;   /* directory */
    uint32_t first = ((uint32_t)e->first_cluster_high << 16) | e->first_cluster_low;

    free_chain(first);   /* re-read the directory sector to be safe */

    if (fat32_read_sector(sector, sector_buffer) != 0) return -1;
    e = (dir_entry_t *)&sector_buffer[offset];
    e->filename[0] = 0xE5;
    return fat32_write_sector(sector, sector_buffer);
}

/* ---------- Filename conversion ---------- */

/* Convert filename to FAT 8.3 format (uppercase, space padded) */
void fat32_filename_to_8_3(const char *filename, uint8_t *name_83) {
    memset(name_83, ' ', 11);

    const char *dot = strchr(filename, '.');
    int name_len = dot ? (int)(dot - filename) : (int)strlen(filename);
    int ext_len = dot ? (int)strlen(dot + 1) : 0;

    if (name_len > 8) name_len = 8;
    if (ext_len > 3) ext_len = 3;

    for (int i = 0; i < name_len; i++) {
        name_83[i] = (filename[i] >= 'a' && filename[i] <= 'z') ?
                     filename[i] - 32 : filename[i];
    }

    if (dot) {
        for (int i = 0; i < ext_len; i++) {
            name_83[8 + i] = (dot[i + 1] >= 'a' && dot[i + 1] <= 'z') ?
                             dot[i + 1] - 32 : dot[i + 1];
        }
    }
}

/* Convert 8.3 FAT32 filename to normal format */
void fat32_8_3_to_filename(uint8_t *name_83, char *filename) {
    int i, j = 0;

    for (i = 0; i < 8; i++) {
        if (name_83[i] != ' ') {
            filename[j++] = name_83[i];
        }
    }

    if (name_83[8] != ' ') {
        filename[j++] = '.';
        for (i = 8; i < 11; i++) {
            if (name_83[i] != ' ') {
                filename[j++] = name_83[i];
            }
        }
    }

    filename[j] = 0;
}
