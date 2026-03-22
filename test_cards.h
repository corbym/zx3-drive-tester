#pragma once

#include <stdio.h>

#define TEST_CARD_MAX_LINES 9U
#define TEST_CARD_LINE_LEN 48U

typedef struct {
    const char* title;
    const char* controls;
    unsigned char line_count;
    char text[TEST_CARD_MAX_LINES][TEST_CARD_LINE_LEN];
    const char* lines[TEST_CARD_MAX_LINES];
} TestCard;

typedef struct {
    TestCard base;
    unsigned char total_pass;
    unsigned char phase;
    unsigned char slot_state[7];
} ReportCard;

typedef struct {
    TestCard base;
} TrackLoopCard;

typedef struct {
    TestCard base;
} RpmLoopCard;

typedef struct {
    TestCard base;
} MotorDriveCard;

typedef struct {
    TestCard base;
} ReadIdProbeCard;

typedef struct {
    TestCard base;
} RecalSeekCard;

typedef struct {
    TestCard base;
} ReadIdCard;

typedef struct {
    TestCard base;
} InteractiveSeekCard;

typedef enum {
    TEST_CARD_RESULT_IDLE = 0U,
    TEST_CARD_RESULT_READY = 1U,
    TEST_CARD_RESULT_RUNNING = 2U,
    TEST_CARD_RESULT_ACTIVE = 3U,
    TEST_CARD_RESULT_PASS = 4U,
    TEST_CARD_RESULT_FAIL = 5U,
    TEST_CARD_RESULT_COMPLETE = 6U,
    TEST_CARD_RESULT_STOPPED = 7U,
    TEST_CARD_RESULT_OUT_OF_RANGE = 8U,
} TestCardResult;

typedef enum {
    RECAL_SEEK_STATUS_UNKNOWN = 0U,
    RECAL_SEEK_STATUS_RUNNING = 1U,
    RECAL_SEEK_STATUS_PENDING = 2U,
    RECAL_SEEK_STATUS_PASS = 3U,
    RECAL_SEEK_STATUS_FAIL = 4U,
    RECAL_SEEK_STATUS_SKIPPED = 5U,
} RecalSeekStatus;

typedef enum {
    REPORT_CARD_STATE_NOT_RUN = 0U,
    REPORT_CARD_STATE_PASS = 1U,
    REPORT_CARD_STATE_FAIL = 2U,
} ReportCardState;

typedef enum {
    REPORT_CARD_PHASE_IDLE = 0U,
    REPORT_CARD_PHASE_READY = 1U,
    REPORT_CARD_PHASE_RUNNING = 2U,
    REPORT_CARD_PHASE_COMPLETE = 3U,
} ReportCardPhase;

typedef enum {
    REPORT_CARD_SLOT_LAST = 0U,
    REPORT_CARD_SLOT_MOTOR = 1U,
    REPORT_CARD_SLOT_DRIVE = 2U,
    REPORT_CARD_SLOT_RECAL = 3U,
    REPORT_CARD_SLOT_SEEK = 4U,
    REPORT_CARD_SLOT_READID = 5U,
    REPORT_CARD_SLOT_OVERALL = 6U,
} ReportCardSlot;

/* Generic card API shared by all test/report screens. */
void test_card_init(TestCard* card, const char* title, const char* controls,
                    unsigned char line_count);
void test_card_render(const TestCard* card, const char* result_label,
                      const char* result_value);
void test_card_render_result(const TestCard* card, TestCardResult result);

/* OO-style wrappers over TestCard for concrete screen types. */
void report_card_init(ReportCard* card);
void report_card_set_phase(ReportCard* card, ReportCardPhase phase);
void report_card_set_total_pass(ReportCard* card, unsigned char total_pass);
void report_card_set_slot_state(ReportCard* card, ReportCardSlot slot,
                                ReportCardState state);
void report_card_render(ReportCard* card);
void track_loop_card_init(TrackLoopCard* card);
void track_loop_card_set_track(TrackLoopCard* card, unsigned char track);
void track_loop_card_set_counts(TrackLoopCard* card, unsigned int pass_count,
                                unsigned int fail_count);
void track_loop_card_set_last_status(TrackLoopCard* card,
                                     const char* status_value);
void track_loop_card_set_info_status(TrackLoopCard* card,
                                     const char* status_value);
void track_loop_card_set_active(TrackLoopCard* card);
void track_loop_card_set_drive_not_ready(TrackLoopCard* card,
                                         unsigned char st3);
void track_loop_card_set_seek_fail(TrackLoopCard* card, unsigned char track,
                                   unsigned char st0);
void track_loop_card_set_read_id_fail(TrackLoopCard* card, unsigned char track,
                                      const char* reason);
