#include "test_cards.h"
#include "shared_strings.h"
#include "ui.h"

#include <string.h>

/* ----------------------------------------------------------------------- */
/* Row label constants — canonical, internal to this translation unit.     */
/* ----------------------------------------------------------------------- */
#define LABEL_TRACK   "TRACK : "
#define LABEL_PASS    "PASS  : "
#define LABEL_FAIL    "FAIL  : "
#define LABEL_LAST    "LAST  : "
#define LABEL_INFO    "INFO  : "
#define LABEL_RPM     "RPM   : "
#define LABEL_MOTOR   "MOTOR : "
#define LABEL_ST3     "ST3   : "
#define LABEL_ID      "ID    : "
#define LABEL_READY   "READY : "
#define LABEL_RECAL   "RECAL : "
#define LABEL_SEEK    "SEEK  : "
#define LABEL_PCN     "PCN   : "
#define LABEL_RESULT  zx3_label_result

/* Row value string constants. Shared-string externs (zx3_str_*) avoid
 * duplicate rodata across translation units; literals remain for values
 * that have no shared-string equivalent. */
#define S_READY         "READY"
#define S_RUNNING       "RUNNING"
#define S_COMPLETE      "COMPLETE"
#define S_IDLE          "IDLE"
#define S_PASS          "PASS"
#define S_FAIL          zx3_str_fail
#define S_STOPPED       zx3_str_stopped
#define S_UNKNOWN       "UNKNOWN"
#define S_WAITING       zx3_str_waiting
#define S_USER_EXIT     zx3_str_user_exit

/* ----------------------------------------------------------------------- */
/* Base TestCard operations                                                  */
/* ----------------------------------------------------------------------- */

void test_card_init(TestCard *card, const char *title, const char *controls,
                    unsigned char line_count) {
    if (line_count > TEST_CARD_MAX_LINES) {
        line_count = TEST_CARD_MAX_LINES;
    }

    card->title = title;
    card->controls = controls;
    card->line_count = line_count;
    for (unsigned char i = 0; i < TEST_CARD_MAX_LINES; i++) {
        card->text[i][0] = '\0';
        card->lines[i] = card->text[i];
    }
}

static void test_card_set_controls(TestCard *card, const char *controls) {
    card->controls = controls;
}

static void test_card_set_line(TestCard *card, unsigned char row,
                               const char *text) {
    const char *safe_text;
    if (row >= card->line_count) return;
    if (text) {
        safe_text = text;
    } else {
        safe_text = "";
    }
    strncpy(card->text[row], safe_text, (size_t)(TEST_CARD_LINE_LEN - 1U));
    card->text[row][TEST_CARD_LINE_LEN - 1U] = '\0';
}

static void test_card_set_labeled_value(TestCard *card, unsigned char row,
                                        const char *label, const char *value,
                                        const char *fallback_value) {
    const char *safe_label;
    const char *safe_value;
    unsigned char label_len = 0U;

    if (row >= card->line_count) return;

    if (label) {
        safe_label = label;
    } else {
        safe_label = "";
    }
    if (value) {
        safe_value = value;
    } else if (fallback_value) {
        safe_value = fallback_value;
    } else {
        safe_value = "";
    }

    while (safe_label[label_len] && label_len < (TEST_CARD_LINE_LEN - 1U)) {
        label_len++;
    }

    if (label_len > 0U) {
        if (card->text[row][0] == '\0') {
            memcpy(card->text[row], safe_label, label_len);
            card->text[row][label_len] = '\0';
        }
        strncpy(&card->text[row][label_len], safe_value,
                (size_t)(TEST_CARD_LINE_LEN - 1U - (unsigned int)label_len));
    }
    else {
        strncpy(card->text[row], safe_value, (size_t)(TEST_CARD_LINE_LEN - 1U));
    }
    card->text[row][TEST_CARD_LINE_LEN - 1U] = '\0';
}

static const char *test_card_line(const TestCard *card, unsigned char row) {
    if (row >= card->line_count) return "";
    return card->lines[row];
}

void test_card_render(const TestCard *card, const char *result_label,
                      const char *result_value) {
    ui_render_text_screen(card->title, card->controls, card->lines,
                          card->line_count, result_label, result_value);
}

static const char *test_card_result_text(TestCardResult result) {
    switch (result) {
        case TEST_CARD_RESULT_READY: return S_READY;
        case TEST_CARD_RESULT_RUNNING: return S_RUNNING;
        case TEST_CARD_RESULT_ACTIVE: return "ACTIVE";
        case TEST_CARD_RESULT_PASS: return S_PASS;
        case TEST_CARD_RESULT_FAIL: return S_FAIL;
        case TEST_CARD_RESULT_COMPLETE: return S_COMPLETE;
        case TEST_CARD_RESULT_STOPPED: return S_STOPPED;
        case TEST_CARD_RESULT_OUT_OF_RANGE: return zx3_str_out_of_range;
        default: return S_IDLE;
    }
}

