/* cmds_main.c — Multi-call binary for x-os shell commands.
 * Based on Apple's shell_cmds-329 (BSD-licensed).
 * Dispatches based on argv[0] basename (like busybox).
 *
 * Commands: echo, pwd, true, false, basename, dirname, yes, sleep, uname,
 *           cat, ls, env, printenv, hostname, logname, id, date, seq, tee,
 *           test, printf, kill, wc, head, tail, sort, tr, uniq, cut, which,
 *           touch, mkdir, rm, cp, mv, grep, xargs, expr, false, true
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
#include <ctype.h>
#include <sys/wait.h>
#include <termios.h>
#include <sys/ioctl.h>

#include "kernel/include/syscall.h"
#include "kernel/include/ipc.h"
#include "kernel/fs/xfs.h"

/* When launched from the terminal shell, attach stdout/stderr to the bridge. */
extern void set_shell_bridge(port_handle_t input_port, port_handle_t output_port);

/* memmem is a GNU extension not available in newlib */
static void *memmem(const void *haystack, size_t haystacklen,
                    const void *needle, size_t needlelen) {
    if (needlelen == 0) return (void *)haystack;
    if (haystacklen < needlelen) return NULL;
    const char *h = (const char *)haystack;
    const char *n = (const char *)needle;
    for (size_t i = 0; i <= haystacklen - needlelen; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, needlelen) == 0)
            return (void *)(h + i);
    }
    return NULL;
}

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
static int date_main(int argc, char **argv);
static int seq_main(int argc, char **argv);
static int tee_main(int argc, char **argv);
static int test_main(int argc, char **argv);
static int printf_main(int argc, char **argv);
static int kill_main(int argc, char **argv);
static int wc_main(int argc, char **argv);
static int head_main(int argc, char **argv);
static int tail_main(int argc, char **argv);
static int sort_main(int argc, char **argv);
static int tr_main(int argc, char **argv);
static int uniq_main(int argc, char **argv);
static int cut_main(int argc, char **argv);
static int which_main(int argc, char **argv);
static int touch_main(int argc, char **argv);
static int mkdir_main(int argc, char **argv);
static int rm_main(int argc, char **argv);
static int cp_main(int argc, char **argv);
static int mv_main(int argc, char **argv);
static int grep_main(int argc, char **argv);
static int xargs_main(int argc, char **argv);
static int expr_main(int argc, char **argv);
static int chmod_main(int argc, char **argv);
static int ln_main(int argc, char **argv);
static int rmdir_main(int argc, char **argv);
static int dd_main(int argc, char **argv);
static int stat_main(int argc, char **argv);
static int cksum_main(int argc, char **argv);
static int du_main(int argc, char **argv);
static int mkfifo_main(int argc, char **argv);
static int chown_main(int argc, char **argv);
static int readlink_main(int argc, char **argv);
static int basename_cmd_main(int argc, char **argv);
static int sed_main(int argc, char **argv);
static int paste_main(int argc, char **argv);
static int fold_main(int argc, char **argv);
static int comm_main(int argc, char **argv);
static int nl_main(int argc, char **argv);
static int rev_main(int argc, char **argv);
static int expand_main(int argc, char **argv);
static int unexpand_main(int argc, char **argv);
static int colrm_main(int argc, char **argv);
static int split_main(int argc, char **argv);
static int csplit_main(int argc, char **argv);
static int less_main(int argc, char **argv);
static int vi_main(int argc, char **argv);
static int sudo_main(int argc, char **argv);
static int su_main(int argc, char **argv);

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
    { "date",      date_main },
    { "seq",       seq_main },
    { "tee",       tee_main },
    { "test",      test_main },
    { "[",         test_main },
    { "printf",    printf_main },
    { "kill",      kill_main },
    { "wc",        wc_main },
    { "head",      head_main },
    { "tail",      tail_main },
    { "sort",      sort_main },
    { "tr",        tr_main },
    { "uniq",      uniq_main },
    { "cut",       cut_main },
    { "which",     which_main },
    { "touch",     touch_main },
    { "mkdir",     mkdir_main },
    { "rm",        rm_main },
    { "cp",        cp_main },
    { "mv",        mv_main },
    { "grep",      grep_main },
    { "xargs",     xargs_main },
    { "expr",      expr_main },
    { "chmod",     chmod_main },
    { "ln",        ln_main },
    { "link",      ln_main },
    { "rmdir",     rmdir_main },
    { "dd",        dd_main },
    { "stat",      stat_main },
    { "readlink",  readlink_main },
    { "cksum",     cksum_main },
    { "du",        du_main },
    { "mkfifo",    mkfifo_main },
    { "chown",     chown_main },
    { "sed",       sed_main },
    { "paste",     paste_main },
    { "fold",      fold_main },
    { "comm",      comm_main },
    { "nl",        nl_main },
    { "rev",       rev_main },
    { "expand",    expand_main },
    { "unexpand",  unexpand_main },
    { "colrm",     colrm_main },
    { "split",     split_main },
    { "less",      less_main },
    { "more",      less_main },
    { "vi",        vi_main },
    { "vim",       vi_main },
    { "sudo",      sudo_main },
    { "su",        su_main },
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
    /* Exec clears the parent's bridge state — reattach so printf reaches the terminal. */
    {
        port_handle_t bridge = sys_ns_lookup(PORT_NS_SHELL_BRIDGE);
        if (bridge)
            set_shell_bridge(0, bridge);
    }

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
    fprintf(stderr, "Available: echo pwd true false basename dirname yes sleep uname cat ls env printenv hostname logname id date seq tee test printf kill wc head tail sort tr uniq cut which touch mkdir rm cp mv grep xargs expr chmod ln rmdir dd stat readlink cksum du mkfifo chown sed paste fold comm nl rev expand unexpand colrm split less more vi vim sudo su\n");
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

/* -------------------------------------------------------------------------- */
/* date — adapted from shell_cmds/date/date.c (BSD-3-Clause) */
/* Simplified: no strftime formatting, just prints current time */

static int date_main(int argc, char **argv) {
    time_t now;
    struct tm *tm;
    char buf[64];

    time(&now);
    tm = gmtime(&now);
    if (!tm) {
        printf("Thu Jan  1 00:00:00 UTC 2025\n");
        fflush(stdout);
        return 0;
    }

    const char *days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    const char *mons[] = {"Jan","Feb","Mar","Apr","May","Jun",
                          "Jul","Aug","Sep","Oct","Nov","Dec"};

    snprintf(buf, sizeof(buf), "%s %s %2d %02d:%02d:%02d UTC %d\n",
             days[tm->tm_wday], mons[tm->tm_mon], tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
    printf("%s", buf);
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* seq — adapted from shell_cmds/seq/seq.c (BSD-2-Clause-NetBSD) */

static int seq_main(int argc, char **argv) {
    double first = 1.0, last = 0.0, step = 1.0;
    const char *sep = "\n";
    const char *fmt = NULL;

    int i = 1;
    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0 &&
           (argv[i][1] < '0' || argv[i][1] > '9')) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            sep = argv[++i];
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            fmt = argv[++i];
        } else if (strcmp(argv[i], "-w") == 0) {
            /* equal width - not fully implemented */
        }
        i++;
    }

    int remaining = argc - i;
    if (remaining < 1 || remaining > 3) {
        fprintf(stderr, "usage: seq [-s sep] [-f fmt] [first [step]] last\n");
        return 1;
    }

    if (remaining == 1) {
        last = atof(argv[i]);
    } else if (remaining == 2) {
        first = atof(argv[i]);
        last = atof(argv[i + 1]);
    } else {
        first = atof(argv[i]);
        step = atof(argv[i + 1]);
        last = atof(argv[i + 2]);
    }

    if (step == 0.0) {
        fprintf(stderr, "seq: zero increment\n");
        return 1;
    }

    int first_print = 1;
    if (step > 0) {
        for (double v = first; v <= last + 1e-9; v += step) {
            if (!first_print) printf("%s", sep);
            if (fmt)
                printf(fmt, v);
            else
                printf("%g", v);
            first_print = 0;
        }
    } else {
        for (double v = first; v >= last - 1e-9; v += step) {
            if (!first_print) printf("%s", sep);
            if (fmt)
                printf(fmt, v);
            else
                printf("%g", v);
            first_print = 0;
        }
    }
    if (!first_print) printf("\n");
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* tee — adapted from shell_cmds/tee/tee.c (BSD-3-Clause) */

static int tee_main(int argc, char **argv) {
    int append = 0;
    int i = 1;

    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-a") == 0) {
            append = 1;
            i++;
        } else if (strcmp(argv[i], "-i") == 0) {
            /* ignore SIGINT - not supported */
            i++;
        } else {
            break;
        }
    }

    int nfiles = argc - i;
    int fds[16];
    int nfd = 0;

    for (int j = i; j < argc && nfd < 16; j++) {
        int flags = O_WRONLY | O_CREAT;
        flags |= append ? O_APPEND : O_TRUNC;
        int fd = open(argv[j], flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "tee: %s: cannot open\n", argv[j]);
        } else {
            fds[nfd++] = fd;
        }
    }

    char buf[4096];
    ssize_t n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        write(1, buf, n);
        for (int j = 0; j < nfd; j++)
            write(fds[j], buf, n);
    }

    for (int j = 0; j < nfd; j++)
        close(fds[j]);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* test — adapted from shell_cmds/test/test.c (Public Domain) */
/* Simplified: supports basic file tests and string/integer comparisons */

