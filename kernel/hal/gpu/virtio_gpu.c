#include "kernel/hal/gpu/virtio_gpu.h"
#include "kernel/hal/virtio/virtio_pci.h"
#include "kernel/memory/pmm.h"
#include "kernel/memory/heap.h"
#include "kernel/lib/string.h"
#include "kernel/lib/kprintf.h"

static virtio_pci_dev_t g_vdev;
static virtqueue_t g_ctrlq;
static virtqueue_t g_cursorq;
static uint32_t g_resource_id = 1;
static uint32_t g_fb_width = 0;
static uint32_t g_fb_height = 0;
static uint32_t g_fb_stride = 0;
static uint64_t g_fb_phys = 0;
static uint64_t g_fb_size = 0;
static int g_initialized = 0;
static int g_virgl = 0;
static uint32_t g_next_resource_id = 100;  /* start high to avoid scanout/cursor ids */

/* Track 3D resource dimensions so TRANSFER_TO_HOST can use the real row
 * stride (resource width), not the transfer-box width. Wrong stride on
 * sub-rects scrambled the desktop under the context menu. */
#define GPU_RES_DIMS_MAX 64
static struct {
    uint32_t id;
    uint32_t width;
    uint32_t height;
} g_res_dims[GPU_RES_DIMS_MAX];
static uint32_t g_res_dims_n;

static void gpu_track_res_dims(uint32_t id, uint32_t width, uint32_t height) {
    for (uint32_t i = 0; i < g_res_dims_n; i++) {
        if (g_res_dims[i].id == id) {
            g_res_dims[i].width = width;
            g_res_dims[i].height = height;
            return;
        }
    }
    if (g_res_dims_n < GPU_RES_DIMS_MAX) {
        g_res_dims[g_res_dims_n].id = id;
        g_res_dims[g_res_dims_n].width = width;
        g_res_dims[g_res_dims_n].height = height;
        g_res_dims_n++;
    }
}

bool virtio_gpu_res_dims(uint32_t resource_id, uint32_t *width, uint32_t *height) {
    for (uint32_t i = 0; i < g_res_dims_n; i++) {
        if (g_res_dims[i].id == resource_id) {
            if (width) *width = g_res_dims[i].width;
            if (height) *height = g_res_dims[i].height;
            return true;
        }
    }
    return false;
}

static uint32_t g_cursor_resource_id = 2;
static uint64_t g_cursor_phys = 0;
static uint32_t g_cursor_w = 64;
static uint32_t g_cursor_h = 64;

static bool gpu_send_recv(void *cmd, uint32_t cmd_len, void *resp, uint32_t resp_len) {
    /* QEMU may write more than sizeof(ctrl_hdr) for some commands,
     * so use a larger response buffer to avoid truncation warnings. */
    uint8_t resp_buf[64];
    memset(resp_buf, 0, sizeof(resp_buf));
    void *bufs[2]   = { cmd, resp_buf };
    uint32_t lens[2] = { cmd_len, sizeof(resp_buf) };
    uint16_t flags[2] = { 0, VRING_DESC_F_WRITE };
    uint16_t desc_idx;
    if (!virtqueue_add_buf(&g_ctrlq, &desc_idx, bufs, lens, flags, 2))
        return false;
    virtio_pci_notify_queue(&g_vdev, &g_ctrlq);
    if (!virtio_pci_wait_for_queue(&g_vdev, &g_ctrlq, 10000000))
        return false;
    uint16_t used_idx;
    uint32_t used_len;
    if (!virtqueue_get_used(&g_ctrlq, &used_idx, &used_len))
        return false;
    /* Copy response back to caller's buffer */
    uint32_t copy_len = resp_len < sizeof(resp_buf) ? resp_len : sizeof(resp_buf);
    memcpy(resp, resp_buf, copy_len);
    struct virtio_gpu_ctrl_hdr *rh = (struct virtio_gpu_ctrl_hdr *)resp;
    bool ok = (rh->type & 0xFF00) == 0x1100;  /* any 0x11xx = success */
    return ok;
}

