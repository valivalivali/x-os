#pragma once
#include <stdint.h>

/* GPU compositing using virgl (OpenGL ES via virtio-gpu 3D).
 *
 * Architecture:
 * - Each app surface gets a virtio-gpu 2D resource (texture) backed by
 *   the app's shared memory buffer.
 * - The compositor creates a render target (FBO) backed by a 2D resource
 *   that is also the scanout source.
 * - Each frame: transfer dirty surfaces to GPU, then submit a 3D command
 *   stream that binds the FBO, clears it to desktop bg, and draws textured
 *   quads for each surface with alpha blending.
 *
 * Virgl command stream format:
 * Commands are arrays of uint32_t words. Each command starts with a
 * virgl_context_cmd header word. The virglrenderer interprets these as
 * Gallium pipe_context operations. */

#define GPU_CTX_ID       1
#define GPU_FB_RES_ID    1   /* scanout + render target resource */

/* Virgl command opcodes (from virglrenderer enum virgl_context_cmd) */
#define VIRGL_CCMD_NOP                   0
#define VIRGL_CCMD_CREATE_OBJECT         1
#define VIRGL_CCMD_BIND_OBJECT           2
#define VIRGL_CCMD_DESTROY_OBJECT        3
#define VIRGL_CCMD_SET_VIEWPORT_STATE    4
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE 5
#define VIRGL_CCMD_SET_VERTEX_BUFFERS    6
#define VIRGL_CCMD_CLEAR                 7
#define VIRGL_CCMD_DRAW_VBO              8
#define VIRGL_CCMD_RESOURCE_INLINE_WRITE 9
#define VIRGL_CCMD_SET_SAMPLER_VIEWS     10
#define VIRGL_CCMD_SET_INDEX_BUFFER      11
#define VIRGL_CCMD_SET_CONSTANT_BUFFER   12
#define VIRGL_CCMD_SET_STENCIL_REF       13
#define VIRGL_CCMD_SET_BLEND_COLOR       14
#define VIRGL_CCMD_SET_SCISSOR_STATE     15
#define VIRGL_CCMD_BLIT                  16
#define VIRGL_CCMD_RESOURCE_COPY_REGION  17
#define VIRGL_CCMD_BIND_SAMPLER_STATES   18
#define VIRGL_CCMD_BEGIN_QUERY           19
#define VIRGL_CCMD_END_QUERY             20
#define VIRGL_CCMD_GET_QUERY_RESULT      21
#define VIRGL_CCMD_SET_POLYGON_STIPPLE   22
#define VIRGL_CCMD_SET_CLIP_STATE        23
#define VIRGL_CCMD_SET_SAMPLE_MASK       24
#define VIRGL_CCMD_SET_STREAMOUT_TARGETS 25
#define VIRGL_CCMD_SET_RENDER_CONDITION  26
#define VIRGL_CCMD_SET_UNIFORM_BUFFER    27
#define VIRGL_CCMD_SET_SUB_CTX           28
#define VIRGL_CCMD_CREATE_SUB_CTX        29
#define VIRGL_CCMD_DESTROY_SUB_CTX       30
#define VIRGL_CCMD_BIND_SHADER           31
#define VIRGL_CCMD_SET_TESS_STATE        32
#define VIRGL_CCMD_SET_MIN_SAMPLES       33
#define VIRGL_CCMD_SET_SHADER_BUFFERS    34
#define VIRGL_CCMD_SET_SHADER_IMAGES     35
#define VIRGL_CCMD_MEMORY_BARRIER        36
#define VIRGL_CCMD_LAUNCH_GRID           37

/* Object types for CREATE_OBJECT (from virglrenderer enum virgl_object_type) */
#define VIRGL_OBJECT_BLEND           1
#define VIRGL_OBJECT_RASTERIZER      2
#define VIRGL_OBJECT_DSA             3
#define VIRGL_OBJECT_SHADER          4
#define VIRGL_OBJECT_VERTEX_ELEMENTS 5
#define VIRGL_OBJECT_SAMPLER_VIEW    6
#define VIRGL_OBJECT_SAMPLER_STATE   7
#define VIRGL_OBJECT_SURFACE         8
#define VIRGL_OBJECT_QUERY           9
#define VIRGL_OBJECT_STREAMOUT_TARGET 10
#define VIRGL_OBJECT_MSAA_SURFACE    11

/* Pipe shader types */
#define PIPE_SHADER_VERTEX   0
#define PIPE_SHADER_FRAGMENT 1

/* Pipe texture targets */
#define PIPE_TEXTURE_2D      2

