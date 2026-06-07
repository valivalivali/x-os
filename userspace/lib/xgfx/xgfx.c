/* xgfx.c — Scanline rasterizer with coverage-based anti-aliasing */

#include "userspace/lib/xgfx/xgfx.h"

/* Freestanding memset — compiler may optimize loops into this */
void *memset(void *s, int c, size_t n) {
    unsigned char *p = s;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)c;
    return s;
}

/* ---- Edge buffer (flattened path) --------------------------------------- */

static float edge_x0[XGFX_MAX_EDGES];
static float edge_y0[XGFX_MAX_EDGES];
static float edge_x1[XGFX_MAX_EDGES];
static float edge_y1[XGFX_MAX_EDGES];
static int   nedges;

static void emit_edge(float x0, float y0, float x1, float y1) {
    if (nedges >= XGFX_MAX_EDGES) return;
    if (y0 == y1 && x0 == x1) return;
    edge_x0[nedges] = x0; edge_y0[nedges] = y0;
    edge_x1[nedges] = x1; edge_y1[nedges] = y1;
    nedges++;
}

/* ---- Path commands ------------------------------------------------------ */

void xgfx_path_init(xgfx_path_t *p) {
    p->ncmds = 0; p->npts = 0;
    p->min_x = p->min_y = 1e9f;
    p->max_x = p->max_y = -1e9f;
}

static void pt_bounds(xgfx_path_t *p, float x, float y) {
    if (x < p->min_x) p->min_x = x;
    if (y < p->min_y) p->min_y = y;
    if (x > p->max_x) p->max_x = x;
    if (y > p->max_y) p->max_y = y;
}

void xgfx_path_move_to(xgfx_path_t *p, float x, float y) {
    if (p->ncmds >= XGFX_MAX_PATH_CMDS || p->npts >= XGFX_MAX_PATH_PTS) return;
    p->cmds[p->ncmds++] = XGFX_CMD_MOVE;
    p->pts[p->npts].x = x; p->pts[p->npts].y = y; p->npts++;
    pt_bounds(p, x, y);
}

void xgfx_path_line_to(xgfx_path_t *p, float x, float y) {
    if (p->ncmds >= XGFX_MAX_PATH_CMDS || p->npts >= XGFX_MAX_PATH_PTS) return;
    p->cmds[p->ncmds++] = XGFX_CMD_LINE;
    p->pts[p->npts].x = x; p->pts[p->npts].y = y; p->npts++;
    pt_bounds(p, x, y);
}

static void flatten_cubic(float x0, float y0, float x1, float y1,
                          float x2, float y2, float x3, float y3, float tol) {
    float dx = x3 - x0, dy = y3 - y0;
    float d1 = ((x1 - x0) * dy - (y1 - y0) * dx);
    float d2 = ((x2 - x0) * dy - (y2 - y0) * dx);
    float dist_sq = (d1 * d1 + d2 * d2) / (dx * dx + dy * dy + 0.001f);
    if (dist_sq <= tol * tol) {
        emit_edge(x0, y0, x3, y3);
        return;
    }
    float m0x = (x0 + x1) * 0.5f, m0y = (y0 + y1) * 0.5f;
    float m1x = (x1 + x2) * 0.5f, m1y = (y1 + y2) * 0.5f;
    float m2x = (x2 + x3) * 0.5f, m2y = (y2 + y3) * 0.5f;
    float n0x = (m0x + m1x) * 0.5f, n0y = (m0y + m1y) * 0.5f;
    float n1x = (m1x + m2x) * 0.5f, n1y = (m1y + m2y) * 0.5f;
    float mx  = (n0x + n1x) * 0.5f, my  = (n0y + n1y) * 0.5f;
    flatten_cubic(x0, y0, m0x, m0y, n0x, n0y, mx, my, tol);
    flatten_cubic(mx, my, n1x, n1y, m2x, m2y, x3, y3, tol);
}