void test_card_render_result(const TestCard *card, TestCardResult result) {
    test_card_render(card, LABEL_RESULT, test_card_result_text(result));
}

/* ----------------------------------------------------------------------- */
/* Shared helpers                                                            */
/* ----------------------------------------------------------------------- */

static const char *yes_no_text(unsigned char flag) {
    if (flag) {
        return "YES";
    }
    return "NO";
}

static const char *recal_seek_status_text(RecalSeekStatus status) {
    switch (status) {
        case RECAL_SEEK_STATUS_RUNNING: return S_RUNNING;
        case RECAL_SEEK_STATUS_PENDING: return "PENDING";
        case RECAL_SEEK_STATUS_PASS: return S_PASS;
        case RECAL_SEEK_STATUS_FAIL: return S_FAIL;
        case RECAL_SEEK_STATUS_SKIPPED: return zx3_str_skipped;
        default: return zx3_str_dash3;
    }
}

/* Shared ST3 decode: fills 4 rows (ready/wprot/track0/fault) from base_row. */
static void card_fill_st3_fields(TestCard *tc, unsigned char base_row,
                                 unsigned char have_st3, unsigned char st3) {
    static const char *const lbl[] = {LABEL_READY, "WPROT : ", "TRACK0: ", "FAULT : "};
    static const unsigned char bits[] = {0x20, 0x40, 0x10, 0x80};
    unsigned char i;
    for (i = 0; i < 4U; i++) {
        test_card_set_labeled_value(
            tc, (unsigned char)(base_row + i), lbl[i],
            yes_no_text(have_st3 && (st3 & bits[i])), "NO");
    }
}

/* ----------------------------------------------------------------------- */
/* Report card functions                                                   */
/* ----------------------------------------------------------------------- */

static const char *report_card_state_text(ReportCardState state) {
    switch (state) {
        case REPORT_CARD_STATE_PASS: return S_PASS;
        case REPORT_CARD_STATE_FAIL: return S_FAIL;
        default: return "NOT RUN";
    }
}

static const char *report_card_controls_text(ReportCardPhase phase) {
    if (phase == REPORT_CARD_PHASE_RUNNING) {
        return "AUTO ADV";
    }
    return zx3_ctrl_enter_esc_menu;
}

static const char *report_card_result_text(ReportCardPhase phase) {
    switch (phase) {
        case REPORT_CARD_PHASE_READY: return S_READY;
        case REPORT_CARD_PHASE_RUNNING: return S_RUNNING;
        case REPORT_CARD_PHASE_COMPLETE: return S_COMPLETE;
        default: return S_IDLE;
    }
}

static const char *report_card_status_text(ReportCardPhase phase) {
    switch (phase) {
        case REPORT_CARD_PHASE_READY: return S_READY;
        case REPORT_CARD_PHASE_RUNNING: return S_RUNNING;
        case REPORT_CARD_PHASE_COMPLETE: return S_COMPLETE;
        default: return "NO TESTS RUN";
    }
}

static const char *report_card_slot_label(ReportCardSlot slot) {
    static const char *labels[] = {"PROBE", "SEEK", "RPM", "OVERALL"};
    if ((unsigned char)slot >= 4U) return "PROBE";
    return labels[(unsigned char)slot];
}

static void report_card_build_row(char *out, ReportCardSlot slot,
                                  ReportCardState state) {
    snprintf(out, 32U, "%-8s%s", report_card_slot_label(slot),
             report_card_state_text(state));
}

static void report_card_build_overall_row(char *out, unsigned char total,
                                          ReportCardState state) {
    snprintf(out, 32U, "OVERALL %u/3 %s", (unsigned int)total,
             report_card_state_text(state));
}

/* Colouration for a single report-card row.  lines[0] renders at screen row 1,
 * so slot i (base line 1+i) lands at screen row 2+i. */
