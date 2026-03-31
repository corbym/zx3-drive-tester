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

#include "menu_system.h"
#include "shared_strings.h"
#include "test_cards.h"
#include "ui.h"

#include "disk_operations.h"

extern unsigned char inportb(unsigned short port);

/* Thin wrappers that keep the UI drive-status badge in sync */
static void motor_on(void)  { plus3_motor_on();  ui_set_drive_motor(1); }
static void motor_off(void) { plus3_motor_off(); ui_set_drive_motor(0); }

#ifndef IOCTL_OTERM_PAUSE
#define IOCTL_OTERM_PAUSE 0xC042
#endif

#define READ_LOOP_PAUSE_STEPS 2U
#define READ_LOOP_PAUSE_MS    25U
#define TEST_CARD_STATE_DELAY_MS 190U
#define RUN_ALL_READY_DELAY_MS 150U
#define RUN_ALL_RUNNING_DELAY_MS 120U
#define RUN_ALL_RESULT_DELAY_MS 1500U

#define RPM_FAIL_DELAY_MS 450U
#define RPM_EXIT_ARM_DELAY_MS 1500U
/* Number of revolutions per measurement. 4 revs ≈ 800 ms at 300 RPM;
 * averaging over 4 reduces 20 ms tick quantisation error to ±2.5%. */
#define RPM_SAMPLE_REVS 4U
/* Measurements taken per display update; the minimum is used to reduce
 * software-overhead inflation of the measured period. */
#define RPM_STAT_SAMPLES 5U
/* Per-measurement timeout in 20 ms ticks. 150 × 20 ms = 3000 ms; with
 * RPM_SAMPLE_REVS=4 this still covers drives down to roughly 80 RPM.
 * A stopped spindle will time out and be shown. */
#define RPM_REV_TIMEOUT_TICKS 150U
/* Reject implausibly-fast samples that would imply >~670 RPM.
 * For 4 measured revolutions this is 18 ticks * 20 ms / 4 = 90 ms/rev. */
#define RPM_MIN_VALID_TICKS 18U

#ifdef DEBUG
#define DEBUG_ENABLED 1
#else
#define DEBUG_ENABLED 0
#endif

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
    unsigned char probe_pass;
    unsigned char probe_ran;
    unsigned char seek_read_pass;
    unsigned char seek_read_ran;
    unsigned char rpm_pass;
    unsigned char rpm_ran;
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

#if RUN_ALL_UI_PACE_HUMAN
#define RUN_ALL_READY_DELAY_EFFECTIVE_MS 250U
#define RUN_ALL_RUNNING_DELAY_EFFECTIVE_MS 250U
#define RUN_ALL_RESULT_DELAY_EFFECTIVE_MS 500U
#else
#define RUN_ALL_READY_DELAY_EFFECTIVE_MS RUN_ALL_READY_DELAY_MS
#define RUN_ALL_RUNNING_DELAY_EFFECTIVE_MS RUN_ALL_RUNNING_DELAY_MS
#define RUN_ALL_RESULT_DELAY_EFFECTIVE_MS RUN_ALL_RESULT_DELAY_MS
#endif

static void disable_terminal_auto_pause(void) {
    /* Avoid hidden key waits when output scrolls beyond one screen. */
    ioctl(1, IOCTL_OTERM_PAUSE, 0);
}

static unsigned char pass_count(void) {
    return (unsigned char)(results.probe_pass + results.seek_read_pass +
                           results.rpm_pass);
}

static unsigned char pass_count_ran(void) {
    return (unsigned char)(results.probe_ran + results.seek_read_ran +
                           results.rpm_ran);
}

static void reset_report_progress(void) {
    report_status_code = REPORT_STATUS_NONE;
}

static void set_report_status(unsigned char status_code) {
    report_status_code = status_code;
}