static bool gpu_cursor_send(void *cmd, uint32_t cmd_len) {
    void *bufs[1]   = { cmd };
    uint32_t lens[1] = { cmd_len };
    uint16_t flags[1] = { 0 };
    uint16_t desc_idx;
    if (!virtqueue_add_buf(&g_cursorq, &desc_idx, bufs, lens, flags, 1))
        return false;
    virtio_pci_notify_queue(&g_vdev, &g_cursorq);
    /* Fire-and-forget: don't wait for response. The cursor queue
     * processes instantly in QEMU. Drain any completed entries to
     * recycle descriptors. */
    uint16_t used_idx;
    uint32_t used_len;
    virtqueue_get_used(&g_cursorq, &used_idx, &used_len);
    return true;
}

bool virtio_gpu_init(void) {
    if (g_initialized) return true;
    if (!virtio_pci_probe(&g_vdev, VIRTIO_GPU_PCI_DEVICE)) {
        kputs("[virtio-gpu] no device found\n");
        return false;
    }
    kprintf("[virtio-gpu] found at %x:%x.0\n", g_vdev.pci.bus, g_vdev.pci.dev);

    pci_enable_bus_master(&g_vdev.pci);
    virtio_pci_set_status(&g_vdev, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_pci_set_status(&g_vdev, VIRTIO_STATUS_DRIVER);

    uint64_t features = virtio_pci_get_features(&g_vdev);
    uint64_t want = VIRTIO_F_VERSION_1;
    if (features & (1ULL << VIRTIO_GPU_F_VIRGL)) {
        g_virgl = 1;
        want |= (1ULL << VIRTIO_GPU_F_VIRGL);
        kputs("[virtio-gpu] virgl (3D) support detected\n");
    } else {
        kputs("[virtio-gpu] no virgl support, 2D only\n");
    }
    features &= want;
    virtio_pci_set_features(&g_vdev, features);
    virtio_pci_set_status(&g_vdev, VIRTIO_STATUS_FEATURES_OK);

    if (!virtio_pci_setup_queue(&g_vdev, &g_ctrlq, 0)) {
        kputs("[virtio-gpu] ctrlq setup failed\n");
        return false;
    }
    if (!virtio_pci_setup_queue(&g_vdev, &g_cursorq, 1)) {
        kputs("[virtio-gpu] cursorq setup failed\n");
        return false;
    }
    virtio_pci_set_status(&g_vdev, VIRTIO_STATUS_DRIVER_OK);

    /* Get display info */
    struct virtio_gpu_ctrl_hdr req;
    struct virtio_gpu_resp_display_info resp;
    memset(&req, 0, sizeof(req));
    req.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    memset(&resp, 0, sizeof(resp));
    if (!gpu_send_recv(&req, sizeof(req), &resp, sizeof(resp))) {
        kputs("[virtio-gpu] get_display_info failed\n");
        return false;
    }

    /* Pick first enabled display or default to 1024x768 */
    g_fb_width = 1024;
    g_fb_height = 768;
    for (int i = 0; i < 16; i++) {
        if (resp.pmodes[i].enabled) {
            g_fb_width = resp.pmodes[i].r.width;
            g_fb_height = resp.pmodes[i].r.height;
            kprintf("[virtio-gpu] display %d: %ux%u\n", i, g_fb_width, g_fb_height);
            break;
        }
    }
    g_fb_stride = g_fb_width * 4;
    g_fb_size = (uint64_t)g_fb_height * g_fb_stride;

    /* Allocate scanout framebuffer (physically contiguous) */
    uint32_t pages = (uint32_t)((g_fb_size + PAGE_SIZE - 1) / PAGE_SIZE);
    g_fb_phys = pmm_alloc_contig(pages);
    if (!g_fb_phys) {
        kputs("[virtio-gpu] failed to alloc framebuffer\n");
        return false;
    }
    memset((void *)phys_to_virt(g_fb_phys), 0, g_fb_size);

    /* Create 2D resource */
    struct virtio_gpu_resource_create_2d create;
    memset(&create, 0, sizeof(create));
    create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create.resource_id = g_resource_id;
    create.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    create.width = g_fb_width;
    create.height = g_fb_height;
    struct virtio_gpu_ctrl_hdr create_resp;
    memset(&create_resp, 0, sizeof(create_resp));
    if (!gpu_send_recv(&create, sizeof(create), &create_resp, sizeof(create_resp))) {
        kputs("[virtio-gpu] resource_create failed\n");
        return false;
    }

    /* Attach backing — single contiguous descriptor for cmd+entries */
    struct {
        struct virtio_gpu_resource_attach_backing hdr;
        struct virtio_gpu_mem_entry entry;
    } attach;
    memset(&attach, 0, sizeof(attach));
    attach.hdr.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach.hdr.resource_id = g_resource_id;
    attach.hdr.nr_entries = 1;
    attach.entry.addr = g_fb_phys;
    attach.entry.length = (uint32_t)g_fb_size;
    struct virtio_gpu_ctrl_hdr attach_resp;
    memset(&attach_resp, 0, sizeof(attach_resp));
    if (!gpu_send_recv(&attach, sizeof(attach), &attach_resp, sizeof(attach_resp))) {
        kprintf("[virtio-gpu] attach_backing failed (QEMU compat), using VGA fallback resp=0x%x\n", attach_resp.type);
        /* Destroy the resource to clean up, then return false. */
        struct virtio_gpu_resource_unref unref;
        memset(&unref, 0, sizeof(unref));
        unref.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
        unref.resource_id = g_resource_id;
        struct virtio_gpu_ctrl_hdr unref_resp;
        memset(&unref_resp, 0, sizeof(unref_resp));
        gpu_send_recv(&unref, sizeof(unref), &unref_resp, sizeof(unref_resp));
        g_initialized = 0;
        g_fb_phys = 0;
        return false;
    }

    /* Set scanout */
    struct virtio_gpu_set_scanout scanout;
    memset(&scanout, 0, sizeof(scanout));
    scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout.r.x = 0; scanout.r.y = 0;
    scanout.r.width = g_fb_width; scanout.r.height = g_fb_height;
    scanout.scanout_id = 0;
    scanout.resource_id = g_resource_id;
    struct virtio_gpu_ctrl_hdr scanout_resp;
    memset(&scanout_resp, 0, sizeof(scanout_resp));
    if (!gpu_send_recv(&scanout, sizeof(scanout), &scanout_resp, sizeof(scanout_resp))) {
        kputs("[virtio-gpu] set_scanout failed\n");
        return false;
    }

    /* Create cursor resource */
    uint32_t cursor_size = g_cursor_w * g_cursor_h * 4;
    uint32_t cursor_pages = (cursor_size + PAGE_SIZE - 1) / PAGE_SIZE;
    g_cursor_phys = pmm_alloc_contig(cursor_pages);
    if (g_cursor_phys) {
        memset((void *)phys_to_virt(g_cursor_phys), 0, cursor_size);

        struct virtio_gpu_resource_create_2d cres;
        memset(&cres, 0, sizeof(cres));
        cres.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
        cres.resource_id = g_cursor_resource_id;
        cres.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
        cres.width = g_cursor_w;
        cres.height = g_cursor_h;
        struct virtio_gpu_ctrl_hdr cres_resp;
        memset(&cres_resp, 0, sizeof(cres_resp));
        if (gpu_send_recv(&cres, sizeof(cres), &cres_resp, sizeof(cres_resp))) {
            struct {
                struct virtio_gpu_resource_attach_backing hdr;
                struct virtio_gpu_mem_entry entry;
            } catt;
            memset(&catt, 0, sizeof(catt));
            catt.hdr.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
            catt.hdr.resource_id = g_cursor_resource_id;
            catt.hdr.nr_entries = 1;
            catt.entry.addr = g_cursor_phys;
            catt.entry.length = cursor_size;
            struct virtio_gpu_ctrl_hdr catt_resp;
            memset(&catt_resp, 0, sizeof(catt_resp));
            if (!gpu_send_recv(&catt, sizeof(catt), &catt_resp, sizeof(catt_resp))) {
                kputs("[virtio-gpu] cursor attach_backing failed\n");
            }
        } else {
            kputs("[virtio-gpu] cursor resource_create failed\n");
        }
    } else {
        kputs("[virtio-gpu] cursor phys alloc failed\n");
    }

    /* Initial flush to clear screen */
    virtio_gpu_flush(0, 0, g_fb_width, g_fb_height);

    g_initialized = 1;
    kputs("[virtio-gpu] initialized\n");
    return true;
}

bool virtio_gpu_get_fb_info(gpu_fb_info_t *info) {
    if (!g_initialized || !info) return false;
    info->backing_phys = g_fb_phys;
    info->backing_size = g_fb_size;
    info->width = g_fb_width;
    info->height = g_fb_height;
    info->stride = g_fb_stride;
    info->cursor_phys = g_cursor_phys;
    info->cursor_w = g_cursor_w;
    info->cursor_h = g_cursor_h;
    info->virgl = g_virgl ? 1 : 0;
    return true;
}

bool virtio_gpu_cursor_set(int32_t x, int32_t y, uint32_t hot_x, uint32_t hot_y) {
    if (!g_initialized || !g_cursor_phys) return false;

    /* Transfer cursor resource to host so QEMU has the pixel data. */
    struct virtio_gpu_transfer_to_host_2d xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    xfer.r.x = 0; xfer.r.y = 0;
    xfer.r.width = g_cursor_w; xfer.r.height = g_cursor_h;
    xfer.offset = 0;
    xfer.resource_id = g_cursor_resource_id;
    struct virtio_gpu_ctrl_hdr xfer_resp;
    memset(&xfer_resp, 0, sizeof(xfer_resp));
    bool xfer_ok = gpu_send_recv(&xfer, sizeof(xfer), &xfer_resp, sizeof(xfer_resp));
    if (!xfer_ok) kputs("[virtio-gpu] cursor transfer failed\n");

    struct virtio_gpu_update_cursor cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_UPDATE_CURSOR;
    cmd.pos.scanout_id = 0;
    cmd.pos.x = (uint32_t)x;
    cmd.pos.y = (uint32_t)y;
    cmd.resource_id = g_cursor_resource_id;
    cmd.hot_x = hot_x;
    cmd.hot_y = hot_y;
    return gpu_cursor_send(&cmd, sizeof(cmd));
}

bool virtio_gpu_cursor_move(int32_t x, int32_t y) {
    if (!g_initialized || !g_cursor_phys) return false;
    struct virtio_gpu_update_cursor cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_MOVE_CURSOR;
    cmd.pos.scanout_id = 0;
    cmd.pos.x = (uint32_t)x;
    cmd.pos.y = (uint32_t)y;
    cmd.resource_id = g_cursor_resource_id; /* keep cursor visible */
    return gpu_cursor_send(&cmd, sizeof(cmd));
}

bool virtio_gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!g_initialized) return false;
    if (x + w > g_fb_width) w = g_fb_width - x;
    if (y + h > g_fb_height) h = g_fb_height - y;
    if (w == 0 || h == 0) return true;

    /* Transfer to host */
    struct virtio_gpu_transfer_to_host_2d xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    xfer.r.x = x; xfer.r.y = y;
    xfer.r.width = w; xfer.r.height = h;
    xfer.offset = (uint64_t)y * g_fb_stride + (uint64_t)x * 4;
    xfer.resource_id = g_resource_id;
    struct virtio_gpu_ctrl_hdr xfer_resp;
    memset(&xfer_resp, 0, sizeof(xfer_resp));
    if (!gpu_send_recv(&xfer, sizeof(xfer), &xfer_resp, sizeof(xfer_resp))) {
        kputs("[virtio-gpu] transfer failed\n");
        return false;
    }

    /* Flush to display */
    struct virtio_gpu_resource_flush flush;
    memset(&flush, 0, sizeof(flush));
    flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush.r.x = x; flush.r.y = y;
    flush.r.width = w; flush.r.height = h;
    flush.resource_id = g_resource_id;
    struct virtio_gpu_ctrl_hdr flush_resp;
    memset(&flush_resp, 0, sizeof(flush_resp));
    if (!gpu_send_recv(&flush, sizeof(flush), &flush_resp, sizeof(flush_resp))) {
        kputs("[virtio-gpu] flush failed\n");
        return false;
    }
    return true;
}