static void report_card_colour_row(unsigned char screen_row, const char *text,
                                   ReportCardState state) {
    unsigned char col;
    if (!text || screen_row >= 24U) return;
    for (col = 0; col < 29U; col++) {
        if (state == REPORT_CARD_STATE_PASS && strncmp(&text[col], "PASS", 4U) == 0) {
            ui_attr_set_run(screen_row, col, 4, ZX_COLOUR_BLACK, ZX_COLOUR_GREEN, 1);
            return;
        }
        if (state == REPORT_CARD_STATE_FAIL && strncmp(&text[col], "FAIL", 4U) == 0) {
            ui_attr_set_run(screen_row, col, 4, ZX_COLOUR_RED, ZX_COLOUR_YELLOW, 1);
            return;
        }
        if (state == REPORT_CARD_STATE_NOT_RUN && strncmp(&text[col], "NOT RUN", 7U) == 0) {
            ui_attr_set_run(screen_row, col, 7, ZX_COLOUR_BLUE, ZX_COLOUR_WHITE, 1);
            return;
        }
    }
}

void report_card_init(ReportCard *card) {
    unsigned char i;

    test_card_init(&card->base, "TEST REPORT CARD",
                   report_card_controls_text(REPORT_CARD_PHASE_IDLE), 5U);
    card->total_pass = 0U;
    card->phase = (unsigned char)REPORT_CARD_PHASE_IDLE;
    for (i = 0; i < 4U; i++) {
        card->slot_state[i] = (unsigned char)REPORT_CARD_STATE_NOT_RUN;
    }
}

void report_card_set_phase(ReportCard *card, ReportCardPhase phase) {
    card->phase = (unsigned char)phase;
    test_card_set_controls(&card->base, report_card_controls_text(phase));
}

void report_card_set_total_pass(ReportCard *card, unsigned char total_pass) {
    card->total_pass = total_pass;
}

void report_card_set_slot_state(ReportCard *card, ReportCardSlot slot,
                                ReportCardState state) {
    if ((unsigned char)slot >= 4U) return;
    card->slot_state[(unsigned char)slot] = (unsigned char)state;
}

void report_card_render(ReportCard *card) {
    unsigned char i;
    char row_buf[32];

    test_card_set_labeled_value(&card->base, 0U, "STATUS: ",
                                report_card_status_text((ReportCardPhase)card->phase),
                                NULL);

    for (i = 0U; i < 3U; i++) {
        report_card_build_row(row_buf, (ReportCardSlot)i,
                              (ReportCardState)card->slot_state[i]);
        test_card_set_line(&card->base, (unsigned char)(1U + i), row_buf);
    }

    report_card_build_overall_row(row_buf, card->total_pass,
                                  (ReportCardState)card->slot_state[REPORT_CARD_SLOT_OVERALL]);
    test_card_set_line(&card->base, 4U, row_buf);

    test_card_render(&card->base, LABEL_RESULT,
                     report_card_result_text((ReportCardPhase)card->phase));

    for (i = 0U; i < 3U; i++) {
        report_card_colour_row(
            (unsigned char)(2U + i),
            test_card_line(&card->base, (unsigned char)(1U + i)),
            (ReportCardState)card->slot_state[i]);
    }
    report_card_colour_row(5U,
                           test_card_line(&card->base, 4U),
                           (ReportCardState)card->slot_state[REPORT_CARD_SLOT_OVERALL]);
}

/* ----------------------------------------------------------------------- */
/* Drive probe card — MOTOR/ST3/READY/WPROT/TRACK0/FAULT/ID (7 lines)      */
/* ----------------------------------------------------------------------- */

void drive_probe_card_init(DriveProbeCard *card, const char *controls) {
    test_card_init(&card->base, "DRIVE PROBE", controls, 7U);
    test_card_set_labeled_value(&card->base, 0U, LABEL_MOTOR, "OFF", "OFF");
    test_card_set_labeled_value(&card->base, 1U, LABEL_ST3,
                                zx3_str_dash3, zx3_str_dash3);
    card_fill_st3_fields(&card->base, 2U, 0, 0);
    test_card_set_labeled_value(&card->base, 6U, LABEL_ID,
                                zx3_str_dash3, zx3_str_dash3);
}

void drive_probe_card_set_motor(DriveProbeCard *card, unsigned char on) {
    const char *motor_text;
    if (on) {
        motor_text = "ON";
    } else {
        motor_text = "OFF";
    }
    test_card_set_labeled_value(&card->base, 0U, LABEL_MOTOR, motor_text, "OFF");
}

void drive_probe_card_set_st3(DriveProbeCard *card, unsigned char have_st3,
                              unsigned char st3) {
    if (!have_st3) {
        test_card_set_labeled_value(&card->base, 1U, LABEL_ST3,
                                    zx3_str_timeout, zx3_str_timeout);
    }
    else {
        snprintf(card->base.text[1], TEST_CARD_LINE_LEN, "%s0x%02X",
                 LABEL_ST3, st3);
    }
    card_fill_st3_fields(&card->base, 2U, have_st3, st3);
}

