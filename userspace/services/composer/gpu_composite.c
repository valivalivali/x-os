/* GPU compositing via virgl (OpenGL ES through virtio-gpu 3D).
 *
 * Phase 3: Per-surface GPU texture resources with backing memory,
 * transfer-to-host for GPU-side texture uploads, and virgl 3D
 * command stream for compositing.
 *
 * Each app surface gets a 2D GPU resource backed by the surface's
 * shared memory pages. Surface data is uploaded to the GPU via
 * TRANSFER_TO_HOST_2D. The compositor then submits a 3D command
 * stream that draws textured quads with alpha blending. */

#include "kernel/include/syscall.h"
#include "gpu_composite.h"
#include <stddef.h>
#include <stdint.h>

/* Freestanding: no string.h available */
static void *local_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}
static size_t local_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

#define MAX_GPU_SURFACES 8
#define PAGE_SIZE 4096
#define GPU_CTX_ID    1

/* Virtio-gpu formats */
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2

/* Virgl command encoding: VIRGL_CMD0(cmd, obj, len) */
#define VIRGL_CMD0(cmd, obj, len) ((cmd) | ((obj) << 8) | ((len) << 16))

/* Virgl context commands (from virgl_protocol.h enum) */
#define VCCMD_CREATE_OBJECT        1
#define VCCMD_BIND_OBJECT          2
#define VCCMD_DESTROY_OBJECT       3
#define VCCMD_SET_VIEWPORT_STATE   4
#define VCCMD_SET_FRAMEBUFFER_STATE 5
#define VCCMD_SET_VERTEX_BUFFERS   6
#define VCCMD_CLEAR                7
#define VCCMD_DRAW_VBO             8
#define VCCMD_RESOURCE_INLINE_WRITE 9
#define VCCMD_SET_SAMPLER_VIEWS    10
#define VCCMD_BIND_SAMPLER_STATES  18
#define VCCMD_SET_SAMPLE_MASK      24
#define VCCMD_BIND_SHADER          31
#define VCCMD_LINK_SHADER          50

/* Object types */
#define VOBJ_BLEND       1
#define VOBJ_RASTERIZER  2
#define VOBJ_DSA         3
#define VOBJ_SHADER      4
#define VOBJ_VERTEX_ELEMENTS 5
#define VOBJ_SURFACE     8
#define VOBJ_SAMPLER_VIEW  6
#define VOBJ_SAMPLER_STATE 7

/* Pipe shader types */
#define PIPE_SHADER_VERTEX   0
#define PIPE_SHADER_FRAGMENT 1

/* Pipe primitive types */
#define PIPE_PRIM_TRIANGLE_STRIP 6

/* Pipe blend factors/functions */
#define PIPE_BLENDFACTOR_ONE             1
#define PIPE_BLENDFACTOR_SRC_ALPHA       4
#define PIPE_BLENDFACTOR_ONE_MINUS_SRC_ALPHA 5
#define PIPE_BLEND_ADD                  0

/* Pipe resource formats */
#define PIPE_FORMAT_B8G8R8A8_UNORM 1

/* Virglrenderer bind flags (VIRGL_RES_BIND_*) — different from pipe bind flags! */
#define VIRGL_BIND_RENDER_TARGET   (1 << 1)
#define VIRGL_BIND_SAMPLER_VIEW    (1 << 3)
#define VIRGL_BIND_VERTEX_BUFFER   (1 << 4)
#define VIRGL_BIND_INDEX_BUFFER    (1 << 5)
#define VIRGL_BIND_CONSTANT_BUFFER (1 << 6)
#define VIRGL_BIND_SCANOUT         (1 << 18)

/* Pipe texture targets */
#define PIPE_BUFFER          0
#define PIPE_TEXTURE_2D      2


/* Object handles (fixed, for virgl command stream objects) */
#define FB_SURFACE_H    10  /* surface handle for FB */
#define VS_HANDLE       20  /* vertex shader handle */
#define FS_HANDLE       21  /* fragment shader handle */
#define VE_HANDLE       30  /* vertex elements handle */
#define BLEND_HANDLE    40  /* blend state handle */
#define RAST_HANDLE     41  /* rasterizer state handle */
#define DSA_HANDLE      42  /* depth-stencil-alpha state handle */
#define SAMPER_STATE_H  60  /* sampler state handle */

