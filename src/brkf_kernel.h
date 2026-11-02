#ifndef BRKF_KERNEL_H
#define BRKF_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include "../brkf/brkf.h"

/* BRKF on the second IDE disk, and the "mounted at /" view the shell and user
 * programs share. */
int      kbrkf_available(void);              /* is there a second disk? */
int      kbrkf_mounted(void);
brkf_t  *kbrkf(void);
int      kbrkf_mount(void);                  /* 0 or a negative BRKF error */
int      kbrkf_umount(void);
int      kbrkf_format(const char *label);    /* ERASES the second disk */
int      kbrkf_check(int repair);            /* run fsck (the volume must be unmounted) */
const char *kbrkf_strerror(int err);

/* Read a whole file into a kmalloc'd buffer (the caller frees it) */
int      kbrkf_read_file(uint32_t cwd, const char *path, uint8_t **data, size_t *len);

/* Build the absolute path of a directory inode ("/a/b") */
int      kbrkf_path_of(uint32_t dir, char *buf, size_t cap);

#endif
