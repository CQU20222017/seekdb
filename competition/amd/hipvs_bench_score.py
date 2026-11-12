#!/usr/bin/env python3
# Copyright (c) 2025 OceanBase
# Licensed under the Apache License, Version 2.0 (see ../../LICENSE).
"""Score a hipVS benchmark report.

Mirrors tools/benchmark/fts_large_bench_score.py: takes the JSON produced by
tools/benchmark/hipvs_bench.sh and prints the numbers the evaluation cares
about. There is no synthetic baseline here on purpose - the GPU path is a cost
model until a Radeon machine runs it - so the score reports the measured CPU
baseline, the recall gate and the modelled speedup side by side and never mixes
them.

    python3 tools/benchmark/hipvs_bench_score.py amd/bench/result.json
"""

from __future__ import annotations

import argparse
import json
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("report", nargs="?", default="amd/bench/result.json")
    ap.add_argument("--min-recall", type=float, default=0.40,
                    help="sanity gate for recall@k (a working graph beats random by far)")
    args = ap.parse_args()

    try:
        with open(args.report, encoding="utf-8") as f:
            r = json.load(f)
    except OSError as exc:
        print(f"cannot read {args.report}: {exc}")
        return 2

    cfg = r["config"]
    total = r["cpu_stats"]["total"]
    recall = r["recall"]["value"]
    graph = r["graph"]
    con = r["conclusion"]

    print("=== hipVS HNSW build benchmark score ===")
    print(f"config:               n={cfg['n']} d={cfg['d']} "
          f"cand_k={cfg['cand_k']} max_degree={cfg['max_degree']} runs={cfg['runs']}")
    print(f"build_median_ms:      {con['cpu_median_ms']:.1f}")
    print(f"build_min_ms:         {total['min'] * 1000:.1f}")
    print(f"build_stdev_ms:       {total['stdev'] * 1000:.2f}")
    print(f"recall_at_{cfg['k']}:         {recall:.4f}")
    print(f"graph_avg_degree:     {graph['avg_degree']:.2f}")
    print(f"graph_isolated_nodes: {graph['isolated_nodes']}")
    print(f"graph_reachable:      {graph['reachable_ratio']:.3f}")
    print(f"gpu_path_status:      {r['gpu_path']['status']} ({r['gpu_path']['reason']})")
    print(f"gpu_model_total_ms:   {con['gpu_model_ms']:.1f}  [cost model, not measured]")
    print(f"gpu_model_speedup:    {con['model_speedup']:.2f}x [cost model, not measured]")

    ok = True
    if recall < args.min_recall:
        print(f"\n[FAIL] recall {recall:.4f} < {args.min_recall}")
        ok = False
    if graph["isolated_nodes"] != 0:
        print(f"\n[FAIL] {graph['isolated_nodes']} isolated nodes")
        ok = False
    if not r["gpu_path"]["countable"]:
        print("\n[INFO] GPU numbers are not countable: no AMD GPU in this environment; "
              "run amd/hipvs-hnsw with hipcc on a Radeon machine for measured speedup")
    print("\nRESULT: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())