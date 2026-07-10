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
    fprintf(stderr, "Available: echo pwd true false basename dirname yes sleep uname cat ls env printenv hostname logname id date seq tee test printf kill wc head tail sort tr uniq cut which touch mkdir rm cp mv grep xargs expr\n");
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
