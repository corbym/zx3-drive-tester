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
static unsigned int dbg_seek_wait_loops;
static unsigned char dbg_seek_sense_tries;
static unsigned char dbg_seek_last_st0;

typedef struct {
    unsigned char status_0;
    unsigned char present_cylinder_number;
} FdcSenseInterruptResult;

/* -------------------------------------------------------------------------- */
/* Timing                                                                     */
/* -------------------------------------------------------------------------- */

/*
 * delay_ms()
 *
 * Busy-wait spin for approximately `millis` milliseconds.
 *
 * The +3 has no hardware timer accessible without interrupts, so timing is
 * achieved by iterating a calibrated inner loop LOOPS_PER_MS times per
 * millisecond. The volatile `delay_spin_sink` variable prevents the compiler
 * from optimising the loop away.
 *
 * This function is safe to call with interrupts disabled; it does not rely on
 * the IM1 frame counter or any peripheral. Accuracy is proportional to Z80
 * clock speed — adequate for FDC pacing but not cycle-precise.
 */
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

/*
 * frame_ticks()
 *
 * Returns the low 16 bits of the ZX Spectrum system FRAMES counter at 0x5C78.
 *
 * The Spectrum's IM1 interrupt handler increments FRAMES (a 3-byte little-
 * endian counter) at 50 Hz (every 20 ms). Reading the low two bytes gives a
 * free-running tick counter that wraps every ~21 minutes. Useful for
 * coarse elapsed-time measurement when interrupt-driven timing is acceptable.
 *
 * Note: this read is not atomic. If an interrupt fires between the two byte
 * reads a carry from low→mid may be missed. For the short intervals used in
 * FDC polling this is inconsequential.
 */
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

/*
 * fdc_write()
 *
 * Writes one byte to the FDC data register (0x3FFD) during a command or
 * parameter phase. Waits for the MSR to show RQM=1 and DIO=0 (FDC ready to
 * receive) before writing, then inserts a short pacing gap afterwards so the
 * FDC has time to latch the byte and update its internal state machine.
 *
 * Returns 1 on success, 0 if the RQM wait times out.
 */
static unsigned char fdc_write(unsigned char b) {
    if (!fdc_wait_rqm(0, FDC_RQM_TIMEOUT)) return 0;
    outportb(FDC_DATA_PORT, b);
    delay_us_approx(FDC_CMD_BYTE_GAP_UNITS);
    return 1;
}

/*
 * fdc_read()
 *
 * Reads one byte from the FDC data register (0x3FFD) during a result phase.
 * Waits for MSR RQM=1 and DIO=1 (FDC has a byte ready) before reading, then
 * inserts the same pacing gap used by fdc_write() so the FDC has time to
 * prepare the next result byte.
 *
 * Do NOT use this for execution-phase data bytes — use fdc_read_data_byte()
 * instead, which omits the post-read gap to avoid ST1.OR (overrun) errors.
 *
 * Returns 1 on success, 0 if the RQM wait times out.
 */
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

/*
 * plus3_motor_on()
 *
 * Activates the +3 floppy drive motor (bit 3 of port 0x1FFD via set_motor_on()
 * in intstate.asm, which preserves the ROM paging bits), then busy-waits
 * MOTOR_SPINUP_DELAY_MS for the spindle to reach operating speed.
 *
 * After spin-up, any ready-change interrupt the FDC generated is drained so it
 * does not interfere with subsequent commands.
 *
 * Always returns 0 — motor control is fire-and-forget. To confirm the drive is
 * actually ready (ST3.RDY set), call wait_drive_ready() after this function.
 */
unsigned char plus3_motor_on(void) {
    set_motor_on();
    delay_ms(MOTOR_SPINUP_DELAY_MS);
    fdc_drain_interrupts(); /* clear ready-change interrupt(s) */
    return 0;
}

