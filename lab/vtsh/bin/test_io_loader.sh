#!/bin/bash
set +m

BS=$2
COUNT=$3
FILE=$4
RANGE=$5
DIRECT=$6
TYPE=$7
OUT_DIR="./io_load_monitoring_results"

mkdir -p "$OUT_DIR"

run_load_test() {
    local RW=$1
    local BLOCK_SIZE=$2
    local BLOCK_COUNT=$3
    local FILE=$4
    local RANGE=$5
    local DIRECT=$6
    local TYPE=$7
    local TAG="${RW}_${TYPE}_dir${DIRECT}"

    echo "Test: $RW $TYPE (direct=$DIRECT)"
    echo

    iostat 1 > "$OUT_DIR/iostat_log_${TAG}.txt" 2>/dev/null &
    IOSTAT_PID=$!

    top -l 0 -stats pid,command,cpu,mem,time,state,csw > "$OUT_DIR/top_log_${TAG}.txt" 2>/dev/null &
    TOP_PID=$!

    sleep 1

    /usr/bin/time -l ./io-loader \
    "$RW" \
    "$BLOCK_SIZE" \
    "$BLOCK_COUNT" \
    "$FILE" \
    "$RANGE" \
    "$DIRECT" \
    "$TYPE" \
    2> >(tee "$OUT_DIR/time_${TAG}.txt" >&2) \
    | tee "$OUT_DIR/output_${TAG}.txt"

    wair $!

    kill "$IOSTAT_PID" "$TOP_PID" 2>/dev/null
    wait "$IOSTAT_PID" 2>/dev/null
    wait "$TOP_PID" 2>/dev/null

    echo
    echo "--- TOP (CPU/MEM) ---"
    grep io-load "$OUT_DIR/top_log_${TAG}.txt" | tail -10
    echo
    echo "--- IOSTAT (Disk MB/s) ---"
    tail -10 "$OUT_DIR/iostat_log_${TAG}.txt"
}

echo "TEST"
echo "1) write sequence off"
echo "2) write random off"
echo "3) write sequence on"
echo "4) write random on"
echo "5) read sequence off"
echo "6) read random off"
read -p "Choice: " CHOICE
echo

case $CHOICE in
    1) run_load_test write $BS $COUNT $FILE 0-0 off sequence ;;
    2) run_load_test write $BS $COUNT $FILE 0-0 off random ;;
    3) run_load_test write $BS $COUNT $FILE 0-0 on sequence ;;
    4) run_load_test write $BS $COUNT $FILE 0-10737418239 on random ;;
    5) run_load_test read $BS $COUNT $FILE 0-0 off sequence ;;
    6) run_load_test read $BS $COUNT $FILE 0-0 off random ;;
    *) echo "invalid" ;;
esac