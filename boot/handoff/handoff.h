#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Bootloader-agnostic view of what the firmware/bootloader handed us:
 * a linear framebuffer, the physical memory map, and the higher-half
 * direct-map (HHDM) offset used to access physical memory. */

typedef struct {
    uint32_t *addr;     /* pixel base (directly writable virtual address) */
    uint64_t  width;
    uint64_t  height;
    uint64_t  pitch;    /* bytes per scanline */
    uint16_t  bpp;
    uint8_t   red_shift;
    uint8_t   green_shift;
    uint8_t   blue_shift;
} hand_framebuffer_t;

typedef struct {
    uint64_t base;
    uint64_t length;
    uint64_t type;      /* HAND_MEM_* */
} hand_memmap_entry_t;

enum {
    HAND_MEM_USABLE = 0,
    HAND_MEM_RESERVED,
    HAND_MEM_ACPI_RECLAIMABLE,
    HAND_MEM_ACPI_NVS,
    HAND_MEM_BAD,
    HAND_MEM_BOOTLOADER_RECLAIMABLE,
    HAND_MEM_KERNEL_AND_MODULES,
    HAND_MEM_FRAMEBUFFER,
};

typedef struct {
    hand_framebuffer_t   fb;
    uint64_t             hhdm_offset;
    hand_memmap_entry_t *memmap;
    uint64_t             memmap_count;
    bool                 valid;
} handoff_t;

/* Parse bootloader requests once and return the unified handoff. */
const handoff_t *handoff_get(void);
