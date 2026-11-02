"""Check document boundaries and all posting values after offline FTS builds.

Use a dedicated instance: creates/drops only fts_build_batch_regression.
Large documents exceed multiple output batches and the LOB in-row threshold.
"""
import argparse
import hashlib
import json
import subprocess


def verify(client, port):
    def sql(statement):
        run = subprocess.run(
            [client, "-h127.0.0.1", "-P" + str(port), "-uroot", "-N", "-B", "--raw",
             "--default-character-set=utf8mb4"],
            input="SET NAMES utf8mb4; SET ob_query_timeout=120000000; "
                  "SET ob_enable_index_direct_select=ON;\n" + statement,
            text=True, encoding="utf-8", capture_output=True, timeout=180,
        )
        if run.returncode:
            raise RuntimeError(run.stderr + statement[:500])
        return run.stdout.splitlines()

    database = "fts_build_batch_regression"
    sql(f"DROP DATABASE IF EXISTS {database}; CREATE DATABASE {database};")
    words = ["lex" + chr(97 + i // 676) + chr(97 + i // 26 % 26) + chr(97 + i % 26) + "q"
             for i in range(2053)]
    documents = {1: " ".join(words), 2: None, 3: "",
                 4: " ".join(words * 9), 5: "northwordq southwordq eastwordq"}
    results = {}
    try:
        for table, schema, parser in [
            ("primary_docs", "seq INT PRIMARY KEY, label VARCHAR(80), body LONGTEXT", "beng"),
            ("heap_docs", "seq INT, label VARCHAR(80), body LONGTEXT", "beng"),
            ("partition_docs", "seq INT PRIMARY KEY, label VARCHAR(80), body LONGTEXT", "ik"),
        ]:
            partition = " PARTITION BY HASH(seq) PARTITIONS 2" if table == "partition_docs" else ""
            sql(f"CREATE TABLE {database}.{table}({schema}) CHARSET=utf8mb4{partition};")
            for seq, body in documents.items():
                if parser == "ik" and body:
                    body = "数据库 全文索引 银行 " + body
                if body is None:
                    value = "NULL"
                elif body:
                    value = "CONVERT(0x%s USING utf8mb4)" % body.encode().hex()
                else:
                    value = "''"
                sql(f"INSERT INTO {database}.{table} VALUES ({seq},'document-{seq}',{value});")

            def expected_postings():
                rows = sql(f"SELECT seq,TOKENIZE(CAST(body AS CHAR),'{parser}','[{{\"output\":\"all\"}}]') "
                           f"FROM {database}.{table} WHERE body IS NOT NULL ORDER BY seq;")
                grouped = {}
                assert rows, (table, "TOKENIZE returned no rows")
                for row in rows:
                    identity, encoded = row.split("\t", 1)
                    detail = json.loads(encoded)
                    if identity != "3":
                        assert detail["tokens"], (table, identity, encoded[:200])
                    for token in detail["tokens"]:
                        word, count = next(iter(token.items()))
                        grouped.setdefault(word, []).append((count, detail["doc_len"]))
                return sorted((word, str(sum(count for count, _ in values)),
                               str(min(length for _, length in values)),
                               str(max(length for _, length in values)), str(len(values)))
                              for word, values in grouped.items())

            def check_auxiliary(stage):
                # Exercise partition routing through public MATCH queries below,
                # covering every term rather than selecting hidden columns.
                if partition:
                    return
                expected = expected_postings()
                names = sql("SELECT d.table_id,a.table_name,a.index_type FROM oceanbase.__all_table a "
                            "JOIN oceanbase.__all_table d ON a.data_table_id=d.table_id "
                            "JOIN oceanbase.__all_database b ON b.database_id=d.database_id "
                            f"WHERE b.database_name='{database}' AND d.table_name='{table}' "
                            "AND a.index_type IN (13,14) ORDER BY a.index_type;")
                assert len(names) == 2, names
                for name in names:
                    table_id, auxiliary, kind = name.split("\t")
                    columns = sql("SELECT column_name FROM oceanbase.__all_column "
                                  f"WHERE table_id={int(table_id)} ORDER BY column_id;")
                    word, count, length = [next(column for column in columns if column.startswith(prefix))
                                           for prefix in ["__word_segment_", "__word_count_", "__doc_length_"]]
                    try:
                        rows = sql("SELECT /*+ OPT_PARAM('hidden_column_visible','true') */ "
                                   f"`{word}`,SUM(`{count}`),MIN(`{length}`),MAX(`{length}`),COUNT(*) "
                                   f"FROM `{database}`.`{auxiliary}` GROUP BY `{word}`;")
                    except RuntimeError as error:
                        raise RuntimeError((table, kind, stage, columns)) from error
                    actual = sorted(tuple(row.split("\t")) for row in rows)
                    assert actual == expected, (table, kind, stage, "posting values",
                                               len(actual), len(expected),
                                               list(zip(actual, expected))[:3])
                    results[f"{table}_{stage}_{kind}"] = hashlib.sha256(
                        json.dumps(actual, ensure_ascii=False).encode()).hexdigest()

            def check_search(stage):
                if partition:
                    statements = []
                    for term in words:
                        score = f"MATCH(body) AGAINST('{term}')"
                        statements.append(f"SELECT '{term}',seq,{score} FROM {database}.{table} "
                                          f"WHERE {score} ORDER BY seq;")
                    actual = sql("\n".join(statements))
                    expected_ids = ["1", "4"] if stage == "first" else ["4"]
                    assert [(row.split("\t")[0], row.split("\t")[1]) for row in actual] == [
                        (word, identity) for word in words for identity in expected_ids]
                    results[f"{table}_{stage}_all_terms"] = actual
                for term in [words[0], words[255], words[256], words[512], words[-1],
                             "northwordq", "absentwordq"]:
                    score = f"MATCH(body) AGAINST('{term}')"
                    actual = sql(f"SELECT seq,{score} FROM {database}.{table} WHERE {score} ORDER BY seq;")
                    results[f"{table}_{stage}_{term}"] = actual
                    identities = [row.split("\t")[0] for row in actual]
                    expected = (["1", "5"] if stage != "first" else ["5"]) if term == "northwordq" else (
                        [] if term == "absentwordq" else (["1", "4"] if stage == "first" else ["4"]))
                    assert identities == expected, (table, stage, term, identities)
                    assert sql(f"SELECT seq FROM {database}.{table} WHERE {score} ORDER BY seq;") == expected

            sql(f"ALTER TABLE {database}.{table} ADD FULLTEXT INDEX ft(body) WITH PARSER {parser};")
            check_auxiliary("first")
            check_search("first")
            sql(f"UPDATE {database}.{table} SET body='northwordq' WHERE seq=1;")
            check_auxiliary("updated")
            check_search("updated")
            sql(f"ALTER TABLE {database}.{table} DROP INDEX ft;"
                f"ALTER TABLE {database}.{table} ADD FULLTEXT INDEX ft(body) WITH PARSER {parser};")
            check_auxiliary("rebuilt")
            check_search("rebuilt")
    finally:
        sql(f"DROP DATABASE {database};")
    return results


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--client", required=True)
    parser.add_argument("--port", required=True, type=int)
    args = parser.parse_args()
    print(json.dumps(verify(args.client, args.port), ensure_ascii=False, sort_keys=True))
