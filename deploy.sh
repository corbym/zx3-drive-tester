#!/usr/bin/env sh
set -e

# Build headed (custom font) — for screenshot baselines and real hardware.
./build.sh

# Build headless (ROM font) — for OCR smoke tests.
# Goes into out/headless/ so it doesn't overwrite the headed artifacts.
HEADLESS_ROM_FONT=1 OUT_DIR=out/headless ./build.sh

if [ ! -f ./out/disk_tester.tap ]; then
  echo "ERROR: headed TAP missing at ./out/disk_tester.tap"
  exit 1
fi
if [ ! -f ./out/headless/disk_tester.tap ]; then
  echo "ERROR: headless TAP missing at ./out/headless/disk_tester.tap"
  exit 1
fi

echo "Build artifacts ready:"
ls -1 ./out/disk_tester.tap ./out/disk_tester_plus3.dsk
ls -1 ./out/headless/disk_tester.tap ./out/headless/disk_tester_plus3.dsk
