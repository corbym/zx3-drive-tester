#include "test_cards.h"
#include "ui.h"

#include <string.h>

/* ----------------------------------------------------------------------- */
/* Row label constants — internal to this translation unit.                */
/* ----------------------------------------------------------------------- */
#define TRACK_LOOP_LABEL_TRACK "TRACK : "
#define TRACK_LOOP_LABEL_PASS  "PASS  : "
#define TRACK_LOOP_LABEL_FAIL  "FAIL  : "
#define TRACK_LOOP_LABEL_LAST  "LAST  : "
#define TRACK_LOOP_LABEL_INFO  "INFO  : "
#define RPM_LOOP_LABEL_RPM     "RPM   : "
#define RPM_LOOP_LABEL_PASS    "PASS  : "
#define RPM_LOOP_LABEL_FAIL    "FAIL  : "
#define RPM_LOOP_LABEL_LAST    "LAST  : "
#define RPM_LOOP_LABEL_INFO    "INFO  : "
#define MOTOR_DRIVE_LABEL_MOTOR  "MOTOR : "
#define MOTOR_DRIVE_LABEL_ST3    "ST3   : "
#define READ_ID_PROBE_LABEL_ST3    "ST3   : "
#define READ_ID_PROBE_LABEL_ID     "ID    : "
#define RECAL_SEEK_LABEL_READY  "READY : "
#define RECAL_SEEK_LABEL_RECAL  "RECAL : "
#define RECAL_SEEK_LABEL_SEEK   "SEEK  : "
#define RECAL_SEEK_LABEL_DETAIL "DETAIL: "
#define READ_ID_LABEL_MEDIA  "MEDIA : "
#define READ_ID_LABEL_STS    "STS   : "
#define READ_ID_LABEL_READY  "READY : "
#define READ_ID_LABEL_CHRN   "CHRN  : "
#define READ_ID_LABEL_DETAIL "DETAIL: "
#define INTERACTIVE_SEEK_LABEL_READY "READY : "
#define INTERACTIVE_SEEK_LABEL_TRACK "TRACK : "
#define INTERACTIVE_SEEK_LABEL_LAST  "LAST  : "
#define INTERACTIVE_SEEK_LABEL_PCN   "PCN   : "

/* ----------------------------------------------------------------------- */
/* Base TestCard operations                                                  */
/* ----------------------------------------------------------------------- */

void test_card_init(TestCard *card, const char *title, const char *controls,
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
        case TEST_CARD_RESULT_READY: return "READY";
        case TEST_CARD_RESULT_RUNNING: return "RUNNING";
        case TEST_CARD_RESULT_ACTIVE: return "ACTIVE";
        case TEST_CARD_RESULT_PASS: return "PASS";
        case TEST_CARD_RESULT_FAIL: return "FAIL";
        case TEST_CARD_RESULT_COMPLETE: return "COMPLETE";
        case TEST_CARD_RESULT_STOPPED: return "STOPPED";
        case TEST_CARD_RESULT_OUT_OF_RANGE: return "OUT-OF-RANGE";
        default: return "IDLE";
    }
}

void test_card_render_result(const TestCard *card, TestCardResult result) {
    test_card_render(card, "RESULT: ", test_card_result_text(result));
}

/* ----------------------------------------------------------------------- */
/* Shared helpers                                                            */
/* ----------------------------------------------------------------------- */

static const char *yes_no_text(unsigned char flag) {
    return flag ? "YES" : "NO";
}

static const char *recal_seek_status_text(RecalSeekStatus status) {
    switch (status) {
        case RECAL_SEEK_STATUS_RUNNING: return "RUNNING";
        case RECAL_SEEK_STATUS_PENDING: return "PENDING";
        case RECAL_SEEK_STATUS_PASS: return "PASS";
        case RECAL_SEEK_STATUS_FAIL: return "FAIL";
        case RECAL_SEEK_STATUS_SKIPPED: return "SKIPPED";
        default: return "---";
    }
}

/* Shared ST3 decode: fills 4 rows (ready/wprot/track0/fault) from base_row. */
static void card_fill_st3_fields(TestCard *tc, unsigned char base_row,
                                 unsigned char have_st3, unsigned char st3) {
    static const char *const lbl[] = {"READY : ", "WPROT : ", "TRACK0: ", "FAULT : "};
    static const unsigned char bits[] = {0x20, 0x40, 0x10, 0x80};
    unsigned char i;
    for (i = 0; i < 4U; i++) {
        test_card_set_labeled_value(
            tc, (unsigned char) (base_row + i), lbl[i],
            yes_no_text(have_st3 && (st3 & bits[i])), "NO");
    }
}

