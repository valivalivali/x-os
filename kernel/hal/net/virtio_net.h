/* x-os VirtIO-Net Driver
 *
 * Provides packet send/receive via QEMU's virtio-net-pci device.
 * Uses the existing virtio PCI transport layer for queue setup.
 *
 * VirtIO-Net has two queues:
 *   Queue 0: receive (we provide buffers, device fills them)
 *   Queue 1: transmit (we send buffers, device sends them on wire)
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "kernel/hal/virtio/virtio_pci.h"

/* VirtIO-Net PCI device IDs */
#define VIRTIO_NET_PCI_DEVICE     0x1000  /* non-transitional */
#define VIRTIO_NET_PCI_DEVICE_LEGACY 0x1041  /* transitional */

/* VirtIO-Net feature bits */
#define VIRTIO_NET_F_MAC      (1ULL << 5)

/* VirtIO-Net header (precedes each packet in RX and TX) */
struct virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;  /* RX only */
} __attribute__((packed));

#define VIRTIO_NET_HDR_SIZE  sizeof(struct virtio_net_hdr)

/* Maximum packet size */
#define VIRTIO_NET_MTU  1514
#define VIRTIO_NET_BUF_SIZE (VIRTIO_NET_HDR_SIZE + VIRTIO_NET_MTU)

/* API */
bool virtio_net_init(void);
int virtio_net_send(const void *data, int len);
int virtio_net_recv(void *buf, int maxlen);
bool virtio_net_is_ready(void);
void virtio_net_get_mac(uint8_t mac[6]);
bool virtio_net_msix_is_active(void);  /* true if RX uses MSI-X interrupts */
