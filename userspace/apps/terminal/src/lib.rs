//! X OS Terminal — egui `Window` + `Frame` + `Area` chrome + `ScrollArea`.
//! No title bar strip: close/title/menus float over the content.

#![no_std]
#![allow(dead_code)]

extern crate alloc;

use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;

use egui::{
    pos2, scroll_area::{DragScroll, ScrollSource}, Area, Button, CentralPanel, Color32,
    CornerRadius, Frame, Id, Margin, Order, RichText, ScrollArea, Sense, Shadow, Stroke, Vec2,
    Window,
};

use xos_egui_platform::{self as platform, XosEguiPlatform};

const WIN_ID: &str = "xos_terminal_window";
const CHROME_ID: &str = "xos_terminal_chrome";
const MAX_LINES: usize = 500;
const WIN_RADIUS: u8 = 20;
const WIN_INSET: f32 = 4.0;
const CHROME_H: f32 = 28.0;

#[derive(Clone, Copy, PartialEq, Eq)]
enum AnsiParse {
    Normal,
    Esc,
    Csi,
    Osc,
    OscEsc, /* OSC … ESC — waiting for '\' (ST) */
}

struct TerminalApp {
    lines: Vec<String>,
    current: String,
    ansi: AnsiParse,
    bridged: bool,
    focused: bool,
    open: bool,
    surf_w: f32,
    surf_h: f32,
}

impl TerminalApp {
    fn new(surf_w: f32, surf_h: f32) -> Self {
        let mut app = Self {
            lines: Vec::new(),
            current: String::new(),
            ansi: AnsiParse::Normal,
            bridged: false,
            focused: true,
            open: true,
            surf_w,
            surf_h,
        };
        app.lines.push(String::from("X OS Terminal"));
        app.lines
            .push(String::from("Waiting for shell bridge…"));
        app
    }

    fn trim_lines(&mut self) {
        if self.lines.len() > MAX_LINES {
            let drop = self.lines.len() - MAX_LINES;
            self.lines.drain(0..drop);
        }
    }

    fn set_bridged(&mut self) {
        if self.bridged {
            return;
        }
        self.bridged = true;
        /* Drop only the waiting placeholder — keep any early shell output. */
        self.lines
            .retain(|l| l != "Waiting for shell bridge…");
        self.ansi = AnsiParse::Normal;
    }

    fn feed_bytes(&mut self, bytes: &[u8]) {
        if !self.bridged {
            self.set_bridged();
        }
        for &b in bytes {
            match self.ansi {
                AnsiParse::Esc => {
                    self.ansi = match b {
                        b'[' => AnsiParse::Csi,
                        b']' => AnsiParse::Osc,
                        /* Single-char ESC sequences (ESC 7/8/M/c/…) — drop. */
                        _ => AnsiParse::Normal,
                    };
                    continue;
                }
                AnsiParse::Csi => {
                    /* CSI is terminated by a final byte in 0x40..=0x7E. */
                    if (0x40..=0x7e).contains(&b) {
                        self.ansi = AnsiParse::Normal;
                    }
                    continue;
                }
                AnsiParse::Osc => {
                    if b == 0x07 {
                        self.ansi = AnsiParse::Normal;
                    } else if b == 0x1b {
                        self.ansi = AnsiParse::OscEsc;
                    }
                    continue;
                }
                AnsiParse::OscEsc => {
                    self.ansi = if b == b'\\' {
                        AnsiParse::Normal
                    } else {
                        AnsiParse::Osc
                    };
                    continue;
                }
                AnsiParse::Normal => {}
            }

            match b {
                0x1b => {
                    self.ansi = AnsiParse::Esc;
                }
                b'\n' => {
                    self.lines.push(core::mem::take(&mut self.current));
                    self.trim_lines();
                }
                b'\r' => {
                    /* CR: return to column 0 (overwrite current line). */
                    self.current.clear();
                }
                0x0c => {
                    /* Form-feed → clear scrollback (Apple `clear`). */
                    self.lines.clear();
                    self.current.clear();
                }
                0x08 | 0x7f => {
                    self.current.pop();
                }
                c if c >= 0x20 && c < 0x7f => {
                    self.current.push(c as char);
                }
                _ => {}
            }
        }
    }

