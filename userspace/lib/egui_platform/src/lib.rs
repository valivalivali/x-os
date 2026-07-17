//! X OS egui platform layer — shared infrastructure for GPU-accelerated egui apps.
//!
//! This crate provides a reusable platform layer that combines:
//! - egui Context management (creation, input handling, frame lifecycle)
//! - GPU rendering via `egui_virgl_backend` (virglrenderer/virtio-gpu)
//! - A C API for thin C shims to integrate with the WM IPC protocol
//!
//! Apps link against this crate and provide a UI callback closure.
//! The platform layer handles:
//! 1. Creating the egui Context and GPU backend
//! 2. Feeding mouse/keyboard events from IPC into egui
//! 3. Running the egui frame (tessellate shapes)
//! 4. Rendering on the GPU (upload meshes, submit virgl commands)
//! 5. Notifying the compositor to composite the render target

#![no_std]
#![allow(dead_code)]

extern crate alloc;

use alloc::boxed::Box;
use alloc::vec::Vec;

use egui::{
    Context, Event, Modifiers, PointerButton, RawInput, Vec2, TouchPhase,
};
use egui_virgl_backend::EguiVirglBackend;
use epaint::ClippedPrimitive;

/// Platform state shared between C shim and Rust.
#[repr(C)]
pub struct XosEguiPlatform {
    /// egui context
    ctx: *mut Context,
    /// GPU rendering backend
    backend: *mut EguiVirglBackend,
    /// Screen width (surface width)
    width: u32,
    /// Screen height (surface height)
    height: u32,
    /// Pending mouse x
    mouse_x: i32,
    /// Pending mouse y
    mouse_y: i32,
    /// Pending mouse button (0=none, 1=left, 2=right, 3=middle)
    mouse_button: u32,
    /// Pending mouse action (0=move, 1=down, 2=up)
    mouse_action: u32,
    /// Mouse wheel delta (positive = scroll up)
    wheel_delta: f32,
    /// Pending key events (packed as u32: high 16 = key code, low 16 = pressed flag)
    key_events: [u32; 16],
    key_event_count: u32,
    /// Whether the UI has requested a repaint
    needs_repaint: u32,
}

/// Create a new platform state with GPU backend.
///
/// ctx_id: unique virgl context ID (use 2+ to avoid conflict with compositor's ctx 1)
/// width/height: surface dimensions
/// vb_mem/ib_mem/tex_mem: shared memory buffers for GPU data uploads
#[unsafe(no_mangle)]
pub extern "C" fn xos_egui_platform_init(
    ctx_id: u32,
    width: u32,
    height: u32,
    vb_mem: *mut u8,
    vb_mem_size: usize,
    ib_mem: *mut u8,
    ib_mem_size: usize,
    tex_mem: *mut u8,
    tex_mem_size: usize,
) -> *mut XosEguiPlatform {
    let ctx = Context::default();

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

    let state = Box::new(XosEguiPlatform {
        ctx: Box::into_raw(Box::new(ctx)),
        backend: Box::into_raw(backend),
        width,
        height,
        mouse_x: 0,
        mouse_y: 0,
        mouse_button: 0,
        mouse_action: 0,
        wheel_delta: 0.0,
        key_events: [0; 16],
        key_event_count: 0,
        needs_repaint: 1,
    });
    Box::into_raw(state)
}

/// Destroy platform state and free resources.
#[unsafe(no_mangle)]
pub extern "C" fn xos_egui_platform_destroy(state: *mut XosEguiPlatform) {
    if state.is_null() {
        return;
    }
    unsafe {
        let state = Box::from_raw(state);
        if !state.ctx.is_null() {
            let _ = Box::from_raw(state.ctx);
        }
        if !state.backend.is_null() {
            let mut backend = Box::from_raw(state.backend);
            backend.destroy();
        }
    }
}

/// Feed a mouse event.
/// button: 0=none, 1=left(primary), 2=right(secondary), 3=middle
/// action: 0=move, 1=down, 2=up
#[unsafe(no_mangle)]
pub extern "C" fn xos_egui_platform_mouse_event(
    state: *mut XosEguiPlatform,
    x: i32,
    y: i32,
    button: u32,
    action: u32,
) {
    if state.is_null() {
        return;
    }
    unsafe {
        let s = &mut *state;
        s.mouse_x = x;
        s.mouse_y = y;
        s.mouse_button = button;
        s.mouse_action = action;
        s.needs_repaint = 1;
    }
}

