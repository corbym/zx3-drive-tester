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
#define LABEL_DETAIL  "DETAIL: "
#define LABEL_MEDIA   "MEDIA : "
#define LABEL_STS     "STS   : "
#define LABEL_CHRN    "CHRN  : "
#define LABEL_PCN     "PCN   : "
#define LABEL_RESULT  zx3_label_result

/* Row value string constants — #define lets the optimiser share literals. */
#define S_READY         "READY"
#define S_RUNNING       "RUNNING"
#define S_COMPLETE      "COMPLETE"
#define S_IDLE          "IDLE"
#define S_PASS          "PASS"
#define S_FAIL          "FAIL"
#define S_STOPPED       "STOPPED"
#define S_INVALID       "INVALID"
#define S_UNKNOWN       "UNKNOWN"
#define S_READABLE_DISK "READABLE DISK REQUIRED"

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
    if (row >= card->line_count) return;
    strncpy(card->text[row], text ? text : "", (size_t)(TEST_CARD_LINE_LEN - 1U));
    card->text[row][TEST_CARD_LINE_LEN - 1U] = '\0';
}

static void test_card_set_labeled_value(TestCard *card, unsigned char row,
                                        const char *label, const char *value,
                                        const char *fallback_value) {
    const char *safe_label = label ? label : "";
    const char *safe_value = value ? value : (fallback_value ? fallback_value : "");
    unsigned char label_len = 0U;

    if (row >= card->line_count) return;

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
    return flag ? "YES" : "NO";
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
            tc, (unsigned char) (base_row + i), lbl[i],
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
    return (phase == REPORT_CARD_PHASE_RUNNING)
               ? "AUTO ADVANCE"
               : "ENTER/ESC/X MENU";
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
    static const char *labels[] = {
        "LAST", "MOTOR", "DRIVE", "RECAL",
        "SEEK", "READID", "OVERALL"
    };
    if ((unsigned char) slot > (unsigned char) REPORT_CARD_SLOT_OVERALL) {
        return "LAST";
    }
    return labels[(unsigned char) slot];
}

static void report_card_build_row(char *out, ReportCardSlot slot,
                                  ReportCardState state) {
    snprintf(out, 32U, "%-8s%s", report_card_slot_label(slot),
             report_card_state_text(state));
}

static void report_card_build_overall_row(char *out, unsigned char total,
                                          ReportCardState state) {
    snprintf(out, 32U, "OVERALL %u/5 %s", (unsigned int) total,
             report_card_state_text(state));
}

/* Colouration for a single report-card row.  Screen row 2 = lines[0], so
 * slot i (base line 1+i) lands at screen row 3+i. */
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
                   report_card_controls_text(REPORT_CARD_PHASE_IDLE), 8U);
    card->total_pass = 0U;
    card->phase = (unsigned char) REPORT_CARD_PHASE_IDLE;
    for (i = 0; i <= (unsigned char) REPORT_CARD_SLOT_OVERALL; i++) {
        card->slot_state[i] = (unsigned char) REPORT_CARD_STATE_NOT_RUN;
    }
}

void report_card_set_phase(ReportCard *card, ReportCardPhase phase) {
    card->phase = (unsigned char) phase;
    test_card_set_controls(&card->base, report_card_controls_text(phase));
}

void report_card_set_total_pass(ReportCard *card, unsigned char total_pass) {
    card->total_pass = total_pass;
}

void report_card_set_slot_state(ReportCard *card, ReportCardSlot slot,
                                ReportCardState state) {
    if ((unsigned char) slot > (unsigned char) REPORT_CARD_SLOT_OVERALL) return;
    card->slot_state[(unsigned char) slot] = (unsigned char) state;
}

void report_card_render(ReportCard *card) {
    unsigned char i;
    char row_buf[32];

    test_card_set_labeled_value(&card->base, 0U, "STATUS: ",
                                report_card_status_text((ReportCardPhase) card->phase),
                                NULL);

    for (i = (unsigned char) REPORT_CARD_SLOT_LAST;
         i <= (unsigned char) REPORT_CARD_SLOT_READID; i++) {
        report_card_build_row(row_buf, (ReportCardSlot) i,
                              (ReportCardState) card->slot_state[i]);
        test_card_set_line(&card->base, (unsigned char) (1U + i), row_buf);
    }

    report_card_build_overall_row(row_buf, card->total_pass,
                                  (ReportCardState) card->slot_state[REPORT_CARD_SLOT_OVERALL]);
    test_card_set_line(&card->base, (unsigned char) (1U + REPORT_CARD_SLOT_OVERALL),
                       row_buf);

    test_card_render(&card->base, LABEL_RESULT,
                     report_card_result_text((ReportCardPhase) card->phase));

    /* Apply state-based colouration for every slot row.
     * ui_render_text_screen places lines[0] at screen row 2, so slot i
     * (base line 1+i) lands at screen row 3+i. */
    for (i = 0; i <= (unsigned char) REPORT_CARD_SLOT_OVERALL; i++) {
        report_card_colour_row(
            (unsigned char) (3U + i),
            test_card_line(&card->base, (unsigned char) (1U + i)),
            (ReportCardState) card->slot_state[i]);
    }
}