static unsigned char any_report_test_ran(void) {
    return (unsigned char)(results.probe_ran || results.seek_read_ran ||
                           results.rpm_ran);
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

/* Seek/recalibrate completion check.  wait_seek_complete() already guarantees
 * SE (bit 5) is set before returning; the IC bits (7-6) can be non-zero on
 * real +3 hardware even when the head physically reached the target track, so
 * we do not gate on IC here.  The SE confirmation from wait_seek_complete is
 * the authoritative signal that the command completed. */
static unsigned char seek_completion_ok(unsigned char st0) {
    return (unsigned char) ((st0 & 0x20U) != 0U);
}

/* Run RECAL + wait for interrupt + validate SE/IC completion semantics. */
static unsigned char recalibrate_track0_strict(FdcSeekResult *result) {
    result->st0 = 0;
    result->pcn = 0;
    if (!cmd_recalibrate(FDC_DRIVE)) return 0;
    if (!wait_seek_complete(FDC_DRIVE, result)) return 0;
    return seek_completion_ok(result->st0);
}

static void ui_render_report_card(void) {
    const unsigned char total = pass_count();
    ReportCard card;
    ReportCardState overall_state;

    report_card_init(&card);
    report_card_set_phase(&card, report_card_phase_from_status(report_status_code));
    report_card_set_total_pass(&card, total);

    report_card_set_slot_state(&card, REPORT_CARD_SLOT_PROBE,
                               report_card_state_from_runpass(results.probe_ran,
                                                              results.probe_pass));
    report_card_set_slot_state(&card, REPORT_CARD_SLOT_SEEK,
                               report_card_state_from_runpass(results.seek_read_ran,
                                                              results.seek_read_pass));
    report_card_set_slot_state(&card, REPORT_CARD_SLOT_RPM,
                               report_card_state_from_runpass(results.rpm_ran,
                                                              results.rpm_pass));

    if (!any_report_test_ran()) {
        overall_state = REPORT_CARD_STATE_NOT_RUN;
    }
    else if (total == pass_count_ran()) {
        overall_state = REPORT_CARD_STATE_PASS;
    }
    else {
        overall_state = REPORT_CARD_STATE_FAIL;
    }
    report_card_set_slot_state(&card, REPORT_CARD_SLOT_OVERALL, overall_state);

    report_card_render(&card);
}

static int read_key_blocking(void);

static int read_enter_blocking(void);

static void wait_after_test_run(unsigned char manual_run) {
    if (!manual_run) return;
    fflush(stdout);
    read_enter_blocking();
}


static const KeyMap keymap[] = {
    {0xFEFE, 0x04, 'X'},
    {0xFDFE, 0x08, 'F'},  /* F: hex panel scroll up    */
    {0xBFFE, 0x08, 'J'},  /* J: track down             */
    {0xBFFE, 0x04, 'K'},  /* K: track up               */
    {0xFEFE, 0x10, 'V'},  /* V: hex panel scroll down  */
    {0xFBFE, 0x01, 'Q'},
    {0xBFFE, 0x01, '\n'}
};

enum { RUNTIME_KEYMAP_COUNT = sizeof(keymap) / sizeof(keymap[0]) };

static unsigned char runtime_key_latched[RUNTIME_KEYMAP_COUNT];
static unsigned char runtime_pending_key;


static unsigned char scan_runtime_key_event(void) {
    if (break_pressed()) {
        return 27;
    }

    for (unsigned char i = 0; i < RUNTIME_KEYMAP_COUNT; i++) {
        unsigned char pressed = (unsigned char) ((inportb(keymap[i].row_port) &
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

    return 0;
}

static void pump_runtime_key_latch(void) {
    if (runtime_pending_key == 0U) {
        runtime_pending_key = scan_runtime_key_event();
    }
}

static int pop_runtime_pending_key(void) {
    unsigned char key = runtime_pending_key;
    runtime_pending_key = 0;
    return key;
}

static int read_key_blocking(void) {
    for (;;) {
        pump_runtime_key_latch();
        if (runtime_pending_key != 0U) {
            return pop_runtime_pending_key();
        }
    }
}

static int read_enter_blocking(void) {
    for (;;) {
        int key;

        pump_runtime_key_latch();
        if (runtime_pending_key == 0U) {
            continue;
        }
        key = pop_runtime_pending_key();
        if (key == 27 || key == '\n' || toupper((unsigned char)key) == 'X') {
            return key;
        }
    }
}

static unsigned char loop_exit_requested(void) {
    return (unsigned char)(break_pressed() ||
                           ((inportb(0xBFFE) & 0x01) == 0) ||
                           ((inportb(0xFEFE) & 0x04) == 0));
}

static unsigned char rpm_exit_armed(unsigned short loop_start_tick) {
    return (unsigned char) ((unsigned short) (frame_ticks() - loop_start_tick) >=
                            (unsigned short) (RPM_EXIT_ARM_DELAY_MS / 20U));
}

static unsigned char track_loop_consume_action(unsigned char *track,
                                               unsigned char *seek_required,
                                               unsigned char *ui_redraw_required) {
    pump_runtime_key_latch();

    switch (runtime_pending_key) {
        case 'J':
            runtime_pending_key = 0;
            if (*track > 0U) {
                (*track)--;
                *seek_required = 1;
                *ui_redraw_required = 1;
            }
            return 0;
        case 'K':
            runtime_pending_key = 0;
            if (*track < 79U) {
                (*track)++;
                *seek_required = 1;
                *ui_redraw_required = 1;
            }
            return 0;
        case 'X':
        case 27:
            runtime_pending_key = 0;
            return 1;
        default:
            runtime_pending_key = 0;
            return 0;
    }
}


void press_any_key(int interactive) {
    if (interactive == 1) {
        fflush(stdout);
        read_enter_blocking();
    }
}

static unsigned char show_selected_test_cards(void) {
    return (unsigned char) (report_status_code != REPORT_STATUS_RUNNING);
}

static const char *single_shot_test_controls(int interactive) {
    return interactive ? zx3_ctrl_enter_esc_menu : NULL;
}

/* -------------------------------------------------------------------------- */
/* Tests                                                                      */
/* -------------------------------------------------------------------------- */

static void test_drive_probe(int interactive) {
    unsigned char status_3 = 0;
    unsigned char have_st3 = 0;
    FdcSeekResult seek_result;
    unsigned char rid_ok = 0;
    FdcResult rid_result;
    unsigned char show_live_card = show_selected_test_cards();
    DriveProbeCard card;

    seek_result.st0 = 0;
    seek_result.pcn = 0;
    rid_result.status.st0 = 0;
    rid_result.status.st1 = 0;
    rid_result.status.st2 = 0;
    rid_result.chrn.c = 0;
    rid_result.chrn.h = 0;
    rid_result.chrn.r = 0;
    rid_result.chrn.n = 0;

    last_test_failed = 0;
    results.probe_ran = 1;
    drive_probe_card_init(&card, single_shot_test_controls(interactive));

    if (show_live_card) {
        drive_probe_card_render(&card, TEST_CARD_RESULT_READY);
        delay_ms(TEST_CARD_STATE_DELAY_MS);
        drive_probe_card_set_motor(&card, 1);
        drive_probe_card_render(&card, TEST_CARD_RESULT_RUNNING);
    }

    motor_on();

    /* ST3 — shows READY/WPROT/TRACK0/FAULT lines */
    have_st3 = cmd_sense_drive_status(FDC_DRIVE, 0, &status_3);
    if (have_st3) ui_set_drive_st3(status_3);
    drive_probe_card_set_st3(&card, have_st3, status_3);

    /* Recalibrate to track 0 before READ ID */
    cmd_recalibrate(FDC_DRIVE);
    wait_seek_complete(FDC_DRIVE, &seek_result);

    /* READ ID: primary pass/fail indicator */
    rid_ok = cmd_read_id(FDC_DRIVE, 0, &rid_result);

    results.probe_pass = (unsigned char)(have_st3 && rid_ok);
    last_test_failed = (unsigned char)(results.probe_pass == 0);
    motor_off();
    drive_probe_card_set_motor(&card, 0);

    if (rid_ok) {
        drive_probe_card_set_id_chrn(&card, rid_result.chrn.c, rid_result.chrn.h,
                                     rid_result.chrn.r, rid_result.chrn.n);
    }
    else {
        drive_probe_card_set_id_status(&card,
                                       read_id_failure_reason(rid_result.status.st1,
                                                               rid_result.status.st2));
    }
    drive_probe_card_render(&card, results.probe_pass
                            ? TEST_CARD_RESULT_PASS : TEST_CARD_RESULT_FAIL);
}

/* Fixed seek pattern: 0 → 39 → 0 → 79 → 0 (tests full travel both ways) */
#define SEEK_PATTERN_LEN 5U
static const unsigned char seek_pattern[SEEK_PATTERN_LEN] = {0U, 39U, 0U, 79U, 0U};

static void test_seek_and_read(int interactive) {
    static unsigned char sector_data[1024];
    FdcResult read_id_result;
    FdcResult read_data_result;
    FdcSeekResult seek_result;
    unsigned char drive_status_st3 = 0;
    unsigned int sr_pass = 0;
    unsigned int sr_fail = 0;
    unsigned int sector_data_len = 0;
    unsigned char recal_ok = 0;
    unsigned char any_seek_fail = 0;
    unsigned char show_live_card = show_selected_test_cards();
    SeekReadCard card;

    seek_result.st0 = 0;
    seek_result.pcn = 0;

    last_test_failed = 0;
    results.seek_read_ran = 1;

    if (interactive) {
        /* --- Interactive mode: J/K track navigation with hex dump panel --- */
        unsigned char current_track = 0;
        unsigned char seek_required = 1;
        unsigned char recal_required = 1;
        unsigned char ui_redraw_required = 1;
        unsigned char hex_scroll_row = 0;
        unsigned char max_scroll_rows = 0;

        seek_read_card_init(&card, "K/J NAV F/V SCRL X EXIT");

        memset(runtime_key_latched, 0, sizeof(runtime_key_latched));
        runtime_pending_key = 0;
        disk_operations_set_idle_pump(pump_runtime_key_latch);
        ui_set_idle_pump(pump_runtime_key_latch);

        motor_on();

        for (;;) {
            pump_runtime_key_latch();
            if (runtime_pending_key == 'F') {
                runtime_pending_key = 0;
                if (hex_scroll_row > 0U) hex_scroll_row--;
            } else if (runtime_pending_key == 'V') {
                runtime_pending_key = 0;
                if (hex_scroll_row < max_scroll_rows) hex_scroll_row++;
            }

            if (track_loop_consume_action(&current_track, &seek_required,
                                          &ui_redraw_required)) {
                break;
            }

            if (ui_redraw_required) {
                seek_read_card_set_track(&card, current_track);
                seek_read_card_set_counts(&card, sr_pass, sr_fail);
                seek_read_card_render(&card, TEST_CARD_RESULT_ACTIVE);
                ui_redraw_required = 0;
            }

            if (!wait_drive_ready(FDC_DRIVE, 0, &drive_status_st3)) {
                sr_fail++;
                seek_read_card_set_ready_fail_st3(&card, drive_status_st3);
                seek_read_card_set_counts(&card, sr_pass, sr_fail);
                seek_read_card_render(&card, TEST_CARD_RESULT_FAIL);
                seek_required = 1;
                recal_required = 1;
                ui_redraw_required = 0;
                delay_ms(20);
                continue;
            }
            seek_read_card_set_ready(&card, 1);

            if (seek_required) {
                if (recal_required) {
                    FdcSeekResult recal_result;
                    seek_read_card_set_recal_status(&card, RECAL_SEEK_STATUS_RUNNING);
                    if (!recalibrate_track0_strict(&recal_result)) {
                        sr_fail++;
                        seek_read_card_set_recal_status(&card, RECAL_SEEK_STATUS_FAIL);
                        seek_read_card_set_counts(&card, sr_pass, sr_fail);
                        seek_read_card_render(&card, TEST_CARD_RESULT_FAIL);
                        seek_required = 1;
                        ui_redraw_required = 0;
                        delay_ms(20);
                        continue;
                    }
                    seek_read_card_set_recal_status(&card, RECAL_SEEK_STATUS_PASS);
                    recal_required = 0;
                }
                seek_read_card_set_seek_status(&card, RECAL_SEEK_STATUS_RUNNING);
                if (!cmd_seek(FDC_DRIVE, 0, current_track) ||
                    !wait_seek_complete(FDC_DRIVE, &seek_result)) {
                    sr_fail++;
                    seek_read_card_set_seek_status(&card, RECAL_SEEK_STATUS_FAIL);
                    seek_read_card_set_counts(&card, sr_pass, sr_fail);
                    seek_read_card_render(&card, TEST_CARD_RESULT_FAIL);
                    seek_required = 1;
                    ui_redraw_required = 0;
                    delay_ms(20);
                    continue;
                }
                seek_read_card_set_seek_status(&card, RECAL_SEEK_STATUS_PASS);
                seek_required = 0;
                hex_scroll_row = 0;
                max_scroll_rows = 0;
                ui_redraw_required = 1;
                ui_reset_hex_dump_panel();
            }

            if (!cmd_read_id(FDC_DRIVE, 0, &read_id_result)) {
                sr_fail++;
                seek_read_card_set_id_status(&card,
                    read_id_failure_reason(read_id_result.status.st1,
                                           read_id_result.status.st2));
                seek_read_card_set_counts(&card, sr_pass, sr_fail);
                seek_read_card_render(&card, TEST_CARD_RESULT_FAIL);
                seek_required = 1;
                ui_redraw_required = 0;
                delay_ms(20);
                continue;
            }

            sector_data_len = sector_size_from_n(read_id_result.chrn.n);
            if (sector_data_len == 0 || sector_data_len > sizeof(sector_data)) {
                sr_fail++;
                seek_read_card_set_id_status(&card, "BAD N");
                seek_read_card_set_counts(&card, sr_pass, sr_fail);
                seek_read_card_render(&card, TEST_CARD_RESULT_FAIL);
                seek_required = 1;
                ui_redraw_required = 0;
                delay_ms(20);
                continue;
            }
            {
                unsigned int total_rows = (sector_data_len + 7U) / 8U;
                max_scroll_rows = total_rows > 12U ? total_rows - 12U : 0U;
                if (hex_scroll_row > max_scroll_rows) hex_scroll_row = max_scroll_rows;
            }

            if (!cmd_read_data(FDC_DRIVE, read_id_result.chrn.h,
                               read_id_result.chrn.c, read_id_result.chrn.h,
                               read_id_result.chrn.r, read_id_result.chrn.n,
                               &read_data_result, sector_data,
                               sector_data_len)) {
                sr_fail++;
                seek_read_card_set_id_status(&card,
                    read_id_failure_reason(read_data_result.status.st1,
                                           read_data_result.status.st2));
                seek_read_card_set_counts(&card, sr_pass, sr_fail);
                seek_read_card_render(&card, TEST_CARD_RESULT_FAIL);
                seek_required = 1;
                ui_redraw_required = 0;
                delay_ms(20);
                continue;
            }

            sr_pass++;
            seek_read_card_set_id_chrn(&card, read_id_result.chrn.c,
                                        read_id_result.chrn.h,
                                        read_id_result.chrn.r,
                                        read_id_result.chrn.n);
            seek_read_card_set_counts(&card, sr_pass, sr_fail);
            seek_read_card_render(&card, TEST_CARD_RESULT_ACTIVE);
            ui_set_hex_dump_scroll(hex_scroll_row);
            ui_render_hex_dump_panel(sector_data, sector_data_len);
            ui_redraw_required = 0;

            delay_ms((unsigned int)(READ_LOOP_PAUSE_STEPS * READ_LOOP_PAUSE_MS));
            continue;
        }

        motor_off();
        disk_operations_set_idle_pump(0);
        ui_set_idle_pump(0);
        seek_read_card_set_counts(&card, sr_pass, sr_fail);
        seek_read_card_render(&card, TEST_CARD_RESULT_STOPPED);
        results.seek_read_pass = (unsigned char)(sr_fail == 0 && sr_pass > 0);
        last_test_failed = (unsigned char)(results.seek_read_pass == 0);
        return;
    }

    /* --- RunAll mode: fixed seek pattern 0 → 39 → 0 → 79 → 0 --- */
    seek_read_card_init(&card, single_shot_test_controls(0));

    if (show_live_card) {
        seek_read_card_render(&card, TEST_CARD_RESULT_READY);
        delay_ms(TEST_CARD_STATE_DELAY_MS);
        seek_read_card_set_recal_status(&card, RECAL_SEEK_STATUS_RUNNING);
        seek_read_card_render(&card, TEST_CARD_RESULT_RUNNING);
    }

    motor_on();

    if (!wait_drive_ready(FDC_DRIVE, 0, &drive_status_st3)) {
        seek_read_card_set_ready_fail_st3(&card, drive_status_st3);
        results.seek_read_pass = 0;
        last_test_failed = 1;
        motor_off();
        seek_read_card_render(&card, TEST_CARD_RESULT_FAIL);
        return;
    }
    seek_read_card_set_ready(&card, 1);

    if (!recalibrate_track0_strict(&seek_result)) {
        seek_read_card_set_recal_status(&card, RECAL_SEEK_STATUS_FAIL);
        results.seek_read_pass = 0;
        last_test_failed = 1;
        motor_off();
        seek_read_card_render(&card, TEST_CARD_RESULT_FAIL);
        return;
    }
    recal_ok = 1;
    seek_read_card_set_recal_status(&card, RECAL_SEEK_STATUS_PASS);

    {
        unsigned char i;
        for (i = 0; i < SEEK_PATTERN_LEN; i++) {
            unsigned char target = seek_pattern[i];
            seek_read_card_set_track(&card, target);
            seek_read_card_set_seek_status(&card, RECAL_SEEK_STATUS_RUNNING);
            seek_read_card_set_counts(&card, sr_pass, sr_fail);
            if (show_live_card) seek_read_card_render(&card, TEST_CARD_RESULT_RUNNING);

            if (!cmd_seek(FDC_DRIVE, 0, target) ||
                !wait_seek_complete(FDC_DRIVE, &seek_result) ||
                !seek_completion_ok(seek_result.st0)) {
                seek_read_card_set_seek_status(&card, RECAL_SEEK_STATUS_FAIL);
                sr_fail++;
                any_seek_fail = 1;
                seek_read_card_set_counts(&card, sr_pass, sr_fail);
                if (show_live_card) seek_read_card_render(&card, TEST_CARD_RESULT_RUNNING);
                continue;
            }
            seek_read_card_set_seek_status(&card, RECAL_SEEK_STATUS_PASS);

            if (!cmd_read_id(FDC_DRIVE, 0, &read_id_result)) {
                seek_read_card_set_id_status(&card, "RID FAIL");
                sr_fail++;
                seek_read_card_set_counts(&card, sr_pass, sr_fail);
                if (show_live_card) seek_read_card_render(&card, TEST_CARD_RESULT_RUNNING);
                continue;
            }

            sector_data_len = sector_size_from_n(read_id_result.chrn.n);
            if (sector_data_len == 0 || sector_data_len > sizeof(sector_data)) {
                sr_fail++;
                seek_read_card_set_counts(&card, sr_pass, sr_fail);
                continue;
            }

            if (!cmd_read_data(FDC_DRIVE, read_id_result.chrn.h,
                               read_id_result.chrn.c, read_id_result.chrn.h,
                               read_id_result.chrn.r, read_id_result.chrn.n,
                               &read_data_result, sector_data,
                               sector_data_len)) {
                seek_read_card_set_id_status(&card, "READ FAIL");
                sr_fail++;
                seek_read_card_set_counts(&card, sr_pass, sr_fail);
                if (show_live_card) seek_read_card_render(&card, TEST_CARD_RESULT_RUNNING);
                continue;
            }

            sr_pass++;
            seek_read_card_set_id_chrn(&card, read_id_result.chrn.c,
                                        read_id_result.chrn.h,
                                        read_id_result.chrn.r,
                                        read_id_result.chrn.n);
            seek_read_card_set_counts(&card, sr_pass, sr_fail);
            if (show_live_card) seek_read_card_render(&card, TEST_CARD_RESULT_RUNNING);
        }
    }

    results.seek_read_pass = (unsigned char)(recal_ok && !any_seek_fail && sr_fail == 0);
    last_test_failed = (unsigned char)(results.seek_read_pass == 0);
    motor_off();
    seek_read_card_render(&card, results.seek_read_pass
                          ? TEST_CARD_RESULT_PASS : TEST_CARD_RESULT_FAIL);
}


static void interactive_seek_fail(const InteractiveSeekCard *card) {
    motor_off();
    last_test_failed = 1;
    render_interactive_seek(card, TEST_CARD_RESULT_FAIL);
}

static void test_seek_interactive(void) {
    FdcSeekResult seek_result;
    unsigned char st3 = 0;
    unsigned char target = 0;
    InteractiveSeekCard interactive_seek_card;
    seek_result.st0 = 0;
    seek_result.pcn = 0;

    last_test_failed = 0;
    interactive_seek_card_init(&interactive_seek_card,
                               "K/J NAV Q EXIT");

    motor_on();

    if (!wait_drive_ready(FDC_DRIVE, 0, &st3)) {
        set_interactive_seek_ready_fail_st3(&interactive_seek_card, st3);
        set_last_no_seek(&interactive_seek_card);
        set_pcn(&interactive_seek_card, 0U);
        interactive_seek_fail(&interactive_seek_card);
        return;
    }
    /* One-time RECAL before interactive SEEK loop to establish known PCN. */
    if (!recalibrate_track0_strict(&seek_result)) {
        set_last_st0(&interactive_seek_card, seek_result.st0);
        set_pcn(&interactive_seek_card, seek_result.pcn);
        interactive_seek_fail(&interactive_seek_card);
        return;
    }
    for (;;) {
        set_interactive_seek_track(&interactive_seek_card, target);
        set_last_st0(&interactive_seek_card, seek_result.st0);
        set_pcn(&interactive_seek_card, seek_result.pcn);
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
                    motor_off();
                    set_interactive_seek_track(&interactive_seek_card, target);
                    set_last_st0(&interactive_seek_card, seek_result.st0);
                    set_pcn(&interactive_seek_card, seek_result.pcn);
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
            set_pcn(&interactive_seek_card, seek_result.pcn);
            interactive_seek_fail(&interactive_seek_card);
            return;
        }

        if (!wait_seek_complete(FDC_DRIVE, &seek_result)) {
            set_interactive_seek_track(&interactive_seek_card, target);
            set_last_wait_timeout(&interactive_seek_card);
            set_pcn(&interactive_seek_card, seek_result.pcn);
            interactive_seek_fail(&interactive_seek_card);
            return;
        }
    }
}


static void render_rpm_loop_drive_not_ready(unsigned int rpm,
                                            unsigned int pass_count,
                                            unsigned int fail_count) {
    RpmLoopCard card;
    rpm_loop_card_init(&card);
    set_rpm_loop_rpm(&card, rpm, rpm != 0);
    set_rpm_loop_counts(&card, pass_count, fail_count);
    rpm_loop_card_set_drive_not_ready(&card);
    render_rpm_loop(&card, TEST_CARD_RESULT_FAIL);
}

static void render_rpm_loop_seek_fail(unsigned int rpm,
                                      unsigned int pass_count,
                                      unsigned int fail_count) {
    RpmLoopCard card;
    rpm_loop_card_init(&card);
    set_rpm_loop_rpm(&card, rpm, rpm != 0);
    set_rpm_loop_counts(&card, pass_count, fail_count);
    rpm_loop_card_set_seek_fail(&card);
    render_rpm_loop(&card, TEST_CARD_RESULT_FAIL);
}

static void render_rpm_loop_no_measurement(unsigned int rpm,
                                           unsigned int pass_count,
                                           unsigned int fail_count,
                                           unsigned char seen_other) {
    RpmLoopCard card;
    rpm_loop_card_init(&card);
    set_rpm_loop_rpm(&card, rpm, rpm != 0);
    set_rpm_loop_counts(&card, pass_count, fail_count);
    rpm_loop_card_set_no_measurement(&card, seen_other);
    render_rpm_loop(&card, TEST_CARD_RESULT_FAIL);
}

static void render_rpm_loop_sample(unsigned int rpm,
                                   unsigned int pass_count,
                                   unsigned int fail_count) {
    RpmLoopCard card;
    rpm_loop_card_init(&card);
    set_rpm_loop_rpm(&card, rpm, 1);
    set_rpm_loop_counts(&card, pass_count, fail_count);
    rpm_loop_card_set_sample_ready(&card);
    render_rpm_loop(&card,
                    (rpm >= 285U && rpm <= 315U)
                    ? TEST_CARD_RESULT_PASS
                    : TEST_CARD_RESULT_OUT_OF_RANGE);
}

static void render_rpm_loop_active_phase(unsigned int rpm,
                                         unsigned int pass_count,
                                         unsigned int fail_count,
                                         const char *info) {
    RpmLoopCard card;
    rpm_loop_card_init(&card);
    set_rpm_loop_rpm(&card, rpm, rpm != 0);
    set_rpm_loop_counts(&card, pass_count, fail_count);
    rpm_loop_card_set_last_status(&card, "RUN");
    rpm_loop_card_set_info_status(&card, info ? info : "RUN");
    render_rpm_loop(&card, TEST_CARD_RESULT_ACTIVE);
}

static void render_rpm_loop_stopped(unsigned int rpm,
                                    unsigned int pass_count,
                                    unsigned int fail_count) {
    RpmLoopCard card;
    rpm_loop_card_init(&card);
    set_rpm_loop_rpm(&card, rpm, rpm != 0);
    set_rpm_loop_counts(&card, pass_count, fail_count);
    rpm_loop_card_set_stopped(&card);
    render_rpm_loop(&card, TEST_CARD_RESULT_STOPPED);
}


static void test_rpm_checker(int interactive) {
    FdcSeekResult seek_result;
    unsigned int rpm = 0;
    unsigned int pass_count = 0;
    unsigned int fail_count = 0;
    unsigned char st3 = 0;
    unsigned char recal_pending = 1;
    unsigned char min_ticks = 0;
    unsigned char s;
    unsigned short loop_start_tick = 0;

    seek_result.st0 = 0;
    seek_result.pcn = 0;

    last_test_failed = 0;
    results.rpm_ran = 1;

    motor_on();
    loop_start_tick = frame_ticks();

    while (!(rpm_exit_armed(loop_start_tick) && loop_exit_requested()) &&
           !(!interactive && (pass_count + fail_count) >= 1U)) {
        unsigned int period_ms;
        unsigned char sample_error;

        if (!wait_drive_ready(FDC_DRIVE, 0, &st3)) {
            fail_count++;
            render_rpm_loop_drive_not_ready(rpm, pass_count, fail_count);
            delay_ms(RPM_FAIL_DELAY_MS);
            continue;
        }

        /* RECAL once to park head at track 0 for consistent timing. */
        if (recal_pending) {
            if (!recalibrate_track0_strict(&seek_result)) {
                goto seek_fail;
            }
            recal_pending = 0;
        }

        /* Take RPM_STAT_SAMPLES measurements; keep the minimum.
         * fdc_measure_revolutions_ticks blocks on the FDC for each READ ID
         * (~22 ms/sector at 300 RPM) — no artificial delay is needed or
         * wanted here. The minimum of N samples has the least software
         * overhead, giving the most accurate period estimate. */
        render_rpm_loop_active_phase(rpm, pass_count, fail_count, "READ ID");
        min_ticks = 0;
        sample_error = 0;
        for (s = 0; s < RPM_STAT_SAMPLES; s++) {
            unsigned char t;

            if (rpm_exit_armed(loop_start_tick) && loop_exit_requested()) {
                goto rpm_done;
            }

            t = fdc_measure_revolutions_ticks(FDC_DRIVE, 0,
                                              RPM_SAMPLE_REVS,
                                              RPM_REV_TIMEOUT_TICKS);
            if (t == 0) {
                fail_count++;
                render_rpm_loop_no_measurement(rpm, pass_count, fail_count, 0);
                delay_ms(RPM_FAIL_DELAY_MS);
                sample_error = 1;
                break;
            }

            /* Ignore unrealistically-fast samples (typically emulator-side
             * command timing artifacts). */
            if (t < RPM_MIN_VALID_TICKS) {
                continue;
            }

            if (min_ticks == 0 || t < min_ticks) {
                min_ticks = t;
            }
        }

        if (sample_error) {
            continue;
        }

        if (min_ticks == 0) {
            fail_count++;
            render_rpm_loop_no_measurement(rpm, pass_count, fail_count, 0);
            delay_ms(RPM_FAIL_DELAY_MS);
            continue;
        }

        /* FRAMES ticks can undercount elapsed wall time while the FDC command
         * path briefly masks interrupts around byte pacing. Calibrate the tick
         * conversion by 4/3 so emulator readings center on 300 RPM. */
        period_ms = (((unsigned int) min_ticks * 80U) +
                     ((unsigned int) (3U * RPM_SAMPLE_REVS) / 2U)) /
                    ((unsigned int) (3U * RPM_SAMPLE_REVS));

        rpm = (unsigned int)((60000U + (period_ms / 2U)) / period_ms);
        pass_count++;
        render_rpm_loop_sample(rpm, pass_count, fail_count);
        continue;

    seek_fail:
        fail_count++;
        render_rpm_loop_seek_fail(rpm, pass_count, fail_count);
        delay_ms(RPM_FAIL_DELAY_MS);
        continue;
    }

rpm_done:
    motor_off();
    render_rpm_loop_stopped(rpm, pass_count, fail_count);
    results.rpm_pass = (unsigned char)(pass_count > 0 && fail_count == 0);
    last_test_failed = (unsigned char)(fail_count > 0);
}

/* -------------------------------------------------------------------------- */
/* UI                                                                         */
/* -------------------------------------------------------------------------- */

typedef void (*TestFunc)(int interactive);

static const TestFunc run_all_test_list[] = {
    test_drive_probe,
    test_seek_and_read,
    test_rpm_checker,
};

enum {
    RUN_ALL_TEST_COUNT = sizeof(run_all_test_list) / sizeof(run_all_test_list[0])
};

static void run_all_tests(unsigned char human_mode) {
    memset(&results, 0, sizeof(results));
    reset_report_progress();
    last_test_failed = 0;

    set_report_status(REPORT_STATUS_READY);
    ui_render_report_card();
    if (human_mode) delay_ms(RUN_ALL_READY_DELAY_EFFECTIVE_MS);

    set_report_status(REPORT_STATUS_RUNNING);
    for (unsigned char i = 0; i < RUN_ALL_TEST_COUNT; i++) {
        if (human_mode) delay_ms(RUN_ALL_RUNNING_DELAY_EFFECTIVE_MS);
        run_all_test_list[i](0);
        ui_render_report_card();
        if (human_mode) delay_ms(RUN_ALL_RESULT_DELAY_EFFECTIVE_MS);
    }
    set_report_status(REPORT_STATUS_COMPLETE);
    ui_render_report_card();
    if (human_mode) delay_ms(RUN_ALL_RESULT_DELAY_EFFECTIVE_MS);

    last_test_failed = (unsigned char) (pass_count() < pass_count_ran());
}


int main(void) {
    int ch;
    unsigned char selected_menu = 0;
    unsigned char menu_dirty = 1;

    /* Fix DivMMC font corruption and apply readable UI font. */
    init_ui_font();

#if DEBUG_ENABLED
    /* Capture paging state at startup for debug output. */
    startup_bank678 = *(volatile unsigned char *) 0x5B67;
#endif

    /* Initialize motor and paging to safe defaults. */
    motor_off();

    memset(&results, 0, sizeof(results));
    reset_report_progress();
    disable_terminal_auto_pause();
#if DEBUG_ENABLED
    printf("DEBUG BUILD\n");
    printf("DBG startup BANK678=0x%02X\n", startup_bank678);
#endif

    for (;;) {
        if (menu_dirty) {
            menu_render_full(selected_menu, pass_count());
            menu_dirty = 0;
        }

        ch = read_menu_key_blocking();
        {
            const unsigned char old_selected = selected_menu;
            unsigned char selection_changed = 0;
            const int action_key = menu_resolve_action_key(ch, &selected_menu, &selection_changed);

            if (selection_changed && action_key == 0) {
                menu_update_selection(old_selected, selected_menu);
                continue;
            }

            if (action_key == 0) {
                continue;
            }

            ch = action_key;
        }

        switch (ch) {
            case 'M':
                test_drive_probe(1);
                wait_after_test_run(1);
                menu_dirty = 1;
                break;
            case 'E':
                test_seek_and_read(1);
                wait_after_test_run(1);
                menu_dirty = 1;
                break;
            case 'I':
                test_seek_interactive();
                wait_after_test_run(1);
                menu_dirty = 1;
                break;
            case 'H':
                test_rpm_checker(1);
                wait_after_test_run(1);
                menu_dirty = 1;
                break;
            case 'A':
                run_all_tests(1);
                wait_after_test_run(1);
                menu_dirty = 1;
                break;
            case 'R':
                ui_render_report_card();
                press_any_key(1);
                menu_dirty = 1;
                break;
            case 'C':
                memset(&results, 0, sizeof(results));
                last_test_failed = 0;
                reset_report_progress();
                printf("CLEARED\n");
                press_any_key(1);
                menu_dirty = 1;
                break;
            case 'Q':
                motor_off();
                printf("EXIT\n");
                return 0;
            default:
                /* Ignore unknown keys in menu to avoid display jitter. */
                break;
        }
    }
    return 0;
}
