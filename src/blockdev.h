#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include <stdint.h>

/* Raw sector access to the two IDE disks on the primary channel:
 * drive 0 = master (the FAT32 disk), drive 1 = slave (the BRKF disk). */
#define BLK_SECTOR_SIZE 512

int      blk_init(void);                   /* probe both drives (ATA IDENTIFY) */
int      blk_present(int drive);
uint64_t blk_sectors(int drive);
int      blk_read(int drive, uint64_t lba, uint32_t count, void *buf);         /* count 1..255 */
int      blk_write(int drive, uint64_t lba, uint32_t count, const void *buf);  /* 0 or -1 */

#endif
