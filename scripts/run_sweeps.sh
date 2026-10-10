#!/usr/bin/env bash
# run_sweeps.sh - extra experiments (optional, after run_all.sh):
#   1. memory+disk variant with different buffer pool sizes  -> results/sweep/pool_<MB>.json
#   2. memory-only variant with different node (page) sizes  -> results/sweep/page_<B>.json
#
#   scripts/run_sweeps.sh DATA.csv [results_dir]
#
#   POOLS="16 256 1280"   PAGES="8192 16384 65536"   QUERIES=1000000
set -euo pipefail
cd "$(dirname "$0")/.."

DATA=${1:?usage: scripts/run_sweeps.sh DATA.csv [results_dir]}
OUT=${2:-results}/sweep
POOLS=${POOLS:-"16 256 1280"}
PAGES=${PAGES:-"8192 16384 65536"}
QUERIES=${QUERIES:-1000000}
MISS_QUERIES=${MISS_QUERIES:-100000}
INDEX_DIR=${INDEX_DIR:-$OUT}
mkdir -p "$OUT" "$INDEX_DIR"
make -s all

for p in $PAGES; do
  echo "=================== memory-only, node size $p B ==================="
  ./build/bptree_bench --mode memory --data "$DATA" --page-size "$p" --queries "$QUERIES" \
    --miss-queries "$MISS_QUERIES" --json "$OUT/page_$p.json" | tee "$OUT/page_$p.log"
done

for mb in $POOLS; do
  echo "=================== memory+disk, buffer pool $mb MB ==================="
  ./build/bptree_bench --mode hybrid --data "$DATA" --pool-mb "$mb" --queries "$QUERIES" \
    --miss-queries "$MISS_QUERIES" --dir "$INDEX_DIR" --json "$OUT/pool_$mb.json" --cleanup \
    | tee "$OUT/pool_$mb.log"
done