/* Pipe bind flags */
#define PIPE_BIND_RENDER_TARGET  (1 << 1)
#define PIPE_BIND_SAMPLER_VIEW   (1 << 3)
#define PIPE_BIND_DISPLAY_TARGET (1 << 7)
#define PIPE_BIND_SCANOUT        (1 << 9)

/* Pipe resource formats (subset) */
#define PIPE_FORMAT_B8G8R8A8_UNORM  1
#define PIPE_FORMAT_B8G8R8X8_UNORM  2
#define PIPE_FORMAT_R8G8B8A8_UNORM  67

/* Virgl resource handle types */
#define VIRGL_RESOURCE_2D   1
#define VIRGL_RESOURCE_3D   2

/* Initialize GPU compositing context.
 * Returns 1 on success, 0 if virgl not available. */
int gpu_comp_init(int32_t fb_w, int32_t fb_h, uint64_t fb_phys, uint64_t fb_size,
                  uint32_t *bg_pixels, int32_t bg_stride);

/* Upload a surface's pixel data to its GPU texture.
 * Creates the texture resource on first call. */
void gpu_comp_upload_surface(int surf_idx, uint32_t *pixels,
                             uint32_t w, uint32_t h,
                             uint32_t dirty_x, uint32_t dirty_y,
                             uint32_t dirty_w, uint32_t dirty_h);

/* Composite all surfaces to the framebuffer using GPU.
 * Draws textured quads for each surface at its screen position.
 * surfs array provides position and visibility for each surface slot. */
typedef struct {
    int32_t  x, y;
    uint32_t w, h;
    int      valid;
    int      hidden;
} gpu_comp_surf_info_t;

void gpu_comp_composite(int32_t fb_w, int32_t fb_h,
                        gpu_comp_surf_info_t *surfs, int nsurf);

/* Cleanup a surface's GPU resources. */
void gpu_comp_destroy_surface(int surf_idx);

/* Create a sampler view for a GPU-backed surface (app-rendered render target).
 * Called when an app sends WM_SURFACE_GPU_READY with its resource ID. */
void gpu_comp_create_gpu_surface_sv(int surf_idx, uint32_t res_id,
                                     uint32_t sv_handle);

/* Destroy a compositor-side sampler view (call before the app frees its RT). */
void gpu_comp_destroy_gpu_surface_sv(uint32_t sv_handle);

/* Composite a GPU-backed surface directly (no CPU pixel upload needed).
 * Uses the sampler view created by gpu_comp_create_gpu_surface_sv. */
void gpu_comp_composite_gpu_surface(int surf_idx, uint32_t sv_handle,
                                     int32_t x, int32_t y,
                                     uint32_t w, uint32_t h);

/* Re-transfer the backing buffer to the background texture.
 * Call after CPU rendering (draw_region) updates the backing buffer. */
void gpu_comp_update_bg(uint32_t *bg_pixels, int32_t fb_w, int32_t fb_h);

/* GPU surface info for batch compositing. */
typedef struct {
    uint32_t sv_handle;
    int32_t  x, y;
    uint32_t w, h;       /* on-screen quad (may be smaller than texture) */
    uint32_t tex_w, tex_h; /* GPU texture size; 0 = same as w/h */
} gpu_comp_gpu_surf_t;

/* Composite layers onto scanout.
 *
 *   L1 DESKTOP+L2/L3 panels — CPU backing, transferred when they change.
 *   L3 OVERLAY — GPU textured quads (menus).
 *   L4 cursor — hardware plane (not here).
 *
 * overlay_fast=1: do NOT re-transfer L1; just draw OVERLAY quads on the
 * cached scanout. Use when only the menu texture changed.
 * overlay_fast=0: transfer dirty rect from backing (L1–L3), then draw quads.
 * Pass dw/dh <= 0 for a full-FB transfer. */
void gpu_comp_present(int32_t fb_w, int32_t fb_h,
                      gpu_comp_gpu_surf_t *gpu_surfs, int ngpu,
                      int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                      int overlay_fast);

/* Check if GPU compositing is active. */
int gpu_comp_active(void);

/* Re-transfer corner mask pixels from backing to the 3D fb resource.
 * Only transfers pixels OUTSIDE the rounded rect (the actual mask area),
 * preserving GPU-rendered content inside the circle.
 * x0,y0 = window origin; w = window width; total_h = full window height; r = radius */
void gpu_comp_mask_corners(int32_t x0, int32_t y0, int32_t w, int32_t total_h, int32_t r);
