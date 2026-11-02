"""Check concurrent IK SQL results against serial results on a dedicated instance."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import subprocess


def verify(client, port, workers=8, rounds=32):
    statements = []
    for text in ["南京市长江大桥 数据库全文索引", "三百二十公斤 十二个小时",
                 "OceanBase café ＡＢＣ１２３ 😀", "数据库连接超时 网络重试。" * 80]:
        literal = "CONVERT(0x%s USING utf8mb4)" % text.encode("utf-8").hex()
        for mode in ["smart", "max_word"]:
            options = json.dumps([{"output": "all"}, {"additional_args": [{"ik_mode": mode}]}])
            statements.append("SELECT tokenize(%s,'ik','%s');" % (literal, options))

    def execute(sql):
        result = subprocess.run(
            [client, "-h127.0.0.1", "-P" + str(port), "-uroot", "-N", "-B", "--raw",
             "--default-character-set=utf8mb4"],
            input="SET NAMES utf8mb4; SET ob_query_timeout=120000000;\n" + sql,
            text=True, capture_output=True, timeout=150,
        )
        if result.returncode:
            raise RuntimeError(result.stderr)
        return [json.loads(line) for line in result.stdout.splitlines()]

    serial = execute("\n".join(statements))
    expected = serial * rounds

    def check_worker(_):
        actual = execute("\n".join(statements * rounds))
        if actual != expected:
            raise AssertionError("Concurrent TOKENIZE results differ from serial results")
        return len(actual)

    with ThreadPoolExecutor(max_workers=workers) as pool:
        counts = list(pool.map(check_worker, range(workers)))
    return {"clients": workers, "statements": sum(counts), "serial_cases": len(serial)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--client", required=True)
    parser.add_argument("--port", required=True, type=int)
    args = parser.parse_args()
    print(json.dumps(verify(args.client, args.port)))
