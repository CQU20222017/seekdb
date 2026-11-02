"""Capture native SQL results for baseline/candidate comparison.

Run with --client=/path/to/obclient --port=2893. JSON is written to stdout.
Uses and drops only the fts_semantics_regression database.
"""

import argparse
import json
import random
import subprocess


def capture(client, port, *, row_count=300):
    def sql(statements):
        result = subprocess.run(
            [client, "-h127.0.0.1", "-P" + str(port), "-uroot", "-N", "-B", "--raw",
             "--default-character-set=utf8mb4"],
            input="SET NAMES utf8mb4; SET ob_query_timeout=100000000;\n" + statements,
            text=True, capture_output=True, timeout=120,
        )
        if result.returncode:
            raise RuntimeError(result.stderr)
        return result.stdout.splitlines()

    def literal(text):
        return "CONVERT(0x%s USING utf8mb4)" % text.encode("utf-8").hex() if text else "''"

    texts = ["", " ", "...!?", "数据库数据库全文索引", "一二两三壹贰叁佰仟萬億兆卅廿",
             "OceanBase OCEANBASE oceanbase", "ＡＢＣ１２３ abc123 abc-123", "école café Ångström",
             "中文English한글カタカナひらがな", "emoji😀🌍 数据库\n连接\t超时",
             "a an the and of to with 的 了 和 是 在",  # IK must retain its existing stopword behavior.
             ("数据库连接超时 三百二十公斤 OceanBase的全文索引。" * 80)]
    rng = random.Random(1947)
    atoms = ["南京市长江大桥", "研究生命起源", "数据库", "全文索引", "SQL", "sql", "１２３",
             "一", "贰", "三百", " ", "-", ".", "\n", "é", "α", "한글", "カ", "😀", "𠀀"]
    texts += ["".join(rng.choices(atoms, k=rng.randint(1, 30))) for _ in range(128)]
    statements = []
    for text in texts:
        for parser, mode in [("ik", "smart"), ("ik", "max_word"), ("beng", None)]:
            options = [{"output": "all"}]
            if mode:
                options.append({"additional_args": [{"ik_mode": mode}]})
            statements.append("SELECT tokenize(%s,'%s','%s');" % (
                literal(text), parser, json.dumps(options)))
    result = {"tokenize": [json.loads(line) for line in sql("\n".join(statements))]}

    sql("DROP DATABASE IF EXISTS fts_semantics_regression; CREATE DATABASE fts_semantics_regression;"
        "USE fts_semantics_regression; CREATE TABLE docs(id INT PRIMARY KEY, body TEXT, english TEXT,"
        " FULLTEXT INDEX cn(body) WITH PARSER ik, FULLTEXT INDEX en(english) WITH PARSER beng);")
    values = []
    for identity in range(row_count):
        text = ["数据库 数据库 全文索引", "全文检索 查询 优化", "数据库连接 网络超时", "无关文档"][identity % 4]
        english = ["alpha alpha beta", "beta gamma", "database search engine", "unrelated"][identity % 4]
        values.append("(%d,%s,%s)" % (identity, literal(text), literal(english)))
    sql("USE fts_semantics_regression; INSERT INTO docs VALUES " + ",".join(values) + ";")
    queries = [("body", "数据库"), ("body", "数据库 全文索引"), ("body", "不存在"),
               ("english", "alpha beta"), ("english", "database engine")]
    for phase in range(4):
        if phase == 1:
            sql("UPDATE fts_semantics_regression.docs SET body='数据库 数据库 新词',"
                "english='alpha gamma alpha' WHERE id % 7=0;")
        elif phase == 2:
            sql("DELETE FROM fts_semantics_regression.docs WHERE id % 11=0;")
        elif phase == 3:
            sql("INSERT INTO fts_semantics_regression.docs VALUES(%d,'数据库 全文索引','alpha beta gamma');"
                % (row_count + 100))
        statements = ["USE fts_semantics_regression;"]
        for column, query in queries:
            score = "MATCH(%s) AGAINST(%s)" % (column, literal(query))
            statements.append("SELECT id,%s FROM docs WHERE %s>0 ORDER BY id;" % (score, score))
            statements.append("SELECT id,%s FROM docs WHERE %s>0 AND id%%3=0 "
                              "ORDER BY %s DESC,id LIMIT 17;" % (score, score, score))
        result["query_phase_%d" % phase] = sql("\n".join(statements))
        # Exercise BOOL(MATCH), score predicates and output batch boundaries.
        # Preserve query boundaries as well as every ID and exact score.
        statements = ["USE fts_semantics_regression;"]
        scores = ["MATCH(%s) AGAINST(%s)" % (column, literal(query))
                  for column, query in queries]
        scores += ["MATCH(english) AGAINST('+alpha -gamma' IN BOOLEAN MODE)",
                   "MATCH(english) AGAINST('alpha beta' IN BOOLEAN MODE)"]
        for index, score in enumerate(scores):
            for size in [1, 20, 255, 256, 257]:
                statements.append("SELECT 'plain_%d_%d';" % (index, size))
                statements.append("SELECT id,%s FROM docs WHERE %s ORDER BY id LIMIT %d OFFSET 3;"
                                  % (score, score, size))
            for predicate in [score + ">0.01", "(" + score + ") AND id%3=0"]:
                statements.append("SELECT 'predicate_%d';" % index)
                statements.append("SELECT id,%s FROM docs WHERE %s ORDER BY id;" % (score, predicate))
            statements.append("SELECT 'bool_projection_%d';" % index)
            statements.append("SELECT id,%s,CASE WHEN %s THEN 'match' ELSE 'none' END "
                              "FROM docs WHERE %s ORDER BY id LIMIT 257 OFFSET 3;"
                              % (score, score, score))
        result["query_batch_phase_%d" % phase] = sql("\n".join(statements))
    sql("DROP DATABASE fts_semantics_regression;")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--client", required=True)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--rows", type=int, default=300,
                        help="use 2300 or more rows to exercise multiple TAAT partitions")
    args = parser.parse_args()
    if args.rows <= 0:
        parser.error("--rows must be positive")
    print(json.dumps(capture(args.client, args.port, row_count=args.rows),
                     ensure_ascii=False, sort_keys=True))
