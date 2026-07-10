/* Minimal but functional terminfo/termcap library for zsh on x-os.
 * Provides real VT100/xterm escape sequences so zsh line editing,
 * history, and tab completion work correctly.
 *
 * Adapted from ncurses-79 tinfo design (Free Software license).
 * Terminal capabilities hardcoded for xterm-256color (ANSI/VT100 compatible).
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Terminal capability table                                          */
/* ------------------------------------------------------------------ */

/* Escape sequences for xterm (VT100/ANSI compatible) */
#define ESC "\033"
#define CSI ESC "["

/* Boolean (flag) capabilities */
struct bool_cap {
    const char *name;
    int value;
};

/* Numeric capabilities */
struct num_cap {
    const char *name;
    int value;
};

/* String capabilities — stored as terminfo format (with %i, %p1, etc.) */
struct str_cap {
    const char *name;
    const char *value;
};

static const struct bool_cap bool_caps[] = {
    { "am",  1 },  /* auto margins */
    { "xn",  1 },  /* newline ignored (xterm) */
    { "bs",  1 },  /* backspace works */
    { "km",  1 },  /* has meta key */
    { "mi",  1 },  /* safe to move in insert mode */
    { "ms",  1 },  /* safe to move in standout mode */
    { "bw",  0 },  /* auto left margin */
    { "bce", 0 },  /* background color erase */
    { "ccc", 0 },  /* can change color */
    { "chts", 0 }, /* cursor hidden */
    { "da",  0 },  /* lines can be deleted */
    { "db",  0 },  /* lines can be inserted */
    { "esam", 0 }, /* escape time padding */
    { "gn",  0 },  /* generic line */
    { "hc",  0 },  /* hardcopy */
    { "hl",  0 },  /* half-line */
    { "hs",  0 },  /* has status line */
    { "hz",  0 },  /* hazeltine */
    { "in",  0 },  /* insert null */
    { "lp",  0 },  /* line padding */
    { "ma",  0 },  /* meta key */
    { "mir", 1 },  /* move in insert mode */
    { "msgr", 1 }, /* move in standout mode */
    { "nxon", 0 }, /* no xon */
    { "nxor", 0 }, /* no xor */
    { "os",  0 },  /* overstrike */
    { "pt",  1 },  /* physical tabs */
    { "ul",  0 },  /* underline */
    { "xhp", 0 },  /* hp glitch */
    { "xenl", 1 }, /* newline ignored */
    { NULL, 0 }
};

static const struct num_cap num_caps[] = {
    { "co", 80 },   /* columns */
    { "li", 25 },   /* lines */
    { "it", 8 },    /* initial tabs */
    { "sg", 0 },    /* magic cookie glitch */
    { "ug", 0 },    /* unglitch */
    { "pb", 0 },    /* padding baud */
    { "vt", 0 },    /* virtual terminal */
    { "ws", 0 },    /* width status */
    { NULL, -1 }
};