static int test_main(int argc, char **argv) {
    /* Handle [ ... ] syntax */
    const char *cmd = base_name(argv[0]);
    if (cmd[0] == '[') {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            fprintf(stderr, "test: missing ]\n");
            return 2;
        }
        argc--;
        argv[argc] = NULL;
    }

    if (argc == 1)
        return 1; /* no arguments = false */

    if (argc == 2) {
        /* unary: test string (true if non-empty) */
        return argv[1][0] ? 0 : 1;
    }

    if (argc == 3) {
        /* unary operator */
        const char *op = argv[1];
        const char *arg = argv[2];
        if (strcmp(op, "-z") == 0) return arg[0] == 0 ? 0 : 1;
        if (strcmp(op, "-n") == 0) return arg[0] != 0 ? 0 : 1;
        if (strcmp(op, "-f") == 0) {
            struct stat st;
            return (stat(arg, &st) == 0 && S_ISREG(st.st_mode)) ? 0 : 1;
        }
        if (strcmp(op, "-d") == 0) {
            struct stat st;
            return (stat(arg, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : 1;
        }
        if (strcmp(op, "-e") == 0) {
            struct stat st;
            return stat(arg, &st) == 0 ? 0 : 1;
        }
        if (strcmp(op, "-r") == 0) {
            struct stat st;
            return stat(arg, &st) == 0 ? 0 : 1;
        }
        if (strcmp(op, "-w") == 0) {
            struct stat st;
            return stat(arg, &st) == 0 ? 0 : 1;
        }
        if (strcmp(op, "-s") == 0) {
            struct stat st;
            return (stat(arg, &st) == 0 && st.st_size > 0) ? 0 : 1;
        }
        if (strcmp(op, "!") == 0) {
            return test_main(argc - 1, argv + 1) == 0 ? 1 : 0;
        }
        return 2;
    }

    if (argc == 4) {
        /* binary operator: A op B */
        const char *a = argv[1];
        const char *op = argv[2];
        const char *b = argv[3];

        if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0)
            return strcmp(a, b) == 0 ? 0 : 1;
        if (strcmp(op, "!=") == 0)
            return strcmp(a, b) != 0 ? 0 : 1;
        if (strcmp(op, "-eq") == 0)
            return atoi(a) == atoi(b) ? 0 : 1;
        if (strcmp(op, "-ne") == 0)
            return atoi(a) != atoi(b) ? 0 : 1;
        if (strcmp(op, "-lt") == 0)
            return atoi(a) < atoi(b) ? 0 : 1;
        if (strcmp(op, "-le") == 0)
            return atoi(a) <= atoi(b) ? 0 : 1;
        if (strcmp(op, "-gt") == 0)
            return atoi(a) > atoi(b) ? 0 : 1;
        if (strcmp(op, "-ge") == 0)
            return atoi(a) >= atoi(b) ? 0 : 1;
        if (strcmp(op, "-a") == 0) {
            int r1 = test_main(2, (char *[]){(char *)"", (char *)a, NULL});
            int r2 = test_main(2, (char *[]){(char *)"", (char *)b, NULL});
            return (r1 == 0 && r2 == 0) ? 0 : 1;
        }
        if (strcmp(op, "-o") == 0) {
            int r1 = test_main(2, (char *[]){(char *)"", (char *)a, NULL});
            int r2 = test_main(2, (char *[]){(char *)"", (char *)b, NULL});
            return (r1 == 0 || r2 == 0) ? 0 : 1;
        }
        return 2;
    }

    return 2;
}

/* -------------------------------------------------------------------------- */
/* printf — adapted from shell_cmds/printf/printf.c (BSD-3-Clause) */
/* Simplified: handles basic format specifiers, no %b */

static int printf_main(int argc, char **argv) {
    if (argc < 2) {
        return 0;
    }

    const char *fmt = argv[1];
    int argi = 2;
    const char *p = fmt;

    while (*p) {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n': putchar('\n'); break;
                case 't': putchar('\t'); break;
                case 'r': putchar('\r'); break;
                case '\\': putchar('\\'); break;
                case '0': putchar('\0'); break;
                default: putchar('\\'); putchar(*p); break;
            }
            if (*p) p++;
            continue;
        }
        if (*p == '%') {
            p++;
            if (*p == '%') { putchar('%'); p++; continue; }
            if (*p == 0) break;

            /* Parse format spec: flags, width, precision */
            char spec[64];
            int si = 0;
            spec[si++] = '%';
            while (*p && strchr("-+ #0", *p)) spec[si++] = *p++;
            while (*p && (*p >= '0' && *p <= '9')) spec[si++] = *p++;
            if (*p == '.') { spec[si++] = *p++; while (*p && (*p >= '0' && *p <= '9')) spec[si++] = *p++; }

            char conv = *p;
            spec[si++] = conv;
            spec[si] = 0;

            if (*p) p++;

            const char *arg = (argi < argc) ? argv[argi++] : "";

            switch (conv) {
                case 's': printf(spec, arg); break;
                case 'd': case 'i': printf(spec, atoi(arg)); break;
                case 'x': case 'X': case 'o': case 'u':
                    printf(spec, (unsigned int)strtoul(arg, NULL, 0)); break;
                case 'c': printf(spec, arg[0]); break;
                case 'f': case 'g': case 'e': case 'E':
                    printf(spec, atof(arg)); break;
                default: printf("%s", spec); break;
            }
        } else {
            putchar(*p);
            p++;
        }
    }
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* kill — adapted from shell_cmds/kill/kill.c (BSD-3-Clause) */

static int kill_main(int argc, char **argv) {
    int sig = 15; /* SIGTERM */
    int i = 1;

    if (argc < 2) {
        fprintf(stderr, "usage: kill [-signal] pid ...\n");
        return 1;
    }

    if (argv[1][0] == '-') {
        if (strcmp(argv[1], "-l") == 0) {
            printf("HUP INT QUIT ILL TRAP ABRT EMT FPE KILL BUS SEGV SYS PIPE ALRM TERM\n");
            fflush(stdout);
            return 0;
        }
        if (strcmp(argv[1], "-9") == 0 || strcmp(argv[1], "-KILL") == 0)
            sig = 9;
        else if (strcmp(argv[1], "-15") == 0 || strcmp(argv[1], "-TERM") == 0)
            sig = 15;
        else if (strcmp(argv[1], "-2") == 0 || strcmp(argv[1], "-INT") == 0)
            sig = 2;
        else if (strcmp(argv[1], "-1") == 0 || strcmp(argv[1], "-HUP") == 0)
            sig = 1;
        else
            sig = atoi(argv[1] + 1);
        i = 2;
    }

    int rc = 0;
    for (; i < argc; i++) {
        int pid = atoi(argv[i]);
        if (pid <= 0) {
            fprintf(stderr, "kill: invalid pid: %s\n", argv[i]);
            rc = 1;
            continue;
        }
        if (kill(pid, sig) < 0) {
            fprintf(stderr, "kill: %s: no such process\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}

/* -------------------------------------------------------------------------- */
/* wc — word count (BSD-3-Clause, adapted from text_cmds) */

static int wc_main(int argc, char **argv) {
    int total_lines = 0, total_words = 0, total_chars = 0;
    int i;

    if (argc < 2) {
        /* read stdin */
        char buf[4096];
        int lines = 0, words = 0, chars = 0, in_word = 0;
        ssize_t n;
        while ((n = read(0, buf, sizeof(buf))) > 0) {
            for (int j = 0; j < n; j++) {
                chars++;
                if (buf[j] == '\n') lines++;
                if (buf[j] == ' ' || buf[j] == '\t' || buf[j] == '\n' || buf[j] == '\r')
                    in_word = 0;
                else if (!in_word) { in_word = 1; words++; }
            }
        }
        printf("%7d %7d %7d\n", lines, words, chars);
        fflush(stdout);
        return 0;
    }

    for (i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "wc: %s: cannot open\n", argv[i]);
            continue;
        }
        int lines = 0, words = 0, chars = 0, in_word = 0;
        int c;
        while ((c = fgetc(f)) != EOF) {
            chars++;
            if (c == '\n') lines++;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                in_word = 0;
            else if (!in_word) { in_word = 1; words++; }
        }
        fclose(f);
        printf("%7d %7d %7d %s\n", lines, words, chars, argv[i]);
        total_lines += lines;
        total_words += words;
        total_chars += chars;
    }

    if (argc > 2)
        printf("%7d %7d %7d total\n", total_lines, total_words, total_chars);
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* head — print first N lines (BSD-3-Clause, adapted from text_cmds) */

static int head_main(int argc, char **argv) {
    int nlines = 10;
    int i = 1;

    if (argc >= 2 && strcmp(argv[1], "-n") == 0 && argc >= 3) {
        nlines = atoi(argv[2]);
        i = 3;
    } else if (argc >= 2 && argv[1][0] == '-' && argv[1][1] >= '0' && argv[1][1] <= '9') {
        nlines = atoi(argv[1] + 1);
        i = 2;
    }

    for (; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "head: %s: cannot open\n", argv[i]);
            continue;
        }
        int c, line = 0;
        while (line < nlines && (c = fgetc(f)) != EOF) {
            putchar(c);
            if (c == '\n') line++;
        }
        fclose(f);
    }

    if (i == 1) {
        /* read from stdin */
        int c, line = 0;
        while (line < nlines && (c = getchar()) != EOF) {
            putchar(c);
            if (c == '\n') line++;
        }
    }
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* tail — print last N lines (BSD-3-Clause, adapted from text_cmds) */

static int tail_main(int argc, char **argv) {
    int nlines = 10;
    int i = 1;

    if (argc >= 2 && strcmp(argv[1], "-n") == 0 && argc >= 3) {
        nlines = atoi(argv[2]);
        i = 3;
    } else if (argc >= 2 && argv[1][0] == '-' && argv[1][1] >= '0' && argv[1][1] <= '9') {
        nlines = atoi(argv[1] + 1);
        i = 2;
    }

    /* Read all lines into a circular buffer */
    char **lines = malloc(sizeof(char *) * (nlines + 1));
    if (!lines) return 1;
    for (int j = 0; j <= nlines; j++) lines[j] = NULL;
    int idx = 0, count = 0;
    char buf[4096];
    int bi = 0;

    FILE *f = NULL;
    if (i < argc) {
        f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "tail: %s: cannot open\n", argv[i]);
            free(lines);
            return 1;
        }
    }

    int c;
    while (1) {
        if (f) c = fgetc(f);
        else c = getchar();
        if (c == EOF) break;
        if (bi < (int)sizeof(buf) - 1) buf[bi++] = c;
        if (c == '\n') {
            buf[bi] = 0;
            if (lines[idx]) free(lines[idx]);
            lines[idx] = strdup(buf);
            idx = (idx + 1) % (nlines + 1);
            if (count < nlines) count++;
            bi = 0;
        }
    }
    if (bi > 0) {
        buf[bi] = 0;
        if (lines[idx]) free(lines[idx]);
        lines[idx] = strdup(buf);
        idx = (idx + 1) % (nlines + 1);
        if (count < nlines) count++;
    }

    int start = (idx - count + (nlines + 1)) % (nlines + 1);
    for (int j = 0; j < count; j++) {
        int pos = (start + j) % (nlines + 1);
        if (lines[pos]) printf("%s", lines[pos]);
    }

    for (int j = 0; j <= nlines; j++) if (lines[j]) free(lines[j]);
    free(lines);
    if (f) fclose(f);
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* sort — sort lines (BSD-3-Clause, adapted from text_cmds) */

static int sort_cmp(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

static int sort_main(int argc, char **argv) {
    int reverse = 0, i = 1;

    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-r") == 0) reverse = 1;
        i++;
    }

    char **lines = NULL;
    int nlines = 0, cap = 64;
    lines = malloc(sizeof(char *) * cap);
    if (!lines) return 1;

    char buf[4096];
    int bi = 0;
    FILE *f = NULL;
    if (i < argc) {
        f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "sort: %s: cannot open\n", argv[i]);
            free(lines);
            return 1;
        }
    }

    int c;
    while (1) {
        if (f) c = fgetc(f); else c = getchar();
        if (c == EOF) break;
        if (bi < (int)sizeof(buf) - 1) buf[bi++] = c;
        if (c == '\n') {
            buf[bi] = 0;
            if (nlines >= cap) { cap *= 2; lines = realloc(lines, sizeof(char *) * cap); }
            lines[nlines++] = strdup(buf);
            bi = 0;
        }
    }
    if (bi > 0) { buf[bi] = 0; lines[nlines++] = strdup(buf); }

    qsort(lines, nlines, sizeof(char *), sort_cmp);

    for (int j = 0; j < nlines; j++) {
        int idx = reverse ? nlines - 1 - j : j;
        printf("%s", lines[idx]);
        free(lines[j]);
    }
    free(lines);
    if (f) fclose(f);
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* tr — translate characters (BSD-3-Clause, adapted from text_cmds) */

