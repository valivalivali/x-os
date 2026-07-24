/* NVMe driver — polling-based, single namespace, single I/O queue.
 * Implements the generic block_dev_t interface so the filesystem
 * layer does not care whether the backend is NVMe or RAM disk. */

#include "kernel/hal/block/nvme.h"
#include "kernel/hal/pci/pci.h"
#include "kernel/memory/pmm.h"
#include "kernel/memory/vmm.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"
#include "kernel/hal/apic/spinlock.h"

#define NVME_CLASS    PCI_CLASS_MASS_STORAGE
#define NVME_SUBCLASS PCI_SUBCLASS_NVME

/* Controller register offsets */
#define NVME_CAP   0x00   /* 64-bit */
#define NVME_VS    0x08   /* 32-bit */
#define NVME_CC    0x14   /* 32-bit */
#define NVME_CSTS  0x1C   /* 32-bit */
#define NVME_AQA   0x24   /* 32-bit */
#define NVME_ASQ   0x28   /* 64-bit */
#define NVME_ACQ   0x30   /* 64-bit */
#define NVME_DBS   0x1000 /* doorbells */

/* CC bits */
#define CC_EN      (1U << 0)
#define CC_CSS_NVM (0U << 4)
#define CC_MPS_4K  (0U << 7)
#define CC_AMS_RR  (0U << 11)

/* CSTS bits */
#define CSTS_RDY   (1U << 0)
#define CSTS_CFS   (1U << 1)

/* Admin opcodes */
#define ADMIN_CREATE_IO_CQ 0x05
#define ADMIN_CREATE_IO_SQ 0x01
#define ADMIN_IDENTIFY     0x06

/* I/O opcodes */
#define IO_OPCODE_WRITE 0x01
#define IO_OPCODE_READ  0x02

/* Identify CNS */
#define CNS_NAMESPACE 0
#define CNS_CONTROLLER 1

/* Page size */
#define NVME_PAGE_SIZE 4096

/* Where we map PCI BARs in kernel virtual space */
#define NVME_MMIO_VADDR_BASE 0xFFFF900000000000ULL
static uint64_t g_next_mmio_vaddr = NVME_MMIO_VADDR_BASE;

/* -------------------------------------------------------------------------- */
/* Register access helpers */

static inline uint32_t reg_read32(volatile uint32_t *bar, uint32_t off) {
    return ((volatile uint32_t *)((uint8_t *)bar + off))[0];
}

static inline uint64_t reg_read64(volatile uint32_t *bar, uint32_t off) {
    return ((volatile uint64_t *)((uint8_t *)bar + off))[0];
}

static inline void reg_write32(volatile uint32_t *bar, uint32_t off, uint32_t val) {
    ((volatile uint32_t *)((uint8_t *)bar + off))[0] = val;
}

static inline void reg_write64(volatile uint32_t *bar, uint32_t off, uint64_t val) {
    ((volatile uint64_t *)((uint8_t *)bar + off))[0] = val;
}

/* -------------------------------------------------------------------------- */
/* NVMe structures */

typedef struct {
    uint32_t dw0;   /* opcode, flags, cid */
    uint32_t nsid;
    uint64_t rsvd;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed)) nvme_cmd_t;

typedef struct {
    uint32_t dw0;
    uint32_t dw1;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t phase_status; /* bit 0 = phase, bits 1-15 = status >> 1 */
} __attribute__((packed)) nvme_cqe_t;

typedef struct {
    block_dev_t dev;    /* must stay first */
    volatile uint32_t *bar;
    uint64_t bar_phys;
    uint32_t doorbell_stride;
    uint32_t timeout;      /* in 500ms units */
    uint32_t max_q_entries;

    /* Admin queue */
    uint64_t admin_sq_phys;
    volatile nvme_cmd_t *admin_sq;
    uint16_t admin_sq_size;
    uint16_t admin_sq_tail;
    volatile uint32_t *admin_sq_doorbell;

    uint64_t admin_cq_phys;
    volatile nvme_cqe_t *admin_cq;
    uint16_t admin_cq_size;
    uint16_t admin_cq_head;
    uint8_t  admin_cq_phase;
    volatile uint32_t *admin_cq_doorbell;

    /* I/O queue (qid = 1) */
    uint64_t io_sq_phys;
    volatile nvme_cmd_t *io_sq;
    uint16_t io_sq_size;
    uint16_t io_sq_tail;
    volatile uint32_t *io_sq_doorbell;

    uint64_t io_cq_phys;
    volatile nvme_cqe_t *io_cq;
    uint16_t io_cq_size;
    uint16_t io_cq_head;
    uint8_t  io_cq_phase;
    volatile uint32_t *io_cq_doorbell;

    uint32_t nsid;
    uint64_t ns_size;      /* namespace capacity in LBAs */
    uint32_t lba_shift;    /* log2(lba_size) */

    uint16_t next_cid;
    spinlock_t io_lock;   /* protects io_sq_tail, next_cid, completion polling */
} nvme_ctrl_t;

