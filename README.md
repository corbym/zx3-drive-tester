# zx3-disc-check

A small ZX Spectrum +3 floppy drive check utility written in C and built with **z88dk**. It talks to the internal +3 floppy controller (uPD765A compatible) via the +3 FDC ports and provides a simple interactive test menu.

## What it does

The program exposes a menu of low-level checks against the +3’s internal drive:

- Motor on/off (via +3 system control port `0x1FFD`, motor bit 3)
- Sense drive status (ST3) and a basic media probe
- Recalibrate (seek to track 0) and report completion status
- Seek to a target track and report completion status
- Read ID (track 0) where supported

There is also a “Run all tests” option to execute the non-interactive tests in sequence and print a summary.

## Hardware and port usage

This project targets the **ZX Spectrum +3** internal floppy system:

- `0x2FFD`: FDC Main Status Register (MSR), read-only
- `0x3FFD`: FDC Data Register, read/write
- `0x1FFD`: +3 system control (motor on bit 3), also memory/ROM paging, write-only

## Build

### Prerequisites

- `z88dk` in your PATH (provides `zcc`)

### Build script

The repo includes `build.sh`, which builds into `./out` using `zcc +zx` with `-clib=new`:

```sh
./build.sh
```

Equivalent manual build:

```sh
mkdir -p out
zcc +zx -vn -create-app disk_tester.c -o ./out/disk_tester

# newlib build (current project setting)
zcc +zx -vn -clib=new -create-app disk_tester.c intstate.asm -o ./out/disk_tester
```

Deploy wrapper (build + artifact check):

```sh
./deploy.sh
```

ZEsarUX smoke test harness:

```sh
./tools/zesarux_smoketest.py
```

This starts ZEsarUX in `+3` mode, enables ZRCP on TCP `10000`, smartloads `out/disk_tester.tap`, runs `A` (Run all), reports OCR summary, and then shuts ZEsarUX down.

### Output

With `-create-app` on `+zx`, z88dk/appmake produces a loadable artefact alongside the binary in `out/` (commonly a `.tap`, depending on your z88dk/appmake setup and options).

## Run

- Load the produced output in your emulator or on real hardware.
- Use the on-screen menu to run individual tests or “Run all tests”.

## Notes on emulators

Some emulators do not model all drive “signal line” bits (for example, write-protect and ready) in the same way as real hardware. In those cases, disk-touching probes (like Read ID) are a more reliable indicator than ST3 bits alone.

## Files

- `disk_tester.c`: main program and tests
- `disk_tester.h`: small helper declarations used by the main program
- `build.sh`: build script

## License

This repository is released under the **Unlicense**.
