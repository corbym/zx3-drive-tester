#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

(cd "$ROOT_DIR" && go test ./tests -count=1 -v)

echo "All host Go tests passed"