void drive_probe_card_set_id_status(DriveProbeCard *card, const char *status) {
    test_card_set_labeled_value(&card->base, 6U, LABEL_ID,
                                status, zx3_str_dash3);
}

void drive_probe_card_set_id_chrn(DriveProbeCard *card, unsigned char c,
                                  unsigned char h, unsigned char r,
                                  unsigned char n) {
    snprintf(card->base.text[6], TEST_CARD_LINE_LEN, "%sC%u H%u R%u N%u",
             LABEL_ID, (unsigned int)c, (unsigned int)h,
             (unsigned int)r, (unsigned int)n);
}

void drive_probe_card_render(const DriveProbeCard *card, TestCardResult result) {
    test_card_render_result(&card->base, result);
}

/* ----------------------------------------------------------------------- */
/* Seek & read card — READY/RECAL/TRACK/SEEK/ID/PASS/FAIL (7 lines)        */
/* ----------------------------------------------------------------------- */

void seek_read_card_init(SeekReadCard *card, const char *controls) {
    test_card_init(&card->base, "SEEK & READ DATA", controls, 7U);
    test_card_set_labeled_value(&card->base, 0U, LABEL_READY,
                                zx3_str_dash3, zx3_str_dash3);
    test_card_set_labeled_value(&card->base, 1U, LABEL_RECAL,
                                zx3_str_dash3, zx3_str_dash3);
    snprintf(card->base.text[2], TEST_CARD_LINE_LEN, "%s--", LABEL_TRACK);
    test_card_set_labeled_value(&card->base, 3U, LABEL_SEEK,
                                zx3_str_dash3, zx3_str_dash3);
    test_card_set_labeled_value(&card->base, 4U, LABEL_ID,
                                zx3_str_dash3, zx3_str_dash3);
    snprintf(card->base.text[5], TEST_CARD_LINE_LEN, "%s  0", LABEL_PASS);
    snprintf(card->base.text[6], TEST_CARD_LINE_LEN, "%s  0", LABEL_FAIL);
}

void seek_read_card_set_ready(SeekReadCard *card, unsigned char yes) {
    const char *ready_text;
    if (yes) {
        ready_text = "YES";
    } else {
        ready_text = "NO";
    }
    test_card_set_labeled_value(&card->base, 0U, LABEL_READY,
                                ready_text, zx3_str_dash3);
}

void seek_read_card_set_ready_fail_st3(SeekReadCard *card, unsigned char st3) {
    snprintf(card->base.text[0], TEST_CARD_LINE_LEN, "%sFAIL ST3=%02X",
             LABEL_READY, st3);
}

void seek_read_card_set_recal_status(SeekReadCard *card, RecalSeekStatus status) {
    test_card_set_labeled_value(&card->base, 1U, LABEL_RECAL,
                                recal_seek_status_text(status), zx3_str_dash3);
}

void seek_read_card_set_track(SeekReadCard *card, unsigned char track) {
    snprintf(card->base.text[2], TEST_CARD_LINE_LEN, "%s%3u",
             LABEL_TRACK, (unsigned int)track);
}

void seek_read_card_set_seek_status(SeekReadCard *card, RecalSeekStatus status) {
    test_card_set_labeled_value(&card->base, 3U, LABEL_SEEK,
                                recal_seek_status_text(status), zx3_str_dash3);
}

void seek_read_card_set_id_chrn(SeekReadCard *card, unsigned char c,
                                unsigned char h, unsigned char r,
                                unsigned char n) {
    snprintf(card->base.text[4], TEST_CARD_LINE_LEN, "%sC%u H%u R%u N%u",
             LABEL_ID, (unsigned int)c, (unsigned int)h,
             (unsigned int)r, (unsigned int)n);
}

void seek_read_card_set_id_status(SeekReadCard *card, const char *status) {
    test_card_set_labeled_value(&card->base, 4U, LABEL_ID,
                                status, zx3_str_dash3);
}

void seek_read_card_set_counts(SeekReadCard *card, unsigned int pass,
                               unsigned int fail) {
    snprintf(card->base.text[5], TEST_CARD_LINE_LEN, "%s%3u", LABEL_PASS, pass);
    snprintf(card->base.text[6], TEST_CARD_LINE_LEN, "%s%3u", LABEL_FAIL, fail);
}

void seek_read_card_render(const SeekReadCard *card, TestCardResult result) {
    test_card_render_result(&card->base, result);
}

/* ----------------------------------------------------------------------- */
/* RPM loop card                                                             */
/* ----------------------------------------------------------------------- */