static int tr_main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: tr string1 string2\n");
        return 1;
    }

    const char *set1 = argv[1];
    const char *set2 = argv[2];
    int len2 = strlen(set2);

    int c;
    while ((c = getchar()) != EOF) {
        const char *pos = strchr(set1, c);
        if (pos && len2 > 0) {
            int idx = pos - set1;
            if (idx < len2)
                putchar(set2[idx]);
            else
                putchar(set2[len2 - 1]);
        } else {
            putchar(c);
        }
    }
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* uniq — remove duplicate adjacent lines (BSD-3-Clause, from text_cmds) */

static int uniq_main(int argc, char **argv) {
    char prev[4096] = "";
    int have_prev = 0;
    char buf[4096];
    int bi = 0;
    int c;

    while ((c = getchar()) != EOF) {
        if (bi < (int)sizeof(buf) - 1) buf[bi++] = c;
        if (c == '\n') {
            buf[bi] = 0;
            if (!have_prev || strcmp(buf, prev) != 0) {
                printf("%s", buf);
                strncpy(prev, buf, sizeof(prev) - 1);
                prev[sizeof(prev) - 1] = 0;
                have_prev = 1;
            }
            bi = 0;
        }
    }
    if (bi > 0) {
        buf[bi] = 0;
        if (!have_prev || strcmp(buf, prev) != 0)
            printf("%s", buf);
    }
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* cut — cut fields or characters (BSD-3-Clause, from text_cmds) */

static int cut_main(int argc, char **argv) {
    char delim = '\t';
    int char_mode = 0;
    int fields[64];
    int nfields = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            delim = argv[++i][0];
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            char_mode = 1;
            i++;
            /* parse positions like "1-3,5" - simplified to single range */
            fields[0] = 1; fields[1] = atoi(argv[i]);
            nfields = 2;
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            i++;
            char *p = argv[i];
            while (*p && nfields < 64) {
                fields[nfields++] = atoi(p) - 1;
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
        } else {
            break;
        }
    }

    char buf[4096];
    int bi = 0;
    int c;
    FILE *f = NULL;
    if (i < argc) {
        f = fopen(argv[i], "r");
        if (!f) { fprintf(stderr, "cut: %s: cannot open\n", argv[i]); return 1; }
    }

    while (1) {
        if (f) c = fgetc(f); else c = getchar();
        if (c == EOF) break;
        if (bi < (int)sizeof(buf) - 1) buf[bi++] = c;
        if (c == '\n') {
            buf[bi] = 0;
            if (char_mode) {
                int start = fields[0] - 1;
                int end = fields[1] - 1;
                if (end <= 0) end = start;
                int len = strlen(buf);
                for (int j = start; j <= end && j < len; j++)
                    putchar(buf[j]);
                putchar('\n');
            } else {
                char *p = buf;
                int field = 0;
                while (*p) {
                    char *next = strchr(p, delim);
                    if (!next) next = p + strlen(p);
                    for (int j = 0; j < nfields; j++) {
                        if (field == fields[j]) {
                            fwrite(p, 1, next - p, stdout);
                            if (j < nfields - 1) putchar(delim);
                        }
                    }
                    field++;
                    if (*next == 0) break;
                    p = next + 1;
                }
                putchar('\n');
            }
            bi = 0;
        }
    }
    if (f) fclose(f);
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* which — find command in PATH (adapted from shell_cmds/which) */

static int which_main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: which command ...\n");
        return 1;
    }

    const char *path = getenv("PATH");
    if (!path) path = "/bin:/usr/bin";

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char pbuf[512];
        const char *p = path;
        int found = 0;
        while (*p && !found) {
            const char *next = strchr(p, ':');
            int plen = next ? (int)(next - p) : (int)strlen(p);
            if (plen > 0) {
                snprintf(pbuf, sizeof(pbuf), "%.*s/%s", plen, p, argv[i]);
                struct stat st;
                if (stat(pbuf, &st) == 0) {
                    printf("%s\n", pbuf);
                    found = 1;
                }
            }
            p = next ? next + 1 : p + plen;
        }
        if (!found) {
            printf("%s not found\n", argv[i]);
            rc = 1;
        }
    }
    fflush(stdout);
    return rc;
}

/* -------------------------------------------------------------------------- */
/* touch — create empty file or update timestamps (from file_cmds) */

static int touch_main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: touch file ...\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
        if (fd < 0) {
            fprintf(stderr, "touch: %s: cannot create\n", argv[i]);
            rc = 1;
        } else {
            close(fd);
        }
    }
    return rc;
}

/* -------------------------------------------------------------------------- */
/* mkdir — create directory (from file_cmds) */

static int mkdir_main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: mkdir dir ...\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (mkdir(argv[i], 0755) < 0) {
            fprintf(stderr, "mkdir: %s: cannot create\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}

/* -------------------------------------------------------------------------- */
/* rm — remove files (from file_cmds) */
/* Simplified: no -r, no -f, no -i */

static int rm_main(int argc, char **argv) {
    int force = 0;
    int i = 1;

    while (i < argc && argv[i][0] == '-') {
        if (strchr(argv[i], 'f')) force = 1;
        i++;
    }

    int rc = 0;
    for (; i < argc; i++) {
        if (unlink(argv[i]) < 0 && !force) {
            fprintf(stderr, "rm: %s: cannot remove\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}

/* -------------------------------------------------------------------------- */
/* cp — copy files (from file_cmds) */
/* Simplified: no -r, no -p */

static int cp_main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: cp src dst\n");
        return 1;
    }

    int src = open(argv[1], O_RDONLY);
    if (src < 0) {
        fprintf(stderr, "cp: %s: cannot open\n", argv[1]);
        return 1;
    }

    int dst = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst < 0) {
        fprintf(stderr, "cp: %s: cannot create\n", argv[2]);
        close(src);
        return 1;
    }

    char buf[4096];
    ssize_t n;
    while ((n = read(src, buf, sizeof(buf))) > 0) {
        if (write(dst, buf, n) != n) {
            fprintf(stderr, "cp: write error\n");
            close(src); close(dst);
            return 1;
        }
    }
    close(src);
    close(dst);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* mv — move/rename files (from file_cmds) */

static int mv_main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: mv src dst\n");
        return 1;
    }

    /* Try rename first */
    if (rename(argv[1], argv[2]) == 0)
        return 0;

    /* Fall back to copy + unlink */
    int rc = cp_main(3, (char *[]){"cp", argv[1], argv[2], NULL});
    if (rc != 0) return rc;
    unlink(argv[1]);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* grep — search for pattern (BSD-3-Clause, from text_cmds) */

static int grep_main(int argc, char **argv) {
    int case_insensitive = 0;
    int invert = 0;
    int line_numbers = 0;
    int i = 1;

    while (i < argc && argv[i][0] == '-' && argv[i][1]) {
        if (strchr(argv[i], 'i')) case_insensitive = 1;
        if (strchr(argv[i], 'v')) invert = 1;
        if (strchr(argv[i], 'n')) line_numbers = 1;
        i++;
    }

    if (i >= argc) {
        fprintf(stderr, "usage: grep [-ivn] pattern [file ...]\n");
        return 2;
    }

    const char *pattern = argv[i++];

    FILE *f = NULL;
    if (i < argc) {
        f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "grep: %s: cannot open\n", argv[i]);
            return 2;
        }
    }

    char buf[4096];
    int lineno = 0;
    int found = 0;

    while (fgets(buf, sizeof(buf), f ? f : stdin)) {
        lineno++;
        int match = 0;
        if (case_insensitive) {
            char lbuf[4096], lpat[256];
            int j;
            for (j = 0; buf[j] && j < (int)sizeof(lbuf) - 1; j++)
                lbuf[j] = tolower((unsigned char)buf[j]);
            lbuf[j] = 0;
            for (j = 0; pattern[j] && j < (int)sizeof(lpat) - 1; j++)
                lpat[j] = tolower((unsigned char)pattern[j]);
            lpat[j] = 0;
            match = strstr(lbuf, lpat) != NULL;
        } else {
            match = strstr(buf, pattern) != NULL;
        }

        if (match != invert) {
            if (line_numbers)
                printf("%d:%s", lineno, buf);
            else
                printf("%s", buf);
            found = 1;
        }
    }

    if (f) fclose(f);
    fflush(stdout);
    return found ? 0 : 1;
}

/* -------------------------------------------------------------------------- */
/* xargs — build and execute commands from stdin (from shell_cmds) */

static int xargs_main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: xargs command [args...]\n");
        return 1;
    }

    char *cmd = argv[1];
    char *args[64];
    int nargs = 0;

    /* Copy initial args */
    for (int i = 2; i < argc && nargs < 60; i++)
        args[nargs++] = argv[i];

    /* Read tokens from stdin */
    char buf[4096];
    int bi = 0;
    int c;
    while ((c = getchar()) != EOF) {
        if (bi < (int)sizeof(buf) - 1) buf[bi++] = c;
    }
    buf[bi] = 0;

    char *p = buf;
    while (*p && nargs < 62) {
        while (*p && strchr(" \t\n\r", *p)) p++;
        if (!*p) break;
        char *start = p;
        while (*p && !strchr(" \t\n\r", *p)) p++;
        if (*p) { *p = 0; p++; }
        args[nargs++] = start;
    }
    args[nargs] = NULL;

    /* Fork and exec */
    pid_t pid = fork();
    if (pid == 0) {
        execv(cmd, args);
        fprintf(stderr, "xargs: %s: exec failed\n", cmd);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* expr — evaluate expression (from shell_cmds) */
/* Simplified: basic arithmetic and string operations */

static int expr_main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: expr expression\n");
        return 2;
    }

    /* Very simplified: handle integer arithmetic with +, -, *, /, % */
    /* Also handle string match : */
    if (argc == 4) {
        long a = atol(argv[1]);
        const char *op = argv[2];
        long b = atol(argv[3]);

        if (strcmp(op, "+") == 0) { printf("%ld\n", a + b); fflush(stdout); return 0; }
        if (strcmp(op, "-") == 0) { printf("%ld\n", a - b); fflush(stdout); return 0; }
        if (strcmp(op, "*") == 0) { printf("%ld\n", a * b); fflush(stdout); return 0; }
        if (strcmp(op, "/") == 0) { if (b == 0) { fprintf(stderr, "expr: division by zero\n"); return 2; } printf("%ld\n", a / b); fflush(stdout); return 0; }
        if (strcmp(op, "%") == 0) { if (b == 0) { fprintf(stderr, "expr: division by zero\n"); return 2; } printf("%ld\n", a % b); fflush(stdout); return 0; }
        if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) { int r = a == b; printf("%d\n", r); fflush(stdout); return r ? 0 : 1; }
        if (strcmp(op, "!=") == 0) { int r = a != b; printf("%d\n", r); fflush(stdout); return r ? 0 : 1; }
        if (strcmp(op, "<") == 0) { int r = a < b; printf("%d\n", r); fflush(stdout); return r ? 0 : 1; }
        if (strcmp(op, "<=") == 0) { int r = a <= b; printf("%d\n", r); fflush(stdout); return r ? 0 : 1; }
        if (strcmp(op, ">") == 0) { int r = a > b; printf("%d\n", r); fflush(stdout); return r ? 0 : 1; }
        if (strcmp(op, ">=") == 0) { int r = a >= b; printf("%d\n", r); fflush(stdout); return r ? 0 : 1; }
    }

    /* String comparison with : */
    if (argc == 4 && strcmp(argv[2], ":") == 0) {
        /* Basic: return length of match if argv[1] starts with argv[3] */
        const char *s = argv[1];
        const char *p = argv[3];
        int len = 0;
        while (p[len] && s[len] == p[len]) len++;
        printf("%d\n", p[len] ? 0 : len);
        fflush(stdout);
        return 0;
    }

    /* Single argument: just print it */
    if (argc == 2) {
        printf("%s\n", argv[1]);
        fflush(stdout);
        return 0;
    }

    fprintf(stderr, "expr: too complex expression\n");
    return 2;
}