/* -------------------------------------------------------------------------- */
/* Static controller */

static nvme_ctrl_t g_nvme;

/* -------------------------------------------------------------------------- */
/* Utility */

static void mb(void) { __asm__ volatile("" ::: "memory"); }

static uint16_t alloc_cid(nvme_ctrl_t *ctrl) {
    uint16_t cid = ctrl->next_cid;
    ctrl->next_cid = (cid + 1) & 0xFFFF;
    if (ctrl->next_cid == 0) ctrl->next_cid = 1;
    return cid;
}

/* Map 'page_count' physical pages starting at 'phys' into kernel VA space. */
static uint8_t *map_mmio(uint64_t phys, size_t page_count) {
    uint64_t *kpml4 = vmm_kernel_pml4();
    uint64_t vaddr = g_next_mmio_vaddr;
    for (size_t i = 0; i < page_count; i++) {
        if (!vmm_map_page(kpml4, vaddr + i * PAGE_SIZE,
                          phys + i * PAGE_SIZE,
                          VMM_RW | VMM_P | VMM_NX | VMM_CD)) {
            kputs("[nvme] vmm_map_page failed\n");
            return NULL;
        }
    }
    g_next_mmio_vaddr += page_count * PAGE_SIZE;
    return (uint8_t *)vaddr;
}

/* -------------------------------------------------------------------------- */
/* Controller reset & enable */

static bool nvme_reset(nvme_ctrl_t *ctrl) {
    volatile uint32_t *bar = ctrl->bar;

    /* Disable controller */
    uint32_t cc = reg_read32(bar, NVME_CC);
    if (cc & CC_EN) {
        reg_write32(bar, NVME_CC, cc & ~CC_EN);
        mb();
        uint32_t timeout = ctrl->timeout ? ctrl->timeout : 1;
        for (uint32_t i = 0; i < timeout * 1000; i++) {
            if (!(reg_read32(bar, NVME_CSTS) & CSTS_RDY)) break;
        }
        if (reg_read32(bar, NVME_CSTS) & CSTS_RDY) {
            kputs("[nvme] timeout waiting for disable\n");
            return false;
        }
    }

    /* Configure admin queue */
    reg_write32(bar, NVME_AQA,
                ((ctrl->admin_cq_size - 1) << 16) |
                (ctrl->admin_sq_size - 1));
    reg_write64(bar, NVME_ASQ, ctrl->admin_sq_phys);
    reg_write64(bar, NVME_ACQ, ctrl->admin_cq_phys);
    mb();

    /* Set CC and enable */
    reg_write32(bar, NVME_CC,
                CC_EN | CC_CSS_NVM | CC_MPS_4K | CC_AMS_RR |
                (4U << 20) | (6U << 16));
    mb();

    /* Wait for ready */
    uint32_t timeout = ctrl->timeout ? ctrl->timeout : 1;
    for (uint32_t i = 0; i < timeout * 1000; i++) {
        uint32_t csts = reg_read32(bar, NVME_CSTS);
        if (csts & CSTS_RDY) return true;
        if (csts & CSTS_CFS) {
            kputs("[nvme] controller fatal status\n");
            return false;
        }
    }
    kputs("[nvme] timeout waiting for ready\n");
    return false;
}

/* -------------------------------------------------------------------------- */
/* Queue submission / polling */

