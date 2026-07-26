/* x-os VirtIO-Net Driver
 *
 * Provides packet send/receive via QEMU's virtio-net-pci device.
 * Uses MSI-X interrupts for RX notification instead of timer-tick polling.
 */

#include "kernel/hal/net/virtio_net.h"
#include "kernel/hal/pci/msix.h"
#include "kernel/memory/pmm.h"
#include "kernel/memory/heap.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

/* VirtIO-Net device config (MAC address etc.) */
struct virtio_net_config {
    uint8_t  mac[6];
    uint16_t status;
    uint16_t max_virtqueue_pairs;
    uint16_t mtu;
} __attribute__((packed));

static virtio_pci_dev_t g_netdev;
static virtqueue_t g_rxq;
static virtqueue_t g_txq;
static struct virtio_net_config *g_net_cfg = NULL;
static uint8_t g_mac[6] = {0};
static bool g_net_ready = false;
static msix_cap_t g_net_msix;
static bool g_net_msix_active = false;

/* RX interrupt handler — called from MSI-X stub when the device has
 * filled RX buffers.  Delegates to the BSD layer to process packets. */
static void virtio_net_rx_isr(void *ctx) {
    (void)ctx;
    extern void vioif_rx_poll(void);
    vioif_rx_poll();
}

/* RX buffer pool — pre-allocated buffers for receiving packets */
#define RX_BUF_COUNT  16
static uint8_t *g_rx_bufs[RX_BUF_COUNT];
static uint16_t g_rx_desc_idx[RX_BUF_COUNT];

/* TX buffer — single buffer for sending */
static uint8_t *g_tx_buf = NULL;

/* Allocate a receive buffer and submit it to the RX queue */
static bool rx_submit(int slot) {
    if (!g_rx_bufs[slot]) {
        g_rx_bufs[slot] = (uint8_t *)kmalloc(VIRTIO_NET_BUF_SIZE);
        if (!g_rx_bufs[slot]) return false;
        memset(g_rx_bufs[slot], 0, VIRTIO_NET_BUF_SIZE);
    }

    /* Submit as a writable descriptor */
    void *bufs[1] = { g_rx_bufs[slot] };
    uint32_t lens[1] = { VIRTIO_NET_BUF_SIZE };
    uint16_t flags[1] = { VRING_DESC_F_WRITE };
    uint16_t desc_idx;

    if (!virtqueue_add_buf(&g_rxq, &desc_idx, bufs, lens, flags, 1))
        return false;

    g_rx_desc_idx[slot] = desc_idx;
    return true;
}

