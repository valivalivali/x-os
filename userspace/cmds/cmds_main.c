/* cmds_main.c — Multi-call binary for x-os shell commands.
 * Based on Apple's shell_cmds-329 (BSD-licensed).
 * Dispatches based on argv[0] basename (like busybox).
 *
 * Commands: echo, pwd, true, false, basename, dirname, yes, sleep, uname,
 *           cat, ls, env, printenv, hostname, logname, id
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>

#include "kernel/include/syscall.h"
#include "kernel/fs/xfs.h"

/* -------------------------------------------------------------------------- */
/* Dispatch table */

static int echo_main(int argc, char **argv);
static int pwd_main(int argc, char **argv);
static int true_main(int argc, char **argv);
static int false_main(int argc, char **argv);
static int basename_main(int argc, char **argv);
static int dirname_main(int argc, char **argv);
static int yes_main(int argc, char **argv);
static int sleep_main(int argc, char **argv);
static int uname_main(int argc, char **argv);
static int cat_main(int argc, char **argv);
static int ls_main(int argc, char **argv);
static int printenv_main(int argc, char **argv);
static int hostname_main(int argc, char **argv);
static int logname_main(int argc, char **argv);
static int id_main(int argc, char **argv);

struct cmd_entry {
    const char *name;
    int (*fn)(int argc, char **argv);
};

static const struct cmd_entry cmd_table[] = {
    { "echo",      echo_main },
    { "pwd",       pwd_main },
    { "true",      true_main },
    { "false",     false_main },
    { "basename",  basename_main },
    { "dirname",   dirname_main },
    { "yes",       yes_main },
    { "sleep",     sleep_main },
    { "uname",     uname_main },
    { "cat",       cat_main },
    { "ls",        ls_main },
    { "printenv",  printenv_main },
    { "env",       printenv_main },
    { "hostname",  hostname_main },
    { "logname",   logname_main },
    { "id",        id_main },
    { NULL, NULL }
};

/* Get basename of argv[0] */
static const char *base_name(const char *path) {
    const char *p = path;
    const char *last = path;
    for (; *p; p++) {
        if (*p == '/') last = p + 1;
    }
    return last;
}

int cmds_main(int argc, char **argv) {
    if (argc < 1 || !argv || !argv[0]) {
        fprintf(stderr, "cmds: no command name\n");
        return 1;
    }

    const char *cmd = base_name(argv[0]);

    for (const struct cmd_entry *e = cmd_table; e->name; e++) {
        if (strcmp(cmd, e->name) == 0) {
            return e->fn(argc, argv);
        }
    }

    fprintf(stderr, "cmds: unknown command '%s'\n", cmd);
    fprintf(stderr, "Available: echo pwd true false basename dirname yes sleep uname cat ls printenv hostname logname id\n");
    return 1;
}

/* -------------------------------------------------------------------------- */
/* echo — adapted from shell_cmds/echo/echo.c (BSD-3-Clause) */
/* Simplified: no wchar support, no POSIXLY_CORRECT mode */

static int echo_main(int argc, char **argv) {
    int nflag = 0;
    int i = 1;

    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        nflag = 1;
        i++;
    }

    for (; i < argc; i++) {
        if (i > 1 + nflag)
            putchar(' ');

        const char *s = argv[i];
        while (*s) {
            if (*s == '\\' && s[1]) {
                s++;
                switch (*s) {
                    case 'a':  putchar('\a'); break;
                    case 'b':  putchar('\b'); break;
                    case 'c':  return 0;  /* stop output */
                    case 'f':  putchar('\f'); break;
                    case 'n':  putchar('\n'); break;
                    case 'r':  putchar('\r'); break;
                    case 't':  putchar('\t'); break;
                    case 'v':  putchar('\v'); break;
                    case '\\': putchar('\\'); break;
                    case '0': {
                        int j = 0, num = 0;
                        while (s[1] >= '0' && s[1] <= '7' && j++ < 3) {
                            s++;
                            num <<= 3;
                            num |= (*s - '0');
                        }
                        putchar(num);
                        break;
                    }
                    default:
                        putchar('\\');
                        putchar(*s);
                        break;
                }
                s++;
            } else {
                putchar(*s++);
            }
        }
    }

    if (!nflag)
        putchar('\n');

    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* pwd — adapted from shell_cmds/pwd/pwd.c (BSD-3-Clause) */
