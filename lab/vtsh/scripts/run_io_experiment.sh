#!/usr/bin/env bash
# Run io-loader experiment with iostat and time logging
# Usage: ./scripts/run_io_experiment.sh <block_size> <block_count> <repetitions>
set -euo pipefail
BLOCK_SIZE=${1:-1048576}
BLOCK_COUNT=${2:-10000}
REPS=${3:-1}
OUTDIR=results/io_bs${BLOCK_SIZE}_bc${BLOCK_COUNT}_rep${REPS}
mkdir -p "$OUTDIR"
# iostat
if command -v iostat >/dev/null 2>&1; then
  iostat 1 > "$OUTDIR/iostat.log" 2>&1 &
  IOSTAT_PID=$!
else
  IOSTAT_PID=0
fi
# top snapshot
top -l 1 > "$OUTDIR/top_snapshot.log" 2>&1 || top -b -n 1 > "$OUTDIR/top_snapshot.log" 2>&1 || true

# run io-loader with /usr/bin/time
TIME_BIN="/usr/bin/time"
TIME_OPTS=(-l)
if ! "$TIME_BIN" "${TIME_OPTS[@]}" true >/dev/null 2>&1; then
  TIME_OPTS=(-v)
fi
CMD=("./build/bin/io-loader" --rw write --block_size "$BLOCK_SIZE" --block_count "$BLOCK_COUNT" --file ./bin/files/data.dat --direct on --type random --repetitions "$REPS")

echo "Running: ${CMD[*]}"
"$TIME_BIN" "${TIME_OPTS[@]}" "${CMD[@]}" 2> "$OUTDIR/time.log"

if [ "$IOSTAT_PID" -ne 0 ]; then
  kill "$IOSTAT_PID" || true
fi

cat > "$OUTDIR/summary.md" <<EOF
# io-loader experiment
block_size: $BLOCK_SIZE
block_count: $BLOCK_COUNT
repetitions: $REPS

## Logs
- time: time.log
- iostat: iostat.log
- top snapshot: top_snapshot.log

EOF

echo "Experiment finished, outputs in $OUTDIR"
