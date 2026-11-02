"""Compare membership-only MATCH with scored execution on a dedicated instance.

Creates and drops only fts_membership_regression. Run with --require-fast-path
to also check that eligible plans omit relevance calculation.
"""
import argparse
import json
import subprocess


def verify(client, port, require_fast_path=False):
    def sql(text):
        result = subprocess.run(
            [client, "-h127.0.0.1", "-P" + str(port), "-uroot", "-N", "-B", "--raw",
             "--default-character-set=utf8mb4"],
            input="SET ob_query_timeout=120000000;\n" + text,
            text=True, encoding="utf-8", capture_output=True, timeout=180,
        )
        if result.returncode:
            raise RuntimeError(result.stderr)
        return result.stdout.splitlines()

    db = "fts_membership_regression"
    sql(f"DROP DATABASE IF EXISTS {db}; CREATE DATABASE {db}; USE {db};"
        "CREATE TABLE docs(id INT PRIMARY KEY, body TEXT, "
        "FULLTEXT INDEX ft(body) WITH PARSER beng);")
    for start in range(1, 1026, 200):
        values = []
        for i in range(start, min(start + 200, 1026)):
            text = ("NULL" if i % 17 == 0 else "''" if i % 13 == 0 else
                    "'alpha beta beta'" if i % 3 == 0 else "'gamma delta'")
            values.append(f"({i},{text})")
        sql(f"INSERT INTO {db}.docs VALUES " + ",".join(values) + ";")

    checked = 0
    plans = 0
    long_query = "alpha " + " ".join("term" + chr(97 + i // 26) + chr(97 + i % 26) for i in range(270))
    queries = ["alpha", "gamma", "alpha gamma", "alpha alpha gamma", "absentword",
               "alpha beta", "alpha beta gamma", "alpha beta gamma delta",
               "alpha beta gamma delta absentword", "absentword alpha beta gamma",
               "", "the and or", long_query]
    try:
        for query in queries:
            score = "MATCH(body) AGAINST('%s')" % query
            for extra in ["", " AND id%5=0", " AND id BETWEEN 255 AND 769"]:
                where = score + extra
                plain = f"SELECT id FROM {db}.docs WHERE {where} ORDER BY id;"
                scored = f"SELECT id,{score} FROM {db}.docs WHERE {where} ORDER BY id;"
                expected = [line.split("\t")[0] for line in sql(scored)]
                assert sql(plain) == expected, (query, extra, "identities")
                count_sql = f"SELECT COUNT(*) FROM {db}.docs WHERE {where};"
                assert sql(count_sql) == [str(len(expected))], (query, extra, "count")
                if require_fast_path and query in ("alpha", "alpha gamma"):
                    plan = "\n".join(sql("EXPLAIN EXTENDED " + count_sql))
                    assert "calc_relevance=false" in plan, plan
                    plans += 1
                checked += 2

        # Numeric uses must retain actual scores, including aliases and HAVING.
        score = "MATCH(body) AGAINST('alpha gamma')"
        fallbacks = [
            f"SELECT id,{score} AS s FROM {db}.docs WHERE {score} ORDER BY s,id;",
            f"SELECT id FROM {db}.docs WHERE {score} ORDER BY {score},id;",
            f"SELECT id,{score} AS s FROM {db}.docs WHERE {score} HAVING s>0.1 ORDER BY id;",
            f"SELECT COUNT(*) FROM {db}.docs WHERE {score}>0.1;",
            f"SELECT id FROM {db}.docs WHERE {score} ORDER BY id LIMIT 20;",
            f"SELECT id FROM {db}.docs WHERE MATCH(body) AGAINST('+alpha -delta' IN BOOLEAN MODE);",
        ]
        for query in fallbacks:
            plan = "\n".join(sql("EXPLAIN EXTENDED " + query))
            assert "calc_relevance=false" not in plan, plan
            sql(query)
            checked += 1

        # Rescans after writes, deletes and restoring text must not reuse stale hits.
        for update in ["UPDATE docs SET body='alpha restored' WHERE id%7=0;",
                       "DELETE FROM docs WHERE id%11=0;",
                       "UPDATE docs SET body=NULL WHERE id%19=0;"]:
            sql(f"USE {db};" + update)
            expected = sql(f"SELECT id,{score} FROM {db}.docs WHERE {score} ORDER BY id;")
            actual = sql(f"SELECT id FROM {db}.docs WHERE {score} ORDER BY id;")
            assert actual == [line.split("\t")[0] for line in expected]
            checked += 1

        # Build indexes after loading data, so the posting scans also exercise
        # persisted index rows and different document-ID/rowkey mappings.
        layouts = [
            ("heap_docs", "id INT, body TEXT", "id,body", "id", "id%5=0"),
            ("composite_docs", "tenant INT, k VARCHAR(128), body TEXT, PRIMARY KEY(tenant,k)",
             "id%3,CONCAT('文档-',id),body", "CONCAT(tenant,':',k)", "tenant=0"),
        ]
        for table, columns, values, identity, extra in layouts:
            sql(f"USE {db}; CREATE TABLE {table}({columns});"
                f"INSERT INTO {table} SELECT {values} FROM docs;"
                f"ALTER TABLE {table} ADD FULLTEXT INDEX ft(body) WITH PARSER beng;")
            for query in ["alpha", "alpha gamma", "alpha beta gamma delta",
                          "alpha beta gamma delta absentword", "absentword", long_query]:
                score = f"MATCH(body) AGAINST('{query}')"
                for suffix in ["", " AND " + extra]:
                    where = score + suffix
                    expected = sql(f"SELECT {identity},{score} FROM {db}.{table} "
                                   f"WHERE {where} ORDER BY {identity};")
                    actual = sql(f"SELECT {identity} FROM {db}.{table} "
                                 f"WHERE {where} ORDER BY {identity};")
                    assert actual == [line.split("\t")[0] for line in expected], (table, query)
                    assert sql(f"SELECT COUNT(*) FROM {db}.{table} WHERE {where};") == [str(len(actual))]
                    checked += 2
    finally:
        sql(f"DROP DATABASE {db};")
    return {"queries_checked": checked, "fast_plans_checked": plans, "initial_rows": 1025}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--client", required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--require-fast-path", action="store_true")
    args = parser.parse_args()
    print(json.dumps(verify(args.client, args.port, args.require_fast_path)))