bool virtio_gpu_present(void) {
    return g_initialized != 0;
}

bool virtio_gpu_virgl_present(void) {
    return g_initialized != 0 && g_virgl != 0;
}

uint32_t virtio_gpu_alloc_resource_id(void) {
    return g_next_resource_id++;
}

bool virtio_gpu_ctx_create(uint32_t ctx_id) {
    if (!g_virgl) return false;
    struct virtio_gpu_ctx_create cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_CTX_CREATE;
    cmd.hdr.ctx_id = ctx_id;
    cmd.nlen = 4;
    cmd.name[0] = 'v'; cmd.name[1] = 'i'; cmd.name[2] = 'r'; cmd.name[3] = 'g';
    struct virtio_gpu_ctrl_hdr resp;
    memset(&resp, 0, sizeof(resp));
    bool ok = gpu_send_recv(&cmd, sizeof(cmd), &resp, sizeof(resp));
    kprintf("[virtio-gpu] ctx_create(id=%u) %s resp type=0x%x flags=0x%x ctx_id=%u\n",
            ctx_id, ok ? "OK" : "FAIL", resp.type, resp.flags, resp.ctx_id);
    return ok;
}

bool virtio_gpu_ctx_destroy(uint32_t ctx_id) {
    if (!g_virgl) return false;
    struct virtio_gpu_ctx_destroy cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_CTX_DESTROY;
    cmd.hdr.ctx_id = ctx_id;
    struct virtio_gpu_ctrl_hdr resp;
    memset(&resp, 0, sizeof(resp));
    return gpu_send_recv(&cmd, sizeof(cmd), &resp, sizeof(resp));
}

