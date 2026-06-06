#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "kernel/hal/block/block_dev.h"

/* NVMe block device driver.
 * Probes PCI for an NVMe controller, initializes it, and exposes a
 * block_dev_t interface that the filesystem layer can mount. */

block_dev_t *nvme_probe(void);