/* ----------------------------------------------------------------------- */
/* Track loop card                                                           */
/* ----------------------------------------------------------------------- */

void track_loop_card_init(TrackLoopCard *card) {
    test_card_init(&card->base, "READ TRACK DATA LOOP",
                   "J/K TRACK  ENTER/X EXIT", 5U);
    track_loop_card_set_last_status(card, "OK");
    track_loop_card_set_info_status(card, S_READY);
}

void track_loop_card_set_track(TrackLoopCard *card, unsigned char track) {
    snprintf(card->base.text[0], TEST_CARD_LINE_LEN, "%s%3u",
             LABEL_TRACK, (unsigned int) track);
}

void track_loop_card_set_counts(TrackLoopCard *card, unsigned int pass_count,
                                unsigned int fail_count) {
    snprintf(card->base.text[1], TEST_CARD_LINE_LEN, "%s%3u",
             LABEL_PASS, pass_count);
    snprintf(card->base.text[2], TEST_CARD_LINE_LEN, "%s%3u",
             LABEL_FAIL, fail_count);
}

void track_loop_card_set_last_status(TrackLoopCard *card,
                                     const char *status_value) {
    test_card_set_labeled_value(&card->base, 3U, LABEL_LAST,
                                status_value, "OK");
}

void track_loop_card_set_info_status(TrackLoopCard *card,
                                     const char *status_value) {
    test_card_set_labeled_value(&card->base, 4U, LABEL_INFO,
                                status_value, S_READY);
}

void track_loop_card_set_active(TrackLoopCard *card) {
    track_loop_card_set_last_status(card, "OK");
    track_loop_card_set_info_status(card, "LOOPING");
}

void track_loop_card_set_drive_not_ready(TrackLoopCard *card,
                                         unsigned char st3) {
    snprintf(card->base.text[3], TEST_CARD_LINE_LEN, "%sDRIVE NR ST3=%02X",
             LABEL_LAST, st3);
    track_loop_card_set_info_status(card, "RETRYING");
}

void track_loop_card_set_seek_fail(TrackLoopCard *card, unsigned char track,
                                   unsigned char st0) {
    snprintf(card->base.text[3], TEST_CARD_LINE_LEN, "%sSEEK FAIL T=%u",
             LABEL_LAST, (unsigned int) track);
    snprintf(card->base.text[4], TEST_CARD_LINE_LEN, "%sST0=%02X",
             LABEL_INFO, st0);
}

void track_loop_card_set_read_id_fail(TrackLoopCard *card, unsigned char track,
                                      const char *reason) {
    snprintf(card->base.text[3], TEST_CARD_LINE_LEN, "%sRID FAIL T=%u",
             LABEL_LAST, (unsigned int) track);
    track_loop_card_set_info_status(card, reason ? reason : S_UNKNOWN);
}

void track_loop_card_set_bad_sector_size(TrackLoopCard *card,
                                         unsigned char size_code) {
    snprintf(card->base.text[3], TEST_CARD_LINE_LEN, "%sRID N=%u BAD",
             LABEL_LAST, (unsigned int) size_code);
    track_loop_card_set_info_status(card, "INVALID SECTOR SIZE");
}

void track_loop_card_set_read_fail(TrackLoopCard *card, unsigned char track,
                                   const char *reason) {
    snprintf(card->base.text[3], TEST_CARD_LINE_LEN, "%sREAD FAIL T=%u",
             LABEL_LAST, (unsigned int) track);
    track_loop_card_set_info_status(card, reason ? reason : S_UNKNOWN);
}

void track_loop_card_set_stopped(TrackLoopCard *card) {
    track_loop_card_set_last_status(card, S_STOPPED);
    track_loop_card_set_info_status(card, "USER EXIT");
}

void track_loop_card_render(const TrackLoopCard *card, TestCardResult result) {
    test_card_render_result(&card->base, result);
}

/* ----------------------------------------------------------------------- */
/* RPM loop card                                                             */
/* ----------------------------------------------------------------------- */

