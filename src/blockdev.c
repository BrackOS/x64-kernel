#include "blockdev.h"
#include "io.h"
#include "serial.h"

#define ATA_DATA       0x1F0
#define ATA_COUNT      0x1F2
#define ATA_LBA_LOW    0x1F3
#define ATA_LBA_MID    0x1F4
#define ATA_LBA_HIGH   0x1F5
#define ATA_DRIVE      0x1F6
#define ATA_STATUS     0x1F7
#define ATA_COMMAND    0x1F7
#define ATA_ALT_STATUS 0x3F6

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_BSY 0x80

#define CMD_READ        0x20
#define CMD_WRITE       0x30
#define CMD_READ_EXT    0x24
#define CMD_WRITE_EXT   0x34
#define CMD_FLUSH       0xE7
#define CMD_FLUSH_EXT   0xEA
#define CMD_IDENTIFY    0xEC

typedef struct { int present; int lba48; uint64_t sectors; } blk_dev_t;
static blk_dev_t devs[2];

static void delay400ns(void) {
    for (int i = 0; i < 4; i++) (void)inb(ATA_ALT_STATUS);
}

static int wait_not_busy(void) {
    for (int t = 200000; t > 0; t--) if (!(inb(ATA_STATUS) & ST_BSY)) return 0;
    return -1;
}

static int wait_drq(void) {
    for (int t = 200000; t > 0; t--) {
        uint8_t s = inb(ATA_STATUS);
        if (s & ST_ERR) return -1;
        if (!(s & ST_BSY) && (s & ST_DRQ)) return 0;
    }
    return -1;
}

static void identify(int d) {
    blk_dev_t *dev = &devs[d];
    dev->present = 0;
    outb(ATA_DRIVE, (uint8_t)(0xA0 | (d << 4)));
    delay400ns();
    outb(ATA_COUNT, 0); outb(ATA_LBA_LOW, 0); outb(ATA_LBA_MID, 0); outb(ATA_LBA_HIGH, 0);
    outb(ATA_COMMAND, CMD_IDENTIFY);
    delay400ns();

    uint8_t s = inb(ATA_STATUS);
    if (s == 0 || s == 0xFF) return;                      /* no drive (or a floating bus) */
    if (wait_not_busy() != 0) return;
    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HIGH) != 0) return;   /* ATAPI or SATA signature: not a plain disk */
    if (wait_drq() != 0) return;

    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(ATA_DATA);
    uint64_t lba28 = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    dev->lba48 = (id[83] & (1u << 10)) != 0;
    uint64_t lba48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16) | ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    dev->sectors = (dev->lba48 && lba48) ? lba48 : lba28;
    dev->present = dev->sectors != 0;
}

int blk_init(void) {
    outb(ATA_ALT_STATUS, 0x02);                           /* nIEN: no interrupts, we poll */
    identify(0);
    identify(1);
    return devs[0].present + devs[1].present;
}

int blk_present(int drive)    { return drive >= 0 && drive < 2 && devs[drive].present; }
uint64_t blk_sectors(int drive) { return blk_present(drive) ? devs[drive].sectors : 0; }

/* Program drive, sector number and count. Returns 1 if 48-bit commands are needed. */
static int setup(int d, uint64_t lba, uint32_t count) {
    int ext = devs[d].lba48 && (lba + count > 0x0FFFFFFFULL);
    if (ext) {
        outb(ATA_DRIVE, (uint8_t)(0x40 | (d << 4)));
        outb(ATA_COUNT, 0);
        outb(ATA_LBA_LOW, (uint8_t)(lba >> 24));
        outb(ATA_LBA_MID, (uint8_t)(lba >> 32));
        outb(ATA_LBA_HIGH, (uint8_t)(lba >> 40));
        outb(ATA_COUNT, (uint8_t)count);
        outb(ATA_LBA_LOW, (uint8_t)lba);
        outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
        outb(ATA_LBA_HIGH, (uint8_t)(lba >> 16));
    } else {
        outb(ATA_COUNT, (uint8_t)count);
        outb(ATA_LBA_LOW, (uint8_t)lba);
        outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
        outb(ATA_LBA_HIGH, (uint8_t)(lba >> 16));
        outb(ATA_DRIVE, (uint8_t)(0xE0 | (d << 4) | ((lba >> 24) & 0x0F)));
    }
    return ext;
}

int blk_read(int d, uint64_t lba, uint32_t count, void *buf) {
    if (!blk_present(d) || count == 0 || count > 255 || lba + count > devs[d].sectors) return -1;
    if (wait_not_busy() != 0) return -1;
    outb(ATA_DRIVE, (uint8_t)(0xA0 | (d << 4)));
    delay400ns();
    int ext = setup(d, lba, count);
    outb(ATA_COMMAND, ext ? CMD_READ_EXT : CMD_READ);

    uint16_t *p = (uint16_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        if (wait_drq() != 0) return -1;
        for (int i = 0; i < 256; i++) *p++ = inw(ATA_DATA);
    }
    return 0;
}

int blk_write(int d, uint64_t lba, uint32_t count, const void *buf) {
    if (!blk_present(d) || count == 0 || count > 255 || lba + count > devs[d].sectors) return -1;
    if (wait_not_busy() != 0) return -1;
    outb(ATA_DRIVE, (uint8_t)(0xA0 | (d << 4)));
    delay400ns();
    int ext = setup(d, lba, count);
    outb(ATA_COMMAND, ext ? CMD_WRITE_EXT : CMD_WRITE);

    const uint16_t *p = (const uint16_t *)buf;
    for (uint32_t s = 0; s < count; s++) {
        if (wait_drq() != 0) return -1;
        for (int i = 0; i < 256; i++) outw(ATA_DATA, *p++);
    }
    outb(ATA_COMMAND, ext ? CMD_FLUSH_EXT : CMD_FLUSH);       /* make sure it is on the platter */
    return wait_not_busy();
}