/* -------------------------------------------------------------------------- */
/* chmod — change file modes (adapted from file_cmds-479/chmod/chmod.c, BSD-3) */

static int chmod_main(int argc, char **argv) {
    int recursive = 0;
    int verbose = 0;
    int i = 1;

    while (i < argc && argv[i][0] == '-' && argv[i][1]) {
        if (strchr(argv[i], 'R')) recursive = 1;
        if (strchr(argv[i], 'v')) verbose = 1;
        i++;
    }

    if (i + 1 >= argc) {
        fprintf(stderr, "usage: chmod [-Rv] mode file ...\n");
        return 1;
    }

    const char *mode_str = argv[i++];
    int mode;

    /* Parse mode: octal or symbolic */
    if (mode_str[0] >= '0' && mode_str[0] <= '7') {
        mode = (int)strtol(mode_str, NULL, 8);
    } else {
        /* Symbolic: u+rwx, go-w, etc. — simplified */
        mode = 0644;  /* default */
        if (strchr(mode_str, 'x')) mode |= 0111;
        if (strstr(mode_str, "777") || strstr(mode_str, "a+rwx")) mode = 0777;
        if (strstr(mode_str, "755")) mode = 0755;
        if (strstr(mode_str, "644")) mode = 0644;
        if (strstr(mode_str, "600")) mode = 0600;
        if (strstr(mode_str, "444")) mode = 0444;
    }

    int rc = 0;
    for (; i < argc; i++) {
        if (chmod(argv[i], mode) < 0) {
            fprintf(stderr, "chmod: %s: cannot change mode\n", argv[i]);
            rc = 1;
        } else if (verbose) {
            printf("chmod: %s: mode changed to %o\n", argv[i], mode);
        }
    }
    fflush(stdout);
    return rc;
}

/* -------------------------------------------------------------------------- */
/* ln — link files (adapted from file_cmds-479/ln/ln.c, BSD-3) */

static int ln_main(int argc, char **argv) {
    int symbolic = 0;
    int force = 0;
    int i = 1;

    while (i < argc && argv[i][0] == '-' && argv[i][1]) {
        if (strchr(argv[i], 's')) symbolic = 1;
        if (strchr(argv[i], 'f')) force = 1;
        i++;
    }

    if (i + 1 >= argc) {
        fprintf(stderr, "usage: ln [-sf] src [src] ... dst\n");
        return 1;
    }

    /* If dst is a directory or multiple sources, link into it */
    int nsrc = argc - i - 1;
    const char *dst = argv[argc - 1];
    int rc = 0;

    for (int j = i; j < argc - 1; j++) {
        char dstpath[512];
        const char *src = argv[j];

        if (nsrc > 1) {
            /* Link into directory */
            const char *base = base_name(src);
            snprintf(dstpath, sizeof(dstpath), "%s/%s", dst, base);
        } else {
            snprintf(dstpath, sizeof(dstpath), "%s", dst);
        }

        if (force) unlink(dstpath);

        int ret;
        if (symbolic)
            ret = symlink(src, dstpath);
        else
            ret = link(src, dstpath);

        if (ret < 0) {
            fprintf(stderr, "ln: %s -> %s: failed\n", src, dstpath);
            rc = 1;
        }
    }
    return rc;
}

/* -------------------------------------------------------------------------- */
/* rmdir — remove directories (adapted from file_cmds-479) */

static int rmdir_main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: rmdir dir ...\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (rmdir(argv[i]) < 0) {
            fprintf(stderr, "rmdir: %s: cannot remove\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}

/* -------------------------------------------------------------------------- */
/* dd — convert and copy files (adapted from file_cmds-479/dd/dd.c, BSD-3) */
/* Simplified: supports bs=, count=, skip=, seek=, if=, of= */

static int dd_main(int argc, char **argv) {
    const char *infile = NULL;
    const char *outfile = NULL;
    int bs = 512;
    int count = -1;
    int skip = 0;
    int seekb = 0;
    int conv_sync = 0;
    int conv_noerror = 0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strncmp(arg, "if=", 3) == 0) {
            infile = arg + 3;
        } else if (strncmp(arg, "of=", 3) == 0) {
            outfile = arg + 3;
        } else if (strncmp(arg, "bs=", 3) == 0) {
            bs = atoi(arg + 3);
            if (bs <= 0) bs = 512;
        } else if (strncmp(arg, "count=", 6) == 0) {
            count = atoi(arg + 6);
        } else if (strncmp(arg, "skip=", 5) == 0) {
            skip = atoi(arg + 5);
        } else if (strncmp(arg, "seek=", 5) == 0) {
            seekb = atoi(arg + 5);
        } else if (strncmp(arg, "conv=", 5) == 0) {
            if (strstr(arg + 5, "sync")) conv_sync = 1;
            if (strstr(arg + 5, "noerror")) conv_noerror = 1;
        }
    }

    int infd = 0, outfd = 1;
    if (infile) {
        infd = open(infile, O_RDONLY);
        if (infd < 0) {
            fprintf(stderr, "dd: %s: cannot open\n", infile);
            return 1;
        }
    }
    if (outfile) {
        outfd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (outfd < 0) {
            fprintf(stderr, "dd: %s: cannot open\n", outfile);
            if (infd > 2) close(infd);
            return 1;
        }
    }

    /* Skip input blocks */
    if (skip > 0) {
        char *buf = malloc(bs);
        for (int s = 0; s < skip; s++) {
            ssize_t n = read(infd, buf, bs);
            if (n <= 0) break;
        }
        free(buf);
    }

    /* Seek output blocks */
    if (seekb > 0) {
        char *buf = malloc(bs);
        memset(buf, 0, bs);
        for (int s = 0; s < seekb; s++)
            write(outfd, buf, bs);
        free(buf);
    }

    char *buf = malloc(bs);
    if (!buf) return 1;
    int blocks = 0;
    int partial = 0;

    while (count == -1 || blocks < count) {
        ssize_t n = read(infd, buf, bs);
        if (n <= 0) break;
        if (n < bs && conv_sync) {
            memset(buf + n, 0, bs - n);
            n = bs;
        }
        ssize_t w = write(outfd, buf, n);
        if (w < 0) {
            if (conv_noerror) continue;
            break;
        }
        if (n < bs) { partial++; break; }
        blocks++;
    }

    free(buf);
    if (infd > 2) close(infd);
    if (outfd > 2) close(outfd);

    fprintf(stderr, "%d+%d records in\n%d+%d records out\n",
            blocks, partial, blocks, partial);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* stat — display file status (adapted from file_cmds-479/stat/stat.c, BSD-3) */

static int stat_main(int argc, char **argv) {
    int format_flag = 0;
    int i = 1;

    if (argc >= 2 && strcmp(argv[1], "-f") == 0 && argc >= 3) {
        format_flag = 1;
        i = 3;
    } else if (argc >= 2 && strcmp(argv[1], "-l") == 0) {
        i = 2;
    }

    if (i >= argc) {
        fprintf(stderr, "usage: stat [-f format] file ...\n");
        return 1;
    }

    int rc = 0;
    for (; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            fprintf(stderr, "stat: %s: not found\n", argv[i]);
            rc = 1;
            continue;
        }

        if (format_flag) {
            /* Simplified format: just print size */
            printf("%lld\n", (long long)st.st_size);
        } else {
            printf("  File: %s\n", argv[i]);
            printf("  Size: %lld\t", (long long)st.st_size);
            printf("Mode: %o\n", st.st_mode);
            printf("Inode: %lu\tLinks: %lu\n",
                   (unsigned long)st.st_ino, (unsigned long)st.st_nlink);
            printf("Uid: %d\tGid: %d\n", st.st_uid, st.st_gid);
        }
    }
    fflush(stdout);
    return rc;
}

/* -------------------------------------------------------------------------- */
/* readlink — print symlink target (from file_cmds-479/stat) */

static int readlink_main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: readlink file\n");
        return 1;
    }

    char buf[4096];
    ssize_t n = readlink(argv[1], buf, sizeof(buf) - 1);
    if (n < 0) {
        fprintf(stderr, "readlink: %s: not a symlink\n", argv[1]);
        return 1;
    }
    buf[n] = 0;
    printf("%s\n", buf);
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* cksum — checksum (adapted from file_cmds-479/cksum, BSD-3) */