void xgfx_path_cubic_to(xgfx_path_t *p, float c1x, float c1y,
                        float c2x, float c2y, float ex, float ey) {
    if (p->ncmds >= XGFX_MAX_PATH_CMDS || p->npts + 3 > XGFX_MAX_PATH_PTS) return;
    p->cmds[p->ncmds++] = XGFX_CMD_CUBIC;
    p->pts[p->npts].x = c1x; p->pts[p->npts].y = c1y; p->npts++;
    p->pts[p->npts].x = c2x; p->pts[p->npts].y = c2y; p->npts++;
    p->pts[p->npts].x = ex;  p->pts[p->npts].y = ey;  p->npts++;
    pt_bounds(p, c1x, c1y); pt_bounds(p, c2x, c2y); pt_bounds(p, ex, ey);
}

void xgfx_path_close(xgfx_path_t *p) {
    if (p->ncmds >= XGFX_MAX_PATH_CMDS) return;
    p->cmds[p->ncmds++] = XGFX_CMD_CLOSE;
}

void xgfx_path_rect(xgfx_path_t *p, float x, float y, float w, float h) {
    xgfx_path_move_to(p, x, y);
    xgfx_path_line_to(p, x + w, y);
    xgfx_path_line_to(p, x + w, y + h);
    xgfx_path_line_to(p, x, y + h);
    xgfx_path_close(p);
}

#define CIRCLE_K 0.5522847498307936f

void xgfx_path_rounded_rect(xgfx_path_t *p, float x, float y, float w, float h, float r) {
    if (r < 0.5f) r = 0;
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    float kr = r * CIRCLE_K;
    float x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    xgfx_path_move_to(p, x0 + r, y0);
    xgfx_path_line_to(p, x1 - r, y0);
    xgfx_path_cubic_to(p, x1 - r + kr, y0, x1, y0 + r - kr, x1, y0 + r);
    xgfx_path_line_to(p, x1, y1 - r);
    xgfx_path_cubic_to(p, x1, y1 - r + kr, x1 - r + kr, y1, x1 - r, y1);
    xgfx_path_line_to(p, x0 + r, y1);
    xgfx_path_cubic_to(p, x0 + r - kr, y1, x0, y1 - r + kr, x0, y1 - r);
    xgfx_path_line_to(p, x0, y0 + r);
    xgfx_path_cubic_to(p, x0, y0 + r - kr, x0 + r - kr, y0, x0 + r, y0);
    xgfx_path_close(p);
}

/* ---- Paint -------------------------------------------------------------- */

void xgfx_paint_solid(xgfx_paint_t *p, uint32_t color) {
    p->type = XGFX_PAINT_SOLID; p->color = color;
}

void xgfx_paint_linear(xgfx_paint_t *p, float x1, float y1, float x2, float y2,
                       uint32_t c1, uint32_t c2) {
    p->type = XGFX_PAINT_LINEAR;
    p->x1 = x1; p->y1 = y1; p->x2 = x2; p->y2 = y2;
    p->c1 = c1; p->c2 = c2;
}

/* ---- Color & gradient helpers ------------------------------------------- */

static inline uint8_t lerp_u8(uint8_t a, uint8_t b, float t) {
    return (uint8_t)((a * (1.0f - t)) + (b * t) + 0.5f);
}

static inline uint32_t lerp_color(uint32_t c1, uint32_t c2, float t) {
    return xgfx_argb(
        lerp_u8((c1 >> 24) & 0xFF, (c2 >> 24) & 0xFF, t),
        lerp_u8((c1 >> 16) & 0xFF, (c2 >> 16) & 0xFF, t),
        lerp_u8((c1 >> 8)  & 0xFF, (c2 >> 8)  & 0xFF, t),
        lerp_u8(c1 & 0xFF, c2 & 0xFF, t));
}

