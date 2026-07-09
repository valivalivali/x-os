/* Minimal curses/termcap stub library for zsh on x-os.
 * All functions return success/empty so zsh links and runs
 * without a real terminal/curses implementation. */

#include <stddef.h>

typedef struct TERMINAL TERMINAL;
TERMINAL *cur_term = (void *)0;

/* termcap stubs */
int tgetent(char *bp, const char *name) {
    (void)bp; (void)name;
    return 0;
}

int tgetflag(const char *id) {
    (void)id;
    return 0;
}

int tgetnum(const char *id) {
    (void)id;
    return -1;
}

char *tgetstr(const char *id, char **area) {
    (void)id; (void)area;
    return (char *)0;
}

char *tgoto(const char *cap, int col, int row) {
    (void)cap; (void)col; (void)row;
    return (char *)0;
}

int tputs(const char *str, int affcnt, int (*putc_)(int)) {
    (void)str; (void)affcnt;
    if (putc_) return putc_(0);
    return 0;
}

/* terminfo stubs */
int setupterm(const char *term, int fildes, int *errret) {
    (void)term; (void)fildes;
    if (errret) *errret = 1;
    return 0;
}

int del_curterm(TERMINAL *oterm) {
    (void)oterm;
    return 0;
}
