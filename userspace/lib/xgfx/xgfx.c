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

/* ---- Text: 8x8 monospace bitmap font ---------------------------------- */
/* Each char: 8 bytes, one per row. MSB=leftmost pixel. Advance = 8px. */
/* Covers ASCII 32-127 (96 glyphs). */
static const uint8_t font8x8[96][8] = {
    /* 32 ' ' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 33 '!' */ {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    /* 34 '"' */ {0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 35 '#' */ {0x66,0x66,0xFF,0x66,0xFF,0x66,0x66,0x00},
    /* 36 '$' */ {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00},
    /* 37 '%' */ {0x62,0x66,0x0C,0x18,0x30,0x66,0x46,0x00},
    /* 38 '&' */ {0x3C,0x66,0x3C,0x38,0x67,0x66,0x3F,0x00},
    /* 39 ''' */ {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
    /* 40 '(' */ {0x0E,0x18,0x30,0x30,0x30,0x18,0x0E,0x00},
    /* 41 ')' */ {0x70,0x18,0x0C,0x0C,0x0C,0x18,0x70,0x00},
    /* 42 '*' */ {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
    /* 43 '+' */ {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    /* 44 ',' */ {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},
    /* 45 '-' */ {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    /* 46 '.' */ {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
    /* 47 '/' */ {0x00,0x06,0x0C,0x18,0x30,0x60,0x00,0x00},
    /* 48 '0' */ {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00},
    /* 49 '1' */ {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},
    /* 50 '2' */ {0x3C,0x66,0x06,0x0C,0x30,0x60,0x7E,0x00},
    /* 51 '3' */ {0x7E,0x0C,0x18,0x0C,0x06,0x66,0x3C,0x00},
    /* 52 '4' */ {0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x00},
    /* 53 '5' */ {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00},
    /* 54 '6' */ {0x3C,0x60,0x60,0x7C,0x66,0x66,0x3C,0x00},
    /* 55 '7' */ {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00},
    /* 56 '8' */ {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00},
    /* 57 '9' */ {0x3C,0x66,0x66,0x3E,0x06,0x06,0x3C,0x00},
    /* 58 ':' */ {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00},
    /* 59 ';' */ {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30},
    /* 60 '<' */ {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00},
    /* 61 '=' */ {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},
    /* 62 '>' */ {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00},
    /* 63 '?' */ {0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x00},
    /* 64 '@' */ {0x3C,0x66,0x6E,0x6E,0x60,0x62,0x3C,0x00},
    /* 65 'A' */ {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00},
    /* 66 'B' */ {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00},
    /* 67 'C' */ {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00},
    /* 68 'D' */ {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00},
    /* 69 'E' */ {0x7E,0x60,0x60,0x78,0x60,0x60,0x7E,0x00},
    /* 70 'F' */ {0x7E,0x60,0x60,0x78,0x60,0x60,0x60,0x00},
    /* 71 'G' */ {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0x00},
    /* 72 'H' */ {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00},
    /* 73 'I' */ {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    /* 74 'J' */ {0x1E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00},
    /* 75 'K' */ {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00},
    /* 76 'L' */ {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00},
    /* 77 'M' */ {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00},
    /* 78 'N' */ {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00},
    /* 79 'O' */ {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
    /* 80 'P' */ {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00},
    /* 81 'Q' */ {0x3C,0x66,0x66,0x66,0x66,0x3C,0x0E,0x00},
    /* 82 'R' */ {0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x00},
    /* 83 'S' */ {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00},
    /* 84 'T' */ {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00},
    /* 85 'U' */ {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00},
    /* 86 'V' */ {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00},
    /* 87 'W' */ {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    /* 88 'X' */ {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00},
    /* 89 'Y' */ {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00},
    /* 90 'Z' */ {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00},
    /* 91 '[' */ {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
    /* 92 '\' */ {0x00,0x60,0x30,0x18,0x0C,0x06,0x00,0x00},
    /* 93 ']' */ {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
    /* 94 '^' */ {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},
    /* 95 '_' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    /* 96 '`' */ {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00},
    /* 97 'a' */ {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00},
    /* 98 'b' */ {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00},
    /* 99 'c' */ {0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00},
    /* 100 'd' */ {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00},
    /* 101 'e' */ {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00},
    /* 102 'f' */ {0x1C,0x30,0x30,0x7C,0x30,0x30,0x30,0x00},
    /* 103 'g' */ {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C},
    /* 104 'h' */ {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00},
    /* 105 'i' */ {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
    /* 106 'j' */ {0x0C,0x00,0x1C,0x0C,0x0C,0x0C,0x6C,0x38},
    /* 107 'k' */ {0x60,0x60,0x6C,0x78,0x70,0x78,0x6C,0x00},
    /* 108 'l' */ {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    /* 109 'm' */ {0x00,0x00,0x66,0x7F,0x7F,0x6B,0x63,0x00},
    /* 110 'n' */ {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00},
    /* 111 'o' */ {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00},
    /* 112 'p' */ {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60},
    /* 113 'q' */ {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06},
    /* 114 'r' */ {0x00,0x00,0x7C,0x66,0x60,0x60,0x60,0x00},
    /* 115 's' */ {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00},
    /* 116 't' */ {0x30,0x30,0x7C,0x30,0x30,0x30,0x1C,0x00},
    /* 117 'u' */ {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00},
    /* 118 'v' */ {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00},
    /* 119 'w' */ {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},
    /* 120 'x' */ {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00},
    /* 121 'y' */ {0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C},
    /* 122 'z' */ {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00},
    /* 123 '{' */ {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00},
    /* 124 '|' */ {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00},
    /* 125 '}' */ {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00},
    /* 126 '~' */ {0x00,0x00,0x32,0x7E,0x4C,0x00,0x00,0x00},
    /* 127 DEL */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

void xgfx_draw_text(xgfx_surface_t *s, int x, int y, const char *str, uint32_t color) {
    int cx = x;
    for (const char *p = str; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < 32 || ch > 127) ch = 32;
        ch -= 32;
        const uint8_t *row = font8x8[ch];
        for (int r = 0; r < 8; r++) {
            uint8_t bits = row[r];
            for (int c = 0; c < 8; c++) {
                if (bits & (0x80 >> c))
                    write_pixel(s, cx + c, y + r, color);
            }
        }
        cx += 8;
    }
}

void xgfx_draw_text_scaled(xgfx_surface_t *s, int x, int y, const char *str, uint32_t color, int scale) {
    if (scale <= 0) scale = 1;
    int cx = x;
    for (const char *p = str; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < 32 || ch > 127) ch = 32;
        ch -= 32;
        const uint8_t *row = font8x8[ch];
        for (int r = 0; r < 8; r++) {
            uint8_t bits = row[r];
            for (int c = 0; c < 8; c++) {
                if (bits & (0x80 >> c)) {
                    for (int dy = 0; dy < scale; dy++) {
                        for (int dx = 0; dx < scale; dx++) {
                            write_pixel(s, cx + c * scale + dx, y + r * scale + dy, color);
                        }
                    }
                }
            }
        }
        cx += 8 * scale;
    }
}
