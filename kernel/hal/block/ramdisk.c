/* RAM disk — allocates contiguous physical frames and maps them.
 * Used for testing the filesystem before any real storage driver exists. */

#include "kernel/hal/block/block_dev.h"
#include "kernel/memory/pmm.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

#define RAMDISK_BLOCKS 1024   /* 4 MiB */
#define BLOCK_SIZE     4096

typedef struct {
    block_dev_t dev;
    uint8_t    *base;         /* kernel virtual address of the RAM area */
} ramdisk_t;

static ramdisk_t g_ramdisk;

static bool ramdisk_read(block_dev_t *dev, uint64_t lba,
                         uint32_t count, void *buf) {
    ramdisk_t *rd = (ramdisk_t *)dev->private;
    if (lba + count > dev->block_count) return false;
    memcpy(buf, rd->base + lba * BLOCK_SIZE, (size_t)count * BLOCK_SIZE);
    return true;
}

static bool ramdisk_write(block_dev_t *dev, uint64_t lba,
                          uint32_t count, const void *buf) {
    ramdisk_t *rd = (ramdisk_t *)dev->private;
    if (lba + count > dev->block_count) return false;
    memcpy(rd->base + lba * BLOCK_SIZE, buf, (size_t)count * BLOCK_SIZE);
    return true;
}

block_dev_t *ramdisk_create(void) {
    uint64_t phys = pmm_alloc_contig(RAMDISK_BLOCKS);
    if (!phys) {
        kprintf("[ramdisk] pmm_alloc_contig failed\n");
        return NULL;
    }
    memset(&g_ramdisk, 0, sizeof(g_ramdisk));
    g_ramdisk.dev.block_count = RAMDISK_BLOCKS;
    g_ramdisk.dev.read  = ramdisk_read;
    g_ramdisk.dev.write = ramdisk_write;
    g_ramdisk.dev.private = &g_ramdisk;
    g_ramdisk.base   = (uint8_t *)phys_to_virt(phys);

    kprintf("[ramdisk] 4 MiB ready at %p\n", (void *)g_ramdisk.base);
    return &g_ramdisk.dev;
}