/* ----------------------------------------------------------------------- */
/* Report card                                                               */
/* ----------------------------------------------------------------------- */

static const char *report_card_state_text(ReportCardState state) {
    switch (state) {
        case REPORT_CARD_STATE_PASS: return "PASS";
        case REPORT_CARD_STATE_FAIL: return "FAIL";
        default: return "NOT RUN";
    }
}

static const char *report_card_controls_text(ReportCardPhase phase) {
    return (phase == REPORT_CARD_PHASE_RUNNING)
               ? "KEYS  : AUTO ADVANCE"
               : "KEYS  : ENTER/ESC/X MENU";
}

static const char *report_card_result_text(ReportCardPhase phase) {
    switch (phase) {
        case REPORT_CARD_PHASE_READY: return "READY";
        case REPORT_CARD_PHASE_RUNNING: return "RUNNING";
        case REPORT_CARD_PHASE_COMPLETE: return "COMPLETE";
        default: return "IDLE";
    }
}

static const char *report_card_status_text(ReportCardPhase phase) {
    switch (phase) {
        case REPORT_CARD_PHASE_READY: return "READY";
        case REPORT_CARD_PHASE_RUNNING: return "RUNNING";
        case REPORT_CARD_PHASE_COMPLETE: return "COMPLETE";
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
    char bar[9];
    unsigned char i;

    for (i = 0; i < 8U; i++) {
        bar[i] = '|';
    }
    bar[8] = '\0';
    snprintf(out, 32U, "%-6s [%s] %s", report_card_slot_label(slot), bar,
             report_card_state_text(state));
}

static void report_card_build_overall_row(char *out, unsigned char total) {
    unsigned char i;
    char meter[6];

    for (i = 0; i < 5U; i++) {
        meter[i] = (i < total) ? '|' : ' ';
    }
    meter[5] = '\0';
    snprintf(out, 32U, "OVERALL [%s] %u/5 PASS", meter, (unsigned int) total);
}

/* Colouration for a single report-card row.  Screen row 3 = lines[0], so
 * slot i occupies screen row 4+i.  Called exclusively from report_card_render;
 * no knowledge of colouration escapes to the caller. */
static void report_card_colour_row(unsigned char screen_row, const char *text,
                                   ReportCardState state) {
    unsigned char col;
    const char *lbr;
    const char *rbr;
    unsigned char bar_ink;
    unsigned char bar_paper;

    if (!text || screen_row >= 24U) return;

    if (state == REPORT_CARD_STATE_PASS) {
        bar_ink = ZX_COLOUR_BLACK;
        bar_paper = ZX_COLOUR_GREEN;
    }
    else if (state == REPORT_CARD_STATE_FAIL) {
        bar_ink = ZX_COLOUR_RED;
        bar_paper = ZX_COLOUR_YELLOW;
    }
    else {
        bar_ink = ZX_COLOUR_BLACK;
        bar_paper = ZX_COLOUR_WHITE;
    }

    lbr = strchr(text, '[');
    rbr = strchr(text, ']');
    if (lbr && rbr && rbr > lbr) {
        unsigned char bar_col = (unsigned char) (lbr - text + 1);
        while (bar_col < 32U && &text[bar_col] < rbr) {
            if (text[bar_col] == '|') {
                ui_attr_set_cell(screen_row, bar_col, bar_ink, bar_paper, 1);
            }
            bar_col++;
        }
    }

    for (col = 0; col < 29U; col++) {
        if (state == REPORT_CARD_STATE_PASS && strncmp(&text[col], "PASS", 4U) == 0) {
            ui_attr_set_run(screen_row, col, 4, ZX_COLOUR_BLACK, ZX_COLOUR_GREEN, 1);
            break;
        }
        if (state == REPORT_CARD_STATE_FAIL && strncmp(&text[col], "FAIL", 4U) == 0) {
            ui_attr_set_run(screen_row, col, 4, ZX_COLOUR_RED, ZX_COLOUR_YELLOW, 1);
            break;
        }
        if (state == REPORT_CARD_STATE_NOT_RUN && strncmp(&text[col], "NOT RUN", 7U) == 0) {
            ui_attr_set_run(screen_row, col, 7, ZX_COLOUR_BLUE, ZX_COLOUR_WHITE, 1);
            break;
        }
    }
}

void report_card_init(ReportCard *card) {
    unsigned char i;

    test_card_init(&card->base, "TEST REPORT CARD",
                   report_card_controls_text(REPORT_CARD_PHASE_IDLE), 9U);
    card->total_pass = 0U;
    card->phase = (unsigned char) REPORT_CARD_PHASE_IDLE;
    for (i = 0; i <= (unsigned char) REPORT_CARD_SLOT_OVERALL; i++) {
        card->slot_state[i] = (unsigned char) REPORT_CARD_STATE_NOT_RUN;
    }
    test_card_set_line(&card->base, 8U, "BARS  : |=STATE COLOR");
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

    report_card_build_overall_row(row_buf, card->total_pass);
    test_card_set_line(&card->base, (unsigned char) (1U + REPORT_CARD_SLOT_OVERALL),
                       row_buf);

    test_card_render(&card->base, "RESULT: ",
                     report_card_result_text((ReportCardPhase) card->phase));

    /* Apply state-based colouration for every slot row.
     * ui_render_text_screen places lines[0] at screen row 3, so slot i
     * (which occupies base line 1+i) lands at screen row 4+i. */
    for (i = 0; i <= (unsigned char) REPORT_CARD_SLOT_OVERALL; i++) {
        report_card_colour_row(
            (unsigned char) (4U + i),
            test_card_line(&card->base, (unsigned char) (1U + i)),
            (ReportCardState) card->slot_state[i]);
    }
}

/* ----------------------------------------------------------------------- */
/* Track loop card                                                           */
/* ----------------------------------------------------------------------- */

void track_loop_card_init(TrackLoopCard *card) {
    test_card_init(&card->base, "READ TRACK DATA LOOP",
                   "KEYS  : J/K TRACK  ENTER/X EXIT", 5U);
    track_loop_card_set_last_status(card, "OK");
    track_loop_card_set_info_status(card, "READY");
}

void track_loop_card_set_track(TrackLoopCard *card, unsigned char track) {
    snprintf(card->base.text[0], TEST_CARD_LINE_LEN, "%s%3u",
             TRACK_LOOP_LABEL_TRACK, (unsigned int) track);
}

void track_loop_card_set_counts(TrackLoopCard *card, unsigned int pass_count,
                                unsigned int fail_count) {
    snprintf(card->base.text[1], TEST_CARD_LINE_LEN, "%s%3u",
             TRACK_LOOP_LABEL_PASS, pass_count);
    snprintf(card->base.text[2], TEST_CARD_LINE_LEN, "%s%3u",
             TRACK_LOOP_LABEL_FAIL, fail_count);
}

void track_loop_card_set_last_status(TrackLoopCard *card,
                                     const char *status_value) {
    test_card_set_labeled_value(&card->base, 3U, TRACK_LOOP_LABEL_LAST,
                                status_value, "OK");
}

void track_loop_card_set_info_status(TrackLoopCard *card,
                                     const char *status_value) {
    test_card_set_labeled_value(&card->base, 4U, TRACK_LOOP_LABEL_INFO,
                                status_value, "READY");
}

void track_loop_card_set_active(TrackLoopCard *card) {
    track_loop_card_set_last_status(card, "OK");
    track_loop_card_set_info_status(card, "LOOPING");
}

void track_loop_card_set_drive_not_ready(TrackLoopCard *card,
                                         unsigned char st3) {
    snprintf(card->base.text[3], TEST_CARD_LINE_LEN, "%sDRIVE NR ST3=%02X",
             TRACK_LOOP_LABEL_LAST, st3);
    track_loop_card_set_info_status(card, "RETRYING");
}

void track_loop_card_set_seek_fail(TrackLoopCard *card, unsigned char track,
                                   unsigned char st0) {
    snprintf(card->base.text[3], TEST_CARD_LINE_LEN, "%sSEEK FAIL T=%u",
             TRACK_LOOP_LABEL_LAST, (unsigned int) track);
    snprintf(card->base.text[4], TEST_CARD_LINE_LEN, "%sST0=%02X",
             TRACK_LOOP_LABEL_INFO, st0);
}

void track_loop_card_set_read_id_fail(TrackLoopCard *card, unsigned char track,
                                      const char *reason) {
    snprintf(card->base.text[3], TEST_CARD_LINE_LEN, "%sRID FAIL T=%u",
             TRACK_LOOP_LABEL_LAST, (unsigned int) track);
    track_loop_card_set_info_status(card, reason ? reason : "UNKNOWN");
}

void track_loop_card_set_bad_sector_size(TrackLoopCard *card,
                                         unsigned char size_code) {
    snprintf(card->base.text[3], TEST_CARD_LINE_LEN, "%sRID N=%u BAD",
             TRACK_LOOP_LABEL_LAST, (unsigned int) size_code);
    track_loop_card_set_info_status(card, "INVALID SECTOR SIZE");
}

void track_loop_card_set_read_fail(TrackLoopCard *card, unsigned char track,
                                   const char *reason) {
    snprintf(card->base.text[3], TEST_CARD_LINE_LEN, "%sREAD FAIL T=%u",
             TRACK_LOOP_LABEL_LAST, (unsigned int) track);
    track_loop_card_set_info_status(card, reason ? reason : "UNKNOWN");
}

void track_loop_card_set_stopped(TrackLoopCard *card) {
    track_loop_card_set_last_status(card, "STOPPED");
    track_loop_card_set_info_status(card, "USER EXIT");
}

void track_loop_card_render(const TrackLoopCard *card, TestCardResult result) {
    test_card_render_result(&card->base, result);
}

/* ----------------------------------------------------------------------- */
/* RPM loop card                                                             */
/* ----------------------------------------------------------------------- */

void rpm_loop_card_init(RpmLoopCard *card) {
    test_card_init(&card->base, "DISK RPM CHECK LOOP", "KEYS  : ENTER/X EXIT",
                   5U);
    rpm_loop_card_set_last_status(card, "WAITING");
    rpm_loop_card_set_info_status(card, "READY");
}

void rpm_loop_card_set_rpm(RpmLoopCard *card, unsigned int rpm,
                           unsigned char rpm_valid) {
    if (rpm_valid) {
        snprintf(card->base.text[0], TEST_CARD_LINE_LEN, "%s%3u",
                 RPM_LOOP_LABEL_RPM, rpm);
    }
    else {
        test_card_set_labeled_value(&card->base, 0U, RPM_LOOP_LABEL_RPM, "---", "---");
    }
}

void rpm_loop_card_set_counts(RpmLoopCard *card, unsigned int pass_count,
                              unsigned int fail_count) {
    snprintf(card->base.text[1], TEST_CARD_LINE_LEN, "%s%3u",
             RPM_LOOP_LABEL_PASS, pass_count);
    snprintf(card->base.text[2], TEST_CARD_LINE_LEN, "%s%3u",
             RPM_LOOP_LABEL_FAIL, fail_count);
}

void rpm_loop_card_set_last_status(RpmLoopCard *card,
                                   const char *status_value) {
    test_card_set_labeled_value(&card->base, 3U, RPM_LOOP_LABEL_LAST,
                                status_value, "WAITING");
}

void rpm_loop_card_set_info_status(RpmLoopCard *card,
                                   const char *status_value) {
    test_card_set_labeled_value(&card->base, 4U, RPM_LOOP_LABEL_INFO,
                                status_value, "READY");
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
    rpm_loop_card_set_info_status(card, reason ? reason : "UNKNOWN");
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
    rpm_loop_card_set_last_status(card, "STOPPED");
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
    test_card_set_labeled_value(&card->base, 1U, MOTOR_DRIVE_LABEL_ST3, "---", "---");
    card_fill_st3_fields(&card->base, 2U, 0, 0);
}

void motor_drive_card_set_motor_on(MotorDriveCard *card) {
    test_card_set_labeled_value(&card->base, 0U, MOTOR_DRIVE_LABEL_MOTOR, "ON", "ON");
}

void motor_drive_card_set_motor_off(MotorDriveCard *card) {
    test_card_set_labeled_value(&card->base, 0U, MOTOR_DRIVE_LABEL_MOTOR, "OFF", "OFF");
}

void motor_drive_card_set_drive_status(MotorDriveCard *card,
                                       unsigned char have_st3,
                                       unsigned char st3) {
    if (!have_st3) {
        test_card_set_labeled_value(&card->base, 1U, MOTOR_DRIVE_LABEL_ST3,
                                    "TIMEOUT", "TIMEOUT");
    }
    else {
        snprintf(card->base.text[1], TEST_CARD_LINE_LEN, "%s0x%02X",
                 MOTOR_DRIVE_LABEL_ST3, st3);
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
    read_id_probe_card_set_id_status(card, "UNKNOWN");
}

void read_id_probe_card_set_unknown(ReadIdProbeCard *card) {
    test_card_set_labeled_value(&card->base, 0U, READ_ID_PROBE_LABEL_ST3, "---", "---");
    card_fill_st3_fields(&card->base, 1U, 0, 0);
}

void read_id_probe_card_set_id_status(ReadIdProbeCard *card,
                                      const char *status_value) {
    test_card_set_labeled_value(&card->base, 5U, READ_ID_PROBE_LABEL_ID,
                                status_value, "UNKNOWN");
}

void read_id_probe_card_set_drive_status(ReadIdProbeCard *card,
                                         unsigned char have_st3,
                                         unsigned char st3) {
    if (!have_st3) {
        test_card_set_labeled_value(&card->base, 0U, READ_ID_PROBE_LABEL_ST3,
                                    "TIMEOUT", "TIMEOUT");
    }
    else {
        snprintf(card->base.text[0], TEST_CARD_LINE_LEN, "%s0x%02X",
                 READ_ID_PROBE_LABEL_ST3, st3);
    }
    card_fill_st3_fields(&card->base, 1U, have_st3, st3);
}

void read_id_probe_card_set_id_chrn(ReadIdProbeCard *card, unsigned char c,
                                    unsigned char h, unsigned char r,
                                    unsigned char n) {
    snprintf(card->base.text[5], TEST_CARD_LINE_LEN, "%sC%u H%u R%u N%u",
             READ_ID_PROBE_LABEL_ID, (unsigned int) c, (unsigned int) h,
             (unsigned int) r, (unsigned int) n);
}

void read_id_probe_card_set_id_failure(ReadIdProbeCard *card,
                                       const char *reason) {
    test_card_set_labeled_value(&card->base, 5U, READ_ID_PROBE_LABEL_ID,
                                reason, "UNKNOWN");
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
    test_card_set_labeled_value(&card->base, 0U, RECAL_SEEK_LABEL_READY, "---", "---");
    test_card_set_labeled_value(&card->base, 1U, RECAL_SEEK_LABEL_RECAL, "---", "---");
    test_card_set_labeled_value(&card->base, 2U, RECAL_SEEK_LABEL_SEEK, "---", "---");
    test_card_set_labeled_value(&card->base, 3U, RECAL_SEEK_LABEL_DETAIL, "---", "---");
}

void recal_seek_card_set_ready_yes(RecalSeekCard *card) {
    test_card_set_labeled_value(&card->base, 0U, RECAL_SEEK_LABEL_READY, "YES", "YES");
}

void recal_seek_card_set_ready_fail_st3(RecalSeekCard *card,
                                        unsigned char st3) {
    snprintf(card->base.text[0], TEST_CARD_LINE_LEN, "%sFAIL ST3=%02X",
             RECAL_SEEK_LABEL_READY, st3);
}

void recal_seek_card_set_recal_status(RecalSeekCard *card,
                                      RecalSeekStatus status) {
    test_card_set_labeled_value(&card->base, 1U, RECAL_SEEK_LABEL_RECAL,
                                recal_seek_status_text(status), "---");
}

void recal_seek_card_set_seek_status(RecalSeekCard *card,
                                     RecalSeekStatus status) {
    test_card_set_labeled_value(&card->base, 2U, RECAL_SEEK_LABEL_SEEK,
                                recal_seek_status_text(status), "---");
}

void recal_seek_card_set_detail_status(RecalSeekCard *card,
                                       const char *status_value) {
    test_card_set_labeled_value(&card->base, 3U, RECAL_SEEK_LABEL_DETAIL,
                                status_value, "---");
}

void recal_seek_card_set_detail_st0_pcn(RecalSeekCard *card,
                                        unsigned char st0,
                                        unsigned char pcn) {
    snprintf(card->base.text[3], TEST_CARD_LINE_LEN, "%sST0=%02X PCN=%u",
             RECAL_SEEK_LABEL_DETAIL, st0, (unsigned int) pcn);
}

void recal_seek_card_set_detail_track(RecalSeekCard *card,
                                      unsigned char track) {
    snprintf(card->base.text[3], TEST_CARD_LINE_LEN, "%sTRACK %u",
             RECAL_SEEK_LABEL_DETAIL, (unsigned int) track);
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
    test_card_set_labeled_value(&card->base, 0U, READ_ID_LABEL_MEDIA,
                                "READABLE DISK REQUIRED", "READABLE DISK REQUIRED");
    test_card_set_labeled_value(&card->base, 1U, READ_ID_LABEL_STS,
                                "--/--/--", "--/--/--");
    test_card_set_labeled_value(&card->base, 2U, READ_ID_LABEL_CHRN,
                                "INVALID", "INVALID");
    test_card_set_labeled_value(&card->base, 3U, READ_ID_LABEL_DETAIL,
                                "WAITING", "WAITING");
}

void read_id_card_set_reading(ReadIdCard *card) {
    test_card_set_labeled_value(&card->base, 3U, READ_ID_LABEL_DETAIL,
                                "READING ID", "READING ID");
}

void read_id_card_set_drive_not_ready(ReadIdCard *card, unsigned char st3) {
    test_card_set_labeled_value(&card->base, 0U, READ_ID_LABEL_MEDIA,
                                "READABLE DISK REQUIRED", "READABLE DISK REQUIRED");
    snprintf(card->base.text[1], TEST_CARD_LINE_LEN, "%sST3=%02X",
             READ_ID_LABEL_READY, st3);
    test_card_set_labeled_value(&card->base, 2U, READ_ID_LABEL_CHRN,
                                "INVALID", "INVALID");
    test_card_set_labeled_value(&card->base, 3U, READ_ID_LABEL_DETAIL,
                                "DRIVE NOT READY", "DRIVE NOT READY");
}

void read_id_card_set_status(ReadIdCard *card, unsigned char st0,
                             unsigned char st1, unsigned char st2) {
    test_card_set_labeled_value(&card->base, 0U, READ_ID_LABEL_MEDIA,
                                "READABLE DISK REQUIRED", "READABLE DISK REQUIRED");
    snprintf(card->base.text[1], TEST_CARD_LINE_LEN, "%s%02X/%02X/%02X",
             READ_ID_LABEL_STS, st0, st1, st2);
}

void read_id_card_set_chrn_valid(ReadIdCard *card, unsigned char c,
                                 unsigned char h, unsigned char r,
                                 unsigned char n) {
    snprintf(card->base.text[2], TEST_CARD_LINE_LEN, "%s%u/%u/%u/%u",
             READ_ID_LABEL_CHRN, (unsigned int) c, (unsigned int) h,
             (unsigned int) r, (unsigned int) n);
}

void read_id_card_set_chrn_status(ReadIdCard *card,
                                  const char *status_value) {
    test_card_set_labeled_value(&card->base, 2U, READ_ID_LABEL_CHRN,
                                status_value, "INVALID");
}

void read_id_card_set_detail_status(ReadIdCard *card,
                                    const char *status_value) {
    test_card_set_labeled_value(&card->base, 3U, READ_ID_LABEL_DETAIL,
                                status_value, "---");
}

void read_id_card_set_detail_failure(ReadIdCard *card, const char *reason) {
    test_card_set_labeled_value(&card->base, 3U, READ_ID_LABEL_DETAIL,
                                reason, "UNKNOWN");
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
             INTERACTIVE_SEEK_LABEL_READY, st3);
}

void interactive_seek_card_set_track(InteractiveSeekCard *card,
                                     unsigned char track) {
    snprintf(card->base.text[0], TEST_CARD_LINE_LEN, "%s%u",
             INTERACTIVE_SEEK_LABEL_TRACK, (unsigned int) track);
}

void interactive_seek_card_set_last_st0(InteractiveSeekCard *card,
                                        unsigned char st0) {
    snprintf(card->base.text[1], TEST_CARD_LINE_LEN, "%sST0=%02X",
             INTERACTIVE_SEEK_LABEL_LAST, st0);
}

void interactive_seek_card_set_last_status(InteractiveSeekCard *card,
                                           const char *status_value) {
    test_card_set_labeled_value(&card->base, 1U, INTERACTIVE_SEEK_LABEL_LAST,
                                status_value, "---");
}

void interactive_seek_card_set_pcn(InteractiveSeekCard *card,
                                   unsigned char pcn) {
    snprintf(card->base.text[2], TEST_CARD_LINE_LEN, "%s%u",
             INTERACTIVE_SEEK_LABEL_PCN, (unsigned int) pcn);
}

void interactive_seek_card_render(const InteractiveSeekCard *card,
                                  TestCardResult result) {
    test_card_render_result(&card->base, result);
}
