/*
 * ZX Spectrum +3 Disk Drive Tester
 *
 * Talks directly to the internal +3 floppy controller, a uPD765A-compatible
 * FDC.
 *
 * Ports:
 *   0x1FFD  +3 system control (bit 3 controls motor), also memory control and
 * is write-only 0x2FFD  FDC Main Status Register (MSR), read-only 0x3FFD  FDC
 * Data Register, read/write
 *
 * Notes:
 * - Port 0x1FFD also controls memory/ROM paging. Only use bit 3 to avoid
 * messing paging up. The SYS_1FFD_SHADOW variable is used to keep track of the
 * current value of 0x1FFD so we can preserve the paging bits when toggling the
 * motor.
 *
 * Build examples:
 *   Bootable +3 disc (produces .dsk):
 *     zcc +zx -subtype=plus3 -create-app -lndos disk_tester.c -o
 * out/disk_tester
 *
 *   Tape (produces .tap):
 *     zcc +zx -create-app disk_tester.c -o out/disk_tester
 */

#include "disk_tester.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* +3 ports */
#define PLUS3_SYS_PORT 0x1FFD
#define FDC_MSR_PORT 0x2FFD
#define FDC_DATA_PORT 0x3FFD

#define PLUS3_MOTOR_BIT 0x08

/* uPD765A MSR bits */
#define MSR_RQM 0x80 /* Request for Master */
#define MSR_DIO 0x40 /* Data direction: 1 = FDC->CPU */
#define MSR_CB 0x10  /* Controller Busy */

#define FDC_DRIVE 0 /* internal drive is drive 0 */

#define SYS_1FFD_SHADOW (*(volatile unsigned char*)0x5B67)
/* Test results storage */
typedef struct {
  unsigned char motor_test_pass;
  unsigned char sense_drive_pass;
  unsigned char recalibrate_pass;
  unsigned char seek_pass;
  unsigned char read_id_pass;
} TestResults;

static TestResults results;

/* -------------------------------------------------------------------------- */
/* Low-level I/O                                                              */
/* -------------------------------------------------------------------------- */

static unsigned char fdc_msr(void) { return inp(FDC_MSR_PORT); }

static void plus3_motor(unsigned char on) {
  unsigned char v = SYS_1FFD_SHADOW; /* preserve paging bits */
  if (on)
    v |= 0x08; /* set bit 3 */
  else
    v &= (unsigned char)~0x08; /* clear bit 3 */

  SYS_1FFD_SHADOW = v; /* keep OS shadow in sync */
  outp(0x1FFD, v);     /* apply */
}
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
  if (!fdc_wait_rqm(0, 60000)) return 0;
  outp(FDC_DATA_PORT, b);
  return 1;
}

static unsigned char fdc_read(unsigned char* out) {
  if (!fdc_wait_rqm(1, 60000)) return 0;
  *out = inp(FDC_DATA_PORT);
  return 1;
}

/* -------------------------------------------------------------------------- */
/* uPD765A command helpers                                                    */
/* -------------------------------------------------------------------------- */

static unsigned char cmd_sense_interrupt(unsigned char* st0,
                                         unsigned char* pcn) {
  if (!fdc_write(0x08)) return 0; /* Sense Interrupt Status */
  if (!fdc_read(st0)) return 0;
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
  return 1;
}

static unsigned char cmd_seek(unsigned char drive, unsigned char head,
                              unsigned char cyl) {
  if (!fdc_write(0x0F)) return 0; /* Seek */
  if (!fdc_write((head << 2) | (drive & 0x03))) return 0;
  if (!fdc_write(cyl)) return 0;
  return 1;
}

