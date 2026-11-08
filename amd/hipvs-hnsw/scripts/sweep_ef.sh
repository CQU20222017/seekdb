#!/usr/bin/env bash
# Recall / latency sweep over ef_search at the benchmark shape used in BENCHMARK.md.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
for ef in 32 64 128 256; do
  out=$(./build/hipvs_bench --rows 20000 --dim 128 --queries 100 --ef-search "$ef" 2>&1)
  rec=$(printf '%s\n' "$out" | grep -o 'recall@10=[0-9.]*')
  bt=$(printf '%s\n' "$out" | grep -o 'build_seconds=[0-9.]*')
  echo "ef_search=$ef  $rec  $bt"
done
exit 0