void track_loop_card_set_bad_sector_size(TrackLoopCard* card,
                                         unsigned char size_code);
void track_loop_card_set_read_fail(TrackLoopCard* card, unsigned char track,
                                   const char* reason);
void track_loop_card_set_stopped(TrackLoopCard* card);
void track_loop_card_render(const TrackLoopCard* card, TestCardResult result);
void rpm_loop_card_init(RpmLoopCard* card);
void rpm_loop_card_set_rpm(RpmLoopCard* card, unsigned int rpm,
                           unsigned char rpm_valid);
void rpm_loop_card_set_counts(RpmLoopCard* card, unsigned int pass_count,
                              unsigned int fail_count);
void rpm_loop_card_set_last_status(RpmLoopCard* card,
                                   const char* status_value);
void rpm_loop_card_set_info_status(RpmLoopCard* card,
                                   const char* status_value);
void rpm_loop_card_set_drive_not_ready(RpmLoopCard* card);
void rpm_loop_card_set_seek_fail(RpmLoopCard* card);
void rpm_loop_card_set_id_fail(RpmLoopCard* card, const char* reason);
void rpm_loop_card_set_no_measurement(RpmLoopCard* card,
                                      unsigned char seen_other);
void rpm_loop_card_set_period_bad(RpmLoopCard* card);
void rpm_loop_card_set_sample_ready(RpmLoopCard* card);
void rpm_loop_card_set_stopped(RpmLoopCard* card);
void rpm_loop_card_render(const RpmLoopCard* card, TestCardResult result);
void motor_drive_card_init(MotorDriveCard* card, const char* controls);
void motor_drive_card_reset(MotorDriveCard* card);
void motor_drive_card_set_unknown(MotorDriveCard* card);
void motor_drive_card_set_motor_on(MotorDriveCard* card);
void motor_drive_card_set_motor_off(MotorDriveCard* card);
void motor_drive_card_set_drive_status(MotorDriveCard* card,
                                       unsigned char have_st3,
                                       unsigned char st3);
void motor_drive_card_render(const MotorDriveCard* card, TestCardResult result);
void read_id_probe_card_init(ReadIdProbeCard* card, const char* controls);
void read_id_probe_card_reset(ReadIdProbeCard* card);
void read_id_probe_card_set_unknown(ReadIdProbeCard* card);
void read_id_probe_card_set_id_status(ReadIdProbeCard* card,
                                      const char* status_value);
void read_id_probe_card_set_drive_status(ReadIdProbeCard* card,
                                         unsigned char have_st3,
                                         unsigned char st3);
void read_id_probe_card_set_id_chrn(ReadIdProbeCard* card, unsigned char c,
                                    unsigned char h, unsigned char r,
                                    unsigned char n);
void read_id_probe_card_set_id_failure(ReadIdProbeCard* card,
                                       const char* reason);
void read_id_probe_card_render(const ReadIdProbeCard* card,
                               TestCardResult result);
void recal_seek_card_init(RecalSeekCard* card, const char* controls);
void recal_seek_card_reset(RecalSeekCard* card);
void recal_seek_card_set_unknown(RecalSeekCard* card);
void recal_seek_card_set_ready_yes(RecalSeekCard* card);
void recal_seek_card_set_ready_fail_st3(RecalSeekCard* card,
                                        unsigned char st3);
void recal_seek_card_set_recal_status(RecalSeekCard* card,
                                      RecalSeekStatus status);
void recal_seek_card_set_seek_status(RecalSeekCard* card,
                                     RecalSeekStatus status);
void recal_seek_card_set_detail_status(RecalSeekCard* card,
                                       const char* status_value);
void recal_seek_card_set_detail_st0_pcn(RecalSeekCard* card,
                                        unsigned char st0,
                                        unsigned char pcn);
void recal_seek_card_set_detail_track(RecalSeekCard* card,
                                      unsigned char track);
void recal_seek_card_render(const RecalSeekCard* card, TestCardResult result);
void read_id_card_init(ReadIdCard* card, const char* controls);
void read_id_card_reset(ReadIdCard* card);
void read_id_card_set_waiting(ReadIdCard* card);
void read_id_card_set_reading(ReadIdCard* card);
void read_id_card_set_drive_not_ready(ReadIdCard* card, unsigned char st3);
void read_id_card_set_status(ReadIdCard* card, unsigned char st0,
                             unsigned char st1, unsigned char st2);
void read_id_card_set_chrn_valid(ReadIdCard* card, unsigned char c,
                                 unsigned char h, unsigned char r,
                                 unsigned char n);
void read_id_card_set_chrn_status(ReadIdCard* card,
                                  const char* status_value);
void read_id_card_set_detail_status(ReadIdCard* card,
                                    const char* status_value);
