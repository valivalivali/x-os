#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "kernel/include/syscall.h"

/* VirtIO GPU 2D + 3D (virgl) command types */
enum virtio_gpu_ctrl_type {
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO     = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D   = 0x0101,
    VIRTIO_GPU_CMD_RESOURCE_UNREF       = 0x0102,
    VIRTIO_GPU_CMD_SET_SCANOUT          = 0x0103,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH       = 0x0104,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D  = 0x0105,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106,
    VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING = 0x0107,
    /* 2D extended commands (0x0108-0x010D) */
    VIRTIO_GPU_CMD_GET_CAPSET_INFO      = 0x0108,
    VIRTIO_GPU_CMD_GET_CAPSET           = 0x0109,
    VIRTIO_GPU_CMD_GET_EDID             = 0x010A,
    VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID = 0x010B,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB = 0x010C,
    VIRTIO_GPU_CMD_SET_SCANOUT_BLOB     = 0x010D,
    /* 3D (virgl) commands — moved to 0x0200+ in modern spec */
    VIRTIO_GPU_CMD_CTX_CREATE           = 0x0200,
    VIRTIO_GPU_CMD_CTX_DESTROY          = 0x0201,
    VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE  = 0x0202,
    VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE  = 0x0203,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_3D   = 0x0204,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D  = 0x0205,
    VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D = 0x0206,
    VIRTIO_GPU_CMD_SUBMIT_3D            = 0x0207,
    /* Cursor commands */
    VIRTIO_GPU_CMD_UPDATE_CURSOR          = 0x0300,
    VIRTIO_GPU_CMD_MOVE_CURSOR            = 0x0301,
    /* Responses */
    VIRTIO_GPU_RESP_OK_NODATA           = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO     = 0x1101,
    VIRTIO_GPU_RESP_ERR_UNSPEC          = 0x1200,
    VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY   = 0x1201,
    VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID = 0x1202,
    VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID = 0x1203,
    VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID = 0x1204,
    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER = 0x1205,
};

/* Feature bits */
#define VIRTIO_GPU_F_VIRGL          0
#define VIRTIO_GPU_F_EDID           1

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

/* 3D (virgl) structs */
struct virtio_gpu_ctx_create {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t nlen;      /* context name length */
    uint32_t padding;
    char name[64];      /* context name */
    uint32_t context_init; /* context init flags (virglrenderer 1.0+) */
} __attribute__((packed));

struct virtio_gpu_ctx_destroy {
    struct virtio_gpu_ctrl_hdr hdr;
} __attribute__((packed));

struct virtio_gpu_ctx_attach_resource {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_submit_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t size;      /* size of command stream in bytes */
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_create_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t target;     /* GL_TEXTURE_2D etc */
    uint32_t format;     /* virgl format */
    uint32_t bind;       /* GL_TEXTURE_2D | GL_RENDERBUFFER etc */
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_box {
        uint32_t x, y, z;
        uint32_t w, h, d;
    } box;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
    uint32_t padding;
} __attribute__((packed));

bool virtio_gpu_init(void);
bool virtio_gpu_get_fb_info(gpu_fb_info_t *info);
bool virtio_gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
bool virtio_gpu_cursor_set(int32_t x, int32_t y, uint32_t hot_x, uint32_t hot_y);
bool virtio_gpu_cursor_move(int32_t x, int32_t y);
bool virtio_gpu_present(void);

/* 3D (virgl) API */
bool virtio_gpu_virgl_present(void);
bool virtio_gpu_ctx_create(uint32_t ctx_id);
bool virtio_gpu_ctx_destroy(uint32_t ctx_id);
bool virtio_gpu_ctx_attach_resource(uint32_t ctx_id, uint32_t resource_id);
bool virtio_gpu_resource_create_2d_for(uint32_t resource_id, uint32_t format,
                                       uint32_t width, uint32_t height);
bool virtio_gpu_resource_create_3d_for(uint32_t resource_id, uint32_t target,
                                       uint32_t format, uint32_t bind,
                                       uint32_t width, uint32_t height,
                                       uint32_t depth, uint32_t array_size,
                                       uint32_t last_level, uint32_t nr_samples,
                                       uint32_t flags);
bool virtio_gpu_resource_attach_backing_for(uint32_t resource_id,
                                            uint64_t phys, uint64_t size);
bool virtio_gpu_resource_attach_backing_sg(uint32_t resource_id,
                                           uint64_t *phys_pages,
                                           uint32_t npages,
                                           uint64_t buf_size);
bool virtio_gpu_resource_unref_for(uint32_t resource_id);
bool virtio_gpu_transfer_to_host_2d_for(uint32_t resource_id, uint32_t x,
                                        uint32_t y, uint32_t w, uint32_t h,
                                        uint64_t offset);
bool virtio_gpu_transfer_to_host_3d_for(uint32_t resource_id, uint32_t x,
                                        uint32_t y, uint32_t z, uint32_t w,
                                        uint32_t h, uint32_t d, uint64_t offset,
                                        uint32_t level, uint32_t stride,
                                        uint32_t layer_stride);
/* Look up width/height recorded at res_create_3d time. */
bool virtio_gpu_res_dims(uint32_t resource_id, uint32_t *width, uint32_t *height);
bool virtio_gpu_submit_3d(uint32_t ctx_id, void *cmds, uint32_t size);
bool virtio_gpu_set_scanout_for(uint32_t scanout_id, uint32_t resource_id,
                                uint32_t x, uint32_t y, uint32_t w, uint32_t h);
bool virtio_gpu_flush_for(uint32_t resource_id, uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h);
uint32_t virtio_gpu_alloc_resource_id(void);
