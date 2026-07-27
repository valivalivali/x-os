#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "kernel/fs/xfs.h"

/* sysfs — the running system, exposed as files.
 *
 * X OS is built for software that reasons about its own machine, so the
 * machine has to be legible.  Everything under /sys is generated on demand
 * from live kernel state: process table, memory, CPU, scheduler, input,
 * display, network.  No daemon, no snapshotting, no rebuild step — read the
 * file and you have the current answer.
 *
 * Some nodes are writable.  Writing to them changes the running system
 * immediately, in place, with no restart: retarget the pointer, retune a
 * process's priority, stop a service.  That is the point — an agent should
 * be able to inspect and adjust the platform the same way it edits a file.
 *
 * sysfs owns its own descriptor range so it can sit next to the on-disk
 * filesystem and the pipe layer without any of them knowing about each
 * other; the syscall layer routes by fd range.
 */

#define SYSFS_FD_BASE  512
#define SYSFS_MAX_FDS  32
#define SYSFS_BUF_MAX  4096

/* True for any path this filesystem owns ("/sys" and everything below). */
bool sysfs_owns(const char *path);
bool sysfs_owns_fd(int fd);

int  sysfs_open(const char *path, uint32_t flags);
void sysfs_close(int fd);
int  sysfs_read(int fd, void *buf, size_t count);
int  sysfs_write(int fd, const void *buf, size_t count);
int  sysfs_lseek(int fd, int offset, int whence);
int  sysfs_readdir(int fd, xfs_dirent_t *entries, int max_entries);
int  sysfs_stat(const char *path, xfs_dirent_t *out);
int  sysfs_fstat(int fd, xfs_dirent_t *out);
