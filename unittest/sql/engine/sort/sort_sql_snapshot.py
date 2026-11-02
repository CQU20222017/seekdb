"""Native sort regression snapshots for two seekdb binaries.

Use a dedicated instance: this creates/drops only sort_access_regression.
For external merge coverage, start the instance with workarea_size_policy=MANUAL
and _sort_area_size=2M. Compare snapshots against an unmodified binary.
"""
import argparse
import json
import subprocess


def capture(client, port, row_count=6000, require_prefix_plan=False):
    def sql(statement):
        run = subprocess.run(
            [client, "-h127.0.0.1", "-P" + str(port), "-uroot", "-N", "-B", "--raw",
             "--default-character-set=utf8mb4"],
            input="SET NAMES utf8mb4; SET ob_query_timeout=120000000;\n" + statement,
            capture_output=True, text=True, timeout=150,
        )
        if run.returncode:
            raise RuntimeError(run.stderr)
        return run.stdout.splitlines()

    sql("DROP DATABASE IF EXISTS sort_access_regression; CREATE DATABASE sort_access_regression;"
        "USE sort_access_regression; CREATE TABLE docs (id INT PRIMARY KEY, k INT, g INT, "
        "txt VARCHAR(100), payload LONGTEXT) CHARSET=utf8mb4;")
    words = [None, "", "alpha", "Alpha", "ALPHA ", "école", "ecole", "数据库", "😀", "a  ",
             "\0", "a\0b"]
    values = []
    for i in range(row_count):
        number = None if i % 13 == 0 else (i * 37) % 97 - 48
        values.append((i, number))
    for offset in range(0, row_count, 500):
        rows = []
        for i, number in values[offset:offset + 500]:
            word = words[i % len(words)]
            text = "NULL" if word is None else "CONVERT(0x%s USING utf8mb4)" % word.encode().hex() if word else "''"
            rows.append("(%d,%s,%d,%s,CONCAT(%s,REPEAT('x',1100),LPAD(%d,6,'0')))"
                        % (i, "NULL" if number is None else number, i % 19, text, text, i))
        sql("INSERT INTO sort_access_regression.docs VALUES " + ",".join(rows) + ";")
    result = {}
    try:
        sql("CREATE INDEX by_g ON sort_access_regression.docs(g);")
        for collation in ["utf8mb4_bin", "utf8mb4_general_ci"]:
            for direction in ["ASC", "DESC"]:
                order = f"g,txt COLLATE {collation} {direction},k DESC,id"
                select = ("SELECT /*+ INDEX(d by_g) OPT_PARAM('enable_newsort','%s') */ id "
                          "FROM sort_access_regression.docs d ORDER BY " + order + ";")
                expected = sql(select % "false")
                actual = sql(select % "true")
                assert actual == expected, (collation, direction, "encoded prefix sort")
                if require_prefix_plan:
                    plan = "\n".join(sql("EXPLAIN EXTENDED " + (select % "true"))).lower()
                    assert "prefix_pos(1)" in plan, plan
                result["prefix_" + collation + "_" + direction] = actual
        result["numeric"] = sql("SELECT id FROM sort_access_regression.docs ORDER BY k ASC,id DESC;")
        expected = [str(i) for i, number in sorted(
            values, key=lambda pair: (pair[1] is not None, pair[1] or 0, -pair[0]))]
        assert result["numeric"] == expected, "numeric/NULL ordering disagrees with independent oracle"
        for collation in ["utf8mb4_bin", "utf8mb4_general_ci"]:
            for direction in ["ASC", "DESC"]:
                order = "txt COLLATE %s %s,k DESC,id ASC" % (collation, direction)
                query = "SELECT id FROM sort_access_regression.docs ORDER BY " + order
                key = collation + "_" + direction
                result[key] = sql(query + ";")
                for size in [1, 20, 255, 256, 257]:
                    result[key + "_limit_" + str(size)] = sql(query + " LIMIT %d OFFSET 13;" % size)
                    assert result[key + "_limit_" + str(size)] == result[key][13:13 + size]
        result["window"] = sql(
            "SELECT id,ROW_NUMBER() OVER(PARTITION BY g ORDER BY k DESC,txt COLLATE utf8mb4_bin,id),"
            "RANK() OVER(PARTITION BY g ORDER BY k DESC) FROM sort_access_regression.docs ORDER BY id;")
        result["group"] = sql("SELECT HEX(txt),COUNT(*) FROM sort_access_regression.docs "
                              "GROUP BY HEX(txt) ORDER BY COUNT(*) DESC,HEX(txt);")
        result["long_keys"] = sql("SELECT id FROM sort_access_regression.docs "
                                  "ORDER BY payload COLLATE utf8mb4_bin DESC,k,id;")
        result["ci_long_keys"] = sql("SELECT id FROM sort_access_regression.docs "
                                     "ORDER BY payload COLLATE utf8mb4_general_ci DESC,k,id;")
        result["ci_group"] = sql("SELECT MIN(id),COUNT(*) FROM sort_access_regression.docs "
                                 "GROUP BY txt COLLATE utf8mb4_general_ci ORDER BY MIN(id);")
        result["ci_distinct"] = sql("SELECT COUNT(DISTINCT txt COLLATE utf8mb4_general_ci) "
                                    "FROM sort_access_regression.docs;")
        result["ci_join"] = sql(
            "SELECT a.id,b.id FROM sort_access_regression.docs a JOIN sort_access_regression.docs b "
            "ON a.txt COLLATE utf8mb4_general_ci=b.txt COLLATE utf8mb4_general_ci "
            "WHERE a.id<40 AND b.id<60 ORDER BY a.id,b.id;")
        result["ci_rank"] = sql("SELECT id,DENSE_RANK() OVER(ORDER BY txt COLLATE utf8mb4_general_ci) "
                                "FROM sort_access_regression.docs ORDER BY id;")
        # Two large prefix groups exercise spilling with the documented 2M
        # work area, including wide keys, NULLs and the remaining key columns.
        sql("UPDATE sort_access_regression.docs SET g=id%2;")
        select = ("SELECT /*+ INDEX(d by_g) OPT_PARAM('enable_newsort','%s') */ id "
                  "FROM sort_access_regression.docs d ORDER BY g,payload DESC,k DESC,id;")
        result["prefix_external"] = sql(select % "true")
        assert result["prefix_external"] == sql(select % "false"), "external encoded prefix sort"
        select = select.replace("payload DESC", "CAST(payload AS CHAR(2000)) COLLATE utf8mb4_bin DESC")
        result["prefix_external_encoded"] = sql(select % "true")
        assert result["prefix_external_encoded"] == sql(select % "false"), "external encoded varchar prefix sort"
        if require_prefix_plan:
            plan = "\n".join(sql("EXPLAIN EXTENDED " + (select % "true"))).lower()
            assert "prefix_pos(1)" in plan, plan
    finally:
        sql("DROP DATABASE sort_access_regression;")
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--client", required=True)
    parser.add_argument("--port", type=int, required=True)
    args = parser.parse_args()
    print(json.dumps(capture(args.client, args.port), ensure_ascii=False, sort_keys=True))
