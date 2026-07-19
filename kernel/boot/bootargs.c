#include "kernel/boot/bootargs.h"
#include "kernel/lib/string.h"
#include <stdint.h>

static char g_boot_args[BOOT_ARGS_MAX];
static bool g_verbose;

static bool is_sep(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\0';
}

static int copy_num(long long val, void *to, size_t max_len) {
    if (!to || max_len == 0) return 0;
    if (max_len >= sizeof(long long)) {
        *(long long *)to = val;
    } else if (max_len >= sizeof(int)) {
        *(int *)to = (int)val;
    } else if (max_len >= sizeof(short)) {
        *(short *)to = (short)val;
    } else {
        *(char *)to = (char)val;
    }
    return 0;
}

static void copy_str(const char *from, char *to, size_t max_len) {
    size_t i = 0;
    if (!to || max_len == 0) return;
    while (from[i] && !is_sep(from[i]) && i + 1 < max_len) {
        to[i] = from[i];
        i++;
    }
    to[i] = '\0';
}

static bool parse_one(const char *args, const char *want, void *out, size_t out_len) {
    if (!args || !want || !*args) return false;

    while (*args && is_sep(*args)) args++;

    while (*args) {
        bool is_flag = (*args == '-');
        const char *start = args;
        const char *cp = args;

        while (*cp && !is_sep(*cp) && *cp != '=') cp++;

        size_t n = (size_t)(cp - start);
        size_t want_len = strlen(want);
        if (n == want_len && strncmp(start, want, n) == 0) {
            if (is_flag) {
                if (out && out_len) copy_num(1, out, out_len);
                return true;
            }
            if (*cp == '=') {
                cp++;
                if (out && out_len) {
                    /* Prefer number if all digits; else string. */
                    bool digits = (*cp == '-' || (*cp >= '0' && *cp <= '9'));
                    const char *p = cp + (*cp == '-');
                    while (*p && !is_sep(*p)) {
                        if (*p < '0' || *p > '9') {
                            if (!(*p == 'x' && p == cp + (*cp == '0' ? 1 : 0))) {
                                digits = false;
                                break;
                            }
                        }
                        p++;
                    }
                    if (digits && out_len <= sizeof(long long)) {
                        long long v = 0;
                        int neg = 0;
                        if (*cp == '-') { neg = 1; cp++; }
                        if (cp[0] == '0' && (cp[1] == 'x' || cp[1] == 'X')) {
                            cp += 2;
                            while (*cp && !is_sep(*cp)) {
                                char c = *cp++;
                                v <<= 4;
                                if (c >= '0' && c <= '9') v |= c - '0';
                                else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
                                else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
                            }
                        } else {
                            while (*cp >= '0' && *cp <= '9') {
                                v = v * 10 + (*cp - '0');
                                cp++;
                            }
                        }
                        if (neg) v = -v;
                        copy_num(v, out, out_len);
                    } else {
                        copy_str(cp, (char *)out, out_len);
                    }
                }
                return true;
            }
            /* Bare key without '=' — treat as present / true. */
            if (out && out_len) copy_num(1, out, out_len);
            return true;
        }

        while (*args && !is_sep(*args)) args++;
        while (*args && is_sep(*args)) args++;
    }
    return false;
}

void bootargs_init(const char *cmdline) {
    g_boot_args[0] = '\0';
    g_verbose = false;

    if (cmdline && cmdline[0]) {
        size_t i = 0;
        while (cmdline[i] && i + 1 < BOOT_ARGS_MAX) {
            g_boot_args[i] = cmdline[i];
            i++;
        }
        g_boot_args[i] = '\0';
    }

    /* macOS-style: silent unless -v (XNU verbose boot). */
    g_verbose = bootargs_has("-v") || bootargs_has("v");
}

bool bootargs_has(const char *name) {
    return parse_one(g_boot_args, name, NULL, 0);
}

bool bootargs_parse(const char *name, void *out, size_t out_len) {
    return parse_one(g_boot_args, name, out, out_len);
}

const char *bootargs_raw(void) {
    return g_boot_args;
}

bool bootargs_verbose(void) {
    return g_verbose;
}