/* Resource IDs allocated dynamically in gpu_comp_init */
static uint32_t g_fb_res_id;   /* framebuffer/render target 2D resource */
static uint32_t g_vb_res_id;   /* vertex buffer 3D resource */

/* fui: float to uint32 bit representation */
static uint32_t fui(float f) {
    uint32_t u;
    local_memcpy(&u, &f, 4);
    return u;
}

typedef struct {
    uint32_t resource_id;
    int      active;
    uint32_t w, h;
    uint64_t vaddr;
    uint32_t npages;
} gpu_surface_t;

static gpu_surface_t g_surfaces[MAX_GPU_SURFACES];
static int g_active = 0;
static int32_t g_fb_w, g_fb_h;
static int g_initialized = 0;

/* TGSI shader source for textured quad rendering.
 * Vertex shader: takes position (xy) and UV (uv), passes UV through.
 * Fragment shader: samples texture at UV, outputs color. */
static const char vs_source[] =
    "VERT\n"
    "DCL IN[0]\n"
    "DCL IN[1]\n"
    "DCL OUT[0], POSITION\n"
    "DCL OUT[1], GENERIC[0]\n"
    "  0: MOV OUT[1], IN[1]\n"
    "  1: MOV OUT[0], IN[0]\n"
    "  2: END\n";

static const char fs_source[] =
    "FRAG\n"
    "DCL IN[0], GENERIC[0], LINEAR\n"
    "DCL SAMP[0]\n"
    "DCL SVIEW[0], 2D, FLOAT\n"
    "DCL OUT[0], COLOR\n"
    "  0: TEX OUT[0], IN[0], SAMP[0], 2D\n"
    "  1: END\n";

/* Command stream builder */
#define MAX_CMD_DWORDS 4096
static uint32_t g_cmd_buf[MAX_CMD_DWORDS];
static uint32_t g_cmd_pos;

static void cmd_reset(void) { g_cmd_pos = 0; }
static void cmd_dword(uint32_t v) {
    if (g_cmd_pos < MAX_CMD_DWORDS) g_cmd_buf[g_cmd_pos++] = v;
}
static void cmd_float(float f) { cmd_dword(fui(f)); }
static void cmd_string(const char *s, uint32_t len) {
    /* Pad string to 4-byte boundary */
    uint32_t padded = (len + 3) & ~3u;
    uint32_t i;
    for (i = 0; i < len; i++)
        ((uint8_t *)g_cmd_buf)[g_cmd_pos * 4 + i] = s[i];
    for (; i < padded; i++)
        ((uint8_t *)g_cmd_buf)[g_cmd_pos * 4 + i] = 0;
    g_cmd_pos += padded / 4;
}
static int g_submit_seq = 0;
static void cmd_submit(void) {
    if (g_cmd_pos > 0) {
        char dbg[64];
        const char *p = dbg;
        int n = 0;
        /* log: "submit #N dwords=D cmd0=0xHHHHHHHH" */
        const char hex[] = "0123456789abcdef";
        n = 0; dbg[n++]='s';dbg[n++]='u';dbg[n++]='b';dbg[n++]='#';
        dbg[n++] = hex[(g_submit_seq >> 4) & 0xf]; dbg[n++] = hex[g_submit_seq & 0xf];
        dbg[n++]=' ';dbg[n++]='d';dbg[n++]='w';dbg[n++]='=';
        dbg[n++] = hex[(g_cmd_pos >> 4) & 0xf]; dbg[n++] = hex[g_cmd_pos & 0xf];
        dbg[n++]=' ';dbg[n++]='c';dbg[n++]='0';dbg[n++]='=';
        dbg[n++]='0';dbg[n++]='x';
        for (int b=28;b>=0;b-=4) dbg[n++]=hex[(g_cmd_buf[0]>>b)&0xf];
        dbg[n++]='\n';
        syscall2(SYS_DEBUG_LOG, (uintptr_t)p, n);
        g_submit_seq++;
        sys_gpu_submit_3d(GPU_CTX_ID, g_cmd_buf, g_cmd_pos * 4);
    }
}

