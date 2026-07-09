/* nanosvg_xos.c — NanoSVG integration for x-os freestanding userspace.
 *
 * Provides malloc/free/memcpy/string/math wrappers needed by NanoSVG
 * using x-os kernel syscalls (SYS_MEM_ALLOC / SYS_MEM_FREE).
 *
 * Define NANOSVG_IMPLEMENTATION and NANOSVGRAST_IMPLEMENTATION here
 * so the single-header libraries expand their implementation into
 * this translation unit only.
 */

#include <stdint.h>
#include <stddef.h>

/* ---- Memory management (via kernel syscalls) ----------------------------- */

#include "kernel/include/syscall.h"

/* Simple bump allocator backed by SYS_MEM_ALLOC pages.
 * NanoSVG allocates and frees in a parse-then-render pattern,
 * so a simple page-grabbing malloc with a free-list is sufficient. */

#define NSVG_PAGE_SIZE 4096

/* Pool of allocated pages for NanoSVG */
static void *nsvg_pages[256];
static int nsvg_page_count = 0;
static size_t nsvg_bump_offset = 0;

void *malloc(size_t size) {
    if (size == 0) return NULL;

    /* Align to 16 bytes */
    size = (size + 15) & ~15;

    /* Try bump allocator first */
    if (nsvg_bump_offset + size <= NSVG_PAGE_SIZE) {
        void *ptr = (char *)nsvg_pages[nsvg_page_count - 1] + nsvg_bump_offset;
        nsvg_bump_offset += size;
        return ptr;
    }

    /* Allocate a new page */
    if (nsvg_page_count >= 256) return NULL;

    /* Use a fixed virtual address range for NanoSVG pages */
    uint64_t va = 0x0000400000000000ULL + (uint64_t)nsvg_page_count * NSVG_PAGE_SIZE;
    if (sys_mem_alloc(va, VMM_RW | VMM_U) < 0) return NULL;

    nsvg_pages[nsvg_page_count] = (void *)va;
    nsvg_page_count++;
    nsvg_bump_offset = size;
    return (void *)va;
}

void free(void *ptr) {
    /* NanoSVG calls nsvgDelete which frees all allocations.
     * Our bump allocator doesn't reclaim individual blocks,
     * but nsvg_reset() (called below) reclaims everything. */
    (void)ptr;
}

/* Reset the entire NanoSVG pool — call after nsvgDelete + nsvgDeleteRasterizer */
void nsvg_pool_reset(void) {
    nsvg_bump_offset = 0;
    /* Keep pages allocated for reuse; just reset bump pointer.
     * If memory pressure becomes an issue, we can free pages here. */
}

/* ---- String functions ---------------------------------------------------- */

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memset(void *s, int c, size_t n) {
    unsigned char *p = s;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)c;
    return s;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = a, *pb = b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

