#!/usr/bin/env python3
"""M2 probe: AI column task rows are written by DAS inside the DML transaction.

Stages (each prints PASS/FAIL):
  insert    - the task row is visible inside the transaction, survives the commit, and a
              rolled back insert leaves no task row behind
  noop upd  - an UPDATE of a column the AI expression does not depend on creates no work item
  dep upd   - an UPDATE of a dependency column invalidates the result and re-issues the task
  pk upd    - a primary key change moves the task row to the new row key
  delete    - DELETE drops the pending task rows of the deleted rows

Usage (inside WSL, instance on 2881, mock on 8000):
    python3 script/wsl_m2_probe.py [--port 2881] [--mock-port 8000]
"""
import argparse
import sys

import pymysql

DB = "ai_m2_probe"
TABLE = "docs"
ENDPOINT = "m2_probe_ep"
MODEL = "m2_probe_complete"

FAILURES = []

TASK_SQL = """
select task.rowkey_predicate, task.eligible_ts, task.state, task.request_id,
       task.retry_count, task.table_id, task.column_id, task.eligible_ts <= now(6)
from oceanbase.__all_ai_column_task task
join oceanbase.__all_table t on t.table_id = task.table_id
join oceanbase.__all_database d on d.database_id = t.database_id
where d.database_name = '{db}' and t.table_name = '{table}'
order by task.rowkey_predicate
""".format(db=DB, table=TABLE)


def check(name, ok, detail=""):
    print("%-30s %s%s" % (name, "PASS" if ok else "FAIL",
                          ("  " + detail) if detail else ""))
    if not ok:
        FAILURES.append(name)


def connect(port, autocommit=True):
    return pymysql.connect(host="127.0.0.1", port=port, user="root", password="",
                           database="test", autocommit=autocommit,
                           charset="utf8mb4")


def tasks(cur):
    cur.execute(TASK_SQL)
    return cur.fetchall()


