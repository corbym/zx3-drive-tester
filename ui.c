#include "ui.h"

#include <string.h>
#include <sys/ioctl.h>

#ifndef IOCTL_OTERM_FONT
#define IOCTL_OTERM_FONT 0x0802
#endif

#ifndef IOCTL_OTERM_CLS
#define IOCTL_OTERM_CLS 0x0102
#endif

#ifndef IOCTL_OTERM_RESET_SCROLL
#define IOCTL_OTERM_RESET_SCROLL 0x0202
#endif

/*
 * Character font buffer captured at startup before ROM paging is modified.
 *
 * Problem: DivMMC loads the program with the 48K BASIC ROM active (containing
 * the character font at $3D00). z88dk's output_char_32 driver uses font base
 * $3C00. When set_motor_off() writes the stale BANK678 shadow to $1FFD, it
 * clears bit 2 and switches the ROM bank away from the 48K BASIC ROM,
 * pointing address $3D00 to wrong data and corrupting all text output.
 *
 * Solution: Copy the 1 KB font ($3C00-$3FFF) to RAM at program start, then
 * redirect z88dk's terminal driver to use the RAM copy via IOCTL_OTERM_FONT.
 * Character rendering is then independent of any ROM paging changes.
 */
static unsigned char font_ram[1024];
#if COMPACT_UI && !HEADLESS_ROM_FONT
static unsigned char font_rom[1024];
static unsigned char compact_font_active = 1;
#endif

/* --------------------------------------------------------------------- */
/* Compact font glyphs (epto/epto-fonts digital-6x6, GPL-2.0).           */
/* Moved from menu_system.c; purely rendering data, belongs in ui layer. */
/* --------------------------------------------------------------------- */
#if COMPACT_UI
typedef struct {
  unsigned char ch;
  unsigned char rows[8];
} CompactGlyph;

static void set_compact_glyph(unsigned char* font, unsigned char ch,
                               const unsigned char rows[8]) {
  unsigned char* glyph = &font[((unsigned short)ch) * 8U];
  unsigned char r;

  for (r = 0; r < 8; r++) {
    glyph[r] = rows[r];
  }
}

static void copy_font_glyph(unsigned char* font, unsigned char dst,
                             unsigned char src) {
  memcpy(&font[((unsigned short)dst) * 8U], &font[((unsigned short)src) * 8U],
         8U);
}

