"""Shared helpers for the hipVS HNSW build benchmark.

Everything here runs on a plain Python 3.8+ installation. numpy is used when it
is available (faster, larger data sets); a pure stdlib fallback keeps the
benchmark runnable on machines without any third-party packages.

The GPU numbers are produced by an explicit cost model (see estimate_gpu_time),
never by measuring a GPU: this environment has no AMD GPU, and the model
assumptions are printed into the report so nobody can mistake them for
measurements.
"""

from __future__ import annotations

import math
import random
import statistics

try:
    import numpy as np
    HAVE_NUMPY = True
except ImportError:
    HAVE_NUMPY = False


# ---------------------------------------------------------------------------
# data
# ---------------------------------------------------------------------------

def make_vectors(n, d, seed=42):
    """Return n random unit vectors of dimension d.

    With numpy: ndarray of shape (n, d). Without numpy: list of lists.
    """
    rng = random.Random(seed)
    if HAVE_NUMPY:
        import numpy as np
        data = np.random.default_rng(seed).standard_normal((n, d)).astype("float64")
        norms = np.linalg.norm(data, axis=1, keepdims=True)
        norms[norms == 0.0] = 1.0
        return data / norms
    out = []
    for _ in range(n):
        row = [rng.gauss(0.0, 1.0) for _ in range(d)]
        norm = math.sqrt(sum(x * x for x in row)) or 1.0
        out.append([x / norm for x in row])
    return out


# ---------------------------------------------------------------------------
# phase 1: candidate neighbour search (the part hipVS accelerates)
# ---------------------------------------------------------------------------

def candidate_search(vectors, cand_k):
    """For every row of `vectors`, find the cand_k nearest other rows by L2.

    This mirrors the candidate generation inside an HNSW insert: it is the
    distance-computation-heavy phase that a GPU offloads.
    """
    n = len(vectors)
    d = len(vectors[0])
    if HAVE_NUMPY:
        import numpy as np
        v = np.asarray(vectors)
        # pairwise squared distances via the (a-b)^2 = |a|^2+|b|^2-2ab identity
        sq = (v ** 2).sum(axis=1, keepdims=True)
        dist = sq + sq.T - 2.0 * (v @ v.T)
        np.fill_diagonal(dist, math.inf)
        idx = np.argsort(dist, axis=1)[:, :cand_k]
        dist_sorted = np.take_along_axis(dist, idx, axis=1)
        return idx.tolist(), dist_sorted.tolist()
    all_idx = []
    all_dist = []
    for i in range(n):
        vi = vectors[i]
        dists = []
        for j in range(n):
            if i == j:
                continue
            vj = vectors[j]
            acc = 0.0
            for k in range(d):
                diff = vi[k] - vj[k]
                acc += diff * diff
            dists.append((acc, j))
        dists.sort()
        all_idx.append([j for _, j in dists[:cand_k]])
        all_dist.append([dd for dd, _ in dists[:cand_k]])
    return all_idx, all_dist


# ---------------------------------------------------------------------------
# phase 2: graph assembly (always stays on the CPU)
# ---------------------------------------------------------------------------

def assemble_graph(cand_idx, cand_dist, max_degree):
    """Build a single-layer navigable graph from the candidate table.

    forward edges: each node links to its max_degree closest candidates;
    reverse edges are added and then every list is pruned by distance, which is
    what keeps the graph navigable (dropping the tail instead would strand the
    early nodes).
    """
    n = len(cand_idx)
    neighbors = [[] for _ in range(n)]
    for i in range(n):
        neighbors[i] = list(cand_idx[i][:max_degree])
    # reverse edges
    for i in range(n):
        for j in list(neighbors[i]):
            if i not in neighbors[j]:
                neighbors[j].append(i)
    # prune by distance to the owner, capped at max_degree
    for i in range(n):
        if len(neighbors[i]) <= max_degree:
            continue
        di = {j: cand_dist[i][cand_idx[i].index(j)] if j in cand_idx[i] else math.inf
              for j in neighbors[i]}
        neighbors[i] = sorted(neighbors[i], key=lambda j: di[j])[:max_degree]
    return neighbors