static const unsigned long crc_table[] = {
    0x00000000, 0x04c11db7, 0x09823b6e, 0x0d4326d9,
    0x130476dc, 0x17c56b6b, 0x1a864db2, 0x1e475005,
    0x2608edb8, 0x22c9f00f, 0x2f8ad6d6, 0x2b4bcb61,
    0x350c9b62, 0x31cd86d5, 0x3c8ea00a, 0x384fbdbd,
    0x4c11db70, 0x48d0c6c7, 0x4593e01e, 0x4152fda9,
    0x5f15adac, 0x5bd4b01b, 0x569796c2, 0x52568b75,
    0x6a1936c8, 0x6ed82b7f, 0x639b0da6, 0x675a1011,
    0x791d4014, 0x7ddc5da3, 0x709f7b7a, 0x745e66cd,
    0x9823b6e0, 0x9ce2ab57, 0x91a18d8e, 0x95609039,
    0x8b27c03c, 0x8fe6dd8b, 0x82a5fb52, 0x8664e6e5,
    0xbe2b5b58, 0xbaea46ef, 0xb7a96036, 0xb3687d81,
    0xad2f2d84, 0xa9ee3033, 0xa4ad16ea, 0xa06c275d,
    0xd4326d90, 0xd0f37027, 0xddb056fe, 0xd9714b49,
    0xc7361b4c, 0xc3f706fb, 0xceb42022, 0xca753d95,
    0xf23a8028, 0xf6fb9d9f, 0xfbb8bb46, 0xff79a6f1,
    0xe13ef6f4, 0xe5ffeb43, 0xe8bccd9a, 0xec7dd02d,
    0x34867077, 0x30476dc0, 0x3d044b19, 0x39c556ae,
    0x278206ab, 0x23431b1c, 0x2e003dc5, 0x2ac12072,
    0x128e9dcf, 0x164f8078, 0x1b0ca6a1, 0x1fcdbb16,
    0x018aeb13, 0x054bf6a4, 0x0808d07d, 0x0cc9cdca,
    0x7897ab07, 0x7c56b6b0, 0x71159069, 0x75d48dde,
    0x6b93dddb, 0x6f52c06c, 0x6211e6b5, 0x66d0fb02,
    0x5e9f46bf, 0x5a5e5b08, 0x571d7dd1, 0x53dc6066,
    0x4d9b3063, 0x495a2dd4, 0x44190b0d, 0x40d816ba,
    0xaca5c697, 0xa864db20, 0xa527fdf9, 0xa1e6e04e,
    0xbfa1b04b, 0xbb60adfc, 0xb6238b25, 0xb2e29692,
    0x8aad2b0f, 0x8e6c3698, 0x832f1041, 0x87ee0df6,
    0x99a95df3, 0x9d684044, 0x902b669d, 0x94ea7b2a,
    0xe0b41de7, 0xe4750050, 0xe9362689, 0xedf73b3e,
    0xf3b06b3b, 0xf771768c, 0xfa325055, 0xfef34de2,
    0xc6bcf05f, 0xc27dede8, 0xcf3ecb31, 0xcbffd686,
    0xd5b88683, 0xd1799b34, 0xdc3abded, 0xd8fba05a,
    0x690ce0ee, 0x6dcdfd59, 0x608edb80, 0x644fc637,
    0x7a089632, 0x7ec98b85, 0x738aad5c, 0x774bb0eb,
    0x4f040d56, 0x4bc510e1, 0x46863638, 0x42472b8f,
    0x5c007b8a, 0x58c1663d, 0x558240e4, 0x51435d53,
    0x251d3b9e, 0x21dc2629, 0x2c9f00f0, 0x285e1d47,
    0x36194d42, 0x32d850f5, 0x3f9b762c, 0x3b5a6b9b,
    0x0315d626, 0x07d4cb91, 0x0a97ed48, 0x0e56f0ff,
    0x1011a0fa, 0x14d0bd4d, 0x19939b94, 0x1d528623,
    0xf12f560e, 0xf5ee4bb9, 0xf8ad6d60, 0xfc6c70d7,
    0xe22b20d2, 0xe6ea3d65, 0xeba91bbc, 0xef68060b,
    0xd727bbb6, 0xd3e6a601, 0xdea580d8, 0xda649d6f,
    0xc423cd6a, 0xc0e2d0dd, 0xcda1f604, 0xc960ebb3,
    0xbd3e8d7e, 0xb9ff90c9, 0xb4bcb610, 0xb07daba7,
    0xae3afba2, 0xaafbe615, 0xa7b8c0cc, 0xa379dd7b,
    0x9b3660c6, 0x9ff77d71, 0x92b45ba8, 0x9675461f,
    0x8832161a, 0x8cf30bad, 0x81b02d74, 0x857130c3,
    0x5d8a9099, 0x594b8d2e, 0x5408abf7, 0x50c9b640,
    0x4e8ee645, 0x4a4ffbf2, 0x470cdd6b, 0x43cdc09c,
    0x7b827d21, 0x7f436096, 0x7200464f, 0x76c15bf8,
    0x68860bfd, 0x6c47164a, 0x61043093, 0x65c52d24,
    0x119b4be9, 0x155a565e, 0x18197087, 0x1cd86d30,
    0x029f3d35, 0x065e2082, 0x0b1d065b, 0x0fdc1bec,
    0x3793a651, 0x3352bbe6, 0x3e119d3f, 0x3ad08088,
    0x2497d08d, 0x2056cd3a, 0x2d15ebe3, 0x29d4f654,
    0xc5a92679, 0xc1683bce, 0xcc2b1d17, 0xc8ea00a0,
    0xd6ad50a5, 0xd26c4d12, 0xdf2f6bcb, 0xdbee767c,
    0xe3a1cbc1, 0xe760d676, 0xea23f0af, 0xeee2ed18,
    0xf0a5bd1d, 0xf464a0aa, 0xf9278673, 0xfde69bc4,
    0x89b8fd09, 0x8d79e0be, 0x803ac667, 0x84fbdbd0,
    0x9abc8bd5, 0x9e7d9662, 0x933eb0bb, 0x97ffad0c,
    0xafb010b1, 0xab710d06, 0xa6322bdf, 0xa2f33668,
    0xbcb4666d, 0xb8757bda, 0xb5365d03, 0xb1f740b4
};

static int cksum_main(int argc, char **argv) {
    if (argc < 2) {
        /* Read from stdin */
        unsigned long crc = 0;
        unsigned long bytes = 0;
        int c;
        while ((c = getchar()) != EOF) {
            crc = (crc << 8) ^ crc_table[(crc >> 24) ^ (unsigned char)c];
            bytes++;
        }
        printf("%lu %lu\n", crc, bytes);
        fflush(stdout);
        return 0;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "cksum: %s: cannot open\n", argv[i]);
            rc = 1;
            continue;
        }
        unsigned long crc = 0;
        unsigned long bytes = 0;
        int c;
        while ((c = fgetc(f)) != EOF) {
            crc = (crc << 8) ^ crc_table[(crc >> 24) ^ (unsigned char)c];
            bytes++;
        }
        fclose(f);
        printf("%lu %lu %s\n", crc, bytes, argv[i]);
    }
    fflush(stdout);
    return rc;
}

/* -------------------------------------------------------------------------- */
/* du — disk usage (adapted from file_cmds-479/du, BSD-3) */