void rpm_loop_card_init(RpmLoopCard *card) {
    test_card_init(&card->base, "DISK RPM CHECK LOOP", "ENTER/X EXIT", 5U);
    rpm_loop_card_set_last_status(card, "WAITING");
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
                                status_value, "WAITING");
}

void rpm_loop_card_set_info_status(RpmLoopCard *card,
                                   const char *status_value) {
    test_card_set_labeled_value(&card->base, 4U, LABEL_INFO,
                                status_value, S_READY);
}

void rpm_loop_card_set_drive_not_ready(RpmLoopCard *card) {
    rpm_loop_card_set_last_status(card, "DRIVE NOT READY");
    rpm_loop_card_set_info_status(card, "CHECK MEDIA");
}

void rpm_loop_card_set_seek_fail(RpmLoopCard *card) {
    rpm_loop_card_set_last_status(card, "SEEK TRACK0 FAIL");
    rpm_loop_card_set_info_status(card, "ST0 SET");
}

void rpm_loop_card_set_id_fail(RpmLoopCard *card, const char *reason) {
    rpm_loop_card_set_last_status(card, "ID FAIL");
    rpm_loop_card_set_info_status(card, reason ? reason : S_UNKNOWN);
}

void rpm_loop_card_set_no_measurement(RpmLoopCard *card,
                                      unsigned char seen_other) {
    rpm_loop_card_set_last_status(card, "RPM N/A");
    rpm_loop_card_set_info_status(card, seen_other ? "NO REV MARK" : "SAME SEC");
}

void rpm_loop_card_set_period_bad(RpmLoopCard *card) {
    rpm_loop_card_set_last_status(card, "PERIOD BAD");
    rpm_loop_card_set_info_status(card, "ZERO DELTA");
}

void rpm_loop_card_set_sample_ready(RpmLoopCard *card) {
    rpm_loop_card_set_last_status(card, "SAMPLE OK");
    rpm_loop_card_set_info_status(card, "PERIOD READY");
}

void rpm_loop_card_set_stopped(RpmLoopCard *card) {
    rpm_loop_card_set_last_status(card, S_STOPPED);
    rpm_loop_card_set_info_status(card, "USER EXIT");
}

void rpm_loop_card_render(const RpmLoopCard *card, TestCardResult result) {
    test_card_render_result(&card->base, result);
}

/* ----------------------------------------------------------------------- */
/* Motor / drive status card                                                 */
/* ----------------------------------------------------------------------- */

void motor_drive_card_init(MotorDriveCard *card, const char *controls) {
    test_card_init(&card->base, "MOTOR AND DRIVE STATUS", controls, 6U);
}

void motor_drive_card_set_unknown(MotorDriveCard *card) {
    test_card_set_labeled_value(&card->base, 1U, LABEL_ST3,
                                zx3_str_dash3, zx3_str_dash3);
    card_fill_st3_fields(&card->base, 2U, 0, 0);
}

void motor_drive_card_set_motor_on(MotorDriveCard *card) {
    test_card_set_labeled_value(&card->base, 0U, LABEL_MOTOR, "ON", "ON");
}

void motor_drive_card_set_motor_off(MotorDriveCard *card) {
    test_card_set_labeled_value(&card->base, 0U, LABEL_MOTOR, "OFF", "OFF");
}

