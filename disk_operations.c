/*
 * disk_operations.c
 *
 * Low-level floppy drive operations for the ZX Spectrum +3.
 *
 * Hardware interface:
 *   0x1FFD  System control — bit 3 = motor on/off (preserve other bits via ASM)
 *   0x2FFD  FDC Main Status Register (MSR), read-only
 *   0x3FFD  FDC Data Register, read/write
 *
 * All port I/O is performed through the extern primitives provided by
 * intstate.asm; no direct in/out instructions appear here.
 */

#include "disk_operations.h"

#include <intrinsic.h>

extern unsigned char inportb(unsigned short port);
extern void outportb(unsigned short port, unsigned char value);
extern void set_motor_on(void);
extern void set_motor_off(void);

/* -------------------------------------------------------------------------- */
/* Internal constants                                                         */
/* -------------------------------------------------------------------------- */

#define LOOPS_PER_MS 250U

#define FDC_MSR_PORT          0x2FFD
#define FDC_DATA_PORT         0x3FFD

#define MSR_RQM               0x80   /* Request for Master */
#define MSR_DIO               0x40   /* Data direction: 1 = FDC→CPU */

#define FDC_ST0_INTERRUPT_CODE_MASK 0xC0
#define FDC_ST0_INTERRUPT_INVALID_COMMAND 0x80
#define FDC_ST0_INTERRUPT_ABNORMAL_TERMINATION 0x40
#define FDC_ST0_SEEK_END 0x20

#define FDC_ST1_END_OF_CYLINDER 0x80
#define FDC_ST1_OTHER_ERROR_MASK 0x7F

#define FDC_ST3_READY 0x20

#define FDC_CMD_SENSE_INTERRUPT_STATUS 0x08
#define FDC_CMD_SENSE_DRIVE_STATUS 0x04
#define FDC_CMD_RECALIBRATE 0x07
#define FDC_CMD_SEEK 0x0F
#define FDC_CMD_READ_ID_MFM 0x4A
#define FDC_CMD_READ_DATA_MFM 0x46

#define FDC_READ_DATA_DEFAULT_GAP_LENGTH 0x2A
#define FDC_READ_DATA_UNUSED_DTL 0xFF

#define FDC_RQM_TIMEOUT       20000U
/* Command/result bytes need a small pacing gap; execution-phase data do not. */
#define FDC_CMD_BYTE_GAP_UNITS 4U

#define SEEK_PREP_DELAY_MS        2U
#define SEEK_BUSY_TIMEOUT_MS    900U
#define SEEK_SENSE_RETRIES       48U
#define SEEK_SENSE_RETRY_DELAY_MS 2U
#define DRIVE_READY_TIMEOUT_MS  500U
#define DRIVE_READY_POLL_MS       5U
#define MOTOR_OFF_SETTLE_MS      35U
/*
 * Real +3 drives vary with age; a slightly longer spin-up improves reliability
 * on older mechanics without affecting emulator runs materially.
 */
#define MOTOR_SPINUP_DELAY_MS   650U

/* -------------------------------------------------------------------------- */
/* Internal state                                                             */
/* -------------------------------------------------------------------------- */

static volatile unsigned int delay_spin_sink;

/* Debug counters — inspected by a debugger, never printf'd. */
static unsigned int  dbg_seek_wait_loops;
static unsigned char dbg_seek_sense_tries;
static unsigned char dbg_seek_last_st0;

typedef struct {
    unsigned char status_0;
    unsigned char present_cylinder_number;
} FdcSenseInterruptResult;

/* -------------------------------------------------------------------------- */
/* Timing                                                                     */
/* -------------------------------------------------------------------------- */

void delay_ms(unsigned int millis) {
    for (unsigned int outer = 0; outer < millis; outer++) {
        for (unsigned char inner = 0; inner < LOOPS_PER_MS; inner++) {
            delay_spin_sink++;
        }
    }
}

/* Sub-millisecond pacing for FDC command-byte gaps.
 * Bracketed with DI/EI so IM1 interrupts cannot stretch the gap. */
