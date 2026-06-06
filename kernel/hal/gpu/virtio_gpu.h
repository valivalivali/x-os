#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "kernel/include/syscall.h"

/* VirtIO GPU 2D command types */
enum virtio_gpu_ctrl_type {
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO     = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D   = 0x0101,
    VIRTIO_GPU_CMD_RESOURCE_UNREF       = 0x0102,
    VIRTIO_GPU_CMD_SET_SCANOUT          = 0x0103,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH       = 0x0104,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D  = 0x0105,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106,
    VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING = 0x0107,
    VIRTIO_GPU_CMD_UPDATE_CURSOR          = 0x0300,
    VIRTIO_GPU_CMD_MOVE_CURSOR            = 0x0301,
    VIRTIO_GPU_RESP_OK_NODATA           = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO     = 0x1101,
};

enum virtio_gpu_formats {
    VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM = 1,
    VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM = 2,
    VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM = 3,
    VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM = 4,
    VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM = 67,
    VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM = 68,
};

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one {
        struct virtio_gpu_rect r;
        uint32_t enabled;
        uint32_t flags;
    } pmodes[16];
} __attribute__((packed));

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_resource_unref {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed));

struct virtio_gpu_cursor_pos {
    uint32_t scanout_id;
    uint32_t x;
    uint32_t y;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_update_cursor {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_cursor_pos pos;
    uint32_t resource_id;
    uint32_t hot_x;
    uint32_t hot_y;
    uint32_t padding;
} __attribute__((packed));

bool virtio_gpu_init(void);
bool virtio_gpu_get_fb_info(gpu_fb_info_t *info);
bool virtio_gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
bool virtio_gpu_cursor_set(int32_t x, int32_t y, uint32_t hot_x, uint32_t hot_y);
bool virtio_gpu_cursor_move(int32_t x, int32_t y);
bool virtio_gpu_present(void);
