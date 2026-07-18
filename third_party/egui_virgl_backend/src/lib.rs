//! egui → GPU texture backend for X OS (VirGL / virtio-gpu).
//!
//! egui tessellation stays on the CPU; pixels are uploaded into a VirGL
//! texture via TRANSFER_TO_HOST. The compositor samples that texture with
//! a GPU textured quad — so the menu stays on the GPU compositing path
//! (not a CPU shared-memory surface like the dock).

#![no_std]
#![allow(dead_code)]

extern crate alloc;

mod runtime;

use alloc::boxed::Box;

use egui::TexturesDelta;
use egui_software_backend::{BufferMutRef, ColorFieldOrder, EguiSoftwareRender};
use epaint::ClippedPrimitive;

// ---- Syscall numbers (must match kernel/include/syscall.h) ----
const SYS_GPU_VIRGL_PRESENT: u64 = 36;
const SYS_GPU_CTX_CREATE: u64 = 37;
const SYS_GPU_CTX_DESTROY: u64 = 38;
const SYS_GPU_CTX_ATTACH: u64 = 39;
const SYS_GPU_FLUSH_RES: u64 = 46;
const SYS_GPU_ALLOC_RES_ID: u64 = 47;
const SYS_GPU_RES_ATTACH_VIRT: u64 = 48;
const SYS_GPU_RES_CREATE_3D: u64 = 49;
const SYS_GPU_TRANSFER_3D: u64 = 64;

unsafe fn syscall0(num: u64) -> u64 {
    let ret: u64;
    unsafe {
        core::arch::asm!(
            "syscall",
            lateout("rax") ret,
            in("rax") num,
            out("rcx") _,
            out("r11") _,
        );
    }
    ret
}

unsafe fn syscall1(num: u64, a1: u64) -> u64 {
    let ret: u64;
    unsafe {
        core::arch::asm!(
            "syscall",
            lateout("rax") ret,
            in("rax") num,
            in("rdi") a1,
            out("rcx") _,
            out("r11") _,
            out("rsi") _,
            out("rdx") _,
            out("r8") _,
            out("r9") _,
            out("r10") _,
        );
    }
    ret
}

unsafe fn syscall2(num: u64, a1: u64, a2: u64) -> u64 {
    let ret: u64;
    unsafe {
        core::arch::asm!(
            "syscall",
            lateout("rax") ret,
            in("rax") num,
            in("rdi") a1,
            in("rsi") a2,
            out("rcx") _,
            out("r11") _,
            out("rdx") _,
            out("r8") _,
            out("r9") _,
            out("r10") _,
        );
    }
    ret
}

unsafe fn syscall4(num: u64, a1: u64, a2: u64, a3: u64, a4: u64) -> u64 {
    let ret: u64;
    unsafe {
        core::arch::asm!(
            "syscall",
            lateout("rax") ret,
            in("rax") num,
            in("rdi") a1,
            in("rsi") a2,
            in("rdx") a3,
            in("r10") a4,
            out("rcx") _,
            out("r11") _,
            out("r8") _,
            out("r9") _,
        );
    }
    ret
}

unsafe fn syscall6(num: u64, a1: u64, a2: u64, a3: u64, a4: u64, a5: u64, a6: u64) -> u64 {
    let ret: u64;
    unsafe {
        core::arch::asm!(
            "syscall",
            lateout("rax") ret,
            in("rax") num,
            in("rdi") a1,
            in("rsi") a2,
            in("rdx") a3,
            in("r10") a4,
            in("r8") a5,
            in("r9") a6,
            out("rcx") _,
            out("r11") _,
        );
    }
    ret
}

fn sys_gpu_virgl_present() -> bool {
    unsafe { syscall0(SYS_GPU_VIRGL_PRESENT) != 0 }
}

fn sys_gpu_ctx_create(ctx_id: u32) -> bool {
    unsafe { syscall1(SYS_GPU_CTX_CREATE, ctx_id as u64) == 0 }
}

fn sys_gpu_ctx_destroy(ctx_id: u32) -> bool {
    unsafe { syscall1(SYS_GPU_CTX_DESTROY, ctx_id as u64) == 0 }
}

fn sys_gpu_ctx_attach(ctx_id: u32, resource_id: u32) -> bool {
    unsafe { syscall2(SYS_GPU_CTX_ATTACH, ctx_id as u64, resource_id as u64) == 0 }
}

fn sys_gpu_alloc_res_id() -> u32 {
    unsafe { syscall0(SYS_GPU_ALLOC_RES_ID) as u32 }
}

