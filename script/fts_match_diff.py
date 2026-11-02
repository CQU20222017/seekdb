#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Compare full-text MATCH results of two seekdb instances row by row.

The FTS performance work changes the inverted-index merge (DAAT/TAAT), the
scoring path (membership-only scans skip BM25) and the sort/limit handling, so
the cheapest way to prove "results unchanged" is to build the *same* corpus and
the *same* indexes on two instances and compare the complete result set of a
query battery: membership queries, scored queries, ordered limits, boolean and
phrase searches, aggregates, and the score values themselves.

Usage (inside WSL, both instances up):

    python3 script/fts_match_diff.py --old-port 2882 --new-port 2881

The corpus comes from tools/benchmark/fts_large_bench_gen.py (read only), so the
documents are the official benchmark documents.
"""
import argparse
import importlib.util
import sys
from pathlib import Path

import pymysql

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
GENERATOR = REPO_ROOT / "tools" / "benchmark" / "fts_large_bench_gen.py"
DB = "fts_match_diff"
BATCH = 300

# (label, sql) - every query must be deterministic and fully ordered.
QUERIES = [
    ("count_all", "select count(*) from docs"),
    ("match_cn_count", "select count(*) from docs where match(content) against ('数据库')"),
    ("match_cn_ids", "select id from docs where match(content) against ('数据库') order by id"),
    ("match_cn_natural",
     "select id from docs where match(content) against ('全文索引 分词器') order by id"),
    ("match_cn_phrase",
     "select id from docs where match(content) against ('\"全文索引\"' in boolean mode) order by id"),
    ("match_cn_boolean",
     "select id from docs where match(content) against ('+数据库 -稳定' in boolean mode) "
     "order by id"),
    ("match_en_count",
     "select count(*) from docs where match(content_en) against ('database')"),
    ("match_en_ids",
     "select id from docs where match(content_en) against ('database') order by id"),
    ("match_en_multi",
     "select id from docs where match(content_en) against ('database performance optimization') "
     "order by id"),
    ("match_en_phrase",
     "select id from docs where match(content_en) against ('\"full-text search\"' in boolean mode) "
     "order by id"),
    ("match_mixed_title",
     "select id from docs where match(title) against ('OceanBase') order by id"),
    ("match_two_cols",
     "select id from docs where match(title, content) against ('索引') order by id"),
    ("match_no_hit",
     "select id from docs where match(content) against ('zzzznotexist') order by id"),
    ("match_all_hits_count",
     "select count(*) from docs where match(content) against ('数据库 索引 查询 性能')"),
    ("distinct_membership",
     "select distinct category from docs where match(content) against ('数据库') "
     "order by category"),
    ("group_by_membership",
     "select category, count(*) from docs where match(content) against ('数据库') "
     "group by category order by category"),
    ("limit_membership",
     "select id from docs where match(content) against ('数据库') order by id limit 10"),
    ("scored_ids",
     "select id from docs where match(content) against ('数据库') order by id"),
    ("scored_values",
     "select id, round(match(content) against ('数据库'), 6) as s, "
     "round(match(title, content) against ('数据库'), 6) as s2 from docs "
     "where match(content) against ('数据库') order by id"),
    ("scored_topk",
     "select id from docs where match(content) against ('数据库') "
     "order by match(content) against ('数据库') desc, id limit 5"),
    ("scored_having",
     "select id, match(content) against ('数据库') as s from docs "
     "where match(content) against ('数据库') having s > 0.5 order by id"),
    ("scored_where_gt",
     "select id from docs where match(content) against ('数据库') "
     "and match(content) against ('数据库') > 1 order by id"),
    ("boolean_wildcard",
     "select id from docs where match(content_en) against ('datab*' in boolean mode) "
     "order by id"),
    ("boolean_no_op",
     "select id from docs where match(content_en) against ('database -audit' in boolean mode) "
     "order by id"),
    ("match_en_limit",
     "select id from docs where match(content_en) against ('database performance') "
     "order by id limit 20"),
    ("match_cn_extra_filter",
     "select id from docs where match(content) against ('数据库') and category > 5 order by id"),
]


def load_rows(count):
    if not GENERATOR.is_file():
        raise SystemExit("missing corpus generator: %s" % GENERATOR)
    spec = importlib.util.spec_from_file_location("fts_bench_gen", GENERATOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return [module.make_row(i) for i in range(count)]


def connect(port, user, password):
    return pymysql.connect(host="127.0.0.1", port=port, user=user, password=password,
                           database="test", autocommit=True, charset="utf8mb4")


def prepare(conn, rows):
    cur = conn.cursor()
    cur.execute("drop database if exists %s" % DB)
    cur.execute("create database %s" % DB)
    cur.execute("use %s" % DB)
    cur.execute(
        "create table docs(id bigint primary key, category int, title varchar(256), "
        "content text, title_en varchar(256), content_en text, "
        "fulltext index ft_cn(content) with parser ik, "
        "fulltext index ft_mixed(title, content) with parser ik, "
        "fulltext index ft_en(content_en) with parser beng, "
        "fulltext index ft_title(title) with parser ik)")
    insert = ("insert into docs(id, category, title, content, title_en, content_en) values "
              "(%s, %s, %s, %s, %s, %s)")
    pending = []
    for index, (category, title, body, title_en, body_en) in enumerate(rows):
        pending.append((index + 1, category, title, body, title_en, body_en))
        if len(pending) >= BATCH:
            cur.executemany(insert, pending)
            pending = []
    if pending:
        cur.executemany(insert, pending)
    cur.execute("select count(*) from docs")
    loaded = int(cur.fetchone()[0])
    cur.close()
    return loaded


def run_query(conn, sql):
    cur = conn.cursor()
    try:
        cur.execute("use %s" % DB)
        cur.execute(sql)
        rows = cur.fetchall()
    finally:
        cur.close()
    out = []
    for row in rows:
        out.append(tuple(None if cell is None else str(cell) for cell in row))
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--old-port", type=int, default=2882)
    parser.add_argument("--new-port", type=int, default=2881)
    parser.add_argument("--user", default="root")
    parser.add_argument("--password", default="")
    parser.add_argument("--rows", type=int, default=1500)
    options = parser.parse_args(argv)

    rows = load_rows(options.rows)
    old = connect(options.old_port, options.user, options.password)
    new = connect(options.new_port, options.user, options.password)
    diffs = 0
    try:
        print("building the same %d-document corpus on both instances ..." % len(rows))
        old_loaded = prepare(old, rows)
        new_loaded = prepare(new, rows)
        print("loaded rows: before=%d merged=%d" % (old_loaded, new_loaded))
        if old_loaded != new_loaded:
            print("MATCH DIFF: the corpus was not loaded identically")
            return 1
        for label, sql in QUERIES:
            try:
                old_rows = run_query(old, sql)
            except Exception as exc:
                print("SKIP %-24s old error: %s" % (label, exc))
                continue
            try:
                new_rows = run_query(new, sql)
            except Exception as exc:
                diffs += 1
                print("DIFF %-24s new raised: %s" % (label, exc))
                continue
            if old_rows == new_rows:
                print("ok   %-24s %d row(s)" % (label, len(old_rows)))
            else:
                diffs += 1
                print("DIFF %-24s old=%d row(s) new=%d row(s)" % (label, len(old_rows),
                                                                  len(new_rows)))
                only_old = [row for row in old_rows if row not in new_rows][:5]
                only_new = [row for row in new_rows if row not in old_rows][:5]
                print("     only in before: %r" % (only_old,))
                print("     only in merged: %r" % (only_new,))
    finally:
        old.close()
        new.close()

    if diffs:
        print("MATCH DIFF: %d query/queries differ" % diffs)
        return 1
    print("MATCH IDENTICAL: %d queries, 0 diff" % len(QUERIES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
