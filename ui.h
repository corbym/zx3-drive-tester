#pragma once

#include <stdio.h>

#ifndef COMPACT_UI
#define COMPACT_UI 0
#endif

#ifndef HEADLESS_ROM_FONT
#define HEADLESS_ROM_FONT 0
#endif

/* ZX Spectrum colour IDs */
#define ZX_COLOUR_BLACK  0
#define ZX_COLOUR_BLUE   1
#define ZX_COLOUR_RED    2
#define ZX_COLOUR_GREEN  4
#define ZX_COLOUR_WHITE  7
#define ZX_COLOUR_CYAN   5
#define ZX_COLOUR_YELLOW 6

#define ZX_ATTR_BASE   0x5800
#define ZX_PIXELS_BASE 0x4000
#define ZX_PIXELS_SIZE 0x1800U
#define ZX_ATTR_SIZE   0x300U

#define UI_TEXT_ROW_STYLE_BLANK   0U
#define UI_TEXT_ROW_STYLE_CONTROL 1U
#define UI_TEXT_ROW_STYLE_TEXT    2U
#define UI_TEXT_ROW_STYLE_RESULT  3U

/*
 * Initialise the RAM font buffer and redirect the z88dk terminal driver to
 * use it.  Must be called once at startup before any screen output.
 */
void init_ui_font(void);

/* Clear the terminal viewport and reset the scroll position. */
void ui_term_clear(void);

/* Write a single attribute byte to the given character cell. */
void ui_attr_set_cell(unsigned char row, unsigned char col,
                      unsigned char ink, unsigned char paper,
                      unsigned char bright);

/* Fill all 24×32 attribute cells with a single computed byte (one memset). */
void ui_attr_fill(unsigned char ink, unsigned char paper, unsigned char bright);

/* Colour N consecutive attribute cells starting at (row, start_col). */
void ui_attr_set_run(unsigned char row, unsigned char start_col,
                     unsigned char count,
                     unsigned char ink, unsigned char paper,
                     unsigned char bright);

/* Blit one character glyph directly into pixel RAM. */
void ui_screen_put_char(unsigned char row, unsigned char col, char ch);

/*
 * Invalidate the text-screen row cache so that the next call to
 * ui_render_text_screen unconditionally redraws every row.
 * Call this before switching to the main-menu printf path.
 */
void ui_reset_text_screen_cache(void);

/*
 * Draw a labelled text screen with a title bar, controls line, body rows,
 * and a result line.  Uses a dirty-row cache so repeated calls with the
 * same content are near-zero cost.
 */
void ui_render_text_screen(const char* title, const char* controls,
                           const char* const* lines, unsigned char line_count,
                           const char* result_label, const char* result_value);