fn sys_gpu_res_create_3d(
    resource_id: u32,
    target: u32,
    format: u32,
    bind: u32,
    width: u32,
    height: u32,
) -> bool {
    unsafe {
        syscall6(
            SYS_GPU_RES_CREATE_3D,
            resource_id as u64,
            target as u64,
            format as u64,
            bind as u64,
            width as u64,
            height as u64,
        ) == 0
    }
}

fn sys_gpu_res_attach_virt(resource_id: u32, vaddr: u64, npages: u32, buf_size: u64) -> bool {
    unsafe {
        syscall4(
            SYS_GPU_RES_ATTACH_VIRT,
            resource_id as u64,
            vaddr,
            npages as u64,
            buf_size,
        ) == 0
    }
}

/// TRANSFER_TO_HOST_3D — guest backing → host GPU texture.
/// Syscall args: (res, x, y, z, w, h); stride computed in kernel.
fn sys_gpu_transfer_3d(resource_id: u32, x: u32, y: u32, z: u32, w: u32, h: u32) -> bool {
    unsafe {
        syscall6(
            SYS_GPU_TRANSFER_3D,
            resource_id as u64,
            x as u64,
            y as u64,
            z as u64,
            w as u64,
            h as u64,
        ) == 0
    }
}

fn sys_gpu_flush_res(resource_id: u32, x: u32, y: u32, w: u32, h: u32) -> bool {
    unsafe {
        syscall6(
            SYS_GPU_FLUSH_RES,
            resource_id as u64,
            x as u64,
            y as u64,
            w as u64,
            h as u64,
            0,
        ) == 0
    }
}

const PIPE_TEXTURE_2D: u32 = 2;
const PIPE_FORMAT_R8G8B8A8_UNORM: u32 = 67;
const VIRGL_BIND_SAMPLER_VIEW: u32 = 1 << 3;

/// GPU-backed egui surface for the compositor to sample.
pub struct EguiVirglBackend {
    ctx_id: u32,
    rt_res_id: u32,
    sw: EguiSoftwareRender,
    initialized: bool,
    width: u32,
    height: u32,
    /// Page-aligned guest buffer (from the C shim) attached as texture backing.
    pix_mem: *mut u8,
    pix_mem_size: usize,
    _vb_mem: *mut u8,
    _vb_mem_size: usize,
    _ib_mem: *mut u8,
    _ib_mem_size: usize,
}

impl EguiVirglBackend {
    pub fn new(
        ctx_id: u32,
        width: u32,
        height: u32,
        vb_mem: *mut u8,
        vb_mem_size: usize,
        ib_mem: *mut u8,
        ib_mem_size: usize,
        tex_mem: *mut u8,
        tex_mem_size: usize,
    ) -> Self {
        Self {
            ctx_id,
            rt_res_id: 0,
            // Compositor sampler view is R8G8B8A8 with identity swizzle.
            // Caching blits intermediate tiles and can leave opaque garbage in
            // unused regions of the surface — disable it for overlay menus.
            sw: EguiSoftwareRender::new(ColorFieldOrder::Rgba).with_caching(false),
            initialized: false,
            width,
            height,
            pix_mem: tex_mem,
            pix_mem_size: tex_mem_size,
            _vb_mem: vb_mem,
            _vb_mem_size: vb_mem_size,
            _ib_mem: ib_mem,
            _ib_mem_size: ib_mem_size,
        }
    }

    fn pixel_bytes(&self) -> usize {
        (self.width as usize) * (self.height as usize) * 4
    }

    /// Create VirGL context + sampler texture with guest backing.
    pub fn init(&mut self) -> bool {
        if self.initialized {
            return true;
        }
        if !sys_gpu_virgl_present() {
            return false;
        }
        let nbytes = self.pixel_bytes();
        if self.pix_mem.is_null() || nbytes == 0 || nbytes > self.pix_mem_size {
            return false;
        }
        if !sys_gpu_ctx_create(self.ctx_id) {
            return false;
        }

        self.rt_res_id = sys_gpu_alloc_res_id();
        if !sys_gpu_res_create_3d(
            self.rt_res_id,
            PIPE_TEXTURE_2D,
            PIPE_FORMAT_R8G8B8A8_UNORM,
            VIRGL_BIND_SAMPLER_VIEW,
            self.width,
            self.height,
        ) {
            return false;
        }

        let npages = ((nbytes as u64 + 4095) / 4096) as u32;
        if !sys_gpu_res_attach_virt(self.rt_res_id, self.pix_mem as u64, npages, nbytes as u64)
        {
            return false;
        }
        sys_gpu_ctx_attach(self.ctx_id, self.rt_res_id);

        // Clear + upload so the compositor never samples uninitialized memory
        // (that produced the dark-blue diamond / X artifact).
        let w = self.width as usize;
        let h = self.height as usize;
        let pixels =
            unsafe { core::slice::from_raw_parts_mut(self.pix_mem as *mut [u8; 4], w * h) };
        for px in pixels.iter_mut() {
            *px = [0, 0, 0, 0];
        }
        sys_gpu_transfer_3d(self.rt_res_id, 0, 0, 0, self.width, self.height);
        sys_gpu_flush_res(self.rt_res_id, 0, 0, self.width, self.height);

        self.initialized = true;
        true
    }