static bool submit_admin(nvme_ctrl_t *ctrl, nvme_cmd_t *cmd, uint32_t *result) {
    uint16_t tail = ctrl->admin_sq_tail;
    ctrl->admin_sq[tail] = *cmd;
    mb();
    ctrl->admin_sq_tail = (tail + 1) % ctrl->admin_sq_size;
    *ctrl->admin_sq_doorbell = ctrl->admin_sq_tail;
    __asm__ volatile("mfence" ::: "memory");

    uint16_t my_cid = (cmd->dw0 >> 16) & 0xFFFF;

    /* Poll completion queue */
    for (uint32_t spins = 0; spins < 10000000; spins++) {
        volatile nvme_cqe_t *cqe = &ctrl->admin_cq[ctrl->admin_cq_head];
        uint16_t phase = cqe->phase_status & 1;
        if (cqe->cid == my_cid && phase == ctrl->admin_cq_phase) {
            uint16_t status = (cqe->phase_status >> 1) & 0x7FFF;
            if (status != 0) {
                kprintf("[nvme] admin cmd %u failed status=%x ps=%x\n",
                        cmd->dw0 & 0xFF, status, cqe->phase_status);
                /* advance anyway */
                ctrl->admin_cq_head =
                    (ctrl->admin_cq_head + 1) % ctrl->admin_cq_size;
                if (ctrl->admin_cq_head == 0)
                    ctrl->admin_cq_phase ^= 1;
                *ctrl->admin_cq_doorbell = ctrl->admin_cq_head;
                return false;
            }
            if (result) *result = cqe->dw0;
            ctrl->admin_cq_head =
                (ctrl->admin_cq_head + 1) % ctrl->admin_cq_size;
            if (ctrl->admin_cq_head == 0)
                ctrl->admin_cq_phase ^= 1;
            *ctrl->admin_cq_doorbell = ctrl->admin_cq_head;
            mb();
            return true;
        }
    }
    kputs("[nvme] admin cmd timeout (poll)\n");
    return false;
}

static bool submit_io(nvme_ctrl_t *ctrl, nvme_cmd_t *cmd, uint32_t *result) {
    uint64_t rflags = spinlock_acquire_irqsave(&ctrl->io_lock);
    uint16_t tail = ctrl->io_sq_tail;
    ctrl->io_sq[tail] = *cmd;
    mb();
    ctrl->io_sq_tail = (tail + 1) % ctrl->io_sq_size;
    *ctrl->io_sq_doorbell = ctrl->io_sq_tail;
    __asm__ volatile("mfence" ::: "memory");

    uint16_t my_cid = (cmd->dw0 >> 16) & 0xFFFF;

    for (uint32_t spins = 0; spins < 100000000; spins++) {
        volatile nvme_cqe_t *cqe = &ctrl->io_cq[ctrl->io_cq_head];
        uint16_t phase = cqe->phase_status & 1;
        /* Some controllers (e.g. QEMU) don't toggle phase on wrap;
         * accept either the expected phase or the opposite one.
         * Only accept completions with matching CID to avoid stale zeros. */
        if (cqe->cid == my_cid &&
            (phase == ctrl->io_cq_phase || phase == (ctrl->io_cq_phase ^ 1))) {
            uint16_t status = (cqe->phase_status >> 1) & 0x7FFF;
            if (status != 0) {
                kprintf("[nvme] io cmd %u failed status=%x\n",
                        cmd->dw0 & 0xFF, status);
                ctrl->io_cq_head =
                    (ctrl->io_cq_head + 1) % ctrl->io_cq_size;
                if (ctrl->io_cq_head == 0)
                    ctrl->io_cq_phase ^= 1;
                *ctrl->io_cq_doorbell = ctrl->io_cq_head;
                spinlock_release_irqrestore(&ctrl->io_lock, rflags);
                return false;
            }
            if (result) *result = cqe->dw0;
            ctrl->io_cq_head =
                (ctrl->io_cq_head + 1) % ctrl->io_cq_size;
            if (ctrl->io_cq_head == 0)
                ctrl->io_cq_phase ^= 1;
            *ctrl->io_cq_doorbell = ctrl->io_cq_head;
            mb();
            spinlock_release_irqrestore(&ctrl->io_lock, rflags);
            return true;
        }
    }
    volatile nvme_cqe_t *cqe = &ctrl->io_cq[ctrl->io_cq_head];
    kprintf("[nvme] io cmd %u timeout, cq[%u] dw0=%x ps=%x\n",
            cmd->dw0 & 0xFF, ctrl->io_cq_head,
            cqe->dw0, cqe->phase_status);
    spinlock_release_irqrestore(&ctrl->io_lock, rflags);
    return false;
}