def graph_stats(neighbors):
    n = len(neighbors)
    degrees = [len(x) for x in neighbors]
    isolated = sum(1 for x in degrees if x == 0)
    # BFS from node 0 over the undirected view
    seen = [False] * n
    if n:
        stack = [0]
        seen[0] = True
        while stack:
            cur = stack.pop()
            for nb in neighbors[cur]:
                if not seen[nb]:
                    seen[nb] = True
                    stack.append(nb)
    return {
        "nodes": n,
        "avg_degree": (sum(degrees) / n) if n else 0.0,
        "isolated_nodes": isolated,
        "reachable_ratio": (sum(seen) / n) if n else 0.0,
    }


# ---------------------------------------------------------------------------
# search + recall
# ---------------------------------------------------------------------------

def _l2(a, b):
    return sum((x - y) * (x - y) for x, y in zip(a, b))


def best_first(neighbors, vectors, query, k, ef):
    """Greedy best-first search over the assembled graph."""
    import heapq
    n = len(vectors)
    start = 0
    dist0 = _l2(vectors[start], query)
    candidates = [(dist0, start)]
    best = [( -dist0, start)]
    visited = {start}
    while candidates:
        dcur, cur = heapq.heappop(candidates)
        worst = -best[0][0]
        if dcur > worst and len(best) >= ef:
            break
        for nb in neighbors[cur]:
            if nb in visited:
                continue
            visited.add(nb)
            d = _l2(vectors[nb], query)
            if len(best) < ef or d < -best[0][0]:
                heapq.heappush(candidates, (d, nb))
                heapq.heappush(best, (-d, nb))
                if len(best) > ef:
                    heapq.heappop(best)
    out = sorted(((-d, idx) for d, idx in best))
    return [idx for _, idx in out[:k]]


def exact_topk(vectors, query, k):
    dists = sorted(((_l2(v, query), i) for i, v in enumerate(vectors)))
    return [i for _, i in dists[:k]]


def recall_at_k(neighbors, vectors, queries, k, ef):
    hits = 0
    total = 0
    for q in queries:
        approx = best_first(neighbors, vectors, q, k, ef)
        exact = exact_topk(vectors, q, k)
        hits += len(set(approx) & set(exact))
        total += k
    return (hits / total) if total else 0.0


# ---------------------------------------------------------------------------
# statistics + the GPU cost model
# ---------------------------------------------------------------------------

def summarize(runs):
    return {
        "runs": len(runs),
        "median": statistics.median(runs),
        "mean": statistics.fmean(runs),
        "min": min(runs),
        "max": max(runs),
        "stdev": statistics.pstdev(runs) if len(runs) > 1 else 0.0,
    }


def estimate_gpu_time(distance_seconds, assemble_seconds, upload_bytes,
                      download_bytes, distance_speedup=8.0,
                      bandwidth_gb_s=10.0, launch_overhead_s=0.005):
    """Model the GPU path; every factor is an explicit, documented assumption.

    gpu_total = distance / distance_speedup      (hipVS batch kNN)
              + assemble                         (graph repair stays on the CPU)
              + (upload + download) / bandwidth  (PCIe transfer)
              + launch_overhead                  (kernel launch, sync)
    """
    transfer_s = (upload_bytes + download_bytes) / (bandwidth_gb_s * 1e9)
    gpu_distance = distance_seconds / distance_speedup
    total = gpu_distance_seconds = gpu_distance_s = None  # placate linters
    gpu_distance_s = gpu_distance
    total = gpu_distance_s + assemble_seconds + transfer_s + launch_overhead_s
    return {
        "gpu_distance_s": gpu_distance_s,
        "assemble_s": assemble_seconds,
        "transfer_s": transfer_s,
        "launch_overhead_s": launch_overhead_s,
        "gpu_total_s": total,
        "distance_speedup_assumed": distance_speedup,
        "bandwidth_gb_s_assumed": bandwidth_gb_s,
    }


def path_status(gpu_available, gpu_seconds, cpu_seconds):
    """Three-state verdict used by the report and by the unit tests.

    GPU_ACTIVE : a GPU run happened and is allowed to count as acceleration.
    FALLBACK   : a GPU was requested but unusable; the CPU time must NOT count.
    CPU_ONLY   : no GPU was requested at all.
    """
    if not gpu_available:
        return {"status": "CPU_ONLY", "countable": False,
                "reason": "no AMD GPU / hipVS in this environment"}
    if gpu_seconds is None:
        return {"status": "FALLBACK", "countable": False,
                "reason": "gpu run failed, fell back to cpu; not countable"}
    speedup = (cpu_seconds / gpu_seconds) if gpu_seconds > 0 else 0.0
    return {"status": "GPU_ACTIVE", "countable": True,
            "reason": "ok", "speedup": speedup}