def task_sig(cur, predicate):
    for row in tasks(cur):
        if row[0] == predicate:
            return row
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--mock-port", type=int, default=8000)
    args = parser.parse_args()

    setup = connect(args.port)
    cur = setup.cursor()

    def run(sql, soft=False):
        try:
            cur.execute(sql)
        except Exception as exc:  # noqa: BLE001
            if not soft:
                raise
            print("[setup] ignored: %s -> %s" % (sql.split("(")[0].strip(), exc))
            return ()
        try:
            return cur.fetchall()
        except Exception:  # noqa: BLE001 - statements without a result set
            return ()

    run("drop database if exists %s" % DB, soft=True)
    run("create database %s" % DB)
    run("call DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('%s')" % ENDPOINT, soft=True)
    run("call DBMS_AI_SERVICE.DROP_AI_MODEL('%s')" % MODEL, soft=True)
    run("call DBMS_AI_SERVICE.CREATE_AI_MODEL('%s', "
        "'{\"type\":\"completion\",\"model_name\":\"mock-complete\"}')" % MODEL)
    run("call DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('%s', "
        # the probe asserts on *pending* work items, so it points the endpoint at a closed port:
        # the worker then keeps retrying instead of publishing and dropping the rows
        "'{\"ai_model_name\":\"%s\",\"url\":\"http://127.0.0.1:1/v1/chat/completions\","
        "\"access_key\":\"m2-key\",\"provider\":\"openai\","
        "\"request_model_name\":\"mock-complete\"}')" % (ENDPOINT, MODEL))
    run("create table %s.%s(id bigint primary key, title varchar(100), "
        "content varchar(200), tags json AI COLUMN (AI_COMPLETE('%s', "
        "concat('Classify: ', content))) WITH (sync_mode='async', max_retries=100))"
        % (DB, TABLE, ENDPOINT))
    print("[setup] table %s.%s created" % (DB, TABLE))

    # ================= insert ================================================
    tx = connect(args.port, autocommit=False)
    tx_cur = tx.cursor()
    tx_cur.execute("insert into %s.%s(id, title, content) values(1, 'title-1', 'initial')"
                   % (DB, TABLE))
    inside = tasks(tx_cur)
    check("insert: txn_visible", len(inside) == 1, "count=%d" % len(inside))
    row = inside[0] if inside else None
    if row:
        check("insert: rowkey_predicate", row[0] == "`id` = 1", "%r" % (row[0],))
        check("insert: eligible_ts <= now(6)", row[7] == 1, "flag=%s" % (row[7],))
        check("insert: status pending", row[2] == 0, "status=%s" % (row[2],))
        check("insert: retry_count 0", row[4] == 0)
        check("insert: request_id set", bool(row[3]), "%r" % (row[3],))
    tx.commit()
    tx.close()

    view = connect(args.port)
    view_cur = view.cursor()
    check("insert: after_commit", len(tasks(view_cur)) == 1)

    rb = connect(args.port, autocommit=False)
    rb_cur = rb.cursor()
    rb_cur.execute("insert into %s.%s(id, title, content) values(9, 't9', 'rollback-value')"
                   % (DB, TABLE))
    check("insert: txn_sees_2nd_row", len(tasks(rb_cur)) == 2)
    rb.rollback()
    rb.close()
    check("insert: rollback_cleaned", len(tasks(view_cur)) == 1)
    view_cur.execute("select count(*) from %s.%s where id = 9" % (DB, TABLE))
    check("insert: rollback_row_gone", int(view_cur.fetchone()[0] or 0) == 0)

    # ================= UPDATE of a column the AI expression ignores ==========
    before = task_sig(view_cur, "`id` = 1")
    view_cur.execute("update %s.%s set title = 'title-2' where id = 1" % (DB, TABLE))
    after_noop = task_sig(view_cur, "`id` = 1")
    check("noop update: no new work item", before == after_noop,
          "%r vs %r" % (before[3] if before else None, after_noop[3] if after_noop else None))
    check("noop update: single task row", len(tasks(view_cur)) == 1)

    # ================= UPDATE of the dependency column =======================
    before = task_sig(view_cur, "`id` = 1")
    ux = connect(args.port, autocommit=False)
    ux_cur = ux.cursor()
    ux_cur.execute("update %s.%s set content = 'dependency-update' where id = 1"
                   % (DB, TABLE))
    ux_cur.execute("select tags is null from %s.%s where id = 1" % (DB, TABLE))
    check("dep update: result is null in txn", int(ux_cur.fetchone()[0]) == 1)
    in_tx = tasks(ux_cur)
    check("dep update: one work item only", len(in_tx) == 1, "count=%d" % len(in_tx))
    after_dep = in_tx[0] if in_tx else None
    check("dep update: request id refreshed",
          after_dep is not None and before is not None and after_dep[3] != before[3],
          "%r vs %r" % (before[3] if before else None,
                        after_dep[3] if after_dep else None))
    ux.commit()
    ux.close()
    check("dep update: after_commit", len(tasks(view_cur)) == 1)

    # ================= primary key change ===================================
    px = connect(args.port, autocommit=False)
    px_cur = px.cursor()
    px_cur.execute("update %s.%s set id = 2 where id = 1" % (DB, TABLE))
    pk_rows = tasks(px_cur)
    px.commit()
    px.close()
    check("pk update: one task row", len(pk_rows) == 1, "count=%d" % len(pk_rows))
    check("pk update: new predicate",
          len(pk_rows) == 1 and pk_rows[0][0] == "`id` = 2",
          "%r" % ([r[0] for r in pk_rows],))

    # ================= delete ==============================================
    view_cur.execute("insert into %s.%s(id, title, content) values(10, 't10', 'c10'), "
                     "(11, 't11', 'c11')" % (DB, TABLE))
    check("batch insert: two more tasks", len(tasks(view_cur)) == 3)
    view_cur.execute("delete from %s.%s where id in (10, 11)" % (DB, TABLE))
    check("delete: pending tasks dropped", len(tasks(view_cur)) == 1,
          "count=%d" % len(tasks(view_cur)))
    view_cur.execute("delete from %s.%s where id = 2" % (DB, TABLE))
    check("delete: all tasks dropped", len(tasks(view_cur)) == 0)

    view_cur.execute("select count(*) from %s.%s" % (DB, TABLE))
    check("delete: rows gone", int(view_cur.fetchone()[0] or 0) == 0)

    # ===== two AI columns of one row share one request id ==================
    # (ai_column_worker asserts count(distinct request_id) = 1 for a row with two AI columns)
    run("create table %s.%s(id bigint primary key, content varchar(200), "
        "tags json AI COLUMN (AI_COMPLETE('%s', concat('Tags: ', content))) "
        "WITH (sync_mode='async', max_retries=100), "
        "score bigint AI COLUMN (AI_COMPLETE('%s', concat('Score: ', content))) "
        "WITH (sync_mode='async', max_retries=100))" % (DB, "docs2", ENDPOINT, ENDPOINT))
    view_cur.execute("insert into %s.%s(id, content) values(1, 'typed-prompts')"
                     % (DB, "docs2"))
    view_cur.execute("select count(*), count(distinct task.request_id), min(task.state) "
                     "from oceanbase.__all_ai_column_task task "
                     "join oceanbase.__all_table t on t.table_id = task.table_id "
                     "join oceanbase.__all_database d on d.database_id = t.database_id "
                     "where d.database_name = '%s' and t.table_name = 'docs2'" % DB)
    group = view_cur.fetchone()
    check("grouped: two work items", int(group[0] or 0) == 2, "count=%s" % (group[0],))
    check("grouped: one request id", int(group[1] or 0) == 1,
          "distinct_request_id=%s" % (group[1],))
    check("grouped: state pending", group[2] == 0, "state=%s" % (group[2],))
    view_cur.execute("delete from %s.%s where id = 1" % (DB, "docs2"))

    view.close()
    setup.close()

    print()
    if FAILURES:
        print("PROBE FAILED: %s" % ", ".join(FAILURES))
        return 1
    print("PROBE PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())