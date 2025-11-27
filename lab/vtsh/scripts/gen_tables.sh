#!/usr/bin/env bash
# Generate test tables for ema-join experiments
# Usage: ./scripts/gen_tables.sh <Nleft> <Nright> <outdir>
set -euo pipefail
NLEFT=${1:-100}
NRIGHT=${2:-100}
OUTDIR=${3:-./test-files}
mkdir -p "$OUTDIR"
LEFT_FILE="$OUTDIR/left_${NLEFT}.txt"
RIGHT_FILE="$OUTDIR/right_${NRIGHT}.txt"
: > "$LEFT_FILE"
: > "$RIGHT_FILE"
printf "%d\n" "$NLEFT" > "$LEFT_FILE"
for i in $(seq 1 $NLEFT); do
  id=$i
  key="key$(printf "%06d" $((RANDOM % (NLEFT/10 + 1))))"
  printf "%d %s\n" "$id" "$key" >> "$LEFT_FILE"
done

printf "%d\n" "$NRIGHT" > "$RIGHT_FILE"
for i in $(seq 1 $NRIGHT); do
  id=$(( (RANDOM % (NRIGHT)) + 1 ))
  key="rkey$(printf "%06d" $((RANDOM % (NRIGHT/10 + 1))))"
  printf "%d %s\n" "$id" "$key" >> "$RIGHT_FILE"
done

echo "Generated: $LEFT_FILE and $RIGHT_FILE"