/* Encode a shader object with TGSI text */
static void emit_shader(uint32_t handle, uint32_t type, const char *src) {
    uint32_t slen = local_strlen(src) + 1;
    uint32_t padded = (slen + 3) & ~3u;
    /* len = dwords after header = handle + type + offlen + num_tokens + num_so_outputs + text */
    uint32_t data_len = 5 + padded / 4;
    char dbg[80];
    const char hex[] = "0123456789abcdef";
    int n=0;
    dbg[n++]='s';dbg[n++]='h';dbg[n++]=':';dbg[n++]='s';dbg[n++]='l';
    dbg[n++]='=';dbg[n++]=hex[(slen>>4)&0xf];dbg[n++]=hex[slen&0xf];
    dbg[n++]=' ';dbg[n++]='p';dbg[n++]='a';dbg[n++]='d';dbg[n++]='=';
    dbg[n++]=hex[(padded>>4)&0xf];dbg[n++]=hex[padded&0xf];
    dbg[n++]=' ';dbg[n++]='d';dbg[n++]='l';dbg[n++]='=';
    dbg[n++]=hex[(data_len>>4)&0xf];dbg[n++]=hex[data_len&0xf];
    dbg[n++]='\n';
    syscall2(SYS_DEBUG_LOG, (uintptr_t)dbg, n);
    cmd_dword(VIRGL_CMD0(VCCMD_CREATE_OBJECT, VOBJ_SHADER, data_len));
    cmd_dword(handle);
    cmd_dword(type);
    cmd_dword(slen);  /* offlen: full length in low 31 bits */
    cmd_dword(300);   /* num_tokens (approximate) */
    cmd_dword(0);     /* num_so_outputs = 0 (no stream output) */
    cmd_string(src, slen);
}

