#ifndef VFS_H
#define VFS_H

#include <stdint.h>

/* File system calls for user programs. BRKF is mounted at "/" (when a BRKF disk
 * is attached); the FAT32 disk stays reachable from the shell only. Descriptors
 * 0-2 are the console; files start at 3. Errors are negative Linux errno values. */
long vfs_open(uint64_t upath, int flags, int mode);
long vfs_close(int fd);
long vfs_read(int fd, uint64_t ubuf, uint64_t count);
long vfs_write(int fd, uint64_t ubuf, uint64_t count);
long vfs_lseek(int fd, int64_t off, int whence);
long vfs_fstat(int fd, uint64_t ustat);
long vfs_stat(uint64_t upath, uint64_t ustat, int follow);
long vfs_ftruncate(int fd, uint64_t size);
long vfs_getdents64(int fd, uint64_t ubuf, uint64_t count);
long vfs_mkdir(uint64_t upath, int mode);
long vfs_rmdir(uint64_t upath);
long vfs_unlink(uint64_t upath);
long vfs_rename(uint64_t uold, uint64_t unew);
long vfs_chdir(uint64_t upath);
long vfs_getcwd(uint64_t ubuf, uint64_t size);
long vfs_readlink(uint64_t upath, uint64_t ubuf, uint64_t size);

int  vfs_is_file_fd(int fd);          /* fd >= 3 : not the console */

#endif
