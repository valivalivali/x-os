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
use alloc::string::String;
use alloc::vec::Vec;

use egui::{Context, Ui, Vec2, Color32, Pos2, Rect, Sense, RawInput, CornerRadius, Frame, Margin};

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

/// Build and show a desktop right-click context menu using egui's
/// real `Response::context_menu` API.
///
/// Call this every frame on a full-screen invisible `Sense::click` area
/// so egui can detect the right-click and show the popup automatically.
pub fn show_desktop_context_menu(
    ui: &mut Ui,
    result: &mut MenuResult,
) {
    // Allocate an invisible interactive area covering the full desktop.
    // egui's `context_menu` will open a popup on right-click.
    let screen_rect = ui.max_rect();
    let response = ui.allocate_rect(screen_rect, Sense::click());

    // Use egui's built-in context_menu API on the response
    response.context_menu(|ui| {
        style_menu(ui);

        // New Folder
        if ui.add(menu_button("New Folder")).clicked() {
            result.action = MenuAction::NewFolder;
            result.closed = true;
            ui.close();
        }

        // Get Info
        if ui.add(menu_button("Get Info")).clicked() {
            result.action = MenuAction::GetInfo;
            result.closed = true;
            ui.close();
        }

        // Change Wallpaper
        if ui.add(menu_button("Change Wallpaper…")).clicked() {
            result.action = MenuAction::ChangeWallpaper;
            result.closed = true;
            ui.close();
        }

        // Separator
        ui.separator();

        // Use Stacks (toggle item)
        let stacks_enabled = false; // TODO: get from desktop state
        if ui.add(menu_button_check("Use Stacks", stacks_enabled)).clicked() {
            result.action = MenuAction::UseStacks;
            result.closed = true;
            ui.close();
        }

        // Clean Up By ▸ (submenu)
        ui.menu_button("Clean Up By", |ui| {
            style_menu(ui);

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

        // Separator
        ui.separator();

        // Sort By ▸ (submenu)
        ui.menu_button("Sort By", |ui| {
            style_menu(ui);

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

        // Separator
        ui.separator();

        // Import from iPhone
        if ui.add(menu_button("Import from iPhone…")).clicked() {
            result.action = MenuAction::ImportFromIPhone;
            result.closed = true;
            ui.close();
        }

        // Separator
        ui.separator();

        // Show View Options
        if ui.add(menu_button("Show View Options")).clicked() {
            result.action = MenuAction::ShowViewOptions;
            result.closed = true;
            ui.close();
        }
    });
}

// ---- Menu styling helpers ----------------------------------------------

/// Apply macOS-like styling to a menu popup.
fn style_menu(ui: &mut Ui) {
    let style = ui.style_mut();

    // Rounded corners on menu items
    style.visuals.widgets.hovered.corner_radius = CornerRadius::same(5);
    style.visuals.widgets.active.corner_radius = CornerRadius::same(5);
    style.visuals.widgets.inactive.corner_radius = CornerRadius::same(5);

    // Hover highlight color (macOS blue)
    style.visuals.selection = {
        let mut s = style.visuals.selection;
        s.bg_fill = Color32::from_rgb(0, 0x69, 0xF9); // macOS blue
        s
    };

    // Slightly translucent background for the menu popup
    style.visuals.window_fill = Color32::from_rgba_premultiplied(40, 40, 40, 240);

    // Item spacing
    style.spacing.item_spacing = Vec2::new(0.0, 2.0);
    style.spacing.button_padding = Vec2::new(12.0, 4.0);

    // Menu width
    style.spacing.menu_margin = Margin {
        left: 6,
        right: 6,
        top: 6,
        bottom: 6,
    };
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
    /// Mouse x
    mouse_x: i32,
    /// Mouse y
    mouse_y: i32,
    /// Whether right-click happened this frame
    right_clicked: u32,
}

/// Initialize a new egui context for the context menu service.
#[unsafe(no_mangle)]
pub extern "C" fn xos_context_menu_init(screen_w: u32, screen_h: u32) -> *mut ContextMenuState {
    let ctx = Context::default();
    let state = Box::new(ContextMenuState {
        ctx: Box::into_raw(Box::new(ctx)),
        action: 0,
        closed: 0,
        screen_w,
        screen_h,
        mouse_x: 0,
        mouse_y: 0,
        right_clicked: 0,
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
    }
}

/// Feed a mouse event to the context menu.
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
        // action 1 = down, button 2 = right
        if action == 1 && button == 2 {
            state.right_clicked = 1;
        }
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

        // Run egui frame — run_ui handles begin_pass/end_pass internally
        let mut input = RawInput::default();
        input.screen_rect = Some(egui::Rect::from_min_size(
            egui::pos2(0.0, 0.0),
            Vec2::new(state.screen_w as f32, state.screen_h as f32),
        ));
        let _output = ctx.run_ui(input, |ui| {
            show_desktop_context_menu(ui, &mut menu_result);
        });

        // Check result
        if menu_result.closed {
            state.closed = 1;
            state.action = menu_result.action as u32;
        }

        // TODO: Render egui output to pixels buffer using software backend
        // For now, the C shim handles rendering

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
