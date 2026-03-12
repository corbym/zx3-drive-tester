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

#ifndef IOCTL_OTERM_FONT
#define IOCTL_OTERM_FONT 0x0802
#endif

/* uPD765A MSR bits */
#define MSR_RQM 0x80 /* Request for Master */
#define MSR_DIO 0x40 /* Data direction: 1 = FDC->CPU */
#define FDC_DRIVE 0 /* internal drive is drive 0 */
#define FDC_RQM_TIMEOUT 20000U
/*
 * Real +3 drive needs ~300-500 ms at operating speed.
 * delay_ms(500) = ~180 ms on emulator (600%/21 MHz) and ~1 s on real 3.5 MHz
 * hardware, which is ample for both paths.
 */
#define MOTOR_SPINUP_DELAY_MS 500U

static unsigned int dbg_seek_wait_loops;
static unsigned char dbg_seek_sense_tries;
static unsigned char dbg_seek_last_st0;
static unsigned char debug_enabled;
/*
 * Captured at startup before any paging changes, for debug visibility.
 * Shows the state DivMMC left the system in.
 */
static unsigned char startup_bank678;

/*
 * Character font buffer captured at startup before ROM paging is modified.
 *
 * Problem: DivMMC loads the program with the 48K BASIC ROM active (containing
 * the character font at $3D00). z88dk's output_char_32 driver uses font base
 * $3C00. When set_motor_off() writes the stale BANK678 shadow to $1FFD, it
 * clears bit 2 and switches the ROM bank away from the 48K BASIC ROM,
 * pointing address $3D00 to wrong data and corrupting all text output.
 *
 * Solution: Copy the 1 KB font ($3C00-$3FFF) to RAM at program start, then
 * redirect z88dk's terminal driver to use the RAM copy via IOCTL_OTERM_FONT.
 * Character rendering is then independent of any ROM paging changes.
 */
static unsigned char font_ram[1024];

/* Test results storage */
typedef struct {
  unsigned char motor_test_pass;
  unsigned char sense_drive_pass;
  unsigned char recalibrate_pass;
  unsigned char seek_pass;
  unsigned char read_id_pass;
} TestResults;

static TestResults results;

static void disable_terminal_auto_pause(void) {
  /* Avoid hidden key waits when output scrolls beyond one screen. */
  ioctl(1, IOCTL_OTERM_PAUSE, 0);
}

static const char* pass_fail(unsigned char ok) {
  return ok ? "PASS" : "FAIL";
}

static const char* yes_no(unsigned char flag) {
  return flag ? "YES" : "NO";
}

static void set_debug(unsigned char enabled) {
  debug_enabled = enabled ? 1 : 0;
  printf("Debug: %s\n", debug_enabled ? "ON" : "OFF");
  if (debug_enabled) {
    printf("DBG startup BANK678=0x%02X\n", startup_bank678);
  }
}

/* -------------------------------------------------------------------------- */
/* Low-level I/O                                                              */
/* -------------------------------------------------------------------------- */



static unsigned char fdc_msr(void) { return inportb(FDC_MSR_PORT); }

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
  if (!fdc_wait_rqm(0, FDC_RQM_TIMEOUT)) return 0;
  outportb(FDC_DATA_PORT, b);
  return 1;
}

static unsigned char fdc_read(unsigned char* out) {
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
  delay_ms(20);           /* let not-ready transition latch */
  fdc_drain_interrupts(); /* clear pending interrupt(s) */
  delay_ms(20);           /* catch late edge on first motor-off */
  fdc_drain_interrupts();
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
  unsigned char st0, pcn;
  unsigned int i;
  unsigned char tries;
  unsigned char busy_bit = (unsigned char)(1u << (drive & 0x03));

  /*
   * Wait for drive-busy bit to clear in MSR.
   * Some emulator runs hold the busy bit longer than expected.
   */
  for (i = 0; i < 15000U; i++) {
    if (!(fdc_msr() & busy_bit)) break;
  }
  dbg_seek_wait_loops = i;

  /*
   * Try Sense Interrupt a few times: this works on real hardware and avoids
   * long apparent hangs if emulator timing is jittery.
   */
  for (tries = 0; tries < 8; tries++) {
    dbg_seek_sense_tries = tries;
    if (cmd_sense_interrupt(&st0, &pcn) && (st0 & 0x20)) {
      dbg_seek_last_st0 = st0;
      *out_st0 = st0;
      *out_pcn = pcn;
      return 1;
    }
    delay_ms(5);
  }

  dbg_seek_last_st0 = st0;

  return 0;
}

void press_any_key(int interactive) {
  if (interactive == 1) {
    printf("\nPress any key\n");
    fflush(stdout);
    getchar();
  }
}

