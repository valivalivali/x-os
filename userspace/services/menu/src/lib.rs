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

use egui::menu::{SubMenuButton, menu_style};
use egui::{
    Align, Atom, Button, Color32, Context, CornerRadius, Event, FontId, Id, Layout, Margin,
    Modifiers, PointerButton, Popup, PopupCloseBehavior, PopupKind, RawInput, Sense, Shadow,
    Stroke, TextStyle, Vec2, pos2, vec2,
};
use egui_virgl_backend::EguiVirglBackend;

/* Compact professional menu — natural dark slate + red highlight.
 * No soft shadows (too expensive on the CPU raster path under QEMU). */
fn apply_menu_style(style: &mut egui::Style) {
    menu_style(style);

    let panel = Color32::from_rgb(30, 32, 40); /* natural cool slate */
    let rim = Color32::from_rgb(110, 118, 138);
    let hover = Color32::from_rgb(180, 45, 45); /* red on dark */
    let active = Color32::from_rgb(160, 35, 35);
    let open = Color32::from_rgb(150, 40, 40);
    let text = Color32::from_rgb(242, 244, 248);
    let sep = Color32::from_rgb(52, 56, 68);
    let radius = CornerRadius::same(10);
    let row = CornerRadius::same(5);

    /* Thin & dense — kill the bulky default interact height. */
    style.spacing.button_padding = vec2(8.0, 3.0);
    style.spacing.item_spacing = vec2(0.0, 1.0);
    style.spacing.menu_margin = Margin::symmetric(5, 5);
    style.spacing.interact_size = vec2(40.0, 18.0);
    style.text_styles.insert(TextStyle::Button, FontId::proportional(13.0));
    style.text_styles.insert(TextStyle::Body, FontId::proportional(13.0));

    style.visuals.dark_mode = true;
    style.visuals.override_text_color = Some(text);
    style.visuals.window_fill = panel;
    /* Root ui must stay clear — opaque panel_fill painted the whole 420×360
     * RT and showed up as a dark plate under the popup. */
    style.visuals.panel_fill = Color32::TRANSPARENT;
    style.visuals.extreme_bg_color = Color32::TRANSPARENT;
    style.visuals.faint_bg_color = Color32::from_rgb(38, 40, 50);
    style.visuals.window_corner_radius = radius;
    style.visuals.menu_corner_radius = radius;
    style.visuals.popup_shadow = Shadow::NONE;
    style.visuals.window_shadow = Shadow::NONE;

    style.visuals.widgets.noninteractive.bg_stroke = Stroke::new(1.0, sep);
    style.visuals.widgets.noninteractive.fg_stroke = Stroke::new(1.0, text);
    style.visuals.widgets.noninteractive.corner_radius = radius;

    style.visuals.widgets.inactive.weak_bg_fill = Color32::TRANSPARENT;
    style.visuals.widgets.inactive.bg_fill = Color32::TRANSPARENT;
    style.visuals.widgets.inactive.bg_stroke = Stroke::NONE;
    style.visuals.widgets.inactive.fg_stroke = Stroke::new(1.0, text);
    style.visuals.widgets.inactive.corner_radius = row;

    style.visuals.widgets.hovered.weak_bg_fill = hover;
    style.visuals.widgets.hovered.bg_fill = hover;
    style.visuals.widgets.hovered.bg_stroke = Stroke::NONE;
    style.visuals.widgets.hovered.fg_stroke = Stroke::new(1.0, Color32::WHITE);
    style.visuals.widgets.hovered.corner_radius = row;

    style.visuals.widgets.active.weak_bg_fill = active;
    style.visuals.widgets.active.bg_fill = active;
    style.visuals.widgets.active.bg_stroke = Stroke::NONE;
    style.visuals.widgets.active.fg_stroke = Stroke::new(1.0, Color32::WHITE);
    style.visuals.widgets.active.corner_radius = row;

    style.visuals.widgets.open.weak_bg_fill = open;
    style.visuals.widgets.open.bg_fill = open;
    style.visuals.widgets.open.bg_stroke = Stroke::NONE;
    style.visuals.widgets.open.fg_stroke = Stroke::new(1.0, Color32::WHITE);
    style.visuals.widgets.open.corner_radius = row;

    style.visuals.selection.bg_fill = hover;
    style.visuals.selection.stroke = Stroke::NONE;

    style.animation_time = 0.0;
}

/* macOS-style columns: same width on the left (icons) and right (shortcuts / ›). */
const MENU_GUTTER: f32 = 14.0;

fn leading_gutter() -> Atom<'static> {
    Atom::custom(Id::new("xos_menu_icon"), vec2(MENU_GUTTER, MENU_GUTTER))
}

fn trailing_gutter() -> Atom<'static> {
    Atom::custom(Id::new("xos_menu_trail"), vec2(MENU_GUTTER, MENU_GUTTER))
}

