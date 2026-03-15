/*
 * ZX Spectrum +3 Disk Drive Tester
 *
 * Talks directly to the internal +3 floppy controller, a uPD765A-compatible
 * FDC.
 *
 * Ports:
 *    0x1FFD  +3 system control (bit 3 controls motor), also memory control and
 * is write-only
 *    0x2FFD  FDC Main Status Register (MSR), read-only
 *    0x3FFD  FDC Data Register, read/write
 *
 * Notes:
 * - Port 0x1FFD also controls memory/ROM paging. Only use bit 3 to avoid
 * messing paging up.
 *
 * Build examples:
 *   Bootable +3 disc (produces .dsk):
 *     zcc +zx -clib=new -subtype=plus3 -create-app disk_tester.c -o
 * out/disk_tester
 *
 *   Tape (produces .tap):
 *     zcc +zx -clib=new -create-app disk_tester.c -o out/disk_tester
 */

#include "disk_tester.h"
#include "menu_system.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

extern unsigned char inportb(unsigned short port);
extern void outportb(unsigned short port, unsigned char value);
extern void set_motor_on(void);
extern void set_motor_off(void);

#define LOOPS_PER_MS 250U

static volatile unsigned int delay_spin_sink;

/* Busy-wait delay without classic inline asm, suitable for newlib builds. */
static void delay_ms(unsigned int ms) {
  unsigned int i, j;
  for (i = 0; i < ms; i++) {
    for (j = 0; j < LOOPS_PER_MS; j++) {
      delay_spin_sink++;
    }
  }
}

/* +3 ports */
#define FDC_MSR_PORT 0x2FFD
#define FDC_DATA_PORT 0x3FFD

#ifndef IOCTL_OTERM_PAUSE
#define IOCTL_OTERM_PAUSE 0xC042
#endif

#ifndef IOCTL_OTERM_FONT
#define IOCTL_OTERM_FONT 0x0802
#endif

#ifndef IOCTL_OTERM_CLS
#define IOCTL_OTERM_CLS 0x0102
#endif

#ifndef IOCTL_OTERM_RESET_SCROLL
#define IOCTL_OTERM_RESET_SCROLL 0x0202
#endif

#ifndef COMPACT_UI
#define COMPACT_UI 0
#endif

/* uPD765A MSR bits */
#define MSR_RQM 0x80 /* Request for Master */
#define MSR_DIO 0x40 /* Data direction: 1 = FDC->CPU */
#define FDC_DRIVE 0 /* internal drive is drive 0 */
#define FDC_RQM_TIMEOUT 20000U
#define FDC_CMD_BYTE_GAP_US 16U
#define SEEK_PREP_DELAY_MS 2U
#define SEEK_BUSY_TIMEOUT_MS 900U
#define SEEK_SENSE_RETRIES 48U
#define SEEK_SENSE_RETRY_DELAY_MS 2U
#define DRIVE_READY_TIMEOUT_MS 500U
#define DRIVE_READY_POLL_MS 5U
#define MOTOR_OFF_SETTLE_MS 35U
#define READ_LOOP_PAUSE_STEPS 4U
#define READ_LOOP_PAUSE_MS 6U

/* Approximate sub-millisecond pacing for FDC byte gaps. */
static void delay_us_approx(unsigned int us) {
  unsigned int i, j;
  for (i = 0; i < us; i++) {
    for (j = 0; j < 4U; j++) {
      delay_spin_sink++;
    }
  }
}
#define RPM_LOOP_DELAY_MS 80U
#define RPM_FAIL_DELAY_MS 80U
/*
 * Real +3 drives vary with age; a slightly longer spin-up improves reliability
 * on older mechanics without affecting emulator runs materially.
 */
#define MOTOR_SPINUP_DELAY_MS 650U

#ifdef DEBUG
#define DEBUG_ENABLED 1
#else
#define DEBUG_ENABLED 0
#endif

static unsigned int dbg_seek_wait_loops;
static unsigned char dbg_seek_sense_tries;
static unsigned char dbg_seek_last_st0;
#if DEBUG_ENABLED
static const unsigned char debug_enabled = DEBUG_ENABLED;
/*
 * Captured at startup before any paging changes, for debug visibility.
 * Shows the state DivMMC left the system in.
 */
static unsigned char startup_bank678;
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

#ifndef HEADLESS_ROM_FONT
#define HEADLESS_ROM_FONT 0
#endif

#if COMPACT_UI
typedef struct {
  unsigned char ch;
  unsigned char rows[8];
} CompactGlyph;

static void set_compact_glyph(unsigned char *font,
                              unsigned char ch,
                              const unsigned char rows[8]) {
  unsigned char *glyph = &font[((unsigned short)ch) * 8U];
  unsigned char r;

  for (r = 0; r < 8; r++) {
    glyph[r] = rows[r];
  }
}

static void copy_font_glyph(unsigned char *font,
                            unsigned char dst,
                            unsigned char src) {
  memcpy(&font[((unsigned short)dst) * 8U],
         &font[((unsigned short)src) * 8U],
         8U);
}

