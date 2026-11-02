"""Validate cardinality-only limited FTS views on a dedicated SQL instance."""
import argparse
import json
import subprocess


def verify(client, port, require_optimization=False):
    db = 'fts_count_limit_regression'

    def sql(text):
        r = subprocess.run([client, '-h127.0.0.1', '-P'+str(port), '-uroot', '-N', '-B',
                            '--raw', '--default-character-set=utf8mb4'],
                           input='SET ob_query_timeout=120000000;\n'+text,
                           text=True, capture_output=True, timeout=150)
        if r.returncode:
            raise RuntimeError(r.stderr+'\n'+text)
        return r.stdout.strip()

    sql(f'DROP DATABASE IF EXISTS {db}; CREATE DATABASE {db}; '
        f'CREATE TABLE {db}.docs(id INT PRIMARY KEY, optional_id INT, body TEXT);')
    matching = []
    for start in range(1, 514, 100):
        values = []
        for i in range(start, min(start+100, 514)):
            body = None if i % 17 == 0 else '' if i % 19 == 0 else (
                'alpha beta beta' if i % 3 == 0 else 'gamma delta')
            if body and ('alpha' in body or 'gamma' in body):
                matching.append(i)
            encoded = 'NULL' if body is None else "'"+body+"'"
            optional = 'NULL' if i % 5 == 0 else str(i)
            values.append(f'({i},{optional},{encoded})')
        sql(f'INSERT INTO {db}.docs VALUES '+','.join(values)+';')
    sql(f'ALTER TABLE {db}.docs ADD FULLTEXT INDEX ft(body) WITH PARSER beng;')
    match = "MATCH(body) AGAINST('alpha gamma')"
    results = {}
    try:
        for projection in ['id', 'id,optional_id', 'optional_id']:
            for limit in [0, 1, 2, 20, 256, 512, 1000]:
                for offset in [0, 5, 256, 2000]:
                    query = (f'SELECT COUNT(*) FROM (SELECT {projection} FROM {db}.docs '
                             f'WHERE {match} LIMIT {limit} OFFSET {offset}) t')
                    expected = min(limit, max(len(matching)-offset, 0))
                    assert sql(query) == str(expected), (query, expected)
                    results[f'{projection}:{limit}:{offset}'] = expected
        positive = (f'SELECT COUNT(*) FROM (SELECT id FROM {db}.docs '
                    f'WHERE {match} LIMIT 20) t')
        plan = sql('EXPLAIN '+positive)
        if require_optimization:
            assert 'calc_relevance=false' in plan, plan
            assert 'sort_keys(' not in plan, plan
        # These callers observe values, an explicit order, a predicate or the
        # numeric relevance. They must retain the original scoring path.
        guards = {
            'sum': f'SELECT SUM(id) FROM (SELECT id FROM {db}.docs WHERE {match} LIMIT 20) t',
            'nullable_count': f'SELECT COUNT(optional_id) FROM (SELECT optional_id FROM {db}.docs WHERE {match} LIMIT 20) t',
            'outer_filter': f'SELECT COUNT(*) FROM (SELECT id FROM {db}.docs WHERE {match} LIMIT 20) t WHERE id>100',
            'outer_group': f'SELECT id,COUNT(*) FROM (SELECT id FROM {db}.docs WHERE {match} LIMIT 20) t GROUP BY id',
            'numeric_filter': f'SELECT COUNT(*) FROM (SELECT id FROM {db}.docs WHERE {match}>0.1 LIMIT 20) t',
            'score_output': f'SELECT COUNT(*) FROM (SELECT {match} AS score FROM {db}.docs WHERE {match} LIMIT 20) t',
            'explicit_score': f'SELECT COUNT(*) FROM (SELECT id FROM {db}.docs WHERE {match} ORDER BY {match} DESC LIMIT 20) t',
            'returned_ids': f'SELECT id FROM {db}.docs WHERE {match} LIMIT 20',
            'no_rewrite': f'SELECT COUNT(*) FROM (SELECT /*+ NO_REWRITE */ id FROM {db}.docs WHERE {match} LIMIT 20) t',
        }
        for name, query in guards.items():
            guarded_plan = sql('EXPLAIN '+query)
            # Numeric score predicates may use the existing functional lookup
            # rather than a TEXT RETRIEVAL SCAN; both must keep real scoring.
            assert ('calc_relevance=true' in guarded_plan
                    or 'has_functional_lookup=true' in guarded_plan), (name, guarded_plan)
            assert 'calc_relevance=false' not in guarded_plan, (name, guarded_plan)
            sql(query)
        for query, expected in [
            (f'SELECT COUNT(*) FROM (SELECT id FROM {db}.docs WHERE {match} ORDER BY id DESC LIMIT 20) t', '20'),
            (f"SELECT COUNT(*) FROM (SELECT id FROM {db}.docs WHERE MATCH(body) AGAINST('absentword') LIMIT 20) t", '0'),
        ]:
            assert sql(query) == expected, query
        prepared_query = (f'SELECT COUNT(*) FROM (SELECT id FROM {db}.docs '
                          'WHERE MATCH(body) AGAINST(?) LIMIT ? OFFSET ?) t')
        commands = [f"PREPARE count_ps FROM '{prepared_query}';"]
        prepared_expected = []
        for text, limit, offset in [('alpha gamma', 20, 0), ('alpha gamma', 5, 3),
                                    ('alpha gamma', 1000, 256), ('alpha gamma', 0, 0),
                                    ('absentword', 20, 0)]:
            commands += [f"SET @count_q='{text}',@count_n={limit},@count_o={offset};",
                         'EXECUTE count_ps USING @count_q,@count_n,@count_o;']
            available = len(matching) if text == 'alpha gamma' else 0
            prepared_expected.append(str(min(limit, max(available-offset, 0))))
        commands.append('DEALLOCATE PREPARE count_ps;')
        assert sql('\n'.join(commands)).splitlines() == prepared_expected
        # The ordered version retains the original scored path and supplies a
        # native reference for query inputs with no tokens, NULL and Unicode.
        edge_queries = 0
        for literal in ['NULL', "''", "'the and'", "'☃'", "'alpha beta'"]:
            predicate = f'MATCH(body) AGAINST({literal})'
            for limit, offset in [(0, 0), (1, 0), (20, 5)]:
                child = f'SELECT id FROM {db}.docs WHERE {predicate}'
                fast = f'SELECT COUNT(*) FROM ({child} LIMIT {limit} OFFSET {offset}) t'
                scored = (f'SELECT COUNT(*) FROM ({child} ORDER BY {predicate} DESC '
                          f'LIMIT {limit} OFFSET {offset}) t')
                assert sql(fast) == sql(scored), (fast, scored)
                edge_queries += 1
        return {'count_cases': len(results), 'scored_guards': len(guards), 'membership_plan': plan,
                'matching_rows': len(matching), 'prepared_cases': len(prepared_expected),
                'edge_queries': edge_queries}
    finally:
        sql(f'DROP DATABASE {db};')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', required=True)
    parser.add_argument('--port', type=int, default=2895)
    parser.add_argument('--require-optimization', action='store_true')
    args = parser.parse_args()
    print(json.dumps(verify(args.client, args.port, args.require_optimization), ensure_ascii=False))