static unsigned char cmd_read_id(unsigned char drive, unsigned char head,
                                 unsigned char* st0, unsigned char* st1,
                                 unsigned char* st2, unsigned char* c,
                                 unsigned char* h, unsigned char* r,
                                 unsigned char* n) {
  /* Read ID */
  if (!fdc_write(0x0A)) return 0;
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

/* Poll for completion of a seek/recalibrate by consuming Sense Interrupt
   Status. Returns 1 when SE bit observed in ST0, 0 on timeout or I/O failure.
 */
static unsigned char wait_seek_complete(unsigned int polls,
                                        unsigned char* out_st0,
                                        unsigned char* out_pcn) {
  while (polls--) {
    unsigned char st0, pcn;
    if (!cmd_sense_interrupt(&st0, &pcn)) return 0;

    /* ST0 bit 5 = Seek End (SE) */
    if (st0 & 0x20) {
      *out_st0 = st0;
      *out_pcn = pcn;
      return 1;
    }
  }
  return 0;
}

void press_any_key(int interactive) {
  if (interactive == 1) {
    printf("Press any key to continue...\n");
    getchar(); /* pause for user to inspect results */
  }
}
/* -------------------------------------------------------------------------- */
/* Tests                                                                      */
/* -------------------------------------------------------------------------- */

static void test_motor(int interactive) {
  unsigned int i;

  printf("\n*** TEST: Motor Control (0x1FFD bit 3) ***\n");
  printf("Turning motor ON...\n");
  plus3_motor(1);
  for (i = 0; i < 30000; i++);

  printf("FDC MSR: 0x%02X\n", fdc_msr());
  printf(
      "NOTE: The uPD765A MSR has no motor status bit, this only verifies we "
      "can still read MSR.\n");
  results.motor_test_pass = 1;
  press_any_key(interactive);
  printf("Turning motor OFF...\n");
  plus3_motor(0);
  for (i = 0; i < 8000; i++);
  press_any_key(interactive);
}
static void test_sense_drive(int interactive) {
  unsigned char st3 = 0;
  unsigned char st0 = 0, pcn = 0;
  unsigned char rid_ok = 0;
  unsigned char st1 = 0, st2 = 0, c = 0, h = 0, r = 0, n = 0;
  unsigned char have_st3 = 0;

  printf("\n*** TEST: Drive status (ST3) + media probe (Read ID) ***\n");
  printf(
      "Note: ST3 lines may be fixed in some emulators, Read ID is the reliable "
      "probe.\n");

  plus3_motor(1);

  /* 1) Raw drive lines (ST3), informational */
  have_st3 = cmd_sense_drive_status(FDC_DRIVE, 0, &st3);
  if (!have_st3) {
    printf("ST3: not available (timeout)\n");
  } else {
    printf("ST3 = 0x%02X\n", st3);
    printf("  Ready:         %s\n", (st3 & 0x20) ? "YES" : "NO");
    printf("  Write protect: %s\n", (st3 & 0x40) ? "YES" : "NO");
    printf("  Track 0:       %s\n", (st3 & 0x10) ? "YES" : "NO");
    printf("  Fault:         %s\n", (st3 & 0x80) ? "YES" : "NO");
  }

  /* 2) A command that tends to surface "not ready" in ST0 (and steps hardware)
   */
  if (cmd_recalibrate(FDC_DRIVE) && wait_seek_complete(250, &st0, &pcn)) {
    printf("SenseInt after recal: ST0=0x%02X, PCN=%u\n", st0, pcn);
    /* ST0 bit 3 = NR (Not Ready) */
    printf("  Not Ready (NR): %s\n", (st0 & 0x08) ? "YES" : "NO");
  } else {
    printf("Recalibrate: no completion (timeout)\n");
  }

  /* 3) Media probe: Read ID, best indicator for both hardware and emulation */
  rid_ok = cmd_read_id(FDC_DRIVE, 0, &st0, &st1, &st2, &c, &h, &r, &n);

  printf("Read ID result: ST0=0x%02X ST1=0x%02X ST2=0x%02X\n", st0, st1, st2);
  if (rid_ok) {
    printf("  CHRN: C=%u H=%u R=%u N=%u\n", c, h, r, n);
    printf("  Media probe: PASS (ID field readable)\n");
  } else {
    printf(
        "  Media probe: FAIL (no disk, not ready, wrong format, or read "
        "error)\n");
  }

  /*
   * PASS/FAIL logic:
   * - We PASS if Read ID works, because that proves the disk path works in both
   * real hardware and ZEsarUX.
   * - If Read ID fails, we FAIL even if ST3 says Ready (common emulator
   * behaviour).
   */
  results.sense_drive_pass = rid_ok ? 1 : 0;
  printf("Overall: %s\n", results.sense_drive_pass ? "PASS" : "FAIL");

  plus3_motor(0);
  press_any_key(interactive);
}


static void test_recalibrate(int interactive) {
  unsigned char st0 = 0, pcn = 0;

  printf("\n*** TEST: Recalibrate to Track 0 (0x07) ***\n");

  plus3_motor(1);

  if (!cmd_recalibrate(FDC_DRIVE)) {
    printf("FAIL: Could not issue recalibrate (timeout)\n");
    results.recalibrate_pass = 0;
    plus3_motor(0);
    return;
  }

  if (!wait_seek_complete(250, &st0, &pcn)) {
    printf("FAIL: Timeout waiting for completion\n");
    results.recalibrate_pass = 0;
    plus3_motor(0);
    return;
  }

  printf("SenseInt: ST0=0x%02X, PCN=%u\n", st0, pcn);
  results.recalibrate_pass = (unsigned char)(pcn == 0);
  printf("  %s\n", results.recalibrate_pass ? "PASS" : "FAIL");

  plus3_motor(0);
  press_any_key(interactive);
}

static void test_seek(int interactive) {
  unsigned char st0 = 0, pcn = 0;
  unsigned char target = 10;

  printf("\n*** TEST: Seek to Cylinder %u (0x0F) ***\n", target);

  plus3_motor(1);

  if (!cmd_seek(FDC_DRIVE, 0, target)) {
    printf("FAIL: Could not issue seek (timeout)\n");
    results.seek_pass = 0;
    plus3_motor(0);
    return;
  }

  if (!wait_seek_complete(250, &st0, &pcn)) {
    printf("FAIL: Timeout waiting for completion\n");
    results.seek_pass = 0;
    plus3_motor(0);
    return;
  }

  printf("SenseInt: ST0=0x%02X, PCN=%u\n", st0, pcn);
  results.seek_pass = (unsigned char)(pcn == target);
  printf("  %s\n", results.seek_pass ? "PASS" : "FAIL");

  plus3_motor(0);
  press_any_key(interactive);
}
static void test_seek_interactive(void) {
  unsigned char st0 = 0, pcn = 0;
  unsigned char target = 0;

  printf("\n*** TEST: Seek to Cylinder Step ***\n");

  plus3_motor(1);

  for (;;) {
    printf(
        "Target cyclinder is currently %u.\n"
        "Use k (up) or j (down) to adjust or Q to quit: ",
        target);
    int ch = getchar();
    printf("\n");
    ch = toupper((unsigned char)ch);

    switch (ch) {
      case 'J':
        target = target > 0 ? target - 1 : 0;
        break;
      case 'K':
        target = target < 39 ? target + 1 : 39;  // +3 has 40 cylinders numbered 0-39
        break;
      case 'Q':
        plus3_motor(0);
        return;
      default:
        break;
    }

    if (!cmd_seek(FDC_DRIVE, 0, target)) {
      printf("FAIL: Could not issue seek (timeout)\n");
      return;
    }

    if (!wait_seek_complete(250, &st0, &pcn)) {
      printf("FAIL: Timeout waiting for completion\n");
      return;
    }

    printf("SenseInt: ST0=0x%02X, PCN=%u\n", st0, pcn);
  }
}
static void test_read_id(int interactive) {
  unsigned char st0, st1, st2, c, h, r, n;
  unsigned char ok;

  printf("\n*** TEST: Read ID (0x0A) ***\n");
  printf("Reads an ID field on the current track. Requires a readable disk.\n");

  plus3_motor(1);

  /* Try to get to track 0 first */
  cmd_recalibrate(FDC_DRIVE);
  {
    unsigned char t0, tp;
    wait_seek_complete(250, &t0, &tp);
  }

  ok = cmd_read_id(FDC_DRIVE, 0, &st0, &st1, &st2, &c, &h, &r, &n);

  printf("Result: ST0=0x%02X ST1=0x%02X ST2=0x%02X\n", st0, st1, st2);
  printf("CHRN:   C=%u H=%u R=%u N=%u\n", c, h, r, n);

  results.read_id_pass = ok;
  printf("  %s\n", ok ? "PASS" : "FAIL");

  plus3_motor(0);
  press_any_key(interactive);
}

/* -------------------------------------------------------------------------- */
/* UI                                                                         */
/* -------------------------------------------------------------------------- */

static void print_results(void) {
  unsigned char total;

  printf("\n=====================================\n");
  printf("        TEST RESULTS SUMMARY\n");
  printf("=====================================\n");
  printf("Motor Control Test:      %s\n",
         results.motor_test_pass ? "PASS" : "FAIL");
  printf("Sense Drive Status Test: %s\n",
         results.sense_drive_pass ? "PASS" : "FAIL");
  printf("Recalibrate Test:        %s\n",
         results.recalibrate_pass ? "PASS" : "FAIL");
  printf("Seek Test:               %s\n", results.seek_pass ? "PASS" : "FAIL");
  printf("Read ID Test:            %s\n",
         results.read_id_pass ? "PASS" : "FAIL");
  printf("=====================================\n");

  total = results.motor_test_pass + results.sense_drive_pass +
          results.recalibrate_pass + results.seek_pass + results.read_id_pass;

  printf("TOTAL: %u/5 tests passed\n", total);
  printf("=====================================\n");
}

static void run_all_tests(void) {
  memset(&results, 0, sizeof(results));
  test_motor(0);
  test_sense_drive(0);
  test_recalibrate(0);
  test_seek(0);
  test_read_id(0);
  print_results();
}

static void menu_print(void) {
  printf("\n");
  printf("===================================\n");
  printf("  ZX Spectrum +3 Disk Drive Tester\n");
  printf("===================================\n");
  printf("1) Motor on/off\n");
  printf("2) Sense drive status\n");
  printf("3) Recalibrate to track 0\n");
  printf("4) Seek to track 10\n");
  printf("5) Seek to track (interactive)\n");
  printf("6) Read ID (track 0)\n");
  printf("A) Run all tests\n");
  printf("R) Show results summary\n");
  printf("C) Clear results\n");
  printf("Q) Quit\n");
  printf("-----------------------------------\n");
  printf("Select: ");
}

int main(void) {
  int ch;

  memset(&results, 0, sizeof(results));

  for (;;) {
    menu_print();
    ch = getchar();
    printf("\n");
    ch = toupper((unsigned char)ch);

    switch (ch) {
      case '1':
        test_motor(1);
        break;
      case '2':
        test_sense_drive(1);
        break;
      case '3':
        test_recalibrate(1);
        break;
      case '4':
        test_seek(1);
        break;
      case '5':
        test_seek_interactive();
        break;
      case '6':
        test_read_id(1);
        break;
      case 'A':
        run_all_tests();
        break;
      case 'R':
        print_results();
        break;
      case 'C':
        memset(&results, 0, sizeof(results));
        printf("Results cleared.\n");
        break;
      case 'Q':
        plus3_motor(0);
        printf("Exiting...\n");
        return 0;
      default:
        printf("Unknown option.\n");
        break;
    }
  }
  return 0;
}