fn menu_row(label: &'static str) -> Button<'static> {
    /* [gutter] label ………… [trail] — gutters kept for icons/shortcuts later */
    Button::new((leading_gutter(), label))
        .gap(8.0)
        .right_text(trailing_gutter())
}

fn submenu_row(label: &'static str) -> Button<'static> {
    Button::new((leading_gutter(), label))
        .gap(8.0)
        .right_text(SubMenuButton::RIGHT_ARROW)
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
        .width(220.0)
        .show(|ui| {
            if ui.add(menu_row("New Folder")).clicked() {
                take_action(result, MenuAction::NewFolder, ui);
            }
            if ui.add(menu_row("Get Info")).clicked() {
                take_action(result, MenuAction::GetInfo, ui);
            }
            if ui.add(menu_row("Change Wallpaper")).clicked() {
                take_action(result, MenuAction::ChangeWallpaper, ui);
            }

            ui.separator();

            if ui.add(menu_row("Use Stacks")).clicked() {
                take_action(result, MenuAction::UseStacks, ui);
            }

            SubMenuButton::from_button(submenu_row("Clean Up By")).ui(ui, |ui| {
                open_sub.set(1);
                if ui.add(menu_row("Name")).clicked() {
                    take_action(result, MenuAction::CleanUpByName, ui);
                }
                if ui.add(menu_row("Date Modified")).clicked() {
                    take_action(result, MenuAction::CleanUpByDateModified, ui);
                }
                if ui.add(menu_row("Kind")).clicked() {
                    take_action(result, MenuAction::CleanUpByKind, ui);
                }
                if ui.add(menu_row("Size")).clicked() {
                    take_action(result, MenuAction::CleanUpBySize, ui);
                }
            });

            SubMenuButton::from_button(submenu_row("Sort By")).ui(ui, |ui| {
                open_sub.set(2);
                if ui.add(menu_row("Name")).clicked() {
                    take_action(result, MenuAction::SortByName, ui);
                }
                if ui.add(menu_row("Date Modified")).clicked() {
                    take_action(result, MenuAction::SortByDate, ui);
                }
                if ui.add(menu_row("Size")).clicked() {
                    take_action(result, MenuAction::SortBySize, ui);
                }
                if ui.add(menu_row("Kind")).clicked() {
                    take_action(result, MenuAction::SortByKind, ui);
                }
            });

            ui.separator();

            if ui.add(menu_row("Import from iPhone")).clicked() {
                take_action(result, MenuAction::ImportFromIPhone, ui);
            }
            if ui.add(menu_row("Show View Options")).clicked() {
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
    /// Painted opaque bbox (for cropping the WM overlay quad).
    content_w: u32,
    content_h: u32,
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
        content_w: surf_w,
        content_h: surf_h,
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

        let flyout_changed = submenu_id != state.last_submenu_id;
        state.last_submenu_id = submenu_id;

        let hover_fp = hover_fingerprint(ctx);
        let hover_changed = hover_fp != state.last_hover_fp;
        state.last_hover_fp = hover_fp;

        let need_paint = state.first_frame_done == 0
            || was_triggering
            || flyout_changed
            || hover_changed;
        if need_paint {
            let pixels_per_point = output.pixels_per_point;
            let primitives = ctx.tessellate(output.shapes, pixels_per_point);
            backend.render(&primitives, &output.textures_delta, pixels_per_point);
            let (cw, ch) = backend.measure_content_size();
            let size_changed = cw != state.content_w || ch != state.content_h;
            state.content_w = cw;
            state.content_h = ch;
            if size_changed && state.first_frame_done != 0 {
                /* Grow/shrink overlay quad — restore L1 under old bounds. */
                state.needs_present = 1 | 2;
            }
        }

        if state.first_frame_done == 0 || was_triggering {
            /* First frame: SURFACE_GPU_READY presents. */
            state.first_frame_done = 1;
            state.needs_present = 0;
            let (cw, ch) = backend.measure_content_size();
            state.content_w = cw;
            state.content_h = ch;
        } else if flyout_changed {
            /* Fly-out open/close/switch: must restore L1 (transparent tex
             * cannot erase old opaque fly-out pixels in the scanout). */
            state.needs_present = 1 | 2;
        } else if hover_changed {
            /* Row highlight only — overlay_fast blit (opaque rows replace).
             * Do not clear bit1 if a prior present is still pending. */
            state.needs_present |= 1;
        }
        /* else: leave needs_present alone so a throttled present is not lost */

        state.closed
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn xos_context_menu_content_size(
    state: *mut ContextMenuState,
    out_w: *mut u32,
    out_h: *mut u32,
) {
    if state.is_null() || out_w.is_null() || out_h.is_null() {
        return;
    }
    unsafe {
        *out_w = (*state).content_w;
        *out_h = (*state).content_h;
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
