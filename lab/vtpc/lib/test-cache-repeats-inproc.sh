#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${IO_LOADER_BIN:-$SCRIPT_DIR/io-loader-cache-test}"

FILE_SIZE_MB="${1:-500}"
READ_PASSES="${2:-5}"
BLOCK_SIZE="${BLOCK_SIZE:-1048576}"
TEST_FILE="${TEST_FILE:-$SCRIPT_DIR/test-cache-repeats.bin}"
RANGE="${RANGE:-0-0}"
WRITE_DIRECT="${WRITE_DIRECT:-on}"
READ_DIRECT="${READ_DIRECT:-on}"
ACCESS_TYPE="${ACCESS_TYPE:-sequence}"

if [[ ! -x "$BIN" ]]; then
  echo "error: binary not found: $BIN" >&2
  echo "build: cmake --build \"$SCRIPT_DIR/..\" --target io-loader-cache-test" >&2
  exit 1
fi

TOTAL_BYTES=$((FILE_SIZE_MB * 1024 * 1024))
if (( TOTAL_BYTES % BLOCK_SIZE != 0 )); then
  echo "error: file size must be divisible by block size" >&2
  exit 1
fi
BLOCK_COUNT=$((TOTAL_BYTES / BLOCK_SIZE))

OUT_DIR="$SCRIPT_DIR/io_load_monitoring_results/cache_repeats_inproc_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT_DIR"

echo "Scenario (in-process read repeats)"
echo "  binary      : $BIN"
echo "  file size   : ${FILE_SIZE_MB} MB"
echo "  read passes : $READ_PASSES"
echo "  direct W/R  : $WRITE_DIRECT/$READ_DIRECT"
echo "  output dir  : $OUT_DIR"

echo
echo "[1/2] Write once"
/usr/bin/time -v "$BIN" write "$BLOCK_SIZE" "$BLOCK_COUNT" "$TEST_FILE" "$RANGE" "$WRITE_DIRECT" "$ACCESS_TYPE" 1 \
  > "$OUT_DIR/write_output.txt" 2> "$OUT_DIR/write_time.txt"

echo "[2/2] Read repeats in one process"
/usr/bin/time -v "$BIN" read "$BLOCK_SIZE" "$BLOCK_COUNT" "$TEST_FILE" "$RANGE" "$READ_DIRECT" "$ACCESS_TYPE" "$READ_PASSES" \
  | tee "$OUT_DIR/read_output.txt" \
  2> "$OUT_DIR/read_time.txt"

echo
echo "Per-pass timings from io-loader-cache-test:"
grep -E '^Pass [0-9]+/[0-9]+:' "$OUT_DIR/read_output.txt" || true

echo
echo "Saved logs to: $OUT_DIR"
