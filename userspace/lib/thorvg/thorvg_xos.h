#ifndef THORVG_XOS_H
#define THORVG_XOS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize ThorVG engine (call once at startup) */
int thorvg_xos_init(void);

/* Terminate ThorVG engine */
void thorvg_xos_term(void);

/* Load a font from memory data (TTF/OTF).
 * name: font family name to reference in SVG text elements.
 * Returns 0 on success. */
int thorvg_xos_load_font(const char *name, const void *data, size_t size);

/* Parse an SVG document from memory.
 * Returns an opaque handle, or NULL on failure. */
typedef struct thorvg_xos_doc thorvg_xos_doc_t;
thorvg_xos_doc_t *thorvg_xos_parse(const char *data, int length);

/* Get SVG document dimensions */
int thorvg_xos_width(const thorvg_xos_doc_t *doc);
int thorvg_xos_height(const thorvg_xos_doc_t *doc);

/* Render SVG to a pixel buffer (ARGB8888, premultiplied).
 * pixels: buffer of width*height*4 bytes
 * stride: bytes per row (typically width*4)
 * The buffer is cleared to transparent before rendering. */
void thorvg_xos_render(thorvg_xos_doc_t *doc, unsigned char *pixels, int width, int height, int stride);

/* Render SVG on top of existing pixels without clearing first.
 * Use this when you have a background (e.g. highlight) already in the buffer. */
void thorvg_xos_render_overlay(thorvg_xos_doc_t *doc, unsigned char *pixels, int width, int height, int stride);

/* Destroy a parsed document */
void thorvg_xos_destroy(thorvg_xos_doc_t *doc);

#ifdef __cplusplus
}
#endif

#endif /* THORVG_XOS_H */
