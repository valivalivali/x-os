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
        syscall3(SYS_PROC_SPAWN, (uintptr_t)blob_buf, n, (uintptr_t)name);
    }
}

static int write_blob_to_file(int index, const char *path) {
    size_t len = syscall2(SYS_SVC_BLOB, index, 0);
    if (len == 0 || len > 2 * 1024 * 1024) {
        log("[init] blob too large or empty: ");
        log(path);
        log("\n");
        return -1;
    }
    size_t n = syscall3(SYS_SVC_BLOB, index, (uintptr_t)blob_buf, len);
    int fd = sys_open(path, XFS_O_CREAT | XFS_O_WRONLY | XFS_O_TRUNC);
    if (fd < 0) {
        log("[init] failed to create ");
        log(path);
        log("\n");
        return -1;
    }
    sys_write(fd, blob_buf, n);
    sys_close(fd);
    return 0;
}

static void write_str(const char *path, const char *s) {
    int fd = sys_open(path, XFS_O_CREAT | XFS_O_WRONLY | XFS_O_TRUNC);
    if (fd < 0) return;
    size_t n = 0;
    while (s[n]) n++;
    sys_write(fd, s, n);
    sys_close(fd);
}

static void mkdir_p(const char *path) {
    /* One level at a time — parents must already exist from kernel or prior. */
    sys_mkdir(path);
}

static void seed_fs(void) {
    /* Apple files-974 / usertemplate-109 layout (subset that fits the ramdisk). */
    mkdir_p("/Applications");
    mkdir_p("/Applications/Utilities");
    mkdir_p("/Documents");
    mkdir_p("/Desktop");
    mkdir_p("/Downloads");
    mkdir_p("/Users");
    mkdir_p("/Users/Shared");
    mkdir_p("/Users/vali");
    mkdir_p("/Users/vali/Desktop");
    mkdir_p("/Users/vali/Documents");
    mkdir_p("/Users/vali/Downloads");
    mkdir_p("/Users/vali/Library");
    mkdir_p("/Users/vali/Library/Preferences");
    mkdir_p("/Library/Application Support");
    mkdir_p("/Library/Fonts");
    mkdir_p("/Library/Keychains");
    mkdir_p("/System/Library/CoreServices");
    mkdir_p("/System/Library/Frameworks");
    mkdir_p("/System/Library/LaunchDaemons");
    mkdir_p("/System/Library/LaunchAgents");
    mkdir_p("/System/Volumes");
    mkdir_p("/System/Volumes/Data");
    mkdir_p("/private/etc");
    mkdir_p("/private/var/db");
    mkdir_p("/private/var/log");
    mkdir_p("/etc");
    mkdir_p("/etc/paths.d");

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

    write_str("/System/Library/CoreServices/SystemVersion.plist",
              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<plist version=\"1.0\"><dict>\n"
              "  <key>ProductName</key><string>X OS</string>\n"
              "  <key>ProductVersion</key><string>1.0</string>\n"
              "  <key>ProductUserVisibleVersion</key><string>1.0</string>\n"
              "  <key>ProductBuildVersion</key><string>XOS1</string>\n"
              "  <key>ProductCopyright</key><string>X OS</string>\n"
              "</dict></plist>\n");
    write_str("/Users/vali/Library/Preferences/.GlobalPreferences.plist",
              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<plist version=\"1.0\"><dict>\n"
              "  <key>Locale</key><string>en_US</string>\n"
              "</dict></plist>\n");

    /* files-974 private/etc skeletons (also under /etc for convenience). */
    write_str("/etc/paths",
              "/usr/local/bin\n"
              "/usr/bin\n"
              "/bin\n"
              "/usr/sbin\n"
              "/sbin\n");
    write_str("/private/etc/paths",
              "/usr/local/bin\n"
              "/usr/bin\n"
              "/bin\n"
              "/usr/sbin\n"
              "/sbin\n");
    write_str("/etc/shells",
              "/bin/bash\n"
              "/bin/sh\n"
              "/bin/zsh\n"
              "/bin/cmds\n");
    write_str("/private/etc/shells",
              "/bin/bash\n"
              "/bin/sh\n"
              "/bin/zsh\n"
              "/bin/cmds\n");
    write_str("/etc/hosts",
              "##\n"
              "# Host Database\n"
              "##\n"
              "127.0.0.1\tlocalhost\n"
              "255.255.255.255\tbroadcasthost\n"
              "::1\t\tlocalhost\n");
    write_str("/private/etc/hosts",
              "##\n"
              "# Host Database\n"
              "##\n"
              "127.0.0.1\tlocalhost\n"
              "255.255.255.255\tbroadcasthost\n"
              "::1\t\tlocalhost\n");
    write_str("/etc/hostname", "localhost\n");

    log("[init] fs seeded\n");
}

void init_main(void) {
    log("[init] start\n");

    seed_fs();

    /* Single multicall binary — shell/kernel map /bin/<name> → /bin/cmds.
     * Writing ~60 full copies here made desktop take 10–20s to appear. */
    if (write_blob_to_file(5, "/bin/cmds") == 0)
        log("[init] wrote /bin/cmds\n");

    spawn_blob(0, "composer");
    spawn_blob(1, "menubar");
    spawn_blob(2, "dock");
    /* Terminal before zsh so SHELL_BRIDGE is registered when the shell looks it up. */
    spawn_blob(6, "menu");
    spawn_blob(7, "terminal");
    spawn_blob(4, "zsh");

    for (;;) syscall0(SYS_YIELD);
}
