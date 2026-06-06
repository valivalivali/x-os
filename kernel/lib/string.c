#include "kernel/lib/string.h"
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n && ((uintptr_t)d & 7)) {
        *d++ = *s++;
        n--;
    }

    uint64_t *d8 = (uint64_t *)d;
    const uint64_t *s8 = (const uint64_t *)s;
    while (n >= 64) {
        d8[0] = s8[0]; d8[1] = s8[1]; d8[2] = s8[2]; d8[3] = s8[3];
        d8[4] = s8[4]; d8[5] = s8[5]; d8[6] = s8[6]; d8[7] = s8[7];
        d8 += 8; s8 += 8; n -= 64;
    }
    while (n >= 8) {
        *d8++ = *s8++;
        n -= 8;
    }

    d = (uint8_t *)d8;
    s = (const uint8_t *)s8;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t val = (uint8_t)c;
    uint8_t *d = dst;

    while (n && ((uintptr_t)d & 7)) {
        *d++ = val;
        n--;
    }

    uint64_t val64 = val;
    val64 |= val64 << 8;
    val64 |= val64 << 16;
    val64 |= val64 << 32;

    uint64_t *d8 = (uint64_t *)d;
    while (n >= 64) {
        d8[0] = val64; d8[1] = val64; d8[2] = val64; d8[3] = val64;
        d8[4] = val64; d8[5] = val64; d8[6] = val64; d8[7] = val64;
        d8 += 8; n -= 64;
    }
    while (n >= 8) {
        *d8++ = val64;
        n -= 8;
    }

    d = (uint8_t *)d8;
    while (n--) *d++ = val;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (d == s || n == 0) return dst;

    if (d < s) {
        while (n && ((uintptr_t)d & 7)) {
            *d++ = *s++;
            n--;
        }
        uint64_t *d8 = (uint64_t *)d;
        const uint64_t *s8 = (const uint64_t *)s;
        while (n >= 64) {
            d8[0] = s8[0]; d8[1] = s8[1]; d8[2] = s8[2]; d8[3] = s8[3];
            d8[4] = s8[4]; d8[5] = s8[5]; d8[6] = s8[6]; d8[7] = s8[7];
            d8 += 8; s8 += 8; n -= 64;
        }
        while (n >= 8) {
            *d8++ = *s8++;
            n -= 8;
        }
        d = (uint8_t *)d8;
        s = (const uint8_t *)s8;
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n && ((uintptr_t)d & 7)) {
            *--d = *--s;
            n--;
        }
        uint64_t *d8 = (uint64_t *)d;
        const uint64_t *s8 = (const uint64_t *)s;
        while (n >= 64) {
            d8 -= 8; s8 -= 8;
            d8[7] = s8[7]; d8[6] = s8[6]; d8[5] = s8[5]; d8[4] = s8[4];
            d8[3] = s8[3]; d8[2] = s8[2]; d8[1] = s8[1]; d8[0] = s8[0];
            n -= 64;
        }
        while (n >= 8) {
            *--d8 = *--s8;
            n -= 8;
        }
        d = (uint8_t *)d8;
        s = (const uint8_t *)s8;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a;
    const uint8_t *y = b;

    while (n && (((uintptr_t)x & 7) || ((uintptr_t)y & 7))) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++; n--;
    }

    const uint64_t *x8 = (const uint64_t *)x;
    const uint64_t *y8 = (const uint64_t *)y;
    while (n >= 8) {
        if (*x8 != *y8) {
            x = (const uint8_t *)x8;
            y = (const uint8_t *)y8;
            for (int i = 0; i < 8; i++) {
                if (x[i] != y[i]) return (int)x[i] - (int)y[i];
            }
        }
        x8++; y8++; n -= 8;
    }

    x = (const uint8_t *)x8;
    y = (const uint8_t *)y8;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++; y++;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == 0)
            return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
    }
    return 0;
}

char *strcpy(char *dst, const char *src) {
    char *p = dst;
    while ((*p++ = *src++)) { }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