bool virtio_gpu_ctx_attach_resource(uint32_t ctx_id, uint32_t resource_id) {
    if (!g_virgl) return false;
    struct virtio_gpu_ctx_attach_resource cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type = VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE;
    cmd.hdr.ctx_id = ctx_id;
    cmd.resource_id = resource_id;
    struct virtio_gpu_ctrl_hdr resp;
    memset(&resp, 0, sizeof(resp));
    bool ok = gpu_send_recv(&cmd, sizeof(cmd), &resp, sizeof(resp));
    kprintf("[virtio-gpu] ctx_attach(ctx=%u res=%u) %s resp=0x%x\n",
            ctx_id, resource_id, ok ? "OK" : "FAIL", resp.type);
    return ok;
}

bool virtio_gpu_resource_create_3d_for(uint32_t resource_id, uint32_t target,
                                       uint32_t format, uint32_t bind,
                                       uint32_t width, uint32_t height,
                                       uint32_t depth, uint32_t array_size,
                                       uint32_t last_level, uint32_t nr_samples,
                                       uint32_t flags) {
    struct virtio_gpu_resource_create_3d create;
    memset(&create, 0, sizeof(create));
    create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    create.resource_id = resource_id;
    create.target = target;
    create.format = format;
    create.bind = bind;
    create.width = width;
    create.height = height;
    create.depth = depth;
    create.array_size = array_size;
    create.last_level = last_level;
    create.nr_samples = nr_samples;
    create.flags = flags;
    struct virtio_gpu_ctrl_hdr resp;
    memset(&resp, 0, sizeof(resp));
    bool ok3d = gpu_send_recv(&create, sizeof(create), &resp, sizeof(resp));
    kprintf("[virtio-gpu] res_create_3d(id=%u target=%u %ux%u bind=%u) %s resp=0x%x\n",
            resource_id, target, width, height, bind, ok3d ? "OK" : "FAIL", resp.type);
    if (ok3d)
        gpu_track_res_dims(resource_id, width, height);
    return ok3d;
}

