# Code Review: zx3-disk-test

**Date:** 2026-03-22
**Reviewer:** Claude Code
**Branch:** main

---

## Summary

The codebase is a ZX Spectrum floppy disk tester targeting the ZX3 hardware. It is generally well-structured with clear separation of concerns across hardware abstraction (`disk_operations.c`), test logic (`disk_tester.c`), menu system (`menu_system.c`), and UI (`ui.c`). Low-level hardware interaction is handled carefully with timeouts and status checking. The main issues found are unsafe string operations, an uninitialized variable, a logic ordering error in RPM calculation, and several consistency/style problems.

| Severity     | Count |
|-------------|-------|
| High        | 4     |
| Medium      | 5     |
| Low         | 7     |
| **Total**   | **16** |

---

## High Severity

### H1 — Unsafe `sprintf()` without bounds

**File:** `menu_system.c:247`

```c
sprintf(out, "%u/5 PASS", (unsigned int)total_pass);
```

The destination buffer is 20 bytes. While in practice the values are bounded, `sprintf()` performs no length check.

**Fix:** Replace with `snprintf()`:

```c
snprintf(out, 20, "%u/5 PASS", (unsigned int)total_pass);
```

---

### H2 — Unsafe `strcpy()` without bounds

**File:** `menu_system.c:245`

```c
strcpy(out, "NO TESTS RUN");
```

Same 20-byte destination buffer as above. "NO TESTS RUN" is 13 bytes including the null terminator, so it currently fits, but this pattern is fragile.

**Fix:**

```c
strncpy(out, "NO TESTS RUN", 19);
out[19] = '\0';
```

---

### H3 — Uninitialized variable passed to function

**File:** `disk_tester.c:704–706`

```c
FdcSeekResult seek_result;
wait_seek_complete(FDC_DRIVE, &seek_result);
```

`seek_result` is not initialised before being passed to `wait_seek_complete()`. If the function reads from the struct before writing to it (e.g. in an early-exit path), this is undefined behaviour.

**Fix:**

```c
FdcSeekResult seek_result = {0};
```

---

### H4 — Logic error: division before zero-check in RPM calculation

**File:** `disk_tester.c:1118–1126`

```c
period_ms = (unsigned int) dticks * 20U;
if (period_ms == 0) {           // check is here...
    fail_count++;
    // ...
    continue;
}
rpm = (60000U + (period_ms / 2U)) / period_ms;  // ...but division already happened above
```

The flow is correct in that the `continue` fires before the division, however the division on the last line occurs only when `period_ms != 0`, so the logic is safe as written. That said, the zero-check is placed *after* the expression that would divide by zero if flow were ever reordered. This is an ordering fragility.

**Fix:** Move the guard immediately after computing `period_ms`, before any expression that uses it in the denominator, and add a comment:

```c
period_ms = (unsigned int) dticks * 20U;
if (period_ms == 0) {
    fail_count++;
    render_rpm_loop_period_bad(rpm, pass_count, fail_count);
    delay_ms(RPM_FAIL_DELAY_MS);
    continue;
}
/* period_ms is non-zero from here */
rpm = (60000U + (period_ms / 2U)) / period_ms;
```

---

## Medium Severity

### M1 — Inconsistent null-pointer checks on output parameters

**File:** `disk_operations.c:213–266`

Some functions validate output pointer parameters (`out_result` is checked in `cmd_read_data()` at line 278), but others do not:

```c
unsigned char cmd_sense_drive_status(unsigned char drive, unsigned char head,
                                     unsigned char *st3) {
    // st3 is never checked before dereference via fdc_read(st3)
```

**Fix:** Add null guards consistently at the top of each function that dereferences an output parameter:

```c
if (!st3) return 0;
```

---

### M2 — Track limit inconsistency: 39 vs 79

**File:** `disk_tester.c:634` and `disk_tester.c:929`

The interactive seek test limits tracks to 39:

```c
target = target < 39 ? target + 1 : 39;
```

But the read data loop uses 79:

```c
if (current_track < 79) current_track++;
```

Standard DD floppies have 80 tracks (0–79). Using 39 for the interactive test may be intentional for a specific test geometry, but it is undocumented and inconsistent.

**Fix:** Either document why 39 is correct for this test, or unify both to use a named constant:

```c
#define MAX_TRACK 79
```

---