static inline uint32_t grad_color(const xgfx_paint_t *paint, float px, float py) {
    float dx = paint->x2 - paint->x1, dy = paint->y2 - paint->y1;
    float len2 = dx * dx + dy * dy;
    float t = 0;
    if (len2 > 0.0001f)
        t = ((px - paint->x1) * dx + (py - paint->y1) * dy) / len2;
    if (t < 0) t = 0; if (t > 1) t = 1;
    return lerp_color(paint->c1, paint->c2, t);
}

/* ---- Blending ----------------------------------------------------------- */

static inline void blend_pixel(xgfx_surface_t *s, int x, int y,
                               uint8_t sr, uint8_t sg, uint8_t sb, uint8_t sa, float cov) {
    if (x < 0 || x >= s->w || y < 0 || y >= s->h) return;
    int a = (int)(sa * cov + 0.5f);
    if (a <= 0) return;
    uint32_t idx = (uint32_t)y * (uint32_t)s->stride + (uint32_t)x;
    if (a >= 255) {
        s->pixels[idx] = 0xFF000000 | (sr << 16) | (sg << 8) | sb;
        return;
    }
    uint32_t dst = s->pixels[idx];
    int da = (dst >> 24) & 0xFF;
    int dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    /* Source-over blend with pre-existing destination alpha */
    int ia = 255 - a;
    int out_a = a + (da * ia) / 255;
    if (out_a <= 0) return;
    int r = (sr * a + dr * ia) / 255;
    int g = (sg * a + dg * ia) / 255;
    int b = (sb * a + db * ia) / 255;
    s->pixels[idx] = ((uint32_t)out_a << 24) | (r << 16) | (g << 8) | b;
}

static inline void write_pixel(xgfx_surface_t *s, int x, int y, uint32_t color) {
    if (x < 0 || x >= s->w || y < 0 || y >= s->h) return;
    s->pixels[y * s->stride + x] = color;
}

/* ---- Scanline rasterization --------------------------------------------- */

typedef struct { float x; int dir; } crossing_t;
static crossing_t crossings[XGFX_MAX_EDGES];

static void sort_crossings(int n) {
    for (int i = 1; i < n; i++) {
        crossing_t key = crossings[i];
        int j = i - 1;
        while (j >= 0 && crossings[j].x > key.x) {
            crossings[j + 1] = crossings[j];
            j--;
        }
        crossings[j + 1] = key;
    }
}

static void fill_scanline_span(xgfx_surface_t *s, int y, float x0f, float x1f,
                               const xgfx_paint_t *paint) {
    if (y < 0 || y >= s->h) return;
    if (x0f > x1f) { float t = x0f; x0f = x1f; x1f = t; }

    int x0 = (int)x0f, x1 = (int)x1f;
    if (x0 >= s->w || x1 < 0) return;

    float cov_start = 1.0f - (x0f - (float)x0);
    float cov_end   = x1f - (float)x1;

    if (x0 == x1) {
        float cov = x1f - x0f;
        if (cov > 0.001f) {
            uint32_t c = (paint->type == XGFX_PAINT_LINEAR)
                ? grad_color(paint, x0f + cov * 0.5f, (float)y + 0.5f)
                : paint->color;
            blend_pixel(s, x0, y, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF,
                        (c >> 24) & 0xFF, cov);
        }
        return;
    }

    if (x0 >= 0 && cov_start > 0.001f) {
        uint32_t c = (paint->type == XGFX_PAINT_LINEAR)
            ? grad_color(paint, (float)x0 + 0.5f, (float)y + 0.5f)
            : paint->color;
        blend_pixel(s, x0, y, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF,
                    (c >> 24) & 0xFF, cov_start);
    }

    int mid_start = (x0 < 0) ? 0 : x0 + 1;
    int mid_end   = (x1 >= s->w) ? s->w - 1 : x1 - 1;
    if (paint->type == XGFX_PAINT_SOLID) {
        uint32_t c = paint->color;
        uint32_t base = (uint32_t)y * (uint32_t)s->stride;
        for (int px = mid_start; px <= mid_end; px++)
            s->pixels[base + px] = c;
    } else {
        for (int px = mid_start; px <= mid_end; px++) {
            uint32_t c = grad_color(paint, (float)px + 0.5f, (float)y + 0.5f);
            write_pixel(s, px, y, c);
        }
    }

    if (x1 < s->w && cov_end > 0.001f && x1 != x0) {
        uint32_t c = (paint->type == XGFX_PAINT_LINEAR)
            ? grad_color(paint, (float)x1 + 0.5f, (float)y + 0.5f)
            : paint->color;
        blend_pixel(s, x1, y, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF,
                    (c >> 24) & 0xFF, cov_end);
    }
}