/*
 * plus3_motor_off()
 *
 * Deactivates the drive motor and waits for the not-ready transition to
 * complete. The double settle+drain sequence is necessary because some ageing
 * +3 drive mechanics de-assert the READY line with a noticeable lag; a single
 * drain can miss the trailing edge. Both interrupt drains use
 * MOTOR_OFF_SETTLE_MS to give the FDC time to register the state change.
 *
 * The motor bit write is performed in intstate.asm to atomically preserve the
 * ROM paging bits in 0x1FFD.
 */
void plus3_motor_off(void) {
    set_motor_off();
    delay_ms(MOTOR_OFF_SETTLE_MS); /* let not-ready transition latch */
    fdc_drain_interrupts(); /* clear pending interrupt(s) */
    delay_ms(MOTOR_OFF_SETTLE_MS); /* catch late edge on first motor-off */
    fdc_drain_interrupts();
}

/* -------------------------------------------------------------------------- */
/* uPD765A command helpers                                                    */
/* -------------------------------------------------------------------------- */

/*
 * cmd_sense_interrupt()
 *
 * Issues SENSE INTERRUPT STATUS (0x08) to the uPD765A and collects ST0 and
 * PCN (Present Cylinder Number) into *out_result.
 *
 * This command must be issued after every SEEK or RECALIBRATE to acknowledge
 * the completion interrupt. If it is not issued, the FDC holds the interrupt
 * line asserted and subsequent commands will behave incorrectly on real
 * hardware (emulators are typically more forgiving).
 *
 * Special case: if the FDC has no interrupt pending it returns IC=11 (0x80,
 * "invalid command") as a single byte with no PCN following. This function
 * detects that and returns 1 with PCN=0 rather than timing out waiting for
 * the absent second byte.
 *
 * Returns 1 on success, 0 on FDC bus timeout.
 *
 * Reference: https://problemkaputt.de/zxdocs.htm#spectrumdiscspectrum3disccontrollernecupd765
 */
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

/*
 * cmd_sense_drive_status()
 *
 * Issues SENSE DRIVE STATUS (0x04) and returns ST3 in *st3. ST3 reports the
 * raw state of the drive signal lines at the time of the command:
 *
 *   Bit 7  FLT  — Fault (drive fault line, active high)
 *   Bit 6  WP   — Write Protect (disk is write-protected)
 *   Bit 5  RDY  — Ready (drive is spinning and ready)
 *   Bit 4  T0   — Track 0 (head is at cylinder 0)
 *   Bit 3  TS   — Two-Sided (drive supports two sides)
 *   Bit 2  HD   — Head address (side selected)
 *   Bits 1-0 DS — Drive select
 *
 * Returns 1 if the command completed, 0 on FDC bus timeout.
 * Note: a return value of 1 only means the FDC responded — check ST3.RDY
 * (bit 5) to determine whether the drive is actually ready.
 *
 * Reference: https://problemkaputt.de/zxdocs.htm#spectrumdiscspectrum3disccontrollernecupd765
 */
unsigned char cmd_sense_drive_status(unsigned char drive, unsigned char head,
                                     unsigned char *st3) {
    if (!fdc_write(FDC_CMD_SENSE_DRIVE_STATUS)) return 0;
    if (!fdc_write((head << 2) | (drive & 0x03))) return 0;
    if (!fdc_read(st3)) return 0;
    return 1;
}

/*
 * wait_drive_ready()
 *
 * Polls SENSE DRIVE STATUS every DRIVE_READY_POLL_MS until ST3.RDY (bit 5)
 * is set, or until DRIVE_READY_TIMEOUT_MS has elapsed.
 *
 * The final ST3 value is always written to *out_st3 (if non-NULL), whether
 * or not the drive became ready, so callers can inspect the full drive state
 * on timeout (e.g. to distinguish "not spinning" from "write protected").
 *
 * Returns 1 when RDY is seen, 0 on timeout.
 */
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

