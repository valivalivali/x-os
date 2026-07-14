//! X OS Context Menu Service
//!
//! Provides right-click context menus for the X OS desktop using egui's
//! real context menu API (`Response::context_menu`, `SubMenuButton`).
//!
//! This module is compiled as a static library and called from a thin
//! C shim that handles IPC with the compositor.
//!
//! The menu structure:
//!   New Folder
//!   Get Info
//!   Change Wallpaper
//!   ---
//!   Use Stacks
//!   Clean Up By ▸
//!     Name
//!     Date Modified
//!     Kind
//!     Size
//!   ---
//!   Import from iPhone

#![no_std]
#![allow(dead_code)]

extern crate alloc;

mod runtime;

use alloc::boxed::Box;

use egui::{
    Context, Ui, Vec2, Color32, Sense, RawInput, CornerRadius, Margin,
    Event, PointerButton, Modifiers,
};
use egui_software_backend::{BufferMutRef, ColorFieldOrder, EguiSoftwareRender};

// ---- Menu action types -------------------------------------------------

/// Actions that a context menu item can trigger.
/// These are returned to the caller (the C shim) so it can perform
/// the appropriate IPC/syscall to carry out the action.
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

/// State of the context menu: which item (if any) was selected.
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

// ---- Context menu builder ----------------------------------------------

/// Show the context menu using egui's real `Popup::context_menu` API.
///
/// We allocate a full-surface widget and call `response.context_menu()` on it.
/// egui handles popup positioning at the pointer, submenu hover/click,
/// and close-on-click-outside behavior.
pub fn show_desktop_context_menu(
    ui: &mut Ui,
    result: &mut MenuResult,
) {
    let ctx = ui.ctx().clone();

    // Set style on the context BEFORE the popup is created.
    // Frame::popup() reads style.visuals.popup_shadow at popup creation time,
    // so setting it inside the closure (after frame creation) has no effect.
    ctx.style_mut_of(egui::Theme::Dark, |style| {
        style.visuals.popup_shadow = egui::Shadow::default(); // no shadow — our CPU renderer can't blur
        style.visuals.window_fill = Color32::from_rgb(40, 40, 40); // solid opaque panel
        style.visuals.window_stroke = egui::Stroke::new(1.0, Color32::from_rgb(80, 80, 80));
        style.visuals.menu_corner_radius = CornerRadius::same(8);
        style.visuals.selection.bg_fill = Color32::from_rgb(0, 0x69, 0xF9); // macOS blue hover
        style.spacing.item_spacing = Vec2::new(0.0, 2.0);
        style.spacing.button_padding = Vec2::new(12.0, 4.0);
        style.spacing.menu_margin = Margin { left: 6, right: 6, top: 6, bottom: 6 };
        style.visuals.widgets.hovered.corner_radius = CornerRadius::same(5);
        style.visuals.widgets.active.corner_radius = CornerRadius::same(5);
        style.visuals.widgets.inactive.corner_radius = CornerRadius::same(5);
    });

    // Allocate a widget that fills the entire surface so right-clicks are detected
    let max_rect = ui.max_rect();
    let response = ui.allocate_rect(max_rect, Sense::click());

    // Use egui's real context_menu API — opens on secondary click,
    // positions at the pointer, handles submenus and close behavior
    response.context_menu(|ui| {
        if ui.add(menu_button("New Folder")).clicked() {
            result.action = MenuAction::NewFolder;
            result.closed = true;
            ui.close();
        }
        if ui.add(menu_button("Get Info")).clicked() {
            result.action = MenuAction::GetInfo;
            result.closed = true;
            ui.close();
        }
        if ui.add(menu_button("Change Wallpaper")).clicked() {
            result.action = MenuAction::ChangeWallpaper;
            result.closed = true;
            ui.close();
        }

        ui.separator();

        if ui.add(menu_button_check("Use Stacks", false)).clicked() {
            result.action = MenuAction::UseStacks;
            result.closed = true;
            ui.close();
        }

        // Clean Up By ▸ (submenu) — ui.menu_button creates SubMenuButton inside menu context
        ui.menu_button("Clean Up By", |ui| {
            if ui.add(menu_button("Name")).clicked() {
                result.action = MenuAction::CleanUpByName;
                result.closed = true;
                ui.close();
            }
            if ui.add(menu_button("Date Modified")).clicked() {
                result.action = MenuAction::CleanUpByDateModified;
                result.closed = true;
                ui.close();
            }
            if ui.add(menu_button("Kind")).clicked() {
                result.action = MenuAction::CleanUpByKind;
                result.closed = true;
                ui.close();
            }
            if ui.add(menu_button("Size")).clicked() {
                result.action = MenuAction::CleanUpBySize;
                result.closed = true;
                ui.close();
            }
        });

        ui.separator();

        // Sort By ▸ (submenu)
        ui.menu_button("Sort By", |ui| {
            if ui.add(menu_button("Name")).clicked() {
                result.action = MenuAction::SortByName;
                result.closed = true;
                ui.close();
            }
            if ui.add(menu_button("Date Modified")).clicked() {
                result.action = MenuAction::SortByDate;
                result.closed = true;
                ui.close();
            }
            if ui.add(menu_button("Size")).clicked() {
                result.action = MenuAction::SortBySize;
                result.closed = true;
                ui.close();
            }
            if ui.add(menu_button("Kind")).clicked() {
                result.action = MenuAction::SortByKind;
                result.closed = true;
                ui.close();
            }
        });

        ui.separator();

        if ui.add(menu_button("Import from iPhone")).clicked() {
            result.action = MenuAction::ImportFromIPhone;
            result.closed = true;
            ui.close();
        }

        ui.separator();

        if ui.add(menu_button("Show View Options")).clicked() {
            result.action = MenuAction::ShowViewOptions;
            result.closed = true;
            ui.close();
        }
    });
}