/* -------------------------------------------------------------------------- */
/* Tests                                                                      */
/* -------------------------------------------------------------------------- */

static void test_motor(int interactive) {
  printf("\n*** Motor test ***\n");
  printf("Motor ON\n");
  plus3_motor_on();
  printf("Motor OFF\n");
  plus3_motor_off();
  results.motor_test_pass = 1;
  press_any_key(interactive);
}

static void test_sense_drive(int interactive) {
  unsigned char st3 = 0;
  unsigned char st0 = 0, pcn = 0;
  unsigned char rid_ok = 0;
  unsigned char st1 = 0, st2 = 0, c = 0, h = 0, r = 0, n = 0;
  unsigned char have_st3 = 0;

  printf("\n*** Drive status ***\n");
  plus3_motor_on();

  /* 1) Raw drive lines (ST3), informational */
  have_st3 = cmd_sense_drive_status(FDC_DRIVE, 0, &st3);
  if (!have_st3) {
    printf("ST3: timeout\n");
  } else {
    printf("ST3 = 0x%02X\n", st3);
    printf("Ready: %s\n", yes_no(st3 & 0x20));
    printf("WProt: %s\n", yes_no(st3 & 0x40));
    printf("Track0: %s\n", yes_no(st3 & 0x10));
    printf("Fault: %s\n", yes_no(st3 & 0x80));
  }

  /* 2) A command that tends to surface "not ready" in ST0 (and steps hardware)
   */
  if (cmd_recalibrate(FDC_DRIVE) && wait_seek_complete(FDC_DRIVE, &st0, &pcn)) {
    printf("Recal ST0=0x%02X\n", st0);
    printf("PCN=%u NR=%s\n", pcn, yes_no(st0 & 0x08));
  } else {
    printf("Recal: timeout\n");
  }

  /* 3) Media probe: Read ID, best indicator for both hardware and emulation */
  rid_ok = cmd_read_id(FDC_DRIVE, 0, &st0, &st1, &st2, &c, &h, &r, &n);

  printf("Read ID result: ST0=0x%02X ST1=0x%02X ST2=0x%02X\n", st0, st1, st2);
  if (rid_ok) {
    printf("  CHRN: C=%u H=%u R=%u N=%u\n", c, h, r, n);
    printf("Probe: PASS\n");
  } else {
    printf("Probe: FAIL\n");
  }


  results.sense_drive_pass = rid_ok ? 1 : 0;
  printf("Overall: %s\n", pass_fail(results.sense_drive_pass));

  plus3_motor_off();
  press_any_key(interactive);
}

static void test_recalibrate(int interactive) {
  unsigned char st0 = 0, pcn = 0;

  printf("\n*** Recal track 0 ***\n");

  plus3_motor_on();

  if (!cmd_recalibrate(FDC_DRIVE)) {
    printf("FAIL: recal cmd\n");
    results.recalibrate_pass = 0;
    plus3_motor_off();
    return;
  }

  if (!wait_seek_complete(FDC_DRIVE, &st0, &pcn)) {
    printf("FAIL: wait timeout\n");
    results.recalibrate_pass = 0;
    plus3_motor_off();
    return;
  }

  printf("SenseInt: ST0=0x%02X, PCN=%u\n", st0, pcn);
  results.recalibrate_pass = (unsigned char)(pcn == 0);
  printf("%s\n", pass_fail(results.recalibrate_pass));

  plus3_motor_off();
  press_any_key(interactive);
}

static void test_seek(int interactive) {
  unsigned char st0 = 0, pcn = 0;
  unsigned char target = 2;

  printf("\n*** Seek track %u ***\n", target);

  plus3_motor_on();
  if (debug_enabled) {
    printf("DBG seek start MSR=0x%02X\n", fdc_msr());
  }

  if (!cmd_seek(FDC_DRIVE, 0, target)) {
    printf("FAIL: seek cmd\n");
    if (debug_enabled) {
      printf("DBG seek cmd MSR=0x%02X\n", fdc_msr());
    }
    results.seek_pass = 0;
    plus3_motor_off();
    return;
  }

  if (!wait_seek_complete(FDC_DRIVE, &st0, &pcn)) {
    printf("FAIL: wait timeout\n");
    if (debug_enabled) {
      printf("DBG wait loops=%u tries=%u st0=0x%02X msr=0x%02X\n",
             dbg_seek_wait_loops, dbg_seek_sense_tries, dbg_seek_last_st0,
             fdc_msr());
    }
    results.seek_pass = 0;
    plus3_motor_off();
    return;
  }

  printf("SenseInt: ST0=0x%02X, PCN=%u\n", st0, pcn);
  if (debug_enabled) {
    printf("DBG wait loops=%u tries=%u msr=0x%02X\n",
           dbg_seek_wait_loops, dbg_seek_sense_tries, fdc_msr());
  }
  results.seek_pass = (unsigned char)(pcn == target);
  printf("%s\n", pass_fail(results.seek_pass));

  plus3_motor_off();
  press_any_key(interactive);
}

