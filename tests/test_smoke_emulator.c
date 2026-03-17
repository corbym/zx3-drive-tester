#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>

#include "emulator_client.h"
#include "test_common.h"

static int file_exists(const char* path) { return access(path, F_OK) == 0; }

static int run_shell(const char* cmd) {
  int rc = system(cmd);
  if (rc == -1) return -1;
  if (WIFEXITED(rc)) return WEXITSTATUS(rc);
  return -1;
}

static int make_abs_path(const char* rel, char* out, size_t out_sz) {
  char cwd[PATH_MAX];
  if (!getcwd(cwd, sizeof(cwd))) return -1;
  if (snprintf(out, out_sz, "%s/%s", cwd, rel) >= (int)out_sz) return -1;
  return 0;
}

/* Global client and suite state. */
static emulator_client_t* global_client = NULL;
static char global_tap_path[PATH_MAX];
static char global_dsk_path[PATH_MAX];

/* Suite setup: build, resolve paths, start emulator. */
static int setup_suite(void) {
  if (run_shell("./build.sh > /tmp/zx3-build.log 2>&1") != 0) {
    fprintf(stderr, "    build failed; see /tmp/zx3-build.log\n");
    return 1;
  }

  if (!file_exists("out/disk_tester.tap") ||
      !file_exists("out/disk_tester_plus3.dsk")) {
    fprintf(stderr, "    expected TAP/DSK outputs were not produced\n");
    return 1;
  }

  if (make_abs_path("out/disk_tester.tap", global_tap_path,
                    sizeof(global_tap_path)) != 0 ||
      make_abs_path("out/disk_tester_plus3.dsk", global_dsk_path,
                    sizeof(global_dsk_path)) != 0) {
    fprintf(stderr, "    failed to resolve absolute TAP/DSK paths\n");
    return 1;
  }

  global_client = emulator_client_start();
  if (!global_client) {
    const char* required = getenv("ZX3_REQUIRE_EMU_SMOKE");
    if (required && strcmp(required, "1") == 0) {
      fprintf(stderr, "    emulator smoke required but unavailable\n");
      return 1;
    }
    printf("    note: emulator smoke skipped (zesarux not available)\n");
    return 0;
  }

  return 0;
}

/* Suite teardown: stop emulator. */
static void teardown_suite(void) {
  if (global_client) {
    emulator_client_stop(global_client);
    global_client = NULL;
  }
}

/* Test: Menu appears after TAP load. */
static int test_menu_appears_after_tap_load(void) {
  char ocr[65536];

  if (!global_client) {
    return 0; /* Skip if emulator not available. */
  }

  emulator_close_all_menus(global_client);
  emulator_hard_reset(global_client);
  usleep(200000);

  if (emulator_smartload(global_client, global_tap_path) != 0) {
    fprintf(stderr, "    failed to smartload TAP\n");
    return 1;
  }

  if (emulator_wait_for_ocr(global_client, "ZX +3 DISK TESTER",
                            "ENTER: SELECT", 30000, ocr,
                            sizeof(ocr)) != 0) {
    fprintf(stderr, "    timed out waiting for menu after TAP load\n");
    return 1;
  }

  return 0;
}

/* Test: Report card opens with R key and contains expected markers. */
static int test_report_card_opens(void) {
  char ocr[65536];

  if (!global_client) {
    return 0;
  }

  if (emulator_send_key(global_client, 82) != 0) { /* 'R' */
    fprintf(stderr, "    failed to send R key\n");
    return 1;
  }

  if (emulator_wait_for_ocr(global_client, "TEST REPORT CARD", "OVERALL [", 15000,
                            ocr, sizeof(ocr)) != 0) {
    fprintf(stderr, "    timed out waiting for report card\n");
    return 1;
  }

  return 0;
}

/* Test: Enter key returns from report card to menu. */
static int test_return_to_menu_from_report(void) {
  char ocr[65536];

  if (!global_client) {
    return 0;
  }

  if (emulator_send_key(global_client, 13) != 0) { /* Enter */
    fprintf(stderr, "    failed to send Enter key\n");
    return 1;
  }

  if (emulator_wait_for_ocr(global_client, "ZX +3 DISK TESTER",
                            "ENTER: SELECT", 15000, ocr,
                            sizeof(ocr)) != 0) {
    fprintf(stderr, "    timed out returning to menu\n");
    return 1;
  }

  return 0;
}