/*
 * cmd_recalibrate()
 *
 * Issues RECALIBRATE (0x07), which steps the head back to track 0. The
 * command is asynchronous: the FDC starts the step sequence and returns
 * immediately without waiting for the head to settle. The FDC sets the
 * drive-busy bit (D0B) in the MSR while the step is in progress.
 *
 * After calling this function, use wait_seek_complete() to poll for
 * completion and collect ST0/PCN. SENSE INTERRUPT STATUS must be issued
 * to acknowledge the completion interrupt before issuing any further command.
 *
 * Returns 1 if the two command bytes were accepted by the FDC, 0 on timeout.
 *
 * Reference: https://problemkaputt.de/zxdocs.htm#spectrumdiscspectrum3disccontrollernecupd765
 */
unsigned char cmd_recalibrate(unsigned char drive) {
    if (!fdc_write(FDC_CMD_RECALIBRATE)) return 0;
    if (!fdc_write(drive & 0x03)) return 0;
    delay_ms(SEEK_PREP_DELAY_MS);
    return 1;
}

/*
 * cmd_seek()
 *
 * Issues SEEK (0x0F), commanding the drive to step the head to cylinder `cyl`.
 * Like RECALIBRATE, this is asynchronous — the FDC sets D0B in the MSR and
 * returns immediately. Use wait_seek_complete() to detect completion.
 *
 * The second command byte encodes both head and drive:
 *   bits 3-2 = head number, bits 1-0 = drive number
 * matching the uPD765A HD/DS encoding used throughout all multi-byte commands.
 *
 * Returns 1 if the three command bytes were accepted, 0 on FDC bus timeout.
 *
 * Reference: https://problemkaputt.de/zxdocs.htm#spectrumdiscspectrum3disccontrollernecupd765
 */
unsigned char cmd_seek(unsigned char drive, unsigned char head,
                       unsigned char cyl) {
    if (!fdc_write(FDC_CMD_SEEK)) return 0;
    if (!fdc_write((head << 2) | (drive & 0x03))) return 0;
    if (!fdc_write(cyl)) return 0;
    delay_ms(SEEK_PREP_DELAY_MS);
    return 1;
}

/*
 * cmd_read_id()
 *
 * Issues a READ ID (MFM) command to the uPD765A and collects the result.
 *
 * The FDC waits for the next ID address mark to pass under the read head and
 * returns the CHRN bytes from that sector header — C (cylinder), H (head/side),
 * R (record/sector number), N (sector size code, where 0=128 B … 3=1024 B).
 * No sector data is transferred; this is purely a header probe.  It is the
 * cheapest way to confirm that the drive is spinning, the head is positioned
 * over a formatted track, and the media is readable.
 *
 * The three status bytes placed in out_result->status follow the standard
 * uPD765A meaning:
 *   ST0  — interrupt code in bits 7-6 (00 = normal termination)
 *   ST1  — per-sector error flags (missing AM, CRC, overrun, etc.)
 *   ST2  — data-field error flags (bad cylinder, control mark, etc.)
 *
 * Returns 1 (success) when ST0 interrupt code == 00 and both ST1 and ST2
 * are entirely clear — i.e. the FDC found a valid sector header with no
 * errors.  Returns 0 if any FDC bus transfer times out or if the status
 * bytes indicate an abnormal termination.
 */
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

    return (unsigned char) ((out_result->status.st0 & FDC_ST0_INTERRUPT_CODE_MASK) == 0 &&
                            out_result->status.st1 == 0 &&
                            out_result->status.st2 == 0);
}

/*
 * sector_size_from_n()
 *
 * Converts the N (sector size code) field from a uPD765A sector ID to the
 * corresponding byte count. The FDC defines N as a power-of-two exponent
 * relative to 128 bytes:
 *
 *   N=0 → 128 bytes,  N=1 → 256 bytes,  N=2 → 512 bytes,  N=3 → 1024 bytes
 *
 * N values above 3 are not valid for standard +3 media (the +3 format always
 * uses N=2, 512-byte sectors); this function returns 0 for those.
 *
 * Reference: https://problemkaputt.de/zxdocs.htm#spectrumdiscspectrum3disccontrollernecupd765
 */