/* -------------------------------------------------------------------------- */
/* Admin commands */

static bool admin_identify(nvme_ctrl_t *ctrl, uint32_t nsid, uint32_t cns,
                           void *buf, size_t len) {
    memset(buf, 0, len);
    uint64_t buf_phys = virt_to_phys(buf);
    nvme_cmd_t cmd = {0};
    cmd.dw0     = ADMIN_IDENTIFY | ((uint32_t)alloc_cid(ctrl) << 16);
    cmd.nsid    = nsid;
    cmd.prp1    = buf_phys;
    cmd.cdw10   = cns;
    return submit_admin(ctrl, &cmd, NULL);
}

static bool admin_set_features_queues(nvme_ctrl_t *ctrl) {
    nvme_cmd_t cmd = {0};
    cmd.dw0   = 0x09 | ((uint32_t)alloc_cid(ctrl) << 16); /* Set Features */
    cmd.cdw10 = 0x07; /* Number of Queues */
    cmd.cdw11 = (0 << 16) | 0; /* 1 CQ, 1 SQ */
    return submit_admin(ctrl, &cmd, NULL);
}

static bool admin_create_io_cq(nvme_ctrl_t *ctrl) {
    nvme_cmd_t cmd = {0};
    cmd.dw0   = ADMIN_CREATE_IO_CQ | ((uint32_t)alloc_cid(ctrl) << 16);
    cmd.prp1  = ctrl->io_cq_phys;
    cmd.cdw10 = ((ctrl->io_cq_size - 1) << 16) | 1; /* qid=1 */
    cmd.cdw11 = 1; /* physically contiguous */
    return submit_admin(ctrl, &cmd, NULL);
}

static bool admin_create_io_sq(nvme_ctrl_t *ctrl) {
    nvme_cmd_t cmd = {0};
    cmd.dw0   = ADMIN_CREATE_IO_SQ | ((uint32_t)alloc_cid(ctrl) << 16);
    cmd.prp1  = ctrl->io_sq_phys;
    cmd.cdw10 = ((ctrl->io_sq_size - 1) << 16) | 1; /* qid=1 */
    cmd.cdw11 = (1 << 16) | 1; /* cqid=1, physically contiguous */
    return submit_admin(ctrl, &cmd, NULL);
}

/* -------------------------------------------------------------------------- */
/* I/O commands */

static bool io_read(nvme_ctrl_t *ctrl, uint64_t lba, uint32_t count,
                    uint64_t buf_phys) {
    nvme_cmd_t cmd = {0};
    cmd.dw0   = IO_OPCODE_READ | ((uint32_t)alloc_cid(ctrl) << 16);
    cmd.nsid  = ctrl->nsid;
    cmd.prp1  = buf_phys;
    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = count - 1;
    return submit_io(ctrl, &cmd, NULL);
}

static bool io_write(nvme_ctrl_t *ctrl, uint64_t lba, uint32_t count,
                     uint64_t buf_phys) {
    nvme_cmd_t cmd = {0};
    cmd.dw0   = IO_OPCODE_WRITE | ((uint32_t)alloc_cid(ctrl) << 16);
    cmd.nsid  = ctrl->nsid;
    cmd.prp1  = buf_phys;
    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = count - 1;
    return submit_io(ctrl, &cmd, NULL);
}

/* -------------------------------------------------------------------------- */
/* block_dev_t wrappers — use DMA bounce buffers because caller buf may be
 * on the kernel stack, which is NOT in the HHDM identity map. */

static bool nvme_read_block(block_dev_t *dev, uint64_t lba,
                            uint32_t count, void *buf) {
    nvme_ctrl_t *ctrl = (nvme_ctrl_t *)dev;
    uint8_t *dst = (uint8_t *)buf;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t bounce = pmm_alloc_frame();
        if (!bounce) return false;

        uint64_t nvme_lba  = (lba + i) << (12 - ctrl->lba_shift);
        uint32_t sectors   = 1 << (12 - ctrl->lba_shift);
        bool ok = io_read(ctrl, nvme_lba, sectors, bounce);
        if (ok) memcpy(dst + i * PAGE_SIZE, phys_to_virt(bounce), PAGE_SIZE);

        pmm_free_frame(bounce);
        if (!ok) return false;
    }
    return true;
}

