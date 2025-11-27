#!/usr/bin/env bash
# Run ema-join-sm experiments for given sizes and repetitions
# Usage: ./scripts/run_ema_join_experiments.sh <Nleft> <Nright> <repetitions>
set -euo pipefail
NLEFT=${1:-100}
NRIGHT=${2:-100}
REPS=${3:-1}
OUTDIR=results/ema_${NLEFT}x${NRIGHT}_rep${REPS}
mkdir -p "$OUTDIR"
mkdir -p test-files
# generate tables
./scripts/gen_tables.sh "$NLEFT" "$NRIGHT" test-files > "$OUTDIR/gen.log" 2>&1
LEFT_FILE="test-files/left_${NLEFT}.txt"
RIGHT_FILE="test-files/right_${NRIGHT}.txt"
OUT_FILE="$OUTDIR/out.txt"
# start iostat in background (macOS/Linux friendly)
if command -v iostat >/dev/null 2>&1; then
  iostat 1 > "$OUTDIR/iostat.log" 2>&1 &
  IOSTAT_PID=$!
else
  IOSTAT_PID=0
fi
# collect top snapshot
top -l 1 > "$OUTDIR/top_snapshot.log" 2>&1 || top -b -n 1 > "$OUTDIR/top_snapshot.log" 2>&1 || true
# run program with /usr/bin/time -l (macOS) or /usr/bin/time -v (Linux)
TIME_BIN="/usr/bin/time"
TIME_OPTS=(-l)
# fallback for Linux where -l isn't available
if ! "$TIME_BIN" "${TIME_OPTS[@]}" true >/dev/null 2>&1; then
  TIME_OPTS=(-v)
fi
# run ema-join-sm with repetitions (if program accepts repetitions as 4th arg)
echo "Running: ./build/bin/ema-join-sm $LEFT_FILE $RIGHT_FILE $OUT_FILE $REPS"
"$TIME_BIN" "${TIME_OPTS[@]}" ./build/bin/ema-join-sm "$LEFT_FILE" "$RIGHT_FILE" "$OUT_FILE" "$REPS" 2> "$OUTDIR/time.log"
# stop iostat
if [ "$IOSTAT_PID" -ne 0 ]; then
  kill "$IOSTAT_PID" || true
fi
# save result header and count
if [ -f "$OUT_FILE" ]; then
  head -n 1 "$OUT_FILE" > "$OUTDIR/out_header.txt" || true
  tail -n +2 "$OUT_FILE" | wc -l > "$OUTDIR/out_rows_count.txt" || true
fi
# make a short markdown summary
cat > "$OUTDIR/summary.md" <<EOF
# EMA-join-sm experiment
Left: $LEFT_FILE
Right: $RIGHT_FILE
Out: $OUT_FILE
Repetitions: $REPS

## Logs
- time output: time.log
- iostat: iostat.log
- top snapshot: top_snapshot.log
- generated input: gen.log

## Result
- header (first line):

">
">
"$(cat "$OUTDIR/out_header.txt" 2>/dev/null || echo "(no out file)")"

- rows in result (excluding header): $(cat "$OUTDIR/out_rows_count.txt" 2>/dev/null || echo "(n/a)")
EOF

echo "Experiment finished, outputs in $OUTDIR"
