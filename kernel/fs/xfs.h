#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "kernel/hal/block/block_dev.h"

/* XFS — X OS File System
 * Simple extent-based filesystem for a modern OS.
 * 4 KiB blocks, flat directories, direct block pointers.
 */

#define XFS_MAGIC       0x58465331   /* "XFS1" */
#define XFS_VERSION     1
#define XFS_BLOCK_SIZE  4096
#define XFS_NAME_MAX    64

/* Per-process file descriptor (userspace sees an int fd) */
#define XFS_MAX_FDS     32

/* Open flags */
#define XFS_O_RDONLY    0
#define XFS_O_WRONLY    1
#define XFS_O_RDWR      2
#define XFS_O_CREAT     4
#define XFS_O_TRUNC     8

/* dirent flags */
#define XFS_DENT_FILE      0
#define XFS_DENT_DIR       1

typedef struct {
    char     name[XFS_NAME_MAX];
    uint32_t inode_block;
    uint32_t size;
    uint16_t flags;
    uint16_t reserved;
} xfs_dirent_t;

typedef struct {
    char     name[XFS_NAME_MAX];     /* null-terminated */
    uint32_t size;                   /* bytes of data */
    uint16_t flags;                  /* bit 0 = is_directory */
    uint16_t block_count;            /* allocated data blocks */
    uint32_t data_blocks[900];       /* direct block pointers */
    uint32_t checksum;
} xfs_inode_t;

bool xfs_format(block_dev_t *dev);
bool xfs_mount(block_dev_t *dev);
int  xfs_open(const char *path, uint32_t flags);
void xfs_close(int fd);
int  xfs_read(int fd, void *buf, size_t count);
int  xfs_write(int fd, const void *buf, size_t count);
int  xfs_mkdir(const char *path);
int  xfs_readdir(int fd, xfs_dirent_t *entries, int max_entries);
int  xfs_lseek(int fd, int offset, int whence);
int  xfs_stat(const char *path, xfs_dirent_t *out);
int  xfs_fstat(int fd, xfs_dirent_t *out);
int  xfs_unlink(const char *path);
int  xfs_ftruncate(int fd, int size);
int  xfs_exists(const char *path);
void xfs_create_hierarchy(void);