/* Simplified: no logical/physical distinction, just getcwd */

static int pwd_main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    char buf[4096];
    if (getcwd(buf, sizeof(buf)) == NULL) {
        perror("pwd");
        return 1;
    }
    printf("%s\n", buf);
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* true — adapted from shell_cmds/true/true.c (BSD-3-Clause) */

static int true_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* false — adapted from shell_cmds/false/false.c (BSD-3-Clause) */

static int false_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 1;
}

/* -------------------------------------------------------------------------- */
/* basename — adapted from shell_cmds/basename/basename.c (BSD-3-Clause) */
/* Simplified: no wchar, no getopt, no -a flag */

static int basename_main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: basename string [suffix]\n");
        return 1;
    }

    char *p = argv[1];
    if (!*p) {
        printf("\n");
        return 0;
    }

    /* Strip trailing slashes */
    size_t len = strlen(p);
    while (len > 1 && p[len - 1] == '/')
        p[--len] = '\0';

    /* Find last slash */
    char *base = p;
    for (char *q = p; *q; q++) {
        if (*q == '/' && q[1])
            base = q + 1;
    }

    /* Strip suffix if provided */
    if (argc >= 3) {
        char *suffix = argv[2];
        size_t suflen = strlen(suffix);
        size_t baselen = strlen(base);
        if (baselen > suflen && strcmp(base + baselen - suflen, suffix) == 0) {
            base[baselen - suflen] = '\0';
        }
    }

    printf("%s\n", base);
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* dirname — adapted from shell_cmds/dirname/dirname.c (BSD-3-Clause) */

static int dirname_main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: dirname string [...]\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        char *p = argv[i];
        size_t len = strlen(p);

        /* Strip trailing slashes */
        while (len > 1 && p[len - 1] == '/')
            p[--len] = '\0';

        /* Find last slash */
        char *last = NULL;
        for (char *q = p; *q; q++) {
            if (*q == '/')
                last = q;
        }

        if (!last) {
            printf(".\n");
        } else if (last == p) {
            printf("/\n");
        } else {
            *last = '\0';
            /* Strip trailing slashes from result */
            size_t dlen = strlen(p);
            while (dlen > 1 && p[dlen - 1] == '/')
                p[--dlen] = '\0';
            printf("%s\n", p);
            *last = '/';  /* restore */
        }
    }
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* yes — adapted from shell_cmds/yes/yes.c (BSD-3-Clause) */

static int yes_main(int argc, char **argv) {
    const char *s = "y";
    if (argc > 1)
        s = argv[1];

    for (;;) {
        printf("%s\n", s);
        fflush(stdout);
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* sleep — adapted from shell_cmds/sleep/sleep.c (BSD-3-Clause) */
/* Simplified: no signal handling, no sub-second precision */

static int sleep_main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: sleep number[unit] ...\n");
        return 1;
    }

    unsigned int total_seconds = 0;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        char *end;
        long num = strtol(arg, &end, 10);

        if (end == arg) {
            fprintf(stderr, "sleep: invalid time interval: %s\n", arg);
            return 1;
        }

        if (*end) {
            switch (*end) {
                case 'd': num *= 24; /* fall through */
                case 'h': num *= 60; /* fall through */
                case 'm': num *= 60; /* fall through */
                case 's': break;
                default:
                    fprintf(stderr, "sleep: invalid unit '%c'\n", *end);
                    return 1;
            }
        }

        total_seconds += (unsigned int)num;
    }

    if (total_seconds == 0)
        return 0;

    struct timespec ts = { .tv_sec = total_seconds, .tv_nsec = 0 };
    nanosleep(&ts, NULL);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* uname — adapted from shell_cmds/uname/uname.c (BSD-4-Clause) */
/* Simplified: hardcoded values, no sysctl */