/// Feed a mouse wheel/scroll event.
/// delta: positive = scroll up, negative = scroll down
#[unsafe(no_mangle)]
pub extern "C" fn xos_egui_platform_wheel_event(
    state: *mut XosEguiPlatform,
    delta: f32,
) {
    if state.is_null() {
        return;
    }
    unsafe {
        let s = &mut *state;
        s.wheel_delta = delta;
        s.needs_repaint = 1;
    }
}

/// Feed a key event.
/// key: egui key code (see egui::Key)
/// pressed: 1 if key was pressed, 0 if released
#[unsafe(no_mangle)]
pub extern "C" fn xos_egui_platform_key_event(
    state: *mut XosEguiPlatform,
    key: u32,
    pressed: u32,
) {
    if state.is_null() {
        return;
    }
    unsafe {
        let s = &mut *state;
        if (s.key_event_count as usize) < s.key_events.len() {
            s.key_events[s.key_event_count as usize] = (key << 16) | (pressed & 0xFFFF);
            s.key_event_count += 1;
            s.needs_repaint = 1;
        }
    }
}

/// Get the render target resource ID (for compositor to composite).
#[unsafe(no_mangle)]
pub extern "C" fn xos_egui_platform_render_target_id(state: *mut XosEguiPlatform) -> u32 {
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

/// Get the virgl context ID.
#[unsafe(no_mangle)]
pub extern "C" fn xos_egui_platform_context_id(state: *mut XosEguiPlatform) -> u32 {
    if state.is_null() {
        return 0;
    }
    unsafe {
        let s = &*state;
        if s.backend.is_null() {
            return 0;
        }
        (*s.backend).context_id()
    }
}

/// Check if a repaint is needed.
#[unsafe(no_mangle)]
pub extern "C" fn xos_egui_platform_needs_repaint(state: *mut XosEguiPlatform) -> u32 {
    if state.is_null() {
        return 0;
    }
    unsafe { (*state).needs_repaint }
}

/// Run one frame: build input, run egui UI callback, tessellate, render on GPU.
///
/// The `ui_callback` is a C function pointer that receives an egui Ui.
/// In practice, apps will use the Rust API directly via `run_frame_rust`.
///
/// Returns 1 if the frame was rendered, 0 on error.
#[unsafe(no_mangle)]
pub extern "C" fn xos_egui_platform_run_frame(state: *mut XosEguiPlatform) -> u32 {
    if state.is_null() {
        return 0;
    }
    unsafe {
        let s = &mut *state;
        if s.ctx.is_null() || s.backend.is_null() {
            return 0;
        }
        let ctx = &*s.ctx;
        let backend = &mut *s.backend;

        // Build raw input
        let mut input = RawInput::default();
        input.screen_rect = Some(egui::Rect::from_min_size(
            egui::pos2(0.0, 0.0),
            Vec2::new(s.width as f32, s.height as f32),
        ));

        // Mouse move
        let mouse_pos = egui::pos2(s.mouse_x as f32, s.mouse_y as f32);
        input.events.push(Event::PointerMoved(mouse_pos));

        // Mouse button events
        if s.mouse_button != 0 {
            let button = match s.mouse_button {
                1 => PointerButton::Primary,
                2 => PointerButton::Secondary,
                3 => PointerButton::Middle,
                _ => PointerButton::Primary,
            };
            let pressed = s.mouse_action == 1;
            input.events.push(Event::PointerButton {
                pos: mouse_pos,
                button,
                pressed,
                modifiers: Modifiers::default(),
            });
        }

        // Wheel events
        if s.wheel_delta != 0.0 {
            input.events.push(Event::MouseWheel {
                unit: egui::MouseWheelUnit::Point,
                delta: Vec2::new(0.0, s.wheel_delta),
                phase: TouchPhase::Move,
                modifiers: Modifiers::default(),
            });
        }

        // Key events
        for i in 0..s.key_event_count as usize {
            let packed = s.key_events[i];
            let key_code = (packed >> 16) as u32;
            let pressed = (packed & 0xFFFF) != 0;
            // Map key code to egui::Key — apps can extend this
            if let Some(key) = map_key(key_code) {
                input.events.push(Event::Key {
                    key,
                    physical_key: None,
                    pressed,
                    repeat: false,
                    modifiers: Modifiers::default(),
                });
            }
        }

        // Clear pending events
        s.mouse_button = 0;
        s.mouse_action = 0;
        s.wheel_delta = 0.0;
        s.key_event_count = 0;

        // Run egui frame with a dummy UI (apps override via Rust API)
        let output = ctx.run_ui(input, |_ui| {
            // Apps provide their UI via the Rust API
        });

        // Tessellate shapes
        let pixels_per_point = output.pixels_per_point;
        let primitives: Vec<ClippedPrimitive> = ctx.tessellate(output.shapes, pixels_per_point);

        // Render on GPU
        backend.render(&primitives, &output.textures_delta, pixels_per_point);

        // Check if egui wants a repaint
        if ctx.has_requested_repaint() {
            s.needs_repaint = 1;
        } else {
            s.needs_repaint = 0;
        }

        1
    }
}

/// Run one frame with a Rust UI callback. This is the primary API for Rust apps.
pub fn run_frame_rust<F>(state: &mut XosEguiPlatform, ui_callback: F) -> bool
where
    F: FnMut(&mut egui::Ui),
{
    if state.ctx.is_null() || state.backend.is_null() {
        return false;
    }

    let ctx = unsafe { &*state.ctx };
    let backend = unsafe { &mut *state.backend };

    // Build raw input
    let mut input = RawInput::default();
    input.screen_rect = Some(egui::Rect::from_min_size(
        egui::pos2(0.0, 0.0),
        Vec2::new(state.width as f32, state.height as f32),
    ));

    let mouse_pos = egui::pos2(state.mouse_x as f32, state.mouse_y as f32);
    input.events.push(Event::PointerMoved(mouse_pos));

    if state.mouse_button != 0 {
        let button = match state.mouse_button {
            1 => PointerButton::Primary,
            2 => PointerButton::Secondary,
            3 => PointerButton::Middle,
            _ => PointerButton::Primary,
        };
        let pressed = state.mouse_action == 1;
        input.events.push(Event::PointerButton {
            pos: mouse_pos,
            button,
            pressed,
            modifiers: Modifiers::default(),
        });
    }

    if state.wheel_delta != 0.0 {
        input.events.push(Event::MouseWheel {
            unit: egui::MouseWheelUnit::Point,
            delta: Vec2::new(0.0, state.wheel_delta),
            phase: TouchPhase::Move,
            modifiers: Modifiers::default(),
        });
    }

    // Clear pending events
    state.mouse_button = 0;
    state.mouse_action = 0;
    state.wheel_delta = 0.0;
    state.key_event_count = 0;

    // Run egui frame
    let output = ctx.run_ui(input, ui_callback);

    // Tessellate
    let pixels_per_point = output.pixels_per_point;
    let primitives: Vec<ClippedPrimitive> = ctx.tessellate(output.shapes, pixels_per_point);

    // Render on GPU
    backend.render(&primitives, &output.textures_delta, pixels_per_point);

    if ctx.has_requested_repaint() {
        state.needs_repaint = 1;
    } else {
        state.needs_repaint = 0;
    }

    true
}

/// Get the egui Context from platform state (for Rust apps).
pub fn get_context(state: &XosEguiPlatform) -> &Context {
    unsafe { &*state.ctx }
}

/// Map a C key code to egui::Key.
fn map_key(key_code: u32) -> Option<egui::Key> {
    // Basic ASCII mapping
    match key_code {
        0x08 => Some(egui::Key::Backspace),
        0x09 => Some(egui::Key::Tab),
        0x0D => Some(egui::Key::Enter),
        0x1B => Some(egui::Key::Escape),
        0x20 => Some(egui::Key::Space),
        0x7F => Some(egui::Key::Delete),
        // Arrow keys
        0x100 => Some(egui::Key::ArrowLeft),
        0x101 => Some(egui::Key::ArrowRight),
        0x102 => Some(egui::Key::ArrowDown),
        0x103 => Some(egui::Key::ArrowUp),
        // Letters (a-z)
        k if k >= 0x61 && k <= 0x7A => {
            // Convert to egui::Key enum
            let chars = [
                egui::Key::A, egui::Key::B, egui::Key::C, egui::Key::D,
                egui::Key::E, egui::Key::F, egui::Key::G, egui::Key::H,
                egui::Key::I, egui::Key::J, egui::Key::K, egui::Key::L,
                egui::Key::M, egui::Key::N, egui::Key::O, egui::Key::P,
                egui::Key::Q, egui::Key::R, egui::Key::S, egui::Key::T,
                egui::Key::U, egui::Key::V, egui::Key::W, egui::Key::X,
                egui::Key::Y, egui::Key::Z,
            ];
            Some(chars[(k - 0x61) as usize])
        }
        // Numbers (0-9)
        k if k >= 0x30 && k <= 0x39 => {
            let nums = [
                egui::Key::Num0, egui::Key::Num1, egui::Key::Num2,
                egui::Key::Num3, egui::Key::Num4, egui::Key::Num5,
                egui::Key::Num6, egui::Key::Num7, egui::Key::Num8,
                egui::Key::Num9,
            ];
            Some(nums[(k - 0x30) as usize])
        }
        _ => None,
    }
}
