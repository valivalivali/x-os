#include "kernel/hal/block/block_dev.h"

bool block_read(block_dev_t *dev, uint64_t lba, uint32_t count, void *buf) {
    if (!dev || !dev->read) return false;
    return dev->read(dev, lba, count, buf);
}

bool block_write(block_dev_t *dev, uint64_t lba, uint32_t count, const void *buf) {
    if (!dev || !dev->write) return false;
    return dev->write(dev, lba, count, buf);
}