int gpu_comp_init(int32_t fb_w, int32_t fb_h, uint64_t fb_phys, uint64_t fb_size) {
    (void)fb_phys;
    (void)fb_size;

    if (!sys_gpu_virgl_present()) {
        return 0;
    }

    g_fb_w = fb_w;
    g_fb_h = fb_h;

    if (sys_gpu_ctx_create(GPU_CTX_ID) != 0) {
        return 0;
    }

    /* Create framebuffer render target as a 2D resource */
    g_fb_res_id = sys_gpu_alloc_res_id();
    if (sys_gpu_res_create_2d(g_fb_res_id, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                              fb_w, fb_h) != 0) {
        return 0;
    }

    /* Attach FB resource to the virgl context */
    sys_gpu_ctx_attach(GPU_CTX_ID, g_fb_res_id);

    /* Set scanout to the framebuffer resource */
    sys_gpu_set_scanout(0, g_fb_res_id, 0, 0, fb_w, fb_h);

    /* Create vertex shader */
    cmd_reset();
    emit_shader(VS_HANDLE, PIPE_SHADER_VERTEX, vs_source);
    cmd_submit();

    /* Create fragment shader */
    cmd_reset();
    emit_shader(FS_HANDLE, PIPE_SHADER_FRAGMENT, fs_source);
    cmd_submit();

    /* Bind shaders */
    cmd_reset();
    /* BIND_SHADER: cmd + handle + type = 3 dwords */
    cmd_dword(VIRGL_CMD0(VCCMD_BIND_SHADER, 0, 2));
    cmd_dword(VS_HANDLE);
    cmd_dword(PIPE_SHADER_VERTEX);
    cmd_dword(VIRGL_CMD0(VCCMD_BIND_SHADER, 0, 2));
    cmd_dword(FS_HANDLE);
    cmd_dword(PIPE_SHADER_FRAGMENT);
    cmd_submit();

    /* Create blend state: alpha blending (src_alpha, 1-src_alpha) */
    cmd_reset();
    /* CREATE_OBJECT BLEND: cmd + handle + S0 + S1 + 8x RT */
    cmd_dword(VIRGL_CMD0(VCCMD_CREATE_OBJECT, VOBJ_BLEND, 11));
    cmd_dword(BLEND_HANDLE);
    cmd_dword(0);  /* S0: no independent blend, no logicop */
    cmd_dword(0);  /* S1: logicop func */
    /* RT[0]: blend enabled, ADD, src_alpha, 1-src_alpha, colormask=0xF */
    cmd_dword((1 << 0) |  /* blend_enable */
              (PIPE_BLEND_ADD << 2) |     /* rgb_func */
              (PIPE_BLENDFACTOR_SRC_ALPHA << 5) |  /* rgb_src */
              (PIPE_BLENDFACTOR_ONE_MINUS_SRC_ALPHA << 9) | /* rgb_dst */
              (PIPE_BLEND_ADD << 13) |    /* alpha_func */
              (PIPE_BLENDFACTOR_SRC_ALPHA << 16) | /* alpha_src */
              (PIPE_BLENDFACTOR_ONE_MINUS_SRC_ALPHA << 20) | /* alpha_dst */
              (0xF << 27));  /* colormask */
    /* RT[1..7]: all zeros */
    for (int i = 1; i < 8; i++) cmd_dword(0);
    cmd_submit();

    /* Bind blend state */
    cmd_reset();
    cmd_dword(VIRGL_CMD0(VCCMD_BIND_OBJECT, VOBJ_BLEND, 1));
    cmd_dword(BLEND_HANDLE);
    cmd_submit();

    /* Create DSA state: no depth, no stencil, alpha test disabled */
    cmd_reset();
    /* DSA: handle + S0 + S1(stencil0) + S2(stencil1) + alpha_ref = 5 */
    cmd_dword(VIRGL_CMD0(VCCMD_CREATE_OBJECT, VOBJ_DSA, 5));
    cmd_dword(DSA_HANDLE);
    cmd_dword(0);  /* S0: nothing enabled */
    cmd_dword(0);  /* S1: stencil[0] */
    cmd_dword(0);  /* S2: stencil[1] */
    cmd_dword(0);  /* alpha ref (float 0.0) */
    cmd_submit();

    /* Bind DSA state */
    cmd_reset();
    cmd_dword(VIRGL_CMD0(VCCMD_BIND_OBJECT, VOBJ_DSA, 1));
    cmd_dword(DSA_HANDLE);
    cmd_submit();

    /* Create rasterizer state: no culling, no depth clip */
    cmd_reset();
    /* RASTERIZER: handle + S0..S7 = 9 dwords */
    cmd_dword(VIRGL_CMD0(VCCMD_CREATE_OBJECT, VOBJ_RASTERIZER, 9));
    cmd_dword(RAST_HANDLE);
    /* S0: half_pixel_center=1, bottom_edge_rule=1 */
    cmd_dword((1 << 24) | (1 << 25));
    cmd_dword(fui(1.0f));  /* S1: point_size */
    cmd_dword(0);  /* S2: sprite_coord_enable */
    cmd_dword(0);  /* S3 */
    cmd_dword(fui(1.0f));  /* S4: line_width */
    cmd_dword(0);  /* S5: offset_units */
    cmd_dword(0);  /* S6: offset_scale */
    cmd_dword(0);  /* S7: offset_clamp */
    cmd_submit();

    /* Bind rasterizer */
    cmd_reset();
    cmd_dword(VIRGL_CMD0(VCCMD_BIND_OBJECT, VOBJ_RASTERIZER, 1));
    cmd_dword(RAST_HANDLE);
    cmd_submit();

    /* Create vertex elements: 2 attributes
     * attr0: position (2 floats) at offset 0
     * attr1: UV (2 floats) at offset 8 */
    cmd_reset();
    /* VERTEX_ELEMENTS: cmd + handle + (src_offset + instance_divisor + vertex_buffer_index + src_format) * 2 */
    cmd_dword(VIRGL_CMD0(VCCMD_CREATE_OBJECT, VOBJ_VERTEX_ELEMENTS, 1 + 4 * 2));
    cmd_dword(VE_HANDLE);
    /* attr0: position */
    cmd_dword(0);   /* src_offset */
    cmd_dword(0);   /* instance_divisor */
    cmd_dword(0);   /* vertex_buffer_index */
    cmd_dword(67);  /* src_format = R32G32_FLOAT (pipe format) */
    /* attr1: UV */
    cmd_dword(8);   /* src_offset */
    cmd_dword(0);   /* instance_divisor */
    cmd_dword(0);   /* vertex_buffer_index */
    cmd_dword(67);  /* src_format = R32G32_FLOAT */
    cmd_submit();

    /* Bind vertex elements */
    cmd_reset();
    cmd_dword(VIRGL_CMD0(VCCMD_BIND_OBJECT, VOBJ_VERTEX_ELEMENTS, 1));
    cmd_dword(VE_HANDLE);
    cmd_submit();

    /* Create sampler state: linear filtering, clamp_to_edge wrap */
    cmd_reset();
    /* SAMPLER_STATE: len = 9 (handle + S0 + lod_bias + min_lod + max_lod + 4 border_colors) */
    cmd_dword(VIRGL_CMD0(VCCMD_CREATE_OBJECT, VOBJ_SAMPLER_STATE, 9));
    cmd_dword(SAMPER_STATE_H);
    /* S0: wrap_s=CLAMP_TO_EDGE(2) | wrap_t=2<<3 | wrap_r=2<<6 | min_img=LINEAR(1)<<9 | min_mip=NONE(0)<<11 | mag_img=LINEAR(1)<<13 */
    cmd_dword(2 | (2 << 3) | (2 << 6) | (1 << 9) | (0 << 11) | (1 << 13));
    cmd_dword(fui(0.0f));  /* lod_bias */
    cmd_dword(fui(0.0f));  /* min_lod */
    cmd_dword(fui(0.0f));  /* max_lod */
    cmd_dword(0);  /* border_color[0] */
    cmd_dword(0);  /* border_color[1] */
    cmd_dword(0);  /* border_color[2] */
    cmd_dword(0);  /* border_color[3] */
    cmd_submit();

    /* Bind sampler state to fragment shader, slot 0 */
    cmd_reset();
    /* BIND_SAMPLER_STATES: len = shader_type + start_slot + handle = 3 */
    cmd_dword(VIRGL_CMD0(VCCMD_BIND_SAMPLER_STATES, 0, 3));
    cmd_dword(PIPE_SHADER_FRAGMENT);  /* shader_type */
    cmd_dword(0);                     /* start_slot */
    cmd_dword(SAMPER_STATE_H);        /* handle */
    cmd_submit();

    /* Set sample mask */
    cmd_reset();
    cmd_dword(VIRGL_CMD0(VCCMD_SET_SAMPLE_MASK, 0, 1));
    cmd_dword(0xFFFF);
    cmd_submit();

    /* Create surface object for framebuffer resource */
    cmd_reset();
    /* SURFACE: handle + res_handle + format + layer_info + level = 5 */
    cmd_dword(VIRGL_CMD0(VCCMD_CREATE_OBJECT, VOBJ_SURFACE, 5));
    cmd_dword(FB_SURFACE_H);
    cmd_dword(g_fb_res_id);
    cmd_dword(PIPE_FORMAT_B8G8R8A8_UNORM);
    cmd_dword(0);  /* first_layer=0 | last_layer=0<<16 */
    cmd_dword(0);  /* level=0 */
    cmd_submit();

    /* Create vertex buffer resource: PIPE_BUFFER, 256 bytes */
    g_vb_res_id = sys_gpu_alloc_res_id();
    if (sys_gpu_res_create_3d(g_vb_res_id, PIPE_BUFFER, 0,
                              VIRGL_BIND_VERTEX_BUFFER, 256, 1, 1, 1, 0, 0, 0) != 0) {
        /* If 3D resource creation fails, compositing won't work */
    }
    sys_gpu_ctx_attach(GPU_CTX_ID, g_vb_res_id);

    g_initialized = 1;
    g_active = 1;
    return 1;
}

