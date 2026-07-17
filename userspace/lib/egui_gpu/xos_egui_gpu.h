#ifndef XOS_EGUI_GPU_H
#define XOS_EGUI_GPU_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque backend state */
typedef struct EguiGpuState EguiGpuState;

/* Initialize the egui GPU rendering backend.
 *
 * ctx_id:       Unique virgl context ID (use 2+ to avoid conflict
 *               with the compositor's context 1).
 * width/height: Surface dimensions in pixels.
 * vb_mem:       Shared memory buffer for vertex data uploads.
 * vb_mem_size:  Size of vb_mem in bytes (e.g. 256KB).
 * ib_mem:       Shared memory buffer for index data uploads.
 * ib_mem_size:  Size of ib_mem in bytes (e.g. 128KB).
 * tex_mem:      Shared memory buffer for texture data uploads.
 * tex_mem_size: Size of tex_mem in bytes (e.g. 1MB).
 *
 * The caller must allocate the shared memory buffers before calling
 * this function. They are used to upload vertex/index/texture data
 * to the GPU via virtio-gpu resource attach backing.
 *
 * Returns NULL on failure (virgl not available, context creation failed).
 */
EguiGpuState *xos_egui_gpu_init(
    uint32_t ctx_id,
    uint32_t width,
    uint32_t height,
    void *vb_mem,    size_t vb_mem_size,
    void *ib_mem,    size_t ib_mem_size,
    void *tex_mem,   size_t tex_mem_size
);

/* Destroy the GPU backend and free resources. */
void xos_egui_gpu_destroy(EguiGpuState *state);

/* Get the render target resource ID.
 * The compositor uses this to composite the surface. */
uint32_t xos_egui_gpu_render_target_id(EguiGpuState *state);

/* Get the virgl context ID. */
uint32_t xos_egui_gpu_context_id(EguiGpuState *state);

/* Render a frame. This is called after the platform layer has
 * tessellated egui shapes and updated textures. The render target
 * will contain the rendered frame after this call.
 *
 * The actual rendering API is called from the Rust platform layer
 * which has direct access to the EguiVirglBackend struct.
 *
 * Returns 1 on success, 0 on failure. */
uint32_t xos_egui_gpu_render_frame(EguiGpuState *state);

#ifdef __cplusplus
}
#endif

#endif /* XOS_EGUI_GPU_H */
