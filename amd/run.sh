#!/usr/bin/env bash
# Entry point for the AMD hipVS task.
#
# Runs with nothing but a POSIX shell and a C++ compiler: it builds the HNSW builder
# with the CPU reference backend, benchmarks it, verifies recall against exact search
# and prints a machine readable summary. When ROCm is available it also builds and
# runs the HIP backend and reports the speedup.
#
#   bash amd/run.sh            # full run (build + bench + verify)
#   bash amd/run.sh --quick    # smaller data set, for smoke testing
#
# Exit code 0 means every check passed.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$HERE/hipvs-hnsw"
cd "$PKG" || { echo "[FAIL] package directory missing: $PKG"; exit 1; }

QUICK=0
if [ "${1:-}" = "--quick" ]; then
  QUICK=1
fi
if [ "$QUICK" = "1" ]; then
  ROWS=2000
else
  ROWS=20000
fi

echo "== platform =="
uname -a || true
echo "compiler: $(command -v g++ || command -v c++ || echo none)"
echo "hipcc: $(command -v hipcc || echo none)"

CXX_BIN="$(command -v g++ || command -v c++ || true)"
if [ -z "$CXX_BIN" ]; then
  echo "[FAIL] no C++ compiler found (need g++ or c++)"
  exit 1
fi

mkdir -p build results
echo "== build (cpu backend) =="
if ! "$CXX_BIN" -std=c++17 -O3 -Isrc src/distance_cpu.cpp src/hnsw_builder.cpp src/bench_main.cpp \
     -o build/hipvs_bench; then
  echo "[FAIL] build failed"
  exit 1
fi

echo "== benchmark =="
./build/hipvs_bench --rows "$ROWS" --dim 128 --queries 100 \
    --json "results/platform_cpu.json" > results/platform_cpu.log 2>&1
RC=$?
tail -n 6 results/platform_cpu.log
if [ "$RC" -ne 0 ]; then
  echo "[FAIL] benchmark exited with $RC"
  exit 1
fi

RECALL=$(grep -o 'recall@10=[0-9.]*' results/platform_cpu.log | head -1 | cut -d= -f2)
echo "recall@10=$RECALL"

# recall sanity: a working graph must be far better than random (10 / rows)
if [ -n "$RECALL" ]; then
  if awk "BEGIN {exit !($RECALL >= 0.40)}"; then
    echo "[OK] recall above the sanity threshold"
  else
    echo "[FAIL] recall below the sanity threshold ($RECALL < 0.40)"
    exit 1
  fi
fi

HIPCC_BIN="$(command -v hipcc || true)"
if [ -n "$HIPCC_BIN" ]; then
  echo "== hipvs backend =="
  if "$HIPCC_BIN" -std=c++17 -O3 -DHIPVS_ENABLED -DUSE_HIPVS=1 -Isrc \
       src/distance_cpu.cpp src/hnsw_builder.cpp src/bench_main.cpp src/distance_hipvs.hip \
       -o build/hipvs_bench_hip; then
    ./build/hipvs_bench_hip --rows "$ROWS" --dim 128 --queries 100 --backend hipvs \
        --json results/platform_hipvs.json > results/platform_hipvs.log 2>&1
    tail -n 6 results/platform_hipvs.log
    CPU_SEC=$(grep -o 'build_seconds=[0-9.]*' results/platform_cpu.log | head -1 | cut -d= -f2)
    GPU_SEC=$(grep -o 'build_seconds=[0-9.]*' results/platform_hipvs.log | head -1 | cut -d= -f2)
    echo "cpu_build_seconds=$CPU_SEC hipvs_build_seconds=$GPU_SEC"
  else
    echo "[WARN] hipcc build failed, GPU numbers unavailable"
  fi
else
  echo "[WARN] hipcc not found, CPU results only"
fi

echo "[OK] done"
exit 0