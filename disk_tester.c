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


#ifndef IOCTL_OTERM_PAUSE
#define IOCTL_OTERM_PAUSE 0xC042
#endif

#define READ_LOOP_PAUSE_STEPS 2U
#define READ_LOOP_PAUSE_MS    25U
#define TEST_CARD_STATE_DELAY_MS 190U
#define RUN_ALL_READY_DELAY_MS 150U
#define RUN_ALL_RUNNING_DELAY_MS 120U
#define RUN_ALL_RESULT_DELAY_MS 220U

#define RPM_LOOP_DELAY_MS 180U
#define RPM_FAIL_DELAY_MS 450U
#define RPM_EXIT_ARM_DELAY_MS 400U

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
    return (unsigned char) (results.motor_test_pass + results.sense_drive_pass +
                            results.recalibrate_pass + results.seek_pass +
                            results.read_id_pass);
}

static unsigned char pass_count_ran(void) {
    return (unsigned char) (results.motor_test_ran + results.sense_drive_ran +
                            results.recalibrate_ran + results.seek_ran +
                            results.read_id_ran);
}

static void reset_report_progress(void) {
    report_status_code = REPORT_STATUS_NONE;
}

static void set_report_status(unsigned char status_code) {
    report_status_code = status_code;
}


static unsigned char any_report_test_ran(void) {
    return (unsigned char) (results.motor_test_ran || results.sense_drive_ran ||
                            results.recalibrate_ran || results.seek_ran ||
                            results.read_id_ran);
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

/* Seek/recalibrate completion is signalled by SE (bit 5) with a normal
 * interrupt code in ST0 (IC bits 7-6 == 00).  PCN is useful for diagnostics
 * but can be inconsistent on some setups, so pass/fail uses ST0 completion. */
static unsigned char seek_completion_ok(unsigned char st0) {
    return (unsigned char) (((st0 & 0x20U) != 0U) && ((st0 & 0xC0U) == 0U));
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
    ReportCardState last_state;
    ReportCard card;

    report_card_init(&card);
    report_card_set_phase(&card, report_card_phase_from_status(report_status_code));
    report_card_set_total_pass(&card, total);

    if (!any_report_test_ran()) {
        last_state = REPORT_CARD_STATE_NOT_RUN;
    }
    else {
        last_state =
                last_test_failed ? REPORT_CARD_STATE_FAIL : REPORT_CARD_STATE_PASS;
    }

    ReportCardState motor_state = report_card_state_from_runpass(results.motor_test_ran, results.motor_test_pass);
    ReportCardState drive_state = report_card_state_from_runpass(results.sense_drive_ran,
                                                                 results.sense_drive_pass);
    ReportCardState recal_state = report_card_state_from_runpass(results.recalibrate_ran,
                                                                 results.recalibrate_pass);
    ReportCardState seek_state = report_card_state_from_runpass(results.seek_ran, results.seek_pass);
    ReportCardState readid_state = report_card_state_from_runpass(results.read_id_ran, results.read_id_pass);
    ReportCardState overall_state;
    if (!any_report_test_ran()) {
        overall_state = REPORT_CARD_STATE_NOT_RUN;
    }
    else if (total == pass_count_ran()) {
        overall_state = REPORT_CARD_STATE_PASS;
    }
    else {
        overall_state = REPORT_CARD_STATE_FAIL;
    }

    report_card_set_slot_state(&card, REPORT_CARD_SLOT_LAST, last_state);
    report_card_set_slot_state(&card, REPORT_CARD_SLOT_MOTOR, motor_state);
    report_card_set_slot_state(&card, REPORT_CARD_SLOT_DRIVE, drive_state);
    report_card_set_slot_state(&card, REPORT_CARD_SLOT_RECAL, recal_state);
    report_card_set_slot_state(&card, REPORT_CARD_SLOT_SEEK, seek_state);
    report_card_set_slot_state(&card, REPORT_CARD_SLOT_READID, readid_state);
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
    {0xBFFE, 0x08, 'J'},
    {0xBFFE, 0x04, 'K'},
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
        printf("\nPRESS ENTER OR ESC/X\n");
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

static void test_motor_and_drive_status(int interactive) {
    unsigned char status_3 = 0;
    unsigned char have_st3 = 0;
    unsigned char show_live_card = show_selected_test_cards();
    MotorDriveCard motor_drive_card;

    last_test_failed = 0;
    results.motor_test_ran = 1;
    results.sense_drive_ran = 1;
    motor_drive_card_init(&motor_drive_card, single_shot_test_controls(interactive));

    if (show_live_card) {
        reset_motor_drive(&motor_drive_card);
        render_motor_drive(&motor_drive_card, TEST_CARD_RESULT_READY);
        delay_ms(TEST_CARD_STATE_DELAY_MS);
        set_card_motor_on(&motor_drive_card);
        render_motor_drive(&motor_drive_card, TEST_CARD_RESULT_RUNNING);
    }

    plus3_motor_on();

    have_st3 = cmd_sense_drive_status(FDC_DRIVE, 0, &status_3);
    set_motor_drive_status(&motor_drive_card, have_st3, status_3);
    results.sense_drive_pass = have_st3 ? 1U : 0U;

    plus3_motor_off();
    results.motor_test_pass = have_st3;
    last_test_failed = (unsigned char) (results.sense_drive_pass == 0);
    set_card_motor_off(&motor_drive_card);
    render_motor_drive(&motor_drive_card, results.sense_drive_pass
                       ? TEST_CARD_RESULT_PASS
                       : TEST_CARD_RESULT_FAIL);
}

static void test_read_id_probe(const char interactive) {
    unsigned char status_3 = 0;
    FdcSeekResult seek_result;
    unsigned char rid_ok = 0;
    FdcResult rid_result;
    unsigned char have_st3 = 0;
    unsigned char show_live_card = show_selected_test_cards();
    ReadIdProbeCard read_id_probe_card;

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
    read_id_probe_card_init(&read_id_probe_card,
                            single_shot_test_controls(interactive));

    if (show_live_card) {
        reset_read_id_probe(&read_id_probe_card);
        render_read_id_probe(&read_id_probe_card, TEST_CARD_RESULT_READY);
        delay_ms(TEST_CARD_STATE_DELAY_MS);
        set_id_probing(&read_id_probe_card);
        render_read_id_probe(&read_id_probe_card, TEST_CARD_RESULT_RUNNING);
    }

    plus3_motor_on();

    /* 1) Raw drive lines (ST3), informational */
    have_st3 = cmd_sense_drive_status(FDC_DRIVE, 0, &status_3);
    set_read_id_probe_status(&read_id_probe_card, have_st3, status_3);

    /* 2) A command that tends to surface "not ready" in ST0 (and steps hardware)
     */
    if (cmd_recalibrate(FDC_DRIVE) && wait_seek_complete(FDC_DRIVE, &seek_result)) {
        set_precheck_ok(&read_id_probe_card);
    }
    else {
        set_precheck_fail(&read_id_probe_card);
    }

    /* 3) Media probe: Read ID, best indicator for both hardware and emulation */
    rid_ok = cmd_read_id(FDC_DRIVE, 0, &rid_result);

    /*
     * Probe is informational — result is shown on its own card only.
     * Do NOT write to results.read_id_ran/pass; that slot belongs to the
     * dedicated test_read_id function and writing here would cause the
     * report card READID slot to show FAIL mid-run when the real test
     * hasn't executed yet.
     */
    last_test_failed = (unsigned char) (rid_ok == 0);
    plus3_motor_off();
    if (rid_ok) {
        set_id_chrn(&read_id_probe_card,
                    rid_result.chrn.c,
                    rid_result.chrn.h,
                    rid_result.chrn.r,
                    rid_result.chrn.n
        );
    }
    else {
        set_id_failure(&read_id_probe_card,
                       read_id_failure_reason(
                           rid_result.status.st1,
                           rid_result.status.st2
                       )
        );
    }
    render_read_id_probe(&read_id_probe_card, rid_ok
                         ? TEST_CARD_RESULT_PASS
                         : TEST_CARD_RESULT_FAIL);
}

static void test_recal_seek_track2(int interactive) {
    unsigned char st3 = 0;
    unsigned char track_target = 2;
    unsigned char recal_ok = 0;
    FdcSeekResult seek_result;
    unsigned char show_live_card = show_selected_test_cards();
    RecalSeekCard recal_seek_card;
    (void) interactive;

    seek_result.st0 = 0;
    seek_result.pcn = 0;

    last_test_failed = 0;
    results.recalibrate_ran = 1;
    results.seek_ran = 1;
    recal_seek_card_init(&recal_seek_card, single_shot_test_controls(interactive));

    if (show_live_card) {
        reset_recal_seek(&recal_seek_card);
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
    if (!cmd_recalibrate(FDC_DRIVE)) {
        set_recal_status_fail(&recal_seek_card);
        set_seek_status_skipped(&recal_seek_card);
        set_detail_recal_cmd_fail(&recal_seek_card);
        results.recalibrate_pass = 0;
        results.seek_pass = 0;
        last_test_failed = 1;
        plus3_motor_off();
        render_recal_seek(&recal_seek_card, TEST_CARD_RESULT_FAIL);
        return;
    }
    if (!wait_seek_complete(FDC_DRIVE, &seek_result)) {
        set_recal_status_fail(&recal_seek_card);
        set_seek_status_skipped(&recal_seek_card);
        set_detail_st0_pcn(&recal_seek_card, seek_result.st0, seek_result.pcn);
        results.recalibrate_pass = 0;
        results.seek_pass = 0;
        last_test_failed = 1;
        plus3_motor_off();
        render_recal_seek(&recal_seek_card, TEST_CARD_RESULT_FAIL);
        return;
    }
    recal_ok = seek_completion_ok(seek_result.st0);

    if (recal_ok) {
        set_recal_status_pass(&recal_seek_card);
    }
    else {
        set_recal_status_fail(&recal_seek_card);
    }

    if (!cmd_seek(FDC_DRIVE, 0, track_target)) {
        set_ready_yes(&recal_seek_card);
        set_seek_status_fail(&recal_seek_card);
        set_detail_seek_cmd_fail(&recal_seek_card);
        results.recalibrate_pass = recal_ok;
        results.seek_pass = 0;
        last_test_failed = 1;
        plus3_motor_off();
        render_recal_seek(&recal_seek_card, TEST_CARD_RESULT_FAIL);
        return;
    }
    if (!wait_seek_complete(FDC_DRIVE, &seek_result)) {
        set_ready_yes(&recal_seek_card);
        set_seek_status_fail(&recal_seek_card);
        set_detail_st0_pcn(&recal_seek_card, seek_result.st0, seek_result.pcn);
        results.recalibrate_pass = recal_ok;
        results.seek_pass = 0;
        last_test_failed = 1;
        plus3_motor_off();
        render_recal_seek(&recal_seek_card, TEST_CARD_RESULT_FAIL);
        return;
    }

    results.recalibrate_pass = recal_ok;
    results.seek_pass = seek_completion_ok(seek_result.st0);
    plus3_motor_off();
    last_test_failed =
            (unsigned char) !(results.recalibrate_pass && results.seek_pass);
    if (results.recalibrate_pass) {
        set_recal_status_pass(&recal_seek_card);
    }
    else {
        set_recal_status_fail(&recal_seek_card);
    }
    if (results.seek_pass) {
        set_seek_status_pass(&recal_seek_card);
    }
    else {
        set_seek_status_fail(&recal_seek_card);
    }
    set_detail_track(&recal_seek_card, seek_result.pcn);
    render_recal_seek(&recal_seek_card, (results.recalibrate_pass && results.seek_pass)
                      ? TEST_CARD_RESULT_PASS
                      : TEST_CARD_RESULT_FAIL);
}

static void interactive_seek_fail(const InteractiveSeekCard *card) {
    plus3_motor_off();
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
                               "K UP  J DOWN  Q EXIT");

    plus3_motor_on();

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
                    plus3_motor_off();
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

static void test_read_id(int interactive) {
    FdcResult rid_result;
    unsigned char st3 = 0;
    unsigned char show_live_card = show_selected_test_cards();
    ReadIdCard read_id_card;
    (void) interactive;

    last_test_failed = 0;
    results.read_id_ran = 1;
    read_id_card_init(&read_id_card, single_shot_test_controls(0));

    if (show_live_card) {
        reset_read_id(&read_id_card);
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

    /* Try to get to track 0 first — abort if RECAL fails */
    {
        FdcSeekResult recal_result;
        if (!recalibrate_track0_strict(&recal_result)) {
            set_detail_failure(&read_id_card, "RECAL FAILED");
            results.read_id_pass = 0;
            last_test_failed = 1;
            plus3_motor_off();
            render_read_id(&read_id_card, TEST_CARD_RESULT_FAIL);
            return;
        }
    }

    unsigned char ok = cmd_read_id(FDC_DRIVE, 0, &rid_result);

    set_status(&read_id_card, rid_result.status.st0, rid_result.status.st1,
               rid_result.status.st2);
    if (ok) {
        set_chrn_valid(&read_id_card, rid_result.chrn.c, rid_result.chrn.h,
                       rid_result.chrn.r, rid_result.chrn.n);
        set_detail_id_ok(&read_id_card);
    }
    else {
        set_chrn_invalid(&read_id_card);
        set_detail_failure(&read_id_card,
                           read_id_failure_reason(rid_result.status.st1,
                               rid_result.status.st2));
    }

    results.read_id_pass = ok;
    last_test_failed = (unsigned char) (ok == 0);
    plus3_motor_off();
    render_read_id(&read_id_card,
                   ok ? TEST_CARD_RESULT_PASS : TEST_CARD_RESULT_FAIL);
}

static void render_track_loop_active(unsigned char track,
                                     unsigned int pass_count,
                                     unsigned int fail_count) {
    TrackLoopCard card;
    track_loop_card_init(&card);
    set_track_loop_track(&card, track);
    set_track_loop_counts(&card, pass_count, fail_count);
    track_loop_card_set_active(&card);
    render_track_loop(&card, TEST_CARD_RESULT_ACTIVE);
}

static void render_track_loop_drive_not_ready(unsigned char track,
                                              unsigned int pass_count,
                                              unsigned int fail_count,
                                              unsigned char st3) {
    TrackLoopCard card;
    track_loop_card_init(&card);
    set_track_loop_track(&card, track);
    set_track_loop_counts(&card, pass_count, fail_count);
    track_loop_card_set_drive_not_ready(&card, st3);
    render_track_loop(&card, TEST_CARD_RESULT_FAIL);
}

static void render_track_loop_seek_fail(unsigned char track,
                                        unsigned int pass_count,
                                        unsigned int fail_count,
                                        unsigned char st0) {
    TrackLoopCard card;
    track_loop_card_init(&card);
    set_track_loop_track(&card, track);
    set_track_loop_counts(&card, pass_count, fail_count);
    track_loop_card_set_seek_fail(&card, track, st0);
    render_track_loop(&card, TEST_CARD_RESULT_FAIL);
}

static void render_track_loop_read_id_fail(unsigned char track,
                                           unsigned int pass_count,
                                           unsigned int fail_count,
                                           const char *reason) {
    TrackLoopCard card;
    track_loop_card_init(&card);
    set_track_loop_track(&card, track);
    set_track_loop_counts(&card, pass_count, fail_count);
    track_loop_card_set_read_id_fail(&card, track, reason);
    render_track_loop(&card, TEST_CARD_RESULT_FAIL);
}

static void render_track_loop_bad_sector_size(unsigned char track,
                                              unsigned int pass_count,
                                              unsigned int fail_count,
                                              unsigned char size_code) {
    TrackLoopCard card;
    track_loop_card_init(&card);
    set_track_loop_track(&card, track);
    set_track_loop_counts(&card, pass_count, fail_count);
    track_loop_card_set_bad_sector_size(&card, size_code);
    render_track_loop(&card, TEST_CARD_RESULT_FAIL);
}

static void render_track_loop_read_fail(unsigned char track,
                                        unsigned int pass_count,
                                        unsigned int fail_count,
                                        const char *reason) {
    TrackLoopCard card;
    track_loop_card_init(&card);
    set_track_loop_track(&card, track);
    set_track_loop_counts(&card, pass_count, fail_count);
    track_loop_card_set_read_fail(&card, track, reason);
    render_track_loop(&card, TEST_CARD_RESULT_FAIL);
}

static void render_track_loop_stopped(unsigned char track,
                                      unsigned int pass_count,
                                      unsigned int fail_count) {
    TrackLoopCard card;
    track_loop_card_init(&card);
    set_track_loop_track(&card, track);
    set_track_loop_counts(&card, pass_count, fail_count);
    track_loop_card_set_stopped(&card);
    render_track_loop(&card, TEST_CARD_RESULT_STOPPED);
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

static void render_rpm_loop_id_fail(unsigned int rpm,
                                    unsigned int pass_count,
                                    unsigned int fail_count,
                                    const char *reason) {
    RpmLoopCard card;
    rpm_loop_card_init(&card);
    set_rpm_loop_rpm(&card, rpm, rpm != 0);
    set_rpm_loop_counts(&card, pass_count, fail_count);
    rpm_loop_card_set_id_fail(&card, reason);
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

static void render_rpm_loop_period_bad(unsigned int rpm,
                                       unsigned int pass_count,
                                       unsigned int fail_count) {
    RpmLoopCard card;
    rpm_loop_card_init(&card);
    set_rpm_loop_rpm(&card, rpm, rpm != 0);
    set_rpm_loop_counts(&card, pass_count, fail_count);
    rpm_loop_card_set_period_bad(&card);
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

static void test_read_track_data_loop(void) {
    static unsigned char sector_data[1024];
    FdcResult read_id_result;
    FdcResult read_data_result;
    FdcSeekResult seek_result;
    unsigned char current_track = 0;
    unsigned char seek_required = 1;
    unsigned char recal_required = 1;
    unsigned char drive_status_st3 = 0;
    unsigned int pass_count = 0;
    unsigned int fail_count = 0;
    unsigned int sector_data_len;
    unsigned char ui_redraw_required = 1;

    seek_result.st0 = 0;
    seek_result.pcn = 0;

    last_test_failed = 0;
    memset(runtime_key_latched, 0, sizeof(runtime_key_latched));
    runtime_pending_key = 0;
    disk_operations_set_idle_pump(pump_runtime_key_latch);

    plus3_motor_on();

    for (;;) {
        unsigned char seek_fail_st0 = 0;

#if HEADLESS_ROM_FONT
        if (pass_count + fail_count >= 3U) break;
#endif
        if (track_loop_consume_action(&current_track, &seek_required,
                                      &ui_redraw_required)) {
            break;
        }

        if (ui_redraw_required) {
            render_track_loop_active(current_track, pass_count, fail_count);
            ui_redraw_required = 0;
        }

        if (!wait_drive_ready(FDC_DRIVE, 0, &drive_status_st3)) {
            fail_count++;
            render_track_loop_drive_not_ready(current_track, pass_count,
                                              fail_count, drive_status_st3);
            seek_required = 1;
            recal_required = 1;
            ui_redraw_required = 0;
            delay_ms(20);
            continue;
        }

        if (seek_required) {
            if (recal_required) {
                FdcSeekResult recal_result;
                if (!recalibrate_track0_strict(&recal_result)) {
                    seek_fail_st0 = recal_result.st0;
                    goto track_seek_fail;
                }
                recal_required = 0;
            }
            if (!cmd_seek(FDC_DRIVE, 0, current_track) ||
                !wait_seek_complete(FDC_DRIVE, &seek_result)) {
                seek_fail_st0 = seek_result.st0;
                goto track_seek_fail;
            }
            seek_required = 0;
            ui_redraw_required = 1;
            /* Moved to a different track — old hex dump data is stale. */
            ui_reset_hex_dump_panel();
        }

        if (!cmd_read_id(FDC_DRIVE, 0, &read_id_result)) {
            fail_count++;
            render_track_loop_read_id_fail(
                current_track, pass_count, fail_count,
                read_id_failure_reason(read_id_result.status.st1,
                                       read_id_result.status.st2));
            goto track_retry_fail;
        }

        sector_data_len = sector_size_from_n(read_id_result.chrn.n);
        if (sector_data_len == 0 || sector_data_len > sizeof(sector_data)) {
            fail_count++;
            render_track_loop_bad_sector_size(current_track, pass_count,
                                              fail_count, read_id_result.chrn.n);
            goto track_retry_fail;
        }

        if (!cmd_read_data(FDC_DRIVE, read_id_result.chrn.h,
                           read_id_result.chrn.c, read_id_result.chrn.h,
                           read_id_result.chrn.r, read_id_result.chrn.n,
                           &read_data_result, sector_data,
                           sector_data_len)) {
            fail_count++;
            render_track_loop_read_fail(
                current_track, pass_count, fail_count,
                read_id_failure_reason(read_data_result.status.st1,
                                       read_data_result.status.st2));
            goto track_retry_fail;
        }

        pass_count++;

        /*
         * Update the card with the new pass count, then blit the sector data
         * below the card as a hex+ASCII dump.  The dump is skipped by the
         * renderer if the sector content hasn't changed (checksum dirty check).
         * ui_redraw_required is cleared here since we just rendered the card.
         */
        render_track_loop_active(current_track, pass_count, fail_count);
        ui_render_hex_dump_panel(sector_data, sector_data_len);
        ui_redraw_required = 0;

        /* Short pause: track_loop_consume_action at the next iteration
         * will catch any pending J/K/X key after the delay. */
        delay_ms((unsigned int)(READ_LOOP_PAUSE_STEPS * READ_LOOP_PAUSE_MS));
        continue;

track_seek_fail:
        fail_count++;
        render_track_loop_seek_fail(current_track, pass_count,
                                    fail_count, seek_fail_st0);
track_retry_fail:
        seek_required = 1;
        ui_redraw_required = 0;
        delay_ms(20);
        continue;
    }

    plus3_motor_off();
    disk_operations_set_idle_pump(0);
    render_track_loop_stopped(current_track, pass_count, fail_count);
    last_test_failed = (unsigned char) (fail_count > 0);
}

static void test_rpm_checker(void) {
    FdcSeekResult seek_result;
    FdcResult rid_result;
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
    unsigned char exit_now = 0;
    unsigned char recal_pending = 1;
    unsigned short loop_start_tick = 0;

    seek_result.st0 = 0;
    seek_result.pcn = 0;

    last_test_failed = 0;

    plus3_motor_on();
    loop_start_tick = frame_ticks();

    while (!(rpm_exit_armed(loop_start_tick) && loop_exit_requested())) {
#if HEADLESS_ROM_FONT
        if (pass_count + fail_count >= 3U) break;
#endif

        if (!wait_drive_ready(FDC_DRIVE, 0, &st3)) {
            fail_count++;
            render_rpm_loop_drive_not_ready(rpm, pass_count, fail_count);
            delay_ms(RPM_FAIL_DELAY_MS);
            continue;
        }

        /* Run RECAL once; if it fails, retry next loop until one success. */
        if (recal_pending) {
            if (!recalibrate_track0_strict(&seek_result)) {
                goto seek_fail;
            }
            recal_pending = 0;
        }

        if (!cmd_seek(FDC_DRIVE, 0, 0) ||
            !wait_seek_complete(FDC_DRIVE, &seek_result)) {
            goto seek_fail;
        }

        if (!cmd_read_id(FDC_DRIVE, 0, &rid_result)) {
            fail_count++;
            render_rpm_loop_id_fail(rpm, pass_count, fail_count,
                                    read_id_failure_reason(rid_result.status.st1,
                                                           rid_result.status.st2));
            delay_ms(RPM_FAIL_DELAY_MS);
            continue;
        }

        first_r = rid_result.chrn.r;
        start_tick = frame_ticks();
        seen_other = 0;
        dticks = 0;
        exit_now = 0;

        for (unsigned char i = 0; i < 120; i++) {
            if (rpm_exit_armed(loop_start_tick) && loop_exit_requested()) {
                exit_now = 1;
                break;
            }
            if (!cmd_read_id(FDC_DRIVE, 0, &rid_result)) {
                break;
            }
            if (rid_result.chrn.r != first_r) {
                seen_other = 1;
            }
            else if (seen_other) {
                end_tick = frame_ticks();
                dticks = (unsigned short) (end_tick - start_tick);
                break;
            }
            delay_ms(2);
            if ((unsigned short) (frame_ticks() - start_tick) > 50U) {
                break;
            }
        }

        if (exit_now) {
            break;
        }

        if (dticks == 0) {
            fail_count++;
            render_rpm_loop_no_measurement(rpm, pass_count, fail_count,
                                           seen_other);
            delay_ms(RPM_FAIL_DELAY_MS);
            continue;
        }

        period_ms = (unsigned int) dticks * 20U;
        if (period_ms == 0) {
            fail_count++;
            render_rpm_loop_period_bad(rpm, pass_count, fail_count);
            delay_ms(RPM_FAIL_DELAY_MS);
            continue;
        }

        rpm = (unsigned int) ((60000U + (period_ms / 2U)) / period_ms);
        pass_count++;
        render_rpm_loop_sample(rpm, pass_count, fail_count);
        delay_ms(RPM_LOOP_DELAY_MS);
        continue;

seek_fail:
        fail_count++;
        render_rpm_loop_seek_fail(rpm, pass_count, fail_count);
        delay_ms(RPM_FAIL_DELAY_MS);
        continue;
    }

    plus3_motor_off();
    render_rpm_loop_stopped(rpm, pass_count, fail_count);
    last_test_failed = (unsigned char) (fail_count > 0);
}

/* -------------------------------------------------------------------------- */
/* UI                                                                         */
/* -------------------------------------------------------------------------- */

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
    memset(&results, 0, sizeof(results));
    reset_report_progress();
    last_test_failed = 0;

    set_report_status(REPORT_STATUS_READY);
    ui_render_report_card();
    if (human_mode) delay_ms(RUN_ALL_READY_DELAY_EFFECTIVE_MS);

    for (unsigned char i = 0; i < RUN_ALL_TEST_COUNT; i++) {
        set_report_status(REPORT_STATUS_RUNNING);
        if (human_mode) delay_ms(RUN_ALL_RUNNING_DELAY_EFFECTIVE_MS);
        run_all_test_list[i](0);
        set_report_status(REPORT_STATUS_COMPLETE);
        ui_render_report_card();
        if (human_mode) delay_ms(RUN_ALL_RESULT_DELAY_EFFECTIVE_MS);
    }

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
    plus3_motor_off();

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
                ui_render_report_card();
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