void motor_drive_card_set_drive_status(MotorDriveCard *card,
                                       unsigned char have_st3,
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

void motor_drive_card_reset(MotorDriveCard *card) {
    motor_drive_card_set_motor_off(card);
    motor_drive_card_set_unknown(card);
}

void motor_drive_card_render(const MotorDriveCard *card, TestCardResult result) {
    test_card_render_result(&card->base, result);
}

/* ----------------------------------------------------------------------- */
/* Read ID probe card                                                        */
/* ----------------------------------------------------------------------- */

void read_id_probe_card_init(ReadIdProbeCard *card, const char *controls) {
    test_card_init(&card->base, "DRIVE READ ID PROBE", controls, 6U);
    read_id_probe_card_set_unknown(card);
    read_id_probe_card_set_id_status(card, S_UNKNOWN);
}

void read_id_probe_card_set_unknown(ReadIdProbeCard *card) {
    test_card_set_labeled_value(&card->base, 0U, LABEL_ST3,
                                zx3_str_dash3, zx3_str_dash3);
    card_fill_st3_fields(&card->base, 1U, 0, 0);
}

void read_id_probe_card_set_id_status(ReadIdProbeCard *card,
                                      const char *status_value) {
    test_card_set_labeled_value(&card->base, 5U, LABEL_ID,
                                status_value, S_UNKNOWN);
}

void read_id_probe_card_set_drive_status(ReadIdProbeCard *card,
                                         unsigned char have_st3,
                                         unsigned char st3) {
    if (!have_st3) {
        test_card_set_labeled_value(&card->base, 0U, LABEL_ST3,
                                    zx3_str_timeout, zx3_str_timeout);
    }
    else {
        snprintf(card->base.text[0], TEST_CARD_LINE_LEN, "%s0x%02X",
                 LABEL_ST3, st3);
    }
    card_fill_st3_fields(&card->base, 1U, have_st3, st3);
}

void read_id_probe_card_set_id_chrn(ReadIdProbeCard *card, unsigned char c,
                                    unsigned char h, unsigned char r,
                                    unsigned char n) {
    snprintf(card->base.text[5], TEST_CARD_LINE_LEN, "%sC%u H%u R%u N%u",
             LABEL_ID, (unsigned int) c, (unsigned int) h,
             (unsigned int) r, (unsigned int) n);
}

void read_id_probe_card_set_id_failure(ReadIdProbeCard *card,
                                       const char *reason) {
    test_card_set_labeled_value(&card->base, 5U, LABEL_ID,
                                reason, S_UNKNOWN);
}

void read_id_probe_card_reset(ReadIdProbeCard *card) {
    read_id_probe_card_set_unknown(card);
    read_id_probe_card_set_id_status(card, "WAITING");
}

void read_id_probe_card_render(const ReadIdProbeCard *card,
                               TestCardResult result) {
    test_card_render_result(&card->base, result);
}

/* ----------------------------------------------------------------------- */
/* Recalibrate / seek card                                                   */
/* ----------------------------------------------------------------------- */

void recal_seek_card_init(RecalSeekCard *card, const char *controls) {
    test_card_init(&card->base, "RECALIBRATE AND SEEK TRACK 2", controls, 4U);
}

void recal_seek_card_set_unknown(RecalSeekCard *card) {
    test_card_set_labeled_value(&card->base, 0U, LABEL_READY,
                                zx3_str_dash3, zx3_str_dash3);
    test_card_set_labeled_value(&card->base, 1U, LABEL_RECAL,
                                zx3_str_dash3, zx3_str_dash3);
    test_card_set_labeled_value(&card->base, 2U, LABEL_SEEK,
                                zx3_str_dash3, zx3_str_dash3);
    test_card_set_labeled_value(&card->base, 3U, LABEL_DETAIL,
                                zx3_str_dash3, zx3_str_dash3);
}

void recal_seek_card_set_ready_yes(RecalSeekCard *card) {
    test_card_set_labeled_value(&card->base, 0U, LABEL_READY, "YES", "YES");
}

void recal_seek_card_set_ready_fail_st3(RecalSeekCard *card,
                                        unsigned char st3) {
    snprintf(card->base.text[0], TEST_CARD_LINE_LEN, "%sFAIL ST3=%02X",
             LABEL_READY, st3);
}

void recal_seek_card_set_recal_status(RecalSeekCard *card,
                                      RecalSeekStatus status) {
    test_card_set_labeled_value(&card->base, 1U, LABEL_RECAL,
                                recal_seek_status_text(status), zx3_str_dash3);
}

void recal_seek_card_set_seek_status(RecalSeekCard *card,
                                     RecalSeekStatus status) {
    test_card_set_labeled_value(&card->base, 2U, LABEL_SEEK,
                                recal_seek_status_text(status), zx3_str_dash3);
}

void recal_seek_card_set_detail_status(RecalSeekCard *card,
                                       const char *status_value) {
    test_card_set_labeled_value(&card->base, 3U, LABEL_DETAIL,
                                status_value, zx3_str_dash3);
}

void recal_seek_card_set_detail_st0_pcn(RecalSeekCard *card,
                                        unsigned char st0,
                                        unsigned char pcn) {
    snprintf(card->base.text[3], TEST_CARD_LINE_LEN, "%sST0=%02X PCN=%u",
             LABEL_DETAIL, st0, (unsigned int) pcn);
}

void recal_seek_card_set_detail_track(RecalSeekCard *card,
                                      unsigned char track) {
    snprintf(card->base.text[3], TEST_CARD_LINE_LEN, "%sTRACK %u",
             LABEL_DETAIL, (unsigned int) track);
}

void recal_seek_card_reset(RecalSeekCard *card) {
    recal_seek_card_set_unknown(card);
}

void recal_seek_card_render(const RecalSeekCard *card, TestCardResult result) {
    test_card_render_result(&card->base, result);
}

/* ----------------------------------------------------------------------- */
/* Read ID card                                                              */
/* ----------------------------------------------------------------------- */

void read_id_card_init(ReadIdCard *card, const char *controls) {
    test_card_init(&card->base, "READ ID ON TRACK 0", controls, 4U);
}

void read_id_card_set_waiting(ReadIdCard *card) {
    test_card_set_labeled_value(&card->base, 0U, LABEL_MEDIA,
                                S_READABLE_DISK, S_READABLE_DISK);
    test_card_set_labeled_value(&card->base, 1U, LABEL_STS,
                                "--/--/--", "--/--/--");
    test_card_set_labeled_value(&card->base, 2U, LABEL_CHRN,
                                S_INVALID, S_INVALID);
    test_card_set_labeled_value(&card->base, 3U, LABEL_DETAIL,
                                "WAITING", "WAITING");
}

void read_id_card_set_reading(ReadIdCard *card) {
    test_card_set_labeled_value(&card->base, 3U, LABEL_DETAIL,
                                "READING ID", "READING ID");
}

void read_id_card_set_drive_not_ready(ReadIdCard *card, unsigned char st3) {
    test_card_set_labeled_value(&card->base, 0U, LABEL_MEDIA,
                                S_READABLE_DISK, S_READABLE_DISK);
    snprintf(card->base.text[1], TEST_CARD_LINE_LEN, "%sST3=%02X",
             LABEL_READY, st3);
    test_card_set_labeled_value(&card->base, 2U, LABEL_CHRN,
                                S_INVALID, S_INVALID);
    test_card_set_labeled_value(&card->base, 3U, LABEL_DETAIL,
                                "DRIVE NOT READY", "DRIVE NOT READY");
}

void read_id_card_set_status(ReadIdCard *card, unsigned char st0,
                             unsigned char st1, unsigned char st2) {
    test_card_set_labeled_value(&card->base, 0U, LABEL_MEDIA,
                                S_READABLE_DISK, S_READABLE_DISK);
    snprintf(card->base.text[1], TEST_CARD_LINE_LEN, "%s%02X/%02X/%02X",
             LABEL_STS, st0, st1, st2);
}

void read_id_card_set_chrn_valid(ReadIdCard *card, unsigned char c,
                                 unsigned char h, unsigned char r,
                                 unsigned char n) {
    snprintf(card->base.text[2], TEST_CARD_LINE_LEN, "%s%u/%u/%u/%u",
             LABEL_CHRN, (unsigned int) c, (unsigned int) h,
             (unsigned int) r, (unsigned int) n);
}

void read_id_card_set_chrn_status(ReadIdCard *card,
                                  const char *status_value) {
    test_card_set_labeled_value(&card->base, 2U, LABEL_CHRN,
                                status_value, S_INVALID);
}

void read_id_card_set_detail_status(ReadIdCard *card,
                                    const char *status_value) {
    test_card_set_labeled_value(&card->base, 3U, LABEL_DETAIL,
                                status_value, zx3_str_dash3);
}

void read_id_card_set_detail_failure(ReadIdCard *card, const char *reason) {
    test_card_set_labeled_value(&card->base, 3U, LABEL_DETAIL,
                                reason, S_UNKNOWN);
}

void read_id_card_reset(ReadIdCard *card) {
    read_id_card_set_waiting(card);
}

void read_id_card_render(const ReadIdCard *card, TestCardResult result) {
    test_card_render_result(&card->base, result);
}

/* ----------------------------------------------------------------------- */
/* Interactive seek card                                                     */
/* ----------------------------------------------------------------------- */

void interactive_seek_card_init(InteractiveSeekCard *card,
                                const char *controls) {
    test_card_init(&card->base, "INTERACTIVE STEP SEEK", controls, 3U);
}

void interactive_seek_card_set_ready_fail(InteractiveSeekCard *card,
                                          unsigned char st3) {
    snprintf(card->base.text[0], TEST_CARD_LINE_LEN, "%sFAIL ST3=%02X",
             LABEL_READY, st3);
}

void interactive_seek_card_set_track(InteractiveSeekCard *card,
                                     unsigned char track) {
    snprintf(card->base.text[0], TEST_CARD_LINE_LEN, "%s%u",
             LABEL_TRACK, (unsigned int) track);
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
             LABEL_PCN, (unsigned int) pcn);
}

void interactive_seek_card_render(const InteractiveSeekCard *card,
                                  TestCardResult result) {
    test_card_render_result(&card->base, result);
}
