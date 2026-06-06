#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Generic block device interface.
 * Any storage driver (RAM disk, NVMe, ATA) implements this.
 * The filesystem layer only talks to block devices. */

typedef struct block_dev {
    uint64_t block_count;   /* total 4KiB blocks */
    bool (*read)(struct block_dev *dev, uint64_t lba,
                 uint32_t count, void *buf);
    bool (*write)(struct block_dev *dev, uint64_t lba,
                  uint32_t count, const void *buf);
    void *private;
} block_dev_t;

bool block_read(block_dev_t *dev, uint64_t lba, uint32_t count, void *buf);
bool block_write(block_dev_t *dev, uint64_t lba, uint32_t count, const void *buf);

/* RAM disk — allocates contiguous physical frames */
block_dev_t *ramdisk_create(void);
block_dev_t *nvme_probe(void);
