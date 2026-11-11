#!/usr/bin/env python3
"""HNSW build benchmark for the hipVS task.

CPU phases are really measured. The GPU column is produced by an explicit cost
model (see bench_utils.estimate_gpu_time) because this environment has no AMD
GPU; every simulated number is labelled as a model output in the report.

usage:
    python3 amd/bench/bench_build.py [--n 800] [--d 64] [--queries 50] [--runs 3]
                                     [--out amd/bench/report.md] [--json amd/bench/result.json]
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench_utils as BU  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=800)
    ap.add_argument("--d", type=int, default=64)
    ap.add_argument("--queries", type=int, default=50)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--ef", type=int, default=64)
    ap.add_argument("--cand-k", type=int, default=33)
    ap.add_argument("--max-degree", type=int, default=16)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--mock-gpu-speedup", type=float, default=8.0)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "report.md"))
    ap.add_argument("--json", default=os.path.join(os.path.dirname(__file__), "result.json"))
    args = ap.parse_args()

    print(f"[bench] numpy={'yes' if BU.HAVE_NUMPY else 'no'}  n={args.n} d={args.d} "
          f"queries={args.queries} runs={args.runs}")

    t0 = time.perf_counter()
    vectors = BU.make_vectors(args.n, args.d, seed=args.seed)
    queries = BU.make_vectors(args.queries, args.d, seed=args.seed + 1)
    gen_s = time.perf_counter() - t0
    print(f"[bench] data ready in {gen_s * 1000:.1f} ms (data generation is not part of the build time)")

    cand_runs, asm_runs, tot_runs = [], [], []
    neighbors = None
    for r in range(args.runs):
        t1 = time.perf_counter()
        cand_idx, cand_dist = BU.candidate_search(vectors, args.cand_k)
        t2 = time.perf_counter()
        neighbors = BU.assemble_graph(cand_idx, cand_dist, args.max_degree)
        t3 = time.perf_counter()
        cand_runs.append(t2 - t1)
        asm_runs.append(t3 - t2)
        tot_runs.append(t3 - t1)
        print(f"[bench] run {r + 1}/{args.runs}: candidates={cand_runs[-1] * 1000:.1f} ms "
              f"assemble={asm_runs[-1] * 1000:.1f} ms total={tot_runs[-1] * 1000:.1f} ms")

    cand_stats = BU.summarize(cand_runs)
    asm_stats = BU.summarize(asm_runs)
    tot_stats = BU.summarize(tot_runs)

    graph = BU.graph_stats(neighbors)
    t4 = time.perf_counter()
    recall = BU.recall_at_k(neighbors, vectors, queries, args.k, args.ef)
    query_s = time.perf_counter() - t4

    upload_bytes = args.n * args.d * 4
    download_bytes = args.n * args.cand_k * 4
    gpu = BU.estimate_gpu_time(cand_stats["median"], asm_stats["median"],
                               upload_bytes, download_bytes,
                               distance_speedup=args.mock_gpu_speedup)
    speedup = tot_stats["median"] / gpu["gpu_total_s"] if gpu["gpu_total_s"] > 0 else 0.0
    path = BU.path_status(gpu_available=False, gpu_seconds=None,
                          cpu_seconds=tot_stats["median"])
    path["model_speedup"] = speedup

    result = {
        "config": {"n": args.n, "d": args.d, "queries": args.queries, "k": args.k,
                   "ef": args.ef, "cand_k": args.cand_k, "max_degree": args.max_degree,
                   "runs": args.runs, "seed": args.seed, "numpy": BU.HAVE_NUMPY},
        "gen_seconds": gen_s,
        "cpu_runs": [{"candidates_s": c, "assemble_s": a, "total_s": t}
                     for c, a, t in zip(cand_runs, asm_runs, tot_runs)],
        "cpu_stats": {"candidates": cand_stats, "assemble": asm_stats, "total": tot_stats},
        "graph": graph,
        "recall": {"k": args.k, "value": recall, "query_seconds": query_s},
        "gpu_model": gpu,
        "gpu_path": path,
        "conclusion": {"cpu_median_ms": tot_stats["median"] * 1000.0,
                       "gpu_model_ms": gpu["gpu_total_s"] * 1000.0,
                       "model_speedup": speedup},
    }

    os.makedirs(os.path.dirname(os.path.abspath(args.json)), exist_ok=True)
    with open(args.json, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2, ensure_ascii=False)

    report = render_report(result)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(report)

    print(f"[bench] cpu_median={result['conclusion']['cpu_median_ms']:.1f} ms  "
          f"gpu_model={result['conclusion']['gpu_model_ms']:.1f} ms  "
          f"model_speedup={speedup:.2f}x  recall@{args.k}={recall:.4f}")
    print(f"[bench] report: {args.out}")
    print(f"[bench] json:   {args.json}")
    return 0


def render_report(r: dict) -> str:
    c = r["config"]
    cand, asm, tot = r["cpu_stats"]["candidates"], r["cpu_stats"]["assemble"], r["cpu_stats"]["total"]
    g = r["gpu_model"]
    con = r["conclusion"]
    lines = []
    add = lines.append
    add("# HNSW build benchmark report (auto-generated by bench_build.py)")
    add("")
    add("CPU phases are measured; the GPU column comes from an explicit cost model, "
        "not from a GPU (no AMD GPU in this environment).")
    add("")
    add(f"- data: n={c['n']} d={c['d']} queries={c['queries']} seed={c['seed']} "
        f"numpy={'yes' if c['numpy'] else 'no'}")
    add(f"- parameters: cand_k={c['cand_k']} max_degree={c['max_degree']} k={c['k']} ef={c['ef']}")
    add(f"- data generation: {r['gen_seconds'] * 1000:.1f} ms (excluded from build time)")
    add("")
    add("## CPU build (measured, median of %d runs)" % c["runs"])
    add("")
    add("| phase | median ms | min ms | max ms | stdev ms |")
    add("| --- | ---: | ---: | ---: | ---: |")
    add(f"| candidate search (distance) | {cand['median'] * 1000:.1f} | {cand['min'] * 1000:.1f} "
        f"| {cand['max'] * 1000:.1f} | {cand['stdev'] * 1000:.2f} |")
    add(f"| graph assembly (CPU)        | {asm['median'] * 1000:.1f} | {asm['min'] * 1000:.1f} "
        f"| {asm['max'] * 1000:.1f} | {asm['stdev'] * 1000:.2f} |")
    add(f"| **total**                   | **{tot['median'] * 1000:.1f}** | {tot['min'] * 1000:.1f} "
        f"| {tot['max'] * 1000:.1f} | {tot['stdev'] * 1000:.2f} |")
    add("")
    add("## GPU path (cost model, assumptions stated)")
    add("")
    add(f"- distance on hipVS: {cand['median'] * 1000:.1f} / {g['distance_speedup_assumed']:.1f} "
        f"= {g['gpu_distance_s'] * 1000:.1f} ms")
    add(f"- assembly (stays on CPU): {g['assemble_s'] * 1000:.1f} ms")
    add(f"- PCIe transfer @ {g['bandwidth_gb_s_assigned'] if 'bandwidth_gb_s_assigned' in g else g['bandwidth_gb_s_assumed']:.0f} GB/s: "
        f"{g['transfer_s'] * 1000:.2f} ms; launch overhead: {g['launch_overhead_s'] * 1000:.1f} ms")
    add(f"- **model total: {g['gpu_total_s'] * 1000:.1f} ms  ->  model speedup {con['model_speedup']:.2f}x**")
    add("")
    add("## Quality gate")
    add("")
    add(f"- recall@{c['k']}: **{r['recall']['value']:.4f}** (best-first search vs exact ground truth)")
    add(f"- graph: {r['graph']['nodes']} nodes, avg degree {r['graph']['avg_degree']:.1f}, "
        f"isolated {r['graph']['isolated_nodes']}, reachable {r['graph']['reachable_ratio']:.3f}")
    add("")
    add("## Path status")
    add("")
    add(f"- {r['gpu_path']['status']}: {r['gpu_path']['reason']}")
    add("")
    return "\n".join(lines)


if __name__ == "__main__":
    sys.exit(main())
