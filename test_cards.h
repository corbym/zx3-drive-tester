#pragma once

#include <stdio.h>
#include "shared_strings.h"

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
    unsigned char slot_state[4];
} ReportCard;

typedef struct { TestCard base; } DriveProbeCard;
typedef struct { TestCard base; } SeekReadCard;
typedef struct { TestCard base; } RpmLoopCard;
typedef struct { TestCard base; } InteractiveSeekCard;

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
    REPORT_CARD_SLOT_PROBE = 0U,
    REPORT_CARD_SLOT_SEEK = 1U,
    REPORT_CARD_SLOT_RPM = 2U,
    REPORT_CARD_SLOT_OVERALL = 3U,
} ReportCardSlot;

/* Generic card API shared by all test/report screens. */
void test_card_init(TestCard* card, const char* title, const char* controls,
                    unsigned char line_count);
void test_card_render(const TestCard* card, const char* result_label,
                      const char* result_value);
void test_card_render_result(const TestCard* card, TestCardResult result);

/* Report card */
void report_card_init(ReportCard* card);
void report_card_set_phase(ReportCard* card, ReportCardPhase phase);
void report_card_set_total_pass(ReportCard* card, unsigned char total_pass);
void report_card_set_slot_state(ReportCard* card, ReportCardSlot slot,
                                ReportCardState state);
void report_card_render(ReportCard* card);

/* Drive probe card — MOTOR/ST3/READY/WPROT/TRACK0/FAULT/ID (7 lines) */
void drive_probe_card_init(DriveProbeCard* card, const char* controls);
void drive_probe_card_set_motor(DriveProbeCard* card, unsigned char on);
void drive_probe_card_set_st3(DriveProbeCard* card, unsigned char have_st3,
                               unsigned char st3);
void drive_probe_card_set_id_status(DriveProbeCard* card, const char* status);
void drive_probe_card_set_id_chrn(DriveProbeCard* card, unsigned char c,
                                   unsigned char h, unsigned char r,
                                   unsigned char n);
void drive_probe_card_render(const DriveProbeCard* card, TestCardResult result);

/* Seek & read card — READY/RECAL/TRACK/SEEK/ID/PASS/FAIL (7 lines) */
void seek_read_card_init(SeekReadCard* card, const char* controls);
void seek_read_card_set_ready(SeekReadCard* card, unsigned char yes);
void seek_read_card_set_ready_fail_st3(SeekReadCard* card, unsigned char st3);
void seek_read_card_set_recal_status(SeekReadCard* card, RecalSeekStatus status);
void seek_read_card_set_track(SeekReadCard* card, unsigned char track);
void seek_read_card_set_seek_status(SeekReadCard* card, RecalSeekStatus status);
void seek_read_card_set_id_chrn(SeekReadCard* card, unsigned char c,
                                 unsigned char h, unsigned char r,
                                 unsigned char n);
void seek_read_card_set_id_status(SeekReadCard* card, const char* status);
void seek_read_card_set_counts(SeekReadCard* card, unsigned int pass,
                                unsigned int fail);
void seek_read_card_render(const SeekReadCard* card, TestCardResult result);

/* RPM loop card */
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
void rpm_loop_card_set_no_measurement(RpmLoopCard* card,
                                      unsigned char seen_other);
void rpm_loop_card_set_sample_ready(RpmLoopCard* card);
void rpm_loop_card_set_stopped(RpmLoopCard* card);
void rpm_loop_card_render(const RpmLoopCard* card, TestCardResult result);

/* Interactive seek card */
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

/* Aliases for interactive seek card used by call sites. */
#define set_interactive_seek_ready_fail_st3(card_ptr, st3_value)                \
    interactive_seek_card_set_ready_fail((card_ptr), (st3_value))
#define set_interactive_seek_track(card_ptr, track_value)                        \
    interactive_seek_card_set_track((card_ptr), (track_value))
#define set_last_st0(card_ptr, st0_value)                                        \
    interactive_seek_card_set_last_st0((card_ptr), (st0_value))
#define set_last_no_seek(card_ptr)                                               \
    interactive_seek_card_set_last_status((card_ptr), "NO SEEK")
#define set_last_seek_cmd_fail(card_ptr)                                         \
    interactive_seek_card_set_last_status((card_ptr), "SEEK FAIL")
#define set_last_wait_timeout(card_ptr)                                          \
    interactive_seek_card_set_last_status((card_ptr), zx3_str_timeout)
#define set_pcn(card_ptr, pcn_value)                                             \
    interactive_seek_card_set_pcn((card_ptr), (pcn_value))
#define render_interactive_seek(card_ptr, result_value)                          \
    interactive_seek_card_render((card_ptr), (result_value))

/* Aliases for RPM loop card used by call sites. */
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