static void path_to_edges(xgfx_path_t *p) {
    nedges = 0;
    float cx = 0, cy = 0, mx = 0, my = 0;
    int pi = 0;
    for (int ci = 0; ci < p->ncmds; ci++) {
        switch (p->cmds[ci]) {
        case XGFX_CMD_MOVE:
            cx = p->pts[pi].x; cy = p->pts[pi].y;
            mx = cx; my = cy; pi++;
            break;
        case XGFX_CMD_LINE:
            emit_edge(cx, cy, p->pts[pi].x, p->pts[pi].y);
            cx = p->pts[pi].x; cy = p->pts[pi].y; pi++;
            break;
        case XGFX_CMD_CUBIC: {
            float c1x = p->pts[pi].x, c1y = p->pts[pi].y; pi++;
            float c2x = p->pts[pi].x, c2y = p->pts[pi].y; pi++;
            float ex  = p->pts[pi].x, ey  = p->pts[pi].y; pi++;
            flatten_cubic(cx, cy, c1x, c1y, c2x, c2y, ex, ey, 0.25f);
            cx = ex; cy = ey;
            break;
        }
        case XGFX_CMD_CLOSE:
            emit_edge(cx, cy, mx, my);
            cx = mx; cy = my;
            break;
        }
    }
}

void xgfx_fill_path(xgfx_surface_t *s, xgfx_path_t *p, xgfx_paint_t *paint) {
    if (!s || !p || !paint || p->ncmds == 0) return;
    path_to_edges(p);
    if (nedges == 0) return;

    int y0 = (int)(p->min_y);
    int y1 = (int)(p->max_y) + 1;
    if (y0 < 0) y0 = 0;
    if (y1 > s->h) y1 = s->h;

    for (int y = y0; y < y1; y++) {
        float scan_y = (float)y + 0.5f;
        int nc = 0;
        for (int ei = 0; ei < nedges; ei++) {
            float ey0 = edge_y0[ei], ey1 = edge_y1[ei];
            if (ey0 == ey1) continue;
            if ((scan_y >= ey0 && scan_y < ey1) || (scan_y >= ey1 && scan_y < ey0)) {
                float ex0 = edge_x0[ei], ex1 = edge_x1[ei];
                float t = (scan_y - ey0) / (ey1 - ey0);
                float x = ex0 + t * (ex1 - ex0);
                crossings[nc].x = x;
                crossings[nc].dir = (ey1 > ey0) ? 1 : -1;
                nc++;
            }
        }
        if (nc < 2) continue;
        sort_crossings(nc);

        int winding = 0;
        float prev_x = crossings[0].x;
        for (int i = 0; i < nc; i++) {
            if (winding != 0)
                fill_scanline_span(s, y, prev_x, crossings[i].x, paint);
            winding += crossings[i].dir;
            prev_x = crossings[i].x;
        }
    }
}

void xgfx_stroke_path(xgfx_surface_t *s, xgfx_path_t *p, xgfx_paint_t *paint, float width) {
    (void)width;
    if (!s || !p || !paint) return;
    xgfx_fill_path(s, p, paint);
}

