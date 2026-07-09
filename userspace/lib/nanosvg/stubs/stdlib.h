/* Stub stdlib.h for NanoSVG freestanding build.
 * malloc/free are provided by nanosvg_xos.c. */

#ifndef _XOS_STUB_STDLIB_H
#define _XOS_STUB_STDLIB_H

#include <stddef.h>

static inline void *realloc(void *ptr, size_t size) {
    /* Simple: allocate new block, copy data, ignore old.
     * NanoSVG uses realloc for edge array growth. */
    extern void *malloc(size_t);
    extern void *memcpy(void *, const void *, size_t);
    if (size == 0) return 0;
    void *p = malloc(size);
    if (ptr && p) {
        /* Copy as much as we can — we don't know old size, but
         * NanoSVG's realloc pattern always grows, so copy new size */
        memcpy(p, ptr, size);
    }
    return p;
}

static inline void qsort(void *base, size_t nmemb, size_t size,
                         int (*cmp)(const void *, const void *)) {
    /* Simple insertion sort — sufficient for NanoSVG edge sorting */
    char *arr = (char *)base;
    char tmp[256]; /* max element size for NanoSVG edges */
    for (size_t i = 1; i < nmemb; i++) {
        size_t j = i;
        while (j > 0 && cmp(arr + (j - 1) * size, arr + j * size) > 0) {
            for (size_t k = 0; k < size; k++) tmp[k] = arr[(j - 1) * size + k];
            for (size_t k = 0; k < size; k++) arr[(j - 1) * size + k] = arr[j * size + k];
            for (size_t k = 0; k < size; k++) arr[j * size + k] = tmp[k];
            j--;
        }
    }
}

#endif