static void apply_compact_font(unsigned char* font) {
  unsigned char ch;
  unsigned int i;
  /*
   * Source: epto/epto-fonts (digital-6x6), GPL-2.0 repository.
   * We use an ASCII subset required by the UI.
   */
  static const unsigned char fallback_rows[8] = {0x00, 0x7E, 0xA5, 0x99,
                                                 0x99, 0xA5, 0x7E, 0x00};
  static const CompactGlyph glyphs[] = {
      {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
      {'!', {0x00, 0x18, 0x3C, 0x18, 0x00, 0x18, 0x00, 0x00}},
      {'"', {0x00, 0x28, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00}},
      {'#', {0x00, 0x10, 0x3C, 0x7C, 0x3C, 0x10, 0x00, 0x00}},
      {'+', {0x00, 0x10, 0x10, 0x7C, 0x10, 0x10, 0x00, 0x00}},
      {',', {0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30, 0x00}},
      {'-', {0x00, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x00, 0x00}},
      {'.', {0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00}},
      {'/', {0x00, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x00, 0x00}},
      {'0', {0x00, 0x7C, 0x44, 0x64, 0x64, 0x7C, 0x00, 0x00}},
      {'1', {0x00, 0x10, 0x10, 0x18, 0x18, 0x18, 0x00, 0x00}},
      {'2', {0x00, 0x7C, 0x04, 0x7C, 0x60, 0x7C, 0x00, 0x00}},
      {'3', {0x00, 0x78, 0x08, 0x7C, 0x0C, 0x7C, 0x00, 0x00}},
      {'4', {0x00, 0x50, 0x50, 0x7C, 0x18, 0x18, 0x00, 0x00}},
      {'5', {0x00, 0x7C, 0x40, 0x7C, 0x0C, 0x7C, 0x00, 0x00}},
      {'6', {0x00, 0x7C, 0x40, 0x7C, 0x4C, 0x7C, 0x00, 0x00}},
      {'7', {0x00, 0x78, 0x08, 0x0C, 0x0C, 0x0C, 0x00, 0x00}},
      {'8', {0x00, 0x78, 0x48, 0x7C, 0x4C, 0x7C, 0x00, 0x00}},
      {'9', {0x00, 0x78, 0x48, 0x7C, 0x0C, 0x7C, 0x00, 0x00}},
      {':', {0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00}},
      {';', {0x00, 0x00, 0x0C, 0x00, 0x0C, 0x0C, 0x18, 0x00}},
      {'=', {0x00, 0x00, 0x7C, 0x00, 0x7C, 0x00, 0x00, 0x00}},
      {'?', {0x00, 0x18, 0x24, 0x0C, 0x18, 0x00, 0x18, 0x00}},
      {'A', {0x00, 0x7C, 0x44, 0x7C, 0x64, 0x64, 0x00, 0x00}},
      {'B', {0x00, 0x78, 0x44, 0x78, 0x64, 0x78, 0x00, 0x00}},
      {'C', {0x00, 0x7C, 0x40, 0x60, 0x60, 0x7C, 0x00, 0x00}},
      {'D', {0x00, 0x78, 0x44, 0x64, 0x64, 0x78, 0x00, 0x00}},
      {'E', {0x00, 0x7C, 0x40, 0x78, 0x60, 0x7C, 0x00, 0x00}},
      {'F', {0x00, 0x7C, 0x40, 0x78, 0x60, 0x60, 0x00, 0x00}},
      {'G', {0x00, 0x7C, 0x40, 0x6C, 0x64, 0x7C, 0x00, 0x00}},
      {'H', {0x00, 0x44, 0x44, 0x7C, 0x64, 0x64, 0x00, 0x00}},
      {'I', {0x00, 0x7C, 0x10, 0x18, 0x18, 0x7C, 0x00, 0x00}},
      {'J', {0x00, 0x7C, 0x04, 0x0C, 0x4C, 0x7C, 0x00, 0x00}},
      {'K', {0x00, 0x44, 0x48, 0x70, 0x68, 0x64, 0x00, 0x00}},
      {'L', {0x00, 0x40, 0x40, 0x60, 0x60, 0x7C, 0x00, 0x00}},
      {'M', {0x00, 0x7C, 0x74, 0x74, 0x54, 0x44, 0x00, 0x00}},
      {'N', {0x00, 0x7C, 0x44, 0x64, 0x64, 0x64, 0x00, 0x00}},
      {'O', {0x00, 0x7C, 0x44, 0x64, 0x64, 0x7C, 0x00, 0x00}},
      {'P', {0x00, 0x7C, 0x44, 0x7C, 0x60, 0x60, 0x00, 0x00}},
      {'Q', {0x00, 0x7C, 0x44, 0x64, 0x6C, 0x7E, 0x00, 0x00}},
      {'R', {0x00, 0x7C, 0x44, 0x7C, 0x68, 0x64, 0x00, 0x00}},
      {'S', {0x00, 0x7C, 0x40, 0x7C, 0x0C, 0x7C, 0x00, 0x00}},
      {'T', {0x00, 0x7C, 0x10, 0x18, 0x18, 0x18, 0x00, 0x00}},
      {'U', {0x00, 0x44, 0x44, 0x64, 0x64, 0x7C, 0x00, 0x00}},
      {'V', {0x00, 0x44, 0x6C, 0x28, 0x38, 0x10, 0x00, 0x00}},
      {'W', {0x00, 0x44, 0x54, 0x74, 0x74, 0x7C, 0x00, 0x00}},
      {'X', {0x00, 0x6C, 0x38, 0x10, 0x38, 0x6C, 0x00, 0x00}},
      {'Y', {0x00, 0x6C, 0x38, 0x10, 0x18, 0x18, 0x00, 0x00}},
      {'Z', {0x00, 0x7C, 0x0C, 0x18, 0x30, 0x7C, 0x00, 0x00}},
      {'[', {0x00, 0x3C, 0x30, 0x30, 0x30, 0x3C, 0x00, 0x00}},
      {'\\', {0x00, 0x30, 0x18, 0x0C, 0x06, 0x02, 0x00, 0x00}},
      {']', {0x00, 0x3C, 0x0C, 0x0C, 0x0C, 0x3C, 0x00, 0x00}},
      {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0x00}},
      {'|', {0x00, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x00}},
  };

  /* Start from a fully custom font: no ROM glyph dependency in compact mode. */
  memset(font, 0x00, sizeof(font_ram));

  /* Seed printable range with a custom fallback to avoid random artifacts. */
  for (ch = 32; ch <= 126; ch++) {
    set_compact_glyph(font, ch, fallback_rows);
  }

  for (i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); i++) {
    set_compact_glyph(font, glyphs[i].ch, glyphs[i].rows);
  }

  /* Lowercase maps to uppercase to keep wording readable with a compact set. */
  for (ch = 'a'; ch <= 'z'; ch++) {
    copy_font_glyph(font, ch, (unsigned char)(ch - 'a' + 'A'));
  }
}
#endif

/* ----------------------------------------------------------------------- */
/* Font initialisation                                                       */
/* ----------------------------------------------------------------------- */

void init_ui_font(void) {
#if HEADLESS_ROM_FONT
  /* Headless/OCR builds use ROM glyphs for maximum recognition stability. */
  memcpy(font_ram, (const void*)0x3C00, sizeof(font_ram));
#elif COMPACT_UI
  /* Compact GUI uses a fully custom sprite-like glyph set by default. */
  memcpy(font_rom, (const void*)0x3C00, sizeof(font_rom));
  apply_compact_font(font_ram);
  compact_font_active = 1;
#else
  /* Copy ROM font first so paging changes cannot corrupt rendered text. */
  memcpy(font_ram, (const void*)0x3C00, sizeof(font_ram));

  /* Default build keeps ROM-compatible glyphs for OCR stability. */
#endif

  ioctl(1, IOCTL_OTERM_FONT, font_ram);
}

#if COMPACT_UI && !HEADLESS_ROM_FONT
static void select_compact_font(void) {
  if (!compact_font_active) {
    ioctl(1, IOCTL_OTERM_FONT, font_ram);
    compact_font_active = 1;
  }
}
#endif

/* ----------------------------------------------------------------------- */
/* Terminal / attribute primitives                                           */
/* ----------------------------------------------------------------------- */

void ui_term_clear(void) {
  ioctl(1, IOCTL_OTERM_CLS, 0);
  ioctl(1, IOCTL_OTERM_RESET_SCROLL, 0);
}

/* ZX Spectrum pixel address layout for an (y, xbyte) position. */
static unsigned short zx_pixel_offset(unsigned char y, unsigned char xbyte) {
  return (unsigned short)((((unsigned short)(y & 0xC0U)) << 5) |
                          (((unsigned short)(y & 0x07U)) << 8) |
                          (((unsigned short)(y & 0x38U)) << 2) | xbyte);
}

