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
static const unsigned char debug_enabled = DEBUG_ENABLED;
/*
 * Captured at startup before any paging changes, for debug visibility.
 * Shows the state DivMMC left the system in.
 */
static unsigned char startup_bank678;

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

#if COMPACT_UI
typedef struct {
  unsigned char ch;
  unsigned char rows[7];
} CompactGlyph;

static void set_compact_glyph(unsigned char ch, const unsigned char rows[7]) {
  unsigned char *glyph = &font_ram[((unsigned short)ch) * 8U];
  unsigned char r;

  for (r = 0; r < 7; r++) {
    glyph[r] = (unsigned char)(rows[r] << 2);
  }
  glyph[7] = 0x00;
}

static void copy_font_glyph(unsigned char dst, unsigned char src) {
  memcpy(&font_ram[((unsigned short)dst) * 8U],
         &font_ram[((unsigned short)src) * 8U],
         8U);
}

static void apply_compact_font(void) {
  unsigned char ch;
  unsigned int i;
  static const CompactGlyph glyphs[] = {
      {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
      {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
      {'"', {0x0A, 0x0A, 0x0A, 0x00, 0x00, 0x00, 0x00}},
      {'#', {0x0A, 0x1F, 0x0A, 0x0A, 0x1F, 0x0A, 0x00}},
      {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
      {',', {0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x08}},
      {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
      {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04}},
      {'/', {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10}},
      {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
      {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
      {'2', {0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F}},
      {'3', {0x1F, 0x01, 0x02, 0x06, 0x01, 0x11, 0x0E}},
      {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
      {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
      {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
      {'7', {0x1F, 0x01, 0x02, 0x02, 0x04, 0x04, 0x04}},
      {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
      {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
      {':', {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}},
      {';', {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x08}},
      {'=', {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}},
      {'?', {0x0E, 0x11, 0x01, 0x06, 0x04, 0x00, 0x04}},
      {'[', {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}},
      {'\\', {0x10, 0x08, 0x08, 0x04, 0x02, 0x02, 0x01}},
      {']', {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}},
      {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}},
      {'|', {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
      {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
      {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
      {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
      {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
      {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
      {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
      {'G', {0x0E, 0x11, 0x10, 0x10, 0x13, 0x11, 0x0E}},
      {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
      {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
      {'J', {0x1F, 0x01, 0x01, 0x01, 0x01, 0x11, 0x0E}},
      {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
      {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
      {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
      {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
      {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
      {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
      {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
      {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
      {'S', {0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E}},
      {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
      {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
      {'V', {0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04}},
      {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
      {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
      {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
      {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
  };

  memset(font_ram, 0x00, sizeof(font_ram));

  for (i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); i++) {
    set_compact_glyph(glyphs[i].ch, glyphs[i].rows);
  }

  /* Make undefined printable glyphs readable in compact mode. */
  for (ch = 32; ch <= 126; ch++) {
    unsigned char *glyph = &font_ram[((unsigned short)ch) * 8U];
    if ((glyph[0] | glyph[1] | glyph[2] | glyph[3] |
         glyph[4] | glyph[5] | glyph[6] | glyph[7]) == 0) {
      copy_font_glyph(ch, '?');
    }
  }

  /* Lowercase maps to uppercase to keep wording readable with a compact set. */
  for (ch = 'a'; ch <= 'z'; ch++) {
    copy_font_glyph(ch, (unsigned char)(ch - 'a' + 'A'));
  }
}
#endif

static void init_ui_font(void) {
  /* Copy ROM font first so paging changes cannot corrupt rendered text. */
  memcpy(font_ram, (const void *)0x3C00, sizeof(font_ram));

  /*
   * Optional compact mode replaces glyphs with a custom compact font.
   * Default build keeps ROM-compatible glyphs for OCR stability.
   */
#if COMPACT_UI
  apply_compact_font();
#endif

  ioctl(1, IOCTL_OTERM_FONT, font_ram);
}

/* Test results storage */
typedef struct {
  unsigned char motor_test_pass;
  unsigned char sense_drive_pass;
  unsigned char recalibrate_pass;
  unsigned char seek_pass;
  unsigned char read_id_pass;
} TestResults;

static TestResults results;

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

static void ui_clear_screen(void) {
  /* Form feed clears the text area on Spectrum terminals. */
  putchar('\f');
  fflush(stdout);
}

static void ui_print_bar(const char* label, unsigned char ok) {
  unsigned char i;

  printf("%-6s ", label);
  putchar('[');
  for (i = 0; i < 8; i++) {
    putchar(ok ? '#' : '.');
  }
  putchar(']');
  printf(" %s\n", pass_fail(ok));
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

static unsigned short frame_ticks(void) {
  volatile unsigned char* frames = (volatile unsigned char*)0x5C78;
  return (unsigned short)(frames[0] | ((unsigned short)frames[1] << 8));
}

static unsigned char any_mapped_key_down(void) {
  unsigned int i;
  for (i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++) {
    if ((inportb(keymap[i].row_port) & keymap[i].bit_mask) == 0) return 1;
  }
  return 0;
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
        while (any_mapped_key_down()) {
        }
        return key;
      }
    }
  }
}

static unsigned char retry_or_exit(void) {
  int ch;

  printf("X OR BREAK=EXIT  ANY KEY=RETRY\n");
  fflush(stdout);
  ch = read_key_blocking();
  return (unsigned char)((ch == 'X') || (ch == 'x') || (ch == 27));
}

static unsigned char loop_exit_requested(void) {
  if (break_pressed()) {
    while (break_pressed()) {
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
    printf("\nPRESS ANY KEY\n");
    fflush(stdout);
    read_key_blocking();
  }
}

/* -------------------------------------------------------------------------- */
/* Tests                                                                      */
/* -------------------------------------------------------------------------- */

static void test_motor_and_drive_status(int interactive) {
  unsigned char st3 = 0;
  unsigned char have_st3;

  printf("\n== MOTOR AND STATUS ==\n");
  printf("MOTOR ON\n");
  plus3_motor_on();

  have_st3 = cmd_sense_drive_status(FDC_DRIVE, 0, &st3);
  if (!have_st3) {
    printf("ST3: TIMEOUT\n");
    results.sense_drive_pass = 0;
  } else {
    printf("ST3 = 0x%02X\n", st3);
    printf("READY : %s\n", yes_no(st3 & 0x20));
    printf("WPROT : %s\n", yes_no(st3 & 0x40));
    printf("TRACK0: %s\n", yes_no(st3 & 0x10));
    printf("FAULT : %s\n", yes_no(st3 & 0x80));
    results.sense_drive_pass = 1;
  }

  printf("MOTOR OFF\n");
  plus3_motor_off();
  results.motor_test_pass = 1;
  press_any_key(interactive);
}

static void test_read_id_probe(void) {
  unsigned char st3 = 0;
  unsigned char st0 = 0, pcn = 0;
  unsigned char rid_ok = 0;
  unsigned char st1 = 0, st2 = 0, c = 0, h = 0, r = 0, n = 0;
  unsigned char have_st3 = 0;

  printf("\n== DRIVE PROBE ==\n");
  plus3_motor_on();

  /* 1) Raw drive lines (ST3), informational */
  have_st3 = cmd_sense_drive_status(FDC_DRIVE, 0, &st3);
  if (!have_st3) {
    printf("ST3: TIMEOUT\n");
  } else {
    printf("ST3 = 0x%02X\n", st3);
    printf("READY : %s\n", yes_no(st3 & 0x20));
    printf("WPROT : %s\n", yes_no(st3 & 0x40));
    printf("TRACK0: %s\n", yes_no(st3 & 0x10));
    printf("FAULT : %s\n", yes_no(st3 & 0x80));
  }

  /* 2) A command that tends to surface "not ready" in ST0 (and steps hardware)
   */
  if (cmd_recalibrate(FDC_DRIVE) && wait_seek_complete(FDC_DRIVE, &st0, &pcn)) {
    printf("RECAL ST0=0x%02X\n", st0);
    printf("PCN=%u NR=%s\n", pcn, yes_no(st0 & 0x08));
  } else {
    printf("RECAL: TIMEOUT\n");
  }

  /* 3) Media probe: Read ID, best indicator for both hardware and emulation */
  rid_ok = cmd_read_id(FDC_DRIVE, 0, &st0, &st1, &st2, &c, &h, &r, &n);

  printf("READ ID\n");
  printf("ST0=0x%02X\n", st0);
  printf("ST1=0x%02X\n", st1);
  printf("ST2=0x%02X\n", st2);
  if (rid_ok) {
    printf("C=%u H=%u\n", c, h);
    printf("R=%u N=%u\n", r, n);
    printf("PROBE: PASS\n");
  } else {
    printf("PROBE: FAIL\n");
  }

  results.sense_drive_pass = rid_ok ? 1 : 0;
  printf("RESULT: %s\n", pass_fail(results.sense_drive_pass));

  plus3_motor_off();
}

static void test_recal_seek_track2(int interactive) {
  unsigned char st0 = 0, pcn = 0;
  unsigned char target = 2;
  unsigned char recal_ok = 0;
  unsigned char seek_ok = 0;
  unsigned char done = 0;

  while (!done) {
    printf("\n== RECAL + SEEK %u ==\n", target);

    plus3_motor_on();

    if (!cmd_recalibrate(FDC_DRIVE)) {
      printf("FAIL: recal cmd\n");
      results.recalibrate_pass = 0;
      results.seek_pass = 0;
      plus3_motor_off();
      if (interactive) {
        done = retry_or_exit();
        continue;
      }
      return;
    }

    if (!wait_seek_complete(FDC_DRIVE, &st0, &pcn)) {
      printf("FAIL: wait timeout\n");
      results.recalibrate_pass = 0;
      results.seek_pass = 0;
      plus3_motor_off();
      if (interactive) {
        done = retry_or_exit();
        continue;
      }
      return;
    }

    printf("SenseInt: ST0=0x%02X, PCN=%u\n", st0, pcn);
    recal_ok = (unsigned char)(pcn == 0);

    if (debug_enabled) {
      printf("DBG seek start MSR=0x%02X\n", fdc_msr());
    }

    if (!cmd_seek(FDC_DRIVE, 0, target)) {
      printf("FAIL: seek cmd\n");
      if (debug_enabled) {
        printf("DBG seek cmd MSR=0x%02X\n", fdc_msr());
      }
      results.recalibrate_pass = recal_ok;
      results.seek_pass = 0;
      plus3_motor_off();
      if (interactive) {
        done = retry_or_exit();
        continue;
      }
      return;
    }

    if (!wait_seek_complete(FDC_DRIVE, &st0, &pcn)) {
      printf("FAIL: wait timeout\n");
      if (debug_enabled) {
        printf("DBG wait loops=%u tries=%u st0=0x%02X msr=0x%02X\n",
               dbg_seek_wait_loops, dbg_seek_sense_tries, dbg_seek_last_st0,
               fdc_msr());
      }
      results.recalibrate_pass = recal_ok;
      results.seek_pass = 0;
      plus3_motor_off();
      if (interactive) {
        done = retry_or_exit();
        continue;
      }
      return;
    }

    printf("SenseInt: ST0=0x%02X, PCN=%u\n", st0, pcn);
    if (debug_enabled) {
      printf("DBG wait loops=%u tries=%u msr=0x%02X\n",
             dbg_seek_wait_loops, dbg_seek_sense_tries, fdc_msr());
    }
    seek_ok = (unsigned char)(pcn == target);

    results.recalibrate_pass = recal_ok;
    results.seek_pass = seek_ok;
    printf("Recal: %s\n", pass_fail(results.recalibrate_pass));
    printf("Seek : %s\n", pass_fail(results.seek_pass));

    plus3_motor_off();

    if (!interactive) {
      return;
    }

    done = retry_or_exit();
  }
}

static void test_seek_interactive(void) {
  unsigned char st0 = 0, pcn = 0;
  unsigned char target = 0;

  printf("\n== STEP SEEK ==\n");

  plus3_motor_on();

  for (;;) {
    printf("TRACK %u\n", target);
    printf("K=UP J=DOWN Q=QUIT: ");
    int ch = read_key_blocking();
    printf("\n");
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
        return;
      default:
        break;
    }

    if (!cmd_seek(FDC_DRIVE, 0, target)) {
      printf("FAIL: seek cmd\n");
      plus3_motor_off();
      return;
    }

    if (!wait_seek_complete(FDC_DRIVE, &st0, &pcn)) {
      printf("FAIL: wait timeout\n");
      plus3_motor_off();
      return;
    }

    printf("SenseInt: ST0=0x%02X, PCN=%u\n", st0, pcn);
  }
}

static void test_read_id(int interactive) {
  unsigned char st0 = 0, st1 = 0, st2 = 0, c = 0, h = 0, r = 0, n = 0;
  unsigned char ok;
  unsigned char done = 0;

  while (!done) {
    printf("\n== READ ID ==\n");
    printf("NEEDS READABLE DISK\n");

    plus3_motor_on();

    /* Try to get to track 0 first */
    cmd_recalibrate(FDC_DRIVE);
    {
      unsigned char t0, tp;
      wait_seek_complete(FDC_DRIVE, &t0, &tp);
    }

    ok = cmd_read_id(FDC_DRIVE, 0, &st0, &st1, &st2, &c, &h, &r, &n);

    printf("READ ID\n");
    printf("ST0=0x%02X\n", st0);
    printf("ST1=0x%02X\n", st1);
    printf("ST2=0x%02X\n", st2);
    if (ok) {
      printf("C=%u H=%u\n", c, h);
      printf("R=%u N=%u\n", r, n);
    } else {
      printf("CHRN: INVALID\n");
      printf("REASON: %s\n", read_id_failure_reason(st1, st2));
    }

    results.read_id_pass = ok;
    printf("RESULT: %s\n", pass_fail(ok));

    plus3_motor_off();

    if (!interactive) {
      return;
    }

    done = retry_or_exit();
  }
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
  unsigned int pass_count = 0;
  unsigned int fail_count = 0;
  unsigned int data_len;
  unsigned int i;
  unsigned int checksum;
  unsigned char pause_step;
  unsigned char exit_now;

  printf("\n== READ TRACK DATA LOOP ==\n");
  printf("J/K=TRACK  X/BREAK=EXIT\n");

  plus3_motor_on();

  for (;;) {
    if (loop_exit_requested()) break;
    exit_now = 0;

    if (!jk_latch && j_pressed()) {
      if (track > 0) track--;
      need_seek = 1;
      jk_latch = 1;
      printf("TRACK %u\n", track);
    } else if (!jk_latch && k_pressed()) {
      if (track < 79) track++;
      need_seek = 1;
      jk_latch = 1;
      printf("TRACK %u\n", track);
    } else if (!j_pressed() && !k_pressed()) {
      jk_latch = 0;
    }

    if (need_seek) {
      if (!cmd_seek(FDC_DRIVE, 0, track) ||
          !wait_seek_complete(FDC_DRIVE, &st0, &c)) {
        fail_count++;
        printf("FAIL #%u\n", fail_count);
        printf("SEEK T=%u\n", track);
        printf("ST0=0x%02X\n", st0);
        delay_ms(20);
        continue;
      }
      need_seek = 0;
    }

    if (!cmd_read_id(FDC_DRIVE, 0, &st0, &st1, &st2, &c, &h, &r, &n)) {
      fail_count++;
      printf("FAIL #%u\n", fail_count);
      printf("RID T=%u\n", track);
      printf("ST0=0x%02X\n", st0);
      printf("ST1=0x%02X\n", st1);
      printf("ST2=0x%02X\n", st2);
      printf("%s\n", read_id_failure_reason(st1, st2));
      delay_ms(20);
      continue;
    }

    data_len = sector_size_from_n(n);
    if (data_len == 0 || data_len > sizeof(sector_data)) {
      fail_count++;
      printf("FAIL #%u\n", fail_count);
      printf("RID N=%u BAD\n", n);
      delay_ms(20);
      continue;
    }

    if (!cmd_read_data(FDC_DRIVE, h, c, h, r, n, &rd0, &rd1, &rd2, &rc, &rh,
                       &rr, &rn, sector_data, data_len)) {
      fail_count++;
      printf("FAIL #%u\n", fail_count);
      printf("RD T=%u\n", track);
      printf("CHRN=%u/%u/%u/%u\n", c, h, r, n);
      printf("ST0=0x%02X\n", rd0);
      printf("ST1=0x%02X\n", rd1);
      printf("ST2=0x%02X\n", rd2);
      printf("%s\n", read_id_failure_reason(rd1, rd2));
      delay_ms(20);
      continue;
    }

    checksum = 0;
    for (i = 0; i < data_len; i++) checksum += sector_data[i];

    pass_count++;
    printf("PASS #%u\n", pass_count);
    printf("T=%u\n", track);
    printf("C=%u H=%u\n", c, h);
    printf("R=%u N=%u\n", r, n);
    printf("BYTES=%u\n", data_len);
    printf("SUM=0x%04X\n", (unsigned int)(checksum & 0xFFFF));

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
  printf("TRACK LOOP STOP. PASS=%u FAIL=%u\n", pass_count, fail_count);
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
  unsigned char i;

  printf("\n== DISK RPM CHECK ==\n");
  printf("NEEDS READABLE ID. X/BREAK=EXIT\n");

  plus3_motor_on();

  for (;;) {
    if (loop_exit_requested()) break;

    if (!cmd_seek(FDC_DRIVE, 0, 0) || !wait_seek_complete(FDC_DRIVE, &st0, &c)) {
      fail_count++;
      printf("FAIL #%u\n", fail_count);
      printf("SEEK TRACK0\n");
      printf("ST0=0x%02X\n", st0);
      delay_ms(RPM_FAIL_DELAY_MS);
      continue;
    }

    if (!cmd_read_id(FDC_DRIVE, 0, &st0, &st1, &st2, &c, &h, &r, &n)) {
      fail_count++;
      printf("RPM N/A #%u\n", fail_count);
      printf("ID FAIL\n");
      printf("ST0=0x%02X\n", st0);
      printf("ST1=0x%02X\n", st1);
      printf("ST2=0x%02X\n", st2);
      printf("%s\n", read_id_failure_reason(st1, st2));
      delay_ms(RPM_FAIL_DELAY_MS);
      continue;
    }

    first_r = r;
    start_tick = frame_ticks();
    seen_other = 0;
    dticks = 0;

    for (i = 0; i < 80; i++) {
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
    }

    if (dticks == 0) {
      fail_count++;
      printf("RPM N/A #%u\n", fail_count);
      /* seen_other==0: emulator returned same sector every read (no disk rotation
       * simulation). seen_other==1: sector varied but original never came back
       * within 80 reads (genuine no-index condition on real hardware). */
      printf("%s\n", seen_other ? "NO REV MARK" : "SAME SEC");
      delay_ms(RPM_FAIL_DELAY_MS);
      continue;
    }

    period_ms = (unsigned int)dticks * 20U;
    if (period_ms == 0) {
      fail_count++;
      printf("FAIL #%u\n", fail_count);
      printf("RPM PERIOD BAD\n");
      delay_ms(RPM_FAIL_DELAY_MS);
      continue;
    }

    rpm = (unsigned int)((60000U + (period_ms / 2U)) / period_ms);
    pass_count++;
    printf("RPM #%u\n", pass_count);
    printf("VALUE=%u\n", rpm);
    printf("PERIOD=%ums\n", period_ms);
    printf("STATE=%s\n", (rpm >= 285U && rpm <= 315U) ? "OK" : "OUT-OF-RANGE");
    delay_ms(RPM_LOOP_DELAY_MS);
  }

  plus3_motor_off();
  printf("RPM LOOP STOP. OK=%u FAIL=%u\n", pass_count, fail_count);
}

/* -------------------------------------------------------------------------- */
/* UI                                                                         */
/* -------------------------------------------------------------------------- */

static void print_results(void) {
  unsigned char total;
  unsigned char i;

  ui_clear_screen();
  total = pass_count();

  printf("+------------------------------+\n");
  printf("|       TEST REPORT CARD       |\n");
  printf("+------------------------------+\n");
  ui_print_bar("MOTOR", results.motor_test_pass);
  ui_print_bar("DRIVE", results.sense_drive_pass);
  ui_print_bar("RECAL", results.recalibrate_pass);
  ui_print_bar("SEEK", results.seek_pass);
  ui_print_bar("READID", results.read_id_pass);
  printf("\nOVERALL [");
  for (i = 0; i < total; i++) putchar('#');
  for (; i < 5; i++) putchar('.');
  printf("] %u/5 PASS\n", total);
}

static void run_all_tests(void) {
  ui_clear_screen();
  memset(&results, 0, sizeof(results));
  test_motor_and_drive_status(0);
  test_recal_seek_track2(0);
  test_read_id(0);
  print_results();
  press_any_key(1);
}

static void menu_print(void) {
  unsigned char total = pass_count();
  char status_line[29];

  printf("\n");
  printf("+------------------------------+\n");
  printf("|      ZX +3 DISK TESTER      |\n");
  printf("+------------------------------+\n");
  if (total == 0) {
    strcpy(status_line, "STATUS: NO TESTS RUN");
  } else {
    sprintf(status_line, "STATUS: %u/5 PASS", total);
  }
  printf("| %-28s |\n", status_line);
  printf("+------------------------------+\n");
  printf("| 1 MOTOR AND DRIVE STATUS    |\n");
  printf("| 2 DRIVE READ ID PROBE       |\n");
  printf("| 3 RECALIBRATE AND SEEK      |\n");
  printf("|   TRACK 2                   |\n");
  printf("| 4 INTERACTIVE STEP SEEK     |\n");
  printf("| 5 READ ID ON TRACK 0        |\n");
  printf("| 6 READ TRACK DATA LOOP      |\n");
  printf("| 7 DISK RPM CHECK LOOP       |\n");
  printf("| A RUN ALL CORE TESTS        |\n");
  printf("| R SHOW REPORT CARD          |\n");
  printf("| C CLEAR STORED RESULTS      |\n");
  printf("| Q QUIT                      |\n");
  printf("+------------------------------+\n");
  printf("SELECT KEY: ");
}

int main(void) {
  int ch;

    /* Fix DivMMC font corruption and apply readable UI font. */
    init_ui_font();

  /* Capture paging state at startup for debug output. */
  startup_bank678 = *(volatile unsigned char *)0x5B67;

  /* Initialize motor and paging to safe defaults. */
  set_motor_off();

  memset(&results, 0, sizeof(results));
  disable_terminal_auto_pause();
  if (debug_enabled) {
    printf("DEBUG BUILD\n");
    printf("DBG startup BANK678=0x%02X\n", startup_bank678);
  }

  for (;;) {
    ui_clear_screen();
    menu_print();
    ch = read_key_blocking();
    printf("\n");
    ch = toupper((unsigned char)ch);

    switch (ch) {
      case '1':
        ui_clear_screen();
        test_motor_and_drive_status(1);
        break;
      case '2':
        ui_clear_screen();
        test_read_id_probe();
        press_any_key(1);
        break;
      case '3':
        ui_clear_screen();
        test_recal_seek_track2(1);
        break;
      case '4':
        ui_clear_screen();
        test_seek_interactive();
        break;
      case '5':
        ui_clear_screen();
        test_read_id(1);
        break;
      case '6':
        ui_clear_screen();
        test_read_track_data_loop();
        break;
      case '7':
        ui_clear_screen();
        test_rpm_checker();
        break;
      case 'A':
        ui_clear_screen();
        run_all_tests();
        break;
      case 'R':
        ui_clear_screen();
        print_results();
        press_any_key(1);
        break;
      case 'C':
        ui_clear_screen();
        memset(&results, 0, sizeof(results));
        printf("RESULTS CLEARED\n");
        press_any_key(1);
        break;
      case 'Q':
        plus3_motor_off();
        printf("Exiting...\n");
        return 0;
      default:
        printf("BAD KEY\n");
        break;
    }
  }
  return 0;
}