/// Create a standard menu button widget (macOS-style).
fn menu_button(text: &str) -> egui::Button<'_> {
    egui::Button::new(text)
        .min_size(Vec2::new(160.0, 0.0))
        .frame(false)
        .corner_radius(CornerRadius::same(5))
}

/// Create a menu button with a checkmark (for toggle items).
fn menu_button_check(text: &str, checked: bool) -> egui::Button<'_> {
    let label = if checked {
        alloc::format!("✓ {}", text)
    } else {
        alloc::format!("   {}", text)
    };
    egui::Button::new(label)
        .min_size(Vec2::new(160.0, 0.0))
        .frame(false)
        .corner_radius(CornerRadius::same(5))
}

// ---- CPU rasterization -------------------------------------------------
//
// Rendering is delegated to `egui_software_backend`, which tessellates
// egui's shapes into textured triangle meshes (using egui's real font
// atlas/glyph cache) and rasterizes them with anti-aliasing. See
// `xos_context_menu_run_frame` below.

// ---- C FFI interface ---------------------------------------------------

/// Opaque context menu state, shared between C shim and Rust.
#[repr(C)]
pub struct ContextMenuState {
    /// The egui context
    ctx: *mut Context,
    /// Last action selected
    action: u32,
    /// Whether the menu was closed (item clicked)
    closed: u32,
    /// Screen width
    screen_w: u32,
    /// Screen height
    screen_h: u32,
    /// Mouse x (relative to surface)
    mouse_x: i32,
    /// Mouse y (relative to surface)
    mouse_y: i32,
    /// Pending mouse button (0=none, 1=left, 2=right)
    mouse_button: u32,
    /// Pending mouse action (0=move, 1=down, 2=up)
    mouse_action: u32,
    /// Whether to trigger a right-click to open the menu
    trigger_open: u32,
    /// The egui software CPU rasterizer (tessellates + rasterizes real egui
    /// primitives, including the font atlas, instead of a hand-rolled renderer)
    renderer: *mut EguiSoftwareRender,
}

/// Initialize a new egui context for the context menu service.
#[unsafe(no_mangle)]
pub extern "C" fn xos_context_menu_init(screen_w: u32, screen_h: u32) -> *mut ContextMenuState {
    let ctx = Context::default();
    let renderer = EguiSoftwareRender::new(ColorFieldOrder::Bgra);
    let state = Box::new(ContextMenuState {
        ctx: Box::into_raw(Box::new(ctx)),
        action: 0,
        closed: 0,
        screen_w,
        screen_h,
        mouse_x: 0,
        mouse_y: 0,
        mouse_button: 0,
        mouse_action: 0,
        trigger_open: 0,
        renderer: Box::into_raw(Box::new(renderer)),
    });
    Box::into_raw(state)
}

/// Destroy the context menu state.
#[unsafe(no_mangle)]
pub extern "C" fn xos_context_menu_destroy(state: *mut ContextMenuState) {
    if state.is_null() {
        return;
    }
    unsafe {
        let state = Box::from_raw(state);
        if !state.ctx.is_null() {
            let _ = Box::from_raw(state.ctx);
        }
        if !state.renderer.is_null() {
            let _ = Box::from_raw(state.renderer);
        }
    }
}

/// Feed a mouse event to the context menu.
/// button: 0=none, 1=left, 2=right
/// action: 0=move, 1=down, 2=up
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

/// Trigger the context menu to open (called when right-click is received via IPC).
/// Sets trigger_open so run_frame feeds a secondary click to open the popup.
#[unsafe(no_mangle)]
pub extern "C" fn xos_context_menu_trigger(state: *mut ContextMenuState) {
    if state.is_null() {
        return;
    }
    unsafe {
        let state = &mut *state;
        state.trigger_open = 1;
        state.mouse_x = 0;
        state.mouse_y = 0;
        state.mouse_button = 0;
        state.mouse_action = 0;
    }
}