### M3 — Busy-wait without delay in `loop_exit_requested()`

**File:** `disk_tester.c:328–345`

```c
while (break_pressed()) {  // tight spin
}
```

A spin loop polling I/O without any delay burns CPU cycles and may interfere with timing-sensitive operations elsewhere.

**Fix:** Add a short delay inside the loop:

```c
while (break_pressed()) {
    delay_ms(5);
}
```

---

### M4 — Magic numbers without named constants

**File:** `disk_tester.c` (multiple locations), `disk_operations.c:275`

Values like `285U`, `315U`, `120`, `39`, `79`, and the sector size code limit `3` appear without named constants, making intent unclear.

**Fix:** Define constants at the top of the relevant file or in a shared header:

```c
#define RPM_MIN          285U
#define RPM_MAX          315U
#define MAX_TRACK        79
#define SECTOR_SIZE_CODE_MAX  3
```

---

### M5 — Inverted return-value convention undocumented

**File:** `disk_operations.c` (all public functions)

Functions return `1` for success and `0` for failure, which is the inverse of the standard C convention (`0` = success). This is a valid choice but is not documented anywhere and could mislead contributors.

**Fix:** Add a comment at the top of `disk_operations.c` and `disk_operations.h`:

```c
/* Return convention: 1 = success, 0 = failure (inverted from standard C errno style) */
```

---

## Low Severity

### L1 — `static` forward declaration of `press_any_key` in header

**File:** `disk_tester.h:2`

```c
static void press_any_key(int interactive);
```

Declaring a `static` function in a header is unusual. If the header is ever included from a second translation unit, each unit gets its own copy. Since this function is only used in `disk_tester.c`, the forward declaration should either be moved into the `.c` file or removed.

---

### L2 — Unused parameter pattern is inconsistent

**File:** `disk_tester.c:676, 484`

Several test functions accept an `interactive` parameter but never use it:

```c
static void test_read_id(int interactive) {
    (void) interactive;
```

Other test functions do use the parameter. Either remove it from functions that don't need it, or document why it is present (e.g. future use, consistent API).

---

### L3 — Redundant nested cast

**File:** `ui.c:231`

```c
glyph = &font[((unsigned short)(unsigned char)ch) * 8U];
```

The inner `(unsigned char)` cast is redundant when the outer cast is `(unsigned short)`. Simplify:

```c
glyph = &font[((unsigned short)ch) * 8U];
```

---

### L4 — Frame ticker wraparound undocumented

**File:** `disk_tester.c:1097, 1101`

The 16-bit frame ticker wraps every ~22 minutes (65535 × 20 ms). The subtraction arithmetic handles short-lived deltas correctly, but there are no comments stating the assumption that measured periods will never exceed the wraparound window.

**Fix:** Add a comment near the timer logic:

```c
/* frame_ticks() is a 16-bit counter at 50 Hz (~22 min wraparound).
   Subtraction is safe for intervals < 1310 seconds. */
```

---

### L5 — FDC timeout return value is ambiguous

**File:** `disk_operations.c:124–132`

`fdc_wait_rqm()` returns `0` for both a timeout and a wrong data-direction response. This makes it harder to distinguish hardware failure modes during debugging. Low priority as the callers don't currently need the distinction, but worth noting for future diagnostics.

---

### L6 — Missing function-level documentation

**File:** `disk_operations.c`, `ui.c` (most functions)

Functions lack documentation for parameter constraints, return value semantics, and side effects. This is especially important for functions that interact with hardware interrupts or that have non-obvious pre/post conditions.

---

### L7 — Inconsistent error handling strategy across the codebase

Three distinct error handling styles are used:
1. Return `1`/`0` with output parameters (most of `disk_operations.c`)
2. Void functions that update global display state on error
3. Void functions with no error reporting

This makes the API harder to learn and audit. A consistent approach—even just documenting which style applies where—would help.

---

## Top 3 Recommended Fixes

1. **Replace `sprintf`/`strcpy` with `snprintf`/`strncpy`** (`menu_system.c:245,247`) — quick, low-risk, eliminates buffer-safety concern.
2. **Initialise `seek_result`** (`disk_tester.c:704`) — one-line fix, eliminates undefined behaviour.
3. **Add null guards to output parameters in `disk_operations.c`** — prevents potential null-dereference crashes from misuse.
