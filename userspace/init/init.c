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

/* Log a prefix + name + suffix in a single syscall so the message is
 * atomic and can't interleave with other CPUs' console output. */
static void log_concat(const char *pfx, const char *name, const char *sfx) {
    char buf[128];
    size_t i = 0;
    for (size_t j = 0; pfx[j] && i < sizeof(buf) - 1; j++) buf[i++] = pfx[j];
    for (size_t j = 0; name[j] && i < sizeof(buf) - 1; j++) buf[i++] = name[j];
    for (size_t j = 0; sfx[j] && i < sizeof(buf) - 1; j++) buf[i++] = sfx[j];
    syscall2(SYS_DEBUG_LOG, (uintptr_t)buf, i);
}

/* Fork + exec a binary from disk. Like launchd spawning a daemon. */
static void spawn_from_disk(const char *path, const char *name) {
    int pid = sys_fork();
    if (pid == 0) {
        char *argv[] = { (char *)name, NULL };
        sys_exec(path, argv);
        log_concat("[init] exec failed: ", path, "\n");
        sys_exit(1);
    }
    log_concat("[init] spawned ", name, "\n");
}

void init_main(void) {
    log("[init] start\n");

    /* Spawn GUI services from disk (like launchd). */
    spawn_from_disk("/sbin/composer", "composer");
    spawn_from_disk("/sbin/menubar", "menubar");
    spawn_from_disk("/sbin/dock", "dock");
    /* Terminal before zsh so SHELL_BRIDGE is registered when the shell looks it up. */
    spawn_from_disk("/Applications/Terminal.app/Contents/Xos/Terminal", "terminal");
    /* zsh re-enabled */
    spawn_from_disk("/sbin/zsh", "zsh");

    /* init stays alive as PID 1 to reap children.  Sleep instead of
     * busy-yielding — SIGCHLD deliveries will wake us via the signal
     * path, and the 100ms sleep is just a backstop. */
    for (;;) syscall1(12, 100);  /* SYS_NSLEEP, 100ms */
}
