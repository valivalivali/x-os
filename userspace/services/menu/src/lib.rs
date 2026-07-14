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

use egui::{
    Context, Ui, Vec2, Color32, Pos2, Rect, Sense, RawInput, CornerRadius, Frame, Margin,
    Event, PointerButton, Modifiers, Shape, Id, Area, Order, Layout, Align,
};

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

/// Show the context menu directly as an Area at position (0,0) on the surface.
/// The C shim creates the surface at the cursor position on right-click,
/// so we just need to draw the menu contents at the top-left of the surface.
pub fn show_desktop_context_menu(
    ctx: &Context,
    result: &mut MenuResult,
) {
    // Use egui::Area to place the menu at (0,0) on the surface
    Area::new(Id::new("xos_context_menu"))
        .order(Order::Foreground)
        .fixed_pos(Pos2::new(0.0, 0.0))
        .show(ctx, |ui| {
            style_menu(ui);

            // Wrap menu contents in a frame for the background
            Frame::group(ui.style())
                .fill(Color32::from_rgba_premultiplied(40, 40, 40, 240))
                .corner_radius(CornerRadius::same(8))
                .inner_margin(Margin {
                    left: 6,
                    right: 6,
                    top: 6,
                    bottom: 6,
                })
                .show(ui, |ui| {
                    ui.with_layout(Layout::top_down_justified(Align::Min), |ui| {
                    // New Folder
                    if ui.add(menu_button("New Folder")).clicked() {
                        result.action = MenuAction::NewFolder;
                        result.closed = true;
                    }

                    // Get Info
                    if ui.add(menu_button("Get Info")).clicked() {
                        result.action = MenuAction::GetInfo;
                        result.closed = true;
                    }

                    // Change Wallpaper
                    if ui.add(menu_button("Change Wallpaper…")).clicked() {
                        result.action = MenuAction::ChangeWallpaper;
                        result.closed = true;
                    }

                    // Separator
                    ui.separator();

                    // Use Stacks (toggle item)
                    let stacks_enabled = false;
                    if ui.add(menu_button_check("Use Stacks", stacks_enabled)).clicked() {
                        result.action = MenuAction::UseStacks;
                        result.closed = true;
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
                    }

                    // Separator
                    ui.separator();

                    // Show View Options
                    if ui.add(menu_button("Show View Options")).clicked() {
                        result.action = MenuAction::ShowViewOptions;
                        result.closed = true;
                    }
                    });
                });
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

// ---- Simple software renderer -------------------------------------------

/// Simple 5x7 bitmap font for ASCII characters.
/// Each character is 5 columns x 7 rows, encoded as 7 bytes (one per row).
/// 1 bits = foreground pixel.
const FONT_5X7: &[(char, [u8; 7])] = &[
    (' ', [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]),
    ('!', [0x00, 0x00, 0x2F, 0x00, 0x00, 0x00, 0x00]),
    ('"', [0x00, 0x07, 0x00, 0x07, 0x00, 0x00, 0x00]),
    ('#', [0x14, 0x7F, 0x14, 0x7F, 0x14, 0x00, 0x00]),
    ('$', [0x24, 0x2A, 0x7F, 0x2A, 0x12, 0x00, 0x00]),
    ('%', [0x23, 0x13, 0x08, 0x64, 0x62, 0x00, 0x00]),
    ('&', [0x36, 0x49, 0x55, 0x22, 0x50, 0x00, 0x00]),
    ('\'', [0x00, 0x05, 0x03, 0x00, 0x00, 0x00, 0x00]),
    ('(', [0x00, 0x1C, 0x22, 0x41, 0x00, 0x00, 0x00]),
    (')', [0x00, 0x41, 0x22, 0x1C, 0x00, 0x00, 0x00]),
    ('*', [0x08, 0x2A, 0x1C, 0x2A, 0x08, 0x00, 0x00]),
    ('+', [0x08, 0x08, 0x3E, 0x08, 0x08, 0x00, 0x00]),
    (',', [0x00, 0x50, 0x30, 0x00, 0x00, 0x00, 0x00]),
    ('-', [0x00, 0x08, 0x08, 0x08, 0x08, 0x00, 0x00]),
    ('.', [0x00, 0x60, 0x60, 0x00, 0x00, 0x00, 0x00]),
    ('/', [0x20, 0x10, 0x08, 0x04, 0x02, 0x00, 0x00]),
    ('0', [0x3E, 0x51, 0x49, 0x45, 0x3E, 0x00, 0x00]),
    ('1', [0x00, 0x42, 0x7F, 0x40, 0x00, 0x00, 0x00]),
    ('2', [0x42, 0x61, 0x51, 0x49, 0x46, 0x00, 0x00]),
    ('3', [0x21, 0x41, 0x45, 0x4B, 0x31, 0x00, 0x00]),
    ('4', [0x18, 0x14, 0x12, 0x7F, 0x10, 0x00, 0x00]),
    ('5', [0x27, 0x45, 0x45, 0x45, 0x39, 0x00, 0x00]),
    ('6', [0x3C, 0x4A, 0x49, 0x49, 0x30, 0x00, 0x00]),
    ('7', [0x01, 0x71, 0x09, 0x05, 0x03, 0x00, 0x00]),
    ('8', [0x36, 0x49, 0x49, 0x49, 0x36, 0x00, 0x00]),
    ('9', [0x06, 0x49, 0x49, 0x29, 0x1E, 0x00, 0x00]),
    (':', [0x00, 0x36, 0x36, 0x00, 0x00, 0x00, 0x00]),
    (';', [0x00, 0x56, 0x36, 0x00, 0x00, 0x00, 0x00]),
    ('<', [0x00, 0x08, 0x14, 0x22, 0x41, 0x00, 0x00]),
    ('=', [0x14, 0x14, 0x14, 0x14, 0x14, 0x00, 0x00]),
    ('>', [0x41, 0x22, 0x14, 0x08, 0x00, 0x00, 0x00]),
    ('?', [0x02, 0x01, 0x51, 0x09, 0x06, 0x00, 0x00]),
    ('@', [0x32, 0x49, 0x79, 0x41, 0x3E, 0x00, 0x00]),
    ('A', [0x7E, 0x11, 0x11, 0x11, 0x7E, 0x00, 0x00]),
    ('B', [0x7F, 0x49, 0x49, 0x49, 0x36, 0x00, 0x00]),
    ('C', [0x3E, 0x41, 0x41, 0x41, 0x22, 0x00, 0x00]),
    ('D', [0x7F, 0x41, 0x41, 0x22, 0x1C, 0x00, 0x00]),
    ('E', [0x7F, 0x49, 0x49, 0x49, 0x41, 0x00, 0x00]),
    ('F', [0x7F, 0x09, 0x09, 0x01, 0x01, 0x00, 0x00]),
    ('G', [0x3E, 0x41, 0x41, 0x51, 0x32, 0x00, 0x00]),
    ('H', [0x7F, 0x08, 0x08, 0x08, 0x7F, 0x00, 0x00]),
    ('I', [0x00, 0x41, 0x7F, 0x41, 0x00, 0x00, 0x00]),
    ('J', [0x20, 0x40, 0x41, 0x3F, 0x01, 0x00, 0x00]),
    ('K', [0x7F, 0x08, 0x14, 0x22, 0x41, 0x00, 0x00]),
    ('L', [0x7F, 0x40, 0x40, 0x40, 0x40, 0x00, 0x00]),
    ('M', [0x7F, 0x02, 0x04, 0x02, 0x7F, 0x00, 0x00]),
    ('N', [0x7F, 0x04, 0x08, 0x10, 0x7F, 0x00, 0x00]),
    ('O', [0x3E, 0x41, 0x41, 0x41, 0x3E, 0x00, 0x00]),
    ('P', [0x7F, 0x09, 0x09, 0x09, 0x06, 0x00, 0x00]),
    ('Q', [0x3E, 0x41, 0x51, 0x21, 0x5E, 0x00, 0x00]),
    ('R', [0x7F, 0x09, 0x19, 0x29, 0x46, 0x00, 0x00]),
    ('S', [0x46, 0x49, 0x49, 0x49, 0x31, 0x00, 0x00]),
    ('T', [0x01, 0x01, 0x7F, 0x01, 0x01, 0x00, 0x00]),
    ('U', [0x3F, 0x40, 0x40, 0x40, 0x3F, 0x00, 0x00]),
    ('V', [0x1F, 0x20, 0x40, 0x20, 0x1F, 0x00, 0x00]),
    ('W', [0x7F, 0x20, 0x18, 0x20, 0x7F, 0x00, 0x00]),
    ('X', [0x63, 0x14, 0x08, 0x14, 0x63, 0x00, 0x00]),
    ('Y', [0x03, 0x04, 0x78, 0x04, 0x03, 0x00, 0x00]),
    ('Z', [0x61, 0x51, 0x49, 0x45, 0x43, 0x00, 0x00]),
    ('[', [0x00, 0x7F, 0x41, 0x41, 0x00, 0x00, 0x00]),
    ('\\', [0x02, 0x04, 0x08, 0x10, 0x20, 0x00, 0x00]),
    (']', [0x00, 0x41, 0x41, 0x7F, 0x00, 0x00, 0x00]),
    ('^', [0x04, 0x02, 0x01, 0x02, 0x04, 0x00, 0x00]),
    ('_', [0x40, 0x40, 0x40, 0x40, 0x40, 0x00, 0x00]),
    ('`', [0x00, 0x01, 0x02, 0x04, 0x00, 0x00, 0x00]),
    ('a', [0x20, 0x54, 0x54, 0x54, 0x78, 0x00, 0x00]),
    ('b', [0x7F, 0x48, 0x44, 0x44, 0x38, 0x00, 0x00]),
    ('c', [0x38, 0x44, 0x44, 0x44, 0x20, 0x00, 0x00]),
    ('d', [0x38, 0x44, 0x44, 0x48, 0x7F, 0x00, 0x00]),
    ('e', [0x38, 0x54, 0x54, 0x54, 0x18, 0x00, 0x00]),
    ('f', [0x08, 0x7E, 0x09, 0x01, 0x02, 0x00, 0x00]),
    ('g', [0x08, 0x14, 0x54, 0x54, 0x3C, 0x00, 0x00]),
    ('h', [0x7F, 0x08, 0x04, 0x04, 0x78, 0x00, 0x00]),
    ('i', [0x00, 0x44, 0x7D, 0x40, 0x00, 0x00, 0x00]),
    ('j', [0x20, 0x40, 0x44, 0x3D, 0x00, 0x00, 0x00]),
    ('k', [0x7F, 0x10, 0x28, 0x44, 0x00, 0x00, 0x00]),
    ('l', [0x00, 0x41, 0x7F, 0x40, 0x00, 0x00, 0x00]),
    ('m', [0x7C, 0x04, 0x18, 0x04, 0x78, 0x00, 0x00]),
    ('n', [0x7C, 0x08, 0x04, 0x04, 0x78, 0x00, 0x00]),
    ('o', [0x38, 0x44, 0x44, 0x44, 0x38, 0x00, 0x00]),
    ('p', [0x7C, 0x14, 0x14, 0x14, 0x08, 0x00, 0x00]),
    ('q', [0x08, 0x14, 0x14, 0x18, 0x7C, 0x00, 0x00]),
    ('r', [0x7C, 0x08, 0x04, 0x04, 0x08, 0x00, 0x00]),
    ('s', [0x48, 0x54, 0x54, 0x54, 0x20, 0x00, 0x00]),
    ('t', [0x04, 0x7F, 0x44, 0x40, 0x20, 0x00, 0x00]),
    ('u', [0x3C, 0x40, 0x40, 0x20, 0x7C, 0x00, 0x00]),
    ('v', [0x1C, 0x20, 0x40, 0x20, 0x1C, 0x00, 0x00]),
    ('w', [0x3C, 0x40, 0x30, 0x40, 0x3C, 0x00, 0x00]),
    ('x', [0x44, 0x28, 0x10, 0x28, 0x44, 0x00, 0x00]),
    ('y', [0x0C, 0x50, 0x50, 0x50, 0x3C, 0x00, 0x00]),
    ('z', [0x44, 0x64, 0x54, 0x4C, 0x44, 0x00, 0x00]),
    ('{', [0x00, 0x08, 0x36, 0x41, 0x00, 0x00, 0x00]),
    ('|', [0x00, 0x00, 0x7F, 0x00, 0x00, 0x00, 0x00]),
    ('}', [0x00, 0x41, 0x36, 0x08, 0x00, 0x00, 0x00]),
    ('~', [0x02, 0x01, 0x02, 0x04, 0x02, 0x00, 0x00]),
];

fn lookup_glyph(c: char) -> [u8; 7] {
    for &(ch, data) in FONT_5X7 {
        if ch == c {
            return data;
        }
    }
    [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]
}

/// Draw a single character at (x, y) with the given color.
fn draw_char(
    pixels: &mut [u32],
    width: u32,
    height: u32,
    x: i32,
    y: i32,
    c: char,
    color: Color32,
) {
    let glyph = lookup_glyph(c);
    let col = color_to_u32(color);
    for row in 0..7i32 {
        let bits = glyph[row as usize];
        for col_idx in 0..5i32 {
            if (bits >> (4 - col_idx)) & 1 != 0 {
                let px = x + col_idx;
                let py = y + row;
                if px >= 0 && py >= 0 && (px as u32) < width && (py as u32) < height {
                    let idx = (py as u32 * width + px as u32) as usize;
                    if idx < pixels.len() {
                        pixels[idx] = col;
                    }
                }
            }
        }
    }
}

/// Draw a text string starting at (x, y).
fn draw_text(
    pixels: &mut [u32],
    width: u32,
    height: u32,
    x: i32,
    y: i32,
    text: &str,
    color: Color32,
) {
    let mut cx = x;
    for c in text.chars() {
        draw_char(pixels, width, height, cx, y, c, color);
        cx += 6; // 5 px wide + 1 px spacing
    }
}

/// Render egui shapes to a pixel buffer using simple CPU rasterization.
fn render_shapes(
    pixels: &mut [u32],
    width: u32,
    height: u32,
    shapes: &[egui::epaint::ClippedShape],
    background: Color32,
) {
    let bg = color_to_u32(background);
    for px in pixels.iter_mut() {
        *px = bg;
    }

    for clipped in shapes {
        let shape = &clipped.shape;
        match shape {
            Shape::Rect(rect_shape) => {
                if rect_shape.fill != Color32::TRANSPARENT {
                    fill_rect(pixels, width, height, rect_shape.rect, rect_shape.fill);
                }
            }
            Shape::Text(text_shape) => {
                let text_color = text_shape.override_text_color
                    .unwrap_or(text_shape.fallback_color);
                if text_color == Color32::TRANSPARENT {
                    continue;
                }
                let text = &text_shape.galley.job.text;
                let pos = text_shape.pos;
                // Draw each row of the galley
                for row in &text_shape.galley.rows {
                    let row_x = (pos.x + row.pos.x) as i32;
                    let row_y = (pos.y + row.pos.y) as i32;
                    draw_text(pixels, width, height, row_x, row_y, text, text_color);
                }
            }
            Shape::LineSegment { points, stroke } => {
                if stroke.color != Color32::TRANSPARENT && stroke.width > 0.0 {
                    draw_line(
                        pixels, width, height,
                        points[0], points[1],
                        stroke.color,
                        stroke.width as i32,
                    );
                }
            }
            Shape::Noop => {}
            _ => {}
        }
    }
}

/// Draw a line between two points (Bresenham-style).
fn draw_line(
    pixels: &mut [u32],
    width: u32,
    height: u32,
    p0: Pos2,
    p1: Pos2,
    color: Color32,
    thickness: i32,
) {
    let col = color_to_u32(color);
    let mut x0 = p0.x as i32;
    let mut y0 = p0.y as i32;
    let x1 = p1.x as i32;
    let y1 = p1.y as i32;
    let dx = (x1 - x0).abs();
    let dy = (y1 - y0).abs();
    let sx = if x0 < x1 { 1 } else { -1 };
    let sy = if y0 < y1 { 1 } else { -1 };
    let mut err = dx - dy;
    loop {
        for ty in 0..thickness {
            for tx in 0..thickness {
                let px = x0 + tx - thickness / 2;
                let py = y0 + ty - thickness / 2;
                if px >= 0 && py >= 0 && (px as u32) < width && (py as u32) < height {
                    let idx = (py as u32 * width + px as u32) as usize;
                    if idx < pixels.len() {
                        pixels[idx] = col;
                    }
                }
            }
        }
        if x0 == x1 && y0 == y1 { break; }
        let e2 = 2 * err;
        if e2 > -dy { err -= dy; x0 += sx; }
        if e2 < dx { err += dx; y0 += sy; }
    }
}

fn color_to_u32(c: Color32) -> u32 {
    ((c.r() as u32) << 16) | ((c.g() as u32) << 8) | (c.b() as u32)
}

fn blend_pixel(dst: u32, src: Color32) -> u32 {
    let sa = src.a() as u32;
    if sa == 0 {
        return dst;
    }
    if sa == 255 {
        return color_to_u32(src);
    }
    let da = 255 - sa;
    let dr = (dst >> 16) & 0xFF;
    let dg = (dst >> 8) & 0xFF;
    let db = dst & 0xFF;
    let r = (src.r() as u32 * sa + dr * da) / 255;
    let g = (src.g() as u32 * sa + dg * da) / 255;
    let b = (src.b() as u32 * sa + db * da) / 255;
    (r << 16) | (g << 8) | b
}

fn fill_rect(
    pixels: &mut [u32],
    width: u32,
    height: u32,
    rect: Rect,
    color: Color32,
) {
    let x0 = rect.min.x.max(0.0) as u32;
    let y0 = rect.min.y.max(0.0) as u32;
    let x1 = (rect.max.x as u32).min(width);
    let y1 = (rect.max.y as u32).min(height);

    for y in y0..y1 {
        for x in x0..x1 {
            let idx = (y * width + x) as usize;
            if idx < pixels.len() {
                pixels[idx] = blend_pixel(pixels[idx], color);
            }
        }
    }
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
        mouse_button: 0,
        mouse_action: 0,
        trigger_open: 0,
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
/// The menu Area is always shown, so we just need to reset mouse state.
#[unsafe(no_mangle)]
pub extern "C" fn xos_context_menu_trigger(state: *mut ContextMenuState) {
    if state.is_null() {
        return;
    }
    unsafe {
        let state = &mut *state;
        state.trigger_open = 0;
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
        // Use the surface dimensions as screen_rect so egui renders within the surface
        let mut input = RawInput::default();
        input.screen_rect = Some(egui::Rect::from_min_size(
            egui::pos2(0.0, 0.0),
            Vec2::new(width as f32, height as f32),
        ));

        // Feed mouse events into egui
        let mouse_pos = egui::pos2(state.mouse_x as f32, state.mouse_y as f32);
        input.events.push(Event::PointerMoved(mouse_pos));

        if state.mouse_button != 0 || state.trigger_open == 1 {
            let button = if state.trigger_open == 1 || state.mouse_button == 2 {
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

        // Clear pending events
        state.mouse_button = 0;
        state.mouse_action = 0;
        state.trigger_open = 0;

        // Run egui frame
        let output = ctx.run_ui(input, |_ui| {
            show_desktop_context_menu(ctx, &mut menu_result);
        });

        // Check result
        if menu_result.closed {
            state.closed = 1;
            state.action = menu_result.action as u32;
        }

        // Render egui shapes to pixel buffer
        // Fill with opaque dark background first so the surface is visible
        let pixel_slice = core::slice::from_raw_parts_mut(pixels, (width * height) as usize);
        render_shapes(
            pixel_slice,
            width,
            height,
            &output.shapes,
            Color32::from_rgb(30, 30, 30),
        );

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
