# Agent Guidelines for zx3-disk-tester

## Project Overview

A low-level ZX Spectrum +3 floppy drive test utility written in C and Z80 assembly. The program directly communicates with the uPD765A floppy controller via I/O ports, providing a menu-driven interface for diagnosing floppy drive issues. Builds to both a TAP file (via DivMMC) and a bootable +3 DSK image.

## Architecture

### Layered Design

**Hardware Abstraction Layer** (`intstate.asm`)
- Low-level I/O port operations: `inportb()`, `outportb()` (extern functions called from C)
- Motor control: `set_motor_on()`, `set_motor_off()` with atomic read-modify-write to port `0x1FFD`
- Critical: preserves ROM paging bits when writing to `0x1FFD` (only touch bit 3)

**uPD765A FDC Communication** (`disk_operations.c` + `disk_tester.c`)
- Low-level FDC commands (recalibrate, seek, read ID, read data, sense drive status) live in `disk_operations.c`; higher-level test sequences and loops live in `disk_tester.c`
- Direct port-based protocol via `0x2FFD` (MSR status) and `0x3FFD` (data register)
- MSR bit polling: `MSR_RQM` (0x80) for request-to-master, `MSR_DIO` (0x40) for CPU→FDC direction
- Timing-sensitive: command/result bytes require `FDC_CMD_BYTE_GAP_UNITS` pacing; execution-phase data does not
- The +3's lack of TC (Terminal Count) hardware line means successful READ DATA completes with `ST0.IC=01` + `ST1.EN=1` (not clean zero); code treats this as success when no other error bits set
- **Return convention in `disk_operations.c`**: functions return `1` for success and `0` for failure — the inverse of the standard C errno convention. Do not confuse with standard `0 = success` idiom.

**Menu System** (`menu_system.c/h`)
- Direct keyboard scanning via ZX Spectrum keyboard matrix ports
- Key mapping table: each row/column/bit position maps to a printable character
- Navigation: up/down arrows (`MENU_KEY_UP`/`MENU_KEY_DOWN`) edit current selection; letters/digits execute actions
- Break detection: CAPS SHIFT + SPACE (latched across scan cycles to avoid chatter)
- Stateless design: menu layer only reads hardware; test logic updates state separately

**UI Rendering** (`ui.c/h`)
- Hardcoded Spectrum screen memory layout: `0x4000` pixels, `0x5800` attributes (color)
- Character ROM font copied to RAM on startup before ROM paging changes (preserves correctness under DivMMC)
- Build flag: `HEADLESS_ROM_FONT=1` forces ROM glyphs in headless/OCR builds for maximum OCR recognition stability
- Test card abstraction: structs contain title, controls text, and result lines; rendering handles styling via attribute bytes
- **Row dirty cache**: `ui_row_tag[24]` holds a per-row DJB2 checksum combined with the row style. `ui_render_cached_text_row` skips the expensive pixel+attr write when the tag matches. Always invalidate with `ui_reset_text_screen_cache()` before switching away from the text-screen path.
- **Label/value separation convention**: row headers (`"RESULT: "`, `"STATUS: "`, `"TRACK : "`, etc.) must never be embedded inside value strings. Use `test_card_set_labeled_value(card, row, LABEL, value, fallback)` for body rows. For the result row at the bottom, call `test_card_render(card, "RESULT: ", value)` — the label is passed as a separate argument and composed into a stack buffer at render time. This keeps each prefix string in the binary exactly once, reducing ROM pressure on the Z80.
- **Hex dump panel** (`ui_render_hex_dump_panel`): renders rows 10–22 as a hex+ASCII preview of sector data. Row 10 is a header ("DATA #N" where N is the 1-based scroll page); rows 11–22 show up to 12 rows × 8 bytes from the current scroll offset. Row 23 is the drive-status bar and is not overwritten by the hex panel. The panel has its own dirty tracking via `s_hex_dump_prev_scroll`: it skips all pixel writes when the scroll position is unchanged since the last render. Call `ui_reset_hex_dump_panel()` on track change to force a redraw on the next call. Call `ui_set_hex_dump_scroll(row)` to change the visible window; `ui_reset_hex_dump_panel` also resets scroll to 0.
- **Idle pump** (`ui_set_idle_pump`): optional callback invoked once per row during `ui_render_hex_dump_panel`. Use this to keep the key-scan latch alive during the (potentially long) 13-row render on Z80. Wire it to `pump_runtime_key_latch` at loop entry and clear it to NULL at exit — same pattern as `disk_operations_set_idle_pump`.

**Main Test Loop** (`disk_tester.c`)
- Holds state for all 7 tests: result codes, diagnostic bytes from FDC
- Tests communicate via predefined delay constants (e.g., `SEEK_BUSY_TIMEOUT_MS`, `DRIVE_READY_POLL_MS`)
- All I/O waits are busy-loop delay: `delay_ms()` uses spin counter to avoid relying on interrupts

### Critical Port Assignments

