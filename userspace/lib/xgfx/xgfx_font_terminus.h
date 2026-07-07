/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Terminus BSD Console Font 8x16 - ported from FreeBSD vt_font_default.c
 * 2331 glyphs with full Unicode mapping (normal + bold).
 */
#ifndef XGFX_FONT_TERMINUS_H
#define XGFX_FONT_TERMINUS_H

#include <stdint.h>

#define XGFX_FONT_W 8
#define XGFX_FONT_H 16

typedef struct {
    uint32_t src;
    uint16_t dst;
    uint16_t len;
} xgfx_font_map_t;

/* Look up a Unicode codepoint and return pointer to 16 bytes of glyph data.
 * Returns the 'missing glyph' (index 0) if codepoint is not found. */
const uint8_t *xgfx_font_lookup(uint32_t codepoint);

#endif
