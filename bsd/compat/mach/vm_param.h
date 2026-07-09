#pragma once
#define PAGE_SIZE       4096
#define PAGE_SHIFT      12
#define PAGE_MASK       0xFFF
#define PAGE_SIZE_64    4096ULL
#define trunc_page(x)   ((x) & ~0xFFFULL)
#define round_page(x)   (((x) + 0xFFF) & ~0xFFFULL)
#define atop(x)         ((x) >> 12)
#define ptoa(x)         ((x) << 12)
