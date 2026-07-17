//! X OS Context Menu Service
//!
//! Stock egui menu APIs so hover / fly-outs work like desktop egui:
//!   - [`Popup`] + [`PopupKind::Menu`] + [`menu_style`]
//!   - [`Ui::menu_button`] / [`Ui::button`] (SubMenuButton opens on hover)
//!
//! Every pointer event (and idle ticks) runs a full egui frame. Scanout
//! presents only when the painted hover / fly-out topology changes —
//! QEMU recopies the full framebuffer on each flush.

#![no_std]
#![allow(dead_code)]

extern crate alloc;

use alloc::boxed::Box;
use core::cell::Cell;

use egui::menu::menu_style;
use egui::{
    Align, Color32, Context, Event, Id, Layout, Modifiers, PointerButton, Popup,
    PopupCloseBehavior, PopupKind, RawInput, Sense, Shadow, Vec2, pos2,
};
use egui_virgl_backend::EguiVirglBackend;

fn apply_menu_style(style: &mut egui::Style) {
    menu_style(style);
    let menu_bg = Color32::from_rgb(36, 36, 36);
    style.visuals.window_fill = menu_bg;
    style.visuals.panel_fill = menu_bg;
    style.visuals.extreme_bg_color = menu_bg;
    style.visuals.faint_bg_color = menu_bg;
    style.visuals.popup_shadow = Shadow::NONE;
    style.visuals.window_shadow = Shadow::NONE;
    /* Submenus use Area fade-in; keep it instant for overlay compositing. */
    style.animation_time = 0.0;
}

#[repr(u32)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MenuAction {
    None = 0,
    NewFolder = 1,
    GetInfo = 2,
    ChangeWallpaper = 3,
    UseStacks = 4,
    CleanUpByName = 5,
    CleanUpByDateModified = 6,
    CleanUpByKind = 7,
    CleanUpBySize = 8,
    ImportFromIPhone = 9,
    SortByName = 10,
    SortByDate = 11,
    SortBySize = 12,
    SortByKind = 13,
    ShowViewOptions = 14,
}

pub struct MenuResult {
    pub action: MenuAction,
    pub closed: bool,
}

impl Default for MenuResult {
    fn default() -> Self {
        Self {
            action: MenuAction::None,
            closed: false,
        }
    }
}

fn take_action(result: &mut MenuResult, action: MenuAction, ui: &egui::Ui) {
    result.action = action;
    result.closed = true;
    ui.close();
}

fn show_desktop_context_menu(
    ui: &mut egui::Ui,
    result: &mut MenuResult,
    menu_open: &mut bool,
    submenu_id: &mut u32,
) {
    let ctx = ui.ctx().clone();
    for theme in [egui::Theme::Dark, egui::Theme::Light] {
        ctx.style_mut_of(theme, apply_menu_style);
    }

    if !*menu_open {
        return;
    }

    /* Tiny anchor — Popup places the real menu; SubMenuButton owns fly-outs. */
    let anchor = ui.interact(
        egui::Rect::from_min_size(pos2(8.0, 8.0), Vec2::splat(1.0)),
        Id::new("xos_ctx_menu_anchor"),
        Sense::click(),
    );

    let was_open = *menu_open;
    let open_sub = Cell::new(0u32);

    Popup::from_response(&anchor)
        .kind(PopupKind::Menu)
        .layout(Layout::top_down_justified(Align::Min))
        .style(apply_menu_style)
        .at_position(pos2(8.0, 8.0))
        .open_bool(menu_open)
        .close_behavior(PopupCloseBehavior::CloseOnClickOutside)
        .width(200.0)
        .show(|ui| {
            if ui.button("New Folder").clicked() {
                take_action(result, MenuAction::NewFolder, ui);
            }
            if ui.button("Get Info").clicked() {
                take_action(result, MenuAction::GetInfo, ui);
            }
            if ui.button("Change Wallpaper").clicked() {
                take_action(result, MenuAction::ChangeWallpaper, ui);
            }

            ui.separator();

            if ui.button("Use Stacks").clicked() {
                take_action(result, MenuAction::UseStacks, ui);
            }

            ui.menu_button("Clean Up By", |ui| {
                open_sub.set(1);
                if ui.button("Name").clicked() {
                    take_action(result, MenuAction::CleanUpByName, ui);
                }
                if ui.button("Date Modified").clicked() {
                    take_action(result, MenuAction::CleanUpByDateModified, ui);
                }
                if ui.button("Kind").clicked() {
                    take_action(result, MenuAction::CleanUpByKind, ui);
                }
                if ui.button("Size").clicked() {
                    take_action(result, MenuAction::CleanUpBySize, ui);
                }
            });

            ui.menu_button("Sort By", |ui| {
                open_sub.set(2);
                if ui.button("Name").clicked() {
                    take_action(result, MenuAction::SortByName, ui);
                }
                if ui.button("Date Modified").clicked() {
                    take_action(result, MenuAction::SortByDate, ui);
                }
                if ui.button("Size").clicked() {
                    take_action(result, MenuAction::SortBySize, ui);
                }
                if ui.button("Kind").clicked() {
                    take_action(result, MenuAction::SortByKind, ui);
                }
            });

            ui.separator();

            if ui.button("Import from iPhone").clicked() {
                take_action(result, MenuAction::ImportFromIPhone, ui);
            }
            if ui.button("Show View Options").clicked() {
                take_action(result, MenuAction::ShowViewOptions, ui);
            }
        });

    *submenu_id = open_sub.get();

    if was_open && !*menu_open && !result.closed {
        result.closed = true;
        result.action = MenuAction::None;
    }
}