static const unsigned char* ui_active_font_ptr(void) {
#if COMPACT_UI && !HEADLESS_ROM_FONT
  return compact_font_active ? font_ram : font_rom;
#else
  return font_ram;
#endif
}

void ui_attr_set_cell(unsigned char row, unsigned char col,
                      unsigned char ink, unsigned char paper,
                      unsigned char bright) {
  volatile unsigned char* attr = (volatile unsigned char*)ZX_ATTR_BASE;
  if (row >= 24 || col >= 32) return;
  attr[(unsigned short)row * 32U + col] =
      (unsigned char)(((bright ? 1U : 0U) << 6) | ((paper & 0x07U) << 3) |
                      (ink & 0x07U));
}

/* Fill all 768 attribute cells in one memset (replaces 768 ui_attr_set_cell
 * calls in the original loop, critical for first-render latency on Z80). */
void ui_attr_fill(unsigned char ink, unsigned char paper, unsigned char bright) {
  unsigned char attr_byte =
      (unsigned char)(((bright ? 1U : 0U) << 6) | ((paper & 0x07U) << 3) |
                      (ink & 0x07U));
  memset((void*)ZX_ATTR_BASE, attr_byte, ZX_ATTR_SIZE);
}

void ui_screen_put_char(unsigned char row, unsigned char col, char ch) {
  const unsigned char* font = ui_active_font_ptr();
  const unsigned char* glyph;
  unsigned char gy;
  unsigned char* pixels = (unsigned char*)ZX_PIXELS_BASE;

  if (row >= 24 || col >= 32) return;
  glyph = &font[((unsigned short)(unsigned char)ch) * 8U];
  for (gy = 0; gy < 8; gy++) {
    unsigned char y = (unsigned char)(row * 8U + gy);
    pixels[zx_pixel_offset(y, col)] = glyph[gy];
  }
}

/* Fill a character row with one glyph using per-scanline memset.
 * For each of 8 scanlines the 32 column bytes are contiguous in pixel RAM
 * (xbyte runs 0-31 in the low bits of zx_pixel_offset), so one memset per
 * scanline replaces 32 ui_screen_put_char calls per row. */
static void ui_screen_fill_row(unsigned char row, char fill, unsigned char ink,
                               unsigned char paper, unsigned char bright) {
  const unsigned char* glyph;
  unsigned char* pixels;
  volatile unsigned char* attr;
  unsigned char attr_byte;
  unsigned char gy;

  if (row >= 24) return;
  glyph = &ui_active_font_ptr()[((unsigned short)(unsigned char)fill) * 8U];
  pixels = (unsigned char*)ZX_PIXELS_BASE;
  for (gy = 0; gy < 8U; gy++) {
    unsigned char y = (unsigned char)(row * 8U + gy);
    memset(&pixels[zx_pixel_offset(y, 0)], glyph[gy], 32U);
  }
  attr = (volatile unsigned char*)ZX_ATTR_BASE;
  attr_byte = (unsigned char)(((bright ? 1U : 0U) << 6) | ((paper & 0x07U) << 3) |
                              (ink & 0x07U));
  memset((void*)&attr[(unsigned short)row * 32U], attr_byte, 32U);
}

/* Write a text row: one attr memset for the full row, direct glyph writes
 * for text columns, scanline memset for the trailing blank columns.
 * Avoids the double pixel-write that occurred when the old implementation
 * called ui_screen_fill_row (all-space) then overwrote the text columns. */
static void ui_screen_write_row(unsigned char row, const char* text,
                                unsigned char ink, unsigned char paper,
                                unsigned char bright) {
  const unsigned char* font;
  const unsigned char* glyph;
  const unsigned char* space_glyph;
  unsigned char* pixels;
  volatile unsigned char* attr;
  unsigned char attr_byte;
  unsigned char col;
  unsigned char gy;

  if (row >= 24) return;
  font = ui_active_font_ptr();
  pixels = (unsigned char*)ZX_PIXELS_BASE;
  attr = (volatile unsigned char*)ZX_ATTR_BASE;
  attr_byte = (unsigned char)(((bright ? 1U : 0U) << 6) | ((paper & 0x07U) << 3) |
                              (ink & 0x07U));
  memset((void*)&attr[(unsigned short)row * 32U], attr_byte, 32U);

  col = 0;
  if (text) {
    while (*text && col < 32U) {
      glyph = &font[((unsigned short)(unsigned char)*text++) * 8U];
      for (gy = 0; gy < 8U; gy++) {
        unsigned char y = (unsigned char)(row * 8U + gy);
        pixels[zx_pixel_offset(y, col)] = glyph[gy];
      }
      col++;
    }
  }
  if (col < 32U) {
    space_glyph = &font[((unsigned short)' ') * 8U];
    for (gy = 0; gy < 8U; gy++) {
      unsigned char y = (unsigned char)(row * 8U + gy);
      memset(&pixels[zx_pixel_offset(y, col)], space_glyph[gy],
             (unsigned char)(32U - col));
    }
  }
}

/* ----------------------------------------------------------------------- */
/* Text-screen row cache                                                     */
/* ----------------------------------------------------------------------- */

static unsigned char ui_text_screen_active;
static const char* ui_text_screen_title;
static const char* ui_text_screen_controls;
static char ui_text_row_cache[24][33];
static unsigned char ui_text_row_style[24];

void ui_reset_text_screen_cache(void) {
  memset(ui_text_row_cache, 0, sizeof(ui_text_row_cache));
  memset(ui_text_row_style, 0xFF, sizeof(ui_text_row_style));
  ui_text_screen_active = 0;
  ui_text_screen_title = 0;
  ui_text_screen_controls = 0;
}

