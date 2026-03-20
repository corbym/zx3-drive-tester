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
#include <intrinsic.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

#include "menu_system.h"
#include "ui.h"

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

/* uPD765A MSR bits */
#define MSR_RQM 0x80 /* Request for Master */
#define MSR_DIO 0x40 /* Data direction: 1 = FDC->CPU */
#define FDC_DRIVE 0  /* internal drive is drive 0 */
#define FDC_RQM_TIMEOUT 20000U
/* Command/result bytes need a small pacing gap; execution-phase data bytes do not. */
#define FDC_CMD_BYTE_GAP_UNITS 4U
#define SEEK_PREP_DELAY_MS 2U
#define SEEK_BUSY_TIMEOUT_MS 900U
#define SEEK_SENSE_RETRIES 48U
#define SEEK_SENSE_RETRY_DELAY_MS 2U
#define DRIVE_READY_TIMEOUT_MS 500U
#define DRIVE_READY_POLL_MS 5U
#define MOTOR_OFF_SETTLE_MS 35U
#define READ_LOOP_PAUSE_STEPS 4U
#define READ_LOOP_PAUSE_MS 6U
#define TEST_CARD_STATE_DELAY_MS 90U
#define RUN_ALL_READY_DELAY_MS 150U
#define RUN_ALL_RUNNING_DELAY_MS 120U
#define RUN_ALL_RESULT_DELAY_MS 220U

/* Approximate sub-millisecond pacing for FDC byte gaps.
 * Bracketed with DI/EI so IM1 interrupts cannot stretch the controller gap.
 */
static void delay_us_approx(unsigned int units) {
  unsigned int i, j;
  intrinsic_di();
  for (i = 0; i < units; i++) {
    for (j = 0; j < 4U; j++) {
      delay_spin_sink++;
    }
  }
  intrinsic_ei();
}

#define RPM_LOOP_DELAY_MS 180U
#define RPM_FAIL_DELAY_MS 450U
#define RPM_EXIT_ARM_DELAY_MS 400U
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

/* Test results storage */
typedef struct {
  unsigned char motor_test_pass;
  unsigned char motor_test_ran;
  unsigned char sense_drive_pass;
  unsigned char sense_drive_ran;
  unsigned char recalibrate_pass;
  unsigned char recalibrate_ran;
  unsigned char seek_pass;
  unsigned char seek_ran;
  unsigned char read_id_pass;
  unsigned char read_id_ran;
} TestResults;

static TestResults results;
static unsigned char last_test_failed;

#define REPORT_STATUS_NONE 0U
#define REPORT_STATUS_READY 1U
#define REPORT_STATUS_RUNNING 2U
#define REPORT_STATUS_COMPLETE 3U

#ifndef RUN_ALL_UI_PACE_HUMAN
#define RUN_ALL_UI_PACE_HUMAN 0
#endif

static unsigned char report_status_code;
static unsigned int run_all_ready_delay_ms = RUN_ALL_READY_DELAY_MS;
static unsigned int run_all_running_delay_ms = RUN_ALL_RUNNING_DELAY_MS;
static unsigned int run_all_result_delay_ms = RUN_ALL_RESULT_DELAY_MS;

static void configure_run_all_timing(void) {
  if (RUN_ALL_UI_PACE_HUMAN) {
    run_all_ready_delay_ms = 250U;
    run_all_running_delay_ms = 250U;
    run_all_result_delay_ms = 500U;
  }
}

static void disable_terminal_auto_pause(void) {
  /* Avoid hidden key waits when output scrolls beyond one screen. */
  ioctl(1, IOCTL_OTERM_PAUSE, 0);
}

static unsigned char pass_count(void) {
  return (unsigned char)(results.motor_test_pass + results.sense_drive_pass +
                         results.recalibrate_pass + results.seek_pass +
                         results.read_id_pass);
}

static void reset_report_progress(void) {
  report_status_code = REPORT_STATUS_NONE;
}

static void set_report_status(unsigned char status_code) {
  report_status_code = status_code;
}

static void ui_apply_menu_row_visual(unsigned char index,
                                     unsigned char selected) {
  const MenuItem* items = menu_items();
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
    ui_attr_set_cell(row, (unsigned char)(items[index].hot_col + 1U),
                     ZX_COLOUR_BLUE, paper, 1);
  }
  ui_screen_put_char(row, 31, selected ? '~' : ' ');
}

static void ui_update_main_menu_selection(unsigned char old_index,
                                          unsigned char new_index) {
  if (old_index == new_index) return;
  ui_apply_menu_row_visual(old_index, 0);
  ui_apply_menu_row_visual(new_index, 1);
}