static const struct str_cap str_caps[] = {
    /* Cursor movement */
    { "cm", CSI "%i%p1%d;%p2%dH" },  /* cursor move (row, col) — terminfo */
    { "cr", "\r" },                   /* carriage return */
    { "nl", "\n" },                   /* newline */
    { "ho", CSI "H" },               /* home cursor */
    { "ll", CSI "24;1H" },           /* last line, first column */

    /* Cursor movement by direction */
    { "up", CSI "A" },               /* cursor up */
    { "dn", CSI "B" },               /* cursor down */
    { "ri", CSI "C" },               /* cursor right */
    { "le", CSI "D" },               /* cursor left */
    { "UP", CSI "%p1%dA" },          /* cursor up N */
    { "DO", CSI "%p1%dB" },          /* cursor down N */
    { "RI", CSI "%p1%dC" },          /* cursor right N */
    { "LE", CSI "%p1%dD" },          /* cursor left N */
    { "ch", CSI "%i%p1%dG" },        /* column position */
    { "cv", CSI "%i%p1%dd" },        /* row position */

    /* Clearing */
    { "ce", CSI "K" },               /* clear to end of line */
    { "cd", CSI "J" },               /* clear to end of screen */
    { "cl", CSI "H" CSI "2J" },      /* clear screen + home */
    { "cb", CSI "1K" },              /* clear to beginning of line */
    { "cc", ESC "[3J" },             /* clear scrollback */

    /* Insert/delete */
    { "dc", CSI "P" },               /* delete character */
    { "dl", CSI "M" },               /* delete line */
    { "ic", CSI "@" },               /* insert character */
    { "il", CSI "L" },               /* insert line */
    { "DC", CSI "%p1%dP" },          /* delete N characters */
    { "DL", CSI "%p1%dM" },          /* delete N lines */
    { "IC", CSI "%p1%d@" },          /* insert N characters */
    { "IL", CSI "%p1%dL" },          /* insert N lines */

    /* Insert mode */
    { "im", "" },                    /* enter insert mode (no-op for xterm) */
    { "ei", "" },                    /* exit insert mode */
    { "ip", "" },                    /* insert padding */

    /* Scrolling */
    { "sf", "\n" },                  /* scroll forward */
    { "sr", ESC "M" },              /* scroll reverse */
    { "cs", CSI "%i%p1%d;%p2%dr" },  /* set scroll region */

    /* Attributes */
    { "md", CSI "1m" },             /* bold */
    { "mh", CSI "2m" },             /* dim */
    { "us", CSI "4m" },             /* underline */
    { "ue", CSI "24m" },            /* end underline */
    { "so", CSI "7m" },             /* standout (reverse) */
    { "se", CSI "27m" },            /* end standout */
    { "mr", CSI "7m" },             /* reverse */
    { "mb", CSI "5m" },             /* blink */
    { "me", CSI "0m" },             /* end all attributes */
    { "mk", CSI "8m" },             /* invisible */
    { "mp", "" },                   /* like protect */
    { "rmp", "" },                  /* like unprotect */

    /* Colors */
    { "AF", CSI "3%p1%dm" },        /* set foreground color */
    { "AB", CSI "4%p1%dm" },        /* set background color */
    { "op", CSI "39;49m" },         /* reset colors to default */

    /* Keypad */
    { "ks", ESC "=" },              /* keypad send mode */
    { "ke", ESC ">" },              /* keypad local mode */

    /* Keys */
    { "ku", CSI "A" },              /* key up */
    { "kd", CSI "B" },              /* key down */
    { "kr", CSI "C" },              /* key right */
    { "kl", CSI "D" },              /* key left */
    { "k1", CSI "OP" },             /* F1 */
    { "k2", CSI "OQ" },             /* F2 */
    { "k3", CSI "OR" },             /* F3 */
    { "k4", CSI "OS" },             /* F4 */
    { "k5", CSI "15~" },            /* F5 */
    { "k6", CSI "17~" },            /* F6 */
    { "k7", CSI "18~" },            /* F7 */
    { "k8", CSI "19~" },            /* F8 */
    { "k9", CSI "20~" },            /* F9 */
    { "k;", CSI "21~" },            /* F10 */
    { "kD", CSI "3~" },             /* key delete */
    { "kI", CSI "2~" },             /* key insert */
    { "kH", CSI "4~" },             /* key end */
    { "kh", CSI "1~" },             /* key home */
    { "kN", CSI "6~" },             /* key next (page down) */
    { "kP", CSI "5~" },             /* key prev (page up) */
    { "kb", "\b" },                 /* key backspace */
    { "ka", "\t" },                 /* key tab */
    { "kt", "\t" },                 /* key tab */
    { "kE", "" },                   /* key clear to end of line */
    { "kC", "" },                   /* key clear screen */

    /* Misc */
    { "bl", "\a" },                 /* bell */
    { "vb", ESC "g" },             /* visible bell */
    { "ti", "" },                   /* terminal init */
    { "te", "" },                   /* terminal exit */
    { "vs", "" },                   /* visible cursor */
    { "vi", CSI "?25l" },          /* invisible cursor */
    { "ve", CSI "?25h" },          /* normal cursor visible */
    { "sc", ESC "7" },             /* save cursor */
    { "rc", ESC "8" },             /* restore cursor */
    { "st", "\t" },                /* set tab */
    { "ct", CSI "3g" },            /* clear all tabs */
    { "bc", "\b" },                /* backspace */
    { "ta", "\t" },                /* tab */

    /* terminfo long names (used by tigetstr) */
    { "cuu", CSI "A" },            /* cursor_up */
    { "cud", CSI "B" },            /* cursor_down */
    { "cuf", CSI "C" },            /* cursor_right */
    { "cub", CSI "D" },            /* cursor_left */
    { "cuu1", CSI "A" },           /* cursor_up_1 */
    { "cud1", CSI "B" },           /* cursor_down_1 */
    { "cuf1", CSI "C" },           /* cursor_right_1 */
    { "cub1", "\b" },              /* cursor_left_1 */
    { "clear", CSI "H" CSI "2J" }, /* clear_screen */
    { "el", CSI "K" },             /* clr_eol */
    { "ed", CSI "J" },             /* clr_eos */
    { "el1", CSI "1K" },           /* clr_bol */
    { "hpa", CSI "%i%p1%dG" },     /* column_address */
    { "vpa", CSI "%i%p1%dd" },     /* row_address */
    { "cup", CSI "%i%p1%d;%p2%dH" }, /* cursor_address */
    { "cud1", CSI "B" },           /* cursor_down_1 */
    { "bold", CSI "1m" },          /* enter_bold_mode */
    { "sgr0", CSI "0m" },          /* exit_attribute_mode */
    { "smul", CSI "4m" },          /* enter_underline_mode */
    { "rmul", CSI "24m" },         /* exit_underline_mode */
    { "smso", CSI "7m" },          /* enter_standout_mode */
    { "rmso", CSI "27m" },         /* exit_standout_mode */
    { "rev", CSI "7m" },           /* enter_reverse_mode */
    { "blink", CSI "5m" },         /* enter_blink_mode */
    { "dim", CSI "2m" },           /* enter_dim_mode */
    { "invis", CSI "8m" },         /* enter_secure_mode */
    { "ich", CSI "%p1%d@" },       /* insert_character */
    { "dch", CSI "%p1%dP" },       /* delete_character */
    { "ich1", CSI "@" },           /* insert_character_1 */
    { "dch1", CSI "P" },           /* delete_character_1 */
    { "il1", CSI "L" },            /* insert_line_1 */
    { "dl1", CSI "M" },            /* delete_line_1 */
    { "ip", "" },                  /* insert_padding */
    { "smdc", "" },                /* enter_delete_mode */
    { "rmdc", "" },                /* exit_delete_mode */
    { "smir", "" },                /* enter_insert_mode */
    { "rmir", "" },                /* exit_insert_mode */
    { "ind", "\n" },               /* scroll_forward */
    { "ri", ESC "M" },             /* scroll_reverse */
    { "indn", CSI "%p1%dS" },      /* parm_index */
    { "rin", CSI "%p1%dT" },       /* parm_rindex */
    { "cuf", CSI "%p1%dC" },       /* parm_right_cursor */
    { "cub", CSI "%p1%dD" },       /* parm_left_cursor */
    { "cuu", CSI "%p1%dA" },       /* parm_up_cursor */
    { "cud", CSI "%p1%dB" },       /* parm_down_cursor */
    { "setaf", CSI "3%p1%dm" },    /* set_a_foreground */
    { "setab", CSI "4%p1%dm" },    /* set_a_background */
    { "op", CSI "39;49m" },        /* orig_pair */
    { "kcuu1", CSI "A" },          /* key_up */
    { "kcud1", CSI "B" },          /* key_down */
    { "kcuf1", CSI "C" },          /* key_right */
    { "kcub1", CSI "D" },          /* key_left */
    { "kdch1", CSI "3~" },         /* key_dc */
    { "kich1", CSI "2~" },         /* key_ic */
    { "kend1", CSI "4~" },         /* key_end */
    { "khome1", CSI "1~" },        /* key_home */
    { "knp1", CSI "6~" },          /* key_npage */
    { "kpp1", CSI "5~" },          /* key_ppage */
    { "kbs1", "\b" },              /* key_backspace */
    { "ht", "\t" },                /* tab */
    { "cr", "\r" },                /* carriage_return */
    { "nel", "\r\n" },             /* newline */
    { "bel", "\a" },               /* bell */
    { "sgr", CSI "0%p1%?%p2%t;2%;%?%p3%t;7%;%?%p4%t;4%;%?%p5%t;5%;%?%p6%t;1%;%?%p7%t;8%;m" },

    { NULL, NULL }
};

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */

typedef struct TERMINAL TERMINAL;
TERMINAL *cur_term = (void *)0;
char PC = 0;       /* termcap padding character */
short ospeed = 0;  /* termcap output speed */
char *UP = (void *)0;
char *BC = (void *)0;

static int g_initialized = 0;
static char g_tcap_buf[1024];  /* termcap string storage */
static int g_tcap_pos = 0;

/* ------------------------------------------------------------------ */
/* Helper: find capability by 2-char name                             */
/* ------------------------------------------------------------------ */

const char *find_str_cap(const char *id) {
    if (!id || !id[0] || !id[1]) return NULL;
    for (const struct str_cap *c = str_caps; c->name; c++) {
        if (c->name[0] == id[0] && c->name[1] == id[1] && c->name[2] == 0)
            return c->value;
    }
    return NULL;
}

int find_bool_cap(const char *id) {
    if (!id || !id[0] || !id[1]) return 0;
    for (const struct bool_cap *c = bool_caps; c->name; c++) {
        if (c->name[0] == id[0] && c->name[1] == id[1] && c->name[2] == 0)
            return c->value;
    }
    return 0;
}

int find_num_cap(const char *id) {
    if (!id || !id[0] || !id[1]) return -1;
    for (const struct num_cap *c = num_caps; c->name; c++) {
        if (c->name[0] == id[0] && c->name[1] == id[1] && c->name[2] == 0)
            return c->value;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* tparm — format parameterized terminfo string                       */
/* ------------------------------------------------------------------ */

static char g_tparm_buf[256];

char *tparm(const char *str, long p1, long p2, long p3,
                   long p4, long p5, long p6, long p7, long p8, long p9) {
    if (!str) return NULL;

    long params[9] = { p1, p2, p3, p4, p5, p6, p7, p8, p9 };
    int param_idx = 0;  /* current parameter being consumed by %d etc */
    int out = 0;
    int i_incr = 0;  /* %i flag: increment first two params */

    const char *p = str;
    while (*p && out < (int)sizeof(g_tparm_buf) - 1) {
        if (*p != '%') {
            g_tparm_buf[out++] = *p++;
            continue;
        }
        p++;  /* skip % */

        switch (*p) {
        case '%':
            g_tparm_buf[out++] = '%';
            p++;
            break;

        case 'i':
            i_incr = 1;
            if (params[0] >= 0) params[0]++;
            if (params[1] >= 0) params[1]++;
            p++;
            break;

        case 'd':
            out += snprintf(g_tparm_buf + out, sizeof(g_tparm_buf) - out,
                           "%ld", params[param_idx]);
            param_idx++;
            p++;
            break;

        case '2':
            out += snprintf(g_tparm_buf + out, sizeof(g_tparm_buf) - out,
                           "%02ld", params[param_idx]);
            param_idx++;
            p++;
            break;

        case '3':
            out += snprintf(g_tparm_buf + out, sizeof(g_tparm_buf) - out,
                           "%03ld", params[param_idx]);
            param_idx++;
            p++;
            break;

        case 'c':
            g_tparm_buf[out++] = (char)params[param_idx];
            param_idx++;
            p++;
            break;

        case 's':
            out += snprintf(g_tparm_buf + out, sizeof(g_tparm_buf) - out,
                           "%ld", params[param_idx]);
            param_idx++;
            p++;
            break;

        case 'p':
            p++;
            if (*p >= '1' && *p <= '9') {
                int idx = *p - '1';
                /* Push param onto stack — but we use sequential access */
                /* For simple %p1%d patterns, we track param_idx */
                param_idx = idx;  /* set which param to use next */
                p++;
            }
            break;

        case '0':
            p++;
            if (*p == '2') {
                out += snprintf(g_tparm_buf + out, sizeof(g_tparm_buf) - out,
                               "%02ld", params[param_idx]);
                param_idx++;
                p++;
            } else if (*p == '3') {
                out += snprintf(g_tparm_buf + out, sizeof(g_tparm_buf) - out,
                               "%03ld", params[param_idx]);
                param_idx++;
                p++;
            } else if (*p == 'd') {
                out += snprintf(g_tparm_buf + out, sizeof(g_tparm_buf) - out,
                               "%ld", params[param_idx]);
                param_idx++;
                p++;
            } else {
                /* unknown format */
                p++;
            }
            break;

        case 'P':
            /* %P<var> — set variable, skip */
            p++;
            if (*p) p++;
            break;

        case 'g':
            /* %g<var> — get variable, skip */
            p++;
            if (*p) p++;
            break;

        case '\'':
            /* %'c' — push char constant */
            p++;
            if (*p) {
                params[param_idx] = (long)(*p);
                p++;
            }
            if (*p == '\'') p++;
            break;

        case '{':
            /* %{num} — push numeric constant */
            p++;
            {
                long val = 0;
                while (*p >= '0' && *p <= '9') {
                    val = val * 10 + (*p - '0');
                    p++;
                }
                params[param_idx] = val;
            }
            if (*p == '}') p++;
            break;

        case 'l':
            /* %l — string length, skip for now */
            p++;
            break;

        case '+': case '-': case '*': case '/': case 'm':
            /* arithmetic: pop two, push result */
            {
                long b = params[param_idx > 0 ? param_idx - 1 : 0];
                long a = params[param_idx > 1 ? param_idx - 2 : 0];
                long r = 0;
                switch (*p) {
                    case '+': r = a + b; break;
                    case '-': r = a - b; break;
                    case '*': r = a * b; break;
                    case '/': r = b ? a / b : 0; break;
                    case 'm': r = b ? a % b : 0; break;
                }
                params[param_idx] = r;
            }
            p++;
            break;

        case '&': case '|': case '^':
            /* bitwise ops */
            p++;
            break;

        case '=': case '>': case '<':
            /* comparison ops */
            p++;
            break;

        case '!': case '~':
            /* unary ops */
            p++;
            break;

        case '?':
            /* if-then-else start — simplified: just skip conditionals */
            p++;
            break;

        case 't':
            /* then branch */
            p++;
            break;

        case 'e':
            /* else branch */
            p++;
            break;

        case ';':
            /* end if */
            p++;
            break;

        case 'B':
            /* BCD encode */
            p++;
            break;

        case 'D':
            /* reverse encoding */
            p++;
            break;

        case 'x':
            /* hex output */
            out += snprintf(g_tparm_buf + out, sizeof(g_tparm_buf) - out,
                           "%lx", params[param_idx]);
            param_idx++;
            p++;
            break;

        default:
            /* unknown % escape, skip */
            if (*p) p++;
            break;
        }
    }

    g_tparm_buf[out] = 0;
    return g_tparm_buf;
}

/* ------------------------------------------------------------------ */
/* tgoto — termcap cursor motion (simplified)                         */
/* ------------------------------------------------------------------ */

static char g_tgoto_buf[64];

char *tgoto(const char *cap, int col, int row) {
    if (!cap) return NULL;

    /* Check if it's a termcap-style or terminfo-style string */
    int has_terminfo = 0;
    for (const char *p = cap; *p; p++) {
        if (*p == '%' && p[1] == 'p') {
            has_terminfo = 1;
            break;
        }
    }

    if (has_terminfo) {
        /* Use tparm for terminfo format */
        /* In terminfo, cup takes (row, col) — but termcap tgoto takes (col, row) */
        /* Check if %i is present (1-based) */
        return tparm(cap, row, col, 0, 0, 0, 0, 0, 0, 0);
    }

    /* Termcap format: %d, %2, %3, %., %+, %c, etc. */
    /* Simplified: handle %d, %2, %3, %i, %c */
    int params[2] = { col, row };
    int pi = 0;
    int incr = 0;
    int out = 0;
    const char *p = cap;

    /* Check for %i at start */
    while (*p == '%' && p[1] == 'i') {
        incr = 1;
        p += 2;
    }
    if (incr) {
        params[0]++;
        params[1]++;
    }

    while (*p && out < (int)sizeof(g_tgoto_buf) - 1) {
        if (*p != '%') {
            g_tgoto_buf[out++] = *p++;
            continue;
        }
        p++;
        switch (*p) {
        case 'd':
            out += snprintf(g_tgoto_buf + out, sizeof(g_tgoto_buf) - out,
                           "%d", params[pi++]);
            p++;
            break;
        case '2':
            out += snprintf(g_tgoto_buf + out, sizeof(g_tgoto_buf) - out,
                           "%2d", params[pi++]);
            p++;
            break;
        case '3':
            out += snprintf(g_tgoto_buf + out, sizeof(g_tgoto_buf) - out,
                           "%3d", params[pi++]);
            p++;
            break;
        case '0':
            p++;
            if (*p == '2') {
                out += snprintf(g_tgoto_buf + out, sizeof(g_tgoto_buf) - out,
                               "%02d", params[pi++]);
                p++;
            } else if (*p == '3') {
                out += snprintf(g_tgoto_buf + out, sizeof(g_tgoto_buf) - out,
                               "%03d", params[pi++]);
                p++;
            } else {
                out += snprintf(g_tgoto_buf + out, sizeof(g_tgoto_buf) - out,
                               "%d", params[pi++]);
            }
            break;
        case 'c':
            g_tgoto_buf[out++] = (char)params[pi++];
            p++;
            break;
        case '.':
            g_tgoto_buf[out++] = (char)params[pi++];
            p++;
            break;
        case 'i':
            params[0]++;
            params[1]++;
            p++;
            break;
        case '%':
            g_tgoto_buf[out++] = '%';
            p++;
            break;
        case 'p':
            p++;
            if (*p >= '1' && *p <= '2') {
                pi = *p - '1';
                p++;
            }
            break;
        case 'B':
            /* BCD: (param/10)*16 + (param%10) */
            if (pi < 2) {
                int v = params[pi];
                params[pi] = (v / 10) * 16 + (v % 10);
            }
            p++;
            break;
        case 'n':
            /* swap params */
            {
                int tmp = params[0];
                params[0] = params[1];
                params[1] = tmp;
            }
            p++;
            break;
        default:
            if (*p) p++;
            break;
        }
    }

    g_tgoto_buf[out] = 0;
    return g_tgoto_buf;
}

/* ------------------------------------------------------------------ */
/* tputs — output string with padding                                 */
/* ------------------------------------------------------------------ */

int tputs(const char *str, int affcnt, int (*putc_)(int)) {
    (void)affcnt;
    if (!str) return 0;

    /* Skip padding info: digits followed by '*' and maybe '$' */
    const char *p = str;
    while (*p >= '0' && *p <= '9') p++;
    if (*p == '*') p++;
    if (*p == '$') p++;

    /* Output the string character by character */
    while (*p) {
        if (putc_) {
            putc_((int)(unsigned char)*p);
        } else {
            putchar(*p);
        }
        p++;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* termcap interface                                                   */
/* ------------------------------------------------------------------ */

int tgetent(char *bp, const char *name) {
    (void)bp;
    (void)name;  /* We support any terminal name — all use xterm caps */

    g_initialized = 1;
    g_tcap_pos = 0;

    /* Set UP and BC for termcap compatibility */
    UP = (char *)CSI "A";
    BC = (char *)"\b";

    return 1;  /* success */
}

int tgetflag(const char *id) {
    return find_bool_cap(id);
}

int tgetnum(const char *id) {
    return find_num_cap(id);
}

char *tgetstr(const char *id, char **area) {
    const char *val = find_str_cap(id);
    if (!val) return NULL;

    if (area && *area) {
        /* Copy to user buffer (termcap convention) */
        char *dst = *area;
        const char *src = val;
        while (*src) *dst++ = *src++;
        *dst++ = 0;
        *area = dst;
        return (char *)val;  /* Return pointer to our copy */
    }

    return (char *)val;
}

/* ------------------------------------------------------------------ */
/* terminfo interface                                                  */
/* ------------------------------------------------------------------ */

int setupterm(const char *term, int fildes, int *errret) {
    (void)term;
    (void)fildes;

    g_initialized = 1;

    if (errret) *errret = 1;  /* success */
    return 0;  /* OK */
}

int del_curterm(TERMINAL *oterm) {
    (void)oterm;
    return 0;
}

/* terminfo capability getters — long names */
char *tigetstr(const char *capname) {
    if (!capname) return (char *)-1;

    /* Try to find by name — terminfo uses long names */
    const char *val = find_str_cap(capname);
    if (val) return (char *)val;

    return (char *)-1;  /* not found — terminfo convention */
}

int tigetflag(const char *capname) {
    if (!capname) return -1;
    return find_bool_cap(capname);
}

int tigetnum(const char *capname) {
    if (!capname) return -2;
    int v = find_num_cap(capname);
    return v;
}

/* ------------------------------------------------------------------ */
/* Additional functions zsh may need                                   */
/* ------------------------------------------------------------------ */

/* curs_set — set cursor visibility (0=invisible, 1=normal, 2=very visible) */
int curs_set(int visibility) {
    switch (visibility) {
    case 0: tputs(CSI "?25l", 0, NULL); break;
    case 1: tputs(CSI "?25h", 0, NULL); break;
    case 2: tputs(CSI "?25h", 0, NULL); break;
    }
    return 1;  /* previous visibility */
}

/* resetty/savetty — save/restore terminal state */
int savetty(void) { return 0; }
int resetty(void) { return 0; }

/* flushinp — flush input */
int flushinp(void) { return 0; }

/* napms — sleep N milliseconds */
int napms(int ms) {
    (void)ms;
    return 0;
}

/* baudrate — return terminal speed */
int baudrate(void) { return 9600; }

/* has_ic — has insert/delete char */
int has_ic(void) { return 1; }

/* has_il — has insert/delete line */
int has_il(void) { return 1; }

/* termname — return terminal name */
const char *termname(void) { return "xterm"; }

/* longname — return long terminal name */
char *longname(void) { return (char *)"xterm-256color"; }

/* keyname — return name for key */
const char *keyname(int c) {
    static char buf[16];
    if (c >= 0 && c < 32) {
        snprintf(buf, sizeof(buf), "^%c", c + '@');
        return buf;
    }
    snprintf(buf, sizeof(buf), "KEY_%d", c);
    return buf;
}

/* define_key — define a key sequence */
int define_key(const char *sequence, int symbol) {
    (void)sequence; (void)symbol;
    return 0;
}

/* keypad — enable/disable keypad */
int keypad(void *win, int bf) {
    (void)win;
    if (bf) tputs(ESC "=", 0, NULL);
    else tputs(ESC ">", 0, NULL);
    return 0;
}

/* raw/noraw — raw mode */
int raw(void) { return 0; }
int noraw(void) { return 0; }
int cbreak(void) { return 0; }
int nocbreak(void) { return 0; }
int echo(void) { return 0; }
int noecho(void) { return 0; }
int nl(void) { return 0; }
int nonl(void) { return 0; }

/* getcurx/getcury — cursor position */
int getcurx(void *win) { (void)win; return 0; }
int getcury(void *win) { (void)win; return 0; }
int getmaxx(void *win) { (void)win; return 80; }
int getmaxy(void *win) { (void)win; return 25; }

/* addch — output a character */
int addch(int ch) { putchar(ch); return 0; }
int mvaddch(int y, int x, int ch) {
    char *s = tgoto(CSI "%i%p1%d;%p2%dH", x, y);
    tputs(s, 0, NULL);
    putchar(ch);
    return 0;
}

/* mvprintw — move and print */
int mvprintw(int y, int x, const char *fmt, ...) {
    char *s = tgoto(CSI "%i%p1%d;%p2%dH", x, y);
    tputs(s, 0, NULL);
    /* Just print the format string without args for simplicity */
    (void)fmt;
    return 0;
}

/* refresh */
int refresh(void) { fflush(stdout); return 0; }
int endwin(void) { tputs(CSI "0m", 0, NULL); return 0; }
int isendwin(void) { return 0; }

/* initscr — initialize curses */
void *initscr(void) {
    g_initialized = 1;
    return (void *)1;
}

/* newterm — new terminal */
void *newterm(const char *type, void *outfd, void *infd) {
    (void)type; (void)outfd; (void)infd;
    return (void *)1;
}

/* delwin — delete window */
int delwin(void *win) { (void)win; return 0; }

/* newwin — create window */
void *newwin(int nlines, int ncols, int begin_y, int begin_x) {
    (void)nlines; (void)ncols; (void)begin_y; (void)begin_x;
    return (void *)1;
}

/* wrefresh */
int wrefresh(void *win) { (void)win; fflush(stdout); return 0; }

/* getyx — get cursor position */
void getyx(void *win, int y, int x) { (void)win; y = 0; x = 0; }

/* tigetstr with tparm for parameterized strings */
char *tiparm(const char *str, ...) {
    /* Simplified: just return the string as-is */
    return (char *)str;
}