static unsigned char ui_line_value_col(const char* text) {
  unsigned char col = 0;

  if (!text) return 32;
  while (text[col] && col < 31U) {
    if (text[col] == ':') {
      col++;
      while (text[col] == ' ' && col < 31U) {
        col++;
      }
      return col;
    }
    col++;
  }

  return 32;
}

static unsigned char ui_line_is_alert(const char* text) {
  if (!text) return 0;
  return (unsigned char)(strstr(text, "FAIL") != 0 || strstr(text, "NO REV") != 0 ||
                     strstr(text, "NOT READY") != 0 ||
                     strstr(text, "INVALID") != 0 || strstr(text, "BAD") != 0 ||
                     strstr(text, "N/A") != 0 || strstr(text, "TIMEOUT") != 0 ||
                     strstr(text, "CHECK MEDIA") != 0 ||
                     strstr(text, "OUT-OF-RANGE") != 0 ||
                     strstr(text, "SKIPPED") != 0 ||
                     strstr(text, "STOPPED") != 0 ||
                     strstr(text, "ERROR") != 0);
}

static void ui_highlight_screen_value(unsigned char row,
                                      unsigned char start_col,
                                      unsigned char ink, unsigned char paper,
                                      unsigned char bright) {
  unsigned char col;

  if (start_col >= 32U) return;
  for (col = start_col; col < 32U; col++) {
    ui_attr_set_cell(row, col, ink, paper, bright);
  }
}

static void ui_style_screen_text_row(unsigned char row, const char* text) {
  unsigned char value_col;
  unsigned char label_end;
  unsigned char col;

  if (!text || !*text || row >= 24U) return;

  value_col = ui_line_value_col(text);
  if (value_col >= 32U) return;

  label_end = value_col;
  while (label_end > 0U && text[(unsigned char)(label_end - 1U)] == ' ') {
    label_end--;
  }

  for (col = 0; col < label_end && col < 32U; col++) {
    ui_attr_set_cell(row, col, ZX_COLOUR_BLUE, ZX_COLOUR_WHITE, 1);
  }

  ui_highlight_screen_value(
      row, value_col, ZX_COLOUR_BLACK,
      ui_line_is_alert(text) ? ZX_COLOUR_YELLOW : ZX_COLOUR_CYAN, 1);
}

static void ui_render_cached_text_row(unsigned char row, const char* text,
                                      unsigned char row_style) {
  const char* safe_text = text ? text : "";

  if (row >= 24U) return;
  if (ui_text_row_style[row] == row_style &&
      strcmp(ui_text_row_cache[row], safe_text) == 0) {
    return;
  }

  ui_screen_write_row(row, safe_text, ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 0);

  if (row_style == UI_TEXT_ROW_STYLE_TEXT ||
      row_style == UI_TEXT_ROW_STYLE_RESULT) {
    ui_style_screen_text_row(row, safe_text);
  }

  strncpy(ui_text_row_cache[row], safe_text, 32U);
  ui_text_row_cache[row][32] = '\0';
  ui_text_row_style[row] = row_style;
}

