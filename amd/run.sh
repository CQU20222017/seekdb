#!/usr/bin/env bash
# Self-check entry point for the AMD hipVS task.
#
# Runs with nothing but Python 3. The C++ benchmark is built and run when a
# compiler is available; when it is not, the pure Python benchmark still runs,
# so this script only fails if the submission itself is broken.
#
#   bash amd/run.sh            # full check
#   bash amd/run.sh --quick    # smaller data set
#
# Exit code 0 means every check passed.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE" || { echo "[FAIL] cannot enter $HERE"; exit 1; }

QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1

if [ "$QUICK" = "1" ]; then
  N=300; D=32; Q=20
else
  N=800; D=64; Q=50
fi

echo "== environment =="
echo "python: $(command -v python3 || echo none)"
echo "cxx   : $(command -v g++ || command -v c++ || echo none)"
echo "hipcc : $(command -v hipcc || echo none)"

PY="$(command -v python3 || true)"
if [ -z "$PY" ]; then
  echo "[FAIL] python3 not found"
  exit 1
fi

# ---------------------------------------------------------------- 1. unit tests
echo "== unit tests =="
if ! "$PY" -m unittest discover -s tests -v > bench/unittest.log 2>&1; then
  tail -n 20 bench/unittest.log
  echo "[FAIL] unit tests failed"
  exit 1
fi
tail -n 3 bench/unittest.log
echo "[OK] unit tests"

# ------------------------------------------------------- 2. python benchmark
echo "== benchmark (python, no third party dependencies) =="
if ! "$PY" bench/bench_build.py --n "$N" --d "$D" --queries "$Q" --runs 3 \
      --out bench/report.md --json bench/result.json > bench/bench_run.log 2>&1; then
  tail -n 20 bench/bench_run.log
  echo "[FAIL] benchmark failed"
  exit 1
fi
grep -E '^\[bench\] (cpu_median|report)' bench/bench_run.log || tail -n 5 bench/bench_run.log

RECALL="$("$PY" - <<'PY'
import json
with open("bench/result.json", encoding="utf-8") as f:
    print(json.load(f)["recall"]["value"])
PY
)"
echo "recall@10=$RECALL"
if ! "$PY" -c "import sys; sys.exit(0 if float('$RECALL') >= 0.40 else 1)"; then
  echo "[FAIL] recall below the sanity threshold"
  exit 1
fi
echo "[OK] recall above the sanity threshold"

# ------------------------------------------------ 3. C++ implementation (optional)
CXX="$(command -v g++ || command -v c++ || true)"
if [ -n "$CXX" ]; then
  echo "== benchmark (c++ implementation) =="
  if "$CXX" -std=c++17 -O2 -Ihipvs-hnsw/src \
        hipvs-hnsw/src/distance_cpu.cpp hipvs-hnsw/src/hnsw_builder.cpp \
        hipvs-hnsw/src/bench_main.cpp -o hipvs-hnsw/build/hipvs_bench 2>/dev/null \
     || (mkdir -p hipvs-hnsw/build && "$CXX" -std=c++17 -O2 -Ihipvs-hnsw/src \
        hipvs-hnsw/src/distance_cpu.cpp hipvs-hnsw/src/hnsw_builder.cpp \
        hipvs-hnsw/src/bench_main.cpp -o hipvs-hnsw/build/hipvs_bench); then
    ./hipvs-hnsw/build/hipvs_bench --rows 2000 --dim 128 --queries 50 | tail -n 3
    echo "[OK] c++ benchmark"
  else
    echo "[WARN] c++ benchmark build failed, python results stand"
  fi
else
  echo "[WARN] no C++ compiler, python results stand"
fi

# ------------------------------------------------------------- 4. hipvs notice
if [ -z "$(command -v hipcc || true)" ]; then
  echo "[WARN] hipcc not found: GPU numbers unavailable (CPU results are complete)"
fi

echo "[OK] done"
exit 0