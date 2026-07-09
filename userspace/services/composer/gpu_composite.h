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

/* Virgl command opcodes (from virglrenderer gallium commands) */
#define VIRGL_CCMD_NOP                   0
#define VIRGL_CCMD_CREATE_OBJECT         1
#define VIRGL_CCMD_DESTROY_OBJECT        2
#define VIRGL_CCMD_SET_VIEWPORT_STATE    3
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE 4
#define VIRGL_CCMD_SET_VERTEX_BUFFERS    5
#define VIRGL_CCMD_SET_VERTEX_ELEMENTS   6
#define VIRGL_CCMD_DRAW_VBO              7
#define VIRGL_CCMD_RESOURCE_COPY_REGION  8
#define VIRGL_CCMD_BIND_SAMPLER_STATES   9
#define VIRGL_CCMD_BIND_VERTEX_SHADER    10
#define VIRGL_CCMD_BIND_FRAGMENT_SHADER  11
#define VIRGL_CCMD_SET_SAMPLER_VIEWS     14
#define VIRGL_CCMD_SET_RENDER_TARGET     15
#define VIRGL_CCMD_SET_BLEND_STATE       17
#define VIRGL_CCMD_SET_VERTEX_ELEMENTS2  18
#define VIRGL_CCMD_SET_STREAMOUT_TARGETS 20
#define VIRGL_CCMD_BEGIN_QUERY           21
#define VIRGL_CCMD_END_QUERY             22
#define VIRGL_CCMD_GET_QUERY_RESULT      23
#define VIRGL_CCMD_SET_INDEX_BUFFER      24
#define VIRGL_CCMD_SET_UNIFORM_BUFFER    27
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE2 30
#define VIRGL_CCMD_SET_SHADER_BUFFERS    31
#define VIRGL_CCMD_SET_SHADER_IMAGES     32
#define VIRGL_CCMD_SET_SHADER_RESOURCES  33
#define VIRGL_CCMD_MEMORY_BARRIER        34
#define VIRGL_CCMD_SET_SAMPLE_MASK       35
#define VIRGL_CCMD_SET_STREAMOUT_TARGETS2 36
#define VIRGL_CCMD_SET_RENDER_CONDITION  37
#define VIRGL_CCMD_SET_TESS_STATE        38
#define VIRGL_CCMD_SET_SUB_CTX           41
#define VIRGL_CCMD_CREATE_SUB_CTX        42
#define VIRGL_CCMD_DESTROY_SUB_CTX       43
#define VIRGL_CCMD_BIND_SHADER           44
#define VIRGL_CCMD_SET_VERTEX_BUFFERS2   46
#define VIRGL_CCMD_SET_SAMPLER_VIEWS2    47

/* Object types for CREATE_OBJECT */
#define VIRGL_OBJECT_BLEND       1
#define VIRGL_OBJECT_RASTERIZER  2
#define VIRGL_OBJECT_DSA         3
#define VIRGL_OBJECT_BLEND_RT    4
#define VIRGL_OBJECT_SAMPLER     5
#define VIRGL_OBJECT_SAMPLER_VIEW 6
#define VIRGL_OBJECT_SURFACE     7
#define VIRGL_OBJECT_QUERY       8
#define VIRGL_OBJECT_SHADER     9
#define VIRGL_OBJECT_VERTEX_ELEMENTS 10
#define VIRGL_OBJECT_VERTEX_BUFFER 11
#define VIRGL_OBJECT_FS         12
#define VIRGL_OBJECT_VS         13
#define VIRGL_OBJECT_GS         14
#define VIRGL_OBJECT_SO_TARGET  15
#define VIRGL_OBJECT_SAMPLER_STATE 16
#define VIRGL_OBJECT_TCS        17
#define VIRGL_OBJECT_TES        18
#define VIRGL_OBJECT_SURFACE2   19

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

/* Check if GPU compositing is active. */
int gpu_comp_active(void);
