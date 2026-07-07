/* xgfx — X Graphics Engine
 * Scanline rasterizer with coverage-based anti-aliasing.
 * Pure C, no heap, no libc dependencies.
 */

#pragma once
#include <stdint.h>
#include <stddef.h>

/* ---- Types -------------------------------------------------------------- */

typedef struct {
    uint32_t *pixels;
    int       w, h, stride;
} xgfx_surface_t;

typedef struct {
    float x, y;
} xgfx_point_t;

/* Paint: solid color or linear gradient */
typedef enum {
    XGFX_PAINT_SOLID,
    XGFX_PAINT_LINEAR
} xgfx_paint_type_t;

typedef struct {
    xgfx_paint_type_t type;
    uint32_t color;              /* solid */
    float    x1, y1, x2, y2;     /* gradient line */
    uint32_t c1, c2;             /* gradient colors */
} xgfx_paint_t;

/* Path: stores move/line/cubic/close commands and points */
#define XGFX_MAX_PATH_CMDS  512
#define XGFX_MAX_PATH_PTS   2048
#define XGFX_MAX_EDGES      4096

typedef enum {
    XGFX_CMD_MOVE,
    XGFX_CMD_LINE,
    XGFX_CMD_CUBIC,
    XGFX_CMD_CLOSE
} xgfx_cmd_type_t;

typedef struct {
    uint8_t      cmds[XGFX_MAX_PATH_CMDS];
    xgfx_point_t pts[XGFX_MAX_PATH_PTS];
    int          ncmds;
    int          npts;
    float        min_x, min_y, max_x, max_y;
} xgfx_path_t;

/* ---- Path --------------------------------------------------------------- */

void xgfx_path_init(xgfx_path_t *p);
void xgfx_path_move_to(xgfx_path_t *p, float x, float y);
void xgfx_path_line_to(xgfx_path_t *p, float x, float y);
void xgfx_path_cubic_to(xgfx_path_t *p, float c1x, float c1y,
                        float c2x, float c2y, float ex, float ey);
void xgfx_path_close(xgfx_path_t *p);
void xgfx_path_rect(xgfx_path_t *p, float x, float y, float w, float h);
void xgfx_path_rounded_rect(xgfx_path_t *p, float x, float y, float w, float h, float r);

/* ---- Paint -------------------------------------------------------------- */

void xgfx_paint_solid(xgfx_paint_t *p, uint32_t color);
void xgfx_paint_linear(xgfx_paint_t *p, float x1, float y1, float x2, float y2,
                       uint32_t c1, uint32_t c2);

/* ---- Drawing ------------------------------------------------------------ */

void xgfx_fill_path(xgfx_surface_t *s, xgfx_path_t *p, xgfx_paint_t *paint);
void xgfx_stroke_path(xgfx_surface_t *s, xgfx_path_t *p, xgfx_paint_t *paint, float width);

/* ---- Helpers ------------------------------------------------------------ */

static inline uint32_t xgfx_argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline void xgfx_put(xgfx_surface_t *s, int x, int y, uint32_t c) {
    if (x >= 0 && x < s->w && y >= 0 && y < s->h)
        s->pixels[y * s->stride + x] = c;
}

/* 8x16 Terminus monospace bitmap font (each char 8px wide, 16px tall) */
void xgfx_draw_text(xgfx_surface_t *s, int x, int y, const char *str, uint32_t color);
void xgfx_draw_text_scaled(xgfx_surface_t *s, int x, int y, const char *str, uint32_t color, int scale);
void xgfx_draw_text_aa(xgfx_surface_t *s, float x, float y, const char *str, uint32_t color);
