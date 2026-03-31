#include "ui.h"
#include "shared_strings.h"

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
 * Character font buffer used by z88dk's terminal driver.
 *
 * In the default build (no HEADLESS_ROM_FONT), font_ram is filled from the
 * embedded glyph tables below — fully ROM-independent, works on machines
 * with absent or corrupted ROMs.
 *
 * When HEADLESS_ROM_FONT=1 (deploy.sh / CI / OCR builds), the 48K BASIC ROM
 * font is copied from $3C00 instead, giving Tesseract-compatible glyphs.
 */
static unsigned char font_ram[1024];

#if !HEADLESS_ROM_FONT
static const unsigned char s_font_digits[10][8] = {
  /* 0 */ { 0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00 },
  /* 1 */ { 0x18,0x38,0x18,0x18,0x18,0x18,0x3C,0x00 },
  /* 2 */ { 0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00 },
  /* 3 */ { 0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00 },
  /* 4 */ { 0x0C,0x1C,0x2C,0x4C,0x7E,0x0C,0x0C,0x00 },
  /* 5 */ { 0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00 },
  /* 6 */ { 0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00 },
  /* 7 */ { 0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00 },
  /* 8 */ { 0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00 },
  /* 9 */ { 0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00 },
};
static const unsigned char s_font_upper[26][8] = {
  /* A */ { 0x18,0x3C,0x66,0x7E,0x66,0x66,0x66,0x00 },
  /* B */ { 0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00 },
  /* C */ { 0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00 },
  /* D */ { 0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00 },
  /* E */ { 0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00 },
  /* F */ { 0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00 },
  /* G */ { 0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0x00 },
  /* H */ { 0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00 },
  /* I */ { 0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00 },
  /* J */ { 0x1E,0x06,0x06,0x06,0x66,0x66,0x3C,0x00 },
  /* K */ { 0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00 },
  /* L */ { 0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00 },
  /* M */ { 0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00 },
  /* N */ { 0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00 },
  /* O */ { 0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00 },
  /* P */ { 0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00 },
  /* Q */ { 0x3C,0x66,0x66,0x66,0x6E,0x3C,0x06,0x00 },
  /* R */ { 0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x00 },
  /* S */ { 0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00 },
  /* T */ { 0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00 },
  /* U */ { 0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00 },
  /* V */ { 0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00 },
  /* W */ { 0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00 },
  /* X */ { 0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00 },
  /* Y */ { 0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00 },
  /* Z */ { 0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00 },
};
/* Each entry: [ascii_code, 8 pixel bytes].
 * IOCTL_OTERM_FONT uses glyph_addr = font_ptr + ascii_code * 8
 * (same as the ZX CHARS sysvar convention, NOT char - 0x20). */
static const unsigned char s_font_punct[][9] = {
  /* '#' 0x23=35 */ { 0x23, 0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00 },
  /* '-' 0x2D=45 */ { 0x2D, 0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00 },
  /* '.' 0x2E=46 */ { 0x2E, 0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00 },
  /* '/' 0x2F=47 */ { 0x2F, 0x00,0x06,0x0C,0x18,0x30,0x60,0x00,0x00 },
  /* ':' 0x3A=58 */ { 0x3A, 0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00 },
};
#define S_PUNCT_COUNT ((unsigned char)(sizeof(s_font_punct) / sizeof(s_font_punct[0])))
#endif /* !HEADLESS_ROM_FONT */

/* ----------------------------------------------------------------------- */
/* Font initialisation                                                       */
/* ----------------------------------------------------------------------- */