void read_id_card_set_detail_failure(ReadIdCard* card, const char* reason);
void read_id_card_render(const ReadIdCard* card, TestCardResult result);
void interactive_seek_card_init(InteractiveSeekCard* card,
                                const char* controls);
void interactive_seek_card_set_ready_fail(InteractiveSeekCard* card,
                                          unsigned char st3);
void interactive_seek_card_set_track(InteractiveSeekCard* card,
                                     unsigned char track);
void interactive_seek_card_set_last_st0(InteractiveSeekCard* card,
                                        unsigned char st0);
void interactive_seek_card_set_last_status(InteractiveSeekCard* card,
                                           const char* status_value);
void interactive_seek_card_set_pcn(InteractiveSeekCard* card,
                                   unsigned char pcn);
void interactive_seek_card_render(const InteractiveSeekCard* card,
                                  TestCardResult result);

/* Method-style wrapper aliases for card APIs used by call sites. */
#define set_track_loop_track(card_ptr, track_value)                              \
    track_loop_card_set_track((card_ptr), (track_value))
#define set_track_loop_counts(card_ptr, pass_value, fail_value)                  \
    track_loop_card_set_counts((card_ptr), (pass_value), (fail_value))
#define set_track_loop_last_status(card_ptr, status_value)                       \
    track_loop_card_set_last_status((card_ptr), (status_value))
#define set_track_loop_info_status(card_ptr, status_value)                       \
    track_loop_card_set_info_status((card_ptr), (status_value))
#define render_track_loop(card_ptr, result_value)                                \
    track_loop_card_render((card_ptr), (result_value))

#define set_rpm_loop_rpm(card_ptr, rpm_value, is_valid)                          \
    rpm_loop_card_set_rpm((card_ptr), (rpm_value), (is_valid))
#define set_rpm_loop_counts(card_ptr, pass_value, fail_value)                    \
    rpm_loop_card_set_counts((card_ptr), (pass_value), (fail_value))
#define set_rpm_loop_last_status(card_ptr, status_value)                         \
    rpm_loop_card_set_last_status((card_ptr), (status_value))
#define set_rpm_loop_info_status(card_ptr, status_value)                         \
    rpm_loop_card_set_info_status((card_ptr), (status_value))
#define render_rpm_loop(card_ptr, result_value)                                  \
    rpm_loop_card_render((card_ptr), (result_value))

#define set_motor_drive_unknown(card_ptr) motor_drive_card_set_unknown((card_ptr))
#define reset_motor_drive(card_ptr) motor_drive_card_reset((card_ptr))
#define set_card_motor_on(card_ptr) motor_drive_card_set_motor_on((card_ptr))
#define set_card_motor_off(card_ptr) motor_drive_card_set_motor_off((card_ptr))
#define set_motor_drive_status(card_ptr, have_st3, st3_value)                    \
    motor_drive_card_set_drive_status((card_ptr), (have_st3), (st3_value))
#define render_motor_drive(card_ptr, result_value)                               \
    motor_drive_card_render((card_ptr), (result_value))

#define set_read_id_probe_unknown(card_ptr)                                      \
    read_id_probe_card_set_unknown((card_ptr))
#define reset_read_id_probe(card_ptr) read_id_probe_card_reset((card_ptr))
#define set_id_waiting(card_ptr)                                                 \
    read_id_probe_card_set_id_status((card_ptr), "WAITING")
#define set_id_probing(card_ptr)                                                 \
    read_id_probe_card_set_id_status((card_ptr), "PROBING")
#define set_read_id_probe_status(card_ptr, have_st3, st3_value)                  \
    read_id_probe_card_set_drive_status((card_ptr), (have_st3), (st3_value))
#define set_precheck_ok(card_ptr)                                                \
    read_id_probe_card_set_id_status((card_ptr), "PRECHECK OK")
#define set_precheck_fail(card_ptr)                                              \
    read_id_probe_card_set_id_status((card_ptr), "PRECHECK FAIL")
#define set_id_chrn(card_ptr, c_value, h_value, r_value, n_value)                \
    read_id_probe_card_set_id_chrn((card_ptr), (c_value), (h_value), (r_value),   \
                                                                 (n_value))
#define set_id_failure(card_ptr, reason_text)                                    \
    read_id_probe_card_set_id_failure((card_ptr), (reason_text))
#define render_read_id_probe(card_ptr, result_value)                             \
    read_id_probe_card_render((card_ptr), (result_value))

#define set_recal_seek_unknown(card_ptr) recal_seek_card_set_unknown((card_ptr))
#define reset_recal_seek(card_ptr) recal_seek_card_reset((card_ptr))
#define set_ready_yes(card_ptr) recal_seek_card_set_ready_yes((card_ptr))
#define set_recal_seek_ready_fail_st3(card_ptr, st3_value)                       \
    recal_seek_card_set_ready_fail_st3((card_ptr), (st3_value))
