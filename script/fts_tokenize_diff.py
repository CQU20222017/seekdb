#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Compare TOKENIZE() output of two seekdb instances, byte for byte.

The FTS performance work touches the charset and IK fast paths, i.e. exactly the
code that decides *which* characters are letters and how they are case folded, so
every candidate has to prove that the token stream is unchanged.  This script
runs the same corpus through two instances (typically "before" and "after") and
reports any difference:

  * 31 adversarial snippets (mixed scripts, astral plane, combining marks,
    case folding, full-width forms, punctuation, long tokens, control chars),
  * the official benchmark corpus from tools/benchmark/fts_large_bench_gen.py
    (``--docs`` rows, 1000 by default),
  * each text through the ``beng``, ``ik`` and ``space`` parsers.

Usage (inside WSL, after both instances are up):

    python3 script/fts_tokenize_diff.py --old-port 2882 --new-port 2881
"""
import argparse
import importlib.util
import sys
from pathlib import Path

import pymysql

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
GENERATOR = REPO_ROOT / "tools" / "benchmark" / "fts_large_bench_gen.py"

PARSERS = ("beng", "ik", "space")

# 31 adversarial snippets.  They are intentionally unpleasant: the point is to
# hit case folding, script classification, width mapping and the ASCII fast paths.
ADVERSARIAL = [
    "hello world",
    "HELLO World MiXeD",
    "ABCdefGHI123",
    "user_name@example.com",
    "_leading_underscore and trailing_",
    "数据库性能优化实践",
    "全文索引 分词器 热路径",
    "中文，标点。符号！？；：、（）【】《》",
    "ＡＢＣ　１２３　ｆｕｌｌｗｉｄｔｈ",
    "😀 🚀 emoji in text",
    "中文😀混合emoji测试",
    "cafe\u0301 combining accent",
    "ÉÜÑ àçè latin-1 accents",
    "Ελληνικά κείμενο",
    "Русский текст пример",
    "النص العربي للاختبار",
    "עברית טקסט לדוגמה",
    "हिन्दी पाठ परीक्षण",
    "한국어 텍스트 테스트",
    "日本語のテキストです",
    "zero\u200bwidth\u200djoiner",
    "tab\tseparated\ntokens",
    "quote's \"double\" `backtick`",
    "back\\slash\\and\\\\double",
    ("longtoken" * 25),
    "重复中文" * 30,
    "numbers 3.14 42% -7 1,000 0x1F",
    "https://example.com/path?q=1&r=2",
    "sql LIKE '%_%' AND x <> 1",
    "混合Mixed中英text无缝no space",
    "「全角引号」『双引号』——破折号……",
    "   ",
]

# Extra probes requested for the charset/IK fast-path merge: dictionary heavy
# Chinese, the English stop word list, brand names with inner capitals and
# apostrophes, accented Latin, scientific notation and punctuation-only text.
FAST_PATH_SAMPLES = [
    "the of and to in is are was were be been for on at by it this that with as "
    "from or an but not have has had he she they we you i his her their our",
    "中华人民共和国人民大会堂",
    "OceanBase是一款非常稳定数据库，在110个星球上都非常著名，这简直难以置信",
    "人际关系",
    "iPhone eBay O'Reilly don't can't isn't",
    "naïve café Zürich ÅNGSTRÖM",
    "2.02e9 and 1E-7 and 3.14159e+10",
    "，。！？；：、（）【】《》〔〕……——",
    "ALL CAPS SENTENCE THAT MUST STILL FOLD",
    "mixedCASEwithCamelHumpsAndUPPER",
    "ANALYZE TABLE t1, T2 and ANALYSE",
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa_"
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
    "分词器分词器分词器分词器分词器分词器分词器分词器分词器分词器",
    "你好，世界！Hello, world! 123",
    "ｈｅｌｌｏ　ｗｏｒｌｄ　１２３",
    "e\u0301te\u0301 cafe\u0301 nai\u0308ve",
]

# Texts taken from the official fts_index cases (cn_word / tokenize_function /
# basic_dml) so the comparison also covers the corpus the graded suite uses.
OFFICIAL_TEST_TEXTS = [
    "abc英文空格 中文分号；英文分号;全角空格　全角逗号，等",
    "昨晚看的一部电影非常感人，让我思考了很多。",
    "最近迷上了一位古典音乐家的作品，他的钢琴曲非常优美。",
    "这次旅行中发生了很多有趣的事情，记录下来留作纪念。",
    "Completed the annual security audit. No major vulnerabilities were found, "
    "but we will implement additional measures for data protection.",
    "Sales have increased by 15% compared to last quarter. The sales team is "
    "working on strategies to maintain this growth.",
    "一二三四五 六七八",
    "123 4567890",
    "hello world",
]

# (parser, properties JSON) pairs.  The IK modes and the "all" output mode are
# part of the parser contract, and the merge touched the IK character fast path,
# so every mode has to produce exactly the same token stream as before.
PROBES = [
    ("beng", None),
    ("ik", None),
    ("space", None),
    ("ngram2", None),
    ("ik", '[{"additional_args": [{"ik_mode": "smart"}]}]'),
    ("ik", '[{"additional_args": [{"ik_mode": "max_word"}]}]'),
    ("ngram2", '[{"additional_args": [{"min_ngram_size": 2}, {"max_ngram_size": 3}]}]'),
    ("beng", '[{"output": "all"}]'),
    ("ik", '[{"output": "all"}]'),
    ("space", '[{"output": "all"}]'),
]

TOKENIZE_SQL = "select hex(tokenize(%s, %s)) as v"
TOKENIZE_SQL_PROPS = "select hex(tokenize(%s, %s, %s)) as v"


def load_corpus(docs):
    """Corpus rows (title, content, content_en) from the official generator."""
    if not GENERATOR.is_file():
        raise SystemExit("missing corpus generator: %s" % GENERATOR)
    spec = importlib.util.spec_from_file_location("fts_bench_gen", GENERATOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    rows = []
    for i in range(docs):
        _cat, title, body, title_en, body_en = module.make_row(i)
        rows.append(title)
        rows.append(body)
        rows.append(body_en)
    return rows


def connect(port, user, password):
    return pymysql.connect(host="127.0.0.1", port=port, user=user, password=password,
                           database="test", autocommit=True, charset="utf8mb4")


def tokenize(cursor, text, probe):
    parser, properties = probe
    if properties is None:
        cursor.execute(TOKENIZE_SQL, (text, parser))
    else:
        cursor.execute(TOKENIZE_SQL_PROPS, (text, parser, properties))
    row = cursor.fetchone()
    return row[0] if row else None


def compare_texts(texts, probes, old_cursor, new_cursor, verbose, label):
    diffs = 0
    checked = 0
    for probe in probes:
        for index, text in enumerate(texts):
            old_value = tokenize(old_cursor, text, probe)
            new_value = tokenize(new_cursor, text, probe)
            checked += 1
            if old_value != new_value:
                diffs += 1
                print("DIFF %s probe=%s sample=%d" % (label, probe, index))
                print("  input: %r" % (text,))
                print("  before: %r" % (old_value,))
                print("  after : %r" % (new_value,))
                if verbose and isinstance(old_value, str) and isinstance(new_value, str):
                    print("  before hex: %s" % old_value.encode("utf-8", "surrogateescape").hex())
                    print("  after  hex: %s" % new_value.encode("utf-8", "surrogateescape").hex())
    return checked, diffs


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--old-port", type=int, default=2882)
    parser.add_argument("--new-port", type=int, default=2881)
    parser.add_argument("--user", default="root")
    parser.add_argument("--password", default="")
    parser.add_argument("--docs", type=int, default=1000,
                        help="corpus rows taken from the official generator")
    parser.add_argument("--probes", default=None,
                        help="comma separated parser names to limit the probe set")
    parser.add_argument("--skip-adversarial", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    options = parser.parse_args(argv)

    probes = PROBES
    if options.probes:
        wanted = [p.strip() for p in options.probes.split(",") if p.strip()]
        probes = [probe for probe in PROBES if probe[0] in wanted]
        if not probes:
            raise SystemExit("no probe matches %r" % options.probes)

    corpus = load_corpus(options.docs)

    old = connect(options.old_port, options.user, options.password)
    new = connect(options.new_port, options.user, options.password)
    total_checked = total_diffs = 0
    try:
        old_cursor = old.cursor()
        new_cursor = new.cursor()
        groups = []
        if not options.skip_adversarial:
            groups.append(("adversarial", ADVERSARIAL))
        groups.append(("fastpath", FAST_PATH_SAMPLES))
        groups.append(("official", OFFICIAL_TEST_TEXTS))
        groups.append(("corpus", corpus))
        for label, texts in groups:
            checked, diffs = compare_texts(texts, probes, old_cursor, new_cursor,
                                           options.verbose, label)
            print("%-12s %5d text(s) x %2d probe(s) -> %d diff"
                  % (label + ":", len(texts), len(probes), diffs))
            total_checked += checked
            total_diffs += diffs
        old_cursor.close()
        new_cursor.close()
    finally:
        old.close()
        new.close()

    if total_diffs:
        print("TOKENIZE DIFF: %d difference(s) in %d comparison(s)" % (total_diffs, total_checked))
        return 1
    print("TOKENIZE IDENTICAL: %d comparison(s), 0 diff" % total_checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())