void gpu_comp_upload_surface(int surf_idx, uint32_t *pixels,
                             uint32_t w, uint32_t h,
                             uint32_t dirty_x, uint32_t dirty_y,
                             uint32_t dirty_w, uint32_t dirty_h) {
    if (!g_active || surf_idx < 0 || surf_idx >= MAX_GPU_SURFACES) return;
    if (w == 0 || h == 0 || !pixels) return;

    gpu_surface_t *gs = &g_surfaces[surf_idx];

    /* The shared buffer is laid out with SURF_W stride (2560 pixels),
     * but the actual surface may be smaller. We create the GPU resource
     * with the actual surface dimensions and transfer the full surface. */
    uint32_t buf_w = w;
    uint32_t buf_h = h;

    if (!gs->active) {
        gs->resource_id = sys_gpu_alloc_res_id();
        if (sys_gpu_res_create_2d(gs->resource_id, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,
                                  buf_w, buf_h) != 0) {
            return;
        }

        /* Attach backing memory: the surface's shared memory pages.
         * The kernel walks page tables to get physical addresses.
         * We only attach enough pages for the actual surface data. */
        uint64_t buf_vaddr = (uint64_t)pixels;
        uint32_t buf_size = buf_w * buf_h * 4;
        uint32_t npages = (buf_size + PAGE_SIZE - 1) / PAGE_SIZE;

        if (sys_gpu_res_attach_virt(gs->resource_id, buf_vaddr, npages,
                                    (uint64_t)buf_w * buf_h * 4) != 0) {
            sys_gpu_res_unref(gs->resource_id);
            return;
        }

        /* Attach texture resource to the virgl context so it can be sampled */
        sys_gpu_ctx_attach(GPU_CTX_ID, gs->resource_id);

        /* Create a sampler view object for this texture resource */
        uint32_t sv_handle = 100 + surf_idx;
        cmd_reset();
        /* SAMPLER_VIEW: len = 6 (handle + res_handle + format + first_elem + last_elem + swizzle) */
        cmd_dword(VIRGL_CMD0(VCCMD_CREATE_OBJECT, VOBJ_SAMPLER_VIEW, 6));
        cmd_dword(sv_handle);           /* handle */
        cmd_dword(gs->resource_id);     /* res_handle */
        cmd_dword(PIPE_FORMAT_B8G8R8A8_UNORM);  /* format */
        cmd_dword(0);                   /* first_element / texture_level */
        cmd_dword(0);                   /* last_element / texture_layers */
        cmd_dword(0);                   /* swizzle (identity = 0) */
        cmd_submit();

        gs->active = 1;
        gs->w = buf_w;
        gs->h = buf_h;
        gs->vaddr = buf_vaddr;
        gs->npages = npages;
    }

    /* Transfer the full resource to the GPU.
     * The surface data in the shared buffer is at offset 0 and is
     * buf_w * buf_h * 4 bytes contiguous (first w*h pixels of the slot). */
    (void)dirty_x; (void)dirty_y; (void)dirty_w; (void)dirty_h;
    sys_gpu_transfer_2d(gs->resource_id, 0, 0, gs->w, gs->h, 0);
}