void init_ui_font(void) {
#if HEADLESS_ROM_FONT
  /* Copy ROM font — requires 48K BASIC ROM correctly paged at $3C00.
   * Used by deploy.sh / CI builds so Tesseract OCR can read the screen. */
  memcpy(font_ram, (const void*)0x3C00, sizeof(font_ram));
#else
  /* ROM-independent path: works on machines with absent or corrupted ROMs.
   * Covers uppercase A-Z, digits 0-9, and key punctuation used by the UI.
   * Undefined characters render as blank — acceptable for a diagnostic tool
   * where all important output uses the defined glyphs. */
  unsigned char i;
  memset(font_ram, 0, sizeof(font_ram));
  /* IOCTL_OTERM_FONT: glyph for char c is at font_ptr + c * 8
   * (same convention as the ZX CHARS sysvar: base is 256 bytes before
   * the first printable glyph, so char*8 lands on the right data). */
  /* Digits '0'-'9': ASCII 0x30 */
  memcpy(&font_ram[(unsigned short)0x30U * 8U], s_font_digits, sizeof(s_font_digits));
  /* Uppercase 'A'-'Z': ASCII 0x41 */
  memcpy(&font_ram[(unsigned short)0x41U * 8U], s_font_upper, sizeof(s_font_upper));
  /* Punctuation: patch individual slots (s_font_punct[i][0] = ASCII code) */
  for (i = 0; i < S_PUNCT_COUNT; i++) {
    memcpy(&font_ram[(unsigned short)s_font_punct[i][0] * 8U],
           &s_font_punct[i][1], 8U);
  }
#endif
  ioctl(1, IOCTL_OTERM_FONT, font_ram);
}

/* ----------------------------------------------------------------------- */
/* Terminal / attribute primitives                                           */
/* ----------------------------------------------------------------------- */

void ui_term_clear(void) {
  ioctl(1, IOCTL_OTERM_CLS, 0);
  ioctl(1, IOCTL_OTERM_RESET_SCROLL, 0);
}

/* ZX Spectrum pixel address layout for an (y, xbyte) position. */
static unsigned short zx_pixel_offset(unsigned char y, unsigned char xbyte) {
  return (unsigned short)((unsigned short)(y & 0xC0U) << 5 |
                          (unsigned short)(y & 0x07U) << 8 |
                          (unsigned short)(y & 0x38U) << 2 | xbyte);
}

static const unsigned char* ui_active_font_ptr(void) {
  return font_ram;
}

void ui_attr_set_cell(unsigned char row, unsigned char col,
                      unsigned char ink, unsigned char paper,
                      unsigned char bright) {
  volatile unsigned char* attr = (volatile unsigned char*)ZX_ATTR_BASE;
  if (row >= 24 || col >= 32) return;
  attr[(unsigned short)row * 32U + col] =
      (unsigned char)((bright ? 1U : 0U) << 6 | (paper & 0x07U) << 3 |
                      ink & 0x07U);
}

/* Fill all 768 attribute cells in one memset (replaces 768 ui_attr_set_cell
 * calls in the original loop, critical for first-render latency on Z80). */
void ui_attr_fill(unsigned char ink, unsigned char paper, unsigned char bright) {
  unsigned char attr_byte =
      (unsigned char)((bright ? 1U : 0U) << 6 | (paper & 0x07U) << 3 |
                      ink & 0x07U);
  memset((void*)ZX_ATTR_BASE, attr_byte, ZX_ATTR_SIZE);
}

void ui_screen_put_char(unsigned char row, unsigned char col, char ch) {
  const unsigned char* font = ui_active_font_ptr();
  unsigned char* pixels = (unsigned char*)ZX_PIXELS_BASE;

  if (row >= 24 || col >= 32) return;
  const unsigned char *glyph = &font[((unsigned short) (unsigned char) ch) * 8U];
  for (unsigned char gy = 0; gy < 8; gy++) {
    unsigned char y = (unsigned char)(row * 8U + gy);
    pixels[zx_pixel_offset(y, col)] = glyph[gy];
  }
}

/* Write a text row: one attr memset for the full row, direct glyph writes
 * for text columns, scanline memset for the trailing blank columns.
 * Avoids the double pixel-write that occurred when the old implementation
 * called ui_screen_fill_row (all-space) then overwrote the text columns. */
