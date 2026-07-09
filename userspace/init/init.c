/* X OS Init — PID 1. Spawns the composer, then sleeps forever. */

#include "kernel/include/syscall.h"
#include "kernel/fs/xfs.h"
#include <stddef.h>

static void log(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    syscall2(SYS_DEBUG_LOG, (uintptr_t)s, n);
}

static uint8_t blob_buf[2 * 1024 * 1024];

static void spawn_blob(int index, const char *name) {
    size_t len = syscall2(SYS_SVC_BLOB, index, 0);
    if (len > 0 && len <= 2 * 1024 * 1024) {
        size_t n = syscall3(SYS_SVC_BLOB, index, (uintptr_t)blob_buf, len);
        log("[init] spawn ");
        log(name);
        log("\n");
        syscall2(SYS_PROC_SPAWN, (uintptr_t)blob_buf, n);
    }
}

static void seed_fs(void) {
    sys_mkdir("/Applications");
    sys_mkdir("/Documents");
    sys_mkdir("/Desktop");
    sys_mkdir("/Downloads");

    int fd;
    fd = sys_open("/Applications/hello.txt", XFS_O_CREAT | XFS_O_WRONLY);
    if (fd >= 0) { sys_write(fd, "hello", 5); sys_close(fd); }
    fd = sys_open("/Documents/notes.txt", XFS_O_CREAT | XFS_O_WRONLY);
    if (fd >= 0) { sys_write(fd, "notes", 5); sys_close(fd); }
    fd = sys_open("/Desktop/readme.txt", XFS_O_CREAT | XFS_O_WRONLY);
    if (fd >= 0) { sys_write(fd, "readme", 6); sys_close(fd); }
    fd = sys_open("/Downloads/file1.txt", XFS_O_CREAT | XFS_O_WRONLY);
    if (fd >= 0) { sys_write(fd, "file1", 5); sys_close(fd); }
    fd = sys_open("/Downloads/file2.txt", XFS_O_CREAT | XFS_O_WRONLY);
    if (fd >= 0) { sys_write(fd, "file2", 5); sys_close(fd); }
    log("[init] fs seeded\n");
}

void init_main(void) {
    log("[init] start\n");

    seed_fs();

    spawn_blob(0, "composer");
    spawn_blob(1, "menubar");
    spawn_blob(4, "zsh");

    for (;;) syscall0(SYS_YIELD);
}