unsigned int sector_size_from_n(unsigned char n) {
    if (n > 3) return 0;
    return (unsigned int) (128u << n);
}

/*
 * cmd_read_data()
 *
 * Issues READ DATA (MFM, 0x46) to transfer one sector from disk into data[].
 *
 * Command phase (9 bytes): opcode, drive/head, C, H, R, N, EOT, GPL, DTL.
 *   EOT is set to R so only the one requested sector is read.
 *   GPL uses the standard +3 value 0x2A.
 *   DTL is 0xFF (irrelevant when N > 0).
 *
 * Execution phase: data_len bytes read through fdc_read_data_byte() (no
 * inter-byte gap) to avoid ST1.OR (overrun) errors on the Z80 at 3.5 MHz.
 *
 * Result phase (7 bytes): ST0, ST1, ST2, C, H, R, N into *out_result.
 *
 * TC (Terminal Count) hardware note:
 *   On the +3 the TC pin of the uPD765A is not wired to software-controlled
 *   logic. A single-sector read therefore always ends with the FDC stepping
 *   past the sector boundary and raising ST1.EN (End of Cylinder, bit 7).
 *   This is treated as a successful completion when IC=01 in ST0, EN=1 in
 *   ST1, no other ST1 bits set, and ST2=0. See also AGENTS.md.
 *
 * Returns 1 on success, 0 on FDC bus timeout or an unrecognised error status.
 *
 * Reference: https://problemkaputt.de/zxdocs.htm#spectrumdiscspectrum3disccontrollernecupd765
 */
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

/*
 * read_id_failure_reason()
 *
 * Maps the ST1 and ST2 status bytes from a failed READ ID or READ DATA result
 * to a short human-readable string, suitable for display on a test card.
 *
 * Bits are checked in priority order (ST1 before ST2; most diagnostic first).
 * If no specific error bit is recognized, constructs a fallback hex string of
 * the form "S1=XX S2=XX" (e.g., "S1=04 S2=10") to expose unknown error states.
 *
 * ST1 bits checked (uPD765A):
 *   0x01 MA  — Missing ID address mark
 *   0x04 ND  — No data (sector not found)
 *   0x10 OR  — Overrun (CPU too slow during execution phase)
 *   0x20 DE  — Data error (CRC failure in ID field)
 *   0x80 EN  — End of cylinder (unexpected sector boundary)
 *
 * ST2 bits checked:
 *   0x01 MD  — Missing data address mark
 *   0x02 BC  — Bad cylinder (cylinder address mismatch)
 *   0x10 WC  — Wrong cylinder (head on different track than expected)
 *   0x20 DD  — Data error in data field (CRC failure)
 *   0x40 CM  — Control mark (deleted data address mark encountered)
 *
 * Reference: https://problemkaputt.de/zxdocs.htm#spectrumdiscspectrum3disccontrollernecupd765
 */
const char *read_id_failure_reason(unsigned char st1, unsigned char st2) {
    static char fallback[12];
    static const char hex[] = "0123456789ABCDEF";
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

    fallback[0] = 'S';
    fallback[1] = '1';
    fallback[2] = '=';
    fallback[3] = hex[st1 >> 4 & 0x0F];
    fallback[4] = hex[st1 & 0x0F];
    fallback[5] = ' ';
    fallback[6] = 'S';
    fallback[7] = '2';
    fallback[8] = '=';
    fallback[9] = hex[(st2 >> 4) & 0x0F];
    fallback[10] = hex[st2 & 0x0F];
    fallback[11] = '\0';
    return fallback;
}
