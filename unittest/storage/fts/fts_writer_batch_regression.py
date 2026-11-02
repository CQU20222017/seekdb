"""Check FTS writer routing, NULLs, empty input and index rebuilds.

Run against a dedicated instance; creates/drops only fts_writer_batch_regression.
The composite key repeats document numbers across seven partitions.
"""
import argparse
import json
import subprocess


def verify(client, port):
    database = "fts_writer_batch_regression"

    def sql(statement):
        result = subprocess.run(
            [client, "-h127.0.0.1", "-P" + str(port), "-uroot", "-N", "-B", "--raw",
             "--default-character-set=utf8mb4"],
            input="SET NAMES utf8mb4; SET ob_query_timeout=120000000;\n" + statement,
            text=True, encoding="utf-8", capture_output=True, timeout=180,
        )
        if result.returncode:
            raise RuntimeError(result.stderr + statement[:500])
        return result.stdout.splitlines()

    def literal(text):
        return "NULL" if text is None else "CONVERT(0x%s USING utf8mb4)" % text.encode().hex()

    sql(f"DROP DATABASE IF EXISTS {database}; CREATE DATABASE {database};")
    documents = {}
    words = []
    snapshots = {}
    try:
        sql(f"CREATE TABLE {database}.docs(tenant_id INT,doc_id INT,title TEXT,body LONGTEXT,"
            "PRIMARY KEY(tenant_id,doc_id)) CHARSET=utf8mb4 "
            "PARTITION BY HASH(tenant_id) PARTITIONS 7;")
        values = []
        for i in range(257):
            word = "probe" + chr(97 + i // 676) + chr(97 + i // 26 % 26) + chr(97 + i % 26) + "q"
            words.append(word)
            title = None if i % 19 == 0 else word + " titlewordq"
            body = None if i % 17 == 0 else "数据库 银行 commonwordq " + word + " " + word
            key = (i % 7, i // 7)
            documents[key] = (title, body)
            values.append(f"({key[0]},{key[1]},{literal(title)},{literal(body)})")
        sql(f"INSERT INTO {database}.docs VALUES " + ",".join(values) + ";")

        def build():
            sql(f"ALTER TABLE {database}.docs ADD FULLTEXT INDEX ft_all(title,body) WITH PARSER beng;")
            sql(f"ALTER TABLE {database}.docs ADD FULLTEXT INDEX ft_body(body) WITH PARSER ik;")

        def capture(stage):
            for name, fields in [("all", "title,body"), ("body", "body")]:
                statements = []
                terms = words + ["commonwordq", "changedwordq", "missingwordq"]
                for term in terms:
                    score = f"MATCH({fields}) AGAINST('{term}')"
                    statements.append(f"SELECT '{term}',tenant_id,doc_id,{score} "
                                      f"FROM {database}.docs WHERE {score} ORDER BY tenant_id,doc_id;")
                actual = sql("\n".join(statements))
                expected = []
                for term in terms:
                    for key, (title, body) in sorted(documents.items()):
                        text = " ".join(part for part in ([title, body] if name == "all" else [body])
                                        if part is not None)
                        if term in text.split():
                            expected.append((term, str(key[0]), str(key[1])))
                identities = [tuple(row.split("\t")[:3]) for row in actual]
                assert identities == expected, (stage, name, len(identities), len(expected))
                snapshots[f"{stage}_{name}"] = actual

        build()
        capture("first")
        for i in [17, 42, 43, 44, 245]:
            key = (i % 7, i // 7)
            documents[key] = (documents[key][0], "changedwordq")
            sql(f"UPDATE {database}.docs SET body='changedwordq' "
                f"WHERE tenant_id={key[0]} AND doc_id={key[1]};")
        capture("updated")
        sql(f"ALTER TABLE {database}.docs DROP INDEX ft_all;"
            f"ALTER TABLE {database}.docs DROP INDEX ft_body;")
        build()
        capture("rebuilt")
        for kind in ["all", "body"]:
            assert snapshots[f"updated_{kind}"] == snapshots[f"rebuilt_{kind}"]

        sql(f"CREATE TABLE {database}.empty_docs(id INT PRIMARY KEY,body TEXT);"
            f"INSERT INTO {database}.empty_docs VALUES(1,NULL),(2,''),(3,NULL);"
            f"ALTER TABLE {database}.empty_docs ADD FULLTEXT INDEX ft(body) WITH PARSER beng;")
        assert sql(f"SELECT id FROM {database}.empty_docs WHERE MATCH(body) AGAINST('commonwordq');") == []
        snapshots["empty"] = True
    finally:
        sql(f"DROP DATABASE {database};")
    return snapshots


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--client", required=True)
    parser.add_argument("--port", required=True, type=int)
    args = parser.parse_args()
    print(json.dumps(verify(args.client, args.port), ensure_ascii=False, sort_keys=True))
