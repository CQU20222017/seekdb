#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Summarise N FTS benchmark reports: median and range per metric.

``tools/benchmark/fts_large_bench.sh`` appends one report per run to a JSONL
file (the file is really ``key: value`` text blocks).  Performance claims are
only meaningful across *clean restarts*, so this script takes the report files
of one side ("before" or "after"), prints the median of every metric plus its
range, and - with ``--baseline`` - the median improvement against another side.

Usage:

    python3 script/fts_bench_variance.py before \
        ~/fts_reports/wk_off_r1.json ~/fts_reports/wk_off_r2.json ...
    python3 script/fts_bench_variance.py after --baseline before \
        ~/fts_reports/wk_new_r1.json ...

The same labels are read back from the report files, so a bare
``--labels before=... after=...`` form is also supported when the reports are
named ``<label>_r<N>_<date>_<time>.jsonl`` under ~/fts_reports.
"""
import argparse
import re
import statistics
import sys
from pathlib import Path

# The nine metrics the competition scoring uses (three groups of three).
GROUPS = [
    ("build", ["build_ik_all_sec", "build_ik_content_sec", "build_beng_en_sec"]),
    ("tokenize", ["tokenize_ik_avg_ms", "tokenize_beng_avg_ms"]),
    ("query", ["query_cn_avg_ms", "query_beng_avg_ms", "query_mixed_avg_ms",
               "query_limit_avg_ms"]),
]
ALL_METRICS = [metric for _group, metrics in GROUPS for metric in metrics]
HIT_METRICS = ["query_cn_hits", "query_beng_hits", "query_mixed_hits", "query_limit_hits"]
LINE_RE = re.compile(r"^([a-z0-9_]+):\s+([-+0-9.eE]+)\s*$")


def parse_report(path):
    """Read a benchmark report and return {metric: float}."""
    values = {}
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise SystemExit("cannot read %s: %s" % (path, exc))
    for line in text.splitlines():
        match = LINE_RE.match(line.strip())
        if match:
            try:
                values[match.group(1)] = float(match.group(2))
            except ValueError:
                continue
    if not values:
        raise SystemExit("no metric found in %s" % path)
    return values


def summarise(label, paths):
    reports = [parse_report(path) for path in paths]
    print("%s: %d report(s)" % (label, len(reports)))
    rows = {}
    for metric in ALL_METRICS:
        samples = [report[metric] for report in reports if metric in report]
        if not samples:
            continue
        rows[metric] = (statistics.median(samples), min(samples), max(samples),
                        len(samples))
    for group, metrics in GROUPS:
        for metric in metrics:
            if metric not in rows:
                continue
            median, low, high, count = rows[metric]
            print("  %-24s median=%10.4f  range=%10.4f .. %10.4f  (n=%d)"
                  % (metric, median, low, high, count))
    for metric in HIT_METRICS:
        samples = [report[metric] for report in reports if metric in report]
        if samples:
            print("  %-24s values=%s" % (metric, sorted({int(v) for v in samples})))
    return rows


def compare(baseline, candidate):
    print()
    print("=== improvement (candidate vs baseline, medians) ===")
    total = []
    for group, metrics in GROUPS:
        group_deltas = []
        for metric in metrics:
            if metric not in baseline or metric not in candidate:
                continue
            base = baseline[metric][0]
            cand = candidate[metric][0]
            if base <= 0:
                continue
            delta = (base - cand) / base * 100.0
            group_deltas.append(delta)
            print("  %-24s %+7.2f%%  (%.4f -> %.4f)" % (metric, delta, base, cand))
        if group_deltas:
            mean = sum(group_deltas) / len(group_deltas)
            total.append(mean)
            print("  %-24s %+7.2f%%  (group mean)" % (group, mean))
    if total:
        print("  %-24s %+7.2f%%  (mean of the three groups)" % ("overall", sum(total) / len(total)))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("label", help="name of this side, e.g. before / after")
    parser.add_argument("reports", nargs="+", help="report files of this side")
    parser.add_argument("--baseline", default=None,
                        help="baseline label; the baseline reports must be given with "
                             "--baseline-reports")
    parser.add_argument("--baseline-reports", nargs="+", default=None)
    options = parser.parse_args(argv)

    candidate = summarise(options.label, options.reports)
    if options.baseline:
        if not options.baseline_reports:
            raise SystemExit("--baseline needs --baseline-reports")
        baseline = summarise(options.baseline, options.baseline_reports)
        compare(baseline, candidate)
    return 0


if __name__ == "__main__":
    sys.exit(main())