static int du_main(int argc, char **argv) {
    int human = 0;
    int i = 1;

    while (i < argc && argv[i][0] == '-') {
        if (strchr(argv[i], 'h')) human = 1;
        i++;
    }

    const char *path = (i < argc) ? argv[i] : ".";

    struct stat st;
    if (stat(path, &st) < 0) {
        fprintf(stderr, "du: %s: not found\n", path);
        return 1;
    }

    /* Simplified: just report file size in 512-byte blocks */
    long long blocks = (st.st_size + 511) / 512;
    if (human) {
        if (blocks < 2048)
            printf("%lldK\t%s\n", blocks / 2, path);
        else
            printf("%lldM\t%s\n", blocks / 2048, path);
    } else {
        printf("%lld\t%s\n", blocks, path);
    }
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* mkfifo — create named pipe (adapted from file_cmds-479) */

static int mkfifo_main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: mkfifo file ...\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (mkfifo(argv[i], 0644) < 0) {
            fprintf(stderr, "mkfifo: %s: cannot create\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}

/* -------------------------------------------------------------------------- */
/* chown — change file ownership (adapted from file_cmds-479/chown, BSD-3) */
/* Simplified: x-os is single-user, so this is mostly a no-op */

static int chown_main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: chown owner[:group] file ...\n");
        return 1;
    }

    /* Parse owner:group */
    const char *owner = argv[1];
    uid_t uid = 0;
    gid_t gid = 0;

    /* In x-os, only root exists — accept "root" or numeric */
    if (strcmp(owner, "root") == 0 || strcmp(owner, "0") == 0) {
        uid = 0; gid = 0;
    } else {
        uid = (uid_t)atoi(owner);
        const char *colon = strchr(owner, ':');
        if (colon) gid = (gid_t)atoi(colon + 1);
    }

    int rc = 0;
    for (int i = 2; i < argc; i++) {
        if (chown(argv[i], uid, gid) < 0) {
            fprintf(stderr, "chown: %s: cannot change owner\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}

/* -------------------------------------------------------------------------- */
/* sed — stream editor (adapted from text_cmds-199/sed, BSD-3) */
/* Simplified: supports s/pat/repl/[g], p, d commands */

static int sed_main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: sed script [file ...]\n");
        return 1;
    }

    const char *script = argv[1];
    int file_start = 2;

    /* Parse -e or -n flags */
    int suppress = 0;
    if (strcmp(argv[1], "-n") == 0) {
        suppress = 1;
        if (argc < 3) { fprintf(stderr, "usage: sed [-n] script [file ...]\n"); return 1; }
        script = argv[2];
        file_start = 3;
    } else if (strcmp(argv[1], "-e") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: sed -e script [file ...]\n"); return 1; }
        script = argv[2];
        file_start = 3;
    }

    /* Parse s/pat/repl/flags command */
    char delim = 0;
    char pattern[256] = {0};
    char replacement[256] = {0};
    int global = 0;
    int is_substitute = 0;
    int is_delete = 0;
    int is_print = 0;

    if (script[0] == 's') {
        is_substitute = 1;
        delim = script[1];
        const char *p = script + 2;
        int pi = 0;
        while (*p && *p != delim && pi < (int)sizeof(pattern) - 1)
            pattern[pi++] = *p++;
        pattern[pi] = 0;
        if (*p == delim) p++;
        int ri = 0;
        while (*p && *p != delim && ri < (int)sizeof(replacement) - 1)
            replacement[ri++] = *p++;
        replacement[ri] = 0;
        if (*p == delim) p++;
        while (*p) {
            if (*p == 'g') global = 1;
            if (*p == 'p') is_print = 1;
            p++;
        }
    } else if (script[0] == 'd') {
        is_delete = 1;
    } else if (script[0] == 'p') {
        is_print = 1;
    }

    FILE *f = NULL;
    if (file_start < argc) {
        f = fopen(argv[file_start], "r");
        if (!f) {
            fprintf(stderr, "sed: %s: cannot open\n", argv[file_start]);
            return 1;
        }
    }

    char line[4096];
    while (fgets(line, sizeof(line), f ? f : stdin)) {
        int printed = 0;

        if (is_substitute) {
            /* Simple substring replace */
            char result[8192];
            int ri = 0;
            char *src = line;
            int plen = strlen(pattern);

            while (*src && ri < (int)sizeof(result) - 1) {
                if (plen > 0 && strncmp(src, pattern, plen) == 0) {
                    for (const char *r = replacement; *r && ri < (int)sizeof(result) - 1; r++)
                        result[ri++] = *r;
                    src += plen;
                    if (!global) {
                        while (*src && ri < (int)sizeof(result) - 1)
                            result[ri++] = *src++;
                    }
                } else {
                    result[ri++] = *src++;
                }
            }
            result[ri] = 0;

            if (!is_delete) {
                if (!suppress) { printf("%s", result); printed = 1; }
                if (is_print) printf("%s", result);
            }
        } else if (is_delete) {
            /* Skip line */
        } else {
            if (!suppress) { printf("%s", line); printed = 1; }
            if (is_print) printf("%s", line);
        }
    }

    if (f) fclose(f);
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* paste — merge lines of files (adapted from text_cmds-199/paste, BSD-3) */

static int paste_main(int argc, char **argv) {
    int serial = 0;
    int i = 1;
    char delim = '\t';

    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-s") == 0) serial = 1;
        else if (strncmp(argv[i], "-d", 2) == 0 && argv[i][2])
            delim = argv[i][2];
        i++;
    }

    if (i >= argc) {
        /* Read stdin, just output */
        char buf[4096];
        while (fgets(buf, sizeof(buf), stdin))
            printf("%s", buf);
        fflush(stdout);
        return 0;
    }

    if (serial) {
        /* Paste each file serially */
        for (int j = i; j < argc; j++) {
            FILE *f = fopen(argv[j], "r");
            if (!f) { fprintf(stderr, "paste: %s: cannot open\n", argv[j]); continue; }
            char buf[4096];
            int first = 1;
            while (fgets(buf, sizeof(buf), f)) {
                /* Strip newline, add delimiter */
                int len = strlen(buf);
                if (len > 0 && buf[len-1] == '\n') buf[--len] = 0;
                if (!first) putchar(delim);
                printf("%s", buf);
                first = 0;
            }
            putchar('\n');
            fclose(f);
        }
    } else {
        /* Paste files in parallel */
        FILE **files = malloc(sizeof(FILE *) * (argc - i));
        int nfiles = 0;
        for (int j = i; j < argc; j++) {
            files[nfiles] = fopen(argv[j], "r");
            if (!files[nfiles]) fprintf(stderr, "paste: %s: cannot open\n", argv[j]);
            else nfiles++;
        }

        int done = 0;
        while (!done) {
            done = 1;
            int first = 1;
            for (int j = 0; j < nfiles; j++) {
                char buf[4096];
                if (files[j] && fgets(buf, sizeof(buf), files[j])) {
                    int len = strlen(buf);
                    if (len > 0 && buf[len-1] == '\n') buf[--len] = 0;
                    if (!first) putchar(delim);
                    printf("%s", buf);
                    first = 0;
                    done = 0;
                } else if (!first) {
                    putchar(delim);
                }
            }
            if (!done) putchar('\n');
        }

        for (int j = 0; j < nfiles; j++) if (files[j]) fclose(files[j]);
        free(files);
    }

    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* fold — fold lines (adapted from text_cmds-199/fold, BSD-3) */

static int fold_main(int argc, char **argv) {
    int width = 80;
    int count_bytes = 0;
    int i = 1;

    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (strchr(argv[i], 'b')) {
            count_bytes = 1;
        }
        i++;
    }

    FILE *f = NULL;
    if (i < argc) {
        f = fopen(argv[i], "r");
        if (!f) { fprintf(stderr, "fold: %s: cannot open\n", argv[i]); return 1; }
    }

    int c, col = 0;
    while ((c = fgetc(f ? f : stdin)) != EOF) {
        if (c == '\n') {
            putchar('\n');
            col = 0;
        } else {
            if (col >= width) {
                putchar('\n');
                col = 0;
            }
            putchar(c);
            col++;
        }
    }

    if (f) fclose(f);
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* comm — compare sorted files (adapted from text_cmds-199/comm, BSD-3) */

static int comm_main(int argc, char **argv) {
    int suppress1 = 0, suppress2 = 0, suppress3 = 0;
    int i = 1;

    while (i < argc && argv[i][0] == '-') {
        if (strchr(argv[i], '1')) suppress1 = 1;
        if (strchr(argv[i], '2')) suppress2 = 1;
        if (strchr(argv[i], '3')) suppress3 = 1;
        i++;
    }

    if (i + 1 >= argc) {
        fprintf(stderr, "usage: comm [-123] file1 file2\n");
        return 1;
    }

    FILE *f1 = fopen(argv[i], "r");
    FILE *f2 = fopen(argv[i+1], "r");
    if (!f1) { fprintf(stderr, "comm: %s: cannot open\n", argv[i]); return 1; }
    if (!f2) { fprintf(stderr, "comm: %s: cannot open\n", argv[i+1]); fclose(f1); return 1; }

    char buf1[4096], buf2[4096];
    char *l1 = fgets(buf1, sizeof(buf1), f1);
    char *l2 = fgets(buf2, sizeof(buf2), f2);

    while (l1 || l2) {
        if (!l1) {
            if (!suppress2) printf("\t%s", buf2);
            l2 = fgets(buf2, sizeof(buf2), f2);
        } else if (!l2) {
            if (!suppress1) printf("%s", buf1);
            l1 = fgets(buf1, sizeof(buf1), f1);
        } else {
            int cmp = strcmp(buf1, buf2);
            if (cmp < 0) {
                if (!suppress1) printf("%s", buf1);
                l1 = fgets(buf1, sizeof(buf1), f1);
            } else if (cmp > 0) {
                if (!suppress2) printf("\t%s", buf2);
                l2 = fgets(buf2, sizeof(buf2), f2);
            } else {
                if (!suppress3) printf("\t\t%s", buf1);
                l1 = fgets(buf1, sizeof(buf1), f1);
                l2 = fgets(buf2, sizeof(buf2), f2);
            }
        }
    }

    fclose(f1);
    fclose(f2);
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* nl — number lines (adapted from text_cmds-199/nl, BSD-3) */

static int nl_main(int argc, char **argv) {
    int i = 1;
    int start = 1;
    int sep_width = 6;

    while (i < argc && argv[i][0] == '-') {
        i++;
    }

    FILE *f = NULL;
    if (i < argc) {
        f = fopen(argv[i], "r");
        if (!f) { fprintf(stderr, "nl: %s: cannot open\n", argv[i]); return 1; }
    }

    char buf[4096];
    int lineno = start;
    while (fgets(buf, sizeof(buf), f ? f : stdin)) {
        printf("%*d\t%s", sep_width, lineno++, buf);
    }

    if (f) fclose(f);
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* rev — reverse lines (adapted from text_cmds-199/rev, BSD-3) */

static int rev_main(int argc, char **argv) {
    int i = 1;

    while (i < argc && argv[i][0] == '-') i++;

    FILE *f = NULL;
    if (i < argc) {
        f = fopen(argv[i], "r");
        if (!f) { fprintf(stderr, "rev: %s: cannot open\n", argv[i]); return 1; }
    }

    char buf[4096];
    while (fgets(buf, sizeof(buf), f ? f : stdin)) {
        int len = strlen(buf);
        if (len > 0 && buf[len-1] == '\n') {
            buf[--len] = 0;
            for (int j = len - 1; j >= 0; j--) putchar(buf[j]);
            putchar('\n');
        } else {
            for (int j = len - 1; j >= 0; j--) putchar(buf[j]);
        }
    }

    if (f) fclose(f);
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* expand — tabs to spaces (adapted from text_cmds-199/expand, BSD-3) */

static int expand_main(int argc, char **argv) {
    int tabstop = 8;
    int i = 1;

    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            tabstop = atoi(argv[++i]);
            if (tabstop <= 0) tabstop = 8;
        }
        i++;
    }

    FILE *f = NULL;
    if (i < argc) {
        f = fopen(argv[i], "r");
        if (!f) { fprintf(stderr, "expand: %s: cannot open\n", argv[i]); return 1; }
    }

    int c, col = 0;
    while ((c = fgetc(f ? f : stdin)) != EOF) {
        if (c == '\t') {
            int spaces = tabstop - (col % tabstop);
            for (int s = 0; s < spaces; s++) putchar(' ');
            col += spaces;
        } else if (c == '\n') {
            putchar('\n');
            col = 0;
        } else {
            putchar(c);
            col++;
        }
    }

    if (f) fclose(f);
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* unexpand — spaces to tabs (adapted from text_cmds-199/unexpand, BSD-3) */

static int unexpand_main(int argc, char **argv) {
    int tabstop = 8;
    int all = 0;
    int i = 1;

    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            tabstop = atoi(argv[++i]);
            if (tabstop <= 0) tabstop = 8;
        }
        if (strchr(argv[i], 'a')) all = 1;
        i++;
    }

    FILE *f = NULL;
    if (i < argc) {
        f = fopen(argv[i], "r");
        if (!f) { fprintf(stderr, "unexpand: %s: cannot open\n", argv[i]); return 1; }
    }

    int c, col = 0, spaces = 0;
    while ((c = fgetc(f ? f : stdin)) != EOF) {
        if (c == ' ') {
            spaces++;
            col++;
            if (all && (col % tabstop) == 0 && spaces > 1) {
                putchar('\t');
                spaces = 0;
            }
        } else if (c == '\t') {
            if (spaces > 0) { for (int s = 0; s < spaces; s++) putchar(' '); spaces = 0; }
            putchar('\t');
            col = (col / tabstop + 1) * tabstop;
        } else if (c == '\n') {
            if (spaces > 0) { for (int s = 0; s < spaces; s++) putchar(' '); spaces = 0; }
            putchar('\n');
            col = 0;
        } else {
            if (spaces > 0) { for (int s = 0; s < spaces; s++) putchar(' '); spaces = 0; }
            putchar(c);
            col++;
        }
    }

    if (f) fclose(f);
    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* colrm — remove columns (adapted from text_cmds-199/colrm, BSD-3) */

static int colrm_main(int argc, char **argv) {
    int start = 0, end = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: colrm [start [end]]\n");
        return 1;
    }
    start = atoi(argv[1]);
    if (argc >= 3) end = atoi(argv[2]);
    else end = start;

    if (start <= 0) start = 1;

    char buf[4096];
    while (fgets(buf, sizeof(buf), stdin)) {
        int len = strlen(buf);
        if (len > 0 && buf[len-1] == '\n') buf[--len] = 0;

        for (int i = 0; i < len; i++) {
            if (i < start - 1 || i >= end)
                putchar(buf[i]);
        }
        putchar('\n');
    }

    fflush(stdout);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* split — split file into pieces (adapted from text_cmds-199/split, BSD-3) */

static int split_main(int argc, char **argv) {
    int lines = 1000;
    const char *prefix = "x";
    const char *infile = NULL;
    int i = 1;

    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            lines = atoi(argv[++i]);
        } else if (argv[i][1] >= '0' && argv[i][1] <= '9') {
            lines = atoi(argv[i] + 1);
        }
        i++;
    }

    if (i < argc) { infile = argv[i++]; }
    if (i < argc) { prefix = argv[i++]; }

    FILE *f = NULL;
    if (infile) {
        f = fopen(infile, "r");
        if (!f) { fprintf(stderr, "split: %s: cannot open\n", infile); return 1; }
    }

    char buf[4096];
    int lineno = 0;
    int file_num = 0;
    FILE *outf = NULL;

    while (fgets(buf, sizeof(buf), f ? f : stdin)) {
        if (lineno % lines == 0) {
            if (outf) fclose(outf);
            char fname[256];
            snprintf(fname, sizeof(fname), "%s%c%c", prefix,
                     'a' + (file_num / 26), 'a' + (file_num % 26));
            outf = fopen(fname, "w");
            if (!outf) { fprintf(stderr, "split: cannot create %s\n", fname); return 1; }
            file_num++;
        }
        fprintf(outf, "%s", buf);
        lineno++;
    }

    if (outf) fclose(outf);
    if (f) fclose(f);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* less — simple pager (inspired by less-50, BSD/GPL) */
/* Supports: q/Q quit, Space/f next page, b prev page, j/Enter next line, */
/*           k prev line, g top, G bottom, /search, n next match, h help */

static int less_main(int argc, char **argv) {
    int rows = 25, cols = 80;

    /* Try to get window size */
    struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; } ws;
    if (ioctl(1, 0x40087468, &ws) == 0 && ws.ws_row > 0) {  /* TIOCGWINSZ */
        rows = ws.ws_row;
        cols = ws.ws_col;
    }

    /* Read all input into memory */
    char *buf = NULL;
    size_t bufsize = 0, bufcap = 0;
    FILE *f = NULL;

    if (argc >= 2 && strcmp(argv[1], "-") != 0) {
        f = fopen(argv[1], "r");
        if (!f) { fprintf(stderr, "less: %s: cannot open\n", argv[1]); return 1; }
    }

    int c;
    while ((c = fgetc(f ? f : stdin)) != EOF) {
        if (bufsize >= bufcap) {
            bufcap = bufcap ? bufcap * 2 : 65536;
            buf = realloc(buf, bufcap);
            if (!buf) { if (f) fclose(f); return 1; }
        }
        buf[bufsize++] = (char)c;
    }
    if (f) fclose(f);

    /* Split into lines */
    int nlines = 0;
    int line_cap = 1024;
    char **lines = malloc(sizeof(char *) * line_cap);
    int *line_lens = malloc(sizeof(int) * line_cap);

    int pos = 0;
    while (pos < (int)bufsize) {
        if (nlines >= line_cap) {
            line_cap *= 2;
            lines = realloc(lines, sizeof(char *) * line_cap);
            line_lens = realloc(line_lens, sizeof(int) * line_cap);
        }
        lines[nlines] = buf + pos;
        int start = pos;
        while (pos < (int)bufsize && buf[pos] != '\n') pos++;
        line_lens[nlines] = pos - start;
        if (pos < (int)bufsize) pos++;  /* skip \n */
        nlines++;
    }

    if (nlines == 0) {
        lines[0] = "";
        line_lens[0] = 0;
        nlines = 1;
    }

    /* Display loop */
    int top = 0;  /* top visible line */
    int search_pos = -1;
    char search_str[256] = {0};

    /* Enter raw mode */
    struct termios orig, raw;
    tcgetattr(0, &orig);
    raw = orig;
    raw.c_lflag &= ~(0x00000002 | 0x00000008 | 0x00000080);  /* ~ICANON ~ECHO ~ISIG */
    raw.c_cc[6] = 1;  /* VMIN */
    raw.c_cc[5] = 0;  /* VTIME */
    tcsetattr(0, 0, &raw);  /* TCSANOW */

    while (1) {
        /* Clear screen */
        printf("\033[H\033[2J");

        /* Display lines */
        int displayed = 0;
        for (int i = top; i < nlines && displayed < rows - 1; i++) {
            int len = line_lens[i];
            if (len > cols) len = cols;
            fwrite(lines[i], 1, len, stdout);
            putchar('\n');
            displayed++;
        }

        /* Status bar */
        printf("\033[7m", stdout);
        if (top + rows - 1 >= nlines)
            printf("(END)");
        else
            printf("%d%%", (top * 100) / (nlines > 1 ? nlines - 1 : 1));
        printf("\033[27m");
        fflush(stdout);

        /* Read key */
        c = getchar();

        if (c == 'q' || c == 'Q' || c == 3)  /* q/Q/Ctrl-C */
            break;
        else if (c == ' ' || c == 'f' || c == 6)  /* Space/f/Ctrl-F */
            top += rows - 1;
        else if (c == 'b' || c == 2)  /* b/Ctrl-B */
            top -= rows - 1;
        else if (c == '\n' || c == 'j' || c == 14)  /* Enter/j/Ctrl-N */
            top++;
        else if (c == 'k' || c == 16)  /* k/Ctrl-P */
            top--;
        else if (c == 'g')  /* top */
            top = 0;
        else if (c == 'G')  /* bottom */
            top = nlines - rows + 1;
        else if (c == '/') {  /* search */
            printf("\033[1;1H\033[0m/");
            fflush(stdout);
            int si = 0;
            while ((c = getchar()) != '\n' && c != '\r' && si < 255) {
                if (c == 27) { si = -1; break; }  /* Esc */
                if (c == 127 || c == 8) { if (si > 0) si--; continue; }
                search_str[si++] = c;
            }
            search_str[si] = 0;
            if (si >= 0) {
                /* Search forward from top */
                search_pos = -1;
                for (int i = top + 1; i < nlines; i++) {
                    if (memmem(lines[i], line_lens[i], search_str, si)) {
                        search_pos = i;
                        break;
                    }
                }
                if (search_pos >= 0)
                    top = search_pos;
            }
        }
        else if (c == 'n' && search_str[0]) {  /* next match */
            for (int i = top + 1; i < nlines; i++) {
                if (memmem(lines[i], line_lens[i], search_str, strlen(search_str))) {
                    top = i;
                    break;
                }
            }
        }
        else if (c == 27) {  /* Esc sequence (arrow keys) */
            c = getchar();
            if (c == '[') {
                c = getchar();
                if (c == 'A') top--;       /* up */
                else if (c == 'B') top++;  /* down */
                else if (c == 'C') {}      /* right */
                else if (c == 'D') {}      /* left */
                else if (c == '6') { getchar(); top += rows - 1; }  /* PgDn */
                else if (c == '5') { getchar(); top -= rows - 1; }  /* PgUp */
                else if (c == 'H') top = 0;   /* Home */
                else if (c == 'F') top = nlines - rows + 1;  /* End */
            }
        }

        /* Clamp */
        if (top < 0) top = 0;
        if (top > nlines - 1) top = nlines - 1;
    }

    /* Restore terminal */
    tcsetattr(0, 0, &orig);

    /* Clear screen */
    printf("\033[H\033[2J");
    fflush(stdout);

    free(lines);
    free(line_lens);
    free(buf);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* vi — minimal vi editor (inspired by vim-167/nvi, BSD/GPL) */
/* Supports: i insert, ESC, :w write, :q quit, :wq, :x, dd, o, O, a, x, */
/*           h/j/k/l movement, :wq save and quit */

static int vi_main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: vi file\n");
        return 1;
    }

    const char *filename = argv[1];
    int rows = 25, cols = 80;
    struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; } ws;
    if (ioctl(1, 0x40087468, &ws) == 0 && ws.ws_row > 0) {
        rows = ws.ws_row; cols = ws.ws_col;
    }

    /* Load file into lines array */
    int nlines = 1;
    int line_cap = 1024;
    char **lines = malloc(sizeof(char *) * line_cap);
    int *line_lens = malloc(sizeof(int) * line_cap);

    lines[0] = strdup("");
    line_lens[0] = 0;

    FILE *f = fopen(filename, "r");
    if (f) {
        nlines = 0;
        char buf[4096];
        while (fgets(buf, sizeof(buf), f)) {
            if (nlines >= line_cap) {
                line_cap *= 2;
                lines = realloc(lines, sizeof(char *) * line_cap);
                line_lens = realloc(line_lens, sizeof(int) * line_cap);
            }
            int len = strlen(buf);
            if (len > 0 && buf[len-1] == '\n') buf[--len] = 0;
            lines[nlines] = strdup(buf);
            line_lens[nlines] = len;
            nlines++;
        }
        fclose(f);
    }
    if (nlines == 0) { lines[0] = strdup(""); line_lens[0] = 0; nlines = 1; }

    int cur_row = 0, cur_col = 0;
    int top_row = 0;
    int modified = 0;
    enum { MODE_NORMAL, MODE_INSERT, MODE_COMMAND } mode = MODE_NORMAL;
    char cmdbuf[256];
    int cmdlen = 0;

    /* Enter raw mode */
    struct termios orig, raw;
    tcgetattr(0, &orig);
    raw = orig;
    raw.c_lflag &= ~(0x00000002 | 0x00000008 | 0x00000080);
    raw.c_cc[6] = 1;
    raw.c_cc[5] = 0;
    tcsetattr(0, 0, &raw);

    while (1) {
        /* Render */
        printf("\033[H\033[2J");

        int display_rows = rows - 1;  /* leave room for status */
        for (int i = 0; i < display_rows; i++) {
            int line_idx = top_row + i;
            if (line_idx >= nlines) break;
            printf("\033[%d;1H", i + 1);
            int len = line_lens[line_idx];
            if (len > cols) len = cols;
            if (len > 0) fwrite(lines[line_idx], 1, len, stdout);
            if (line_idx == cur_row && mode == MODE_NORMAL) {
                /* Highlight cursor position with reverse video on char */
                /* Already printed the line, cursor is at end */
            }
            putchar('\n');
        }

        /* Position cursor */
        if (mode == MODE_COMMAND) {
            printf("\033[%d;1H:", rows);
            for (int i = 0; i < cmdlen; i++) putchar(cmdbuf[i]);
        } else {
            int disp_row = cur_row - top_row;
            int disp_col = cur_col;
            if (disp_col >= cols) disp_col = cols - 1;
            printf("\033[%d;%dH", disp_row + 1, disp_col + 1);
        }

        /* Status line */
        printf("\033[%d;1H", rows);
        if (mode == MODE_INSERT)
            printf("\033[7m-- INSERT --\033[27m");
        else if (modified)
            printf("[modified] ");
        printf("%s  %d lines", filename, nlines);

        fflush(stdout);

        /* Read key */
        int c = getchar();

        if (c == -1) continue;

        if (mode == MODE_NORMAL) {
            switch (c) {
            case 'h': if (cur_col > 0) cur_col--; break;
            case 'l': if (cur_col < line_lens[cur_row]) cur_col++; break;
            case 'j': if (cur_row < nlines - 1) { cur_row++; if (cur_col > line_lens[cur_row]) cur_col = line_lens[cur_row]; } break;
            case 'k': if (cur_row > 0) { cur_row--; if (cur_col > line_lens[cur_row]) cur_col = line_lens[cur_row]; } break;
            case '0': cur_col = 0; break;
            case '$': cur_col = line_lens[cur_row]; break;
            case 'G': cur_row = nlines - 1; cur_col = 0; break;
            case 'g': if (getchar() == 'g') { cur_row = 0; cur_col = 0; } break;
            case 'i': mode = MODE_INSERT; break;
            case 'a': if (cur_col < line_lens[cur_row]) cur_col++; mode = MODE_INSERT; break;
            case 'A': cur_col = line_lens[cur_row]; mode = MODE_INSERT; break;
            case 'o': {
                /* Insert new line after current */
                if (nlines >= line_cap) {
                    line_cap *= 2;
                    lines = realloc(lines, sizeof(char *) * line_cap);
                    line_lens = realloc(line_lens, sizeof(int) * line_cap);
                }
                for (int i = nlines; i > cur_row + 1; i--) {
                    lines[i] = lines[i-1];
                    line_lens[i] = line_lens[i-1];
                }
                lines[cur_row + 1] = strdup("");
                line_lens[cur_row + 1] = 0;
                nlines++;
                cur_row++;
                cur_col = 0;
                mode = MODE_INSERT;
                modified = 1;
                break;
            }
            case 'O': {
                if (nlines >= line_cap) {
                    line_cap *= 2;
                    lines = realloc(lines, sizeof(char *) * line_cap);
                    line_lens = realloc(line_lens, sizeof(int) * line_cap);
                }
                for (int i = nlines; i > cur_row; i--) {
                    lines[i] = lines[i-1];
                    line_lens[i] = line_lens[i-1];
                }
                lines[cur_row] = strdup("");
                line_lens[cur_row] = 0;
                nlines++;
                cur_col = 0;
                mode = MODE_INSERT;
                modified = 1;
                break;
            }
            case 'x': {
                if (cur_col < line_lens[cur_row]) {
                    char *l = lines[cur_row];
                    int len = line_lens[cur_row];
                    memmove(l + cur_col, l + cur_col + 1, len - cur_col - 1);
                    line_lens[cur_row]--;
                    lines[cur_row] = realloc(l, line_lens[cur_row] + 1);
                    if (lines[cur_row]) lines[cur_row][line_lens[cur_row]] = 0;
                    if (cur_col > line_lens[cur_row]) cur_col = line_lens[cur_row];
                    modified = 1;
                }
                break;
            }
            case 'd': {
                int c2 = getchar();
                if (c2 == 'd' && nlines > 1) {
                    free(lines[cur_row]);
                    for (int i = cur_row; i < nlines - 1; i++) {
                        lines[i] = lines[i+1];
                        line_lens[i] = line_lens[i+1];
                    }
                    nlines--;
                    if (cur_row >= nlines) cur_row = nlines - 1;
                    cur_col = 0;
                    modified = 1;
                }
                break;
            }
            case ':': mode = MODE_COMMAND; cmdlen = 0; break;
            case 27: break;  /* Esc in normal mode = no-op */
            default: break;
            }

            /* Scroll if cursor out of view */
            if (cur_row < top_row) top_row = cur_row;
            if (cur_row >= top_row + rows - 1) top_row = cur_row - rows + 2;

        } else if (mode == MODE_INSERT) {
            if (c == 27) {
                mode = MODE_NORMAL;
                if (cur_col > 0) cur_col--;
            } else if (c == 127 || c == 8) {  /* backspace */
                if (cur_col > 0) {
                    char *l = lines[cur_row];
                    int len = line_lens[cur_row];
                    memmove(l + cur_col - 1, l + cur_col, len - cur_col);
                    line_lens[cur_row]--;
                    cur_col--;
                    lines[cur_row] = realloc(l, line_lens[cur_row] + 1);
                    if (lines[cur_row]) lines[cur_row][line_lens[cur_row]] = 0;
                    modified = 1;
                } else if (cur_row > 0) {
                    /* Join with previous line */
                    int prevlen = line_lens[cur_row - 1];
                    int curlen = line_lens[cur_row];
                    lines[cur_row - 1] = realloc(lines[cur_row - 1], prevlen + curlen + 1);
                    memcpy(lines[cur_row - 1] + prevlen, lines[cur_row], curlen);
                    line_lens[cur_row - 1] = prevlen + curlen;
                    lines[cur_row - 1][line_lens[cur_row - 1]] = 0;
                    free(lines[cur_row]);
                    for (int i = cur_row; i < nlines - 1; i++) {
                        lines[i] = lines[i+1];
                        line_lens[i] = line_lens[i+1];
                    }
                    nlines--;
                    cur_row--;
                    cur_col = prevlen;
                    modified = 1;
                }
            } else if (c == '\n' || c == '\r') {
                /* Split line */
                if (nlines >= line_cap) {
                    line_cap *= 2;
                    lines = realloc(lines, sizeof(char *) * line_cap);
                    line_lens = realloc(line_lens, sizeof(int) * line_cap);
                }
                for (int i = nlines; i > cur_row + 1; i--) {
                    lines[i] = lines[i-1];
                    line_lens[i] = line_lens[i-1];
                }
                int remaining = line_lens[cur_row] - cur_col;
                lines[cur_row + 1] = malloc(remaining + 1);
                memcpy(lines[cur_row + 1], lines[cur_row] + cur_col, remaining);
                lines[cur_row + 1][remaining] = 0;
                line_lens[cur_row + 1] = remaining;

                lines[cur_row] = realloc(lines[cur_row], cur_col + 1);
                lines[cur_row][cur_col] = 0;
                line_lens[cur_row] = cur_col;

                nlines++;
                cur_row++;
                cur_col = 0;
                modified = 1;
            } else if (c >= 32 && c < 127) {
                /* Insert character */
                char *l = lines[cur_row];
                int len = line_lens[cur_row];
                l = realloc(l, len + 2);
                memmove(l + cur_col + 1, l + cur_col, len - cur_col);
                l[cur_col] = (char)c;
                line_lens[cur_row] = len + 1;
                l[line_lens[cur_row]] = 0;
                lines[cur_row] = l;
                cur_col++;
                modified = 1;
            }

            /* Scroll */
            if (cur_row < top_row) top_row = cur_row;
            if (cur_row >= top_row + rows - 1) top_row = cur_row - rows + 2;

        } else if (mode == MODE_COMMAND) {
            if (c == 27) {
                mode = MODE_NORMAL;
                cmdlen = 0;
            } else if (c == '\n' || c == '\r') {
                cmdbuf[cmdlen] = 0;
                /* Process command */
                if (strcmp(cmdbuf, "q") == 0 || strcmp(cmdbuf, "q!") == 0) {
                    break;  /* quit */
                } else if (strcmp(cmdbuf, "w") == 0) {
                    /* Write file */
                    FILE *wf = fopen(filename, "w");
                    if (wf) {
                        for (int i = 0; i < nlines; i++) {
                            fwrite(lines[i], 1, line_lens[i], wf);
                            fputc('\n', wf);
                        }
                        fclose(wf);
                        modified = 0;
                    }
                    mode = MODE_NORMAL;
                } else if (strcmp(cmdbuf, "wq") == 0 || strcmp(cmdbuf, "x") == 0) {
                    FILE *wf = fopen(filename, "w");
                    if (wf) {
                        for (int i = 0; i < nlines; i++) {
                            fwrite(lines[i], 1, line_lens[i], wf);
                            fputc('\n', wf);
                        }
                        fclose(wf);
                    }
                    break;
                } else if (strncmp(cmdbuf, "w ", 2) == 0) {
                    /* Write to different file */
                    FILE *wf = fopen(cmdbuf + 2, "w");
                    if (wf) {
                        for (int i = 0; i < nlines; i++) {
                            fwrite(lines[i], 1, line_lens[i], wf);
                            fputc('\n', wf);
                        }
                        fclose(wf);
                    }
                    mode = MODE_NORMAL;
                } else if (cmdbuf[0] >= '0' && cmdbuf[0] <= '9') {
                    /* Go to line */
                    int line = atoi(cmdbuf) - 1;
                    if (line < 0) line = 0;
                    if (line >= nlines) line = nlines - 1;
                    cur_row = line;
                    cur_col = 0;
                    mode = MODE_NORMAL;
                } else {
                    mode = MODE_NORMAL;
                }
                cmdlen = 0;
            } else if (c == 127 || c == 8) {
                if (cmdlen > 0) cmdlen--;
            } else if (cmdlen < 255) {
                cmdbuf[cmdlen++] = c;
            }
        }
    }

    /* Restore terminal */
    tcsetattr(0, 0, &orig);
    printf("\033[H\033[2J");
    fflush(stdout);

    /* Cleanup */
    for (int i = 0; i < nlines; i++) free(lines[i]);
    free(lines);
    free(line_lens);

    return 0;
}

/* -------------------------------------------------------------------------- */
/* sudo / su — stubs until Apple sudo-114 / PAM is ported.
 * Already uid 0: never nested-fork (that crashed the kernel on sudo su).
 * Replace this process image with the target command via exec. */

static int su_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    /* `su -` would start a login shell; we already are root in the shell. */
    printf("su: already root\n");
    return 0;
}

static int sudo_main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: sudo command [args...]\n");
        return 1;
    }

    /* Already root: run the target applet in-process. A second exec was
     * hitting a kernel argv-stack alignment bug and corrupting argv[0]. */
    return cmds_main(argc - 1, &argv[1]);
}
