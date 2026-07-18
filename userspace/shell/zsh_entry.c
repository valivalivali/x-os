/* zsh_entry.c — C entry point for the x-os shell.
 * Sets up IPC bridge with the terminal, then runs an interactive
 * command shell that fork+execs /bin/cmds (multi-call binary). */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include "kernel/include/syscall.h"
#include "kernel/include/ipc.h"

extern void set_shell_bridge(port_handle_t input_port, port_handle_t output_port);

#define SHELL_BRIDGE_HELLO   0x1000

#define CMDS_PATH "/bin/cmds"
#define MAX_ARGS  32
#define MAX_LINE  512

static int my_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void log(const char *s) {
    syscall2(SYS_DEBUG_LOG, (uintptr_t)s, my_strlen(s));
}

static void shell_write(const char *s) {
    extern _ssize_t _write(int fd, const void *buf, size_t cnt);
    _write(1, s, my_strlen(s));
}

static const char *known_cmds[] = {
    "echo", "pwd", "true", "false", "basename", "dirname",
    "yes", "sleep", "uname", "cat", "ls", "printenv",
    "env", "hostname", "logname", "id", NULL
};

static int is_known_cmd(const char *name) {
    for (int i = 0; known_cmds[i]; i++) {
        if (strcmp(name, known_cmds[i]) == 0)
            return 1;
    }
    return 0;
}

static int parse_line(char *line, char **argv, int max) {
    int argc = 0;
    char *p = line;

    while (*p && argc < max - 1) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (!*p)
            break;

        if (*p == '"') {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"')
                p++;
            if (*p) *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
                p++;
            if (*p) *p++ = '\0';
        }
    }

    argv[argc] = NULL;
    return argc;
}

static void run_command(int argc, char **argv) {
    if (argc == 0)
        return;

    if (strcmp(argv[0], "exit") == 0) {
        shell_write("goodbye\n");
        sys_exit(0);
    }

    if (strcmp(argv[0], "cd") == 0) {
        const char *dir = argc > 1 ? argv[1] : "/";
        if (sys_chdir(dir) < 0) {
            shell_write("cd: cannot change to ");
            shell_write(dir);
            shell_write("\n");
        }
        return;
    }

    if (!is_known_cmd(argv[0])) {
        shell_write(argv[0]);
        shell_write(": command not found\n");
        return;
    }

    /* fork's enter_userspace only sets RAX — other registers are undefined in
     * the child. Copy argv strings into a stable buffer before fork. */
    char argbuf[MAX_LINE];
    char *saved_argv[MAX_ARGS];
    volatile int saved_argc = argc;
    if (saved_argc > MAX_ARGS)
        saved_argc = MAX_ARGS;
    {
        int off = 0;
        int n = saved_argc;
        for (int i = 0; i < n; i++) {
            const char *s = argv[i] ? argv[i] : "";
            int len = my_strlen(s);
            if (off + len + 1 >= MAX_LINE) {
                saved_argc = i;
                break;
            }
            saved_argv[i] = &argbuf[off];
            for (int j = 0; j <= len; j++)
                argbuf[off + j] = s[j];
            off += len + 1;
        }
    }

    int pid = sys_fork();
    if (pid < 0) {
        shell_write("fork failed\n");
        return;
    }

    if (pid == 0) {
        int ac = saved_argc; /* reload from memory — regs undefined after fork */
        char *new_argv[MAX_ARGS + 1];
        for (int i = 0; i < ac; i++)
            new_argv[i] = saved_argv[i];
        new_argv[ac] = NULL;

        int ret = sys_exec(CMDS_PATH, new_argv);
        if (ret < 0) {
            shell_write("exec failed\n");
            sys_exit(127);
        }
        sys_exit(0);
    }

    int status = 0;
    if (sys_waitpid(pid, &status, 0) < 0)
        log("[shell] waitpid failed\n");
}

void zsh_entry(void) {
    log("[shell] entry\n");

    port_handle_t my_input = sys_port_create();
    if (!my_input) {
        log("[shell] port create fail, idling\n");
        for (;;) syscall0(SYS_YIELD);
    }

    /* Wait until the terminal registers SHELL_BRIDGE (may be after GPU init). */
    port_handle_t bridge = 0;
    for (;;) {
        bridge = sys_ns_lookup(PORT_NS_SHELL_BRIDGE);
        if (bridge) break;
        syscall0(SYS_YIELD);
    }

    ipc_msg_t hello;
    for (size_t i = 0; i < sizeof(hello); i++) ((uint8_t *)&hello)[i] = 0;
    hello.type = IPC_MSG_REQUEST;
    hello.sender_pid = syscall0(SYS_PROC_PID);
    hello.cap_count = 0;
    hello.payload_len = sizeof(uint32_t) + sizeof(uint64_t);
    uint32_t hello_type = SHELL_BRIDGE_HELLO;
    for (size_t i = 0; i < sizeof(uint32_t); i++) hello.payload[i] = ((uint8_t *)&hello_type)[i];
    for (size_t i = 0; i < sizeof(uint64_t); i++) hello.payload[sizeof(uint32_t) + i] = ((uint8_t *)&my_input)[i];

    if (!sys_port_send(bridge, &hello)) {
        log("[shell] hello send fail\n");
        return;
    }

    set_shell_bridge(my_input, bridge);

    log("[shell] bridge active, starting shell\n");

    /* Seeded FS lives at /; ensure relative paths (ls .) resolve. */
    sys_chdir("/");

    shell_write("x-os shell ready\n");
    shell_write("Commands: echo pwd true false basename dirname yes sleep uname cat ls printenv hostname logname id cd exit\n");

    extern _ssize_t _write(int fd, const void *buf, size_t cnt);
    extern _ssize_t _read(int fd, void *buf, size_t cnt);

    const char prompt[] = "x> ";

    for (;;) {
        _write(1, prompt, sizeof(prompt) - 1);

        char line[MAX_LINE];
        int pos = 0;

        while (pos < MAX_LINE - 1) {
            char c;
            _ssize_t n = _read(0, &c, 1);
            if (n <= 0) {
                syscall0(SYS_YIELD);
                continue;
            }

            if (c == '\n' || c == '\r') {
                line[pos] = '\0';
                _write(1, "\n", 1);
                break;
            }

            if (c == 0x7f || c == '\b') {
                if (pos > 0) {
                    pos--;
                    _write(1, "\b \b", 3);
                }
                continue;
            }

            /* Echo printable input so the terminal stream shows typing. */
            if (c >= 0x20 && c < 0x7f) {
                line[pos++] = c;
                _write(1, &c, 1);
            }
        }

        line[pos] = '\0';

        char *argv[MAX_ARGS];
        int argc = parse_line(line, argv, MAX_ARGS);

        if (argc > 0) {
            run_command(argc, argv);
        }
    }
}
