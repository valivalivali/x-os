/* X OS Init — PID 1. Spawns the composer, then sleeps forever. */

#include "kernel/include/syscall.h"
#include "kernel/fs/xfs.h"
#include <stddef.h>

static void log(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    syscall2(SYS_DEBUG_LOG, (uintptr_t)s, n);
}

static uint8_t blob_buf[8 * 1024 * 1024];

static void spawn_blob(int index, const char *name) {
    size_t len = syscall2(SYS_SVC_BLOB, index, 0);
    if (len > 0 && len <= 8 * 1024 * 1024) {
        size_t n = syscall3(SYS_SVC_BLOB, index, (uintptr_t)blob_buf, len);
        log("[init] spawn ");
        log(name);
        log("\n");
        syscall2(SYS_PROC_SPAWN, (uintptr_t)blob_buf, n);
    }
}

static void write_blob_to_file(int index, const char *path) {
    size_t len = syscall2(SYS_SVC_BLOB, index, 0);
    if (len == 0 || len > 2 * 1024 * 1024) {
        log("[init] blob too large or empty: ");
        log(path);
        log("\n");
        return;
    }
    size_t n = syscall3(SYS_SVC_BLOB, index, (uintptr_t)blob_buf, len);
    int fd = sys_open(path, XFS_O_CREAT | XFS_O_WRONLY | XFS_O_TRUNC);
    if (fd < 0) {
        log("[init] failed to create ");
        log(path);
        log("\n");
        return;
    }
    sys_write(fd, blob_buf, n);
    sys_close(fd);
    log("[init] wrote ");
    log(path);
    log("\n");
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

    /* Write multi-call cmds binary to /bin/cmds for exec from shell */
    write_blob_to_file(5, "/bin/cmds");

    spawn_blob(0, "composer");
    spawn_blob(1, "menubar");
    spawn_blob(2, "dock");
    /* Terminal before zsh so SHELL_BRIDGE is registered when the shell looks it up. */
    spawn_blob(6, "menu");
    spawn_blob(7, "terminal");
    spawn_blob(4, "zsh");

    for (;;) syscall0(SYS_YIELD);
}