static void test_seek_interactive(void) {
  unsigned char st0 = 0, pcn = 0;
  unsigned char target = 0;

  printf("\n*** Step seek ***\n");

  plus3_motor_on();

  for (;;) {
    printf("Track %u\n", target);
    printf("K up, J down, Q quit: ");
    int ch = getchar();
    printf("\n");
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
        return;
      default:
        break;
    }

    if (!cmd_seek(FDC_DRIVE, 0, target)) {
      printf("FAIL: seek cmd\n");
      plus3_motor_off();
      return;
    }

    if (!wait_seek_complete(FDC_DRIVE, &st0, &pcn)) {
      printf("FAIL: wait timeout\n");
      plus3_motor_off();
      return;
    }

    printf("SenseInt: ST0=0x%02X, PCN=%u\n", st0, pcn);
  }
}

static void test_read_id(int interactive) {
  unsigned char st0, st1, st2, c, h, r, n;
  unsigned char ok;

  printf("\n*** Read ID ***\n");
  printf("Needs readable disk\n");

  plus3_motor_on();

  /* Try to get to track 0 first */
  cmd_recalibrate(FDC_DRIVE);
  {
    unsigned char t0, tp;
    wait_seek_complete(FDC_DRIVE, &t0, &tp);
  }

  ok = cmd_read_id(FDC_DRIVE, 0, &st0, &st1, &st2, &c, &h, &r, &n);

  printf("Result: ST0=0x%02X ST1=0x%02X ST2=0x%02X\n", st0, st1, st2);
  printf("CHRN:   C=%u H=%u R=%u N=%u\n", c, h, r, n);

  results.read_id_pass = ok;
  printf("%s\n", pass_fail(ok));

  plus3_motor_off();
  
  press_any_key(interactive);
}

/* -------------------------------------------------------------------------- */
/* UI                                                                         */
/* -------------------------------------------------------------------------- */

static void print_results(void) {
  unsigned char total;

  printf("\n=== Results ===\n");
  printf("Motor: %s\n", pass_fail(results.motor_test_pass));
  printf("Drive: %s\n", pass_fail(results.sense_drive_pass));
  printf("Recal: %s\n", pass_fail(results.recalibrate_pass));
  printf("Seek : %s\n", pass_fail(results.seek_pass));
  printf("ReadID: %s\n", pass_fail(results.read_id_pass));

  total = results.motor_test_pass + results.sense_drive_pass +
          results.recalibrate_pass + results.seek_pass + results.read_id_pass;

  printf("TOTAL: %u/5 tests passed\n", total);
}

static void run_all_tests(void) {
  memset(&results, 0, sizeof(results));
  test_motor(0);
  test_sense_drive(0);
  test_recalibrate(0);
  test_seek(0);
  test_read_id(0);
  print_results();
  press_any_key(1);
}

static void menu_print(void) {
  printf("\n");
  printf("=== ZX +3 Disk Test ===\n");
  printf("1 Motor on/off\n");
  printf("2 Drive status\n");
  printf("3 Recal track 0\n");
  printf("4 Seek track 2\n");
  printf("5 Step seek\n");
  printf("6 Read ID\n");
  printf("A Run all\n");
  printf("D Debug ON\n");
  printf("E Debug OFF\n");
  printf("R Results\n");
  printf("C Clear\n");
  printf("Q Quit\n");
  printf("Select: ");
}

int main(void) {
  int ch;

  /* Fix DivMMC font corruption: copy ROM font to RAM before ROM paging changes.
     See font_ram comment for details. Must run BEFORE set_motor_off(). */
  memcpy(font_ram, (const void *)0x3C00, sizeof(font_ram));
  ioctl(1, IOCTL_OTERM_FONT, font_ram);

  /* Capture paging state at startup for debug output. */
  startup_bank678 = *(volatile unsigned char *)0x5B67;

  /* Initialize motor and paging to safe defaults. */
  set_motor_off();

  memset(&results, 0, sizeof(results));
  debug_enabled = 0;
  disable_terminal_auto_pause();

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
        test_seek(0);
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
      case 'D':
        set_debug(1);
        break;
      case 'E':
        set_debug(0);
        break;
      case 'R':
        print_results();
        break;
      case 'C':
        memset(&results, 0, sizeof(results));
        printf("Results cleared.\n");
        break;
      case 'Q':
        plus3_motor_off();
        printf("Exiting...\n");
        return 0;
      default:
        printf("Unknown option.\n");
        break;
    }
  }
  return 0;
}