static void delay_us_approx(unsigned char units) {
    intrinsic_di();
    for (unsigned char i = 0; i < units; i++) {
        for (unsigned char j = 0; j < 4U; j++) {
            delay_spin_sink++;
        }
    }
    intrinsic_ei();
}

unsigned short frame_ticks(void) {
    volatile unsigned char *frames = (volatile unsigned char *) 0x5C78;
    return (unsigned short) (frames[0] | ((unsigned short) frames[1] << 8));
}

/* -------------------------------------------------------------------------- */
/* FDC low-level helpers                                                      */
/* -------------------------------------------------------------------------- */

/* Wait until RQM is set and DIO matches the desired direction.
   want_dio = 0 → CPU→FDC (write);  want_dio = 1 → FDC→CPU (read). */
static unsigned char fdc_wait_rqm(const unsigned char want_dio,
                                  unsigned int timeout) {
    while (timeout--) {
        unsigned char main_status_register = inportb(FDC_MSR_PORT);
        if ((main_status_register & MSR_RQM) &&
            (((main_status_register & MSR_DIO) != 0) == (want_dio != 0)))
            return 1;
    }
    return 0;
}

static unsigned char fdc_write(unsigned char b) {
    if (!fdc_wait_rqm(0, FDC_RQM_TIMEOUT)) return 0;
    outportb(FDC_DATA_PORT, b);
    delay_us_approx(FDC_CMD_BYTE_GAP_UNITS);
    return 1;
}

static unsigned char fdc_read(unsigned char *out) {
    if (!fdc_wait_rqm(1, FDC_RQM_TIMEOUT)) return 0;
    *out = inportb(FDC_DATA_PORT);
    delay_us_approx(FDC_CMD_BYTE_GAP_UNITS);
    return 1;
}

/* Execution-phase reads must be as tight as possible to avoid ST1.OR overruns. */
static unsigned char fdc_read_data_byte(unsigned char *out) {
    if (!fdc_wait_rqm(1, FDC_RQM_TIMEOUT)) return 0;
    *out = inportb(FDC_DATA_PORT);
    return 1;
}

/*
 * Consume up to 4 pending uPD765 interrupts using Sense Interrupt Status.
 * This prevents an interrupt storm after motor-ready transitions.
 */
static void fdc_drain_interrupts(void) {
    FdcSenseInterruptResult sense_result;

    for (unsigned char i = 0; i < 4; i++) {
        if (!fdc_write(FDC_CMD_SENSE_INTERRUPT_STATUS)) break;
        if (!fdc_read(&sense_result.status_0)) break;
        if ((sense_result.status_0 & FDC_ST0_INTERRUPT_CODE_MASK) ==
            FDC_ST0_INTERRUPT_INVALID_COMMAND)
            break;
        if (!fdc_read(&sense_result.present_cylinder_number)) break;
    }
}

/* -------------------------------------------------------------------------- */
/* Motor control                                                              */
/* -------------------------------------------------------------------------- */

unsigned char plus3_motor_on(void) {
    set_motor_on();
    delay_ms(MOTOR_SPINUP_DELAY_MS);
    fdc_drain_interrupts(); /* clear ready-change interrupt(s) */
    return 0;
}

void plus3_motor_off(void) {
    set_motor_off();
    delay_ms(MOTOR_OFF_SETTLE_MS);  /* let not-ready transition latch */
    fdc_drain_interrupts();         /* clear pending interrupt(s) */
    delay_ms(MOTOR_OFF_SETTLE_MS);  /* catch late edge on first motor-off */
    fdc_drain_interrupts();
}

/* -------------------------------------------------------------------------- */
/* uPD765A command helpers                                                    */
/* -------------------------------------------------------------------------- */

