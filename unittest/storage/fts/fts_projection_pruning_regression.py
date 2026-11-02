"""Check restricted FTS projection pruning on a dedicated SQL instance.

Creates and drops only fts_projection_pruning_regression. Every query is compared
with the same query using NO_PROJECT_PRUNE on the derived table. --require-pruning
also verifies that unused-column plans avoid lookup while preserving score order.
--require-count-membership permits the cardinality-only COUNT/LIMIT view to avoid
scoring; value-observing and explicitly ordered queries keep their original path.
"""
import argparse
import json
import subprocess


def verify(client, port, require_pruning=False, require_count_membership=False):
    db = "fts_projection_pruning_regression"

    def sql(text):
        result = subprocess.run(
            [client, "-h127.0.0.1", "-P" + str(port), "-uroot", "-N", "-B", "--raw",
             "--default-character-set=utf8mb4"],
            input="SET ob_query_timeout=120000000;\n" + text,
            text=True, encoding="utf-8", capture_output=True, timeout=180,
        )
        if result.returncode:
            raise RuntimeError(result.stderr + "\n" + text)
        return result.stdout.splitlines()

    sql(f"DROP DATABASE IF EXISTS {db}; CREATE DATABASE {db}; USE {db};"
        "CREATE TABLE docs(id INT PRIMARY KEY, nullable_id INT, grp INT, body TEXT);")
    for start in range(1, 514, 100):
        values = []
        for i in range(start, min(start + 100, 514)):
            body = ("NULL" if i % 17 == 0 else "''" if i % 19 == 0 else
                    "'alpha beta beta'" if i % 3 == 0 else "'gamma delta'")
            nullable_id = "NULL" if i % 5 == 0 else str(i)
            values.append(f"({i},{nullable_id},{i % 7},{body})")
        sql(f"INSERT INTO {db}.docs VALUES " + ",".join(values) + ";")
    sql(f"ALTER TABLE {db}.docs ADD FULLTEXT INDEX ft(body) WITH PARSER beng;")

    results = {}
    statements = {}
    plans_checked = 0
    match = "MATCH(body) AGAINST('alpha gamma')"

    def compare(name, projection, tail, outer="COUNT(*)", suffix="", predicate=match):
        child = f"{{hint}} {projection} FROM {db}.docs WHERE {predicate} {tail}"
        statement = f"SELECT {outer} FROM (SELECT {child}) t {suffix};"
        actual = sql(statement.format(hint=""))
        reference = sql(statement.format(hint="/*+ NO_PROJECT_PRUNE */"))
        assert actual == reference, (name, actual, reference)
        results[name] = actual
        statements[name] = statement.format(hint="").replace("SELECT  ", "SELECT ")
        return statements[name]

    try:
        for limit, offset in [(0, 0), (1, 0), (20, 0), (257, 0), (1000, 0),
                              (20, 5), (20, 256), (20, 2000)]:
            compare(f"limit_{limit}_{offset}", "id", f"LIMIT {limit} OFFSET {offset}")

        count_query = compare("membership_count", "id", "LIMIT 20")
        assert results["membership_count"] == ["20"]
        assert results["limit_0_0"] == ["0"]
        assert results["limit_20_2000"] == ["0"]

        # Removing the select-list position must never remove its ORDER BY use.
        compare("order_id", "id", "ORDER BY id DESC LIMIT 20 OFFSET 3")
        compare("order_alias", "id AS selected_id", "ORDER BY selected_id DESC LIMIT 20")
        compare("order_ordinal", "id,nullable_id", "ORDER BY 1 DESC LIMIT 20")
        compare("order_second_ordinal", "nullable_id,id", "ORDER BY 2 DESC LIMIT 20")
        compare("order_nullable", "nullable_id,id", "ORDER BY nullable_id IS NULL,nullable_id,id LIMIT 20")
        compare("order_score", "id", f"ORDER BY {match} DESC,id LIMIT 20 OFFSET 4")
        compare("order_score_asc", "id", f"ORDER BY {match},id LIMIT 20 OFFSET 4")
        compare("used_id_order", "id,nullable_id", "ORDER BY id DESC LIMIT 20", "id", "ORDER BY id")
        compare("used_alias_order", "id AS selected_id,nullable_id", "ORDER BY 1 DESC LIMIT 20",
                "selected_id", "ORDER BY selected_id")
        compare("removed_order_alias", "id AS sort_key,nullable_id",
                "ORDER BY sort_key DESC LIMIT 20 OFFSET 3", "nullable_id", "ORDER BY nullable_id")
        compare("removed_order_first_ordinal", "id,nullable_id",
                "ORDER BY 1 DESC LIMIT 20 OFFSET 3", "nullable_id", "ORDER BY nullable_id")
        compare("removed_order_second_ordinal", "nullable_id,id",
                "ORDER BY 2 DESC LIMIT 20 OFFSET 3", "nullable_id", "ORDER BY nullable_id")
        compare("removed_order_multiple", "grp,id,nullable_id",
                "ORDER BY 1 DESC,2 DESC LIMIT 20 OFFSET 3", "nullable_id", "ORDER BY nullable_id")

        count_id = compare("count_id", "id", "LIMIT 20", "COUNT(id)")
        count_nullable = compare("count_nullable", "nullable_id", "ORDER BY id LIMIT 20",
                                 "COUNT(nullable_id)")
        compare("count_and_sum", "id,nullable_id,grp", "ORDER BY id LIMIT 30 OFFSET 5",
                "COUNT(*),COUNT(nullable_id),SUM(id),MIN(grp),MAX(grp)")
        compare("outer_filter", "id,nullable_id", "ORDER BY id LIMIT 30", "COUNT(*)",
                "WHERE nullable_id IS NOT NULL")
        compare("inner_filter", "id", "AND id%3=0 LIMIT 20")
        compare("outer_group", "id,grp", "ORDER BY id LIMIT 30", "grp,COUNT(*)",
                "GROUP BY grp HAVING COUNT(*)>1 ORDER BY grp")

        # These shapes stay on the original rewrite policy, including expressions
        # and DISTINCT whose evaluation/cardinality must not be changed.
        distinct_query = compare("distinct_nullable", "DISTINCT nullable_id", "LIMIT 20")
        compare("distinct_group", "DISTINCT grp", "ORDER BY grp LIMIT 20")
        compare("computed_projection", "id+1 AS computed", "LIMIT 20")
        compare("score_projection", f"id,{match} AS score", "LIMIT 20")
        compare("window_projection", "id,ROW_NUMBER() OVER(ORDER BY id) AS rn", "LIMIT 20")
        compare("grouped_child", "grp", "GROUP BY grp ORDER BY grp LIMIT 20")
        compare("plain_no_match", "id,nullable_id", "ORDER BY id LIMIT 20", predicate="id>0")
        compare("boolean_mode", "id", "LIMIT 20",
                predicate="MATCH(body) AGAINST('+alpha -delta' IN BOOLEAN MODE)")
        compare("no_matches", "id", "LIMIT 20", predicate="MATCH(body) AGAINST('absentword')")
        compare("null_query", "id", "LIMIT 20", predicate="MATCH(body) AGAINST(NULL)")
        assert results["no_matches"] == ["0"]
        assert results["null_query"] == ["0"]

        for name, statement in [
            ("cte", f"WITH t AS (SELECT {{hint}} id FROM {db}.docs WHERE {match} LIMIT 20) "
                    "SELECT COUNT(*) FROM t;"),
            ("nested_derived", "SELECT COUNT(*) FROM (SELECT id FROM "
                               f"(SELECT {{hint}} id FROM {db}.docs WHERE {match} LIMIT 20) t) u;"),
            ("outer_join", f"SELECT COUNT(*) FROM (SELECT {{hint}} id FROM {db}.docs "
                           f"WHERE {match} LIMIT 20) t JOIN {db}.docs d ON t.id=d.id;"),
            ("found_rows", f"SELECT SQL_CALC_FOUND_ROWS COUNT(*) FROM (SELECT {{hint}} id "
                           f"FROM {db}.docs WHERE {match} LIMIT 20) t; SELECT FOUND_ROWS();"),
        ]:
            actual = sql(statement.format(hint=""))
            reference = sql(statement.format(hint="/*+ NO_PROJECT_PRUNE */"))
            assert actual == reference, (name, actual, reference)
            results[name] = actual
            statements[name] = statement.format(hint="").replace("SELECT  ", "SELECT ")

        sql(f"CREATE VIEW {db}.fts_view AS SELECT id FROM {db}.docs WHERE {match} LIMIT 20;")
        statements["view"] = f"SELECT COUNT(*) FROM {db}.fts_view;"
        results["view"] = sql(statements["view"])
        assert results["view"] == results["membership_count"]
        lateral = (f"SELECT COUNT(*) FROM (SELECT {{hint}} id FROM {db}.docs "
                   f"WHERE {match} LIMIT 20) t CROSS JOIN LATERAL (SELECT t.id) correlated;")
        results["lateral"] = sql(lateral.format(hint=""))
        assert results["lateral"] == sql(lateral.format(hint="/*+ NO_PROJECT_PRUNE */"))
        statements["lateral"] = lateral.format(hint="").replace("SELECT  ", "SELECT ")

        if require_pruning:
            plan = "\n".join(sql("EXPLAIN EXTENDED " + count_query))
            assert "is_index_back=false" in plan, plan
            if require_count_membership:
                assert "calc_relevance=false" in plan and "sort_keys(" not in plan, plan
            else:
                assert "calc_relevance=true" in plan and "sort_keys([MATCH(" in plan, plan
                assert "DESC]" in plan or ", DESC])" in plan, plan
            assert "limit(20" in plan, plan
            assert "PROJECT_PRUNE(" in plan, plan
            plans_checked += 1
            for query in [count_id, count_nullable, distinct_query,
                          count_query.replace("SELECT id FROM", "SELECT /*+ NO_PROJECT_PRUNE */ id FROM"),
                          count_query.replace("SELECT id FROM", "SELECT /*+ NO_REWRITE */ id FROM")]:
                plan = "\n".join(sql("EXPLAIN EXTENDED " + query))
                assert "is_index_back=true" in plan, plan
                plans_checked += 1
            for name in ["distinct_nullable", "computed_projection", "score_projection",
                         "window_projection", "grouped_child", "boolean_mode", "cte",
                         "nested_derived", "outer_join", "found_rows", "view", "lateral"]:
                # The statement remains excluded from the newly enabled rule.
                query = statements[name].split(";", 1)[0] + ";"
                plan = "\n".join(sql("EXPLAIN EXTENDED " + query))
                assert "PROJECT_PRUNE(" not in plan, (name, plan)
                plans_checked += 1

        # Prepared executions change OFFSET/LIMIT while preserving NULL and
        # expression identity; both versions reuse their own prepared statement.
        prepared = []
        for name, hint in [("pruned", ""), ("reference", "/*+ NO_PROJECT_PRUNE */")]:
            query = (f"SELECT COUNT(*) FROM (SELECT {hint} id FROM {db}.docs "
                     "WHERE MATCH(body) AGAINST(?) LIMIT ? OFFSET ?) t")
            commands = [f"PREPARE ps FROM '{query}';"]
            for text, limit, offset in [("alpha", 20, 0), ("gamma", 5, 3), ("absentword", 20, 0)]:
                commands += [f"SET @query='{text}',@lim={limit},@off={offset};",
                             "EXECUTE ps USING @query,@lim,@off;"]
            commands.append("DEALLOCATE PREPARE ps;")
            prepared.append(sql("\n".join(commands)))
        assert prepared[0] == prepared[1], prepared
        results["prepared"] = prepared[0]

        for index, change in enumerate([
            "UPDATE docs SET body='alpha restored',nullable_id=NULL WHERE id%7=0",
            "DELETE FROM docs WHERE id%11=0",
            "UPDATE docs SET body=NULL WHERE id%13=0",
        ]):
            sql(f"USE {db}; {change};")
            compare(f"changed_{index}", "id", "LIMIT 20 OFFSET 3")
            compare(f"changed_nullable_{index}", "nullable_id", "ORDER BY id LIMIT 30", "COUNT(nullable_id)")
    finally:
        sql(f"DROP DATABASE {db};")
    return {"queries_checked": len(results), "plans_checked": plans_checked, "results": results}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--client", required=True)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--require-pruning", action="store_true")
    parser.add_argument("--require-count-membership", action="store_true")
    args = parser.parse_args()
    print(json.dumps(verify(args.client, args.port,
                           args.require_pruning or args.require_count_membership,
                           args.require_count_membership), sort_keys=True))