static bool nvme_write_block(block_dev_t *dev, uint64_t lba,
                             uint32_t count, const void *buf) {
    nvme_ctrl_t *ctrl = (nvme_ctrl_t *)dev;
    const uint8_t *src = (const uint8_t *)buf;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t bounce = pmm_alloc_frame();
        if (!bounce) return false;

        memcpy(phys_to_virt(bounce), src + i * PAGE_SIZE, PAGE_SIZE);
        uint64_t nvme_lba = (lba + i) << (12 - ctrl->lba_shift);
        uint32_t sectors  = 1 << (12 - ctrl->lba_shift);
        bool ok = io_write(ctrl, nvme_lba, sectors, bounce);

        pmm_free_frame(bounce);
        if (!ok) return false;
    }
    return true;
}

/* -------------------------------------------------------------------------- */
/* Main probe/init */

block_dev_t *nvme_probe(void) {
    pci_dev_t pci;
    if (!pci_scan(&pci, NVME_CLASS, NVME_SUBCLASS)) {
        kputs("[nvme] no NVMe controller found\n");
        return NULL;
    }
    kprintf("[nvme] found %x:%x at %u:%u.%u\n",
            pci.vendor, pci.device, pci.bus, pci.dev, pci.func);

    if (!pci.bar_valid[0]) {
        kputs("[nvme] BAR0 not present\n");
        return NULL;
    }

    pci_enable_bus_master(&pci);

    memset(&g_nvme, 0, sizeof(g_nvme));
    nvme_ctrl_t *ctrl = &g_nvme;

    /* Map BAR0 */
    ctrl->bar_phys = pci.bar[0];
    uint8_t *mapped = map_mmio(ctrl->bar_phys, 8); /* 8 pages = 32 KiB */
    if (!mapped) return NULL;
    ctrl->bar = (volatile uint32_t *)mapped;

    /* Read CAP */
    uint64_t cap = reg_read64(ctrl->bar, NVME_CAP);
    ctrl->doorbell_stride = (uint32_t)((cap >> 32) & 0xF);
    ctrl->max_q_entries    = (uint32_t)((cap >> 0)  & 0xFFFF) + 1;
    ctrl->timeout          = (uint32_t)((cap >> 24) & 0xFF);
    ctrl->next_cid         = 1;
    /* io_lock is already zero-initialized (SPINLOCK_INIT = {0,0}) */

    uint32_t dstride = 4U << ctrl->doorbell_stride;
    ctrl->admin_sq_doorbell = (volatile uint32_t *)((uint8_t *)ctrl->bar + NVME_DBS + 0 * dstride);
    ctrl->admin_cq_doorbell = (volatile uint32_t *)((uint8_t *)ctrl->bar + NVME_DBS + 1 * dstride);
    ctrl->io_sq_doorbell    = (volatile uint32_t *)((uint8_t *)ctrl->bar + NVME_DBS + 2 * dstride);
    ctrl->io_cq_doorbell    = (volatile uint32_t *)((uint8_t *)ctrl->bar + NVME_DBS + 3 * dstride);

    kprintf("[nvme] CAP=%x MQES=%u TO=%u dstride=%u\n",
            (uint32_t)cap, ctrl->max_q_entries, ctrl->timeout, dstride);

    /* Allocate queues (must be physically contiguous) */
    ctrl->admin_sq_size = 32;
    ctrl->admin_cq_size = 32;
    ctrl->io_sq_size    = 64;
    ctrl->io_cq_size    = 64;

    ctrl->admin_sq_phys = pmm_alloc_contig(1);
    ctrl->admin_cq_phys = pmm_alloc_contig(1);
    ctrl->io_sq_phys    = pmm_alloc_contig(1);
    ctrl->io_cq_phys    = pmm_alloc_contig(1);

    if (!ctrl->admin_sq_phys || !ctrl->admin_cq_phys ||
        !ctrl->io_sq_phys    || !ctrl->io_cq_phys) {
        kputs("[nvme] queue alloc failed\n");
        return NULL;
    }

    ctrl->admin_sq = (volatile nvme_cmd_t *)phys_to_virt(ctrl->admin_sq_phys);
    ctrl->admin_cq = (volatile nvme_cqe_t *)phys_to_virt(ctrl->admin_cq_phys);
    ctrl->io_sq    = (volatile nvme_cmd_t *)phys_to_virt(ctrl->io_sq_phys);
    ctrl->io_cq    = (volatile nvme_cqe_t *)phys_to_virt(ctrl->io_cq_phys);

    memset((void *)ctrl->admin_sq, 0, PAGE_SIZE);
    memset((void *)ctrl->admin_cq, 0, PAGE_SIZE);
    memset((void *)ctrl->io_sq,    0, PAGE_SIZE);
    memset((void *)ctrl->io_cq,    0, PAGE_SIZE);

    ctrl->admin_cq_phase = 1;  /* QEMU starts admin CQ with phase=1 */
    ctrl->io_cq_phase    = 0;

    /* Reset & enable controller */
    if (!nvme_reset(ctrl)) {
        kputs("[nvme] controller init failed\n");
        return NULL;
    }
    kputs("[nvme] controller ready\n");

    /* Identify controller (we just want NN, but ignore it for now) */
    uint8_t *identify_buf = (uint8_t *)phys_to_virt(pmm_alloc_contig(1));
    if (!identify_buf) {
        kputs("[nvme] identify buffer alloc failed\n");
        return NULL;
    }
    memset(identify_buf, 0, PAGE_SIZE);

    if (!admin_identify(ctrl, 0, CNS_CONTROLLER, identify_buf, PAGE_SIZE)) {
        kputs("[nvme] identify controller failed\n");
        return NULL;
    }
    uint32_t nn = *(uint32_t *)(identify_buf + 516); /* NN at offset 0x204 */
    kprintf("[nvme] controller has %u namespace(s)\n", nn);

    /* Set number of queues before creating them */
    if (!admin_set_features_queues(ctrl)) {
        kputs("[nvme] set features queues failed\n");
        return NULL;
    }

    /* Create I/O queues */
    if (!admin_create_io_cq(ctrl)) {
        kputs("[nvme] create IO CQ failed\n");
        return NULL;
    }
    if (!admin_create_io_sq(ctrl)) {
        kputs("[nvme] create IO SQ failed\n");
        return NULL;
    }

    /* Identify namespace 1 */
    memset(identify_buf, 0, PAGE_SIZE);
    if (!admin_identify(ctrl, 1, CNS_NAMESPACE, identify_buf, PAGE_SIZE)) {
        kputs("[nvme] identify namespace failed\n");
        return NULL;
    }

    ctrl->nsid     = 1;
    ctrl->ns_size  = *(uint64_t *)(identify_buf + 0x00); /* NSZE */
    uint8_t flbas  = *(uint8_t *)(identify_buf + 0x84);  /* formatted LBA size index */
    uint32_t lbaf  = *(uint32_t *)(identify_buf + 0x130 + (flbas & 0xF) * 4);
    uint8_t lbads  = (lbaf >> 8) & 0xFF;                 /* LBA data size */
    ctrl->lba_shift = lbads ? (lbads + 9) : 9;           /* default 512 bytes */

    kprintf("[nvme] ns1 size=%lu lba_shift=%u (%u bytes)\n",
            ctrl->ns_size, ctrl->lba_shift, 1U << ctrl->lba_shift);

    if (ctrl->ns_size == 0) {
        kputs("[nvme] namespace has zero size\n");
        return NULL;
    }

    /* Free identify buffer — no longer needed */
    pmm_free_frame(virt_to_phys(identify_buf));

    /* Expose block_dev_t */
    ctrl->dev.block_count = ctrl->ns_size >> (12 - ctrl->lba_shift);
    ctrl->dev.read  = nvme_read_block;
    ctrl->dev.write = nvme_write_block;
    ctrl->dev.private = ctrl;

    kprintf("[nvme] online, %lu 4KiB blocks\n", ctrl->dev.block_count);
    return &ctrl->dev;
}
