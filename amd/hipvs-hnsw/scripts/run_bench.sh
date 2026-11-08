#!/usr/bin/env bash
# Build the CPU reference binary and run the full benchmark sweep used in BENCHMARK.md.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
mkdir -p build results

g++ -std=c++17 -O2 -march=native -Isrc \
    src/distance_cpu.cpp src/hnsw_builder.cpp src/bench_main.cpp \
    -o build/hipvs_bench || { echo "[error] build failed"; exit 1; }

echo "=== smoke test (300 rows) ==="
./build/hipvs_bench --rows 300 --dim 32 --queries 50 | tail -3

echo "=== full run (20000 x 128) ==="
./build/hipvs_bench --rows 20000 --dim 128 --queries 200 --json results/cpu_20k_128.json | tail -5

echo "=== 50000 x 128 (scale check) ==="
./build/hipvs_bench --rows 50000 --dim 128 --queries 200 --json results/cpu_50k_128.json | tail -5
exit 0