#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR="$ROOT_DIR/out/host-tests"

mkdir -p "$BUILD_DIR"

cc -std=c99 -Wall -Wextra -Wno-unused-function -pedantic \
  "$ROOT_DIR/tests/emulator_harness.c" \
  "$ROOT_DIR/tests/emulator_client.c" \
  "$ROOT_DIR/tests/test_smoke_emulator.c" \
  -o "$BUILD_DIR/test_smoke_emulator"

(cd "$ROOT_DIR" && "$BUILD_DIR/test_smoke_emulator")

echo "All host C tests passed"
