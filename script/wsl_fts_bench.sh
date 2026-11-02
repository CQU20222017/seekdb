#!/usr/bin/env bash
# Run the official FTS benchmark once and score it.
# usage: wsl_fts_bench.sh <label> [rows] [batch] [rounds] [query_rounds] [samples] [warmup] [skip_load]
set -u
LABEL="${1:-local}"
ROWS="${2:-20000}"
BATCH="${3:-500}"
ROUNDS="${4:-3000}"
QUERY_ROUNDS="${5:-200}"
SAMPLES="${6:-3}"
WARMUP="${7:-30}"
SKIP_LOAD="${8:-0}"
export PATH="$HOME/bin:$PATH"
export MYSQL="mysql -h127.0.0.1 -P2881 -uroot -N -s -A --default-character-set=utf8mb4"
export MYSQL_VERBOSE="mysql -h127.0.0.1 -P2881 -uroot -A --default-character-set=utf8mb4"
mkdir -p "$HOME/fts_reports"
REPORT="$HOME/fts_reports/${LABEL}_$(date +%m%d_%H%M%S).jsonl"
echo "report: $REPORT"
cd "$HOME/seekdb" || exit 1
LABEL="$LABEL" ROWS="$ROWS" BATCH="$BATCH" ROUNDS="$ROUNDS" QUERY_ROUNDS="$QUERY_ROUNDS" \
  SAMPLES="$SAMPLES" WARMUP="$WARMUP" SKIP_LOAD="$SKIP_LOAD" OUTPUT="$REPORT" \
  bash tools/benchmark/fts_large_bench.sh 2>&1 | tail -c 4000
echo "=== score ==="
python3 tools/benchmark/fts_large_bench_score.py "$REPORT" 2>&1 | tail -40
echo "=== raw report ==="
cat "$REPORT"
exit 0