| Port   | Name | Direction | Use |
|--------|------|-----------|-----|
| `0x1FFD` | System Control | Write | Motor control (bit 3), ROM paging (bits 1-2), special paging (bit 0) |
| `0x2FFD` | FDC MSR | Read | Floppy controller status (RQM, DIO, busy bits) |
| `0x3FFD` | FDC Data | R/W | FDC command/result/data bytes |

**Motor bit behavior**: bit 3 of `0x1FFD` controls the drive motor; clearing it disables the drive. Always read `0x5B67` (BANK678 system variable) before writing to preserve other bits.
## Critical Memory Requirements
Always pay attention to the memory footprint of the program. Unless you are adding functionality, you should not be increasing the memory usage. The program must fit within the ZX Spectrum +3's 128KB RAM, and ideally should leave some headroom for future tests or features. Use the `memory_budget_regression_test.go` test to track memory usage changes.

When memory budget tests fail, treat `ZX3_STR_STORAGE` in `shared_strings.h` as a fallback strategy:
- The hook is paging-ready, not a free optimization by itself. It only helps after wiring it to a real section/bank placement in the build/link configuration.
- If enabled, move shared immutable strings out of the constrained default region and ensure call sites access them with the correct bank selected (or copy into always-mapped RAM before use).
- Verify with a fresh `./build.sh`, then rerun `go test ./tests -run TestTapCodeSizeBudget` and `go test ./tests -run TestMapHeapStackHeadroom`.
- If this strategy is used, document the chosen section/bank and paging assumptions in `shared_strings.h` and `build.sh` comments.

## Build & Test

### Build Variants

```bash
./build.sh                           # TAP + DSK, default UI
DEBUG=1 ./build.sh                   # Enable FDC debug output (MSR/ST0 trace)
HEADLESS_ROM_FONT=1 ./build.sh       #  use ROM font
```

Uses `z88dk`: `zcc +zx -clib=new` for TAP; `-subtype=plus3` for DSK.

### Testing

```bash
./run_tests.sh                       # Full suite: requires ZEsarUX + Go
```**Mandatory pre-test step**: always run a fresh build before any test run, including `go test ./tests -run TestTapCodeSizeBudget`.
- Why: budget/smoke tests read generated TAP/DSK/map outputs; without a rebuild they can evaluate stale files from an older code state.
- Safe sequence:
  1. `./build.sh`
  2. then run the specific test command(s)

**Go test structure** (`tests/smoke_emulator_test.go`):
- Single shared emulator instance per test run (started in `TestMain`)
- ZRCP protocol (Zenith Emulator Remote Control Protocol) via TCP socket on `localhost:9999` (default) or `ZX3_ZRCP_PORT`
- High-level operations: `Smartload(tapPath)` loads TAP; `SendKey()` sends keypress; `OCR()` extracts UI text via Tesseract
- Fallback logic: if TAP fails with "Drive not ready", auto-boots DSK image instead
- Approved baselines: `tests/approved/screen-check/*.bmp` (screenshot comparison via Go image/png)
- Env var `ZX3_REQUIRE_EMU_SMOKE=1`: CI mode—fails if emulator unavailable

## Key Patterns

### Port I/O Pattern
Always separate command/result phases due to MSR latching. Example:
```c
// Write command
outportb(FDC_DATA_PORT, READ_DATA);
outportb(FDC_DATA_PORT, (FDC_DRIVE | head));
// ... read results in reverse order ...
while ((inportb(FDC_MSR_PORT) & MSR_RQM) == 0) { /* wait */ }
```

### Timing & Tolerance
- Seek verification: retry up to `SEEK_SENSE_RETRIES` times with `SEEK_SENSE_RETRY_DELAY_MS` between polls
- Motor spin-up: `DRIVE_READY_TIMEOUT_MS` total; poll every `DRIVE_READY_POLL_MS`
- All delays use spin counter (`LOOPS_PER_MS=250`), no interrupts

### Menu State Pattern
Menu system reads hardware and outputs which key was pressed; separate test logic modifies test results. Example:
```c
int key = read_menu_key_blocking();
if (menu_resolve_action_key(key, &selected, &changed)) {
  // Execute action; update test state
  run_test(&results);
}
```

### Screen Update Pattern
Test logic writes results to a `TestCard` struct; rendering function converts to screen memory in one pass. No partial updates.

## Project-Specific Conventions