    pub fn render_target_id(&self) -> u32 {
        self.rt_res_id
    }

    pub fn context_id(&self) -> u32 {
        self.ctx_id
    }

    /// Bounding box of opaque pixels (alpha > 8), padded for AA / radii.
    /// Used so the compositor can crop the overlay quad to menu content.
    pub fn measure_content_size(&self) -> (u32, u32) {
        let w = self.width as usize;
        let h = self.height as usize;
        let nbytes = self.pixel_bytes();
        if self.pix_mem.is_null() || nbytes == 0 || w == 0 || h == 0 {
            return (1, 1);
        }
        let pixels =
            unsafe { core::slice::from_raw_parts(self.pix_mem as *const [u8; 4], w * h) };
        let mut max_x = 0usize;
        let mut max_y = 0usize;
        let mut any = false;
        for y in 0..h {
            let row = y * w;
            for x in 0..w {
                if pixels[row + x][3] > 8 {
                    any = true;
                    if x + 1 > max_x {
                        max_x = x + 1;
                    }
                    if y + 1 > max_y {
                        max_y = y + 1;
                    }
                }
            }
        }
        if !any {
            return (1, 1);
        }
        let cw = (max_x + 6).min(w) as u32;
        let ch = (max_y + 6).min(h) as u32;
        (cw.max(1), ch.max(1))
    }

    /// Rasterize egui into the guest buffer and upload to the GPU texture.
    pub fn render(
        &mut self,
        paint_jobs: &[ClippedPrimitive],
        textures_delta: &TexturesDelta,
        pixels_per_point: f32,
    ) {
        if !self.initialized {
            return;
        }

        let w = self.width as usize;
        let h = self.height as usize;
        let nbytes = self.pixel_bytes();
        if self.pix_mem.is_null() || nbytes == 0 || nbytes > self.pix_mem_size {
            return;
        }
        let pixels =
            unsafe { core::slice::from_raw_parts_mut(self.pix_mem as *mut [u8; 4], w * h) };

        /* Transparent clear (A=0). Unused padding must not paint a dark plate —
         * the compositor keeps L1/L2 in the scanout and overlay_fast blits only
         * the opaque egui pixels. */
        unsafe {
            core::ptr::write_bytes(self.pix_mem, 0, nbytes);
        }

        {
            let mut buf = BufferMutRef::new(pixels, w, h);
            self.sw
                .render(&mut buf, paint_jobs, textures_delta, pixels_per_point);
        }

        /* Upload into the sampler texture. Do NOT flush here — res is not
         * scanout; the compositor presents the overlay layer onto the FB. */
        sys_gpu_transfer_3d(self.rt_res_id, 0, 0, 0, self.width, self.height);
    }

    pub fn destroy(&mut self) {
        if !self.initialized {
            return;
        }
        sys_gpu_ctx_destroy(self.ctx_id);
        self.initialized = false;
    }
}

// ---- C FFI ----

#[repr(C)]
pub struct EguiGpuState {
    backend: *mut EguiVirglBackend,
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_egui_gpu_init(
    ctx_id: u32,
    width: u32,
    height: u32,
    vb_mem: *mut u8,
    vb_mem_size: usize,
    ib_mem: *mut u8,
    ib_mem_size: usize,
    tex_mem: *mut u8,
    tex_mem_size: usize,
) -> *mut EguiGpuState {
    let mut backend = Box::new(EguiVirglBackend::new(
        ctx_id,
        width,
        height,
        vb_mem,
        vb_mem_size,
        ib_mem,
        ib_mem_size,
        tex_mem,
        tex_mem_size,
    ));
    if !backend.init() {
        return core::ptr::null_mut();
    }
    Box::into_raw(Box::new(EguiGpuState {
        backend: Box::into_raw(backend),
    }))
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_egui_gpu_render_target_id(state: *mut EguiGpuState) -> u32 {
    if state.is_null() {
        return 0;
    }
    unsafe {
        let s = &*state;
        if s.backend.is_null() {
            return 0;
        }
        (*s.backend).render_target_id()
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_egui_gpu_destroy(state: *mut EguiGpuState) {
    if state.is_null() {
        return;
    }
    unsafe {
        let s = Box::from_raw(state);
        if !s.backend.is_null() {
            let mut backend = Box::from_raw(s.backend);
            backend.destroy();
        }
    }
}