    fn ui(&mut self, ui: &mut egui::Ui) {
        if !self.open {
            return;
        }

        let ctx = ui.ctx().clone();
        let style = ui.style().clone();
        /* Opaque fill — fade-in would leave the first boot frame ghost-transparent. */
        let frame = Frame::window(style.as_ref())
            .fill(Color32::from_rgb(28, 28, 32))
            .corner_radius(CornerRadius::same(WIN_RADIUS))
            .shadow(Shadow::NONE);

        /* Transparent surface so rounded corners show the desktop. */
        CentralPanel::no_frame().show(ui, |_| {});

        let w = (self.surf_w - WIN_INSET * 2.0).max(64.0);
        let h = (self.surf_h - WIN_INSET * 2.0).max(64.0);
        let win_pos = pos2(WIN_INSET, WIN_INSET);

        /* Body only — no stock title bar, no separator strip. */
        Window::new("Terminal")
            .id(Id::new(WIN_ID))
            .current_pos(win_pos)
            .default_size([w, h])
            .min_size([w, h])
            .max_size([w, h])
            .resizable(false)
            .collapsible(false)
            .movable(false)
            .title_bar(false)
            .fade_in(false)
            .fade_out(false)
            .frame(frame)
            .constrain(false)
            .show(&ctx, |ui| {
                let body = ui.available_rect_before_wrap();

                ScrollArea::vertical()
                    .id_salt("term_scroll")
                    .auto_shrink([false, false])
                    /* Stick while following output; user drag/wheel can leave bottom. */
                    .stick_to_bottom(true)
                    /* Desktop: click-drag on content (not only the scrollbar). */
                    .scroll_source(ScrollSource {
                        scroll_bar: true,
                        mouse_wheel: true,
                        drag: DragScroll::Always,
                    })
                    .show(ui, |ui| {
                        ui.set_min_width(body.width());
                        /* Room under floating chrome so first lines aren't covered. */
                        ui.add_space(CHROME_H);

                        /* Focus on click without stealing ScrollArea drag. */
                        let resp = ui.allocate_response(
                            Vec2::new(body.width(), 0.0),
                            Sense::click(),
                        );
                        if resp.clicked() {
                            self.focused = true;
                        }

                        for line in &self.lines {
                            if line.is_empty() {
                                ui.add_space(4.0);
                            } else {
                                ui.monospace(line.as_str());
                            }
                        }

                        ui.horizontal(|ui| {
                            ui.spacing_mut().item_spacing.x = 0.0;
                            ui.monospace(self.current.as_str());
                            if self.focused {
                                ui.monospace("▌");
                            }
                        });
                        /* Extra slack so the last line isn't flush against the bar. */
                        ui.add_space(8.0);
                    });
            });

        /*
         * Floating chrome: close + title only.
         * Shell / Edit / View live in the system menubar later (macOS-style).
         */
        let chrome_frame = Frame::new()
            .fill(Color32::TRANSPARENT)
            .stroke(Stroke::NONE)
            .inner_margin(Margin::symmetric(10, 6))
            .shadow(Shadow::NONE);

        Area::new(Id::new(CHROME_ID))
            .order(Order::Foreground)
            .fixed_pos(win_pos + Vec2::new(0.0, 2.0))
            .movable(false)
            .interactable(true)
            .fade_in(false)
            .constrain(false)
            .show(&ctx, |ui| {
                chrome_frame.show(ui, |ui| {
                    ui.horizontal(|ui| {
                        ui.spacing_mut().item_spacing.x = 10.0;

                        let close = ui.add(
                            Button::new(RichText::new("×").size(16.0))
                                .frame(false)
                                .min_size(Vec2::new(22.0, 20.0)),
                        );
                        if close.clicked() {
                            self.open = false;
                        }

                        ui.label(RichText::new("Terminal").strong());
                    });
                });
            });
    }
}