1. **External assembly functions** are declared `extern` and follow `_name_case` in the ASM file
2. **Z80 callconv**: small-model `__smallc` used; return values in HL, arguments on stack
3. **No dynamic allocation**: all test state and UI buffers are statically allocated
4. **Comments track uPD765A register names** (ST0, ST1, ST2, ST3) for clarity; cross-reference with [problemkaputt.de uPD765 docs](https://problemkaputt.de/zxdocs.htm#spectrumdiscspectrum3disccontrollernecupd765)
5. **Bit-field patterns**: keyboard matrix and FDC status use bitmasks in header constants; no bit-field structs
6. **CI screenshot baselines**: new test outputs stored in `tests/approved/screen-check/`; updated on each CI run

## Files You'll Likely Edit

- **`disk_tester.c`** – Main test logic, FDC command sequences, timing loops, runtime keymap
- **`disk_operations.c`** – Low-level uPD765A command implementations (recal, seek, read ID, read data)
- **`menu_system.c`** – Keyboard matrix scanning, key latching (if adding input)
- **`ui.c`** – Screen rendering, hex dump panel, row dirty cache, idle pump
- **`test_cards.c`** / **`test_cards.h`** – TestCard struct definitions, init/set/render helpers for each test screen
- **`shared_strings.c`** / **`shared_strings.h`** – Shared string literals (reduces duplicate string storage in CODE.bin)
- **`intstate.asm`** – Motor control, I/O atomicity (rarely touched; motor bit must always preserve other paging)
- **`build.sh`** – Build flags and z88dk arguments

## Debugging Tips

- **Debug build** (`DEBUG=1`): prints MSR/ST0/seek loop traces to console
- **Emulator OCR** (`tests/smoke_emulator_test.go`): `c.OCR()` returns raw Tesseract text; use for validating UI state
- **ZRCP direct**: port 9999 TCP, send `send-keys-ascii 25 65` for key 'A' (25 ms hold, ASCII code)
- **Disk image inspection**: `.dsk` files are raw CP/M disk images; use `image` tools to extract sectors if needed
- **Emulator `connect: connection refused` failures**: if a mid-suite emulator test times out or crashes, all subsequent tests in the same run will fail with this error. It is an infrastructure issue (emulator process died), not a code correctness problem. `TestTapCodeSizeBudget`, `TestMapHeapStackHeadroom`, and `TestMenuAppearsAfterTapLoad` are the most reliable correctness signals — they do not depend on emulator continuity.
- **Add a new FDC command**: implement it in `disk_operations.c` following the existing pattern (`fdc_wait_rqm` + `fdc_write`/`fdc_read`); remember return 1=success, 0=failure. Wire it into the test sequence in `disk_tester.c`.

## Optimizations
- Always refer to https://www.z88dk.org/wiki/doku.php?id=optimization and https://github.com/z88dk/z88dk/wiki/WritingOptimalCode for z88dk optimization tips.
- Run the memory_budget_regression_test.go test to see how much memory is used by the program. Optimize the code to reduce memory usage if required.

### Large struct locals — leave as locals
Although the z88dk optimization guide recommends preferring globals/statics over locals to avoid IX frame-pointer overhead, this advice applies primarily to **scalar variables and small structs accessed field-by-field** within the function.

All `TestCard`-derived structs (~455 bytes each) are **only ever taken as a pointer** (`&card`) and passed to init/set/render functions — the caller never touches fields directly. At `-SO3`, SCCZ80 handles this pattern efficiently without IX overhead: it allocates the struct by adjusting SP and computes `&card` from SP directly. No IX setup is needed.

**Empirical result**: making `ReportCard card` `static` in `ui_render_report_card()` added **+441 bytes** to CODE.bin. Do not apply the static-local optimization to card structs.

The static-variable optimization remains valid for frequently-read scalar state (e.g., the existing `static TestResults results`, `static unsigned char report_status_code`, etc.).

## Common Tasks

**Add a new test:**
1. Define result slot in `disk_tester.c` result array
2. Add menu item in `menu_system.c` (MENU_ITEMS array)
3. Implement FDC command sequence in test function
4. Render result in `ui.c` test card
5. Add smoke test case in `tests/smoke_emulator_test.go`

**Modify keyboard input:**
- Edit `menu_keymap[]` table in `menu_system.c` (row_port, bit_mask, mapped char)
- Latching logic prevents bouncing across multiple scan cycles
- Navigation (up/down) handled separately via `w_pressed()`/`s_pressed()` checks
- The read-track-data loop uses its own `keymap[]` table in `disk_tester.c` with a separate latch (`runtime_key_latched[]`, `runtime_pending_key`). This is polled via `pump_runtime_key_latch()`, called both at the top of each loop iteration and as the idle pump during FDC waits and hex panel rendering. Add loop-specific keys here, not to the menu keymap.

**Change UI layout:**
- Update screen dimensions and memory addresses in `ui.h` (e.g., `ZX_PIXELS_BASE`)
- Edit rendering loops in `ui.c` to reposition test cards
- Rebuild with default (no flags) for real hardware; OCR tests use the same standard ROM font build

# Less common tasks but worth mentioning

** When refactoring the FDC command sequence: **
- `disk_operations.c` contains the FDC command implementations
- `intstate.asm` contains the motor control logic

Always refer to 
- [Spectrum +3 disc controller (NEC uPD765) - problemkaputt.de](https://problemkaputt.de/zxdocs.htm#spectrumdiscspectrum3disccontrollernecupd765)
- [uPD765A Disc Controller Primer - muckypaws.com](https://muckypaws.com/2024/02/25/%C2%B5pd765a-disc-controller-primer/)

if you are unsure of how to implement a new FDC command or update the current ones.