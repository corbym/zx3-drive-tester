# zx3-disk-tester

⚠️Not fully tested on a real +3 - no guarantee it won't break your floppies. You have been warned!⚠️

[![Smoke Test](https://github.com/corbym/zx3-disc-check/actions/workflows/smoke-test.yml/badge.svg)](https://github.com/corbym/zx3-disc-check/actions/workflows/smoke-test.yml)

A low-level ZX Spectrum +3 floppy drive test utility written in C and built with **z88dk**. It communicates directly with the internal +3 floppy controller (uPD765A compatible) via dedicated I/O ports and provides an interactive menu for drive diagnostics.

## Features

The program exposes a menu of low-level checks and tests:

- **Motor + drive status** – Combined motor control and ST3 status check
- **Drive probe (Read ID)** – Probe media and report controller status bytes
- **Recal + seek track 2** – Combined track-0 recalibrate then seek verification
- **Read ID** – Read sector ID from track 0 (requires readable disk)
- **Read track X data loop** – Continuously reads sector data on selected track (`J`/`K` to change track)
- **Disk RPM checker** – Continuous rotational-speed estimate from repeated ID reads; requires readable sector IDs
- **Run all** – Execute all tests in sequence and summarize results
- **Debug build** – Optional compile-time verbose telemetry for harness/debug sessions
- **Direct-key menu UI** – Menu navigation and hotkeys respond directly; confirmation prompts use `ENTER`
- **Retry loops** – Interactive seek, Read ID, track-data loop, and RPM checker repeat until `X` (or BREAK) is pressed
- **Low-memory default renderer** – The default build avoids the full-screen backbuffer path to preserve startup headroom on +3 memory limits

## Hardware & I/O Ports

Targets the **ZX Spectrum +3** internal floppy system:

| Port | Name | Direction | Purpose |
|------|------|-----------|---------|
| `0x1FFD` | System Control | Write | Motor control (bit 3), memory/ROM paging control |
| `0x2FFD` | FDC MSR | Read | Floppy controller main status register |
| `0x3FFD` | FDC Data | Read/Write | Floppy controller data register |

## DivMMC Font Corruption Fix

When loading via **DivMMC** on real +3 hardware, the program must copy the character font from ROM to RAM at startup, before any ROM paging changes occur. 

**Why**: DivMMC loads the program with the 48K BASIC ROM active (which contains the character font at `$3D00`). z88dk's character renderer expects the font at `$3C00`. When the program changes ROM paging (via port `0x1FFD`), the font data becomes inaccessible, causing all text output to appear corrupted.

**How it's fixed**: At the very start of `main()`, before any motor control code runs:
1. Copy 1 KB of font data from ROM (`$3C00–$3FFF`) into a RAM buffer
2. Redirect z88dk's terminal driver to use the RAM copy via `ioctl(1, IOCTL_OTERM_FONT, font_ram)`
3. All subsequent character rendering uses the RAM copy, independent of any ROM paging

This fix is transparent to the user and incurs minimal overhead (~1 KB RAM + ~100 bytes startup code).

## Build

### Prerequisites

- `z88dk` in your PATH (provides `zcc`, `z88dk-dis`)

### Quick build

```sh
./build.sh
```

This produces:
- `out/disk_tester.tap` – Loadable via DivMMC on real +3 or in ZEsarUX emulator
- `out/disk_tester_plus3.dsk` – Bootable +3 disk image (if you have a physical disk writer)

### Manual build

```sh
mkdir -p out
zcc +zx -vn -clib=new -create-app disk_tester.c menu_system.c intstate.asm -o ./out/disk_tester
```

## Running

### On real hardware (via DivMMC)

1. Build the project (produces `out/disk_tester.tap`)
2. Copy `disk_tester.tap` to your SD card
3. Boot your +3 with DivMMC, select the TAP file to load
4. Use the on-screen menu to run tests

### Smoke tests (ZEsarUX emulator)

```sh
./run_tests.sh
```

Requires `zesarux` on your PATH (or set `ZESARUX_BIN` to the binary path) and a working Go toolchain. If the emulator is not available the emulator-driven tests are skipped, unless `ZX3_REQUIRE_EMU_SMOKE=1` is set (used in CI to enforce they run).

Smoke tests require prebuilt artifacts at `out/disk_tester.tap` and `out/disk_tester_plus3.dsk` (for example from CI build output or a prior manual `./build.sh`). If either artifact is missing, the suite fails immediately.

The test suite:
1. Validates required prebuilt TAP/DSK artifacts are present
2. Starts ZEsarUX in headless +3 mode
3. Loads the TAP file and validates the main menu appears
4. Exercises key menu paths and validates UI responses via OCR
5. Runs all disk tests end-to-end and checks for completion status
6. Captures staged UI screenshots to `out/screen-check/` and compares them with approved baselines in `tests/approved/screen-check/`
7. Stops the emulator and reports results

Notes:
- `save-screen` via ZRCP supports `bmp`, `scr`, and `pbm` output formats.
- Screenshot artifacts land in `out/screen-check/` and are uploaded by CI.

### Latest CI Screen Pages

These screenshots are inlined from repository files that are refreshed by the Smoke Test workflow after successful tagged builds.

Version note: the screenshots below come from the latest successful tagged CI build that published screen pages, and your local/released version may differ.

![Main Menu](docs/screenshots/latest/03_menu_after_motor.bmp)

![Motor and Drive Status](docs/screenshots/latest/02_motor_status.bmp)

![Report Card](docs/screenshots/latest/04_report_card.bmp)

![Run All Complete](docs/screenshots/latest/06_run_all_complete.bmp)

## Read ID Result Notes

- `CHRN` is only meaningful when Read ID succeeds.
- On failure, the program now prints `CHRN: (invalid: Read ID failed)` and a reason decoded from ST1/ST2.
- Example: `ST1=0x01` means missing ID address mark, which usually indicates unreadable media or a drive/read-path fault.
- The RPM checker cannot measure speed if Read ID fails, because it uses repeated sector IDs as its rotation marker.

## Debug Build

Build with `DEBUG=1 ./build.sh` to enable debug output. In a debug build the program prints:
- Startup paging state (`DBG startup BANK678=0x...`)
- Seek operation status (loop counts, sense interrupt retries, MSR/ST0 values)
- Timeouts and failure details

For a denser human-facing font while keeping menu wording unchanged, build with
`COMPACT_UI=1 ./build.sh`.

For CI and OCR-driven smoke tests, keep `COMPACT_UI=0` (default) to preserve
stable character recognition.

The default non-compact build now also keeps the low-memory text/attribute
renderer enabled rather than the old full-screen backbuffer path. This trades
some cosmetic redraw smoothness for a large reduction in BSS usage and avoids
reintroducing the startup-memory regression.

## Docker Build & CI

### Build with Docker

A `Dockerfile` is provided that includes z88dk, ZEsarUX, and Python dependencies:

```sh
docker build -t zx3-disk-test:latest .
```

### Run smoke tests in Docker

```sh
docker run --rm -e ZX3_REQUIRE_EMU_SMOKE=1 zx3-disk-test:latest \
  bash -c "cd /workspace && ./run_tests.sh"
```

Or with volume mount to use your local checkout:

```sh
docker run --rm -e ZX3_REQUIRE_EMU_SMOKE=1 -v $(pwd):/workspace zx3-disk-test:latest \
  bash -c "cd /workspace && ./run_tests.sh"
```

### GitHub Actions

The repository includes three GitHub Actions workflows:
1. `.github/workflows/toolchain-image.yml` builds and publishes a prebuilt toolchain image to GHCR when the Dockerfile changes
2. `.github/workflows/smoke-test.yml` pulls that prebuilt image for normal CI runs and only falls back to a local build if the image is missing
3. `.github/workflows/manual-release.yml` supports release packaging/tag flow

The smoke workflow runs on pushes and PRs to `main`, `master`, and `develop`. It builds the project artifacts and runs the Go smoke test suite (including staged UI screenshot capture to `out/screen-check/`) in the prebuilt Docker image.

The workflow runs with `ZX3_REQUIRE_EMU_SMOKE=1` to enforce that emulator-driven tests must pass.

## Notes

- **Emulator variability**: Some emulators don't accurately model all drive signal lines (write-protect, ready status, etc.). The program works around this by using disk-touching probes (like Read ID) as more reliable indicators than status-register bits alone.
- **Real hardware timing**: Spin-up and seek polling/retry timings are tuned conservatively for older real +3 drives while remaining stable in emulator runs.
- **Interrupt handling**: The program uses IM 1 (single interrupt handler at `$0038`) and disables interrupts during critical I/O sequences to avoid race conditions.

## Files

- `disk_tester.c` – Main program logic (tests, menu, I/O handling)
- `disk_tester.h` – Helper declarations
- `intstate.asm` – Low-level port I/O and motor control (Z80 assembly)
- `build.sh` – Build script
- `run_tests.sh` – Run the Go smoke test suite (`go test ./tests`)
- `tests/emulator_harness.go` – Low-level ZEsarUX process and ZRCP socket primitives
- `tests/emulator_client.go` – Emulator client (lifecycle + high-level ZRCP operations)
- `tests/smoke_emulator_test.go` – Emulator-driven smoke test suite

## License

This repository is released under the **Unlicense**.
