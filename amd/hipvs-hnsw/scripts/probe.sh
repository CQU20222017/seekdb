#!/usr/bin/env bash
# Probe graph quality at different ef_search values.
set -u
cd /mnt/d/seekdb-2026/amd/hipvs-hnsw || exit 1
for ef in 64 150 300; do
  out=$(./build/hipvs_bench --rows 300 --dim 32 --queries 50 --ef-search "$ef" 2>&1)
  rec=$(printf '%s\n' "$out" | grep -o 'recall@10=[0-9.]*')
  ev=$(printf '%s\n' "$out" | grep -o 'distance_evaluations=[0-9]*')
  echo "ef_search=$ef  $rec  $ev"
done
exit 0