#[repr(C)]
pub struct TerminalState {
    platform: *mut XosEguiPlatform,
    app: *mut TerminalApp,
    needs_present: u32,
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_terminal_init(
    width: u32,
    height: u32,
    ctx_id: u32,
    vb_mem: *mut u8,
    vb_mem_size: usize,
    ib_mem: *mut u8,
    ib_mem_size: usize,
    tex_mem: *mut u8,
    tex_mem_size: usize,
) -> *mut TerminalState {
    let platform = platform::xos_egui_platform_init(
        ctx_id,
        width,
        height,
        vb_mem,
        vb_mem_size,
        ib_mem,
        ib_mem_size,
        tex_mem,
        tex_mem_size,
    );
    if platform.is_null() {
        return core::ptr::null_mut();
    }

    Box::into_raw(Box::new(TerminalState {
        platform,
        app: Box::into_raw(Box::new(TerminalApp::new(width as f32, height as f32))),
        needs_present: 1,
    }))
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_terminal_destroy(state: *mut TerminalState) {
    if state.is_null() {
        return;
    }
    unsafe {
        let state = Box::from_raw(state);
        if !state.app.is_null() {
            let _ = Box::from_raw(state.app);
        }
        if !state.platform.is_null() {
            platform::xos_egui_platform_destroy(state.platform);
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_terminal_mouse_event(
    state: *mut TerminalState,
    x: i32,
    y: i32,
    button: u32,
    action: u32,
) {
    if state.is_null() {
        return;
    }
    unsafe {
        platform::xos_egui_platform_mouse_event((*state).platform, x, y, button, action);
        if action != 0 {
            (*state).needs_present = 1;
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_terminal_wheel_event(state: *mut TerminalState, delta: f32) {
    if state.is_null() {
        return;
    }
    unsafe {
        platform::xos_egui_platform_wheel_event((*state).platform, delta);
        (*state).needs_present = 1;
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_terminal_key_event(state: *mut TerminalState, key: u32, pressed: u32) {
    if state.is_null() {
        return;
    }
    unsafe {
        platform::xos_egui_platform_key_event((*state).platform, key, pressed);
        (*state).needs_present = 1;
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_terminal_text_event(state: *mut TerminalState, ch: u32) {
    if state.is_null() {
        return;
    }
    unsafe {
        platform::xos_egui_platform_text_event((*state).platform, ch);
        (*state).needs_present = 1;
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_terminal_feed_output(state: *mut TerminalState, data: *const u8, len: usize) {
    if state.is_null() || data.is_null() || len == 0 {
        return;
    }
    unsafe {
        let s = &mut *state;
        if s.app.is_null() {
            return;
        }
        let bytes = core::slice::from_raw_parts(data, len);
        (*s.app).feed_bytes(bytes);
        s.needs_present = 1;
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_terminal_set_bridged(state: *mut TerminalState) {
    if state.is_null() {
        return;
    }
    unsafe {
        let s = &mut *state;
        if s.app.is_null() {
            return;
        }
        (*s.app).set_bridged();
        s.needs_present = 1;
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_terminal_run_frame(state: *mut TerminalState) -> u32 {
    if state.is_null() {
        return 0;
    }
    unsafe {
        let s = &mut *state;
        if s.platform.is_null() || s.app.is_null() {
            return 0;
        }
        let plat = &mut *s.platform;
        let app = &mut *s.app;
        if !platform::run_frame_ctx(plat, |ui| {
            app.ui(ui);
        }) {
            return 0;
        }
        /* Always present after a paint; keep dirty if egui wants another frame. */
        s.needs_present = 1;
        1
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_terminal_needs_present(state: *mut TerminalState) -> u32 {
    if state.is_null() {
        return 0;
    }
    unsafe { (*state).needs_present }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_terminal_ack_present(state: *mut TerminalState) {
    if state.is_null() {
        return;
    }
    unsafe {
        (*state).needs_present = 0;
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_terminal_is_open(state: *mut TerminalState) -> u32 {
    if state.is_null() {
        return 0;
    }
    unsafe {
        if (*state).app.is_null() {
            return 0;
        }
        if (*(*state).app).open {
            1
        } else {
            0
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_terminal_render_target_id(state: *mut TerminalState) -> u32 {
    if state.is_null() {
        return 0;
    }
    unsafe { platform::xos_egui_platform_render_target_id((*state).platform) }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_terminal_context_id(state: *mut TerminalState) -> u32 {
    if state.is_null() {
        return 0;
    }
    unsafe { platform::xos_egui_platform_context_id((*state).platform) }
}
