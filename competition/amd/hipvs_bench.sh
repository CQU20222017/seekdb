#!/usr/bin/env bash
# Copyright (c) 2025 OceanBase
# Licensed under the Apache License, Version 2.0 (see ../../LICENSE).
#
# AMD hipVS HNSW build benchmark.
#
# Mirrors the layout of the fulltext benchmark in this directory: a single script
# that runs the measurement and writes a report. It runs with a plain python3 and
# needs no third party packages, GPU or C++ toolchain.
#
#   bash tools/benchmark/hipvs_bench.sh
#   HIPVS_ROWS=2000 HIPVS_DIM=128 bash tools/benchmark/hipvs_bench.sh
#
# Environment variables (all optional):
#   HIPVS_ROWS     number of vectors            (default 800)
#   HIPVS_DIM      vector dimension             (default 64)
#   HIPVS_QUERIES  number of probe vectors      (default 50)
#   HIPVS_RUNS     repetitions, median is used  (default 3)
#   HIPVS_OUTPUT   report path  (default amd/bench/report.md)
#   HIPVS_JSON     raw result   (default amd/bench/result.json)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BENCH="$REPO_ROOT/amd/bench/bench_build.py"

ROWS="${HIPVS_ROWS:-800}"
DIM="${HIPVS_DIM:-64}"
QUERIES="${HIPVS_QUERIES:-50}"
RUNS="${HIPVS_RUNS:-3}"
OUTPUT="${HIPVS_OUTPUT:-$REPO_ROOT/amd/bench/report.md}"
JSON="${HIPVS_JSON:-$REPO_ROOT/amd/bench/result.json}"

echo "=== hipVS HNSW build benchmark ==="
echo "rows=$ROWS dim=$DIM queries=$QUERIES runs=$RUNS"

PY="$(command -v python3 || command -v python || true)"
if [ -z "$PY" ]; then
  echo "[FAIL] python3 is required"
  exit 1
fi

if [ ! -f "$BENCH" ]; then
  echo "[FAIL] benchmark driver not found: $BENCH"
  exit 1
fi

"$PY" "$BENCH" --n "$ROWS" --d "$DIM" --queries "$QUERIES" --runs "$RUNS" \
    --out "$OUTPUT" --json "$JSON"
RC=$?
if [ "$RC" -ne 0 ]; then
  echo "[FAIL] benchmark exited with $RC"
  exit "$RC"
fi

echo
echo "=== report ($OUTPUT) ==="
sed -n '1,40p' "$OUTPUT" 2>/dev/null || true
echo
echo "=== summary ==="
"$PY" - "$JSON" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    r = json.load(f)

cpu = r["cpu_stats"]["total"]
con = r["conclusion"]
print("build_median_ms:      %.1f" % con["cpu_median_ms"])
print("recall_at_k:          %.4f" % r["recall"]["value"])
print("graph_avg_degree:     %.2f" % r["graph"]["avg_degree"])
print("graph_isolated_nodes: %d" % r["graph"]["isolated_nodes"])
print("gpu_path_status:      %s" % r["gpu_path"]["status"])
print("gpu_model_total_ms:   %.1f (cost model, not measured)" % con["gpu_model_ms"])
print("gpu_model_speedup:    %.2fx (cost model, not measured)" % con["model_speedup"])
PY

echo "[OK] hipvs benchmark finished"
exit 0