static void ui_screen_write_row(unsigned char row, const char* text,
                                unsigned char ink, unsigned char paper,
                                unsigned char bright) {
  const unsigned char* font;
  unsigned char* pixels;
  volatile unsigned char* attr;
  unsigned char gy;

  if (row >= 24) return;
  font = ui_active_font_ptr();
  pixels = (unsigned char*)ZX_PIXELS_BASE;
  attr = (volatile unsigned char*)ZX_ATTR_BASE;
  unsigned char attr_byte = (unsigned char) (((bright ? 1U : 0U) << 6) | ((paper & 0x07U) << 3) |
                                             (ink & 0x07U));
  memset((void*)&attr[(unsigned short)row * 32U], attr_byte, 32U);

  unsigned char col = 0;
  if (text) {
    while (*text && col < 32U) {
      const unsigned char *glyph = &font[((unsigned short) (unsigned char) *text++) * 8U];
      for (gy = 0; gy < 8U; gy++) {
        unsigned char y = (unsigned char)(row * 8U + gy);
        pixels[zx_pixel_offset(y, col)] = glyph[gy];
      }
      col++;
    }
  }
  if (col < 32U) {
    const unsigned char* space_glyph;
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

/* ----------------------------------------------------------------------- */
/* Drive status badge state                                                  */
/* ----------------------------------------------------------------------- */

static unsigned char s_badge_st3_valid = 0; /* 0 until first ST3 read     */
static unsigned char s_badge_ready     = 0; /* ST3 bit 5                  */
static unsigned char s_badge_wprot     = 0; /* ST3 bit 6                  */
static unsigned char s_badge_motor_on  = 0; /* updated by motor_on/off    */

void ui_set_drive_motor(unsigned char on) {
    s_badge_motor_on = on;
}

void ui_set_drive_st3(unsigned char st3) {
    s_badge_ready     = (unsigned char)((st3 & 0x20U) ? 1U : 0U);
    s_badge_wprot     = (unsigned char)((st3 & 0x40U) ? 1U : 0U);
    s_badge_st3_valid = 1;
}


/*
 * Per-row dirty cache.  ui_row_tag combines a text checksum with the row
 * style; a full-row cache hit skips the pixel+attr write entirely.
 *
 * Value-only update: for TEXT/RESULT rows with a "LABEL: value" format,
 * after the label has been drawn once we track the value separately.
 * When only the value changes, only the value columns are redrawn.
 *
 *   ui_row_value_col[r]  — column where the value starts (0xFF = no label drawn)
 *   ui_row_value_tag[r]  — hash of value portion (text[value_col..])
 *
 * Label identity is inferred from value_col: labels are fixed per card row
 * and ui_row_value_col is reset to 0xFF whenever the screen title changes,
 * so a matching value_col implies the label is the same.
 */
static unsigned char ui_row_tag[24];
static unsigned char ui_row_value_col[24];
static unsigned char ui_row_value_tag[24];

void ui_reset_text_screen_cache(void) {
  memset(ui_row_tag, 0xFF, sizeof(ui_row_tag));
  memset(ui_row_value_col, 0xFF, sizeof(ui_row_value_col));
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
  return (unsigned char)(strstr(text, zx3_str_fail) != 0 || strstr(text, "NO MARK") != 0 ||
                     strstr(text, zx3_str_not_ready) != 0 ||
                     strstr(text, zx3_str_invalid) != 0 || strstr(text, "BAD") != 0 ||
                     strstr(text, "N/A") != 0 || strstr(text, zx3_str_timeout) != 0 ||
                     strstr(text, zx3_str_check_media) != 0 ||
                     strstr(text, zx3_str_out_of_range) != 0 ||
                     strstr(text, zx3_str_skipped) != 0 ||
                     strstr(text, zx3_str_stopped) != 0 ||
                     strstr(text, zx3_str_error) != 0);
}

/* Colour N consecutive attribute cells starting at (row, start_col). */
void ui_attr_set_run(unsigned char row, unsigned char start_col,
                     unsigned char count,
                     unsigned char ink, unsigned char paper,
                     unsigned char bright) {
  volatile unsigned char *attr = (volatile unsigned char *)ZX_ATTR_BASE;
  unsigned char attr_byte;
  unsigned char safe_count;
  if (row >= 24 || start_col >= 32 || count == 0) return;
  attr_byte = (unsigned char)((bright ? 1U : 0U) << 6 | (paper & 0x07U) << 3 |
                              ink & 0x07U);
  safe_count = (unsigned char)(start_col + count > 32U ? 32U - start_col : count);
  memset((void *)&attr[(unsigned short)row * 32U + start_col], attr_byte,
         safe_count);
}

static void ui_highlight_screen_value(unsigned char row,
                                      unsigned char start_col,
                                      unsigned char ink, unsigned char paper,
                                      unsigned char bright) {
  if (start_col >= 32U) return;
  ui_attr_set_run(row, start_col, (unsigned char)(32U - start_col),
                  ink, paper, bright);
}

static void ui_style_screen_text_row(unsigned char row, const char* text) {
  unsigned char value_col;
  unsigned char label_end;

  if (!text || !*text || row >= 24U) return;

  value_col = ui_line_value_col(text);
  if (value_col >= 32U) return;

  label_end = value_col;
  while (label_end > 0U && text[(unsigned char)(label_end - 1U)] == ' ') {
    label_end--;
  }

  ui_attr_set_run(row, 0, label_end < 32U ? label_end : 32U,
                  ZX_COLOUR_BLUE, ZX_COLOUR_WHITE, 1);

  ui_highlight_screen_value(
      row, value_col, ZX_COLOUR_BLACK,
      ui_line_is_alert(text) ? ZX_COLOUR_YELLOW : ZX_COLOUR_CYAN, 1);
}

static unsigned char ui_row_tag_compute(const char* text, unsigned char style) {
  /* DJB2-style checksum folded to 6 bits, combined with 2-bit style.
   * Final avalanche: spread bit-0 differences to bit 3 so that adjacent
   * single-character values (e.g. "0" vs "1") survive the & 0xFC mask. */
  const unsigned char* p = (const unsigned char*)(text ? text : "");
  unsigned char h = 0x55U;

  while (*p) {
    h = (unsigned char)((h << 1) ^ *p++);
  }
  h ^= (unsigned char)(h << 3);
  return (unsigned char)((h & 0xFCU) | (style & 0x03U));
}

/* Write only the value portion of a row (cols start_col..31), padding with spaces. */
static void ui_screen_write_value(unsigned char row, unsigned char start_col,
                                  const char* value) {
  unsigned char col = start_col;
  const char* p = value ? value : "";

  while (col < 32U && *p) {
    ui_screen_put_char(row, col, *p++);
    col++;
  }
  while (col < 32U) {
    ui_screen_put_char(row, col, ' ');
    col++;
  }
}

static void ui_render_cached_text_row(unsigned char row, const char* text,
                                      unsigned char row_style) {
  const char* safe_text = text ? text : "";
  unsigned char tag;
  unsigned char value_col;

  if (row >= 24U) return;

  tag = ui_row_tag_compute(safe_text, row_style);
  if (ui_row_tag[row] == tag) {
    return;  /* Row unchanged — skip redraw. */
  }

  /* Value-only update: label already drawn; value_col match implies label
   * is unchanged (labels are fixed per card row; value_col is reset to 0xFF
   * on title change, so a match here means same screen, same row, same label). */
  if ((row_style == UI_TEXT_ROW_STYLE_TEXT ||
       row_style == UI_TEXT_ROW_STYLE_RESULT) &&
      ui_row_value_col[row] != 0xFFU) {
    value_col = ui_line_value_col(safe_text);
    if (value_col == ui_row_value_col[row] && value_col < 32U) {
      unsigned char vtag = ui_row_tag_compute(safe_text + value_col, 0);
      if (vtag != ui_row_value_tag[row]) {
        ui_screen_write_value(row, value_col, safe_text + value_col);
        ui_highlight_screen_value(
            row, value_col, ZX_COLOUR_BLACK,
            ui_line_is_alert(safe_text) ? ZX_COLOUR_YELLOW : ZX_COLOUR_CYAN,
            1);
        ui_row_value_tag[row] = vtag;
      }
      ui_row_tag[row] = tag;
      return;
    }
  }

  /* Full redraw. */
  ui_screen_write_row(row, safe_text, ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 0);

  if (row_style == UI_TEXT_ROW_STYLE_TEXT ||
      row_style == UI_TEXT_ROW_STYLE_RESULT) {
    ui_style_screen_text_row(row, safe_text);
    value_col = ui_line_value_col(safe_text);
    if (value_col < 32U) {
      ui_row_value_col[row] = value_col;
      ui_row_value_tag[row] = ui_row_tag_compute(safe_text + value_col, 0);
    } else {
      ui_row_value_col[row] = 0xFFU;
    }
  } else {
    ui_row_value_col[row] = 0xFFU;
  }

  ui_row_tag[row] = tag;
}

/*
 * Write row 23 as a 32-char composite: controls text (cols 0-17) followed
 * by the drive status badge (cols 18-31).  Writes one full row then applies
 * per-field colour overrides for the badge.
 *
 * Badge layout (14 cols, 18-31):
 *   col 18:     separator space
 *   cols 19-21: R:Y/R:N/R:?   cyan=ready, yellow=not ready
 *   col 22:     space
 *   cols 23-25: W:Y/W:N/W:?   yellow=protected, cyan=ok
 *   col 26:     space
 *   cols 27-31: M:ON_ / M:OFF  cyan=on, dim=off
 */
static void ui_render_row23(const char *controls) {
    char buf[33];
    unsigned char i;

    memset(buf, ' ', 32);
    buf[32] = '\0';

    if (controls) {
        for (i = 0; i < 18U && controls[i]; i++) buf[i] = controls[i];
    }

    /* badge chars — positions are fixed regardless of ST3 validity */
    buf[19] = 'R'; buf[20] = ':';
    buf[21] = s_badge_st3_valid ? (s_badge_ready ? 'Y' : 'N') : '?';
    buf[23] = 'W'; buf[24] = ':';
    buf[25] = s_badge_st3_valid ? (s_badge_wprot ? 'Y' : 'N') : '?';
    buf[27] = 'M'; buf[28] = ':'; buf[29] = 'O';
    if (s_badge_motor_on) { buf[30] = 'N'; buf[31] = ' '; }
    else                  { buf[30] = 'F'; buf[31] = 'F'; }

    ui_screen_write_row(23, buf, ZX_COLOUR_WHITE, ZX_COLOUR_BLUE, 1);

    /* per-field colour overrides */
    if (s_badge_st3_valid) {
        ui_attr_set_run(23, 19, 3, ZX_COLOUR_WHITE,
                        s_badge_ready ? ZX_COLOUR_CYAN : ZX_COLOUR_YELLOW, 1);
        ui_attr_set_run(23, 23, 3, ZX_COLOUR_WHITE,
                        s_badge_wprot ? ZX_COLOUR_YELLOW : ZX_COLOUR_CYAN, 1);
    }
    ui_attr_set_run(23, 27, 5, ZX_COLOUR_WHITE,
                    s_badge_motor_on ? ZX_COLOUR_CYAN : ZX_COLOUR_BLUE, 1);
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

  /* Keep the frame stable when only controls/result text changes. */
  if (ui_text_screen_active && ui_text_screen_title == title) {
    ui_text_screen_controls = controls;
    return;
  }

  memset(ui_row_tag, 0xFF, sizeof(ui_row_tag));
  memset(ui_row_value_col, 0xFF, sizeof(ui_row_value_col));

  ui_term_clear();
  ui_attr_fill(ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 0);

  title_row[0] = ' ';
  strncpy(&title_row[1], title ? title : "", 31U);
  title_row[32] = '\0';
  ui_screen_write_row(0, title_row, ZX_COLOUR_WHITE, ZX_COLOUR_BLACK, 1);

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
                           const char* result_label, const char* result_value) {
  ui_begin_text_screen(title, controls);

  unsigned char row = 1;
  for (unsigned char i = 0; i < line_count && row < 23U; i++, row++) {
    if (lines[i]) {
      ui_render_cached_text_row(row, lines[i], UI_TEXT_ROW_STYLE_TEXT);
    } else {
      ui_render_cached_text_row(row, "", UI_TEXT_ROW_STYLE_BLANK);
    }
  }

  if ((result_label || result_value) && (unsigned char)(row + 1U) < 24U) {
    char result_buf[48];
    snprintf(result_buf, sizeof(result_buf), "%s%s",
             result_label ? result_label : "", result_value ? result_value : "");
    ui_render_cached_text_row(row, "", UI_TEXT_ROW_STYLE_BLANK);
    ui_render_cached_text_row((unsigned char)(row + 1U), result_buf,
                              UI_TEXT_ROW_STYLE_RESULT);
    row = (unsigned char)(row + 2U);
  }

  while (row < 23U) {
    ui_render_cached_text_row(row, "", UI_TEXT_ROW_STYLE_BLANK);
    row++;
  }

  /* Render blue status bar at row 23 with drive status badge */
  ui_render_row23(controls);
}

/* ----------------------------------------------------------------------- */
/* Hex dump panel (rows 10-22 below the card area; row 23 = status bar)    */
/* ----------------------------------------------------------------------- */

#define HEX_DUMP_HEADER_ROW    10U
#define HEX_DUMP_DATA_ROWS     12U

static const char s_hex_digits[17] = "0123456789ABCDEF";

static void (*s_ui_idle_pump)(void) = 0;
static unsigned int s_hex_dump_scroll = 0;
static unsigned int s_hex_dump_prev_scroll = 0xFFFFU;

void ui_set_idle_pump(void (*pump)(void)) {
  s_ui_idle_pump = pump;
}

void ui_set_hex_dump_scroll(unsigned int row) {
  s_hex_dump_scroll = row;
}

void ui_reset_hex_dump_panel(void) {
  s_hex_dump_scroll = 0;
  s_hex_dump_prev_scroll = 0xFFFFU;
}

/*
 * ui_render_hex_dump_panel — stream sector data preview below the card area.
 *
 * Each row shows 8 bytes as "XX XX XX XX XX XX XX XX AAAAAAAA" (32 chars):
 * hex pairs (3 chars each = 24 cols) then ASCII (8 cols, '.' for non-printable).
 *
 * Row 10: full-width header banner (white ink, blue paper).
 * Rows 11-23: up to 13 rows × 8 bytes = 104 bytes shown, streamed every call.
 */
void ui_render_hex_dump_panel(const unsigned char *data, unsigned int data_len) {
  char row_buf[33];
  unsigned char r, b, col, bv, row_bytes;
  unsigned int offset;

  if (!data || data_len == 0U) {
    ui_reset_hex_dump_panel();
    for (r = HEX_DUMP_HEADER_ROW;
         r <= (unsigned char)(HEX_DUMP_HEADER_ROW + HEX_DUMP_DATA_ROWS); r++) {
      ui_screen_write_row(r, "", ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 0);
    }
    return;
  }

  /* Skip redraw if the scroll position hasn't changed since last render.
   * Seeks reset s_hex_dump_prev_scroll to 0xFFFF, forcing the first draw. */
  if (s_hex_dump_scroll == s_hex_dump_prev_scroll) {
    return;
  }
  s_hex_dump_prev_scroll = s_hex_dump_scroll;

  /* Header: "DATA #N" where N is the 1-based scroll page. */
  snprintf(row_buf, sizeof(row_buf), "DATA #%u", s_hex_dump_scroll + 1U);
  ui_screen_write_row(HEX_DUMP_HEADER_ROW, row_buf,
                      ZX_COLOUR_WHITE, ZX_COLOUR_BLUE, 1);

  /* Render up to HEX_DUMP_DATA_ROWS rows, 8 bytes each, from the scroll
   * offset.  Pump the key latch between rows so presses are not lost during
   * the (potentially long) 13-row render on Z80. */
  for (r = 0U; r < HEX_DUMP_DATA_ROWS; r++) {
    if (s_ui_idle_pump) s_ui_idle_pump();

    offset = (unsigned int)(s_hex_dump_scroll + r) * 8U;
    if (offset >= data_len) {
      ui_screen_write_row((unsigned char)(HEX_DUMP_HEADER_ROW + 1U + r), "",
                          ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 0);
      continue;
    }
    row_bytes = (unsigned char)(data_len - offset > 8U ? 8U : data_len - offset);
    col = 0U;
    for (b = 0U; b < 8U; b++) {
      if (b < row_bytes) {
        bv = data[offset + b];
        row_buf[col++] = s_hex_digits[(bv >> 4) & 0x0FU];
        row_buf[col++] = s_hex_digits[bv & 0x0FU];
        row_buf[col++] = ' ';
        row_buf[24U + b] = bv >= 0x20U && bv < 0x7FU ? (char)bv : '.';
      } else {
        row_buf[col++] = ' ';
        row_buf[col++] = ' ';
        row_buf[col++] = ' ';
        row_buf[24U + b] = ' ';
      }
    }
    row_buf[32] = '\0';
    ui_screen_write_row((unsigned char)(HEX_DUMP_HEADER_ROW + 1U + r), row_buf,
                        ZX_COLOUR_BLUE, ZX_COLOUR_WHITE, 1);
  }
}