bool virtio_net_init(void) {
    if (g_net_ready) return true;

    /* Try non-transitional (0x1000) first, then transitional (0x1041) */
    if (!virtio_pci_probe(&g_netdev, VIRTIO_NET_PCI_DEVICE)) {
        if (!virtio_pci_probe(&g_netdev, VIRTIO_NET_PCI_DEVICE_LEGACY)) {
            kputs("[virtio-net] no device found\n");
            return false;
        }
    }
    kprintf("[virtio-net] found at %x:%x.0\n", g_netdev.pci.bus, g_netdev.pci.dev);

    pci_enable_bus_master(&g_netdev.pci);
    virtio_pci_set_status(&g_netdev, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_pci_set_status(&g_netdev, VIRTIO_STATUS_DRIVER);

    /* Negotiate features — we want MAC support and VERSION_1 */
    uint64_t features = virtio_pci_get_features(&g_netdev);
    uint64_t want = VIRTIO_F_VERSION_1;
    if (features & VIRTIO_NET_F_MAC) {
        want |= VIRTIO_NET_F_MAC;
    }
    features &= want;
    virtio_pci_set_features(&g_netdev, features);
    virtio_pci_set_status(&g_netdev, VIRTIO_STATUS_FEATURES_OK);

    /* Read device config (MAC address) */
    if (g_netdev.device_cfg_ptr) {
        g_net_cfg = (struct virtio_net_config *)phys_to_virt(g_netdev.device_cfg_ptr);
        memcpy(g_mac, g_net_cfg->mac, 6);
        kprintf("[virtio-net] MAC %x:%x:%x:%x:%x:%x\n",
                g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
    } else {
        kputs("[virtio-net] no device config, using default MAC\n");
        g_mac[0] = 0x52; g_mac[1] = 0x54; g_mac[2] = 0x00;
        g_mac[3] = 0x12; g_mac[4] = 0x34; g_mac[5] = 0x56;
    }

    /* Set up queues: 0=RX, 1=TX */
    if (!virtio_pci_setup_queue(&g_netdev, &g_rxq, 0)) {
        kputs("[virtio-net] RX queue setup failed\n");
        return false;
    }
    if (!virtio_pci_setup_queue(&g_netdev, &g_txq, 1)) {
        kputs("[virtio-net] TX queue setup failed\n");
        return false;
    }

    /* Set up MSI-X for RX interrupts (vector 0 = RX queue). */
    if (msix_parse(&g_netdev.pci, &g_net_msix)) {
        msix_enable(&g_netdev.pci, &g_net_msix);
        uint8_t rx_vec = msix_alloc_vector(virtio_net_rx_isr, NULL);
        if (rx_vec && g_net_msix.table) {
            /* Route RX interrupt to BSP (CPU 0). */
            msix_program_entry(&g_net_msix, 0, rx_vec, 0);
            /* Tell the virtio common config which MSI-X vector the RX
             * queue (queue 0) uses.  Writing the queue index first,
             * then the vector. */
            g_netdev.common->queue_select = 0;
            g_netdev.common->queue_msix_vector = rx_vec - 0x40;
            g_net_msix_active = true;
            kprintf("[virtio-net] MSI-X enabled (RX vector=0x%x)\n", rx_vec);
        }
    } else {
        kprintf("[virtio-net] no MSI-X, falling back to timer polling\n");
    }

    virtio_pci_set_status(&g_netdev, VIRTIO_STATUS_DRIVER_OK);

    /* Allocate TX buffer */
    g_tx_buf = (uint8_t *)kmalloc(VIRTIO_NET_BUF_SIZE);
    if (!g_tx_buf) {
        kputs("[virtio-net] TX buffer alloc failed\n");
        return false;
    }

    /* Submit initial RX buffers */
    for (int i = 0; i < RX_BUF_COUNT; i++) {
        g_rx_bufs[i] = NULL;
        if (!rx_submit(i)) {
            kprintf("[virtio-net] failed to submit RX buf %d\n", i);
            break;
        }
    }
    virtio_pci_notify_queue(&g_netdev, &g_rxq);

    g_net_ready = true;
    kputs("[virtio-net] initialized\n");
    return true;
}

int virtio_net_send(const void *data, int len) {
    if (!g_net_ready || !g_tx_buf) return -1;
    if (len > VIRTIO_NET_MTU) len = VIRTIO_NET_MTU;

    /* Build TX buffer: header + data */
    memset(g_tx_buf, 0, VIRTIO_NET_HDR_SIZE);
    memcpy(g_tx_buf + VIRTIO_NET_HDR_SIZE, data, len);

    int total = VIRTIO_NET_HDR_SIZE + len;

    void *bufs[1] = { g_tx_buf };
    uint32_t lens[1] = { total };
    uint16_t flags[1] = { 0 };
    uint16_t desc_idx;

    if (!virtqueue_add_buf(&g_txq, &desc_idx, bufs, lens, flags, 1)) {
        kprintf("[virtio-net] TX: virtqueue_add_buf failed\n");
        return -1;
    }

    virtio_pci_notify_queue(&g_netdev, &g_txq);

    /* Wait for TX completion */
    if (!virtio_pci_wait_for_queue(&g_netdev, &g_txq, 5000000)) {
        kprintf("[virtio-net] TX: wait_for_queue timeout (len=%d)\n", len);
        return -1;
    }

    uint16_t used_idx;
    uint32_t used_len;
    virtqueue_get_used(&g_txq, &used_idx, &used_len);

    return len;
}

int virtio_net_recv(void *buf, int maxlen) {
    if (!g_net_ready) return -1;

    /* Check if any RX buffers have been filled */
    if (g_rxq.last_used == g_rxq.used->idx) {
        return -1;  /* no packets */
    }

    /* Get the used descriptor */
    uint16_t used_desc;
    uint32_t used_len;
    if (!virtqueue_get_used(&g_rxq, &used_desc, &used_len))
        return -1;

    /* Find which slot corresponds to this descriptor index */
    int slot = -1;
    for (int i = 0; i < RX_BUF_COUNT; i++) {
        if (g_rx_desc_idx[i] == used_desc) {
            slot = i;
            break;
        }
    }
    if (slot < 0 || !g_rx_bufs[slot]) return -1;

    /* The used_len includes the virtio_net_hdr */
    int pkt_len = (int)used_len - VIRTIO_NET_HDR_SIZE;
    if (pkt_len < 0) pkt_len = 0;
    if (pkt_len > maxlen) pkt_len = maxlen;

    memcpy(buf, g_rx_bufs[slot] + VIRTIO_NET_HDR_SIZE, pkt_len);

    /* Resubmit the RX buffer */
    memset(g_rx_bufs[slot], 0, VIRTIO_NET_BUF_SIZE);
    rx_submit(slot);
    virtio_pci_notify_queue(&g_netdev, &g_rxq);

    return pkt_len;
}

bool virtio_net_is_ready(void) {
    return g_net_ready;
}

bool virtio_net_msix_is_active(void) {
    return g_net_msix_active;
}

void virtio_net_get_mac(uint8_t mac[6]) {
    memcpy(mac, g_mac, 6);
}
