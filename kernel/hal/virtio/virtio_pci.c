#include "kernel/hal/virtio/virtio_pci.h"
#include "kernel/memory/pmm.h"
#include "kernel/memory/vmm.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/arch/x86_64/io.h"

#define PCI_CAP_VNDR    0x09
#define PCI_CAP_NEXT    0x01

static uint8_t pci_readb(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off) {
    uint32_t v = pci_read(bus, dev, func, off & ~3u);
    return (uint8_t)(v >> ((off & 3) * 8));
}

bool virtio_pci_probe(virtio_pci_dev_t *vdev, uint16_t device_id) {
    memset(vdev, 0, sizeof(*vdev));
    /* VirtIO GPU: vendor 0x1AF4, device 0x1050 */
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t d = 0; d < 32; d++) {
            uint32_t hdr0 = pci_read((uint8_t)bus, d, 0, 0);
            uint16_t vendor = hdr0 & 0xFFFF;
            uint16_t device = (hdr0 >> 16) & 0xFFFF;
            if (vendor != VIRTIO_PCI_VENDOR) continue;
            if (device != device_id) continue;

            vdev->pci.bus = (uint8_t)bus;
            vdev->pci.dev = d;
            vdev->pci.func = 0;
            vdev->pci.vendor = vendor;
            vdev->pci.device = device;

            /* Populate all BARs so MSI-X can locate its table. */
            for (int i = 0; i < 6; i++) {
                vdev->pci.bar[i] = pci_read_bar(&vdev->pci, i);
                vdev->pci.bar_valid[i] = (vdev->pci.bar[i] != 0);
            }

            /* Walk PCI capabilities */
            uint8_t status = pci_readb((uint8_t)bus, d, 0, PCI_STATUS);
            if (!(status & 0x10)) return false; /* no capabilities */
            uint8_t cap_ptr = pci_readb((uint8_t)bus, d, 0, 0x34);

            while (cap_ptr) {
                uint8_t cap_id = pci_readb((uint8_t)bus, d, 0, cap_ptr);
                if (cap_id == PCI_CAP_VNDR) {
                    uint8_t cap_type = pci_readb((uint8_t)bus, d, 0, cap_ptr + 3);
                    uint8_t bar = pci_readb((uint8_t)bus, d, 0, cap_ptr + 4);
                    uint32_t offset = pci_read((uint8_t)bus, d, 0, cap_ptr + 8);
                    uint64_t bar_addr = pci_read_bar(&vdev->pci, bar);
                    uint64_t ptr = bar_addr + offset;

                    if (cap_type == VIRTIO_PCI_CAP_COMMON_CFG) {
                        vdev->common = (volatile struct virtio_pci_common_cfg *)phys_to_virt(ptr);
                    } else if (cap_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
                        vdev->notify_base = (volatile uint32_t *)phys_to_virt(ptr);
                        vdev->notify_off_multiplier = pci_read((uint8_t)bus, d, 0, cap_ptr + 16) & 0xFFFF;
                        if (vdev->notify_off_multiplier == 0) vdev->notify_off_multiplier = 1;
                    } else if (cap_type == VIRTIO_PCI_CAP_ISR_CFG) {
                        vdev->isr = (volatile uint8_t *)phys_to_virt(ptr);
                    } else if (cap_type == VIRTIO_PCI_CAP_DEVICE_CFG) {
                        vdev->device_cfg_ptr = ptr;
                    }
                }
                cap_ptr = pci_readb((uint8_t)bus, d, 0, cap_ptr + 1);
            }
            if (vdev->common) return true;
            memset(vdev, 0, sizeof(*vdev));
        }
    }
    return false;
}

void virtio_pci_set_status(virtio_pci_dev_t *vdev, uint8_t status) {
    vdev->common->device_status = status;
}

uint8_t virtio_pci_get_status(virtio_pci_dev_t *vdev) {
    return vdev->common->device_status;
}

uint64_t virtio_pci_get_features(virtio_pci_dev_t *vdev) {
    vdev->common->device_feature_select = 0;
    uint32_t lo = vdev->common->device_feature;
    vdev->common->device_feature_select = 1;
    uint32_t hi = vdev->common->device_feature;
    return ((uint64_t)hi << 32) | lo;
}