static unsigned char cmd_sense_interrupt(FdcSenseInterruptResult *out_result) {
    if (!out_result) return 0;

    if (!fdc_write(FDC_CMD_SENSE_INTERRUPT_STATUS)) return 0;
    if (!fdc_read(&out_result->status_0)) return 0;

    if ((out_result->status_0 & FDC_ST0_INTERRUPT_CODE_MASK) ==
        FDC_ST0_INTERRUPT_INVALID_COMMAND) {
        out_result->present_cylinder_number = 0;
        return 1;
    }

    if (!fdc_read(&out_result->present_cylinder_number)) return 0;
    return 1;
}

unsigned char cmd_sense_drive_status(unsigned char drive, unsigned char head,
                                     unsigned char *st3) {
    if (!fdc_write(FDC_CMD_SENSE_DRIVE_STATUS)) return 0;
    if (!fdc_write((head << 2) | (drive & 0x03))) return 0;
    if (!fdc_read(st3)) return 0;
    return 1;
}

unsigned char wait_drive_ready(unsigned char drive, unsigned char head,
                               unsigned char *out_st3) {
    unsigned char st3 = 0;
    for (unsigned int waited = 0; waited < DRIVE_READY_TIMEOUT_MS;
         waited += DRIVE_READY_POLL_MS) {
        if (cmd_sense_drive_status(drive, head, &st3) && (st3 & FDC_ST3_READY)) {
            if (out_st3) *out_st3 = st3;
            return 1;
        }
        delay_ms(DRIVE_READY_POLL_MS);
    }
    if (out_st3) *out_st3 = st3;
    return 0;
}

unsigned char cmd_recalibrate(unsigned char drive) {
    if (!fdc_write(FDC_CMD_RECALIBRATE)) return 0;
    if (!fdc_write(drive & 0x03)) return 0;
    delay_ms(SEEK_PREP_DELAY_MS);
    return 1;
}

unsigned char cmd_seek(unsigned char drive, unsigned char head,
                       unsigned char cyl) {
    if (!fdc_write(FDC_CMD_SEEK)) return 0;
    if (!fdc_write((head << 2) | (drive & 0x03))) return 0;
    if (!fdc_write(cyl)) return 0;
    delay_ms(SEEK_PREP_DELAY_MS);
    return 1;
}

unsigned char cmd_read_id(unsigned char drive, unsigned char head,
                          FdcResult *out_result) {
    if (!out_result) return 0;

    if (!fdc_write(FDC_CMD_READ_ID_MFM)) return 0;
    if (!fdc_write((head << 2) | (drive & 0x03))) return 0;

    /* Result phase: 7 bytes, mapped as ST0/ST1/ST2 + CHRN. */
    /* Result phase order is ST0/ST1/ST2 then CHRN. */
    if (!fdc_read(&out_result->status.st0)) return 0;
    if (!fdc_read(&out_result->status.st1)) return 0;
    if (!fdc_read(&out_result->status.st2)) return 0;
    if (!fdc_read(&out_result->chrn.c)) return 0;
    if (!fdc_read(&out_result->chrn.h)) return 0;
    if (!fdc_read(&out_result->chrn.r)) return 0;
    if (!fdc_read(&out_result->chrn.n)) return 0;

    return (unsigned char) (((out_result->status.st0 & FDC_ST0_INTERRUPT_CODE_MASK) == 0) &&
                            (out_result->status.st1 == 0) &&
                            (out_result->status.st2 == 0));
}

unsigned int sector_size_from_n(unsigned char n) {
    if (n > 3) return 0;
    return (unsigned int) (128u << n);
}