bool virtio_gpu_resource_create_2d_for(uint32_t resource_id, uint32_t format,
                                       uint32_t width, uint32_t height) {
    struct virtio_gpu_resource_create_2d create;
    memset(&create, 0, sizeof(create));
    create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create.resource_id = resource_id;
    create.format = format;
    create.width = width;
    create.height = height;
    struct virtio_gpu_ctrl_hdr resp;
    memset(&resp, 0, sizeof(resp));
    bool ok = gpu_send_recv(&create, sizeof(create), &resp, sizeof(resp));
    kprintf("[virtio-gpu] res_create_2d(id=%u %ux%u fmt=%u) %s resp=0x%x\n",
            resource_id, width, height, format, ok ? "OK" : "FAIL", resp.type);
    return ok;
}

bool virtio_gpu_resource_attach_backing_for(uint32_t resource_id,
                                            uint64_t phys, uint64_t size) {
    struct {
        struct virtio_gpu_resource_attach_backing hdr;
        struct virtio_gpu_mem_entry entry;
    } attach;
    memset(&attach, 0, sizeof(attach));
    attach.hdr.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach.hdr.resource_id = resource_id;
    attach.hdr.nr_entries = 1;
    attach.entry.addr = phys;
    attach.entry.length = (uint32_t)size;
    struct virtio_gpu_ctrl_hdr resp;
    memset(&resp, 0, sizeof(resp));
    return gpu_send_recv(&attach, sizeof(attach), &resp, sizeof(resp));
}