/* Test: Run-all completes and shows all tests passed. */
static int test_run_all_completes(void) {
  char ocr[65536];
  long long deadline;
  long long now_val;
  struct timespec ts;

  if (!global_client) {
    return 0;
  }

  /* Load DSK and start run-all. */
  if (emulator_smartload(global_client, global_dsk_path) != 0) {
    fprintf(stderr, "    failed to smartload DSK\n");
    return 1;
  }
  usleep(200000);

  if (emulator_send_key(global_client, 65) != 0) { /* 'A' */
    fprintf(stderr, "    failed to send A key\n");
    return 1;
  }

  /* Wait for completion with 3-minute timeout. */
  clock_gettime(CLOCK_MONOTONIC, &ts);
  deadline = (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL +
             180000;

  while (1) {
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now_val =
        (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL;

    if (now_val >= deadline) break;

    if (emulator_get_ocr(global_client, ocr, sizeof(ocr)) == 0) {
      if (strstr(ocr, "ZX +3 DISK TESTER") && strstr(ocr, "STATUS: 5/5 PASS")) {
        return 0;
      }

      if (strstr(ocr, "TEST REPORT CARD") && strstr(ocr, "STATUS: COMPLETE")) {
        emulator_send_key(global_client, 13); /* Press Enter */
      }
    }

    usleep(100000);
  }

  fprintf(stderr, "    timed out waiting for run-all completion\n");
  fprintf(stderr, "    last OCR:\n%s\n", ocr);
  return 1;
}

/* Test: Save staged screenshots that verify screen-clear behaviour. */
static int test_screen_capture_stages(void) {
  char shot_dir[PATH_MAX];
  char shot_path[PATH_MAX + 64];
  int i;
  struct {
    unsigned char key;
    unsigned long delay_us;
    const char* filename;
  } stages[] = {
    /* After TAP load: capture menu. */
    {0,  0,        "01_menu.bmp"},
    /* Run test 2. */
    {50, 3000000,  "02_test2_running.bmp"},
    {0,  3000000,  "03_after_test2.bmp"},
    /* Run test 5 (expect fail prompt). */
    {53, 3000000,  "04_test5_running.bmp"},
    {0,  1000000,  "05_test5_fail_prompt.bmp"},
    /* Dismiss with space. */
    {32, 2500000,  "06_menu_after_fail.bmp"},
    /* Run test 6 loop: capture two frames then exit. */
    {54, 5000000,  "07_test6_loop.bmp"},
    {0,  4000000,  "08_test6_loop2.bmp"},
    /* Exit loop with X. */
    {88, 3500000,  "09_menu_after_loop.bmp"},
  };

  if (!global_client) {
    return 0;
  }

  if (make_abs_path("out/screen-check", shot_dir, sizeof(shot_dir)) != 0) {
    fprintf(stderr, "    failed to resolve screen-check path\n");
    return 1;
  }

  if (run_shell("mkdir -p out/screen-check") != 0) {
    fprintf(stderr, "    failed to create out/screen-check directory\n");
    return 1;
  }

  emulator_close_all_menus(global_client);
  emulator_hard_reset(global_client);
  usleep(300000);

  if (emulator_smartload(global_client, global_tap_path) != 0) {
    fprintf(stderr, "    failed to smartload TAP for screen capture\n");
    return 1;
  }
  usleep(7000000); /* Wait for program to load and menu to appear. */

  for (i = 0; i < (int)(sizeof(stages) / sizeof(stages[0])); i++) {
    if (stages[i].key != 0) {
      if (emulator_send_key(global_client, stages[i].key) != 0) {
        fprintf(stderr, "    failed to send key %u before %s\n",
                (unsigned)stages[i].key, stages[i].filename);
        return 1;
      }
    }
    if (stages[i].delay_us > 0) {
      usleep((useconds_t)stages[i].delay_us);
    }
    snprintf(shot_path, sizeof(shot_path), "%s/%s", shot_dir, stages[i].filename);
    if (emulator_save_screen(global_client, shot_path) != 0) {
      fprintf(stderr, "    failed to save %s\n", stages[i].filename);
      return 1;
    }
  }

  printf("    screen captures written to %s\n", shot_dir);
  return 0;
}

/* Test: Motor/drive status menu option shows output. */
static int test_motor_status_menu(void) {
  char ocr[65536];

  if (!global_client) {
    return 0;
  }

  /* Ensure we're at the main menu. */
  if (emulator_wait_for_ocr(global_client, "ZX +3 DISK TESTER",
                            "ENTER: SELECT", 5000, ocr,
                            sizeof(ocr)) != 0) {
    fprintf(stderr, "    not at main menu\n");
    return 1;
  }

  if (emulator_send_key(global_client, 77) != 0) { /* 'M' */
    fprintf(stderr, "    failed to send M key\n");
    return 1;
  }

  /* Motor status test should start and show some diagnostic output. */
  if (emulator_wait_for_ocr(global_client, "Motor", "Status", 10000, ocr,
                            sizeof(ocr)) != 0) {
    fprintf(stderr, "    timed out waiting for motor status output\n");
    return 1;
  }

  return 0;
}

int main(void) {
  int failed = 0;

  if (setup_suite() != 0) {
    fprintf(stderr, "Suite setup failed\n");
    return 1;
  }

  const test_case cases[] = {
      {"menu appears after TAP load", test_menu_appears_after_tap_load},
      {"report card opens on R key", test_report_card_opens},
      {"Enter returns from report to menu", test_return_to_menu_from_report},
      {"run-all completes with 5/5 PASS", test_run_all_completes},
      {"motor status menu responds", test_motor_status_menu},
      {"screen capture stages", test_screen_capture_stages},
  };

  failed = run_suite(cases, sizeof(cases) / sizeof(cases[0]));

  teardown_suite();

  return failed ? 1 : 0;
}
