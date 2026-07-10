#include "thorvg_xos.h"
#include "thorvg.h"
#include "thorvg_capi.h"

struct thorvg_xos_doc {
    Tvg_Paint picture;
    Tvg_Canvas canvas;
    float w;
    float h;
};

static int g_initialized = 0;

int thorvg_xos_init(void) {
    if (g_initialized) return 0;
    Tvg_Result r = tvg_engine_init(0);  /* 0 threads = synchronous */
    if (r != TVG_RESULT_SUCCESS) return -1;
    g_initialized = 1;
    return 0;
}

void thorvg_xos_term(void) {
    if (!g_initialized) return;
    tvg_engine_term();
    g_initialized = 0;
}

int thorvg_xos_load_font(const char *name, const void *data, size_t size) {
    Tvg_Result r = tvg_font_load_data(name, (const char*)data, (uint32_t)size, "ttf", true);
    return (r == TVG_RESULT_SUCCESS) ? 0 : -1;
}

thorvg_xos_doc_t *thorvg_xos_parse(const char *data, int length) {
    if (!g_initialized) {
        if (thorvg_xos_init() < 0) return nullptr;
    }

    auto *doc = new thorvg_xos_doc;
    doc->picture = tvg_picture_new();
    doc->canvas = nullptr;
    if (!doc->picture) {
        delete doc;
        return nullptr;
    }

    Tvg_Result r = tvg_picture_load_data(doc->picture, data, (uint32_t)length, "svg", "", true);
    if (r != TVG_RESULT_SUCCESS) {
        tvg_paint_rel(doc->picture);
        delete doc;
        return nullptr;
    }

    float w, h;
    tvg_picture_get_size(doc->picture, &w, &h);
    doc->w = w;
    doc->h = h;

    return doc;
}

int thorvg_xos_width(const thorvg_xos_doc_t *doc) {
    if (!doc) return 0;
    return (int)doc->w;
}

int thorvg_xos_height(const thorvg_xos_doc_t *doc) {
    if (!doc) return 0;
    return (int)doc->h;
}

/* Ensure the doc has a persistent canvas with the picture added.
 * The canvas must persist across render calls so the SwRenderer's mpool
 * and the picture's render data remain valid. */
static void ensure_canvas(thorvg_xos_doc_t *doc) {
    if (doc->canvas) return;
    doc->canvas = tvg_swcanvas_create(TVG_ENGINE_OPTION_DEFAULT);
    if (doc->canvas) {
        tvg_canvas_add(doc->canvas, doc->picture);
    }
}

void thorvg_xos_render(thorvg_xos_doc_t *doc, unsigned char *pixels, int width, int height, int stride) {
    if (!doc || !pixels) return;

    ensure_canvas(doc);
    if (!doc->canvas) return;

    /* Set the target buffer for this render */
    Tvg_Result r = tvg_swcanvas_set_target(doc->canvas, (uint32_t*)pixels, stride / 4, width, height, TVG_COLORSPACE_ARGB8888S);
    if (r != TVG_RESULT_SUCCESS) return;

    /* Update and draw with clear */
    tvg_canvas_update(doc->canvas);
    tvg_canvas_draw(doc->canvas, true);
    tvg_canvas_sync(doc->canvas);
}

void thorvg_xos_render_overlay(thorvg_xos_doc_t *doc, unsigned char *pixels, int width, int height, int stride) {
    /* Same as render — ThorVG clears the buffer and draws the SVG on top */
    thorvg_xos_render(doc, pixels, width, height, stride);
}

void thorvg_xos_destroy(thorvg_xos_doc_t *doc) {
    if (!doc) return;
    if (doc->canvas) tvg_canvas_destroy(doc->canvas);
    if (doc->picture) tvg_paint_rel(doc->picture);
    delete doc;
}