static void apply_compact_font(unsigned char *font) {
  unsigned char ch;
  unsigned int i;
  /*
  * Source: epto/epto-fonts (digital-6x6), GPL-2.0 repository.
   * We use an ASCII subset required by the UI.
   */
  static const unsigned char fallback_rows[8] = {
      0x00, 0x7E, 0xA5, 0x99, 0x99, 0xA5, 0x7E, 0x00};
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

static void init_ui_font(void) {
#if HEADLESS_ROM_FONT
  /* Headless/OCR builds use ROM glyphs for maximum recognition stability. */
  memcpy(font_ram, (const void *)0x3C00, sizeof(font_ram));
#elif COMPACT_UI
  /* Compact GUI uses a fully custom sprite-like glyph set by default. */
  memcpy(font_rom, (const void *)0x3C00, sizeof(font_rom));
  apply_compact_font(font_ram);
  compact_font_active = 1;
#else
  /* Copy ROM font first so paging changes cannot corrupt rendered text. */
  memcpy(font_ram, (const void *)0x3C00, sizeof(font_ram));

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

static void select_rom_font(void) {
  if (compact_font_active) {
    ioctl(1, IOCTL_OTERM_FONT, font_rom);
    compact_font_active = 0;
  }
}

/*
 * Explicit print mode commands for compact GUI builds.
 * - compact: uses the fully custom 8x8 glyph set.
 * - rom: uses untouched ROM glyphs copied to private RAM.
 */
void ui_print_command_compact(void) {
  select_compact_font();
}

void ui_print_command_rom(void) {
  select_rom_font();
}

#endif

/* Test results storage */
typedef struct {
  unsigned char motor_test_pass;
  unsigned char sense_drive_pass;
  unsigned char recalibrate_pass;
  unsigned char seek_pass;
  unsigned char read_id_pass;
} TestResults;

static TestResults results;
static unsigned char last_test_failed;

#define REPORT_STATUS_NONE     0U
#define REPORT_STATUS_READY    1U
#define REPORT_STATUS_RUNNING  2U
#define REPORT_STATUS_COMPLETE 3U

static unsigned char report_status_code;

static void disable_terminal_auto_pause(void) {
  /* Avoid hidden key waits when output scrolls beyond one screen. */
  ioctl(1, IOCTL_OTERM_PAUSE, 0);
}

static const char* pass_fail(unsigned char ok) {
  return ok ? "PASS" : "FAIL";
}

static const char* yes_no(unsigned char flag) {
  return flag ? "YES" : "NO";
}

static unsigned char pass_count(void) {
  return (unsigned char)(results.motor_test_pass + results.sense_drive_pass +
                         results.recalibrate_pass + results.seek_pass +
                         results.read_id_pass);
}

static void reset_report_progress(void) {
  report_status_code = REPORT_STATUS_NONE;
}

static const char* report_status_text(void) {
  switch (report_status_code) {
    case REPORT_STATUS_READY:
      return "STATUS: READY";
    case REPORT_STATUS_RUNNING:
      return "STATUS: RUNNING";
    case REPORT_STATUS_COMPLETE:
      return "STATUS: COMPLETE";
    default:
      return "STATUS: NO TESTS RUN";
  }
}

static void set_report_status(unsigned char status_code) {
  report_status_code = status_code;
}

static void ui_term_clear(void) {
  ioctl(1, IOCTL_OTERM_CLS, 0);
  ioctl(1, IOCTL_OTERM_RESET_SCROLL, 0);
}

/* ZX colour IDs */
#define ZX_COLOUR_BLACK 0
#define ZX_COLOUR_BLUE 1
#define ZX_COLOUR_RED 2
#define ZX_COLOUR_WHITE 7
#define ZX_COLOUR_CYAN 5
#define ZX_COLOUR_YELLOW 6
#define ZX_ATTR_BASE 0x5800

#define ZX_PIXELS_BASE 0x4000
#define ZX_PIXELS_SIZE 0x1800U
#define ZX_ATTR_SIZE 0x300U

static const unsigned char* ui_active_font_ptr(void) {
#if COMPACT_UI && !HEADLESS_ROM_FONT
  return compact_font_active ? font_ram : font_rom;
#else
  return font_ram;
#endif
}

static unsigned short zx_pixel_offset(unsigned char y, unsigned char xbyte) {
  return (unsigned short)((((unsigned short)(y & 0xC0U)) << 5) |
                          (((unsigned short)(y & 0x07U)) << 8) |
                          (((unsigned short)(y & 0x38U)) << 2) |
                          xbyte);
}

static void ui_screen_put_char(unsigned char row, unsigned char col, char ch) {
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

static void ui_attr_set_cell(unsigned char row,
                             unsigned char col,
                             unsigned char ink,
                             unsigned char paper,
                             unsigned char bright) {
  volatile unsigned char* attr = (volatile unsigned char*)ZX_ATTR_BASE;
  if (row >= 24 || col >= 32) return;
  attr[(unsigned short)row * 32U + col] =
      (unsigned char)(((bright ? 1U : 0U) << 6) |
                      ((paper & 0x07U) << 3) |
                      (ink & 0x07U));
}

static void ui_attr_fill(unsigned char ink,
                         unsigned char paper,
                         unsigned char bright) {
  unsigned char row;
  unsigned char col;
  for (row = 0; row < 24; row++) {
    for (col = 0; col < 32; col++) {
      ui_attr_set_cell(row, col, ink, paper, bright);
    }
  }
}

#if 0
static unsigned char ui_back_pixels[ZX_PIXELS_SIZE];
static unsigned char ui_back_attrs[ZX_ATTR_SIZE];

static void ui_back_attr_set_cell(unsigned char row,
                                  unsigned char col,
                                  unsigned char ink,
                                  unsigned char paper,
                                  unsigned char bright) {
  if (row >= 24 || col >= 32) return;
  ui_back_attrs[(unsigned short)row * 32U + col] =
      (unsigned char)(((bright ? 1U : 0U) << 6) |
                      ((paper & 0x07U) << 3) |
                      (ink & 0x07U));
}

static void ui_back_fill(unsigned char ink,
                         unsigned char paper,
                         unsigned char bright) {
  unsigned short i;
  unsigned char attr =
      (unsigned char)(((bright ? 1U : 0U) << 6) | ((paper & 0x07U) << 3) |
                      (ink & 0x07U));

  memset(ui_back_pixels, 0x00, sizeof(ui_back_pixels));
  for (i = 0; i < ZX_ATTR_SIZE; i++) {
    ui_back_attrs[i] = attr;
  }
}

static void ui_back_put_char(unsigned char row, unsigned char col, char ch) {
  const unsigned char* font = ui_active_font_ptr();
  const unsigned char* glyph;
  unsigned char gy;

  if (row >= 24 || col >= 32) return;
  glyph = &font[((unsigned short)(unsigned char)ch) * 8U];
  for (gy = 0; gy < 8; gy++) {
    unsigned char y = (unsigned char)(row * 8U + gy);
    ui_back_pixels[zx_pixel_offset(y, col)] = glyph[gy];
  }
}

static void ui_back_put_text(unsigned char row, unsigned char col, const char* text) {
  while (*text && col < 32) {
    ui_back_put_char(row, col, *text++);
    col++;
  }
}

static void ui_back_blit(void) {
  /* Present a fully prepared frame without a visible pre-clear flash. */
  memcpy((void*)ZX_PIXELS_BASE, ui_back_pixels, sizeof(ui_back_pixels));
  memcpy((void*)ZX_ATTR_BASE, ui_back_attrs, sizeof(ui_back_attrs));
}

static void ui_back_header_bar(const char* title) {
  unsigned char i;
  static const unsigned char STRIPE_PAPER[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  static const unsigned char STRIPE_INK[8]   = {7, 7, 7, 7, 0, 0, 0, 0};

  ui_back_put_text(0, 1, title);
  for (i = 0; i < 32; i++) {
    ui_back_attr_set_cell(0, i, ZX_COLOUR_WHITE, ZX_COLOUR_BLACK, 1);
  }
  for (i = 0; i < 8; i++) {
    ui_back_attr_set_cell(0, (unsigned char)(24 + i),
                          STRIPE_INK[i], STRIPE_PAPER[i], 1);
  }
}

#if 0
static void ui_back_highlight_kv_value(unsigned char row,
                                       unsigned char start_col,
                                       unsigned char ink,
                                       unsigned char paper,
                                       unsigned char bright) {
  unsigned char col;
  for (col = start_col; col < 32; col++) {
    ui_back_attr_set_cell(row, col, ink, paper, bright);
  }
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
      return (unsigned char)(1U + col);
    }
    col++;
  }

  return 32;
}

static unsigned char ui_line_is_alert(const char* text) {
  if (!text) return 0;
  return (unsigned char)(strstr(text, "FAIL") != 0 ||
                         strstr(text, "NO REV") != 0 ||
                         strstr(text, "NOT READY") != 0 ||
                         strstr(text, "INVALID") != 0 ||
                         strstr(text, "BAD") != 0 ||
                         strstr(text, "N/A") != 0 ||
                         strstr(text, "TIMEOUT") != 0 ||
                         strstr(text, "CHECK MEDIA") != 0 ||
                         strstr(text, "OUT-OF-RANGE") != 0);
}

static void ui_style_text_row(unsigned char row,
                              const char* text,
                              unsigned char paper) {
  unsigned char value_col;

  if (!text || row >= 24) return;
  ui_back_attr_set_cell(row, 1, ZX_COLOUR_BLUE, paper, 1);
  value_col = ui_line_value_col(text);
  if (value_col < 32U) {
    ui_back_highlight_kv_value(
        row,
        value_col,
        ZX_COLOUR_BLACK,
        ui_line_is_alert(text) ? ZX_COLOUR_YELLOW : ZX_COLOUR_CYAN,
        1);
  }
}

static void ui_render_text_screen(const char* title,
                                  const char* controls,
                                  const char* const* lines,
                                  unsigned char line_count,
                                  const char* result) {
  unsigned char i;
  unsigned char row = 4;

  ui_back_fill(ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 0);
  ui_back_header_bar(title);

  if (controls) {
    ui_back_put_text(2, 1, controls);
    for (i = 0; i < 32; i++) {
      ui_back_attr_set_cell(2, i, ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 1);
    }
  }

  for (i = 0; i < line_count && row < 22U; i++, row++) {
    if (!lines[i]) continue;
    ui_back_put_text(row, 1, lines[i]);
    ui_style_text_row(row, lines[i], ZX_COLOUR_WHITE);
  }

  if (result && row < 23U) {
    row++;
    ui_back_put_text(row, 1, result);
    ui_style_text_row(row, result, ZX_COLOUR_WHITE);
    if (ui_line_is_alert(result)) {
      ui_back_highlight_kv_value(row, ui_line_value_col(result),
                                 ZX_COLOUR_BLACK, ZX_COLOUR_YELLOW, 1);
    } else {
      ui_back_highlight_kv_value(row, ui_line_value_col(result),
                                 ZX_COLOUR_BLACK, ZX_COLOUR_CYAN, 1);
    }
  }

  ui_back_blit();
}
#endif

#endif

#if 1
static void ui_apply_menu_row_visual(unsigned char index, unsigned char selected) {
  const MenuItem *items = menu_items();
  unsigned char count = menu_item_count();
  unsigned char row;
  unsigned char col;
  unsigned char paper;

  if (index >= count) return;

  row = (unsigned char)(3U + index);
  paper = selected ? ZX_COLOUR_CYAN : ZX_COLOUR_WHITE;
  for (col = 0; col < 32U; col++) {
    ui_attr_set_cell(row, col, ZX_COLOUR_BLACK, paper, 1);
  }
  if (items[index].hot_col < 31U) {
    ui_attr_set_cell(row, (unsigned char)(items[index].hot_col + 1U), ZX_COLOUR_BLUE, paper, 1);
  }
  ui_screen_put_char(row, 31, selected ? '~' : ' ');
}

static void ui_update_main_menu_selection(unsigned char old_index,
                                          unsigned char new_index) {
  if (old_index == new_index) return;
  ui_apply_menu_row_visual(old_index, 0);
  ui_apply_menu_row_visual(new_index, 1);
}

static void ui_render_main_menu(unsigned char selected_index, unsigned char total) {
  const MenuItem *items = menu_items();
  unsigned char count = menu_item_count();
  unsigned char i;
  unsigned char col;
  char status_line[29];
  static const unsigned char STRIPE_PAPER[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  static const unsigned char STRIPE_INK[8]   = {7, 7, 7, 7, 0, 0, 0, 0};

  ui_term_clear();
  printf(" ZX +3 DISK TESTER\n");
  if (total == 0) {
    strcpy(status_line, "STATUS: NO TESTS RUN");
  } else {
    sprintf(status_line, "STATUS: %u/5 PASS", (unsigned int)total);
  }
  printf("%s\n\n", status_line);

  for (i = 0; i < count; i++) {
    printf(" %s\n", items[i].label);
  }

  printf("\n^: UP  v: DOWN\n");
  printf("ENTER: SELECT\n");

  /* Reapply +3-style colour layout on top of terminal text output. */
  ui_attr_fill(ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 0);
  for (col = 0; col < 32; col++) {
    ui_attr_set_cell(0, col, ZX_COLOUR_WHITE, ZX_COLOUR_BLACK, 1);
    ui_attr_set_cell(15, col, ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 1);
    ui_attr_set_cell(16, col, ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 1);
  }
  for (col = 0; col < 8; col++) {
    ui_attr_set_cell(0, (unsigned char)(24 + col), STRIPE_INK[col], STRIPE_PAPER[col], 1);
  }
  for (i = 0; i < count; i++) {
    ui_apply_menu_row_visual(i, (unsigned char)(i == selected_index));
  }
}

static void ui_render_report_card(void) {
  unsigned char total = pass_count();
  unsigned char col;
  static const unsigned char STRIPE_PAPER[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  static const unsigned char STRIPE_INK[8]   = {7, 7, 7, 7, 0, 0, 0, 0};

  ui_term_clear();
  printf(" TEST REPORT CARD\n");
  printf("------------------------------\n");
    printf("%s\n\n", report_status_text());
  printf("MOTOR  [%c%c%c%c%c%c%c%c] %s\n",
       results.motor_test_pass ? '#' : '.',
       results.motor_test_pass ? '#' : '.',
       results.motor_test_pass ? '#' : '.',
       results.motor_test_pass ? '#' : '.',
       results.motor_test_pass ? '#' : '.',
       results.motor_test_pass ? '#' : '.',
       results.motor_test_pass ? '#' : '.',
       results.motor_test_pass ? '#' : '.',
       pass_fail(results.motor_test_pass));
  printf("DRIVE  [%c%c%c%c%c%c%c%c] %s\n",
       results.sense_drive_pass ? '#' : '.',
       results.sense_drive_pass ? '#' : '.',
       results.sense_drive_pass ? '#' : '.',
       results.sense_drive_pass ? '#' : '.',
       results.sense_drive_pass ? '#' : '.',
       results.sense_drive_pass ? '#' : '.',
       results.sense_drive_pass ? '#' : '.',
       results.sense_drive_pass ? '#' : '.',
       pass_fail(results.sense_drive_pass));
  printf("RECAL  [%c%c%c%c%c%c%c%c] %s\n",
       results.recalibrate_pass ? '#' : '.',
       results.recalibrate_pass ? '#' : '.',
       results.recalibrate_pass ? '#' : '.',
       results.recalibrate_pass ? '#' : '.',
       results.recalibrate_pass ? '#' : '.',
       results.recalibrate_pass ? '#' : '.',
       results.recalibrate_pass ? '#' : '.',
       results.recalibrate_pass ? '#' : '.',
       pass_fail(results.recalibrate_pass));
  printf("SEEK   [%c%c%c%c%c%c%c%c] %s\n",
       results.seek_pass ? '#' : '.',
       results.seek_pass ? '#' : '.',
       results.seek_pass ? '#' : '.',
       results.seek_pass ? '#' : '.',
       results.seek_pass ? '#' : '.',
       results.seek_pass ? '#' : '.',
       results.seek_pass ? '#' : '.',
       results.seek_pass ? '#' : '.',
       pass_fail(results.seek_pass));
  printf("READID [%c%c%c%c%c%c%c%c] %s\n",
       results.read_id_pass ? '#' : '.',
       results.read_id_pass ? '#' : '.',
       results.read_id_pass ? '#' : '.',
       results.read_id_pass ? '#' : '.',
       results.read_id_pass ? '#' : '.',
       results.read_id_pass ? '#' : '.',
       results.read_id_pass ? '#' : '.',
       results.read_id_pass ? '#' : '.',
       pass_fail(results.read_id_pass));
  printf("\nOVERALL [%c%c%c%c%c] %u/5 PASS\n",
         total > 0 ? '#' : '.',
         total > 1 ? '#' : '.',
         total > 2 ? '#' : '.',
         total > 3 ? '#' : '.',
         total > 4 ? '#' : '.',
         total);

  ui_attr_fill(ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 0);
  for (col = 0; col < 32; col++) {
    ui_attr_set_cell(0, col, ZX_COLOUR_WHITE, ZX_COLOUR_BLACK, 1);
  }
  for (col = 0; col < 8; col++) {
    ui_attr_set_cell(0, (unsigned char)(24 + col), STRIPE_INK[col], STRIPE_PAPER[col], 1);
  }
}
#else
static void ui_render_main_menu(unsigned char selected_index, unsigned char total) {
  const MenuItem *items = menu_items();
  unsigned char count = menu_item_count();
  unsigned char i;
  char status_line[29];
  /* Compact menu body with white background and black text. */
  ui_back_fill(ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 0);

  /* Row 0: black header bar with title text and Spectrum rainbow logo. */
  ui_back_header_bar("ZX +3 DISK TESTER");

  if (total == 0) {
    strcpy(status_line, "STATUS: NO TESTS RUN");
  } else {
    sprintf(status_line, "STATUS: %u/5 PASS", (unsigned int)total);
  }
  ui_back_put_text(1, 0, status_line);

  /* Menu items at rows 3-13, preserving existing menu text layout. */
  for (i = 0; i < count; i++) {
    unsigned char row = (unsigned char)(3 + i);
    unsigned char paper = ZX_COLOUR_WHITE;
    ui_back_put_text(row, 1, items[i].label);

    if (i == selected_index) {
      unsigned char col;
      for (col = 0; col < 32; col++) {
        ui_back_attr_set_cell(row, col, ZX_COLOUR_BLACK, ZX_COLOUR_CYAN, 1);
      }
      paper = ZX_COLOUR_CYAN;
#if !COMPACT_UI
      /* OCR-only selected-row marker in non-compact builds. */
      ui_back_put_char(row, 31, '~');
#endif
    }
    if (items[i].hot_col < 32U) {
      ui_back_attr_set_cell(row, items[i].hot_col, ZX_COLOUR_BLUE, paper, 1);
    }
  }

  ui_back_put_text(15, 0, "^: UP  v: DOWN");
  ui_back_put_text(16, 0, "ENTER: SELECT");

  for (i = 0; i < 32; i++) {
    ui_back_attr_set_cell(15, i, ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 1);
    ui_back_attr_set_cell(16, i, ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 1);
  }

  ui_back_blit();
}

static void ui_render_report_card(void) {
  unsigned char total = pass_count();
  char line[33];

  ui_back_fill(ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 0);
  ui_back_header_bar("TEST REPORT CARD");
  ui_back_put_text(1, 0, report_status_text());
  ui_back_put_text(3, 0, "+------------------------------+");

  sprintf(line, "MOTOR  [%c%c%c%c%c%c%c%c] %s",
        results.motor_test_pass ? '#' : '.',
        results.motor_test_pass ? '#' : '.',
        results.motor_test_pass ? '#' : '.',
        results.motor_test_pass ? '#' : '.',
        results.motor_test_pass ? '#' : '.',
        results.motor_test_pass ? '#' : '.',
        results.motor_test_pass ? '#' : '.',
        results.motor_test_pass ? '#' : '.',
        pass_fail(results.motor_test_pass));
  ui_back_put_text(5, 0, line);

  sprintf(line, "DRIVE  [%c%c%c%c%c%c%c%c] %s",
        results.sense_drive_pass ? '#' : '.',
        results.sense_drive_pass ? '#' : '.',
        results.sense_drive_pass ? '#' : '.',
        results.sense_drive_pass ? '#' : '.',
        results.sense_drive_pass ? '#' : '.',
        results.sense_drive_pass ? '#' : '.',
        results.sense_drive_pass ? '#' : '.',
        results.sense_drive_pass ? '#' : '.',
        pass_fail(results.sense_drive_pass));
  ui_back_put_text(6, 0, line);

  sprintf(line, "RECAL  [%c%c%c%c%c%c%c%c] %s",
        results.recalibrate_pass ? '#' : '.',
        results.recalibrate_pass ? '#' : '.',
        results.recalibrate_pass ? '#' : '.',
        results.recalibrate_pass ? '#' : '.',
        results.recalibrate_pass ? '#' : '.',
        results.recalibrate_pass ? '#' : '.',
        results.recalibrate_pass ? '#' : '.',
        results.recalibrate_pass ? '#' : '.',
        pass_fail(results.recalibrate_pass));
  ui_back_put_text(7, 0, line);

  sprintf(line, "SEEK   [%c%c%c%c%c%c%c%c] %s",
        results.seek_pass ? '#' : '.',
        results.seek_pass ? '#' : '.',
        results.seek_pass ? '#' : '.',
        results.seek_pass ? '#' : '.',
        results.seek_pass ? '#' : '.',
        results.seek_pass ? '#' : '.',
        results.seek_pass ? '#' : '.',
        results.seek_pass ? '#' : '.',
        pass_fail(results.seek_pass));
  ui_back_put_text(8, 0, line);

  sprintf(line, "READID [%c%c%c%c%c%c%c%c] %s",
        results.read_id_pass ? '#' : '.',
        results.read_id_pass ? '#' : '.',
        results.read_id_pass ? '#' : '.',
        results.read_id_pass ? '#' : '.',
        results.read_id_pass ? '#' : '.',
        results.read_id_pass ? '#' : '.',
        results.read_id_pass ? '#' : '.',
        results.read_id_pass ? '#' : '.',
        pass_fail(results.read_id_pass));
  ui_back_put_text(9, 0, line);

  sprintf(line, "OVERALL [%c%c%c%c%c] %u/5 PASS",
          total > 0 ? '#' : '.',
          total > 1 ? '#' : '.',
          total > 2 ? '#' : '.',
          total > 3 ? '#' : '.',
          total > 4 ? '#' : '.',
          total);
  ui_back_put_text(11, 0, line);

  ui_back_blit();
}
#endif

static void ui_test_header(const char* title);

#if 1
static void ui_render_text_screen(const char* title,
                                  const char* controls,
                                  const char* const* lines,
                                  unsigned char line_count,
                                  const char* result) {
  unsigned char i;

  ui_test_header(title);
  if (controls) printf("%s\n", controls);
  for (i = 0; i < line_count; i++) {
    if (lines[i]) printf("%s\n", lines[i]);
  }
  if (result) printf("\n%s\n", result);
}

static void ui_render_track_loop_screen(unsigned char track,
                                        unsigned int pass_count,
                                        unsigned int fail_count,
                                        const char* last1,
                                        const char* last2,
                                        const char* result) {
  char track_line[32];
  char pass_line[32];
  char fail_line[32];
  const char* lines[5];

  sprintf(track_line, "TRACK : %3u", (unsigned int)track);
  sprintf(pass_line, "PASS  : %3u", pass_count);
  sprintf(fail_line, "FAIL  : %3u", fail_count);
  lines[0] = track_line;
  lines[1] = pass_line;
  lines[2] = fail_line;
  lines[3] = last1 ? last1 : "LAST  : OK";
  lines[4] = last2 ? last2 : "INFO  : READY";

  ui_render_text_screen("READ TRACK DATA LOOP",
                        "KEYS  : J/K TRACK  ENTER/X EXIT",
                        lines,
                        5,
                        result ? result : "RESULT: ACTIVE");
}

static void ui_render_rpm_loop_screen(unsigned int rpm,
                                      unsigned char rpm_valid,
                                      unsigned int pass_count,
                                      unsigned int fail_count,
                                      const char* last1,
                                      const char* last2,
                                      const char* result) {
  char rpm_line[32];
  char pass_line[32];
  char fail_line[32];
  const char* lines[5];

  if (rpm_valid) {
    sprintf(rpm_line, "RPM   : %3u", rpm);
  } else {
    strcpy(rpm_line, "RPM   : ---");
  }
  sprintf(pass_line, "PASS  : %3u", pass_count);
  sprintf(fail_line, "FAIL  : %3u", fail_count);
  lines[0] = rpm_line;
  lines[1] = pass_line;
  lines[2] = fail_line;
  lines[3] = last1 ? last1 : "LAST  : WAITING";
  lines[4] = last2 ? last2 : "INFO  : READY";

  ui_render_text_screen("DISK RPM CHECK LOOP",
                        "KEYS  : ENTER/X EXIT",
                        lines,
                        5,
                        result ? result : "RESULT: ACTIVE");
}
#else
static void ui_render_track_loop_screen(unsigned char track,
                                        unsigned int pass_count,
                                        unsigned int fail_count,
                                        const char* last1,
                                        const char* last2,
                                        const char* result) {
  char track_line[32];
  char pass_line[32];
  char fail_line[32];
  const char* lines[5];

  sprintf(track_line, "TRACK : %3u", (unsigned int)track);
  sprintf(pass_line, "PASS  : %3u", pass_count);
  sprintf(fail_line, "FAIL  : %3u", fail_count);
  lines[0] = track_line;
  lines[1] = pass_line;
  lines[2] = fail_line;
  lines[3] = last1 ? last1 : "LAST  : OK";
  lines[4] = last2 ? last2 : "INFO  : READY";

  ui_render_text_screen("READ TRACK DATA LOOP",
                        "KEYS  : J/K TRACK  ENTER/X EXIT",
                        lines,
                        5,
                        result ? result : "RESULT: ACTIVE");
}

static void ui_render_rpm_loop_screen(unsigned int rpm,
                                      unsigned char rpm_valid,
                                      unsigned int pass_count,
                                      unsigned int fail_count,
                                      const char* last1,
                                      const char* last2,
                                      const char* result) {
  char rpm_line[32];
  char pass_line[32];
  char fail_line[32];
  const char* lines[5];

  if (rpm_valid) {
    sprintf(rpm_line, "RPM   : %3u", rpm);
  } else {
    strcpy(rpm_line, "RPM   : ---");
  }
  sprintf(pass_line, "PASS  : %3u", pass_count);
  sprintf(fail_line, "FAIL  : %3u", fail_count);
  lines[0] = rpm_line;
  lines[1] = pass_line;
  lines[2] = fail_line;
  lines[3] = last1 ? last1 : "LAST  : WAITING";
  lines[4] = last2 ? last2 : "INFO  : READY";

  ui_render_text_screen("DISK RPM CHECK LOOP",
                        "KEYS  : ENTER/X EXIT",
                        lines,
                        5,
                        result ? result : "RESULT: ACTIVE");
}
#endif

static int read_key_blocking(void);
static int read_enter_blocking(void);

static void ui_test_header(const char* title) {
#if COMPACT_UI && !HEADLESS_ROM_FONT
  /* Force compact 6x6-style font in human mode before drawing test screens. */
  ui_print_command_compact();
#endif
#if 1
  ui_term_clear();
  printf(" %s\n", title);
  printf("------------------------------\n");
  {
    unsigned char col;
    static const unsigned char STRIPE_PAPER[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    static const unsigned char STRIPE_INK[8]   = {7, 7, 7, 7, 0, 0, 0, 0};
    ui_attr_fill(ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 0);
    for (col = 0; col < 32; col++) {
      ui_attr_set_cell(0, col, ZX_COLOUR_WHITE, ZX_COLOUR_BLACK, 1);
    }
    for (col = 0; col < 8; col++) {
      ui_attr_set_cell(0, (unsigned char)(24 + col), STRIPE_INK[col], STRIPE_PAPER[col], 1);
    }
  }
#else
  ui_back_fill(ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 0);
  ui_back_header_bar(title);
  ui_back_blit();
  printf("\n");
#endif
}

static void wait_for_menu_after_failure(void) {
  if (last_test_failed) {
    printf("\nFAIL - PRESS ENTER FOR MENU\n");
    fflush(stdout);
    read_enter_blocking();
  }
}

static const char* read_id_failure_reason(unsigned char st1, unsigned char st2) {
  if (st1 & 0x01) return "Missing ID address mark";
  if (st1 & 0x02) return "Write protect";
  if (st1 & 0x04) return "No data";
  if (st1 & 0x10) return "Overrun";
  if (st1 & 0x20) return "CRC error";
  if (st1 & 0x80) return "End of cylinder";
  if (st2 & 0x01) return "Missing data address mark";
  if (st2 & 0x02) return "Bad cylinder";
  if (st2 & 0x04) return "Scan not satisfied";
  if (st2 & 0x08) return "Scan equal hit";
  if (st2 & 0x10) return "Wrong cylinder";
  if (st2 & 0x20) return "CRC in data field";
  if (st2 & 0x40) return "Control mark";
  return "Unspecified controller/media error";
}

typedef struct {
  unsigned short row_port;
  unsigned char bit_mask;
  char key;
} KeyMap;

static const KeyMap keymap[] = {
    {0xF7FE, 0x01, '1'}, {0xF7FE, 0x02, '2'}, {0xF7FE, 0x04, '3'},
    {0xF7FE, 0x08, '4'}, {0xF7FE, 0x10, '5'}, {0xEFFE, 0x10, '6'},
  {0xEFFE, 0x08, '7'},
    {0xFDFE, 0x01, 'A'}, {0x7FFE, 0x08, 'C'}, {0xFDFE, 0x04, 'D'},
    {0xFBFE, 0x04, 'E'}, {0xFEFE, 0x04, 'X'}, {0xBFFE, 0x08, 'J'},
    {0xBFFE, 0x04, 'K'},
    {0xEFFE, 0x01, '0'}, {0xFBFE, 0x01, 'Q'}, {0xFBFE, 0x08, 'R'},
    {0x7FFE, 0x01, ' '}, {0xBFFE, 0x01, '\n'}};

static unsigned char break_pressed(void) {
  unsigned char caps_shift = (unsigned char)(inportb(0xFEFE) & 0x01);
  unsigned char space = (unsigned char)(inportb(0x7FFE) & 0x01);
  return (unsigned char)((caps_shift == 0) && (space == 0));
}

static unsigned char x_pressed(void) {
  return (unsigned char)((inportb(0xFEFE) & 0x04) == 0);
}

static unsigned char j_pressed(void) {
  return (unsigned char)((inportb(0xBFFE) & 0x08) == 0);
}

static unsigned char k_pressed(void) {
  return (unsigned char)((inportb(0xBFFE) & 0x04) == 0);
}

static unsigned char enter_pressed(void) {
  return (unsigned char)((inportb(0xBFFE) & 0x01) == 0);
}

static unsigned short frame_ticks(void) {
  volatile unsigned char* frames = (volatile unsigned char*)0x5C78;
  return (unsigned short)(frames[0] | ((unsigned short)frames[1] << 8));
}

static void wait_for_key_release(unsigned short row_port, unsigned char bit_mask) {
  while ((inportb(row_port) & bit_mask) == 0) {
  }
}

static int read_key_blocking(void) {
  unsigned int i;

  for (;;) {
    if (break_pressed()) {
      while (break_pressed()) {
      }
      return 27;
    }

    for (i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++) {
      if ((inportb(keymap[i].row_port) & keymap[i].bit_mask) == 0) {
        char key = keymap[i].key;
        wait_for_key_release(keymap[i].row_port, keymap[i].bit_mask);
        return key;
      }
    }
  }
}

static int read_enter_blocking(void) {
  for (;;) {
    if (break_pressed()) {
      while (break_pressed()) {
      }
      return 27;
    }
    if (enter_pressed()) {
      wait_for_key_release(0xBFFE, 0x01);
      return '\n';
    }
  }
}

static unsigned char loop_exit_requested(void) {
  if (break_pressed()) {
    while (break_pressed()) {
    }
    return 1;
  }
  if ((inportb(0xBFFE) & 0x01) == 0) {
    while ((inportb(0xBFFE) & 0x01) == 0) {
    }
    return 1;
  }
  if (x_pressed()) {
    while (x_pressed()) {
    }
    return 1;
  }
  return 0;
}

/* -------------------------------------------------------------------------- */
/* Low-level I/O                                                              */
/* -------------------------------------------------------------------------- */



static unsigned char fdc_msr(void) { return inportb(FDC_MSR_PORT); }

/* Wait until RQM set and DIO matches desired direction.
   want_dio = 0 for CPU->FDC (write), 1 for FDC->CPU (read). */
static unsigned char fdc_wait_rqm(unsigned char want_dio,
                                  unsigned int timeout) {
  while (timeout--) {
    unsigned char msr = fdc_msr();
    if ((msr & MSR_RQM) && (((msr & MSR_DIO) != 0) == (want_dio != 0)))
      return 1;
  }
  return 0;
}

static unsigned char fdc_write(unsigned char b) {
  if (!fdc_wait_rqm(0, FDC_RQM_TIMEOUT)) return 0;
  outportb(FDC_DATA_PORT, b);
  delay_us_approx(FDC_CMD_BYTE_GAP_US);
  return 1;
}

static unsigned char fdc_read(unsigned char* out) {
  if (!fdc_wait_rqm(1, FDC_RQM_TIMEOUT)) return 0;
  *out = inportb(FDC_DATA_PORT);
  delay_us_approx(FDC_CMD_BYTE_GAP_US);
  return 1;
}

/*
 * Consume up to 4 pending uPD765 interrupts using Sense Interrupt Status.
 * This prevents an interrupt storm after motor-ready transitions.
 */
static void fdc_drain_interrupts(void) {
  unsigned char st0, pcn, i;

  for (i = 0; i < 4; i++) {
    if (!fdc_write(0x08)) break;
    if (!fdc_read(&st0)) break;
    if ((st0 & 0xC0) == 0x80) break;
    if (!fdc_read(&pcn)) break;
  }
}

/*
 * plus3_motor_on()
 * Turns the +3 floppy drive motor ON and waits for spin-up.
 *
 */
unsigned char plus3_motor_on(void) {
  set_motor_on();
  delay_ms(MOTOR_SPINUP_DELAY_MS);
  fdc_drain_interrupts(); /* clear ready-change interrupt(s) */
  return 0;
}

/*
 * plus3_motor_off()
 * Turns the +3 floppy drive motor OFF.
 * Only call this when no FDC command is in progress.
 */
void plus3_motor_off(void) {
  set_motor_off();
  delay_ms(MOTOR_OFF_SETTLE_MS);  /* let not-ready transition latch */
  fdc_drain_interrupts(); /* clear pending interrupt(s) */
  delay_ms(MOTOR_OFF_SETTLE_MS);  /* catch late edge on first motor-off */
  fdc_drain_interrupts();
}

/* -------------------------------------------------------------------------- */
/* uPD765A command helpers                                                    */
/* -------------------------------------------------------------------------- */

static unsigned char cmd_sense_interrupt(unsigned char* st0,
                                         unsigned char* pcn) {
  if (!fdc_write(0x08)) return 0; /* Sense Interrupt Status */
  if (!fdc_read(st0)) return 0;
  if ((*st0 & 0xC0) == 0x80) {
    *pcn = 0;
    return 1;
  }
  if (!fdc_read(pcn)) return 0;
  return 1;
}

static unsigned char cmd_sense_drive_status(unsigned char drive,
                                            unsigned char head,
                                            unsigned char* st3) {
  if (!fdc_write(0x04)) return 0; /* Sense Drive Status */
  if (!fdc_write((head << 2) | (drive & 0x03))) return 0;
  if (!fdc_read(st3)) return 0;
  return 1;
}

static unsigned char wait_drive_ready(unsigned char drive,
                                      unsigned char head,
                                      unsigned char* out_st3) {
  unsigned int waited;
  unsigned char st3 = 0;

  for (waited = 0; waited < DRIVE_READY_TIMEOUT_MS; waited += DRIVE_READY_POLL_MS) {
    if (cmd_sense_drive_status(drive, head, &st3) && (st3 & 0x20)) {
      if (out_st3) *out_st3 = st3;
      return 1;
    }
    delay_ms(DRIVE_READY_POLL_MS);
  }

  if (out_st3) *out_st3 = st3;
  return 0;
}

static unsigned char cmd_recalibrate(unsigned char drive) {
  if (!fdc_write(0x07)) return 0; /* Recalibrate */
  if (!fdc_write(drive & 0x03)) return 0;
  delay_ms(SEEK_PREP_DELAY_MS);
  return 1;
}

static unsigned char cmd_seek(unsigned char drive, unsigned char head,
                              unsigned char cyl) {
  if (!fdc_write(0x0F)) return 0; /* Seek */
  if (!fdc_write((head << 2) | (drive & 0x03))) return 0;
  if (!fdc_write(cyl)) return 0;
  delay_ms(SEEK_PREP_DELAY_MS);
  return 1;
}

static unsigned char cmd_read_id(unsigned char drive, unsigned char head,
                                 unsigned char* st0, unsigned char* st1,
                                 unsigned char* st2, unsigned char* c,
                                 unsigned char* h, unsigned char* r,
                                 unsigned char* n) {
  /* Read ID, MFM mode (bit 6 set). 0x0A = FM mode; +3 disks are MFM only. */
  if (!fdc_write(0x4A)) return 0;
  if (!fdc_write((head << 2) | (drive & 0x03))) return 0;

  /* Result phase: 7 bytes */
  if (!fdc_read(st0)) return 0;
  if (!fdc_read(st1)) return 0;
  if (!fdc_read(st2)) return 0;
  if (!fdc_read(c)) return 0;
  if (!fdc_read(h)) return 0;
  if (!fdc_read(r)) return 0;
  if (!fdc_read(n)) return 0;

  /* Simple "ok" heuristic */
  return (unsigned char)(((*st0 & 0xC0) == 0) && (*st1 == 0) && (*st2 == 0));
}

static unsigned int sector_size_from_n(unsigned char n) {
  if (n > 3) return 0;
  return (unsigned int)(128u << n);
}

static unsigned char cmd_read_data(unsigned char drive, unsigned char head,
                                   unsigned char c, unsigned char h,
                                   unsigned char r, unsigned char n,
                                   unsigned char* out_st0,
                                   unsigned char* out_st1,
                                   unsigned char* out_st2,
                                   unsigned char* out_c,
                                   unsigned char* out_h,
                                   unsigned char* out_r,
                                   unsigned char* out_n,
                                   unsigned char* data,
                                   unsigned int data_len) {
  unsigned int i;

  if (!fdc_write(0x46)) return 0; /* Read Data, MFM */
  if (!fdc_write((head << 2) | (drive & 0x03))) return 0;
  if (!fdc_write(c)) return 0;
  if (!fdc_write(h)) return 0;
  if (!fdc_write(r)) return 0;
  if (!fdc_write(n)) return 0;
  if (!fdc_write(r)) return 0;    /* EOT: read only this sector */
  if (!fdc_write(0x2A)) return 0; /* GPL */
  if (!fdc_write(0xFF)) return 0; /* DTL (unused for n != 0) */

  for (i = 0; i < data_len; i++) {
    if (!fdc_read(&data[i])) return 0;
  }

  if (!fdc_read(out_st0)) return 0;
  if (!fdc_read(out_st1)) return 0;
  if (!fdc_read(out_st2)) return 0;
  if (!fdc_read(out_c)) return 0;
  if (!fdc_read(out_h)) return 0;
  if (!fdc_read(out_r)) return 0;
  if (!fdc_read(out_n)) return 0;

  return (unsigned char)(((*out_st0 & 0xC0) == 0) && (*out_st1 == 0) &&
                         (*out_st2 == 0));
}

/*
 * wait_seek_complete()
 *
 * Waits for a seek or recalibrate to finish, then issues one Sense Interrupt
 * Status to collect ST0 and PCN.
 *
 * On a real uPD765A, seek and recalibrate are background operations.  The FDC
 * sets the drive-busy bit in the MSR (bit 0 for drive 0) and immediately
 * returns to idle.  If Sense Interrupt Status is issued before the seek
 * finishes, the FDC has no interrupt to report and returns ST0=0x80 (invalid
 * command) as a SINGLE byte with no PCN.  The second fdc_read() then times
 * out, cmd_sense_interrupt() returns 0, and the caller sees a failure.
 * Emulators tolerate this; real hardware does not.
 *
 * The fix is to poll MSR bit 0 (D0B, drive 0 busy) until it clears, which
 * signals that the seek has completed and a pending interrupt is waiting.
 * Only then is Sense Interrupt Status issued.
 */
static unsigned char wait_seek_complete(unsigned char drive,
                                        unsigned char* out_st0,
                                        unsigned char* out_pcn) {
  unsigned char st0, pcn;
  unsigned int wait_ms;
  unsigned char tries;
  unsigned char busy_bit = (unsigned char)(1u << (drive & 0x03));

  /*
   * Wait for drive-busy bit to clear in MSR.
   * Some emulator runs hold the busy bit longer than expected.
   */
  for (wait_ms = 0; wait_ms < SEEK_BUSY_TIMEOUT_MS; wait_ms++) {
    if (!(fdc_msr() & busy_bit)) break;
    delay_ms(1);
  }
  dbg_seek_wait_loops = wait_ms;

  /*
   * Try Sense Interrupt a few times: this works on real hardware and avoids
   * long apparent hangs if emulator timing is jittery.
   */
  for (tries = 0; tries < SEEK_SENSE_RETRIES; tries++) {
    dbg_seek_sense_tries = tries;
    if (cmd_sense_interrupt(&st0, &pcn) && (st0 & 0x20)) {
      dbg_seek_last_st0 = st0;
      *out_st0 = st0;
      *out_pcn = pcn;
      return 1;
    }
    delay_ms(SEEK_SENSE_RETRY_DELAY_MS);
  }

  dbg_seek_last_st0 = st0;

  return 0;
}

void press_any_key(int interactive) {
  if (interactive == 1) {
    printf("\nPRESS ENTER\n");
    fflush(stdout);
    read_enter_blocking();
  }
}

/* -------------------------------------------------------------------------- */
/* Tests                                                                      */
/* -------------------------------------------------------------------------- */

static void test_motor_and_drive_status(int interactive) {
  unsigned char st3 = 0;
  unsigned char have_st3;
  char line1[32];
  char line2[32];
  char line3[32];
  char line4[32];
  char line5[32];
  char line6[32];
  const char* lines[6];

  last_test_failed = 0;
  plus3_motor_on();

  have_st3 = cmd_sense_drive_status(FDC_DRIVE, 0, &st3);
  if (!have_st3) {
    strcpy(line1, "ST3   : TIMEOUT");
    strcpy(line2, "READY : NO");
    strcpy(line3, "WPROT : NO");
    strcpy(line4, "TRACK0: NO");
    strcpy(line5, "FAULT : NO");
    results.sense_drive_pass = 0;
  } else {
    sprintf(line1, "ST3   : 0x%02X", st3);
    sprintf(line2, "READY : %s", yes_no(st3 & 0x20));
    sprintf(line3, "WPROT : %s", yes_no(st3 & 0x40));
    sprintf(line4, "TRACK0: %s", yes_no(st3 & 0x10));
    sprintf(line5, "FAULT : %s", yes_no(st3 & 0x80));
    results.sense_drive_pass = 1;
  }

  plus3_motor_off();
  results.motor_test_pass = 1;
  last_test_failed = (unsigned char)(results.sense_drive_pass == 0);
  strcpy(line6, "MOTOR : OFF");
  lines[0] = line6;
  lines[1] = line1;
  lines[2] = line2;
  lines[3] = line3;
  lines[4] = line4;
  lines[5] = line5;
  ui_render_text_screen("MOTOR AND DRIVE STATUS",
                        interactive ? "KEYS  : ENTER MENU" : "KEYS  : AUTO RETURN MENU",
                        lines,
                        6,
                        results.sense_drive_pass ? "RESULT: PASS" : "RESULT: FAIL");
  press_any_key(interactive);
}

static void test_read_id_probe(void) {
  unsigned char st3 = 0;
  unsigned char st0 = 0, pcn = 0;
  unsigned char rid_ok = 0;
  unsigned char st1 = 0, st2 = 0, c = 0, h = 0, r = 0, n = 0;
  unsigned char have_st3 = 0;
  char line1[32];
  char line2[32];
  char line3[32];
  char line4[32];
  char line5[32];
  char line6[32];
  const char* lines[6];

  last_test_failed = 0;
  plus3_motor_on();

  /* 1) Raw drive lines (ST3), informational */
  have_st3 = cmd_sense_drive_status(FDC_DRIVE, 0, &st3);
  if (!have_st3) {
    strcpy(line1, "ST3   : TIMEOUT");
  } else {
    sprintf(line1, "ST3   : 0x%02X", st3);
  }
  sprintf(line2, "READY : %s", yes_no(have_st3 && (st3 & 0x20)));
  sprintf(line3, "WPROT : %s", yes_no(have_st3 && (st3 & 0x40)));
  sprintf(line4, "TRACK0: %s", yes_no(have_st3 && (st3 & 0x10)));
  sprintf(line5, "FAULT : %s", yes_no(have_st3 && (st3 & 0x80)));

  /* 2) A command that tends to surface "not ready" in ST0 (and steps hardware)
   */
  if (cmd_recalibrate(FDC_DRIVE) && wait_seek_complete(FDC_DRIVE, &st0, &pcn)) {
    sprintf(line6, "RECAL : ST0=%02X PCN=%u", st0, pcn);
  } else {
    strcpy(line6, "RECAL : TIMEOUT");
  }

  /* 3) Media probe: Read ID, best indicator for both hardware and emulation */
  rid_ok = cmd_read_id(FDC_DRIVE, 0, &st0, &st1, &st2, &c, &h, &r, &n);

  results.sense_drive_pass = rid_ok ? 1 : 0;
  last_test_failed = (unsigned char)(rid_ok == 0);
  plus3_motor_off();
  lines[0] = line1;
  lines[1] = line2;
  lines[2] = line3;
  lines[3] = line4;
  lines[4] = line5;
  lines[5] = line6;
  if (rid_ok) {
    sprintf(line6, "ID    : C%u H%u R%u N%u", c, h, r, n);
  } else {
    sprintf(line6, "ID    : %s", read_id_failure_reason(st1, st2));
  }
  ui_render_text_screen("DRIVE READ ID PROBE",
                        "KEYS  : AUTO RETURN MENU",
                        lines,
                        6,
                        results.sense_drive_pass ? "RESULT: PASS" : "RESULT: FAIL");
}

static void test_recal_seek_track2(int interactive) {
  unsigned char st0 = 0, pcn = 0;
  unsigned char st3 = 0;
  unsigned char target = 2;
  unsigned char recal_ok = 0;
  unsigned char seek_ok = 0;
  char line1[32];
  char line2[32];
  char line3[32];
  char line4[32];
  const char* lines[4];
  (void)interactive;

  last_test_failed = 0;

  plus3_motor_on();

  if (!wait_drive_ready(FDC_DRIVE, 0, &st3)) {
    sprintf(line1, "READY : FAIL ST3=%02X", st3);
    strcpy(line2, "RECAL : SKIPPED");
    strcpy(line3, "SEEK  : SKIPPED");
    lines[0] = line1;
    lines[1] = line2;
    lines[2] = line3;
    lines[3] = "INFO  : CHECK MEDIA";
    results.recalibrate_pass = 0;
    results.seek_pass = 0;
    plus3_motor_off();
    last_test_failed = 1;
    ui_render_text_screen("RECALIBRATE AND SEEK TRACK 2",
                          "KEYS  : AUTO RETURN MENU",
                          lines,
                          4,
                          "RESULT: FAIL");
    return;
  }

  if (!cmd_recalibrate(FDC_DRIVE) || !wait_seek_complete(FDC_DRIVE, &st0, &pcn)) {
    strcpy(line1, "READY : YES");
    strcpy(line2, "RECAL : FAIL");
    strcpy(line3, "SEEK  : SKIPPED");
    sprintf(line4, "INFO  : ST0=%02X PCN=%u", st0, pcn);
    lines[0] = line1;
    lines[1] = line2;
    lines[2] = line3;
    lines[3] = line4;
    results.recalibrate_pass = 0;
    results.seek_pass = 0;
    plus3_motor_off();
    last_test_failed = 1;
    ui_render_text_screen("RECALIBRATE AND SEEK TRACK 2",
                          "KEYS  : AUTO RETURN MENU",
                          lines,
                          4,
                          "RESULT: FAIL");
    return;
  }
  recal_ok = (unsigned char)(pcn == 0);

  if (!cmd_seek(FDC_DRIVE, 0, target) || !wait_seek_complete(FDC_DRIVE, &st0, &pcn)) {
    strcpy(line1, "READY : YES");
    sprintf(line2, "RECAL : %s", pass_fail(recal_ok));
    strcpy(line3, "SEEK  : FAIL");
    sprintf(line4, "INFO  : ST0=%02X PCN=%u", st0, pcn);
    lines[0] = line1;
    lines[1] = line2;
    lines[2] = line3;
    lines[3] = line4;
    results.recalibrate_pass = recal_ok;
    results.seek_pass = 0;
    plus3_motor_off();
    last_test_failed = 1;
    ui_render_text_screen("RECALIBRATE AND SEEK TRACK 2",
                          "KEYS  : AUTO RETURN MENU",
                          lines,
                          4,
                          "RESULT: FAIL");
    return;
  }
  seek_ok = (unsigned char)(pcn == target);

  results.recalibrate_pass = recal_ok;
  results.seek_pass = seek_ok;
  plus3_motor_off();
  last_test_failed = (unsigned char)(!(results.recalibrate_pass && results.seek_pass));
  strcpy(line1, "READY : YES");
  sprintf(line2, "RECAL : %s", pass_fail(results.recalibrate_pass));
  sprintf(line3, "SEEK  : %s", pass_fail(results.seek_pass));
  sprintf(line4, "TRACK : %u", (unsigned int)pcn);
  lines[0] = line1;
  lines[1] = line2;
  lines[2] = line3;
  lines[3] = line4;
  ui_render_text_screen("RECALIBRATE AND SEEK TRACK 2",
                        "KEYS  : AUTO RETURN MENU",
                        lines,
                        4,
                        (results.recalibrate_pass && results.seek_pass) ? "RESULT: PASS" : "RESULT: FAIL");
}

static void test_seek_interactive(void) {
  unsigned char st0 = 0, pcn = 0;
  unsigned char st3 = 0;
  unsigned char target = 0;
  char line1[32];
  char line2[32];
  char line3[32];
  const char* lines[3];

  last_test_failed = 0;

  plus3_motor_on();

  if (!wait_drive_ready(FDC_DRIVE, 0, &st3)) {
    sprintf(line1, "READY : FAIL ST3=%02X", st3);
    strcpy(line2, "TRACK : 0");
    strcpy(line3, "LAST  : NO SEEK");
    plus3_motor_off();
    last_test_failed = 1;
    lines[0] = line1;
    lines[1] = line2;
    lines[2] = line3;
    ui_render_text_screen("INTERACTIVE STEP SEEK",
                          "KEYS  : K UP  J DOWN  Q EXIT",
                          lines,
                          3,
                          "RESULT: FAIL");
    return;
  }

  for (;;) {
    sprintf(line1, "TRACK : %u", (unsigned int)target);
    sprintf(line2, "LAST  : ST0=%02X", st0);
    sprintf(line3, "PCN   : %u", (unsigned int)pcn);
    lines[0] = line1;
    lines[1] = line2;
    lines[2] = line3;
    ui_render_text_screen("INTERACTIVE STEP SEEK",
                          "KEYS  : K UP  J DOWN  Q EXIT",
                          lines,
                          3,
                          "RESULT: ACTIVE");
    {
      int ch = read_key_blocking();
    ch = toupper((unsigned char)ch);

    switch (ch) {
      case 'J':
        target = target > 0 ? target - 1 : 0;
        break;
      case 'K':
        target = target < 39 ? target + 1 : 39;
        break;
      case 'Q':
        plus3_motor_off();
        sprintf(line1, "TRACK : %u", (unsigned int)target);
        sprintf(line2, "LAST  : ST0=%02X", st0);
        sprintf(line3, "PCN   : %u", (unsigned int)pcn);
        lines[0] = line1;
        lines[1] = line2;
        lines[2] = line3;
        ui_render_text_screen("INTERACTIVE STEP SEEK",
                              "KEYS  : K UP  J DOWN  Q EXIT",
                              lines,
                              3,
                              "RESULT: STOPPED");
        return;
      default:
        break;
    }
    }

    if (!cmd_seek(FDC_DRIVE, 0, target)) {
      sprintf(line1, "TRACK : %u", (unsigned int)target);
      strcpy(line2, "LAST  : SEEK CMD FAIL");
      sprintf(line3, "PCN   : %u", (unsigned int)pcn);
      plus3_motor_off();
      last_test_failed = 1;
      lines[0] = line1;
      lines[1] = line2;
      lines[2] = line3;
      ui_render_text_screen("INTERACTIVE STEP SEEK",
                            "KEYS  : K UP  J DOWN  Q EXIT",
                            lines,
                            3,
                            "RESULT: FAIL");
      return;
    }

    if (!wait_seek_complete(FDC_DRIVE, &st0, &pcn)) {
      sprintf(line1, "TRACK : %u", (unsigned int)target);
      strcpy(line2, "LAST  : WAIT TIMEOUT");
      sprintf(line3, "PCN   : %u", (unsigned int)pcn);
      plus3_motor_off();
      last_test_failed = 1;
      lines[0] = line1;
      lines[1] = line2;
      lines[2] = line3;
      ui_render_text_screen("INTERACTIVE STEP SEEK",
                            "KEYS  : K UP  J DOWN  Q EXIT",
                            lines,
                            3,
                            "RESULT: FAIL");
      return;
    }
  }
}

static void test_read_id(int interactive) {
  unsigned char st0 = 0, st1 = 0, st2 = 0, c = 0, h = 0, r = 0, n = 0;
  unsigned char st3 = 0;
  unsigned char ok;
  char line1[32];
  char line2[32];
  char line3[32];
  char line4[32];
  const char* lines[4];
  (void)interactive;

  last_test_failed = 0;

  plus3_motor_on();

  if (!wait_drive_ready(FDC_DRIVE, 0, &st3)) {
    strcpy(line1, "MEDIA : READABLE DISK REQUIRED");
    sprintf(line2, "READY : ST3=%02X", st3);
    strcpy(line3, "CHRN  : INVALID");
    strcpy(line4, "REASON: DRIVE NOT READY");
    lines[0] = line1;
    lines[1] = line2;
    lines[2] = line3;
    lines[3] = line4;
    results.read_id_pass = 0;
    plus3_motor_off();
    last_test_failed = 1;
    ui_render_text_screen("READ ID ON TRACK 0",
                          "KEYS  : AUTO RETURN MENU",
                          lines,
                          4,
                          "RESULT: FAIL");
    return;
  }

  /* Try to get to track 0 first */
  cmd_recalibrate(FDC_DRIVE);
  {
    unsigned char t0, tp;
    wait_seek_complete(FDC_DRIVE, &t0, &tp);
  }

  ok = cmd_read_id(FDC_DRIVE, 0, &st0, &st1, &st2, &c, &h, &r, &n);

  strcpy(line1, "MEDIA : READABLE DISK REQUIRED");
  sprintf(line2, "STS   : %02X/%02X/%02X", st0, st1, st2);
  if (ok) {
    sprintf(line3, "CHRN  : %u/%u/%u/%u", c, h, r, n);
    strcpy(line4, "INFO  : ID READ OK");
  } else {
    strcpy(line3, "CHRN  : INVALID");
    sprintf(line4, "REASON: %s", read_id_failure_reason(st1, st2));
  }

  results.read_id_pass = ok;
  last_test_failed = (unsigned char)(ok == 0);
  plus3_motor_off();
  lines[0] = line1;
  lines[1] = line2;
  lines[2] = line3;
  lines[3] = line4;
  ui_render_text_screen("READ ID ON TRACK 0",
                        "KEYS  : AUTO RETURN MENU",
                        lines,
                        4,
                        ok ? "RESULT: PASS" : "RESULT: FAIL");
}

static void render_track_loop_fail(unsigned char track,
                                   unsigned int pass_count,
                                   unsigned int fail_count,
                                   const char* last1,
                                   const char* last2) {
  ui_render_track_loop_screen(track,
                              pass_count,
                              fail_count,
                              last1,
                              last2,
                              "RESULT: FAIL");
}

static void render_rpm_loop_fail(unsigned int rpm,
                                 unsigned int pass_count,
                                 unsigned int fail_count,
                                 const char* last1,
                                 const char* last2) {
  ui_render_rpm_loop_screen(rpm,
                            rpm != 0,
                            pass_count,
                            fail_count,
                            last1,
                            last2,
                            "RESULT: FAIL");
}

static void test_read_track_data_loop(void) {
  static unsigned char sector_data[1024];
  unsigned char st0 = 0, st1 = 0, st2 = 0;
  unsigned char c = 0, h = 0, r = 0, n = 0;
  unsigned char rd0 = 0, rd1 = 0, rd2 = 0;
  unsigned char rc = 0, rh = 0, rr = 0, rn = 0;
  unsigned char track = 0;
  unsigned char need_seek = 1;
  unsigned char jk_latch = 0;
  unsigned char st3 = 0;
  unsigned int pass_count = 0;
  unsigned int fail_count = 0;
  unsigned int data_len;
  unsigned char pause_step;
  unsigned char exit_now;
  unsigned char need_ui_redraw = 1;
  char ui_line1[40];
  char ui_line2[40];

  last_test_failed = 0;

  plus3_motor_on();

  for (;;) {
    if (loop_exit_requested()) break;
    exit_now = 0;

#if HEADLESS_ROM_FONT
    if (pass_count + fail_count >= 3U) break;
#endif

    if (!jk_latch && j_pressed()) {
      if (track > 0) track--;
      need_seek = 1;
      jk_latch = 1;
      need_ui_redraw = 1;
    } else if (!jk_latch && k_pressed()) {
      if (track < 79) track++;
      need_seek = 1;
      jk_latch = 1;
      need_ui_redraw = 1;
    } else if (!j_pressed() && !k_pressed()) {
      jk_latch = 0;
    }

    if (need_ui_redraw) {
      ui_render_track_loop_screen(track,
                                  pass_count,
                                  fail_count,
                                  "LAST  : OK",
                                  "INFO  : LOOPING",
                                  "RESULT: ACTIVE");
      need_ui_redraw = 0;
    }

    if (!wait_drive_ready(FDC_DRIVE, 0, &st3)) {
      fail_count++;
      sprintf(ui_line1, "LAST  : DRIVE NR ST3=%02X", st3);
      render_track_loop_fail(track, pass_count, fail_count, ui_line1, "INFO  : RETRYING");
      need_seek = 1;
      need_ui_redraw = 0;
      delay_ms(20);
      continue;
    }

    if (need_seek) {
      if (!cmd_seek(FDC_DRIVE, 0, track) ||
          !wait_seek_complete(FDC_DRIVE, &st0, &c)) {
        fail_count++;
        sprintf(ui_line1, "LAST  : SEEK FAIL T=%u", track);
        sprintf(ui_line2, "INFO  : ST0=%02X", st0);
        render_track_loop_fail(track, pass_count, fail_count, ui_line1, ui_line2);
        need_seek = 1;
        need_ui_redraw = 0;
        delay_ms(20);
        continue;
      }
      need_seek = 0;
      need_ui_redraw = 1;
    }

    if (!cmd_read_id(FDC_DRIVE, 0, &st0, &st1, &st2, &c, &h, &r, &n)) {
      fail_count++;
      sprintf(ui_line1, "LAST  : RID FAIL T=%u", track);
      sprintf(ui_line2, "INFO  : %s", read_id_failure_reason(st1, st2));
      render_track_loop_fail(track, pass_count, fail_count, ui_line1, ui_line2);
      need_seek = 1;
      need_ui_redraw = 0;
      delay_ms(20);
      continue;
    }

    data_len = sector_size_from_n(n);
    if (data_len == 0 || data_len > sizeof(sector_data)) {
      fail_count++;
      sprintf(ui_line1, "LAST  : RID N=%u BAD", n);
      render_track_loop_fail(track,
                             pass_count,
                             fail_count,
                             ui_line1,
                             "INFO  : INVALID SECTOR SIZE");
      need_seek = 1;
      need_ui_redraw = 0;
      delay_ms(20);
      continue;
    }

    if (!cmd_read_data(FDC_DRIVE, h, c, h, r, n, &rd0, &rd1, &rd2, &rc, &rh,
                       &rr, &rn, sector_data, data_len)) {
      fail_count++;
      sprintf(ui_line1, "LAST  : READ FAIL T=%u", track);
      sprintf(ui_line2, "INFO  : %s", read_id_failure_reason(rd1, rd2));
      render_track_loop_fail(track, pass_count, fail_count, ui_line1, ui_line2);
      need_seek = 1;
      need_ui_redraw = 0;
      delay_ms(20);
      continue;
    }

    pass_count++;

    /* Short pacing gives keyboard scans time without stalling diagnostics. */
    for (pause_step = 0; pause_step < READ_LOOP_PAUSE_STEPS; pause_step++) {
      if (loop_exit_requested()) {
        exit_now = 1;
        break;
      }
      delay_ms(READ_LOOP_PAUSE_MS);
    }
    if (exit_now) break;
  }

  plus3_motor_off();
  ui_render_track_loop_screen(track,
                              pass_count,
                              fail_count,
                              "LAST  : STOPPED",
                              "INFO  : USER EXIT",
                              "RESULT: STOPPED");
  last_test_failed = (unsigned char)(fail_count > 0);
}

static void test_rpm_checker(void) {
  unsigned char st0 = 0, st1 = 0, st2 = 0;
  unsigned char c = 0, h = 0, r = 0, n = 0;
  unsigned char first_r = 0;
  unsigned short start_tick = 0;
  unsigned short end_tick = 0;
  unsigned short dticks = 0;
  unsigned int period_ms = 0;
  unsigned int rpm = 0;
  unsigned int pass_count = 0;
  unsigned int fail_count = 0;
  unsigned char seen_other = 0;
  unsigned char st3 = 0;
  unsigned char i;
  unsigned char exit_now = 0;

  last_test_failed = 0;

  plus3_motor_on();

  if (!wait_drive_ready(FDC_DRIVE, 0, &st3)) {
    ui_render_rpm_loop_screen(0,
                              0,
                              pass_count,
                              fail_count,
                              "LAST  : DRIVE NOT READY",
                              "INFO  : CHECK MEDIA",
                              "RESULT: FAIL");
    plus3_motor_off();
    return;
  }

  for (;;) {
    if (loop_exit_requested()) break;

#if HEADLESS_ROM_FONT
    if (pass_count + fail_count >= 3U) break;
#endif

    if (!cmd_seek(FDC_DRIVE, 0, 0) || !wait_seek_complete(FDC_DRIVE, &st0, &c)) {
      fail_count++;
      render_rpm_loop_fail(rpm,
                           pass_count,
                           fail_count,
                           "LAST  : SEEK TRACK0 FAIL",
                           "INFO  : ST0 SET");
      delay_ms(RPM_FAIL_DELAY_MS);
      continue;
    }

    if (!cmd_read_id(FDC_DRIVE, 0, &st0, &st1, &st2, &c, &h, &r, &n)) {
      fail_count++;
      render_rpm_loop_fail(
          rpm, pass_count, fail_count, "LAST  : ID FAIL", read_id_failure_reason(st1, st2));
      delay_ms(RPM_FAIL_DELAY_MS);
      continue;
    }

    first_r = r;
    start_tick = frame_ticks();
    seen_other = 0;
    dticks = 0;
    exit_now = 0;

    for (i = 0; i < 120; i++) {
      if (loop_exit_requested()) {
        exit_now = 1;
        break;
      }
      if (!cmd_read_id(FDC_DRIVE, 0, &st0, &st1, &st2, &c, &h, &r, &n)) {
        break;
      }
      if (r != first_r) {
        seen_other = 1;
      } else if (seen_other) {
        end_tick = frame_ticks();
        dticks = (unsigned short)(end_tick - start_tick);
        break;
      }
      delay_ms(2);
      if ((unsigned short)(frame_ticks() - start_tick) > 50U) {
        break;
      }
    }

    if (exit_now) {
      break;
    }

    if (dticks == 0) {
      fail_count++;
      render_rpm_loop_fail(
          rpm, pass_count, fail_count, "LAST  : RPM N/A", seen_other ? "NO REV MARK" : "SAME SEC");
      delay_ms(RPM_FAIL_DELAY_MS);
      continue;
    }

    period_ms = (unsigned int)dticks * 20U;
    if (period_ms == 0) {
      fail_count++;
      render_rpm_loop_fail(rpm,
                           pass_count,
                           fail_count,
                           "LAST  : PERIOD BAD",
                           "INFO  : ZERO DELTA");
      delay_ms(RPM_FAIL_DELAY_MS);
      continue;
    }

    rpm = (unsigned int)((60000U + (period_ms / 2U)) / period_ms);
    pass_count++;
    ui_render_rpm_loop_screen(rpm,
                              1,
                              pass_count,
                              fail_count,
                              "LAST  : SAMPLE OK",
                              "INFO  : PERIOD READY",
                              (rpm >= 285U && rpm <= 315U) ? "RESULT: PASS" : "RESULT: OUT-OF-RANGE");
    delay_ms(RPM_LOOP_DELAY_MS);
  }

  plus3_motor_off();
  ui_render_rpm_loop_screen(rpm,
                            rpm != 0,
                            pass_count,
                            fail_count,
                            "LAST  : STOPPED",
                            "INFO  : USER EXIT",
                            "RESULT: STOPPED");
  last_test_failed = (unsigned char)(fail_count > 0);
}

/* -------------------------------------------------------------------------- */
/* UI                                                                         */
/* -------------------------------------------------------------------------- */

static void print_results(void) {
  ui_render_report_card();
}

static void run_all_tests(unsigned char human_mode) {
  memset(&results, 0, sizeof(results));
  reset_report_progress();
  last_test_failed = 0;

  set_report_status(REPORT_STATUS_READY);
  ui_render_report_card();
  if (human_mode) delay_ms(500);

  set_report_status(REPORT_STATUS_RUNNING);
  if (human_mode) delay_ms(250);
  test_motor_and_drive_status(0);
  set_report_status(REPORT_STATUS_COMPLETE);
  ui_render_report_card();
  if (human_mode) delay_ms(1200);

  set_report_status(REPORT_STATUS_RUNNING);
  if (human_mode) delay_ms(250);
  test_read_id_probe();
  set_report_status(REPORT_STATUS_COMPLETE);
  ui_render_report_card();
  if (human_mode) delay_ms(1200);

  set_report_status(REPORT_STATUS_RUNNING);
  if (human_mode) delay_ms(250);
  test_recal_seek_track2(0);
  set_report_status(REPORT_STATUS_COMPLETE);
  ui_render_report_card();
  if (human_mode) delay_ms(1200);

  set_report_status(REPORT_STATUS_RUNNING);
  if (human_mode) delay_ms(250);
  test_read_id(0);
  set_report_status(REPORT_STATUS_COMPLETE);
  print_results();
  if (human_mode) delay_ms(1200);
  last_test_failed = (unsigned char)(pass_count() < 5U);
}

static void menu_print_with_selection(unsigned char selected_index) {
  unsigned char total = pass_count();
  ui_render_main_menu(selected_index, total);
}

int main(void) {
  int ch;
  unsigned char selected_menu = 0;
  unsigned char menu_dirty = 1;

    /* Fix DivMMC font corruption and apply readable UI font. */
    init_ui_font();

#if DEBUG_ENABLED
  /* Capture paging state at startup for debug output. */
  startup_bank678 = *(volatile unsigned char *)0x5B67;
#endif

  /* Initialize motor and paging to safe defaults. */
  set_motor_off();

  memset(&results, 0, sizeof(results));
  reset_report_progress();
  disable_terminal_auto_pause();
#if DEBUG_ENABLED
  printf("DEBUG BUILD\n");
  printf("DBG startup BANK678=0x%02X\n", startup_bank678);
#endif

  for (;;) {
    if (menu_dirty) {
      menu_print_with_selection(selected_menu);
      menu_dirty = 0;
    }

    ch = read_menu_key_blocking();

    if (ch == MENU_KEY_UP) {
      if (selected_menu > 0) {
        unsigned char old_selected = selected_menu;
        selected_menu--;
        ui_update_main_menu_selection(old_selected, selected_menu);
      }
      continue;
    }

    if (ch == MENU_KEY_DOWN) {
      if (selected_menu + 1 < menu_item_count()) {
        unsigned char old_selected = selected_menu;
        selected_menu++;
        ui_update_main_menu_selection(old_selected, selected_menu);
      }
      continue;
    }

    if (ch == '\n') {
      ch = menu_key_for_index(selected_menu);
    }

    ch = toupper((unsigned char)ch);
    {
      unsigned char found = 0;
      unsigned char key_index = menu_index_for_key((char)ch, &found);
      if (found) {
        selected_menu = key_index;
      }
    }

    switch (ch) {
      case 'M':
        test_motor_and_drive_status(0);
        wait_for_menu_after_failure();
        menu_dirty = 1;
        break;
      case 'P':
        test_read_id_probe();
        wait_for_menu_after_failure();
        menu_dirty = 1;
        break;
      case 'K':
        test_recal_seek_track2(0);
        wait_for_menu_after_failure();
        menu_dirty = 1;
        break;
      case 'I':
        test_seek_interactive();
        wait_for_menu_after_failure();
        menu_dirty = 1;
        break;
      case 'T':
        test_read_id(0);
        wait_for_menu_after_failure();
        menu_dirty = 1;
        break;
      case 'D':
        test_read_track_data_loop();
        wait_for_menu_after_failure();
        menu_dirty = 1;
        break;
      case 'H':
        test_rpm_checker();
        wait_for_menu_after_failure();
        menu_dirty = 1;
        break;
      case 'A':
        run_all_tests(1);
        wait_for_menu_after_failure();
        menu_dirty = 1;
        break;
      case 'R':
        print_results();
        press_any_key(1);
        menu_dirty = 1;
        break;
      case 'C':
        memset(&results, 0, sizeof(results));
        reset_report_progress();
        printf("RESULTS CLEARED\n");
        press_any_key(1);
        menu_dirty = 1;
        break;
      case 'Q':
        plus3_motor_off();
        printf("Exiting...\n");
        return 0;
      default:
        /* Ignore unknown keys in menu to avoid display jitter. */
        break;
    }
  }
  return 0;
}