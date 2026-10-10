#!/usr/bin/env bash
# run_all.sh - build, detect the page size, run the three B+ tree variants, write a summary.
#
#   scripts/run_all.sh DATA.csv [results_dir]
#
# Optional environment variables:
#   POOL_MB=64            buffer pool size of the memory+disk variant
#   QUERIES=1000000       point lookups per variant
#   MISS_QUERIES=100000   lookups of absent keys
#   LIMIT=N               use only the first N rows (quick trial run)
#   MODES="memory hybrid disk"
#   INDEX_DIR=dir         where the index files go (default: results_dir); must be on a real
#                         disk, not tmpfs, or O_DIRECT is unavailable
set -euo pipefail
cd "$(dirname "$0")/.."

DATA=${1:?usage: scripts/run_all.sh DATA.csv [results_dir]}
OUT=${2:-results}
POOL_MB=${POOL_MB:-64}
QUERIES=${QUERIES:-1000000}
MISS_QUERIES=${MISS_QUERIES:-100000}
MODES=${MODES:-"memory hybrid disk"}
INDEX_DIR=${INDEX_DIR:-$OUT}
mkdir -p "$OUT" "$INDEX_DIR"

make -s all
scripts/sysinfo.sh "$INDEX_DIR" "$OUT/sysinfo.env" | tee "$OUT/sysinfo.txt"
# shellcheck disable=SC1091
source "$OUT/sysinfo.env"

EXTRA=()
if [ -n "${LIMIT:-}" ]; then EXTRA+=(--limit "$LIMIT"); fi

for mode in $MODES; do
  echo
  echo "=================== $mode ==================="
  ./build/bptree_bench --mode "$mode" --data "$DATA" --page-size "$PAGE_SIZE" \
    --pool-mb "$POOL_MB" --queries "$QUERIES" --miss-queries "$MISS_QUERIES" \
    --dir "$INDEX_DIR" --json "$OUT/$mode.json" ${EXTRA[@]+"${EXTRA[@]}"} | tee "$OUT/$mode.log"
done

if command -v python3 >/dev/null 2>&1; then
  python3 scripts/make_report.py "$OUT" > "$OUT/results.md"
  echo
  echo "summary table: $OUT/results.md"
fi