#define set_recal_status_running(card_ptr)                                       \
    recal_seek_card_set_recal_status((card_ptr), RECAL_SEEK_STATUS_RUNNING)
#define set_recal_status_pass(card_ptr)                                          \
    recal_seek_card_set_recal_status((card_ptr), RECAL_SEEK_STATUS_PASS)
#define set_recal_status_fail(card_ptr)                                          \
    recal_seek_card_set_recal_status((card_ptr), RECAL_SEEK_STATUS_FAIL)
#define set_recal_status_skipped(card_ptr)                                       \
    recal_seek_card_set_recal_status((card_ptr), RECAL_SEEK_STATUS_SKIPPED)
#define set_seek_status_pending(card_ptr)                                        \
    recal_seek_card_set_seek_status((card_ptr), RECAL_SEEK_STATUS_PENDING)
#define set_seek_status_pass(card_ptr)                                           \
    recal_seek_card_set_seek_status((card_ptr), RECAL_SEEK_STATUS_PASS)
#define set_seek_status_fail(card_ptr)                                           \
    recal_seek_card_set_seek_status((card_ptr), RECAL_SEEK_STATUS_FAIL)
#define set_seek_status_skipped(card_ptr)                                        \
    recal_seek_card_set_seek_status((card_ptr), RECAL_SEEK_STATUS_SKIPPED)
#define set_detail_check_media(card_ptr)                                         \
    recal_seek_card_set_detail_status((card_ptr), "CHECK MEDIA")
#define set_detail_recal_cmd_fail(card_ptr)                                      \
    recal_seek_card_set_detail_status((card_ptr), "RECAL CMD FAIL")
#define set_detail_seek_cmd_fail(card_ptr)                                       \
    recal_seek_card_set_detail_status((card_ptr), "SEEK CMD FAIL")
#define set_detail_st0_pcn(card_ptr, st0_value, pcn_value)                       \
    recal_seek_card_set_detail_st0_pcn((card_ptr), (st0_value), (pcn_value))
#define set_detail_track(card_ptr, track_value)                                  \
    recal_seek_card_set_detail_track((card_ptr), (track_value))
#define render_recal_seek(card_ptr, result_value)                                \
    recal_seek_card_render((card_ptr), (result_value))

#define set_interactive_seek_ready_fail_st3(card_ptr, st3_value)                \
    interactive_seek_card_set_ready_fail((card_ptr), (st3_value))
#define set_interactive_seek_track(card_ptr, track_value)                        \
    interactive_seek_card_set_track((card_ptr), (track_value))
#define set_last_st0(card_ptr, st0_value)                                        \
    interactive_seek_card_set_last_st0((card_ptr), (st0_value))
#define set_last_no_seek(card_ptr)                                               \
    interactive_seek_card_set_last_status((card_ptr), "NO SEEK")
#define set_last_seek_cmd_fail(card_ptr)                                         \
    interactive_seek_card_set_last_status((card_ptr), "SEEK CMD FAIL")
#define set_last_wait_timeout(card_ptr)                                          \
    interactive_seek_card_set_last_status((card_ptr), "WAIT TIMEOUT")
#define set_pcn(card_ptr, pcn_value)                                             \
    interactive_seek_card_set_pcn((card_ptr), (pcn_value))
#define render_interactive_seek(card_ptr, result_value)                          \
    interactive_seek_card_render((card_ptr), (result_value))

#define set_waiting(card_ptr) read_id_card_set_waiting((card_ptr))
#define reset_read_id(card_ptr) read_id_card_reset((card_ptr))
#define set_reading(card_ptr) read_id_card_set_reading((card_ptr))
#define set_drive_not_ready(card_ptr, st3_value)                                 \
    read_id_card_set_drive_not_ready((card_ptr), (st3_value))
#define set_status(card_ptr, st0_value, st1_value, st2_value)                    \
    read_id_card_set_status((card_ptr), (st0_value), (st1_value), (st2_value))
#define set_chrn_valid(card_ptr, c_value, h_value, r_value, n_value)             \
    read_id_card_set_chrn_valid((card_ptr), (c_value), (h_value), (r_value),       \
                                                            (n_value))
#define set_chrn_invalid(card_ptr)                                               \
    read_id_card_set_chrn_status((card_ptr), "INVALID")
#define set_detail_id_ok(card_ptr)                                               \
    read_id_card_set_detail_status((card_ptr), "ID READ OK")
#define set_detail_failure(card_ptr, reason_text)                                \
    read_id_card_set_detail_failure((card_ptr), (reason_text))
#define render_read_id(card_ptr, result_value)                                   \
    read_id_card_render((card_ptr), (result_value))