void virtio_pci_set_features(virtio_pci_dev_t *vdev, uint64_t features) {
    vdev->common->driver_feature_select = 0;
    vdev->common->driver_feature = (uint32_t)features;
    vdev->common->driver_feature_select = 1;
    vdev->common->driver_feature = (uint32_t)(features >> 32);
}

bool virtio_pci_setup_queue(virtio_pci_dev_t *vdev, virtqueue_t *vq, uint16_t idx) {
    vdev->common->queue_select = idx;
    uint16_t qsize = vdev->common->queue_size;
    if (qsize == 0) return false;
    vq->size = qsize;

    /* Descriptor table + avail ring + used ring must be contiguous.
     * Compute total size. */
    size_t desc_sz = sizeof(struct vring_desc) * qsize;
    size_t avail_sz = sizeof(uint16_t) * (3 + qsize); /* flags + idx + ring */
    size_t used_sz = sizeof(uint16_t) * 3 + sizeof(struct vring_used_elem) * qsize;
    size_t total = desc_sz + avail_sz + used_sz;
    /* Align used ring to page boundary for simplicity */
    total = (total + PAGE_SIZE - 1) & PAGE_MASK;

    uint64_t phys = pmm_alloc_contig(total / PAGE_SIZE);
    if (!phys) return false;
    uint8_t *virt = (uint8_t *)phys_to_virt(phys);
    memset(virt, 0, total);

    vq->desc = (struct vring_desc *)virt;
    vq->avail = (struct vring_avail *)(virt + desc_sz);
    vq->used = (struct vring_used *)(virt + desc_sz + avail_sz);

    vdev->common->queue_desc = phys;
    vdev->common->queue_avail = phys + desc_sz;
    vdev->common->queue_used = phys + desc_sz + avail_sz;
    vdev->common->queue_enable = 1;

    vq->queue_idx = idx;
    vq->free_head = 0;
    vq->last_used = 0;
    vq->avail_idx = 0;

    /* Set up notify pointer */
    uint16_t notify_off = vdev->common->queue_notify_off;
    vq->notify = (uint16_t *)((uint8_t *)vdev->notify_base + notify_off * vdev->notify_off_multiplier);
    return true;
}

void virtio_pci_notify_queue(virtio_pci_dev_t *vdev, virtqueue_t *vq) {
    (void)vdev;
    *vq->notify = vq->queue_idx;
    __asm__ volatile("sfence" ::: "memory");
}

bool virtqueue_add_buf(virtqueue_t *vq, uint16_t *desc_idx_out,
                       void *buf[], uint32_t len[], uint16_t flags[],
                       uint16_t ndesc) {
    if (vq->free_head + ndesc > vq->size) return false;
    uint16_t head = vq->free_head;
    for (uint16_t i = 0; i < ndesc; i++) {
        uint16_t idx = vq->free_head++;
        vq->desc[idx].addr = virt_to_phys(buf[i]);
        vq->desc[idx].len  = len[i];
        vq->desc[idx].flags = flags[i];
        if (i + 1 < ndesc) {
            vq->desc[idx].flags |= VRING_DESC_F_NEXT;
            vq->desc[idx].next = idx + 1;
        } else {
            vq->desc[idx].next = 0;
        }
    }
    vq->avail->ring[vq->avail->idx % vq->size] = head;
    __asm__ volatile("sfence" ::: "memory");
    vq->avail->idx++;
    *desc_idx_out = head;
    return true;
}

bool virtqueue_get_used(virtqueue_t *vq, uint16_t *idx_out, uint32_t *len_out) {
    if (vq->last_used == vq->used->idx) return false;
    struct vring_used_elem *elem = &vq->used->ring[vq->last_used % vq->size];
    *idx_out = (uint16_t)elem->id;
    *len_out = elem->len;
    vq->last_used++;
    vq->free_head = 0; /* reuse all descriptors for single-inflight */
    return true;
}

bool virtio_pci_wait_for_queue(virtio_pci_dev_t *vdev, virtqueue_t *vq, uint32_t max_iters) {
    uint32_t iters = 0;
    while (vq->last_used == vq->used->idx) {
        __asm__ volatile("pause");
        if (vdev->isr) (void)*vdev->isr;
        if (++iters > max_iters) return false;
    }
    return true;
}