static void ui_render_main_menu(unsigned char selected_index,
                                unsigned char total) {
  const MenuItem* items = menu_items();
  unsigned char count = menu_item_count();
  unsigned char i;
  unsigned char col;
  char status_line[29];
  static const unsigned char STRIPE_PAPER[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  static const unsigned char STRIPE_INK[8] = {7, 7, 7, 7, 0, 0, 0, 0};

  ui_reset_text_screen_cache();

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

  printf("\nUP   : W/F/CAPS+7\n");
  printf("DOWN : S/V/CAPS+6\n");
  printf("ENTER: SELECT  Q: QUIT\n");

  /* Reapply +3-style colour layout on top of terminal text output. */
  ui_attr_fill(ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 0);
  for (col = 0; col < 32; col++) {
    ui_attr_set_cell(0, col, ZX_COLOUR_WHITE, ZX_COLOUR_BLACK, 1);
    ui_attr_set_cell(15, col, ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 1);
    ui_attr_set_cell(16, col, ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 1);
  }
  for (col = 0; col < 8; col++) {
    ui_attr_set_cell(0, (unsigned char)(24 + col), STRIPE_INK[col],
                     STRIPE_PAPER[col], 1);
  }
  for (i = 0; i < count; i++) {
    ui_apply_menu_row_visual(i, (unsigned char)(i == selected_index));
  }
}

static unsigned char any_report_test_ran(void) {
  return (unsigned char)(results.motor_test_ran || results.sense_drive_ran ||
                         results.recalibrate_ran || results.seek_ran ||
                         results.read_id_ran);
}

static void ui_colour_report_row(unsigned char row, const char* text,
                                 ReportCardState state) {
  const char* lbr;
  const char* rbr;
  unsigned char col;
  unsigned char bar_col;

  if (!text || row >= 24U) return;

  lbr = strchr(text, '[');
  rbr = strchr(text, ']');
  if (lbr && rbr && rbr > lbr) {
    bar_col = (unsigned char)(lbr - text + 1);
    while (bar_col < 32U && &text[bar_col] < rbr) {
      char ch = text[bar_col];
      if (ch == '|') {
        if (state == REPORT_CARD_STATE_PASS) {
          ui_attr_set_cell(row, bar_col, ZX_COLOUR_BLACK, ZX_COLOUR_GREEN, 1);
        } else if (state == REPORT_CARD_STATE_FAIL) {
          /* Spectrum has no orange; red-on-yellow gives a strong warning tone.
           */
          ui_attr_set_cell(row, bar_col, ZX_COLOUR_RED, ZX_COLOUR_YELLOW, 1);
        } else {
          ui_attr_set_cell(row, bar_col, ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 1);
        }
      }
      bar_col++;
    }
  }

  for (col = 0; col < 29U; col++) {
    if (state == REPORT_CARD_STATE_PASS &&
      strncmp(&text[col], "PASS", 4U) == 0) {
      ui_attr_set_cell(row, col, ZX_COLOUR_BLACK, ZX_COLOUR_GREEN, 1);
      ui_attr_set_cell(row, (unsigned char)(col + 1U), ZX_COLOUR_BLACK,
                       ZX_COLOUR_GREEN, 1);
      ui_attr_set_cell(row, (unsigned char)(col + 2U), ZX_COLOUR_BLACK,
                       ZX_COLOUR_GREEN, 1);
      ui_attr_set_cell(row, (unsigned char)(col + 3U), ZX_COLOUR_BLACK,
                       ZX_COLOUR_GREEN, 1);
      break;
    }
    if (state == REPORT_CARD_STATE_FAIL &&
      strncmp(&text[col], "FAIL", 4U) == 0) {
      ui_attr_set_cell(row, col, ZX_COLOUR_RED, ZX_COLOUR_YELLOW, 1);
      ui_attr_set_cell(row, (unsigned char)(col + 1U), ZX_COLOUR_RED,
                       ZX_COLOUR_YELLOW, 1);
      ui_attr_set_cell(row, (unsigned char)(col + 2U), ZX_COLOUR_RED,
                       ZX_COLOUR_YELLOW, 1);
      ui_attr_set_cell(row, (unsigned char)(col + 3U), ZX_COLOUR_RED,
                       ZX_COLOUR_YELLOW, 1);
      break;
    }
    if (state == REPORT_CARD_STATE_NOT_RUN &&
        strncmp(&text[col], "NOT RUN", 7U) == 0) {
      ui_attr_set_cell(row, col, ZX_COLOUR_BLUE, ZX_COLOUR_WHITE, 1);
      ui_attr_set_cell(row, (unsigned char)(col + 1U), ZX_COLOUR_BLUE,
                       ZX_COLOUR_WHITE, 1);
      ui_attr_set_cell(row, (unsigned char)(col + 2U), ZX_COLOUR_BLUE,
                       ZX_COLOUR_WHITE, 1);
      ui_attr_set_cell(row, (unsigned char)(col + 3U), ZX_COLOUR_BLUE,
                       ZX_COLOUR_WHITE, 1);
      ui_attr_set_cell(row, (unsigned char)(col + 4U), ZX_COLOUR_BLUE,
                       ZX_COLOUR_WHITE, 1);
      ui_attr_set_cell(row, (unsigned char)(col + 5U), ZX_COLOUR_BLUE,
                       ZX_COLOUR_WHITE, 1);
      ui_attr_set_cell(row, (unsigned char)(col + 6U), ZX_COLOUR_BLUE,
                       ZX_COLOUR_WHITE, 1);
      break;
    }
  }
}

static ReportCardState report_card_state_from_runpass(unsigned char ran,
                                                       unsigned char pass) {
  if (!ran) return REPORT_CARD_STATE_NOT_RUN;
  return pass ? REPORT_CARD_STATE_PASS : REPORT_CARD_STATE_FAIL;
}

static ReportCardPhase report_card_phase_from_status(unsigned char status) {
  switch (status) {
    case REPORT_STATUS_READY:
      return REPORT_CARD_PHASE_READY;
    case REPORT_STATUS_RUNNING:
      return REPORT_CARD_PHASE_RUNNING;
    case REPORT_STATUS_COMPLETE:
      return REPORT_CARD_PHASE_COMPLETE;
    default:
      return REPORT_CARD_PHASE_IDLE;
  }
}

static void ui_render_report_card(void) {
  unsigned char total = pass_count();
  ReportCardState last_state;
  ReportCardState motor_state;
  ReportCardState drive_state;
  ReportCardState recal_state;
  ReportCardState seek_state;
  ReportCardState readid_state;
  ReportCardState overall_state;
  ReportCard card;

  report_card_init(&card);
  report_card_set_phase(&card, report_card_phase_from_status(report_status_code));
  report_card_set_total_pass(&card, total);

  if (!any_report_test_ran()) {
    last_state = REPORT_CARD_STATE_NOT_RUN;
  } else {
    last_state =
        last_test_failed ? REPORT_CARD_STATE_FAIL : REPORT_CARD_STATE_PASS;
  }

  motor_state =
      report_card_state_from_runpass(results.motor_test_ran, results.motor_test_pass);
  drive_state = report_card_state_from_runpass(results.sense_drive_ran,
                                               results.sense_drive_pass);
  recal_state = report_card_state_from_runpass(results.recalibrate_ran,
                                               results.recalibrate_pass);
  seek_state = report_card_state_from_runpass(results.seek_ran, results.seek_pass);
  readid_state =
      report_card_state_from_runpass(results.read_id_ran, results.read_id_pass);
  overall_state = (total == 5U)
                      ? REPORT_CARD_STATE_PASS
                      : (any_report_test_ran() ? REPORT_CARD_STATE_FAIL
                                               : REPORT_CARD_STATE_NOT_RUN);

  report_card_set_slot_state(&card, REPORT_CARD_SLOT_LAST, last_state);
  report_card_set_slot_state(&card, REPORT_CARD_SLOT_MOTOR, motor_state);
  report_card_set_slot_state(&card, REPORT_CARD_SLOT_DRIVE, drive_state);
  report_card_set_slot_state(&card, REPORT_CARD_SLOT_RECAL, recal_state);
  report_card_set_slot_state(&card, REPORT_CARD_SLOT_SEEK, seek_state);
  report_card_set_slot_state(&card, REPORT_CARD_SLOT_READID, readid_state);
  report_card_set_slot_state(&card, REPORT_CARD_SLOT_OVERALL, overall_state);

  report_card_render(&card);

  /* Rows are fixed by ui_render_text_screen: controls at row 2, lines start at
   * row 3. */
  ui_colour_report_row(4U, report_card_get_slot_line(&card, REPORT_CARD_SLOT_LAST),
                       report_card_get_slot_state(&card, REPORT_CARD_SLOT_LAST));
  ui_colour_report_row(
      5U, report_card_get_slot_line(&card, REPORT_CARD_SLOT_MOTOR),
      report_card_get_slot_state(&card, REPORT_CARD_SLOT_MOTOR));
  ui_colour_report_row(
      6U, report_card_get_slot_line(&card, REPORT_CARD_SLOT_DRIVE),
      report_card_get_slot_state(&card, REPORT_CARD_SLOT_DRIVE));
  ui_colour_report_row(
      7U, report_card_get_slot_line(&card, REPORT_CARD_SLOT_RECAL),
      report_card_get_slot_state(&card, REPORT_CARD_SLOT_RECAL));
  ui_colour_report_row(8U, report_card_get_slot_line(&card, REPORT_CARD_SLOT_SEEK),
                       report_card_get_slot_state(&card, REPORT_CARD_SLOT_SEEK));
  ui_colour_report_row(
      9U, report_card_get_slot_line(&card, REPORT_CARD_SLOT_READID),
      report_card_get_slot_state(&card, REPORT_CARD_SLOT_READID));
  ui_colour_report_row(
      10U, report_card_get_slot_line(&card, REPORT_CARD_SLOT_OVERALL),
      report_card_get_slot_state(&card, REPORT_CARD_SLOT_OVERALL));
}

static void ui_render_track_loop_screen(unsigned char track,
                                        unsigned int pass_count,
                                        unsigned int fail_count,
                                        const char* last1, const char* last2,
                                        const char* result) {
  TrackLoopCard track_loop_card;
  TestCardResult card_result;

  track_loop_card_init(&track_loop_card);
  set_track_loop_track(&track_loop_card, track);
  set_track_loop_counts(&track_loop_card, pass_count, fail_count);
  set_track_loop_last(&track_loop_card, last1 ? last1 : "LAST  : OK");
  set_track_loop_info(&track_loop_card, last2 ? last2 : "INFO  : READY");

  if (result && strcmp(result, "RESULT: FAIL") == 0) {
    card_result = TEST_CARD_RESULT_FAIL;
  } else if (result && strcmp(result, "RESULT: STOPPED") == 0) {
    card_result = TEST_CARD_RESULT_STOPPED;
  } else {
    card_result = TEST_CARD_RESULT_ACTIVE;
  }
  render_track_loop(&track_loop_card, card_result);
}

static void ui_render_rpm_loop_screen(unsigned int rpm, unsigned char rpm_valid,
                                      unsigned int pass_count,
                                      unsigned int fail_count,
                                      const char* last1, const char* last2,
                                      const char* result) {
  RpmLoopCard rpm_loop_card;
  TestCardResult card_result;

  rpm_loop_card_init(&rpm_loop_card);
  set_rpm_loop_rpm(&rpm_loop_card, rpm, rpm_valid);
  set_rpm_loop_counts(&rpm_loop_card, pass_count, fail_count);
  set_rpm_loop_last(&rpm_loop_card, last1 ? last1 : "LAST  : WAITING");
  set_rpm_loop_info(&rpm_loop_card, last2 ? last2 : "INFO  : READY");

  if (result && strcmp(result, "RESULT: FAIL") == 0) {
    card_result = TEST_CARD_RESULT_FAIL;
  } else if (result && strcmp(result, "RESULT: PASS") == 0) {
    card_result = TEST_CARD_RESULT_PASS;
  } else if (result && strcmp(result, "RESULT: OUT-OF-RANGE") == 0) {
    card_result = TEST_CARD_RESULT_OUT_OF_RANGE;
  } else if (result && strcmp(result, "RESULT: STOPPED") == 0) {
    card_result = TEST_CARD_RESULT_STOPPED;
  } else {
    card_result = TEST_CARD_RESULT_ACTIVE;
  }
  render_rpm_loop(&rpm_loop_card, card_result);
}

static int read_key_blocking(void);
static int read_enter_blocking(void);

static void wait_after_test_run(unsigned char manual_run) {
  if (!manual_run) return;
  fflush(stdout);
  read_enter_blocking();
}

static const char* read_id_failure_reason(unsigned char st1,
                                          unsigned char st2) {
  if (st1 & 0x01) return "Missing ID address mark";
  if (st1 & 0x04) return "No data";
  if (st1 & 0x10) return "Overrun";
  if (st1 & 0x20) return "CRC error";
  if (st1 & 0x80) return "End of cylinder";
  if (st2 & 0x01) return "Missing data address mark";
  if (st2 & 0x02) return "Bad cylinder";
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
    {0xEFFE, 0x08, '7'}, {0xFDFE, 0x01, 'A'}, {0x7FFE, 0x08, 'C'},
    {0xFDFE, 0x04, 'D'}, {0xFBFE, 0x04, 'E'}, {0xFEFE, 0x04, 'X'},
    {0xBFFE, 0x08, 'J'}, {0xBFFE, 0x04, 'K'}, {0xEFFE, 0x01, '0'},
    {0xFBFE, 0x01, 'Q'}, {0xFBFE, 0x08, 'R'}, {0x7FFE, 0x01, ' '},
    {0xBFFE, 0x01, '\n'}};

enum { RUNTIME_KEYMAP_COUNT = sizeof(keymap) / sizeof(keymap[0]) };
static unsigned char runtime_key_latched[RUNTIME_KEYMAP_COUNT];
static unsigned char runtime_break_latched;
static int runtime_pending_key = -1;

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

static int scan_runtime_key_event(void) {
  unsigned int i;
  unsigned char pressed;

  if (break_pressed()) {
    if (!runtime_break_latched) {
      runtime_break_latched = 1;
      return 27;
    }
  } else {
    runtime_break_latched = 0;
  }

  for (i = 0; i < RUNTIME_KEYMAP_COUNT; i++) {
    pressed = (unsigned char)((inportb(keymap[i].row_port) &
                               keymap[i].bit_mask) == 0);
    if (pressed) {
      if (runtime_key_latched[i]) {
        continue;
      }
      runtime_key_latched[i] = 1;
      return keymap[i].key;
    }
    runtime_key_latched[i] = 0;
  }

  return -1;
}

static void pump_runtime_key_latch(void) {
  if (runtime_pending_key == -1) {
    runtime_pending_key = scan_runtime_key_event();
  }
}

static int pop_runtime_pending_key(void) {
  int key = runtime_pending_key;
  runtime_pending_key = -1;
  return key;
}

static void delay_ms_pump_keys(unsigned int ms) {
  const unsigned int slice_ms = 50U;
  while (ms >= slice_ms) {
    pump_runtime_key_latch();
    delay_ms(slice_ms);
    ms = (unsigned int)(ms - slice_ms);
  }
  if (ms > 0U) {
    pump_runtime_key_latch();
    delay_ms(ms);
  }
}

static int read_key_blocking(void) {
  for (;;) {
    pump_runtime_key_latch();
    if (runtime_pending_key != -1) {
      return pop_runtime_pending_key();
    }
  }
}

static int read_enter_blocking(void) {
  int key;

  for (;;) {
    pump_runtime_key_latch();
    if (runtime_pending_key == -1) {
      continue;
    }
    key = pop_runtime_pending_key();
    if (key == 27 || key == '\n' || toupper((unsigned char)key) == 'X') {
      return key;
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

static unsigned char rpm_exit_armed(unsigned short loop_start_tick) {
  return (unsigned char)((unsigned short)(frame_ticks() - loop_start_tick) >=
                         (unsigned short)(RPM_EXIT_ARM_DELAY_MS / 20U));
}

/* -------------------------------------------------------------------------- */
/* Low-level I/O                                                              */
/* -------------------------------------------------------------------------- */

/* Wait until RQM set and DIO matches desired direction.
   want_dio = 0 for CPU->FDC (write), 1 for FDC->CPU (read). */
static unsigned char fdc_wait_rqm(unsigned char want_dio,
                                  unsigned int timeout) {
  while (timeout--) {
    unsigned char msr = inportb(FDC_MSR_PORT);
    if ((msr & MSR_RQM) && (((msr & MSR_DIO) != 0) == (want_dio != 0)))
      return 1;
  }
  return 0;
}

static unsigned char fdc_write(unsigned char b) {
  if (!fdc_wait_rqm(0, FDC_RQM_TIMEOUT)) return 0;
  outportb(FDC_DATA_PORT, b);
  delay_us_approx(FDC_CMD_BYTE_GAP_UNITS);
  return 1;
}

static unsigned char fdc_read(unsigned char* out) {
  if (!fdc_wait_rqm(1, FDC_RQM_TIMEOUT)) return 0;
  *out = inportb(FDC_DATA_PORT);
  delay_us_approx(FDC_CMD_BYTE_GAP_UNITS);
  return 1;
}

/* Execution-phase reads must be as tight as possible to avoid ST1.OR overruns.
 */
static unsigned char fdc_read_data_byte(unsigned char* out) {
  if (!fdc_wait_rqm(1, FDC_RQM_TIMEOUT)) return 0;
  *out = inportb(FDC_DATA_PORT);
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
  delay_ms(MOTOR_OFF_SETTLE_MS); /* let not-ready transition latch */
  fdc_drain_interrupts();        /* clear pending interrupt(s) */
  delay_ms(MOTOR_OFF_SETTLE_MS); /* catch late edge on first motor-off */
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

static unsigned char wait_drive_ready(unsigned char drive, unsigned char head,
                                      unsigned char* out_st3) {
  unsigned int waited;
  unsigned char st3 = 0;

  for (waited = 0; waited < DRIVE_READY_TIMEOUT_MS;
       waited += DRIVE_READY_POLL_MS) {
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

static unsigned char cmd_read_data(
    unsigned char drive, unsigned char head, unsigned char c, unsigned char h,
    unsigned char r, unsigned char n, unsigned char* out_st0,
    unsigned char* out_st1, unsigned char* out_st2, unsigned char* out_c,
    unsigned char* out_h, unsigned char* out_r, unsigned char* out_n,
    unsigned char* data, unsigned int data_len) {
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
    if (!fdc_read_data_byte(&data[i])) return 0;
  }

  if (!fdc_read(out_st0)) return 0;
  if (!fdc_read(out_st1)) return 0;
  if (!fdc_read(out_st2)) return 0;
  if (!fdc_read(out_c)) return 0;
  if (!fdc_read(out_h)) return 0;
  if (!fdc_read(out_r)) return 0;
  if (!fdc_read(out_n)) return 0;

  /*
   * On +3 hardware, TC is not software-driven. Successful reads can terminate
   * with IC=01 in ST0 and EN=1 in ST1; treat that case as success when ST2
   * remains clear and no other ST1 bits are set.
   */
  if (((*out_st0 & 0xC0) == 0) && (*out_st1 == 0) && (*out_st2 == 0)) {
    return 1;
  }

  if (((*out_st0 & 0xC0) == 0x40) && ((*out_st1 & 0x80) != 0) &&
      ((*out_st1 & 0x7F) == 0) && (*out_st2 == 0)) {
    return 1;
  }

  return 0;
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
  unsigned char st0 = 0, pcn = 0;
  unsigned int wait_ms;
  unsigned char tries;
  unsigned char busy_bit = (unsigned char)(1u << (drive & 0x03));

  /*
   * Wait for drive-busy bit to clear in MSR.
   * Some emulator runs hold the busy bit longer than expected.
   */
  for (wait_ms = 0; wait_ms < SEEK_BUSY_TIMEOUT_MS; wait_ms++) {
    if (!(inportb(FDC_MSR_PORT) & busy_bit)) break;
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
    printf("\nPRESS ENTER OR ESC/X\n");
    fflush(stdout);
    read_enter_blocking();
  }
}

static unsigned char show_selected_test_cards(void) {
  return (unsigned char)(report_status_code != REPORT_STATUS_RUNNING);
}

static int selected_test_prompt_mode(int interactive) {
  return (interactive || show_selected_test_cards()) ? 1 : 0;
}

static const char* single_shot_test_controls(int interactive) {
  return selected_test_prompt_mode(interactive) ? "KEYS  : ENTER/ESC MENU"
                                                : "KEYS  : AUTO RETURN MENU";
}

/* -------------------------------------------------------------------------- */
/* Tests                                                                      */
/* -------------------------------------------------------------------------- */

static void test_motor_and_drive_status(int interactive) {
  unsigned char st3 = 0;
  unsigned char have_st3;
  unsigned char show_live_card = show_selected_test_cards();
  MotorDriveCard motor_drive_card;

  last_test_failed = 0;
  results.motor_test_ran = 1;
  results.sense_drive_ran = 1;
  motor_drive_card_init(&motor_drive_card, single_shot_test_controls(interactive));

  if (show_live_card) {
    set_card_motor_off(&motor_drive_card);
    set_motor_drive_unknown(&motor_drive_card);
    render_motor_drive(&motor_drive_card, TEST_CARD_RESULT_READY);
    delay_ms(TEST_CARD_STATE_DELAY_MS);
    set_card_motor_on(&motor_drive_card);
    render_motor_drive(&motor_drive_card, TEST_CARD_RESULT_RUNNING);
  }

  plus3_motor_on();

  have_st3 = cmd_sense_drive_status(FDC_DRIVE, 0, &st3);
  set_motor_drive_status(&motor_drive_card, have_st3, st3);
  results.sense_drive_pass = have_st3 ? 1U : 0U;

  plus3_motor_off();
  results.motor_test_pass = have_st3;
  last_test_failed = (unsigned char)(results.sense_drive_pass == 0);
  set_card_motor_off(&motor_drive_card);
  render_motor_drive(&motor_drive_card, results.sense_drive_pass
                                        ? TEST_CARD_RESULT_PASS
                                        : TEST_CARD_RESULT_FAIL);
}

static void test_read_id_probe(int interactive) {
  unsigned char st3 = 0;
  unsigned char st0 = 0, pcn = 0;
  unsigned char rid_ok = 0;
  unsigned char st1 = 0, st2 = 0, c = 0, h = 0, r = 0, n = 0;
  unsigned char have_st3 = 0;
  unsigned char show_live_card = show_selected_test_cards();
  ReadIdProbeCard read_id_probe_card;

  last_test_failed = 0;
  results.read_id_ran = 1;
  read_id_probe_card_init(&read_id_probe_card,
                          single_shot_test_controls(interactive));

  if (show_live_card) {
    set_read_id_probe_unknown(&read_id_probe_card);
    set_id_waiting(&read_id_probe_card);
    render_read_id_probe(&read_id_probe_card, TEST_CARD_RESULT_READY);
    delay_ms(TEST_CARD_STATE_DELAY_MS);
    set_id_probing(&read_id_probe_card);
    render_read_id_probe(&read_id_probe_card, TEST_CARD_RESULT_RUNNING);
  }

  plus3_motor_on();

  /* 1) Raw drive lines (ST3), informational */
  have_st3 = cmd_sense_drive_status(FDC_DRIVE, 0, &st3);
  set_read_id_probe_status(&read_id_probe_card, have_st3, st3);

  /* 2) A command that tends to surface "not ready" in ST0 (and steps hardware)
   */
  if (cmd_recalibrate(FDC_DRIVE) && wait_seek_complete(FDC_DRIVE, &st0, &pcn)) {
    set_precheck_ok(&read_id_probe_card);
  } else {
    set_precheck_fail(&read_id_probe_card);
  }

  /* 3) Media probe: Read ID, best indicator for both hardware and emulation */
  rid_ok = cmd_read_id(FDC_DRIVE, 0, &st0, &st1, &st2, &c, &h, &r, &n);

  results.read_id_pass = rid_ok ? 1 : 0;
  last_test_failed = (unsigned char)(rid_ok == 0);
  plus3_motor_off();
  if (rid_ok) {
    set_id_chrn(&read_id_probe_card, c, h, r, n);
  } else {
    set_id_failure(&read_id_probe_card, read_id_failure_reason(st1, st2));
  }
  render_read_id_probe(&read_id_probe_card, results.read_id_pass
                                          ? TEST_CARD_RESULT_PASS
                                          : TEST_CARD_RESULT_FAIL);
}

static void test_recal_seek_track2(int interactive) {
  unsigned char st0 = 0, pcn = 0;
  unsigned char st3 = 0;
  unsigned char track_target = 2;
  unsigned char recal_ok = 0;
  unsigned char seek_ok = 0;
  unsigned char show_live_card = show_selected_test_cards();
  RecalSeekCard recal_seek_card;
  (void)interactive;

  last_test_failed = 0;
  results.recalibrate_ran = 1;
  results.seek_ran = 1;
  recal_seek_card_init(&recal_seek_card, single_shot_test_controls(interactive));

  if (show_live_card) {
    set_recal_seek_unknown(&recal_seek_card);
    render_recal_seek(&recal_seek_card, TEST_CARD_RESULT_READY);
    delay_ms(TEST_CARD_STATE_DELAY_MS);
    set_recal_status_running(&recal_seek_card);
    set_seek_status_pending(&recal_seek_card);
    render_recal_seek(&recal_seek_card, TEST_CARD_RESULT_RUNNING);
  }

  plus3_motor_on();

  if (!wait_drive_ready(FDC_DRIVE, 0, &st3)) {
    set_recal_seek_ready_fail_st3(&recal_seek_card, st3);
    set_recal_status_skipped(&recal_seek_card);
    set_seek_status_skipped(&recal_seek_card);
    set_detail_check_media(&recal_seek_card);
    results.recalibrate_pass = 0;
    results.seek_pass = 0;
    plus3_motor_off();
    last_test_failed = 1;
    render_recal_seek(&recal_seek_card, TEST_CARD_RESULT_FAIL);
    return;
  }
  set_ready_yes(&recal_seek_card);
  if (!cmd_recalibrate(FDC_DRIVE) ||
      !wait_seek_complete(FDC_DRIVE, &st0, &pcn)) {
    set_recal_status_fail(&recal_seek_card);
    set_seek_status_skipped(&recal_seek_card);
    set_detail_st0_pcn(&recal_seek_card, st0, pcn);
    results.recalibrate_pass = 0;
    results.seek_pass = 0;
    plus3_motor_off();
    last_test_failed = 1;
    render_recal_seek(&recal_seek_card, TEST_CARD_RESULT_FAIL);
    return;
  }
  recal_ok = (unsigned char)(pcn == 0);
  if (recal_ok) {
    set_recal_status_pass(&recal_seek_card);
  } else {
    set_recal_status_fail(&recal_seek_card);
  }

  if (!cmd_seek(FDC_DRIVE, 0, track_target) ||
      !wait_seek_complete(FDC_DRIVE, &st0, &pcn)) {
    set_ready_yes(&recal_seek_card);
    set_seek_status_fail(&recal_seek_card);
    set_detail_st0_pcn(&recal_seek_card, st0, pcn);
    results.recalibrate_pass = recal_ok;
    results.seek_pass = 0;
    plus3_motor_off();
    last_test_failed = 1;
    render_recal_seek(&recal_seek_card, TEST_CARD_RESULT_FAIL);
    return;
  }
  seek_ok = (unsigned char)(pcn == track_target);

  results.recalibrate_pass = recal_ok;
  results.seek_pass = seek_ok;
  plus3_motor_off();
  last_test_failed =
      (unsigned char)(!(results.recalibrate_pass && results.seek_pass));
  if (results.recalibrate_pass) {
    set_recal_status_pass(&recal_seek_card);
  } else {
    set_recal_status_fail(&recal_seek_card);
  }
  if (results.seek_pass) {
    set_seek_status_pass(&recal_seek_card);
  } else {
    set_seek_status_fail(&recal_seek_card);
  }
  set_detail_track(&recal_seek_card, pcn);
  render_recal_seek(&recal_seek_card, (results.recalibrate_pass && results.seek_pass)
                                        ? TEST_CARD_RESULT_PASS
                                        : TEST_CARD_RESULT_FAIL);
}

static void test_seek_interactive(void) {
  unsigned char st0 = 0, pcn = 0;
  unsigned char st3 = 0;
  unsigned char target = 0;
  InteractiveSeekCard interactive_seek_card;

  last_test_failed = 0;
  interactive_seek_card_init(&interactive_seek_card,
                             "KEYS  : K UP  J DOWN  Q EXIT");

  plus3_motor_on();

  if (!wait_drive_ready(FDC_DRIVE, 0, &st3)) {
    set_interactive_seek_ready_fail_st3(&interactive_seek_card, st3);
    set_last_no_seek(&interactive_seek_card);
    set_pcn(&interactive_seek_card, 0U);
    plus3_motor_off();
    last_test_failed = 1;
    render_interactive_seek(&interactive_seek_card, TEST_CARD_RESULT_FAIL);
    return;
  }
  
  for (;;) {
    set_interactive_seek_track(&interactive_seek_card, target);
    set_last_st0(&interactive_seek_card, st0);
    set_pcn(&interactive_seek_card, pcn);
    render_interactive_seek(&interactive_seek_card, TEST_CARD_RESULT_ACTIVE);
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
          set_interactive_seek_track(&interactive_seek_card, target);
          set_last_st0(&interactive_seek_card, st0);
          set_pcn(&interactive_seek_card, pcn);
          render_interactive_seek(&interactive_seek_card,
                                  TEST_CARD_RESULT_STOPPED);
          return;
        default:
          break;
      }
    }

    if (!cmd_seek(FDC_DRIVE, 0, target)) {
      set_interactive_seek_track(&interactive_seek_card, target);
      set_last_seek_cmd_fail(&interactive_seek_card);
      set_pcn(&interactive_seek_card, pcn);
      plus3_motor_off();
      last_test_failed = 1;
      render_interactive_seek(&interactive_seek_card, TEST_CARD_RESULT_FAIL);
      return;
    }

    if (!wait_seek_complete(FDC_DRIVE, &st0, &pcn)) {
      set_interactive_seek_track(&interactive_seek_card, target);
      set_last_wait_timeout(&interactive_seek_card);
      set_pcn(&interactive_seek_card, pcn);
      plus3_motor_off();
      last_test_failed = 1;
      render_interactive_seek(&interactive_seek_card, TEST_CARD_RESULT_FAIL);
      return;
    }
  }
}

static void test_read_id(int interactive) {
  unsigned char st0 = 0, st1 = 0, st2 = 0, c = 0, h = 0, r = 0, n = 0;
  unsigned char st3 = 0;
  unsigned char ok;
  unsigned char show_live_card = show_selected_test_cards();
  ReadIdCard read_id_card;
  (void)interactive;

  last_test_failed = 0;
  results.read_id_ran = 1;
  read_id_card_init(&read_id_card, single_shot_test_controls(0));

  if (show_live_card) {
    set_waiting(&read_id_card);
    render_read_id(&read_id_card, TEST_CARD_RESULT_READY);
    delay_ms(TEST_CARD_STATE_DELAY_MS);
    set_reading(&read_id_card);
    render_read_id(&read_id_card, TEST_CARD_RESULT_RUNNING);
  }

  plus3_motor_on();

  if (!wait_drive_ready(FDC_DRIVE, 0, &st3)) {
    set_drive_not_ready(&read_id_card, st3);
    results.read_id_pass = 0;
    plus3_motor_off();
    last_test_failed = 1;
    render_read_id(&read_id_card, TEST_CARD_RESULT_FAIL);
    return;
  }

  /* Try to get to track 0 first */
  cmd_recalibrate(FDC_DRIVE);
  {
    unsigned char t0, tp;
    wait_seek_complete(FDC_DRIVE, &t0, &tp);
  }

  ok = cmd_read_id(FDC_DRIVE, 0, &st0, &st1, &st2, &c, &h, &r, &n);

  set_status(&read_id_card, st0, st1, st2);
  if (ok) {
    set_chrn_valid(&read_id_card, c, h, r, n);
    set_detail_id_ok(&read_id_card);
  } else {
    set_chrn_invalid(&read_id_card);
    set_detail_failure(&read_id_card, read_id_failure_reason(st1, st2));
  }

  results.read_id_pass = ok;
  last_test_failed = (unsigned char)(ok == 0);
  plus3_motor_off();
  render_read_id(&read_id_card,
                 ok ? TEST_CARD_RESULT_PASS : TEST_CARD_RESULT_FAIL);
}

static void render_track_loop_fail(unsigned char track, unsigned int pass_count,
                                   unsigned int fail_count, const char* last1,
                                   const char* last2) {
  ui_render_track_loop_screen(track, pass_count, fail_count, last1, last2,
                              "RESULT: FAIL");
}

static void render_rpm_loop_fail(unsigned int rpm, unsigned int pass_count,
                                 unsigned int fail_count, const char* last1,
                                 const char* last2) {
  ui_render_rpm_loop_screen(rpm, rpm != 0, pass_count, fail_count, last1, last2,
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
  char ui_line2[48];

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
      ui_render_track_loop_screen(track, pass_count, fail_count, "LAST  : OK",
                                  "INFO  : LOOPING", "RESULT: ACTIVE");
      need_ui_redraw = 0;
    }

    if (!wait_drive_ready(FDC_DRIVE, 0, &st3)) {
      fail_count++;
      sprintf(ui_line1, "LAST  : DRIVE NR ST3=%02X", st3);
      render_track_loop_fail(track, pass_count, fail_count, ui_line1,
                             "INFO  : RETRYING");
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
        render_track_loop_fail(track, pass_count, fail_count, ui_line1,
                               ui_line2);
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
      render_track_loop_fail(track, pass_count, fail_count, ui_line1,
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
  ui_render_track_loop_screen(track, pass_count, fail_count, "LAST  : STOPPED",
                              "INFO  : USER EXIT", "RESULT: STOPPED");
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
  unsigned short loop_start_tick = 0;

  last_test_failed = 0;

  plus3_motor_on();
  loop_start_tick = frame_ticks();

  for (;;) {
    if (rpm_exit_armed(loop_start_tick) && loop_exit_requested()) break;

#if HEADLESS_ROM_FONT
    if (pass_count + fail_count >= 3U) break;
#endif

    if (!wait_drive_ready(FDC_DRIVE, 0, &st3)) {
      fail_count++;
      render_rpm_loop_fail(rpm, pass_count, fail_count,
                           "LAST  : DRIVE NOT READY", "INFO  : CHECK MEDIA");
      delay_ms(RPM_FAIL_DELAY_MS);
      continue;
    }

    if (!cmd_seek(FDC_DRIVE, 0, 0) ||
        !wait_seek_complete(FDC_DRIVE, &st0, &c)) {
      fail_count++;
      render_rpm_loop_fail(rpm, pass_count, fail_count,
                           "LAST  : SEEK TRACK0 FAIL", "INFO  : ST0 SET");
      delay_ms(RPM_FAIL_DELAY_MS);
      continue;
    }

    if (!cmd_read_id(FDC_DRIVE, 0, &st0, &st1, &st2, &c, &h, &r, &n)) {
      fail_count++;
      render_rpm_loop_fail(rpm, pass_count, fail_count, "LAST  : ID FAIL",
                           read_id_failure_reason(st1, st2));
      delay_ms(RPM_FAIL_DELAY_MS);
      continue;
    }

    first_r = r;
    start_tick = frame_ticks();
    seen_other = 0;
    dticks = 0;
    exit_now = 0;

    for (i = 0; i < 120; i++) {
      if (rpm_exit_armed(loop_start_tick) && loop_exit_requested()) {
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
      render_rpm_loop_fail(rpm, pass_count, fail_count, "LAST  : RPM N/A",
                           seen_other ? "NO REV MARK" : "SAME SEC");
      delay_ms(RPM_FAIL_DELAY_MS);
      continue;
    }

    period_ms = (unsigned int)dticks * 20U;
    if (period_ms == 0) {
      fail_count++;
      render_rpm_loop_fail(rpm, pass_count, fail_count, "LAST  : PERIOD BAD",
                           "INFO  : ZERO DELTA");
      delay_ms(RPM_FAIL_DELAY_MS);
      continue;
    }

    rpm = (unsigned int)((60000U + (period_ms / 2U)) / period_ms);
    pass_count++;
    ui_render_rpm_loop_screen(
        rpm, 1, pass_count, fail_count, "LAST  : SAMPLE OK",
        "INFO  : PERIOD READY",
        (rpm >= 285U && rpm <= 315U) ? "RESULT: PASS" : "RESULT: OUT-OF-RANGE");
    delay_ms(RPM_LOOP_DELAY_MS);
  }

  plus3_motor_off();
  ui_render_rpm_loop_screen(rpm, rpm != 0, pass_count, fail_count,
                            "LAST  : STOPPED", "INFO  : USER EXIT",
                            "RESULT: STOPPED");
  last_test_failed = (unsigned char)(fail_count > 0);
}

/* -------------------------------------------------------------------------- */
/* UI                                                                         */
/* -------------------------------------------------------------------------- */

static void print_results(void) { ui_render_report_card(); }

typedef void (*TestFunc)(int interactive);

static const TestFunc run_all_test_list[] = {
    test_motor_and_drive_status,
    test_read_id_probe,
    test_recal_seek_track2,
    test_read_id,
};

enum {
  RUN_ALL_TEST_COUNT = sizeof(run_all_test_list) / sizeof(run_all_test_list[0])
};

static void run_all_tests(unsigned char human_mode) {
  unsigned char i;

  memset(&results, 0, sizeof(results));
  reset_report_progress();
  last_test_failed = 0;

  set_report_status(REPORT_STATUS_READY);
  ui_render_report_card();
  if (human_mode) delay_ms_pump_keys(run_all_ready_delay_ms);

  for (i = 0; i < RUN_ALL_TEST_COUNT; i++) {
    set_report_status(REPORT_STATUS_RUNNING);
    if (human_mode) delay_ms_pump_keys(run_all_running_delay_ms);
    run_all_test_list[i](0);
    set_report_status(REPORT_STATUS_COMPLETE);
    if (i == RUN_ALL_TEST_COUNT - 1U) {
      print_results();
    } else {
      ui_render_report_card();
    }
    if (human_mode) delay_ms_pump_keys(run_all_result_delay_ms);
  }

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
  startup_bank678 = *(volatile unsigned char*)0x5B67;
#endif

  /* Initialize motor and paging to safe defaults. */
  set_motor_off();
  configure_run_all_timing();

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
    {
      unsigned char old_selected = selected_menu;
      unsigned char selection_changed = 0;
      int action_key =
          menu_resolve_action_key(ch, &selected_menu, &selection_changed);

      if (selection_changed && action_key == 0) {
        ui_update_main_menu_selection(old_selected, selected_menu);
        continue;
      }

      if (action_key == 0) {
        continue;
      }

      ch = action_key;
    }

    switch (ch) {
      case 'M':
        test_motor_and_drive_status(1);
        wait_after_test_run(1);
        menu_dirty = 1;
        break;
      case 'P':
        test_read_id_probe(1);
        wait_after_test_run(1);
        menu_dirty = 1;
        break;
      case 'K':
        test_recal_seek_track2(1);
        wait_after_test_run(1);
        menu_dirty = 1;
        break;
      case 'I':
        test_seek_interactive();
        wait_after_test_run(1);
        menu_dirty = 1;
        break;
      case 'T':
        test_read_id(1);
        wait_after_test_run(1);
        menu_dirty = 1;
        break;
      case 'D':
        test_read_track_data_loop();
        wait_after_test_run(1);
        menu_dirty = 1;
        break;
      case 'H':
        test_rpm_checker();
        wait_after_test_run(1);
        menu_dirty = 1;
        break;
      case 'A':
        run_all_tests(1);
        wait_after_test_run(1);
        menu_dirty = 1;
        break;
      case 'R':
        print_results();
        press_any_key(1);
        menu_dirty = 1;
        break;
      case 'C':
        memset(&results, 0, sizeof(results));
        last_test_failed = 0;
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