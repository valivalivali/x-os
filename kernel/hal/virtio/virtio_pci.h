#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "kernel/hal/pci/pci.h"

#define VIRTIO_PCI_VENDOR         0x1AF4
#define VIRTIO_GPU_PCI_DEVICE     0x1050

#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG    3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

#define VIRTIO_F_VERSION_1        (1ULL << 32)
#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8

struct virtio_pci_common_cfg {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t  device_status;
    uint8_t  config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_avail;
    uint64_t queue_used;
} __attribute__((packed));

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[];
} __attribute__((packed));

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

typedef struct virtqueue {
    uint16_t size;
    uint16_t free_head;
    uint16_t last_used;
    uint16_t avail_idx;
    uint16_t queue_idx;
    struct vring_desc *desc;
    struct vring_avail *avail;
    struct vring_used *used;
    uint16_t *notify;
} virtqueue_t;

typedef struct virtio_pci_dev {
    pci_dev_t pci;
    volatile struct virtio_pci_common_cfg *common;
    volatile uint32_t *notify_base;
    volatile uint8_t  *isr;
    uint32_t notify_off_multiplier;
    uint64_t device_cfg_ptr;
} virtio_pci_dev_t;

bool virtio_pci_probe(virtio_pci_dev_t *vdev, uint16_t device_id);
void virtio_pci_set_status(virtio_pci_dev_t *vdev, uint8_t status);
uint8_t virtio_pci_get_status(virtio_pci_dev_t *vdev);
uint64_t virtio_pci_get_features(virtio_pci_dev_t *vdev);
void virtio_pci_set_features(virtio_pci_dev_t *vdev, uint64_t features);
bool virtio_pci_setup_queue(virtio_pci_dev_t *vdev, virtqueue_t *vq, uint16_t idx);
void virtio_pci_notify_queue(virtio_pci_dev_t *vdev, virtqueue_t *vq);
bool virtqueue_add_buf(virtqueue_t *vq, uint16_t *desc_idx_out,
                       void *buf[], uint32_t len[], uint16_t flags[],
                       uint16_t ndesc);
bool virtqueue_get_used(virtqueue_t *vq, uint16_t *idx_out, uint32_t *len_out);
bool virtio_pci_wait_for_queue(virtio_pci_dev_t *vdev, virtqueue_t *vq, uint32_t max_iters);