void rpm_loop_card_init(RpmLoopCard *card) {
    test_card_init(&card->base, "DISK RPM LOOP", "ESC/X EXIT", 5U);
    rpm_loop_card_set_last_status(card, S_WAITING);
    rpm_loop_card_set_info_status(card, S_READY);
}

void rpm_loop_card_set_rpm(RpmLoopCard *card, unsigned int rpm,
                           unsigned char rpm_valid) {
    if (rpm_valid) {
        snprintf(card->base.text[0], TEST_CARD_LINE_LEN, "%s%3u",
                 LABEL_RPM, rpm);
    }
    else {
        test_card_set_labeled_value(&card->base, 0U, LABEL_RPM,
                                    zx3_str_dash3, zx3_str_dash3);
    }
}

void rpm_loop_card_set_counts(RpmLoopCard *card, unsigned int pass_count,
                              unsigned int fail_count) {
    snprintf(card->base.text[1], TEST_CARD_LINE_LEN, "%s%3u",
             LABEL_PASS, pass_count);
    snprintf(card->base.text[2], TEST_CARD_LINE_LEN, "%s%3u",
             LABEL_FAIL, fail_count);
}

void rpm_loop_card_set_last_status(RpmLoopCard *card,
                                   const char *status_value) {
    test_card_set_labeled_value(&card->base, 3U, LABEL_LAST,
                                status_value, S_WAITING);
}

void rpm_loop_card_set_info_status(RpmLoopCard *card,
                                   const char *status_value) {
    test_card_set_labeled_value(&card->base, 4U, LABEL_INFO,
                                status_value, S_READY);
}

void rpm_loop_card_set_drive_not_ready(RpmLoopCard *card) {
    rpm_loop_card_set_last_status(card, zx3_str_not_ready);
    rpm_loop_card_set_info_status(card, zx3_str_check_media);
}

void rpm_loop_card_set_seek_fail(RpmLoopCard *card) {
    rpm_loop_card_set_last_status(card, "T0 SEEK FAIL");
    rpm_loop_card_set_info_status(card, "ST0 SET");
}

void rpm_loop_card_set_no_measurement(RpmLoopCard *card,
                                      unsigned char seen_other) {
    const char *info_text;
    rpm_loop_card_set_last_status(card, "RPM N/A");
    if (seen_other) {
        info_text = "NO MARK";
    } else {
        info_text = "SAME SEC";
    }
    rpm_loop_card_set_info_status(card, info_text);
}

void rpm_loop_card_set_sample_ready(RpmLoopCard *card) {
    rpm_loop_card_set_last_status(card, "SAMPLE OK");
    rpm_loop_card_set_info_status(card, "PERIOD OK");
}

void rpm_loop_card_set_stopped(RpmLoopCard *card) {
    rpm_loop_card_set_last_status(card, S_STOPPED);
    rpm_loop_card_set_info_status(card, S_USER_EXIT);
}

void rpm_loop_card_render(const RpmLoopCard *card, TestCardResult result) {
    test_card_render_result(&card->base, result);
}

/* ----------------------------------------------------------------------- */
/* Interactive seek card                                                     */
/* ----------------------------------------------------------------------- */

void interactive_seek_card_init(InteractiveSeekCard *card,
                                const char *controls) {
    test_card_init(&card->base, "STEP SEEK", controls, 3U);
}

void interactive_seek_card_set_ready_fail(InteractiveSeekCard *card,
                                          unsigned char st3) {
    snprintf(card->base.text[0], TEST_CARD_LINE_LEN, "%sFAIL ST3=%02X",
             LABEL_READY, st3);
}

void interactive_seek_card_set_track(InteractiveSeekCard *card,
                                     unsigned char track) {
    snprintf(card->base.text[0], TEST_CARD_LINE_LEN, "%s%u",
             LABEL_TRACK, (unsigned int)track);
}

void interactive_seek_card_set_last_st0(InteractiveSeekCard *card,
                                        unsigned char st0) {
    snprintf(card->base.text[1], TEST_CARD_LINE_LEN, "%sST0=%02X",
             LABEL_LAST, st0);
}

void interactive_seek_card_set_last_status(InteractiveSeekCard *card,
                                           const char *status_value) {
    test_card_set_labeled_value(&card->base, 1U, LABEL_LAST,
                                status_value, zx3_str_dash3);
}

void interactive_seek_card_set_pcn(InteractiveSeekCard *card,
                                   unsigned char pcn) {
    snprintf(card->base.text[2], TEST_CARD_LINE_LEN, "%s%u",
             LABEL_PCN, (unsigned int)pcn);
}

void interactive_seek_card_render(const InteractiveSeekCard *card,
                                  TestCardResult result) {
    test_card_render_result(&card->base, result);
}