bool virtio_gpu_resource_attach_backing_sg(uint32_t resource_id,
                                           uint64_t *phys_pages,
                                           uint32_t npages,
                                           uint64_t buf_size) {
    /* Build attach_backing command with one mem_entry per page.
     * Command layout: [attach_backing header] [mem_entry * npages] [response]
     * We coalesce contiguous pages to reduce descriptor count.
     * The total IOV length is trimmed to buf_size to avoid exceeding
     * the resource capacity when the last page is partial. */
    if (npages == 0) return false;

    /* Coalesce contiguous physical pages into runs */
    uint32_t max_runs = npages;
    struct {
        uint64_t addr;
        uint32_t length;
    } *runs;

    /* Use stack buffer for small surfaces, heap for large ones */
    runs = (void *)kmalloc(max_runs * sizeof(*runs));
    if (!runs) return false;

    uint32_t nruns = 0;
    runs[0].addr = phys_pages[0];
    runs[0].length = PAGE_SIZE;
    for (uint32_t i = 1; i < npages; i++) {
        if (phys_pages[i] == phys_pages[i-1] + PAGE_SIZE) {
            runs[nruns].length += PAGE_SIZE;
        } else {
            nruns++;
            runs[nruns].addr = phys_pages[i];
            runs[nruns].length = PAGE_SIZE;
        }
    }
    nruns++;

    /* Trim last entry so total IOV length matches buf_size exactly */
    uint64_t total = (uint64_t)npages * PAGE_SIZE;
    if (total > buf_size && nruns > 0) {
        runs[nruns - 1].length -= (uint32_t)(total - buf_size);
    }

    /* Build the command buffer: header + nruns mem_entries */
    uint32_t cmd_size = sizeof(struct virtio_gpu_resource_attach_backing) +
                        nruns * sizeof(struct virtio_gpu_mem_entry);
    void *cmd = kmalloc(cmd_size);
    if (!cmd) { kfree(runs); return false; }

    memset(cmd, 0, cmd_size);
    struct virtio_gpu_resource_attach_backing *hdr =
        (struct virtio_gpu_resource_attach_backing *)cmd;
    hdr->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    hdr->resource_id = resource_id;
    hdr->nr_entries = nruns;

    struct virtio_gpu_mem_entry *entries =
        (struct virtio_gpu_mem_entry *)((uint8_t *)cmd +
        sizeof(struct virtio_gpu_resource_attach_backing));
    for (uint32_t i = 0; i < nruns; i++) {
        entries[i].addr = runs[i].addr;
        entries[i].length = runs[i].length;
    }

    struct virtio_gpu_ctrl_hdr resp;
    memset(&resp, 0, sizeof(resp));
    bool ok = gpu_send_recv(cmd, cmd_size, &resp, sizeof(resp));

    kfree(cmd);
    kfree(runs);
    return ok;
}

bool virtio_gpu_resource_unref_for(uint32_t resource_id) {
    struct virtio_gpu_resource_unref unref;
    memset(&unref, 0, sizeof(unref));
    unref.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    unref.resource_id = resource_id;
    struct virtio_gpu_ctrl_hdr resp;
    memset(&resp, 0, sizeof(resp));
    return gpu_send_recv(&unref, sizeof(unref), &resp, sizeof(resp));
}

bool virtio_gpu_transfer_to_host_2d_for(uint32_t resource_id, uint32_t x,
                                        uint32_t y, uint32_t w, uint32_t h,
                                        uint64_t offset) {
    struct virtio_gpu_transfer_to_host_2d xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    xfer.r.x = x; xfer.r.y = y;
    xfer.r.width = w; xfer.r.height = h;
    xfer.offset = offset;
    xfer.resource_id = resource_id;
    struct virtio_gpu_ctrl_hdr resp;
    memset(&resp, 0, sizeof(resp));
    return gpu_send_recv(&xfer, sizeof(xfer), &resp, sizeof(resp));
}