void gpu_comp_composite(int32_t fb_w, int32_t fb_h,
                        gpu_comp_surf_info_t *surfs, int nsurf) {
    if (!g_active || !g_initialized) return;

    cmd_reset();

    /* Set framebuffer state: bind FB_RES_ID as color target 0 */
    /* SET_FRAMEBUFFER_STATE: cmd + nr_cbufs + zsurf_handle + cbuf_handle */
    cmd_dword(VIRGL_CMD0(VCCMD_SET_FRAMEBUFFER_STATE, 0, 3));
    cmd_dword(1);             /* nr_cbufs */
    cmd_dword(0);             /* zsurf_handle (none) */
    cmd_dword(FB_SURFACE_H);  /* cbuf[0] = framebuffer surface object */

    /* Set viewport: map screen coords to NDC.
     * scale = (fb_w/2, fb_h/2, 0.5), translate = (fb_w/2, fb_h/2, 0.5) */
    /* SET_VIEWPORT_STATE: cmd + start_slot + 3 scale floats + 3 translate floats */
    cmd_dword(VIRGL_CMD0(VCCMD_SET_VIEWPORT_STATE, 0, 7));
    cmd_dword(0);  /* start_slot */
    cmd_float((float)fb_w / 2.0f);
    cmd_float((float)fb_h / 2.0f);
    cmd_float(0.5f);
    cmd_float((float)fb_w / 2.0f);
    cmd_float((float)fb_h / 2.0f);
    cmd_float(0.5f);

    /* Clear the framebuffer to transparent black */
    /* CLEAR: cmd + buffers + 4 color dwords + depth(double) + stencil */
    cmd_dword(VIRGL_CMD0(VCCMD_CLEAR, 0, 8));
    cmd_dword(0x2);  /* PIPE_CLEAR_COLOR0 = bit 1 */
    cmd_dword(0); cmd_dword(0); cmd_dword(0); cmd_dword(0);  /* color RGBA = 0 */
    cmd_dword(0); cmd_dword(0);  /* depth (double as 2 dwords) */
    cmd_dword(0);  /* stencil */

    /* Draw each visible surface as a textured quad */
    for (int i = 0; i < nsurf && i < MAX_GPU_SURFACES; i++) {
        if (!surfs[i].valid || surfs[i].hidden) continue;
        gpu_surface_t *gs = &g_surfaces[i];
        if (!gs->active) continue;

        float sx = (float)surfs[i].x;
        float sy = (float)surfs[i].y;
        float sw = (float)surfs[i].w;
        float sh = (float)surfs[i].h;

        /* Vertex data: 4 vertices, each (pos.xy, uv.xy) = 4 floats = 16 bytes
         * Positions in screen pixels (viewport maps to NDC).
         * UVs: (0,0) top-left, (1,1) bottom-right.
         * Triangle strip: v0=top-left, v1=bottom-left, v2=top-right, v3=bottom-right */
        float verts[4][4] = {
            { sx,       sy,       0.0f, 0.0f },  /* top-left */
            { sx,       sy + sh,  0.0f, 1.0f },  /* bottom-left */
            { sx + sw,  sy,       1.0f, 0.0f },  /* top-right */
            { sx + sw,  sy + sh,  1.0f, 1.0f },  /* bottom-right */
        };

        /* Inline-write vertex data to VB_RES_ID, then set vertex buffer */
        /* RESOURCE_INLINE_WRITE: cmd + res_handle + level + usage + stride
         *   + layer_stride + x + y + z + w + h + d + data... */
        uint32_t vb_size = 4 * 4 * 4;  /* 4 vertices * 4 floats * 4 bytes = 64 */
        uint32_t iw_hdr = 11;  /* header dwords before data */
        uint32_t iw_len = iw_hdr + vb_size / 4;
        cmd_dword(VIRGL_CMD0(VCCMD_RESOURCE_INLINE_WRITE, 0, iw_len));
        cmd_dword(g_vb_res_id);  /* res_handle */
        cmd_dword(0);          /* level */
        cmd_dword(0);          /* usage */
        cmd_dword(16);         /* stride (bytes per vertex) */
        cmd_dword(0);          /* layer_stride */
        cmd_dword(0);          /* x (offset) */
        cmd_dword(0);          /* y */
        cmd_dword(0);          /* z */
        cmd_dword(vb_size);    /* w (data size) */
        cmd_dword(1);          /* h */
        cmd_dword(1);          /* d */
        /* Vertex data */
        for (int v = 0; v < 4; v++) {
            cmd_float(verts[v][0]);
            cmd_float(verts[v][1]);
            cmd_float(verts[v][2]);
            cmd_float(verts[v][3]);
        }

        /* SET_VERTEX_BUFFERS: cmd + stride + offset + handle (1 buffer) */
        cmd_dword(VIRGL_CMD0(VCCMD_SET_VERTEX_BUFFERS, 0, 3));
        cmd_dword(16);         /* stride */
        cmd_dword(0);          /* offset */
        cmd_dword(g_vb_res_id);  /* handle */

        /* Set sampler view: bind surface texture to fragment shader slot 0.
         * We create a sampler view object per surface that references the texture resource. */
        /* SET_SAMPLER_VIEWS: len = shader_type + start_slot + handle = 3 */
        uint32_t sv_handle = 100 + i;  /* sampler view handle for surface i */
        cmd_dword(VIRGL_CMD0(VCCMD_SET_SAMPLER_VIEWS, 0, 3));
        cmd_dword(PIPE_SHADER_FRAGMENT);  /* shader_type */
        cmd_dword(0);                     /* start_slot */
        cmd_dword(sv_handle);             /* sampler view handle */

        /* Draw VBO: 4 vertices, triangle strip, non-indexed */
        /* DRAW_VBO: cmd + start + count + mode + indexed + instance_count + ... */
        cmd_dword(VIRGL_CMD0(VCCMD_DRAW_VBO, 0, 12));
        cmd_dword(0);  /* start */
        cmd_dword(4);  /* count */
        cmd_dword(PIPE_PRIM_TRIANGLE_STRIP);  /* mode */
        cmd_dword(0);  /* indexed */
        cmd_dword(1);  /* instance_count */
        cmd_dword(0);  /* index_bias */
        cmd_dword(0);  /* start_instance */
        cmd_dword(0);  /* primitive_restart */
        cmd_dword(0);  /* restart_index */
        cmd_dword(0);  /* min_index */
        cmd_dword(0xFFFFFFFF);  /* max_index */
        cmd_dword(0);  /* count_from_so */
    }

    /* Flush the framebuffer resource to scanout */
    sys_gpu_flush_res(g_fb_res_id, 0, 0, fb_w, fb_h);

    cmd_submit();
}

void gpu_comp_destroy_surface(int surf_idx) {
    if (!g_active || surf_idx < 0 || surf_idx >= MAX_GPU_SURFACES) return;
    gpu_surface_t *gs = &g_surfaces[surf_idx];
    if (!gs->active) return;
    sys_gpu_res_unref(gs->resource_id);
    gs->active = 0;
    gs->resource_id = 0;
    gs->vaddr = 0;
    gs->npages = 0;
}

int gpu_comp_active(void) {
    return g_active;
}
