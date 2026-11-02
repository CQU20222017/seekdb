"""Check table identity and correlated bindings without normalizing results.

Requires an isolated seekdb instance. Uses only fts_fixture_regression.
"""

import argparse
import json
from pathlib import Path
import re
import subprocess


def verify(client, port):
    def sql(statement):
        result = subprocess.run(
            [client, "-h127.0.0.1", "-P" + str(port), "-uroot", "-N", "-B", "--raw",
             "--default-character-set=utf8mb4"],
            input="SET NAMES utf8mb4; SET ob_query_timeout=100000000; " + statement,
            text=True, capture_output=True, timeout=60, check=True,
        )
        return result.stdout.strip()

    database = "fts_fixture_regression"
    sql(f"CREATE DATABASE {database}")
    try:
        def run(statement):
            return sql(f"USE {database}; " + statement)

        run("CREATE TABLE docs(seq INT PRIMARY KEY, detail TEXT, FULLTEXT KEY ft(detail)) "
            "PARTITION BY HASH(seq) PARTITIONS 2; CREATE TABLE dim(seq INT);")
        rows = [(0, "computer"), (2, "computer"), (4, "unrelated"), (5, "computer")]
        run("INSERT INTO docs VALUES " + ",".join(f"({i},'{text}')" for i, text in rows)
            + "; INSERT INTO dim VALUES(1),(3),(6);")
        query = ("SELECT /*+ LEADING(dim docs) USE_NL(docs) USE_DAS(docs) INDEX(docs PRIMARY) */ "
                 "docs.seq, dim.seq FROM docs, dim WHERE docs.seq < dim.seq "
                 "AND MATCH(docs.detail) AGAINST('computer') ORDER BY docs.seq,dim.seq")
        plan = run("EXPLAIN " + query)
        outer = re.findall(r"nl_params_\(\[dim\.seq\(:(\d+)\)\]", plan)
        inner = re.findall(r"range_cond\(\[docs\.seq < :(\d+)\]", plan)
        assert len(outer) == 1 and inner == outer, plan
        expected = [(i, d) for i, text in rows if text == "computer" for d in [1, 3, 6] if i < d]
        actual = [tuple(map(int, line.split("\t"))) for line in run(query).splitlines()]
        assert actual == expected, (actual, expected)
        # Projecting MATCH can reserve another internal parameter before the
        # correlated range parameter. Check the raw binding and joined IDs too.
        scored_query = query.replace("docs.seq, dim.seq FROM", "docs.seq, dim.seq, "
                                     "MATCH(docs.detail) AGAINST('computer') AS score FROM")
        scored_plan = run("EXPLAIN " + scored_query)
        scored_outer = re.findall(r"nl_params_\(\[dim\.seq\(:(\d+)\)\]", scored_plan)
        scored_inner = re.findall(r"range_cond\(\[docs\.seq < :(\d+)\]", scored_plan)
        assert len(scored_outer) == 1 and scored_inner == scored_outer, scored_plan
        scored_rows = [line.split("\t") for line in run(scored_query).splitlines()]
        assert [tuple(map(int, row[:2])) for row in scored_rows] == expected, scored_rows
        assert all(float(row[2]) > 0 for row in scored_rows), scored_rows

        # Check the suite's exact plan too, before its display-only replacement.
        repository = Path(__file__).resolve().parents[3]
        suite = (repository / "tools/deploy/mysql_test/test_suite/fts_index/t/"
                 "partitioned_simple_query.test").read_text()
        setup_start = suite.index("create table t_ir_single_column(")
        run(suite[setup_start:suite.index("--result_format 4", setup_start)])
        query_start = suite.index("select /*+ use_das(t_ir_single_column) */\n"
                                  "    t_ir_single_column.digest, t_ir_single_column.seq,")
        suite_query = suite[query_start:suite.index(";", query_start)]
        suite_plan = run("EXPLAIN " + suite_query)
        suite_outer = re.findall(r"nl_params_\(\[t_dim_table\.seq\(:(\d+)\)\]", suite_plan)
        suite_inner = re.findall(r"range_cond\(\[t_ir_single_column\.seq < :(\d+)\]", suite_plan)
        assert len(suite_outer) == 1 and suite_inner == suite_outer, suite_plan
        matched = [int(row) for row in run("SELECT seq FROM t_ir_single_column WHERE "
                   "MATCH(detail) AGAINST('computer') ORDER BY seq").splitlines()]
        dimensions = [int(row) for row in run("SELECT seq FROM t_dim_table ORDER BY seq").splitlines()]
        suite_expected = [(i, d) for i in matched for d in dimensions if i < d]
        suite_actual = [tuple(map(int, row.split("\t")[1:3]))
                        for row in run(suite_query).splitlines()]
        assert suite_actual == suite_expected, (suite_actual, suite_expected)

        db_id = int(sql("SELECT database_id FROM oceanbase.__all_database "
                        f"WHERE database_name='{database}'"))

        def identity():
            return int(sql("SELECT table_id FROM oceanbase.__all_table "
                           f"WHERE table_name='docs' AND database_id={db_id}"))

        old_id = identity()
        old_mapping_count = int(sql("SELECT COUNT(*) FROM oceanbase.__all_table "
                                    f"WHERE data_table_id={old_id} AND index_type IN (11,12)"))
        assert old_mapping_count == 2, old_mapping_count
        run("DROP TABLE docs; CREATE TABLE docs(title VARCHAR(200),content TEXT, "
            "FULLTEXT KEY ft(title,content));")
        new_id = identity()
        assert old_id != new_id, (old_id, new_id)
        auxiliary = [tuple(map(int, row.split("\t"))) for row in sql(
            "SELECT index_type, table_id FROM oceanbase.__all_table "
            f"WHERE data_table_id={new_id} AND index_type IN (11,12,13,14) "
            "ORDER BY index_type").splitlines()]
        assert [kind for kind, _ in auxiliary] == [13, 14], auxiliary
        for kind, table_id in auxiliary:
            columns = sql("SELECT column_name FROM oceanbase.__all_column "
                          f"WHERE table_id={table_id} ORDER BY column_id").splitlines()
            assert columns, (kind, table_id)
            assert "seq" not in columns, columns
            for prefix in ["__word_segment_", "__word_count_", "__doc_length_"]:
                assert any(name.startswith(prefix) for name in columns), columns
        run("INSERT INTO docs VALUES('fixture','computer evidence'),('other','unrelated');")
        assert run("SELECT title FROM docs WHERE MATCH(title,content) AGAINST('computer')") == "fixture"
        run("UPDATE docs SET content='corrected evidence' WHERE title='fixture'")
        assert run("SELECT title FROM docs WHERE MATCH(title,content) AGAINST('corrected')") == "fixture"
        run("DELETE FROM docs WHERE title='fixture'")
        assert run("SELECT COUNT(*) FROM docs WHERE MATCH(title,content) AGAINST('corrected')") == "0"
        return {"identity_recreated": True, "auxiliary_schemas": 2,
                "primary_key_mapping_tables": old_mapping_count, "heap_mapping_tables": 0,
                "correlated_parameters": [int(outer[0]), int(scored_outer[0])],
                "suite_correlated_parameter": int(suite_outer[0]),
                "suite_joined_rows": len(suite_actual),
                "joined_rows": len(actual)}
    finally:
        sql(f"DROP DATABASE {database}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--client", required=True)
    parser.add_argument("--port", required=True, type=int)
    args = parser.parse_args()
    print(json.dumps(verify(args.client, args.port), sort_keys=True))
