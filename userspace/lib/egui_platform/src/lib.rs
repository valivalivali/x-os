//! X OS egui platform layer — shared infrastructure for GPU-accelerated egui apps.
//!
//! Apps depend on this as an `rlib` and provide a UI callback. The C shim owns
//! WM IPC; this crate owns egui input + VirGL present.

#![no_std]
#![allow(dead_code)]

extern crate alloc;

use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;

use egui::{
    Context, Event, Modifiers, PointerButton, RawInput, TouchPhase, Vec2,
};
use egui_virgl_backend::EguiVirglBackend;
use epaint::ClippedPrimitive;

/// Platform state shared between C shim and Rust.
#[repr(C)]
pub struct XosEguiPlatform {
    ctx: *mut Context,
    backend: *mut EguiVirglBackend,
    width: u32,
    height: u32,
    mouse_x: i32,
    mouse_y: i32,
    mouse_button: u32,
    mouse_action: u32,
    wheel_delta: f32,
    /// Packed key events: high 16 = key code, low 16 = pressed flag
    key_events: [u32; 16],
    key_event_count: u32,
    /// Pending UTF-8 / ASCII text (for TextEdit)
    text_buf: [u8; 64],
    text_len: u32,
    needs_repaint: u32,
}

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
    ctx.set_visuals(egui::Visuals::dark());

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

    Box::into_raw(Box::new(XosEguiPlatform {
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
        text_buf: [0; 64],
        text_len: 0,
        needs_repaint: 1,
    }))
}

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

#[unsafe(no_mangle)]
pub extern "C" fn xos_egui_platform_wheel_event(state: *mut XosEguiPlatform, delta: f32) {
    if state.is_null() {
        return;
    }
    unsafe {
        let s = &mut *state;
        s.wheel_delta = delta;
        s.needs_repaint = 1;
    }
}

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

/// Feed a printable character (ASCII) as egui Text input.
#[unsafe(no_mangle)]
pub extern "C" fn xos_egui_platform_text_event(state: *mut XosEguiPlatform, ch: u32) {
    if state.is_null() || ch == 0 || ch > 0x7F {
        return;
    }
    unsafe {
        let s = &mut *state;
        if (s.text_len as usize) < s.text_buf.len() {
            s.text_buf[s.text_len as usize] = ch as u8;
            s.text_len += 1;
            s.needs_repaint = 1;
        }
    }
}

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

#[unsafe(no_mangle)]
pub extern "C" fn xos_egui_platform_needs_repaint(state: *mut XosEguiPlatform) -> u32 {
    if state.is_null() {
        return 0;
    }
    unsafe { (*state).needs_repaint }
}

fn build_input(state: &mut XosEguiPlatform) -> RawInput {
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

    for i in 0..state.key_event_count as usize {
        let packed = state.key_events[i];
        let key_code = packed >> 16;
        let pressed = (packed & 0xFFFF) != 0;
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

    if state.text_len > 0 {
        let s = core::str::from_utf8(&state.text_buf[..state.text_len as usize]).unwrap_or("");
        if !s.is_empty() {
            input.events.push(Event::Text(String::from(s)));
        }
    }

    state.mouse_button = 0;
    state.mouse_action = 0;
    state.wheel_delta = 0.0;
    state.key_event_count = 0;
    state.text_len = 0;

    input
}

/// Run one frame with a root [`Ui`] callback (legacy / simple apps).
pub fn run_frame_rust<F>(state: &mut XosEguiPlatform, mut ui_callback: F) -> bool
where
    F: FnMut(&mut egui::Ui),
{
    run_frame_ctx(state, |ui| ui_callback(ui))
}

/// Run one frame with the root [`Ui`] from egui's `Context::run_ui`.
///
/// Build UI with real containers on that `Ui` / its `Context`:
/// `CentralPanel::default().show(ui, …)`, `Window::new(...).show(ui.ctx(), …)`,
/// `MenuBar`, `ScrollArea`, etc.
pub fn run_frame_ctx<F>(state: &mut XosEguiPlatform, mut ui_callback: F) -> bool
where
    F: FnMut(&mut egui::Ui),
{
    if state.ctx.is_null() || state.backend.is_null() {
        return false;
    }

    let ctx = unsafe { &*state.ctx };
    let backend = unsafe { &mut *state.backend };
    let input = build_input(state);

    let output = ctx.run_ui(input, |ui| {
        ui_callback(ui);
    });

    let pixels_per_point = output.pixels_per_point;
    let primitives: Vec<ClippedPrimitive> = ctx.tessellate(output.shapes, pixels_per_point);
    backend.render(&primitives, &output.textures_delta, pixels_per_point);

    state.needs_repaint = if ctx.has_requested_repaint() { 1 } else { 0 };
    true
}

pub fn get_context(state: &XosEguiPlatform) -> &Context {
    unsafe { &*state.ctx }
}

pub fn surface_size(state: &XosEguiPlatform) -> (u32, u32) {
    (state.width, state.height)
}

pub fn as_mut<'a>(state: *mut XosEguiPlatform) -> Option<&'a mut XosEguiPlatform> {
    if state.is_null() {
        None
    } else {
        Some(unsafe { &mut *state })
    }
}

fn map_key(key_code: u32) -> Option<egui::Key> {
    match key_code {
        0x08 => Some(egui::Key::Backspace),
        0x09 => Some(egui::Key::Tab),
        0x0D => Some(egui::Key::Enter),
        0x1B => Some(egui::Key::Escape),
        0x20 => Some(egui::Key::Space),
        0x7F => Some(egui::Key::Delete),
        0x100 => Some(egui::Key::ArrowLeft),
        0x101 => Some(egui::Key::ArrowRight),
        0x102 => Some(egui::Key::ArrowDown),
        0x103 => Some(egui::Key::ArrowUp),
        k if (0x61..=0x7A).contains(&k) => {
            let chars = [
                egui::Key::A,
                egui::Key::B,
                egui::Key::C,
                egui::Key::D,
                egui::Key::E,
                egui::Key::F,
                egui::Key::G,
                egui::Key::H,
                egui::Key::I,
                egui::Key::J,
                egui::Key::K,
                egui::Key::L,
                egui::Key::M,
                egui::Key::N,
                egui::Key::O,
                egui::Key::P,
                egui::Key::Q,
                egui::Key::R,
                egui::Key::S,
                egui::Key::T,
                egui::Key::U,
                egui::Key::V,
                egui::Key::W,
                egui::Key::X,
                egui::Key::Y,
                egui::Key::Z,
            ];
            Some(chars[(k - 0x61) as usize])
        }
        k if (0x30..=0x39).contains(&k) => {
            let nums = [
                egui::Key::Num0,
                egui::Key::Num1,
                egui::Key::Num2,
                egui::Key::Num3,
                egui::Key::Num4,
                egui::Key::Num5,
                egui::Key::Num6,
                egui::Key::Num7,
                egui::Key::Num8,
                egui::Key::Num9,
            ];
            Some(nums[(k - 0x30) as usize])
        }
        _ => None,
    }
}