bool virtio_gpu_transfer_to_host_3d_for(uint32_t resource_id, uint32_t x,
                                        uint32_t y, uint32_t z, uint32_t w,
                                        uint32_t h, uint32_t d, uint64_t offset,
                                        uint32_t level, uint32_t stride,
                                        uint32_t layer_stride) {
    struct virtio_gpu_transfer_to_host_3d xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D;
    xfer.box.x = x; xfer.box.y = y; xfer.box.z = z;
    xfer.box.w = w; xfer.box.h = h; xfer.box.d = d;
    xfer.offset = offset;
    xfer.resource_id = resource_id;
    xfer.level = level;
    xfer.stride = stride;
    xfer.layer_stride = layer_stride;
    struct virtio_gpu_ctrl_hdr resp;
    memset(&resp, 0, sizeof(resp));
    return gpu_send_recv(&xfer, sizeof(xfer), &resp, sizeof(resp));
}

bool virtio_gpu_submit_3d(uint32_t ctx_id, void *cmds, uint32_t size) {
    if (!g_virgl) return false;
    /* per-frame submit_3d logged only on failure */
    /* Copy userspace command data to kernel buffer — virt_to_phys only
     * works for kernel higher-half addresses, not userspace pointers. */
    void *kbuf = kmalloc(size);
    if (!kbuf) return false;
    memcpy(kbuf, cmds, size);

    struct virtio_gpu_submit_3d hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.hdr.type = VIRTIO_GPU_CMD_SUBMIT_3D;
    hdr.hdr.ctx_id = ctx_id;
    hdr.size = size;  /* QEMU expects size in bytes */

    struct virtio_gpu_ctrl_hdr resp;
    memset(&resp, 0, sizeof(resp));
    uint8_t resp_buf[64];
    memset(resp_buf, 0, sizeof(resp_buf));

    void *bufs[3] = { &hdr, kbuf, resp_buf };
    uint32_t lens[3] = { sizeof(hdr), size, sizeof(resp_buf) };
    uint16_t flags[3] = { 0, 0, VRING_DESC_F_WRITE };
    uint16_t desc_idx;
    if (!virtqueue_add_buf(&g_ctrlq, &desc_idx, bufs, lens, flags, 3)) {
        kfree(kbuf);
        return false;
    }
    virtio_pci_notify_queue(&g_vdev, &g_ctrlq);
    if (!virtio_pci_wait_for_queue(&g_vdev, &g_ctrlq, 10000000)) {
        kfree(kbuf);
        return false;
    }
    uint16_t used_idx;
    uint32_t used_len;
    virtqueue_get_used(&g_ctrlq, &used_idx, &used_len);
    kfree(kbuf);
    memcpy(&resp, resp_buf, sizeof(resp));
    bool ok = (resp.type & 0xFF00) == 0x1100;
    /* submit_3d response logged only on failure */
    if (!ok) kprintf("[virtio-gpu] submit_3d FAIL resp 0x%x\n", resp.type);
    return ok;
}

bool virtio_gpu_set_scanout_for(uint32_t scanout_id, uint32_t resource_id,
                                uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    struct virtio_gpu_set_scanout scanout;
    memset(&scanout, 0, sizeof(scanout));
    scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout.r.x = x; scanout.r.y = y;
    scanout.r.width = w; scanout.r.height = h;
    scanout.scanout_id = scanout_id;
    scanout.resource_id = resource_id;
    struct virtio_gpu_ctrl_hdr resp;
    memset(&resp, 0, sizeof(resp));
    return gpu_send_recv(&scanout, sizeof(scanout), &resp, sizeof(resp));
}

bool virtio_gpu_flush_for(uint32_t resource_id, uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h) {
    struct virtio_gpu_resource_flush flush;
    memset(&flush, 0, sizeof(flush));
    flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush.r.x = x; flush.r.y = y;
    flush.r.width = w; flush.r.height = h;
    flush.resource_id = resource_id;
    struct virtio_gpu_ctrl_hdr resp;
    memset(&resp, 0, sizeof(resp));
    return gpu_send_recv(&flush, sizeof(flush), &resp, sizeof(resp));
}