static int uname_main(int argc, char **argv) {
    const char *sysname  = "x-os";
    const char *hostname = "x-os";
    const char *release  = "1.0";
    const char *version  = "x-os 1.0 (Tahoe)";
    const char *machine  = "x86_64";

    int flags = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            fprintf(stderr, "usage: uname [-amnoprsv]\n");
            return 1;
        }
        for (const char *o = argv[i] + 1; *o; o++) {
            switch (*o) {
                case 'a':
                    flags |= 0x01 | 0x02 | 0x04 | 0x08 | 0x10 | 0x20;
                    break;
                case 'm': flags |= 0x01; break;
                case 'n': flags |= 0x02; break;
                case 'r': flags |= 0x04; break;
                case 's': case 'o': flags |= 0x08; break;
                case 'v': flags |= 0x10; break;
                case 'p': flags |= 0x20; break;
                default:
                    fprintf(stderr, "usage: uname [-amnoprsv]\n");
                    return 1;
            }
        }
    }

    if (!flags) flags = 0x08;  /* default: -s */

    int space = 0;
    if (flags & 0x08) { if (space) printf(" "); space++; printf("%s", sysname); }
    if (flags & 0x02) { if (space) printf(" "); space++; printf("%s", hostname); }
    if (flags & 0x04) { if (space) printf(" "); space++; printf("%s", release); }
    if (flags & 0x10) { if (space) printf(" "); space++; printf("%s", version); }
    if (flags & 0x01) { if (space) printf(" "); space++; printf("%s", machine); }
    if (flags & 0x20) { if (space) printf(" "); space++; printf("%s", machine); }
    printf("\n");
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* cat — simple implementation (reads files or stdin) */

static int cat_main(int argc, char **argv) {
    if (argc < 2) {
        /* Read from stdin */
        char buf[4096];
        ssize_t n;
        while ((n = read(0, buf, sizeof(buf))) > 0)
            write(1, buf, n);
        return 0;
    }

    int ret = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            ret = 1;
            continue;
        }
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, n);
        close(fd);
    }
    fflush(stdout);
    return ret;
}

/* -------------------------------------------------------------------------- */
/* ls — simple implementation (lists directory contents via XFS) */

static int ls_main(int argc, char **argv) {
    const char *path = ".";
    if (argc >= 2)
        path = argv[1];

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        /* Try as directory — XFS uses same open for dirs */
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }

    /* Use readdir syscall to list entries */
    xfs_dirent_t entries[64];
    int n = sys_readdir(fd, entries, 64);
    if (n < 0) {
        fprintf(stderr, "ls: %s: not a directory\n", path);
        close(fd);
        return 1;
    }

    for (int i = 0; i < n; i++) {
        if (entries[i].inode_block == 0)
            continue;
        if (entries[i].flags & 1)
            printf("%s/  ", entries[i].name);
        else
            printf("%s   ", entries[i].name);
    }
    if (n > 0)
        printf("\n");
    fflush(stdout);
    close(fd);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* printenv — adapted from shell_cmds/printenv (print environment) */

extern char **environ;

static int printenv_main(int argc, char **argv) {
    if (argc >= 2) {
        /* Print specific variable */
        const char *val = getenv(argv[1]);
        if (val) {
            printf("%s\n", val);
            fflush(stdout);
            return 0;
        }
        return 1;
    }

    /* Print all environment */
    if (environ) {
        for (char **e = environ; *e; e++)
            printf("%s\n", *e);
    }
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* hostname — print or set hostname */

static int hostname_main(int argc, char **argv) {
    char buf[256];
    if (argc >= 2) {
        /* Setting hostname not supported */
        fprintf(stderr, "hostname: cannot set hostname (not supported)\n");
        return 1;
    }
    if (gethostname(buf, sizeof(buf)) == 0) {
        printf("%s\n", buf);
    } else {
        printf("x-os\n");
    }
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* logname — print login name */

static int logname_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char *name = getlogin();
    if (name)
        printf("%s\n", name);
    else
        printf("root\n");
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* id — print user and group IDs */

static int id_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    uid_t uid = getuid();
    gid_t gid = getgid();
    printf("uid=%d(root) gid=%d(wheel)\n", uid, gid);
    fflush(stdout);
    return 0;
}
