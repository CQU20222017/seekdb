#!/usr/bin/env python3
"""Unit tests for the bench helpers. Run: python3 -m unittest discover -s amd/tests -v"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "bench"))
import bench_utils as BU  # noqa: E402


class TestStats(unittest.TestCase):
    def test_median(self):
        s = BU.summarize([3.0, 1.0, 2.0])
        self.assertEqual(2.0, s["median"])
        self.assertEqual(1.0, s["min"])

    def test_single_run(self):
        s = BU.summarize([1.5])
        self.assertEqual(1.5, s["median"])
        self.assertEqual(0.0, s["stdev"])


class TestGraph(unittest.TestCase):
    def test_no_isolated_nodes(self):
        vectors = BU.make_vectors(60, 8, seed=1)
        cand_idx, cand_dist = BU.candidate_search(vectors, 10)
        neighbors = BU.assemble_graph(cand_idx, cand_dist, 8)
        stats = BU.graph_stats(neighbors)
        self.assertEqual(60, stats["nodes"])
        self.assertEqual(0, stats["isolated_nodes"])

    def test_degree_capped(self):
        vectors = BU.make_vectors(40, 8, seed=2)
        cand_idx, cand_dist = BU.candidate_search(vectors, 12)
        neighbors = BU.assemble_graph(cand_idx, cand_dist, 6)
        for links in neighbors:
            self.assertLessEqual(len(links), 6)


class TestRecall(unittest.TestCase):
    def test_recall_better_than_random(self):
        vectors = BU.make_vectors(120, 16, seed=3)
        queries = BU.make_vectors(10, 16, seed=4)
        cand_idx, cand_dist = BU.candidate_search(vectors, 20)
        neighbors = BU.assemble_graph(cand_idx, cand_dist, 10)
        recall = BU.recall_at_k(neighbors, vectors, queries, k=5, ef=32)
        self.assertGreater(recall, 0.2)


class TestGpuModel(unittest.TestCase):
    def test_model_components_add_up(self):
        g = BU.estimate_gpu_time(0.1, 0.05, 1000, 1000, distance_speedup=4.0)
        expected = 0.1 / 4.0 + 0.05 + g["transfer_s"] + g["launch_overhead_s"]
        self.assertAlmostEqual(expected, g["gpu_total_s"])

    def test_no_gpu_is_cpu_only(self):
        p = BU.path_status(gpu_available=False, gpu_seconds=None, cpu_seconds=1.0)
        self.assertEqual("CPU_ONLY", p["status"])
        self.assertFalse(p["countable"])

    def test_fallback_not_countable(self):
        p = BU.path_status(gpu_available=True, gpu_seconds=None, cpu_seconds=1.0)
        self.assertEqual("FALLBACK", p["status"])
        self.assertFalse(p["countable"])

    def test_gpu_active_countable(self):
        p = BU.path_status(gpu_available=True, gpu_seconds=0.5, cpu_seconds=1.0)
        self.assertEqual("GPU_ACTIVE", p["status"])
        self.assertTrue(p["countable"])
        self.assertAlmostEqual(2.0, p["speedup"])


if __name__ == "__main__":
    unittest.main()