char *strcpy(char *dst, const char *src) {
    int i = 0;
    while (src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src) {
    int i = 0, j = 0;
    while (dst[i]) i++;
    while (src[j]) { dst[i + j] = src[j]; j++; }
    dst[i + j] = '\0';
    return dst;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : NULL;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

int atoi(const char *s) {
    int sign = 1, val = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    return val * sign;
}

long strtol(const char *str, char **endptr, int base) {
    long val = 0;
    int sign = 1;
    const char *p = str;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') p++;
    if (base == 0) {
        if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
        else if (*p == '0') { base = 8; p++; }
        else base = 10;
    }
    while (*p) {
        int digit;
        if (*p >= '0' && *p <= '9') digit = *p - '0';
        else if (*p >= 'a' && *p <= 'f') digit = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') digit = *p - 'A' + 10;
        else break;
        if (digit >= base) break;
        val = val * base + digit;
        p++;
    }
    if (endptr) *endptr = (char *)p;
    return val * sign;
}

long long strtoll(const char *str, char **endptr, int base) {
    long long val = 0;
    int sign = 1;
    const char *p = str;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') p++;
    if (base == 0) {
        if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
        else if (*p == '0') { base = 8; p++; }
        else base = 10;
    }
    while (*p) {
        int digit;
        if (*p >= '0' && *p <= '9') digit = *p - '0';
        else if (*p >= 'a' && *p <= 'f') digit = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') digit = *p - 'A' + 10;
        else break;
        if (digit >= base) break;
        val = val * base + digit;
        p++;
    }
    if (endptr) *endptr = (char *)p;
    return val * sign;
}

/* Minimal sscanf — supports only the patterns NanoSVG uses:
 *   "#%2x%2x%2x" and "rgb(%u, %u, %u)" */
int sscanf(const char *str, const char *fmt, ...) {
    /* NanoSVG uses:
     * 1. sscanf(str, "#%2x%2x%2x", &r, &g, &b) — hex color
     * 2. sscanf(str, "rgb(%u, %u, %u)", &r, &g, &b) — rgb color
     * We handle both by simple pattern matching. */
    (void)fmt;

    /* Check for "#" prefix (hex color) */
    if (*str == '#') {
        str++;
        /* Parse 2-digit hex values */
        unsigned int *out[3];
        /* Access varargs */
        __builtin_va_list args;
        __builtin_va_start(args, fmt);
        int count = 0;
        for (int i = 0; i < 3; i++) {
            unsigned int *p = __builtin_va_arg(args, unsigned int *);
            unsigned int val = 0;
            for (int j = 0; j < 2 && *str; j++) {
                int digit = 0;
                if (*str >= '0' && *str <= '9') digit = *str - '0';
                else if (*str >= 'a' && *str <= 'f') digit = *str - 'a' + 10;
                else if (*str >= 'A' && *str <= 'F') digit = *str - 'A' + 10;
                else { __builtin_va_end(args); return count; }
                val = val * 16 + digit;
                str++;
            }
            *p = val;
            count++;
        }
        __builtin_va_end(args);
        return count;
    }

    /* Check for "rgb(" prefix */
    if (str[0] == 'r' && str[1] == 'g' && str[2] == 'b' && str[3] == '(') {
        str += 4;
        __builtin_va_list args;
        __builtin_va_start(args, fmt);
        int count = 0;
        for (int i = 0; i < 3; i++) {
            /* Skip whitespace */
            while (*str == ' ') str++;
            unsigned int val = 0;
            while (*str >= '0' && *str <= '9') {
                val = val * 10 + (*str - '0');
                str++;
            }
            unsigned int *p = __builtin_va_arg(args, unsigned int *);
            *p = val;
            count++;
            /* Skip comma and spaces */
            while (*str == ' ' || *str == ',') str++;
        }
        __builtin_va_end(args);
        return count;
    }

    return 0;
}

double atof(const char *s) {
    double val = 0.0, power = 1.0;
    int sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') {
        val = val * 10.0 + (*s - '0');
        s++;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            val = val * 10.0 + (*s - '0');
            power *= 10.0;
            s++;
        }
    }
    /* Handle exponent */
    if (*s == 'e' || *s == 'E') {
        s++;
        int esign = 1;
        if (*s == '-') { esign = -1; s++; }
        else if (*s == '+') s++;
        int exp = 0;
        while (*s >= '0' && *s <= '9') {
            exp = exp * 10 + (*s - '0');
            s++;
        }
        double mul = 1.0;
        for (int i = 0; i < exp; i++) mul *= 10.0;
        if (esign > 0) val *= mul; else val /= mul;
    }
    return sign * val / power;
}

/* ---- Math functions (software float) ------------------------------------- */