unsigned char cmd_read_data(unsigned char drive, unsigned char head,
                            unsigned char c, unsigned char h,
                            unsigned char r, unsigned char n,
                            FdcResult *out_result,
                            unsigned char *data, unsigned int data_len) {
    if (!out_result) return 0;

    if (!fdc_write(FDC_CMD_READ_DATA_MFM)) return 0;
    if (!fdc_write((head << 2) | (drive & 0x03))) return 0;
    if (!fdc_write(c)) return 0;
    if (!fdc_write(h)) return 0;
    if (!fdc_write(r)) return 0;
    if (!fdc_write(n)) return 0;
    if (!fdc_write(r)) return 0; /* EOT: read only this sector */
    if (!fdc_write(FDC_READ_DATA_DEFAULT_GAP_LENGTH)) return 0;
    if (!fdc_write(FDC_READ_DATA_UNUSED_DTL)) return 0;

    for (unsigned int i = 0; i < data_len; i++) {
        if (!fdc_read_data_byte(&data[i])) return 0;
    }

    if (!fdc_read(&out_result->status.st0)) return 0;
    if (!fdc_read(&out_result->status.st1)) return 0;
    if (!fdc_read(&out_result->status.st2)) return 0;
    if (!fdc_read(&out_result->chrn.c)) return 0;
    if (!fdc_read(&out_result->chrn.h)) return 0;
    if (!fdc_read(&out_result->chrn.r)) return 0;
    if (!fdc_read(&out_result->chrn.n)) return 0;

    /*
     * On +3 hardware, TC is not software-driven. Successful reads can terminate
     * with IC=01 in ST0 and EN=1 in ST1; treat that case as success when ST2
     * remains clear and no other ST1 bits are set.
     */
    if ((out_result->status.st0 & FDC_ST0_INTERRUPT_CODE_MASK) == 0 &&
        out_result->status.st1 == 0 && out_result->status.st2 == 0)
        return 1;

    if ((out_result->status.st0 & FDC_ST0_INTERRUPT_CODE_MASK) ==
            FDC_ST0_INTERRUPT_ABNORMAL_TERMINATION &&
        (out_result->status.st1 & FDC_ST1_END_OF_CYLINDER) != 0 &&
        (out_result->status.st1 & FDC_ST1_OTHER_ERROR_MASK) == 0 &&
        out_result->status.st2 == 0)
        return 1;

    return 0;
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
unsigned char wait_seek_complete(unsigned char drive, FdcSeekResult *out_result) {
    FdcSenseInterruptResult sense_result;
    unsigned int wait_ms;
    unsigned char drive_busy_bit = (unsigned char) (1u << (drive & 0x03));

    if (!out_result) return 0;

    sense_result.status_0 = 0;
    sense_result.present_cylinder_number = 0;

    /* Wait for drive-busy bit to clear in MSR.
     * Some emulator runs hold the busy bit longer than expected. */
    for (wait_ms = 0; wait_ms < SEEK_BUSY_TIMEOUT_MS; wait_ms++) {
        if (!(inportb(FDC_MSR_PORT) & drive_busy_bit)) break;
        delay_ms(1);
    }
    dbg_seek_wait_loops = wait_ms;

    /* Try Sense Interrupt a few times to tolerate jittery emulator timing. */
    for (unsigned char tries = 0; tries < SEEK_SENSE_RETRIES; tries++) {
        dbg_seek_sense_tries = tries;
        if (cmd_sense_interrupt(&sense_result) &&
            (sense_result.status_0 & FDC_ST0_SEEK_END)) {
            dbg_seek_last_st0 = sense_result.status_0;
            out_result->st0 = sense_result.status_0;
            out_result->pcn = sense_result.present_cylinder_number;
            return 1;
        }
        delay_ms(SEEK_SENSE_RETRY_DELAY_MS);
    }

    dbg_seek_last_st0 = sense_result.status_0;
    return 0;
}

const char *read_id_failure_reason(unsigned char st1, unsigned char st2) {
    if (st1 & 0x01) return "Missing ID address mark";
    if (st1 & 0x04) return "No data";
    if (st1 & 0x10) return "Overrun";
    if (st1 & 0x20) return "CRC error";
    if (st1 & 0x80) return "End of cylinder";
    if (st2 & 0x01) return "Missing data address mark";
    if (st2 & 0x02) return "Bad cylinder";
    if (st2 & 0x10) return "Wrong cylinder";
    if (st2 & 0x20) return "CRC in data field";
    if (st2 & 0x40) return "Control mark";
    return "Unspecified controller/media error";
}