/* ---- Text: 5x7 bitmap font -------------------------------------------- */
/* Each char: 5 bytes, each byte is a column (7 rows, bit0=top). */
static const uint8_t font5x7[96][5] = {
    {0,0,0,0,0}, {0x5f,0,0,0,0}, {0x03,0,0,0,0}, {0x14,0x3e,0x14,0x3e,0x14},
    {0x24,0x2a,0x7f,0x2a,0x12}, {0x43,0x33,0x08,0x66,0x61},
    {0x36,0x49,0x55,0x22,0x50}, {0x03,0,0,0,0},
    {0x1c,0x22,0x41,0,0}, {0x41,0x22,0x1c,0,0},
    {0x08,0x2a,0x1c,0x2a,0x08}, {0x08,0x08,0x3e,0x08,0x08},
    {0x50,0x30,0,0,0}, {0x08,0x08,0x08,0x08,0x08},
    {0x60,0x60,0,0,0}, {0x20,0x10,0x08,0x04,0x02},
    {0x3e,0x51,0x49,0x45,0x3e}, {0x00,0x42,0x7f,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4b,0x31},
    {0x18,0x14,0x12,0x7f,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1e},
    {0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3e}, {0x7e,0x11,0x11,0x11,0x7e},
    {0x7f,0x49,0x49,0x49,0x36}, {0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c}, {0x7f,0x49,0x49,0x49,0x41},
    {0x7f,0x09,0x09,0x09,0x01}, {0x3e,0x41,0x49,0x49,0x7a},
    {0x7f,0x08,0x08,0x08,0x7f}, {0x00,0x41,0x7f,0x41,0x00},
    {0x20,0x40,0x41,0x3f,0x01}, {0x7f,0x08,0x14,0x22,0x41},
    {0x7f,0x40,0x40,0x40,0x40}, {0x7f,0x02,0x0c,0x02,0x7f},
    {0x7f,0x04,0x08,0x10,0x7f}, {0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,0x09,0x09,0x09,0x06}, {0x3e,0x41,0x51,0x21,0x5e},
    {0x7f,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7f,0x01,0x01}, {0x3f,0x40,0x40,0x40,0x3f},
    {0x1f,0x20,0x40,0x20,0x1f}, {0x3f,0x40,0x38,0x40,0x3f},
    {0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07},
    {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7f,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7f,0x00},
    {0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78},
    {0x7f,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7f}, {0x38,0x54,0x54,0x54,0x18},
    {0x08,0x7e,0x09,0x01,0x02}, {0x0c,0x52,0x52,0x52,0x3e},
    {0x7f,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7d,0x40,0x00},
    {0x20,0x40,0x44,0x3d,0x00}, {0x7f,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7f,0x40,0x00}, {0x7c,0x04,0x18,0x04,0x78},
    {0x7c,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38},
    {0x7c,0x14,0x14,0x14,0x08}, {0x08,0x14,0x14,0x18,0x7c},
    {0x7c,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3f,0x44,0x40,0x20}, {0x3c,0x40,0x40,0x20,0x7c},
    {0x1c,0x20,0x40,0x20,0x1c}, {0x3c,0x40,0x30,0x40,0x3c},
    {0x44,0x28,0x10,0x28,0x44}, {0x0c,0x50,0x50,0x50,0x3c},
    {0x44,0x64,0x54,0x4c,0x44},
};

void xgfx_draw_text(xgfx_surface_t *s, int x, int y, const char *str, uint32_t color) {
    int cx = x;
    for (const char *p = str; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < 32) ch = 0;
        else if (ch > 127) ch = 0;
        else ch -= 32;
        if (ch == 0 && *p != ' ') { cx += 6; continue; }
        const uint8_t *col = font5x7[ch];
        for (int c = 0; c < 5; c++) {
            uint8_t bits = col[c];
            for (int r = 0; r < 7; r++) {
                if (bits & (1 << r))
                    write_pixel(s, cx + c, y + r, color);
            }
        }
        cx += 6;
    }
}