static void ui_begin_text_screen(const char* title, const char* controls) {
  unsigned char col;
  static const unsigned char STRIPE_PAPER[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  static const unsigned char STRIPE_INK[8] = {7, 7, 7, 7, 0, 0, 0, 0};
  char title_row[33];

#if COMPACT_UI && !HEADLESS_ROM_FONT
  /* Force compact 6x6-style font in human mode before drawing test screens. */
  select_compact_font();
#endif

  /* Pointer-equality cache is safe because callers pass stable string literals. */
  if (ui_text_screen_active && ui_text_screen_title == title &&
      ui_text_screen_controls == controls) {
    return;
  }

  memset(ui_text_row_cache, 0, sizeof(ui_text_row_cache));
  memset(ui_text_row_style, 0xFF, sizeof(ui_text_row_style));

  ui_term_clear();
  ui_attr_fill(ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 0);

  title_row[0] = ' ';
  strncpy(&title_row[1], title ? title : "", 31U);
  title_row[32] = '\0';
  ui_screen_write_row(0, title_row, ZX_COLOUR_WHITE, ZX_COLOUR_BLACK, 1);
  ui_screen_write_row(1, "------------------------------", ZX_COLOUR_BLACK,
                      ZX_COLOUR_WHITE, 0);

  for (col = 0; col < 8; col++) {
    ui_attr_set_cell(0, (unsigned char)(24 + col), STRIPE_INK[col],
                     STRIPE_PAPER[col], 1);
  }

  ui_text_screen_active = 1;
  ui_text_screen_title = title;
  ui_text_screen_controls = controls;
}

/* ----------------------------------------------------------------------- */
/* Public composite renderer                                                 */
/* ----------------------------------------------------------------------- */

void ui_render_text_screen(const char* title, const char* controls,
                           const char* const* lines, unsigned char line_count,
                           const char* result) {
  unsigned char i;
  unsigned char row;

  ui_begin_text_screen(title, controls);

  if (controls) {
    ui_render_cached_text_row(2, controls, UI_TEXT_ROW_STYLE_CONTROL);
  } else {
    ui_render_cached_text_row(2, "", UI_TEXT_ROW_STYLE_BLANK);
  }

  row = 3;
  for (i = 0; i < line_count && row < 23U; i++, row++) {
    if (lines[i]) {
      ui_render_cached_text_row(row, lines[i], UI_TEXT_ROW_STYLE_TEXT);
    } else {
      ui_render_cached_text_row(row, "", UI_TEXT_ROW_STYLE_BLANK);
    }
  }

  if (result && (unsigned char)(row + 1U) < 24U) {
    ui_render_cached_text_row(row, "", UI_TEXT_ROW_STYLE_BLANK);
    ui_render_cached_text_row((unsigned char)(row + 1U), result,
                              UI_TEXT_ROW_STYLE_RESULT);
    row = (unsigned char)(row + 2U);
  }

  while (row < 24U) {
    ui_render_cached_text_row(row, "", UI_TEXT_ROW_STYLE_BLANK);
    row++;
  }
}

void test_card_init(TestCard* card, const char* title, const char* controls,
                    unsigned char line_count) {
  unsigned char i;

  if (line_count > TEST_CARD_MAX_LINES) {
    line_count = TEST_CARD_MAX_LINES;
  }

  card->title = title;
  card->controls = controls;
  card->line_count = line_count;
  for (i = 0; i < TEST_CARD_MAX_LINES; i++) {
    card->text[i][0] = '\0';
    card->lines[i] = card->text[i];
  }
}

static void test_card_set_controls(TestCard* card, const char* controls) {
  card->controls = controls;
}

static void test_card_set_line(TestCard* card, unsigned char row, const char* text) {
  if (row >= card->line_count) return;
  strncpy(card->text[row], text ? text : "", (size_t)(TEST_CARD_LINE_LEN - 1U));
  card->text[row][TEST_CARD_LINE_LEN - 1U] = '\0';
}

static const char* test_card_line(const TestCard* card, unsigned char row) {
  if (row >= card->line_count) return "";
  return card->lines[row];
}

void test_card_render(const TestCard* card, const char* result) {
  ui_render_text_screen(card->title, card->controls, card->lines,
                        card->line_count, result);
}

static const char* test_card_result_text(TestCardResult result) {
  switch (result) {
    case TEST_CARD_RESULT_READY:
      return "RESULT: READY";
    case TEST_CARD_RESULT_RUNNING:
      return "RESULT: RUNNING";
    case TEST_CARD_RESULT_ACTIVE:
      return "RESULT: ACTIVE";
    case TEST_CARD_RESULT_PASS:
      return "RESULT: PASS";
    case TEST_CARD_RESULT_FAIL:
      return "RESULT: FAIL";
    case TEST_CARD_RESULT_COMPLETE:
      return "RESULT: COMPLETE";
    case TEST_CARD_RESULT_STOPPED:
      return "RESULT: STOPPED";
    case TEST_CARD_RESULT_OUT_OF_RANGE:
      return "RESULT: OUT-OF-RANGE";
    default:
      return "RESULT: IDLE";
  }
}

void test_card_render_result(const TestCard* card, TestCardResult result) {
  test_card_render(card, test_card_result_text(result));
}

static const char* yes_no_text(unsigned char flag) { return flag ? "YES" : "NO"; }

static const char* recal_seek_status_text(RecalSeekStatus status) {
  switch (status) {
    case RECAL_SEEK_STATUS_RUNNING:
      return "RUNNING";
    case RECAL_SEEK_STATUS_PENDING:
      return "PENDING";
    case RECAL_SEEK_STATUS_PASS:
      return "PASS";
    case RECAL_SEEK_STATUS_FAIL:
      return "FAIL";
    case RECAL_SEEK_STATUS_SKIPPED:
      return "SKIPPED";
    default:
      return "---";
  }
}

static const char* report_card_state_text(ReportCardState state) {
  switch (state) {
    case REPORT_CARD_STATE_PASS:
      return "PASS";
    case REPORT_CARD_STATE_FAIL:
      return "FAIL";
    default:
      return "NOT RUN";
  }
}

static const char* report_card_controls_text(ReportCardPhase phase) {
  return (phase == REPORT_CARD_PHASE_RUNNING) ? "KEYS  : AUTO ADVANCE"
                                              : "KEYS  : ENTER/ESC/X MENU";
}

static const char* report_card_result_text(ReportCardPhase phase) {
  switch (phase) {
    case REPORT_CARD_PHASE_READY:
      return "RESULT: READY";
    case REPORT_CARD_PHASE_RUNNING:
      return "RESULT: RUNNING";
    case REPORT_CARD_PHASE_COMPLETE:
      return "RESULT: COMPLETE";
    default:
      return "RESULT: IDLE";
  }
}

static const char* report_card_status_text(ReportCardPhase phase) {
  switch (phase) {
    case REPORT_CARD_PHASE_READY:
      return "STATUS: READY";
    case REPORT_CARD_PHASE_RUNNING:
      return "STATUS: RUNNING";
    case REPORT_CARD_PHASE_COMPLETE:
      return "STATUS: COMPLETE";
    default:
      return "STATUS: NO TESTS RUN";
  }
}

static const char* report_card_slot_label(ReportCardSlot slot) {
  static const char* labels[] = {"LAST",  "MOTOR", "DRIVE", "RECAL",
                                 "SEEK", "READID", "OVERALL"};
  if ((unsigned char)slot > (unsigned char)REPORT_CARD_SLOT_OVERALL) {
    return "LAST";
  }
  return labels[(unsigned char)slot];
}

static void report_card_build_row(char* out, ReportCardSlot slot,
                                  ReportCardState state) {
  char bar[9];
  unsigned char i;

  for (i = 0; i < 8U; i++) {
    bar[i] = '|';
  }
  bar[8] = '\0';
  snprintf(out, 32U, "%-6s [%s] %s", report_card_slot_label(slot), bar,
           report_card_state_text(state));
}

static void report_card_build_overall_row(char* out, unsigned char total) {
  unsigned char i;
  char meter[6];

  for (i = 0; i < 5U; i++) {
    meter[i] = (i < total) ? '|' : ' ';
  }
  meter[5] = '\0';
  snprintf(out, 32U, "OVERALL [%s] %u/5 PASS", meter, (unsigned int)total);
}

void report_card_init(ReportCard* card) {
  unsigned char i;

  test_card_init(&card->base, "TEST REPORT CARD",
                 report_card_controls_text(REPORT_CARD_PHASE_IDLE), 9U);
  card->total_pass = 0U;
  card->phase = (unsigned char)REPORT_CARD_PHASE_IDLE;
  for (i = 0; i <= (unsigned char)REPORT_CARD_SLOT_OVERALL; i++) {
    card->slot_state[i] = (unsigned char)REPORT_CARD_STATE_NOT_RUN;
  }
  test_card_set_line(&card->base, 8U, "BARS  : |=STATE COLOR");
}

void report_card_set_phase(ReportCard* card, ReportCardPhase phase) {
  card->phase = (unsigned char)phase;
  test_card_set_controls(&card->base, report_card_controls_text(phase));
}

void report_card_set_total_pass(ReportCard* card, unsigned char total_pass) {
  card->total_pass = total_pass;
}

void report_card_set_slot_state(ReportCard* card, ReportCardSlot slot,
                                ReportCardState state) {
  if ((unsigned char)slot > (unsigned char)REPORT_CARD_SLOT_OVERALL) return;
  card->slot_state[(unsigned char)slot] = (unsigned char)state;
}

ReportCardState report_card_get_slot_state(const ReportCard* card,
                                           ReportCardSlot slot) {
  if ((unsigned char)slot > (unsigned char)REPORT_CARD_SLOT_OVERALL) {
    return REPORT_CARD_STATE_NOT_RUN;
  }
  return (ReportCardState)card->slot_state[(unsigned char)slot];
}

const char* report_card_get_slot_line(const ReportCard* card,
                                      ReportCardSlot slot) {
  if ((unsigned char)slot > (unsigned char)REPORT_CARD_SLOT_OVERALL) return "";
  return test_card_line(&card->base, (unsigned char)(1U + (unsigned char)slot));
}

void report_card_render(ReportCard* card) {
  unsigned char i;
  char row_buf[32];

  test_card_set_line(&card->base, 0U,
                     report_card_status_text((ReportCardPhase)card->phase));

  for (i = (unsigned char)REPORT_CARD_SLOT_LAST;
       i <= (unsigned char)REPORT_CARD_SLOT_READID; i++) {
    report_card_build_row(row_buf, (ReportCardSlot)i,
                          (ReportCardState)card->slot_state[i]);
    test_card_set_line(&card->base, (unsigned char)(1U + i), row_buf);
  }

  report_card_build_overall_row(row_buf, card->total_pass);
  test_card_set_line(&card->base, (unsigned char)(1U + REPORT_CARD_SLOT_OVERALL),
                     row_buf);

  test_card_render(&card->base,
                   report_card_result_text((ReportCardPhase)card->phase));
}

void track_loop_card_init(TrackLoopCard* card) {
  test_card_init(&card->base, "READ TRACK DATA LOOP",
                 "KEYS  : J/K TRACK  ENTER/X EXIT", 5U);
}

void track_loop_card_set_track(TrackLoopCard* card, unsigned char track) {
  snprintf(card->base.text[0], TEST_CARD_LINE_LEN, "TRACK : %3u",
           (unsigned int)track);
}

void track_loop_card_set_counts(TrackLoopCard* card, unsigned int pass_count,
                                unsigned int fail_count) {
  snprintf(card->base.text[1], TEST_CARD_LINE_LEN, "PASS  : %3u", pass_count);
  snprintf(card->base.text[2], TEST_CARD_LINE_LEN, "FAIL  : %3u", fail_count);
}

void track_loop_card_set_last(TrackLoopCard* card, const char* last) {
  test_card_set_line(&card->base, 3U, last ? last : "LAST  : OK");
}

void track_loop_card_set_info(TrackLoopCard* card, const char* info) {
  test_card_set_line(&card->base, 4U, info ? info : "INFO  : READY");
}

void track_loop_card_render(const TrackLoopCard* card, TestCardResult result) {
  test_card_render_result(&card->base, result);
}

void rpm_loop_card_init(RpmLoopCard* card) {
  test_card_init(&card->base, "DISK RPM CHECK LOOP", "KEYS  : ENTER/X EXIT",
                 5U);
}

void rpm_loop_card_set_rpm(RpmLoopCard* card, unsigned int rpm,
                           unsigned char rpm_valid) {
  if (rpm_valid) {
    snprintf(card->base.text[0], TEST_CARD_LINE_LEN, "RPM   : %3u", rpm);
  } else {
    test_card_set_line(&card->base, 0U, "RPM   : ---");
  }
}

void rpm_loop_card_set_counts(RpmLoopCard* card, unsigned int pass_count,
                              unsigned int fail_count) {
  snprintf(card->base.text[1], TEST_CARD_LINE_LEN, "PASS  : %3u", pass_count);
  snprintf(card->base.text[2], TEST_CARD_LINE_LEN, "FAIL  : %3u", fail_count);
}

void rpm_loop_card_set_last(RpmLoopCard* card, const char* last) {
  test_card_set_line(&card->base, 3U, last ? last : "LAST  : WAITING");
}

void rpm_loop_card_set_info(RpmLoopCard* card, const char* info) {
  test_card_set_line(&card->base, 4U, info ? info : "INFO  : READY");
}

void rpm_loop_card_render(const RpmLoopCard* card, TestCardResult result) {
  test_card_render_result(&card->base, result);
}

void motor_drive_card_init(MotorDriveCard* card, const char* controls) {
  test_card_init(&card->base, "MOTOR AND DRIVE STATUS", controls, 6U);
}

void motor_drive_card_set_unknown(MotorDriveCard* card) {
  test_card_set_line(&card->base, 1U, "ST3   : ---");
  test_card_set_line(&card->base, 2U, "READY : ---");
  test_card_set_line(&card->base, 3U, "WPROT : ---");
  test_card_set_line(&card->base, 4U, "TRACK0: ---");
  test_card_set_line(&card->base, 5U, "FAULT : ---");
}

void motor_drive_card_set_motor_on(MotorDriveCard* card) {
  test_card_set_line(&card->base, 0U, "MOTOR : ON");
}

void motor_drive_card_set_motor_off(MotorDriveCard* card) {
  test_card_set_line(&card->base, 0U, "MOTOR : OFF");
}

void motor_drive_card_set_drive_status(MotorDriveCard* card,
                                       unsigned char have_st3,
                                       unsigned char st3) {
  if (!have_st3) {
    test_card_set_line(&card->base, 1U, "ST3   : TIMEOUT");
    test_card_set_line(&card->base, 2U, "READY : NO");
    test_card_set_line(&card->base, 3U, "WPROT : NO");
    test_card_set_line(&card->base, 4U, "TRACK0: NO");
    test_card_set_line(&card->base, 5U, "FAULT : NO");
    return;
  }

  snprintf(card->base.text[1], TEST_CARD_LINE_LEN, "ST3   : 0x%02X", st3);
  snprintf(card->base.text[2], TEST_CARD_LINE_LEN, "READY : %s",
           yes_no_text(st3 & 0x20));
  snprintf(card->base.text[3], TEST_CARD_LINE_LEN, "WPROT : %s",
           yes_no_text(st3 & 0x40));
  snprintf(card->base.text[4], TEST_CARD_LINE_LEN, "TRACK0: %s",
           yes_no_text(st3 & 0x10));
  snprintf(card->base.text[5], TEST_CARD_LINE_LEN, "FAULT : %s",
           yes_no_text(st3 & 0x80));
}

void motor_drive_card_render(const MotorDriveCard* card, TestCardResult result) {
  test_card_render_result(&card->base, result);
}

void read_id_probe_card_init(ReadIdProbeCard* card, const char* controls) {
  test_card_init(&card->base, "DRIVE READ ID PROBE", controls, 6U);
}

void read_id_probe_card_set_unknown(ReadIdProbeCard* card) {
  test_card_set_line(&card->base, 0U, "ST3   : ---");
  test_card_set_line(&card->base, 1U, "READY : ---");
  test_card_set_line(&card->base, 2U, "WPROT : ---");
  test_card_set_line(&card->base, 3U, "TRACK0: ---");
  test_card_set_line(&card->base, 4U, "FAULT : ---");
}

void read_id_probe_card_set_id_text(ReadIdProbeCard* card, const char* text) {
  test_card_set_line(&card->base, 5U, text ? text : "ID    : UNKNOWN");
}

void read_id_probe_card_set_drive_status(ReadIdProbeCard* card,
                                         unsigned char have_st3,
                                         unsigned char st3) {
  if (!have_st3) {
    test_card_set_line(&card->base, 0U, "ST3   : TIMEOUT");
  } else {
    snprintf(card->base.text[0], TEST_CARD_LINE_LEN, "ST3   : 0x%02X", st3);
  }
  snprintf(card->base.text[1], TEST_CARD_LINE_LEN, "READY : %s",
           yes_no_text(have_st3 && (st3 & 0x20)));
  snprintf(card->base.text[2], TEST_CARD_LINE_LEN, "WPROT : %s",
           yes_no_text(have_st3 && (st3 & 0x40)));
  snprintf(card->base.text[3], TEST_CARD_LINE_LEN, "TRACK0: %s",
           yes_no_text(have_st3 && (st3 & 0x10)));
  snprintf(card->base.text[4], TEST_CARD_LINE_LEN, "FAULT : %s",
           yes_no_text(have_st3 && (st3 & 0x80)));
}

void read_id_probe_card_set_id_chrn(ReadIdProbeCard* card, unsigned char c,
                                    unsigned char h, unsigned char r,
                                    unsigned char n) {
  snprintf(card->base.text[5], TEST_CARD_LINE_LEN, "ID    : C%u H%u R%u N%u",
           (unsigned int)c, (unsigned int)h, (unsigned int)r,
           (unsigned int)n);
}

void read_id_probe_card_set_id_failure(ReadIdProbeCard* card,
                                       const char* reason) {
  snprintf(card->base.text[5], TEST_CARD_LINE_LEN, "ID    : %s",
           reason ? reason : "UNKNOWN");
}

void read_id_probe_card_render(const ReadIdProbeCard* card,
                               TestCardResult result) {
  test_card_render_result(&card->base, result);
}

void recal_seek_card_init(RecalSeekCard* card, const char* controls) {
  test_card_init(&card->base, "RECALIBRATE AND SEEK TRACK 2", controls, 4U);
}

void recal_seek_card_set_unknown(RecalSeekCard* card) {
  test_card_set_line(&card->base, 0U, "READY : ---");
  test_card_set_line(&card->base, 1U, "RECAL : ---");
  test_card_set_line(&card->base, 2U, "SEEK  : ---");
  test_card_set_line(&card->base, 3U, "DETAIL: ---");
}

void recal_seek_card_set_ready_yes(RecalSeekCard* card) {
  test_card_set_line(&card->base, 0U, "READY : YES");
}

void recal_seek_card_set_ready_fail_st3(RecalSeekCard* card,
                                        unsigned char st3) {
  snprintf(card->base.text[0], TEST_CARD_LINE_LEN, "READY : FAIL ST3=%02X",
           st3);
}

void recal_seek_card_set_recal_status(RecalSeekCard* card,
                                      RecalSeekStatus status) {
  snprintf(card->base.text[1], TEST_CARD_LINE_LEN, "RECAL : %s",
           recal_seek_status_text(status));
}

void recal_seek_card_set_seek_status(RecalSeekCard* card,
                                     RecalSeekStatus status) {
  snprintf(card->base.text[2], TEST_CARD_LINE_LEN, "SEEK  : %s",
           recal_seek_status_text(status));
}

void recal_seek_card_set_detail_text(RecalSeekCard* card, const char* text) {
  test_card_set_line(&card->base, 3U, text ? text : "DETAIL: ---");
}

void recal_seek_card_set_detail_st0_pcn(RecalSeekCard* card,
                                        unsigned char st0,
                                        unsigned char pcn) {
  snprintf(card->base.text[3], TEST_CARD_LINE_LEN, "DETAIL: ST0=%02X PCN=%u",
           st0, (unsigned int)pcn);
}

void recal_seek_card_set_detail_track(RecalSeekCard* card,
                                      unsigned char track) {
  snprintf(card->base.text[3], TEST_CARD_LINE_LEN, "DETAIL: TRACK %u",
           (unsigned int)track);
}

void recal_seek_card_render(const RecalSeekCard* card, TestCardResult result) {
  test_card_render_result(&card->base, result);
}

void read_id_card_init(ReadIdCard* card, const char* controls) {
  test_card_init(&card->base, "READ ID ON TRACK 0", controls, 4U);
}

void read_id_card_set_waiting(ReadIdCard* card) {
  test_card_set_line(&card->base, 0U, "MEDIA : READABLE DISK REQUIRED");
  test_card_set_line(&card->base, 1U, "STS   : --/--/--");
  test_card_set_line(&card->base, 2U, "CHRN  : INVALID");
  test_card_set_line(&card->base, 3U, "DETAIL: WAITING");
}

void read_id_card_set_reading(ReadIdCard* card) {
  test_card_set_line(&card->base, 3U, "DETAIL: READING ID");
}

void read_id_card_set_drive_not_ready(ReadIdCard* card, unsigned char st3) {
  test_card_set_line(&card->base, 0U, "MEDIA : READABLE DISK REQUIRED");
  snprintf(card->base.text[1], TEST_CARD_LINE_LEN, "READY : ST3=%02X", st3);
  test_card_set_line(&card->base, 2U, "CHRN  : INVALID");
  test_card_set_line(&card->base, 3U, "DETAIL: DRIVE NOT READY");
}

void read_id_card_set_status(ReadIdCard* card, unsigned char st0,
                             unsigned char st1, unsigned char st2) {
  test_card_set_line(&card->base, 0U, "MEDIA : READABLE DISK REQUIRED");
  snprintf(card->base.text[1], TEST_CARD_LINE_LEN, "STS   : %02X/%02X/%02X",
           st0, st1, st2);
}

void read_id_card_set_chrn_valid(ReadIdCard* card, unsigned char c,
                                 unsigned char h, unsigned char r,
                                 unsigned char n) {
  snprintf(card->base.text[2], TEST_CARD_LINE_LEN, "CHRN  : %u/%u/%u/%u",
           (unsigned int)c, (unsigned int)h, (unsigned int)r,
           (unsigned int)n);
}

void read_id_card_set_chrn_text(ReadIdCard* card, const char* text) {
  test_card_set_line(&card->base, 2U, text ? text : "CHRN  : INVALID");
}

void read_id_card_set_detail_text(ReadIdCard* card, const char* text) {
  test_card_set_line(&card->base, 3U, text ? text : "DETAIL: ---");
}

void read_id_card_set_detail_failure(ReadIdCard* card, const char* reason) {
  snprintf(card->base.text[3], TEST_CARD_LINE_LEN, "DETAIL: %s",
           reason ? reason : "UNKNOWN");
}

void read_id_card_render(const ReadIdCard* card, TestCardResult result) {
  test_card_render_result(&card->base, result);
}

void interactive_seek_card_init(InteractiveSeekCard* card,
                                const char* controls) {
  test_card_init(&card->base, "INTERACTIVE STEP SEEK", controls, 3U);
}

void interactive_seek_card_set_ready_fail(InteractiveSeekCard* card,
                                          unsigned char st3) {
  snprintf(card->base.text[0], TEST_CARD_LINE_LEN, "READY : FAIL ST3=%02X",
           st3);
}

void interactive_seek_card_set_track(InteractiveSeekCard* card,
                                     unsigned char track) {
  snprintf(card->base.text[0], TEST_CARD_LINE_LEN, "TRACK : %u",
           (unsigned int)track);
}

void interactive_seek_card_set_last_st0(InteractiveSeekCard* card,
                                        unsigned char st0) {
  snprintf(card->base.text[1], TEST_CARD_LINE_LEN, "LAST  : ST0=%02X", st0);
}

void interactive_seek_card_set_last_text(InteractiveSeekCard* card,
                                         const char* text) {
  test_card_set_line(&card->base, 1U, text ? text : "LAST  : ---");
}

void interactive_seek_card_set_pcn(InteractiveSeekCard* card,
                                   unsigned char pcn) {
  snprintf(card->base.text[2], TEST_CARD_LINE_LEN, "PCN   : %u",
           (unsigned int)pcn);
}

void interactive_seek_card_render(const InteractiveSeekCard* card,
                                  TestCardResult result) {
  test_card_render_result(&card->base, result);
}