/// Run one frame of the context menu. Returns 1 if an item was clicked
/// (check `action` field for which one), 0 otherwise.
#[unsafe(no_mangle)]
pub extern "C" fn xos_context_menu_run_frame(
    state: *mut ContextMenuState,
    pixels: *mut u32,
    width: u32,
    height: u32,
) -> u32 {
    if state.is_null() || pixels.is_null() {
        return 0;
    }
    unsafe {
        let state = &mut *state;
        if state.ctx.is_null() {
            return 0;
        }
        let ctx = &*state.ctx;

        // Reset per-frame state
        state.closed = 0;
        state.action = 0;

        let mut menu_result = MenuResult::default();

        // Build RawInput with mouse events
        let mut input = RawInput::default();
        // Set screen_rect to the FULL screen size, not just the surface.
        // egui uses screen_rect to decide if submenus fit to the right.
        // If screen_rect is too small, egui repositions submenus below instead.
        input.screen_rect = Some(egui::Rect::from_min_size(
            egui::pos2(0.0, 0.0),
            Vec2::new(state.screen_w as f32, state.screen_h as f32),
        ));

        if state.trigger_open == 1 {
            // Feed a full secondary click (press + release) at (0,0) to trigger
            // Popup::context_menu's secondary_clicked() detection
            let pos = egui::pos2(0.0, 0.0);
            input.events.push(Event::PointerMoved(pos));
            input.events.push(Event::PointerButton {
                pos,
                button: PointerButton::Secondary,
                pressed: true,
                modifiers: Modifiers::default(),
            });
            input.events.push(Event::PointerButton {
                pos,
                button: PointerButton::Secondary,
                pressed: false,
                modifiers: Modifiers::default(),
            });
        } else {
            // Feed regular mouse events
            let mouse_pos = egui::pos2(state.mouse_x as f32, state.mouse_y as f32);
            input.events.push(Event::PointerMoved(mouse_pos));

            if state.mouse_button != 0 {
                let button = if state.mouse_button == 2 {
                    PointerButton::Secondary
                } else {
                    PointerButton::Primary
                };
                let pressed = state.mouse_action == 1;
                input.events.push(Event::PointerButton {
                    pos: mouse_pos,
                    button,
                    pressed,
                    modifiers: Modifiers::default(),
                });
            }
        }

        // Capture whether this is a trigger frame before clearing
        let trigger_was_set = state.trigger_open == 1;

        // Clear pending events
        state.mouse_button = 0;
        state.mouse_action = 0;
        state.trigger_open = 0;

        let was_triggering = trigger_was_set;

        // Run egui frame — pass the Ui directly to show_desktop_context_menu
        let output = ctx.run_ui(input, |ui| {
            show_desktop_context_menu(ui, &mut menu_result);
        });

        // Check if an item was clicked
        if menu_result.closed {
            state.closed = 1;
            state.action = menu_result.action as u32;
        } else if was_triggering && !ctx.any_popup_open() {
            // We tried to trigger the menu this frame but it didn't open
            // This shouldn't normally happen, but if it does, don't close
        } else if !was_triggering && !ctx.any_popup_open() {
            // Popup was closed without selecting an item (clicked outside)
            state.closed = 1;
        }

        // Tessellate egui's shapes into textured triangle primitives (this is
        // what actually turns text into real glyphs sampled from egui's font
        // atlas, rather than any hand-drawn substitute) and rasterize them
        // with `egui_software_backend`.
        let pixels_per_point = output.pixels_per_point;
        let primitives = ctx.tessellate(output.shapes, pixels_per_point);

        // Clear to transparent — only the popup's Frame is visible.
        let pixel_slice: &mut [[u8; 4]] = core::slice::from_raw_parts_mut(
            pixels as *mut [u8; 4],
            (width * height) as usize,
        );
        for px in pixel_slice.iter_mut() {
            *px = [0, 0, 0, 0];
        }
        let mut buffer_ref = BufferMutRef::new(pixel_slice, width as usize, height as usize);

        let renderer = &mut *state.renderer;
        renderer.render(&mut buffer_ref, &primitives, &output.textures_delta, pixels_per_point);

        state.closed
    }
}

/// Get the last selected action.
#[unsafe(no_mangle)]
pub extern "C" fn xos_context_menu_get_action(state: *mut ContextMenuState) -> u32 {
    if state.is_null() {
        return 0;
    }
    unsafe { (*state).action }
}

/// Check if the menu is currently open.
#[unsafe(no_mangle)]
pub extern "C" fn xos_context_menu_is_open(state: *mut ContextMenuState) -> u32 {
    if state.is_null() {
        return 0;
    }
    unsafe {
        let state = &*state;
        if state.ctx.is_null() {
            return 0;
        }
        let ctx = &*state.ctx;
        if ctx.any_popup_open() {
            1
        } else {
            0
        }
    }
}