/* Order-independent (XOR) so HashSet iteration cannot false-trigger presents. */
fn hover_fingerprint(ctx: &Context) -> u64 {
    ctx.interaction_snapshot(|snap| {
        let mut h: u64 = snap.hovered.len() as u64;
        for id in &snap.hovered {
            h ^= id.value();
        }
        h
    })
}

#[repr(C)]
pub struct ContextMenuState {
    ctx: *mut Context,
    action: u32,
    closed: u32,
    screen_w: u32,
    screen_h: u32,
    surf_w: u32,
    surf_h: u32,
    mouse_x: i32,
    mouse_y: i32,
    mouse_button: u32,
    mouse_action: u32,
    trigger_open: u32,
    menu_open: u32,
    open_grace: u32,
    /// bit0 = present OVERLAY; bit1 = refresh L1 under menu (fly-out change)
    needs_present: u32,
    last_submenu_id: u32,
    last_hover_fp: u64,
    first_frame_done: u32,
    /// Milliseconds since boot (SYS_GET_TICKS).
    time_ms: u64,
    backend: *mut EguiVirglBackend,
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_context_menu_init(
    screen_w: u32,
    screen_h: u32,
    surf_w: u32,
    surf_h: u32,
    ctx_id: u32,
    vb_mem: *mut u8,
    vb_mem_size: usize,
    ib_mem: *mut u8,
    ib_mem_size: usize,
    tex_mem: *mut u8,
    tex_mem_size: usize,
) -> *mut ContextMenuState {
    let ctx = Context::default();
    let mut backend = Box::new(EguiVirglBackend::new(
        ctx_id,
        surf_w,
        surf_h,
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

    Box::into_raw(Box::new(ContextMenuState {
        ctx: Box::into_raw(Box::new(ctx)),
        action: 0,
        closed: 0,
        screen_w,
        screen_h,
        surf_w,
        surf_h,
        mouse_x: 0,
        mouse_y: 0,
        mouse_button: 0,
        mouse_action: 0,
        trigger_open: 0,
        menu_open: 0,
        open_grace: 0,
        needs_present: 0,
        last_submenu_id: 0,
        last_hover_fp: 0,
        first_frame_done: 0,
        time_ms: 0,
        backend: Box::into_raw(backend),
    }))
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_context_menu_destroy(state: *mut ContextMenuState) {
    if state.is_null() {
        return;
    }
    unsafe {
        let mut state = Box::from_raw(state);
        if !state.backend.is_null() {
            let mut backend = Box::from_raw(state.backend);
            backend.destroy();
            state.backend = core::ptr::null_mut();
        }
        if !state.ctx.is_null() {
            drop(Box::from_raw(state.ctx));
            state.ctx = core::ptr::null_mut();
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_context_menu_mouse_event(
    state: *mut ContextMenuState,
    x: i32,
    y: i32,
    button: u32,
    action: u32,
) {
    if state.is_null() {
        return;
    }
    unsafe {
        let state = &mut *state;
        state.mouse_x = x;
        state.mouse_y = y;
        state.mouse_button = button;
        state.mouse_action = action;
    }
}

/// Wall-clock for egui (ms since boot). Needed for submenu
/// `is_moving_towards_rect` hover heuristics.
#[unsafe(no_mangle)]
pub extern "C" fn xos_context_menu_set_time_ms(state: *mut ContextMenuState, time_ms: u64) {
    if state.is_null() {
        return;
    }
    unsafe {
        (*state).time_ms = time_ms;
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_context_menu_trigger(state: *mut ContextMenuState) {
    if state.is_null() {
        return;
    }
    unsafe {
        let state = &mut *state;
        state.mouse_x = 8;
        state.mouse_y = 8;
        state.mouse_button = 0;
        state.mouse_action = 0;
        state.trigger_open = 1;
        state.menu_open = 1;
        state.open_grace = 3;
        state.closed = 0;
        state.action = 0;
        state.needs_present = 0;
        state.last_submenu_id = 0;
        state.last_hover_fp = 0;
        state.first_frame_done = 0;
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_context_menu_run_frame(state: *mut ContextMenuState) -> u32 {
    if state.is_null() {
        return 0;
    }
    unsafe {
        let state = &mut *state;
        if state.ctx.is_null() || state.backend.is_null() {
            return 0;
        }
        let ctx = &*state.ctx;
        let backend = &mut *state.backend;

        state.closed = 0;
        state.action = 0;

        let mut menu_result = MenuResult::default();
        let was_triggering = state.trigger_open == 1;

        let mut input = RawInput::default();
        input.screen_rect = Some(egui::Rect::from_min_size(
            egui::pos2(0.0, 0.0),
            Vec2::new(state.surf_w as f32, state.surf_h as f32),
        ));
        input.predicted_dt = 1.0 / 60.0;
        input.time = Some(state.time_ms as f64 / 1000.0);

        let mouse_pos = egui::pos2(state.mouse_x as f32, state.mouse_y as f32);
        input.events.push(Event::PointerMoved(mouse_pos));

        if !was_triggering && state.mouse_button != 0 {
            let button = if state.mouse_button == 2 {
                PointerButton::Secondary
            } else {
                PointerButton::Primary
            };
            /* action: 1=down, 2=up (composer), anything else treated as down. */
            let pressed = state.mouse_action != 2;
            input.events.push(Event::PointerButton {
                pos: mouse_pos,
                button,
                pressed,
                modifiers: Modifiers::default(),
            });
        }

        state.mouse_button = 0;
        state.mouse_action = 0;
        state.trigger_open = 0;

        let mut menu_open = state.menu_open != 0;
        if was_triggering {
            menu_open = true;
        }

        let mut submenu_id: u32 = 0;
        let output = ctx.run_ui(input, |ui| {
            show_desktop_context_menu(ui, &mut menu_result, &mut menu_open, &mut submenu_id);
        });

        state.menu_open = if menu_open { 1 } else { 0 };

        if state.open_grace > 0 {
            state.open_grace -= 1;
            if !menu_result.closed {
                menu_open = true;
                state.menu_open = 1;
            }
        }

        if menu_result.closed {
            state.closed = 1;
            state.action = menu_result.action as u32;
            state.menu_open = 0;
            state.open_grace = 0;
        } else if state.open_grace == 0 && !was_triggering && !menu_open {
            state.closed = 1;
            state.action = 0;
        }

        let pixels_per_point = output.pixels_per_point;
        let primitives = ctx.tessellate(output.shapes, pixels_per_point);
        backend.render(&primitives, &output.textures_delta, pixels_per_point);

        let flyout_changed = submenu_id != state.last_submenu_id;
        state.last_submenu_id = submenu_id;

        let hover_fp = hover_fingerprint(ctx);
        let hover_changed = hover_fp != state.last_hover_fp;
        state.last_hover_fp = hover_fp;

        if state.first_frame_done == 0 || was_triggering {
            /* First frame: SURFACE_GPU_READY presents. */
            state.first_frame_done = 1;
            state.needs_present = 0;
        } else if flyout_changed || hover_changed {
            /* bit1 always set: overlay_fast alpha-blits leave old fly-out pixels
             * in the scanout when the new frame is transparent there. Always
             * restore L1 under the menu rect, then blit the fresh texture. */
            state.needs_present = 1 | 2;
        } else {
            state.needs_present = 0;
        }

        state.closed
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_context_menu_needs_present(state: *mut ContextMenuState) -> u32 {
    if state.is_null() {
        return 0;
    }
    unsafe { (*state).needs_present }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_context_menu_ack_present(state: *mut ContextMenuState) {
    if state.is_null() {
        return;
    }
    unsafe { (*state).needs_present = 0 }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_context_menu_pending_close(_state: *mut ContextMenuState) -> u32 {
    0
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_context_menu_render_target_id(state: *mut ContextMenuState) -> u32 {
    if state.is_null() {
        return 0;
    }
    unsafe {
        let state = &*state;
        if state.backend.is_null() {
            return 0;
        }
        (*state.backend).render_target_id()
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_context_menu_context_id(state: *mut ContextMenuState) -> u32 {
    if state.is_null() {
        return 0;
    }
    unsafe {
        let state = &*state;
        if state.backend.is_null() {
            return 0;
        }
        (*state.backend).context_id()
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_context_menu_get_action(state: *mut ContextMenuState) -> u32 {
    if state.is_null() {
        return 0;
    }
    unsafe { (*state).action }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_context_menu_is_open(state: *mut ContextMenuState) -> u32 {
    if state.is_null() {
        return 0;
    }
    unsafe { (*state).menu_open }
}
