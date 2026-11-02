"""Check MATCH boolean output across batches on a dedicated test instance.

Creates and drops only the fts_filter_batch_regression database.
"""
import argparse
import json
import subprocess


def verify(client, port):
    def sql(text):
        result = subprocess.run(
            [client, "-h127.0.0.1", "-P" + str(port), "-uroot", "-N", "-B", "--raw",
             "--default-character-set=utf8mb4"],
            input="SET ob_query_timeout=120000000;\n" + text,
            text=True, capture_output=True, timeout=180,
        )
        if result.returncode:
            raise RuntimeError(result.stderr)
        return result.stdout.splitlines()

    rows = {}
    for index in range(1, 1026):
        rows[index] = (None if index % 17 == 0 else "" if index % 13 == 0
                       else "alpha beta" if index % 3 == 0 else "gamma delta")
    setup = ["DROP DATABASE IF EXISTS fts_filter_batch_regression;",
             "CREATE DATABASE fts_filter_batch_regression;",
             "USE fts_filter_batch_regression;",
             "CREATE TABLE docs(id INT PRIMARY KEY, body TEXT, "
             "FULLTEXT INDEX ft(body) WITH PARSER beng);"]
    items = list(rows.items())
    for start in range(0, len(items), 200):
        values = ["(%d,%s)" % (index, "NULL" if text is None else "'%s'" % text)
                  for index, text in items[start:start + 200]]
        setup.append("INSERT INTO docs VALUES " + ",".join(values) + ";")
    sql("\n".join(setup))
    queries = [("alpha", "", lambda text: "alpha" in text),
               ("gamma", "", lambda text: "gamma" in text),
               ("alpha gamma", "", lambda text: bool(text)),
               ("absentword", "", lambda text: False),
               ("+alpha -beta", " IN BOOLEAN MODE", lambda text: False),
               ("+alpha -delta", " IN BOOLEAN MODE", lambda text: "alpha" in text)]
    statements = ["USE fts_filter_batch_regression;"]
    expected = []
    count = 0
    for query, mode, matches in queries:
        score = "MATCH(body) AGAINST('%s'%s)" % (query, mode)
        for suffix, selected in [("", lambda index: True),
                                 (" AND id%5=0", lambda index: index % 5 == 0),
                                 (" <>0", lambda index: True)]:
            ids = [index for index, text in rows.items()
                   if text is not None and matches(text) and selected(index)]
            for size, offset in [(1, 0), (255, 0), (256, 0), (257, 0),
                                 (769, 0), (257, 255), (257, 256), (257, 511), (20, 1024)]:
                marker = "case_%d" % count
                statements.append("SELECT '%s';" % marker)
                statements.append(
                    "SELECT id,(%s<>0),IF(%s,1,0) FROM docs WHERE %s%s "
                    "ORDER BY id LIMIT %d OFFSET %d;"
                    % (score, score, score, suffix, size, offset)
                )
                expected.append(marker)
                expected.extend("%d\t1\t1" % index for index in ids[offset:offset + size])
                count += 1
    # Exercise multi-batch execution and transitions between populated and
    # empty results in one SQL session.
    actual = sql("\n".join(statements))
    if actual != expected:
        mismatch = next((i for i, pair in enumerate(zip(actual, expected))
                         if pair[0] != pair[1]), min(len(actual), len(expected)))
        raise AssertionError("MATCH boolean results differ at %d: actual=%r expected=%r"
                             % (mismatch, actual[mismatch:mismatch + 3],
                                expected[mismatch:mismatch + 3]))
    sql("DROP DATABASE fts_filter_batch_regression;")
    return {"rows": len(rows), "queries": count, "checked_output_rows": len(expected) - count}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--client", required=True)
    parser.add_argument("--port", required=True, type=int)
    args = parser.parse_args()
    print(json.dumps(verify(args.client, args.port)))