float powf(float base, float exp) {
    /* Simple integer exponent case */
    if (exp == 0.0f) return 1.0f;
    if (exp == 1.0f) return base;
    if (exp == 2.0f) return base * base;
    if (exp == 0.5f) {
        /* Newton's method sqrt */
        if (base < 0) return 0.0f/0.0f; /* NaN */
        float x = base;
        for (int i = 0; i < 20; i++) {
            x = 0.5f * (x + base / x);
        }
        return x;
    }
    /* General case: not commonly needed by NanoSVG */
    float result = 1.0f;
    int e = (int)exp;
    if ((float)e == exp) {
        if (e < 0) { base = 1.0f / base; e = -e; }
        for (int i = 0; i < e; i++) result *= base;
        return result;
    }
    return result; /* fallback */
}

float floorf(float x) {
    int i = (int)x;
    if (x < 0 && (float)i != x) i--;
    return (float)i;
}

float ceilf(float x) {
    int i = (int)x;
    if (x > 0 && (float)i != x) i++;
    return (float)i;
}

float roundf(float x) {
    if (x >= 0) return floorf(x + 0.5f);
    return ceilf(x - 0.5f);
}

float fabsf(float x) {
    return x < 0 ? -x : x;
}

float fmodf(float a, float b) {
    if (b == 0.0f) return 0.0f;
    return a - b * floorf(a / b);
}

float cosf(float x) {
    /* Taylor series, reduce to [-pi, pi] first */
    const float PI = 3.14159265358979f;
    const float TWO_PI = 2.0f * PI;
    while (x > PI) x -= TWO_PI;
    while (x < -PI) x += TWO_PI;
    float result = 1.0f;
    float term = 1.0f;
    for (int i = 1; i <= 12; i++) {
        term *= -x * x / ((2 * i - 1) * (2 * i));
        result += term;
    }
    return result;
}

float sinf(float x) {
    return cosf(x - 3.14159265358979f / 2.0f);
}

float tanf(float x) {
    float c = cosf(x);
    if (c == 0.0f) return 0.0f;
    return sinf(x) / c;
}

float atan2f(float y, float x) {
    if (x == 0.0f) {
        if (y > 0) return 1.57079632679f;
        if (y < 0) return -1.57079632679f;
        return 0.0f;
    }
    float r = y / x;
    float abs_r = r < 0 ? -r : r;
    /* atan approximation */
    float a;
    if (abs_r <= 1.0f) {
        a = abs_r / (1.0f + 0.28f * abs_r * abs_r);
    } else {
        a = 1.57079632679f - (1.0f / abs_r) / (1.0f + 0.28f / (abs_r * abs_r));
    }
    if (r < 0) a = -a;
    if (x < 0) {
        if (y >= 0) a += 3.14159265358979f;
        else a -= 3.14159265358979f;
    }
    return a;
}

float acosf(float x) {
    if (x <= -1.0f) return 3.14159265358979f;
    if (x >= 1.0f) return 0.0f;
    /* Newton's method */
    float a = 0.0f;
    for (int i = 0; i < 30; i++) {
        float c = cosf(a);
        float da = (c - x) / (-sinf(a));
        a -= da;
        if (da < 0) da = -da;
        if (da < 0.0001f) break;
    }
    return a;
}

float sqrtf(float x) {
    if (x < 0) return 0.0f;
    if (x == 0) return 0.0f;
    float r = x;
    for (int i = 0; i < 20; i++) {
        r = 0.5f * (r + x / r);
    }
    return r;
}

/* Double versions (NanoSVG uses some) */
double pow(double base, double exp) { return (double)powf((float)base, (float)exp); }
double floor(double x) { return (double)floorf((float)x); }
double ceil(double x) { return (double)ceilf((float)x); }
double fabs(double x) { return (double)fabsf((float)x); }
double fmod(double a, double b) { return (double)fmodf((float)a, (float)b); }
double cos(double x) { return (double)cosf((float)x); }
double sin(double x) { return (double)sinf((float)x); }
double atan2(double y, double x) { return (double)atan2f((float)y, (float)x); }
double acos(double x) { return (double)acosf((float)x); }
double sqrt(double x) { return (double)sqrtf((float)x); }
double round(double x) { return (double)roundf((float)x); }

/* ---- NanoSVG implementation ---------------------------------------------- */

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"

#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
