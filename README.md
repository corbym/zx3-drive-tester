# zx3-disk-tester

A low-level ZX Spectrum +3 floppy drive test utility written in C and built with **z88dk**. It communicates directly with the internal +3 floppy controller (uPD765A compatible) via dedicated I/O ports and provides an interactive menu for drive diagnostics.

## Features

The program exposes a menu of low-level checks and tests:

- **Motor on/off** – Control via +3 system control port `0x1FFD` (bit 3)
- **Drive status** – Read drive status lines (ST3) and probe media presence
- **Recalibrate** – Seek to track 0 and report completion
- **Seek** – Seek to target track and verify position
- **Read ID** – Read sector ID from track 0 (requires readable disk)
- **Run all** – Execute all tests in sequence and summarize results
- **Debug mode** – Enable/disable verbose telemetry output (motor control, seek status, timeouts)

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
zcc +zx -vn -clib=new -create-app disk_tester.c intstate.asm -o ./out/disk_tester
```

## Running

### On real hardware (via DivMMC)

1. Build the project (produces `out/disk_tester.tap`)
2. Copy `disk_tester.tap` to your SD card
3. Boot your +3 with DivMMC, select the TAP file to load
4. Use the on-screen menu to run tests

### In emulator (ZEsarUX)

```sh
./tools/zesarux_smoketest.py [--debug-mode on|off]
```

**Options:**
- `--debug-mode on` – Enable debug output in the tester before running tests
- `--debug-mode off` – Disable debug output (default)
- `--machine P340` – Use ZX +3, 40-track drive (default)
- `--run-timeout 120` – Maximum seconds to wait for test completion
- `--no-build` – Skip build and use existing TAP

**Example:**
```sh
./tools/zesarux_smoketest.py --debug-mode on
```

The harness will:
1. Build the project
2. Start ZEsarUX in +3 mode
3. Load the TAP file
4. Set debug mode if requested
5. Run all tests (`A` key)
6. Capture and display results via OCR
7. Clean up and exit

## Debug Mode

Enable debug mode in the interactive menu with the `D` key. When active, the program prints:
- Startup paging state (`DBG startup BANK678=0x...`)
- Seek operation status (loop counts, sense interrupt retries, MSR/ST0 values)
- Timeouts and failure details

Disable with the `E` key.

## Notes

- **Emulator variability**: Some emulators don't accurately model all drive signal lines (write-protect, ready status, etc.). The program works around this by using disk-touching probes (like Read ID) as more reliable indicators than status-register bits alone.
- **Real hardware timing**: Timings are calibrated to work on both real hardware (~3.5 MHz) and emulators running at fast CPU speeds (100–600% emulation speed).
- **Interrupt handling**: The program uses IM 1 (single interrupt handler at `$0038`) and disables interrupts during critical I/O sequences to avoid race conditions.

## Files

- `disk_tester.c` – Main program logic (tests, menu, I/O handling)
- `disk_tester.h` – Helper declarations
- `intstate.asm` – Low-level port I/O and motor control (Z80 assembly)
- `build.sh` – Build script
- `tools/zesarux_smoketest.py` – Automated test harness with OCR validation

## License

This repository is released under the **Unlicense**.
