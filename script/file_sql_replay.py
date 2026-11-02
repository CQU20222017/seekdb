#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Replay the ``file_sql`` mysqltest cases against a running seekdb instance.

``tools/competition/run_ai_function_tests.py`` cannot drive this suite: the cases
use ``--source`` include files, ``let $VAR`` substitution, ``--disable_query_log``
/ ``--disable_result_log`` and ``--error`` expectations.  This script understands
exactly those directives (mtr semantics), installs the fixtures that
``include/copy_file_sql_data.inc`` installs with ssh/scp, and compares the output
with the checked-in ``.result`` file line by line.

Usage (inside the WSL instance's host, instance on 2881):

    python3 script/file_sql_replay.py                       # file_sql_basic
    python3 script/file_sql_replay.py --case file_sql_basic --verbose
"""
import argparse
import base64
import os
import re
import shutil
import sys
import tempfile
from datetime import date, datetime, timedelta
from decimal import Decimal
from pathlib import Path

import pymysql

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent
TEST_SUITE_ROOT = REPO_ROOT / "tools" / "deploy" / "mysql_test" / "test_suite"
DEFAULT_CASES = ("file_sql_basic",)

VARIABLE_RE = re.compile(r"\$([A-Za-z_][A-Za-z0-9_]*)")


def format_cell(value) -> str:
    """Render one result cell the way mysql-test does."""
    if value is None:
        return "NULL"
    if isinstance(value, (bytes, bytearray)):
        return bytes(value).decode("utf-8", "surrogateescape")
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, float):
        # the server prints a DOUBLE with the shortest representation, so an
        # integral value has no ".0" suffix (`87`, not `87.0`)
        text = repr(value)
        return text[:-2] if text.endswith(".0") else text
    if isinstance(value, Decimal):
        return format(value, "f")
    if isinstance(value, datetime):
        return value.strftime("%Y-%m-%d %H:%M:%S")
    if isinstance(value, date):
        return value.strftime("%Y-%m-%d")
    if isinstance(value, timedelta):
        total = int(value.total_seconds())
        sign = "-" if total < 0 else ""
        total = abs(total)
        return "%s%02d:%02d:%02d" % (sign, total // 3600, (total % 3600) // 60, total % 60)
    return str(value)


class Statement(object):
    def __init__(self, sql, line, is_eval=False, expected_errors=None):
        self.sql = sql
        self.line = line
        self.is_eval = is_eval
        self.expected_errors = list(expected_errors or [])


class Directive(object):
    def __init__(self, kind, payload, line):
        self.kind = kind
        self.payload = payload
        self.line = line


def split_statements(text):
    """Yield ``Statement`` nodes, honouring quotes/backticks and mtr directives."""
    nodes = []
    query_log = True
    result_log = True
    expected_errors = []
    buf = []
    buf_line = 0
    is_eval = False
    line_no = 0
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        line_no = i + 1
        raw = lines[i]
        i += 1
        stripped = raw.strip()
        if not buf:
            if not stripped or stripped.startswith("#"):
                continue
            if stripped.startswith("--"):
                name, _, argument = stripped[2:].partition(" ")
                name = name.strip().lower()
                argument = argument.strip()
                if name == "echo":
                    nodes.append(Directive("echo", argument, line_no))
                elif name == "source":
                    nodes.append(Directive("source", argument, line_no))
                elif name == "disable_query_log":
                    query_log = False
                    nodes.append(Directive("query_log", False, line_no))
                elif name == "enable_query_log":
                    query_log = True
                    nodes.append(Directive("query_log", True, line_no))
                elif name == "disable_result_log":
                    result_log = False
                    nodes.append(Directive("result_log", False, line_no))
                elif name == "enable_result_log":
                    result_log = True
                    nodes.append(Directive("result_log", True, line_no))
                elif name == "disable_warnings":
                    nodes.append(Directive("warnings", False, line_no))
                elif name == "enable_warnings":
                    nodes.append(Directive("warnings", True, line_no))
                elif name == "error":
                    codes = []
                    for token in re.split(r"[,\s]+", argument):
                        if token.isdigit():
                            codes.append(int(token))
                    expected_errors = codes
                elif name in ("sleep", "real_sleep"):
                    nodes.append(Directive("sleep", argument, line_no))
                elif name in ("tags", "owner"):
                    continue
                else:
                    nodes.append(Directive("skip", "%s %s" % (name, argument), line_no))
                continue
            if stripped.startswith("let "):
                nodes.append(Directive("let", stripped[4:].strip(), line_no))
                continue
        # accumulate one statement up to a top level ';'
        idx = 0
        quote = None
        if not buf:
            buf_line = line_no
            if stripped.startswith("eval "):
                is_eval = True
                stripped = stripped[5:].strip()
            else:
                is_eval = False
            raw = stripped
        while idx < len(raw):
            char = raw[idx]
            if quote:
                if char == "\\":
                    buf.append(raw[idx:idx + 2])
                    idx += 2
                    continue
                if char == quote:
                    quote = None
            elif char in ("'", '"', "`"):
                quote = char
            elif char == ";":
                nodes.append(Statement("".join(buf).strip(), buf_line, is_eval,
                                       expected_errors))
                buf = []
                expected_errors = []
                idx += 1
                rest = raw[idx:].strip()
                # anything after ';' on the same line is ignored (comments)
                raw = rest
                idx = 0
                continue
            buf.append(char)
            idx += 1
        if buf and "".join(buf).strip():
            buf.append(" ")
    if buf and "".join(buf).strip():
        nodes.append(Statement("".join(buf).strip(), buf_line, is_eval, expected_errors))
    return nodes


def install_fixtures(test_dir, base_dir):
    """What include/copy_file_sql_data.inc does through ssh/scp, done locally.

    The case installs the files into ``$OBSERVER_DIR/data_for_mysqltest_file_sql``,
    so the observer-writable base directory is created here and the fixtures are
    put into that sub directory.
    """
    source = Path(test_dir) / "data"
    data_dir = Path(base_dir) / "data_for_mysqltest_file_sql"
    if data_dir.exists():
        shutil.rmtree(data_dir, ignore_errors=True)
    data_dir.mkdir(parents=True, exist_ok=True)
    for name in ("basic.csv", "basic.jsonl", "invalid.txt"):
        shutil.copyfile(source / name, data_dir / name)
    encoded = (source / "simple.parquet.b64").read_text(encoding="utf-8").strip()
    (data_dir / "simple.parquet").write_bytes(base64.b64decode(encoded))
    return data_dir


def remove_fixtures(data_dir):
    shutil.rmtree(Path(data_dir), ignore_errors=True)


def substitute(text, variables):
    def replace(match):
        name = match.group(1)
        return variables.get(name, match.group(0))
    return VARIABLE_RE.sub(replace, text)


class Replay(object):
    def __init__(self, options, case):
        self.options = options
        self.case = case
        self.lines = []
        self.failures = []
        self.errors = 0

    def emit(self, line):
        self.lines.append(line)

    def run_sql(self, statement, variables, result_log):
        sql = substitute(statement.sql, variables) if statement.is_eval else statement.sql
        if self.options.verbose:
            print("  [%4d] %s" % (statement.line, sql))
        cursor = self.connection.cursor()
        try:
            cursor.execute(sql)
            description = cursor.description
            rows = cursor.fetchall() if description is not None else []
            while description is not None and cursor.nextset():
                description = cursor.description
                rows = cursor.fetchall() if description is not None else []
        except pymysql.MySQLError as exc:
            errno = exc.args[0] if exc.args else None
            message = exc.args[1] if len(exc.args) > 1 else str(exc)
            if statement.expected_errors and errno in statement.expected_errors:
                if self.options.verbose:
                    print("      expected error %s: %s" % (errno, message))
            else:
                self.errors += 1
                self.emit("ERROR %s: %s" % (errno, message))
                self.failures.append("line %d: unexpected error %s: %s"
                                     % (statement.line, errno, message))
            return
        finally:
            cursor.close()
        if statement.expected_errors:
            self.errors += 1
            self.failures.append("line %d: expected error %s but the statement succeeded"
                                 % (statement.line, statement.expected_errors))
            return
        if not result_log:
            return
        if description is not None:
            self.emit("\t".join(str(column[0]) for column in description))
            for row in rows:
                self.emit("\t".join(format_cell(cell) for cell in row))

    def run(self, nodes):
        self.fixtures_dir = Path(self.options.data_dir) / "data_for_mysqltest_file_sql"
        install_fixtures(self.case_dir, self.options.data_dir)
        self.variables = {"FILE_SQL_DATA_DIR": str(self.fixtures_dir),
                          "OBSERVER_DIR": str(self.options.data_dir)}
        variables = self.variables
        result_log = True
        self.connection = pymysql.connect(host=self.options.host, port=self.options.port,
                                          user=self.options.user,
                                          password=self.options.password,
                                          database=self.options.database,
                                          autocommit=True, charset="utf8mb4")
        try:
            cursor = self.connection.cursor()
            cursor.execute("select @@session.enable_file_sql")
            cursor.close()
            for node in nodes:
                if isinstance(node, Directive):
                    if node.kind == "echo":
                        self.emit(node.payload)
                    elif node.kind == "result_log":
                        result_log = node.payload
                    elif node.kind == "let":
                        name, _, value = node.payload.partition("=")
                        value = value.strip()
                        if value.endswith(";"):
                            value = value[:-1].strip()
                        variables[name.strip().lstrip("$")] = substitute(value, variables)
                    elif node.kind == "source":
                        if node.payload.endswith("copy_file_sql_data.inc"):
                            install_fixtures(self.case_dir, self.options.data_dir)
                        elif node.payload.endswith("remove_file_sql_data.inc"):
                            remove_fixtures(self.fixtures_dir)
                        else:
                            self.failures.append("line %d: unsupported --source %s"
                                                 % (node.line, node.payload))
                    elif node.kind == "sleep":
                        pass
                    elif node.kind == "skip":
                        if self.options.verbose:
                            print("  [%4d] skipped directive %s" % (node.line, node.payload))
                else:
                    self.run_sql(node, variables, result_log)
        finally:
            self.connection.close()
            if not self.options.keep_data:
                remove_fixtures(self.fixtures_dir)
        return self.lines


def compare(expected, actual):
    failures = []
    for index, (want, got) in enumerate(zip(expected, actual)):
        if want.rstrip() != got.rstrip():
            failures.append("line %d:\n    expected %r\n    actual   %r"
                            % (index + 1, want, got))
            if len(failures) >= 5:
                break
    if len(expected) != len(actual):
        failures.append("line count: expected %d, got %d" % (len(expected), len(actual)))
    return failures


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--user", default="root")
    parser.add_argument("--password", default="")
    parser.add_argument("--database", default="test")
    parser.add_argument("--case", action="append", default=None)
    parser.add_argument("--test-suite-root", default=str(TEST_SUITE_ROOT))
    parser.add_argument("--data-dir", default=os.path.join(tempfile.gettempdir(),
                                                            "seekdb_file_sql_data"))
    parser.add_argument("--keep-data", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    options = parser.parse_args(argv)

    cases = options.case or list(DEFAULT_CASES)
    root = Path(options.test_suite_root)
    failures = 0
    for name in cases:
        test_path = root / "file_sql" / "t" / (name + ".test")
        result_path = root / "file_sql" / "r" / "mysql" / (name + ".result")
        if not test_path.is_file() or not result_path.is_file():
            print("FAIL %s: missing %s or %s" % (name, test_path, result_path))
            failures += 1
            continue
        replay = Replay(options, name)
        replay.case_dir = test_path.parent.parent
        nodes = split_statements(test_path.read_text(encoding="utf-8"))
        actual = replay.run(nodes)
        expected = result_path.read_text(encoding="utf-8").replace("\r\n", "\n").rstrip("\n").split("\n")
        diffs = compare(expected, actual)
        diffs.extend(replay.failures)
        if diffs:
            failures += 1
            print("FAIL %s (%d statement(s) replayed, %d unexpected error(s))"
                  % (name, sum(1 for node in nodes if isinstance(node, Statement)),
                     replay.errors))
            for diff in diffs[:8]:
                print("  " + diff)
        else:
            print("PASS %s (%d line(s) identical, %d statement(s))"
                  % (name, len(expected),
                     sum(1 for node in nodes if isinstance(node, Statement))))
    if failures:
        print("FAILED (%d failure(s))" % failures)
        return 1
    print("PASS (0 failure(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())