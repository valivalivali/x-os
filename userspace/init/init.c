/* X OS Init — PID 1. Like launchd: spawns services from disk via fork+exec.
 *
 * The filesystem is pre-seeded at build time (tools/xfs_mkfs creates the
 * disk image with all binaries, directories, and config files already in
 * place). Init just mounts and fork+execs the GUI services — no blob
 * writing, no directory creation, no config file generation at boot. */

#include "kernel/include/syscall.h"
#include <stddef.h>

static void log(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    syscall2(SYS_DEBUG_LOG, (uintptr_t)s, n);
}

/* Fork + exec a binary from disk. Like launchd spawning a daemon. */
static void spawn_from_disk(const char *path, const char *name) {
    int pid = sys_fork();
    if (pid == 0) {
        char *argv[] = { (char *)name, NULL };
        sys_exec(path, argv);
        log("[init] exec failed: ");
        log(path);
        log("\n");
        sys_exit(1);
    }
    log("[init] spawned ");
    log(name);
    log("\n");
}

void init_main(void) {
    log("[init] start\n");

    /* Spawn GUI services from disk (like launchd). */
    spawn_from_disk("/sbin/composer", "composer");
    spawn_from_disk("/sbin/menubar", "menubar");
    spawn_from_disk("/sbin/dock", "dock");
    /* Terminal before zsh so SHELL_BRIDGE is registered when the shell looks it up. */
    spawn_from_disk("/Applications/Terminal.app/Contents/Xos/Terminal", "terminal");
    spawn_from_disk("/sbin/zsh", "zsh");

    for (;;) syscall0(SYS_YIELD);
}
