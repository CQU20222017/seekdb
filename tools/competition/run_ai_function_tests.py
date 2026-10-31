#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""mysqltest-compatible runner for the seekdb competition suites.

The acceptance baselines are the ``.test`` / ``.result`` pairs under
``tools/deploy/mysql_test/test_suite/<suite>``::

    t/<case>.test           mysqltest script
    r/mysql/<case>.result   expected output

This script plays the role of ``mysqltest``: it interprets a ``.test`` file,
executes every statement over a pymysql connection, renders the output the same
way the reference runner does (echo the statement, then the result set, then
``ERROR <sqlstate>: <message>`` for failures) and diffs it against ``.result``.

Typical use::

    # ai_function (task 1)
    python tools/competition/ai_function_mock.py start 8000
    python tools/competition/run_ai_function_tests.py --port 2881 --database test

    # ai_column (task 3)
    python tools/competition/run_ai_function_tests.py --suite ai_column \\
        --case ai_column_ddl --database test

    # validate the parser / echo layer without any server at all
    python tools/competition/run_ai_function_tests.py --check-echo --suite ai_column --all

    # what is in a suite?
    python tools/competition/run_ai_function_tests.py --list-cases --suite ai_column

Supported mysqltest syntax
--------------------------
``--error N[,M...]``        expected error codes for the next statement
``--disable_warnings``      disable / enable warning reporting
``--enable_warnings``
``--disable_query_log``     accepted; echoing is kept
``--enable_query_log``
``--exec <cmd>``            run a shell command (cwd: tools/deploy/mysql_test)
``--real_sleep N``          really sleep N seconds
``--sleep N``               same as --real_sleep (scaled by --sleep-scale)
``let $v = value;``         assign a literal
``let $v = `select ...`;``  assign the first column of the first row
``while (cond) { ... }``    loop while the condition holds
``if (cond) { ... }``       conditional block
``dec $v;`` / ``inc $v;``   decrement / increment an integer variable
``eval <statement>;``       substitute ``$variables`` then run + echo
``connection X;``           ignored with a notice (single connection only)
``connect (...)``, ``disconnect``
``--source file``           skipped with a notice
``#`` comment lines         ignored

Control structures (``let``/``while``/``if``/``dec``/``inc``) never echo and
never produce output, exactly like mysqltest; ``eval`` echoes the *substituted*
statement text.  ``--exec`` output is echoed as-is because the reference runner
records it in ``.result`` (e.g. ``mock_no_requests=ok``).

Echo modes
----------
The checked-in baselines were not all produced by the same tool, so statements
are echoed in one of two styles and the mode is auto-detected per case:

``verbatim``  the statement text is echoed exactly as written, keeping the
              indentation of continuation lines;
``lstrip``    leading whitespace of a continuation line is dropped unless that
              line starts *inside* a string literal / quoted identifier / block
              comment (so the indentation of a JSON body inside ``'...'`` is
              preserved).

Only the mode that matches the baseline best is used, and it is reported for
every case.  ``--echo-mode`` forces one of them.

Output format notes
-------------------
* ``SELECT`` prints the column names first, ``\\t`` separated, then one row per
  line with ``NULL`` for SQL NULL;
* a failing statement prints ``ERROR <5 char sqlstate>: <message>``;
* when ``0`` appears in a ``--error`` list the statement may fail silently;
* ``Warnings:`` blocks are captured automatically for cases whose ``.result``
  contains one (``--warnings`` forces capture everywhere).

Failures are reported per statement: the block produced by a statement (its
echo, results, warnings and any intervening ``--exec`` output) is compared with
the matching slice of the ``.result`` file, so one mismatch does not stop the
case.  ``--max-diffs N`` (default 3) prints the context of the first N
differences and one line summaries for the rest; ``--max-diffs 0`` prints
summaries only.  ``--stop-on-error`` aborts a case at the first unexpected SQL
error and ``--keep-going`` continues with the remaining cases.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time
from pathlib import Path

try:
    import pymysql
    from pymysql import err as pymysql_err
    from pymysql.constants import FIELD_TYPE
except ImportError:  # pragma: no cover - environment problem, not logic
    sys.stderr.write(
        "[error] pymysql is required: python -m pip install pymysql\n")
    raise SystemExit(2)


DEFAULT_CASES = ("ai_prompt", "ai_function_enhancement")
ALL_CASES = DEFAULT_CASES + ("ai_model_ddl", "ai_model_endpoint_ddl")
#: present in the suite but need multiple connections / tenant setup, so they
#: have to be selected explicitly with --case
EXTRA_CASES = ("ai_model_lower_case_name_mysql0",
               "ai_model_lower_case_name_mysql1")

REPO_ROOT = Path(__file__).resolve().parents[2]
TEST_SUITE_ROOT = REPO_ROOT / "tools" / "deploy" / "mysql_test" / "test_suite"
#: working directory for ``--exec``.  The scripts say
#: ``python3 mysql_test/test_suite/...``, i.e. the path is relative to
#: ``tools/deploy`` (the parent of ``mysql_test``).
EXEC_CWD = REPO_ROOT / "tools" / "deploy"

ECHO_MODES = ("verbatim", "lstrip")
PYTHON_COMMAND_NAMES = ("python", "python3", "python.exe", "python3.exe")

#: Used when the server does not report a sqlstate in its ERR packet.
SQLSTATE_HINTS = {
    1045: "28000",
    1051: "42S02",
    1064: "42000",
    1146: "42S02",
    1210: "HY000",
    1227: "42000",
    1582: "42000",
    5083: "22000",
}

VARIABLE_RE = re.compile(r"\$([A-Za-z_][A-Za-z0-9_]*)")
DIRECTIVE_RE = re.compile(r"--[ \t]*([A-Za-z][A-Za-z0-9_]*)[ \t]*(.*)$")
WORD_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
CONDITION_RE = re.compile(r"^(.*?)\s*(==|!=|<=|>=|<|>)\s*(.*)$", re.S)


def split_command(command: str):
    """Split an ``--exec`` command line, Windows friendly.

    ``shlex`` would eat the backslashes of a Windows path, so quotes are
    honoured but backslashes are kept verbatim.
    """
    tokens = []
    current = []
    quote = None
    for char in command:
        if quote is not None:
            if char == quote:
                quote = None
            else:
                current.append(char)
        elif char in ("'", '"'):
            quote = char
        elif char.isspace():
            if current:
                tokens.append("".join(current))
                current = []
        else:
            current.append(char)
    if current:
        tokens.append("".join(current))
    return tokens


# --------------------------------------------------------------------------- #
# sqlstate capture: pymysql throws the wire sqlstate away, we need it
# --------------------------------------------------------------------------- #
def install_sqlstate_capture() -> None:
    """Teach pymysql to keep the ``#sqlstate`` field of ERR packets."""
    original = pymysql_err.raise_mysql_exception
    if getattr(original, "_seekdb_sqlstate_capture", False):
        return

    def wrapper(data):
        sqlstate = None
        try:
            if len(data) >= 9 and data[3:4] == b"#":
                sqlstate = data[4:9].decode("ascii", "replace")
        except Exception:
            sqlstate = None
        try:
            original(data)
        except Exception as exc:  # MySQLError subclasses carry (errno, message)
            try:
                if sqlstate and not getattr(exc, "sqlstate", None):
                    exc.sqlstate = sqlstate
            except Exception:
                pass
            raise

    wrapper._seekdb_sqlstate_capture = True
    pymysql_err.raise_mysql_exception = wrapper


install_sqlstate_capture()


def raw_conversions() -> dict:
    """Converters that hand back the server's own text for every column type."""
    def to_text(data, charset=None):
        if data is None:
            return None
        if isinstance(data, (bytes, bytearray)):
            encoding = charset or "utf8"
            try:
                return bytes(data).decode(encoding)
            except (UnicodeDecodeError, LookupError):
                return bytes(data).decode("utf-8", "surrogateescape")
        return data

    conversions = {}
    for key in pymysql.converters.conversions:
        if isinstance(key, int):
            conversions[key] = to_text
    conversions[FIELD_TYPE.NULL] = lambda data, charset=None: None
    return conversions


# --------------------------------------------------------------------------- #
# script AST
# --------------------------------------------------------------------------- #
class Node:
    """Base class for one mysqltest step."""

    __slots__ = ("line",)

    def __init__(self, line=0):
        self.line = line


class SqlNode(Node):
    """A statement that is echoed and executed (``eval`` uses ``is_eval``)."""

    __slots__ = ("text", "expected_errors", "warnings_on", "query_log_on",
                 "is_eval", "replace_regexes")

    def __init__(self, text, line, expected_errors, warnings_on, query_log_on,
                 is_eval=False, replace_regexes=None):
        super().__init__(line)
        self.text = text
        self.replace_regexes = replace_regexes or []
        self.expected_errors = expected_errors
        self.warnings_on = warnings_on
        self.query_log_on = query_log_on
        self.is_eval = is_eval

    def echo_lines(self):
        return self.text.split("\n")

    def __repr__(self):  # pragma: no cover - debugging aid
        return "SqlNode(line=%d, text=%r)" % (self.line, self.text[:40])


class ExecNode(Node):
    """``--exec <command>``."""

    __slots__ = ("command",)

    def __init__(self, command, line):
        super().__init__(line)
        self.command = command


class SleepNode(Node):
    """``--real_sleep N`` / ``--sleep N``."""

    __slots__ = ("seconds",)

    def __init__(self, seconds, line):
        super().__init__(line)
        self.seconds = seconds


class LetNode(Node):
    """``let $var = value;`` / ``let $var = `query`;``."""

    __slots__ = ("name", "source", "value")

    def __init__(self, name, source, value, line):
        super().__init__(line)
        self.name = name
        self.source = source      # "literal" | "query" | "var"
        self.value = value


class StepNode(Node):
    """``dec $var;`` / ``inc $var;``."""

    __slots__ = ("name", "delta")

    def __init__(self, name, delta, line):
        super().__init__(line)
        self.name = name
        self.delta = delta


class WhileNode(Node):
    __slots__ = ("condition", "body")

    def __init__(self, condition, body, line):
        super().__init__(line)
        self.condition = condition
        self.body = body


class IfNode(Node):
    __slots__ = ("condition", "body")

    def __init__(self, condition, body, line):
        super().__init__(line)
        self.condition = condition
        self.body = body


class ParseResult:
    def __init__(self):
        self.nodes = []
        self.notes = []
        self.statements = 0        # number of echoed statements
        self.control = 0           # number of control-flow steps
        self.commands = 0          # number of --exec steps

    def note(self, line, message):
        self.notes.append("line %d: %s" % (line, message))

    def anchors(self):
        """Every echo-able statement, in document order."""
        collected = []

        def walk(nodes):
            for node in nodes:
                if isinstance(node, SqlNode):
                    collected.append(node)
                elif isinstance(node, (WhileNode, IfNode)):
                    walk(node.body)

        walk(self.nodes)
        return collected


# --------------------------------------------------------------------------- #
# echo normalisation
# --------------------------------------------------------------------------- #
def normalize_echo(raw: str, mode: str) -> str:
    """Render a statement the way the selected baseline style does."""
    text = raw.strip("\r\n")
    if not text.strip():
        return ""
    if mode == "verbatim":
        return text.lstrip(" \t")
    if mode != "lstrip":
        raise ValueError("unknown echo mode: %r" % (mode,))

    out = []
    quote = None
    in_block = False
    index = 0
    length = len(text)
    at_line_start = True
    while index < length:
        char = text[index]
        if at_line_start and quote is None and not in_block:
            while index < length and text[index] in " \t":
                index += 1
            if index >= length:
                break
            char = text[index]
        at_line_start = False

        if char == "\n":
            out.append(char)
            at_line_start = True
            index += 1
            continue
        if in_block:
            if text.startswith("*/", index):
                out.append("*/")
                in_block = False
                index += 2
                continue
            out.append(char)
            index += 1
            continue
        if quote is not None:
            if char == "\\" and quote in ("'", '"') and index + 1 < length:
                out.append(text[index:index + 2])
                index += 2
                continue
            if char == quote:
                if text.startswith(quote * 2, index):
                    out.append(quote * 2)
                    index += 2
                    continue
                quote = None
                out.append(char)
                index += 1
                continue
            out.append(char)
            index += 1
            continue
        if char in ("'", '"', "`"):
            quote = char
            out.append(char)
            index += 1
            continue
        if text.startswith("/*", index):
            in_block = True
            out.append("/*")
            index += 2
            continue
        if (text.startswith("--", index)
                and (index + 2 >= length or text[index + 2] in " \t\r\n")):
            out.append(text[index:])
            break
        if char == "#":
            out.append(text[index:])
            break
        out.append(char)
        index += 1
    return "".join(out).strip("\r\n")


# --------------------------------------------------------------------------- #
# parser
# --------------------------------------------------------------------------- #
IGNORED_DIRECTIVES = {
    "disable_abort_on_error", "disable_info", "disable_metadata",
    "disable_ps_protocol", "disable_result_log", "disable_rpl_parse",
    "enable_abort_on_error", "enable_info", "enable_metadata",
    "enable_ps_protocol", "enable_result_log", "enable_rpl_parse",
    "horizontal_results", "vertical_results", "query_vertical",
    "replace_column", "replace_result", "sorted_result", "require",
    "skip", "skip_if_hypergraph", "skip_if_ob", "skip_if_mysql",
    "require_mysql", "require_ob", "not_ob", "delimiter", "echo",
    "mkdir", "rmdir", "remove_file", "write_file", "append_file", "perl",
    "loop", "die", "exit", "inc_result_file", "result_format", "let",
    "eval", "query", "connect", "connection", "disconnect", "disable_connect_log",
    "enable_connect_log",
}
SKIPPED_DIRECTIVES = {"source", "perl", "mkdir", "rmdir", "remove_file",
                      "write_file", "append_file", "echo", "delimiter", "loop",
                      "let", "eval", "query"}


class ScriptParser:
    """Recursive-descent parser for the mysqltest subset the suites use."""

    def __init__(self, text, echo_mode="lstrip"):
        self.text = text
        self.length = len(text)
        self.pos = 0
        self.line = 1
        self.echo_mode = echo_mode
        self.result = ParseResult()
        self.pending_errors = None
        self.pending_replace_regexes = []
        self.warnings_on = True
        self.query_log_on = True

    # -- primitives -----------------------------------------------------
    def _at_end(self):
        return self.pos >= self.length

    def _peek(self, count=1):
        return self.text[self.pos:self.pos + count]

    def _take(self, count=1):
        chunk = self.text[self.pos:self.pos + count]
        self.line += chunk.count("\n")
        self.pos += count
        return chunk

    def _skip_to_line_end(self):
        newline = self.text.find("\n", self.pos)
        newline = self.length if newline < 0 else newline
        self.line += self.text.count("\n", self.pos, newline)
        self.pos = newline

    def _at_directive(self):
        return bool(re.match(r"--[A-Za-z_]", self.text[self.pos:self.pos + 3]))

    def _skip_blank(self):
        while not self._at_end():
            char = self.text[self.pos]
            if char in " \t\r\n":
                self._take(1)
            elif char == "#":
                self._skip_to_line_end()
            elif (self.text.startswith("--", self.pos) and not self._at_directive()
                    and (self.pos + 2 >= self.length
                         or self.text[self.pos + 2] in " \t\r\n")):
                self._skip_to_line_end()
            else:
                return

    def _skip_inline_space(self):
        while not self._at_end() and self.text[self.pos] in " \t\r\n":
            self._take(1)

    def _peek_word(self):
        match = WORD_RE.match(self.text, self.pos)
        return match.group(0) if match else ""

    def _next_nonspace_is(self, char):
        index = self.pos
        while index < self.length and self.text[index] in " \t":
            index += 1
        return index < self.length and self.text[index] == char

    def _after_word_is(self, word, char):
        """True when the next non-space character after ``word`` is ``char``."""
        index = self.pos + len(word)
        while index < self.length and self.text[index] in " \t":
            index += 1
        return index < self.length and self.text[index] == char

    # -- readers --------------------------------------------------------
    def _read_until_semicolon(self):
        """Quote-aware read of one statement; returns the raw text with ';'."""
        start = self.pos
        quote = None
        in_block = False
        while not self._at_end():
            char = self.text[self.pos]
            if in_block:
                if self.text.startswith("*/", self.pos):
                    self.pos += 2
                    in_block = False
                else:
                    self.pos += 1
                continue
            if quote is not None:
                if char == "\\" and quote in ("'", '"'):
                    self.pos += 2
                    continue
                if char == quote:
                    if self.text.startswith(quote * 2, self.pos):
                        self.pos += 2
                        continue
                    quote = None
                    self.pos += 1
                    continue
                self.pos += 1
                continue
            if char in ("'", '"', "`"):
                quote = char
                self.pos += 1
                continue
            if self.text.startswith("/*", self.pos):
                in_block = True
                self.pos += 2
                continue
            if (self.text.startswith("--", self.pos)
                    and (self.pos + 2 >= self.length
                         or self.text[self.pos + 2] in " \t\r\n")):
                self._skip_to_line_end()
                continue
            if char == "#":
                self._skip_to_line_end()
                continue
            if char == ";":
                self.pos += 1
                raw = self.text[start:self.pos]
                self.line += raw.count("\n")
                return raw
            self.pos += 1
        raw = self.text[start:self.pos]
        self.line += raw.count("\n")
        self.result.note(self.line, "statement not terminated by ';'")
        return raw

    def _read_parenthesised(self):
        """Read ``( ... )`` from the current position and return the inside."""
        start = self.pos
        depth = 0
        while not self._at_end():
            char = self.text[self.pos]
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0:
                    self.pos += 1
                    inner = self.text[start + 1:self.pos - 1]
                    self.line += inner.count("\n")
                    return inner
            self.pos += 1
        self.result.note(self.line, "unbalanced '(' in condition")
        inner = self.text[start + 1:self.pos]
        self.line += inner.count("\n")
        return inner

    # -- node builders --------------------------------------------------
    def _parse_directive(self):
        line = self.line
        start = self.pos
        self._skip_to_line_end()
        raw = self.text[start:self.pos]
        match = DIRECTIVE_RE.match(raw)
        if not match:
            self.result.note(line, "cannot parse directive %r" % raw[:40])
            return None
        name = match.group(1).lower()
        argument = match.group(2).strip()

        if name == "error":
            codes = []
            for token in re.split(r"[,\s]+", argument):
                if not token:
                    continue
                try:
                    codes.append(int(token, 0))
                except ValueError:
                    self.result.note(line, "cannot parse --error token %r" % token)
            self.pending_errors = codes
            return None
        if name == "replace_regex":
            pairs = parse_replace_regex(argument)
            if not pairs:
                self.result.note(line, "cannot parse --replace_regex %r" % argument[:60])
            else:
                # applies to the output block of the next statement only
                self.pending_replace_regexes.extend(pairs)
            return None
        if name == "disable_warnings":
            self.warnings_on = False
            return None
        if name == "enable_warnings":
            self.warnings_on = True
            return None
        if name == "disable_query_log":
            self.query_log_on = False
            self.result.note(line, "--disable_query_log accepted, echoing stays on")
            return None
        if name == "enable_query_log":
            self.query_log_on = True
            return None
        if name == "exec":
            if not argument:
                self.result.note(line, "--exec without a command")
                return None
            self.result.commands += 1
            return ExecNode(argument, line)
        if name in ("real_sleep", "sleep"):
            try:
                seconds = float(argument)
            except ValueError:
                self.result.note(line, "--%s needs a number, got %r"
                                 % (name, argument))
                return None
            return SleepNode(seconds, line)
        if name in SKIPPED_DIRECTIVES:
            self.result.note(line, "--%s %s skipped" % (name, argument))
            return None
        if name in IGNORED_DIRECTIVES:
            return None
        self.result.note(line, "unsupported directive --%s skipped" % name)
        return None

    def _parse_let(self):
        line = self.line
        self._take(3)  # "let"
        raw = self._read_until_semicolon()
        body = raw[:-1] if raw.endswith(";") else raw
        match = re.match(r"\s*\$([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)$", body, re.S)
        if not match:
            self.result.note(line, "cannot parse let statement %r" % body[:40])
            return None
        name, value = match.group(1), match.group(2).strip()
        self.result.control += 1
        if value.startswith("`") and value.endswith("`") and len(value) >= 2:
            return LetNode(name, "query", value[1:-1], line)
        if re.fullmatch(r"\$[A-Za-z_][A-Za-z0-9_]*", value):
            return LetNode(name, "var", value[1:], line)
        return LetNode(name, "literal", value, line)

    def _parse_step(self, word):
        line = self.line
        self._take(len(word))
        raw = self._read_until_semicolon()
        match = re.match(r"\s*\$([A-Za-z_][A-Za-z0-9_]*)", raw)
        if not match:
            self.result.note(line, "cannot parse %s statement %r" % (word, raw[:40]))
            return None
        self.result.control += 1
        return StepNode(match.group(1), -1 if word == "dec" else 1, line)

    def _parse_block_owner(self, word):
        line = self.line
        self._take(len(word))
        self._skip_inline_space()
        if self._peek() != "(":
            self.result.note(line, "%s without a condition" % word)
            return None
        condition = self._read_parenthesised()
        self._skip_inline_space()
        body = []
        if self._peek() == "{":
            self._take(1)
            body = self._parse_nodes(stop_at_brace=True)
        else:
            self.result.note(line, "%s without a '{' block" % word)
        self.result.control += 1
        if word == "while":
            return WhileNode(condition, body, line)
        return IfNode(condition, body, line)

    def _parse_eval(self):
        line = self.line
        self._take(4)  # "eval"
        raw = self._read_until_semicolon()
        text = normalize_echo(raw, self.echo_mode)
        node = SqlNode(text, line, self.pending_errors, self.warnings_on,
                       self.query_log_on, is_eval=True,
                       replace_regexes=self.pending_replace_regexes)
        self.pending_errors = None
        self.pending_replace_regexes = []
        self.result.statements += 1
        return node

    def _parse_sql(self):
        line = self.line
        raw = self._read_until_semicolon()
        text = normalize_echo(raw, self.echo_mode)
        if not text or text == ";":
            return None
        node = SqlNode(text, line, self.pending_errors, self.warnings_on,
                       self.query_log_on, is_eval=False,
                       replace_regexes=self.pending_replace_regexes)
        self.pending_errors = None
        self.pending_replace_regexes = []
        self.result.statements += 1
        return node

    def _parse_connection_line(self):
        line = self.line
        start = self.pos
        while not self._at_end() and self.text[self.pos] != ";":
            self.pos += 1
        if not self._at_end():
            self.pos += 1
        raw = self.text[start:self.pos].strip()
        self.line += raw.count("\n")
        self.result.note(line, "connection control statement skipped "
                              "(single connection only): %s" % raw)

    # -- main loop ------------------------------------------------------
    def _parse_nodes(self, stop_at_brace=False):
        nodes = []
        while True:
            self._skip_blank()
            if self._at_end():
                if stop_at_brace:
                    self.result.note(self.line, "unterminated '{' block")
                return nodes
            char = self._peek()
            if char == "}":
                self._take(1)
                if stop_at_brace:
                    return nodes
                self.result.note(self.line, "unexpected '}'")
                continue
            if char == "{":
                self._take(1)
                nodes.extend(self._parse_nodes(stop_at_brace=True))
                continue
            if char == ";":
                self._take(1)
                continue
            if self._at_directive():
                node = self._parse_directive()
                if node is not None:
                    nodes.append(node)
                continue
            word = self._peek_word()
            low = word.lower()
            if low == "let" and self._after_word_is(word, "$"):
                node = self._parse_let()
                if node is not None:
                    nodes.append(node)
                continue
            if low in ("while", "if") and self._after_word_is(word, "("):
                node = self._parse_block_owner(low)
                if node is not None:
                    nodes.append(node)
                continue
            if low in ("dec", "inc") and self._after_word_is(word, "$"):
                node = self._parse_step(low)
                if node is not None:
                    nodes.append(node)
                continue
            if low == "eval":
                nodes.append(self._parse_eval())
                continue
            if low in ("connection", "connect", "disconnect"):
                self._parse_connection_line()
                continue
            node = self._parse_sql()
            if node is not None:
                nodes.append(node)
        return nodes

    def parse(self):
        self.result.nodes = self._parse_nodes()
        if self.pending_errors:
            self.result.note(self.line, "--error %s had no following statement"
                             % ",".join(str(code) for code in self.pending_errors))
        return self.result


def parse_test_file(path: Path, echo_mode: str = "lstrip") -> ParseResult:
    text = path.read_text(encoding="utf-8", errors="surrogateescape")
    return ScriptParser(text, echo_mode).parse()


# --------------------------------------------------------------------------- #
# condition / variable helpers
# --------------------------------------------------------------------------- #
def substitute(text: str, variables: dict) -> str:
    """Replace ``$name`` for names that are defined; leave everything else."""
    def replace(match):
        name = match.group(1)
        return variables[name] if name in variables else match.group(0)

    return VARIABLE_RE.sub(replace, text)


def _as_number(value: str):
    text = str(value).strip()
    if len(text) >= 2 and text[0] == text[-1] and text[0] in ("'", '"'):
        text = text[1:-1]
    text = text.strip()
    if not text:
        return None
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return float(text)
    except ValueError:
        return None


def _as_text(value: str) -> str:
    text = str(value).strip()
    if len(text) >= 2 and text[0] == text[-1] and text[0] in ("'", '"'):
        return text[1:-1]
    return text


def evaluate_condition(condition: str, variables: dict,
                       notes=None, line=0) -> bool:
    """Evaluate a mysqltest condition (``$var``, ``$var == 1``, ...)."""
    expression = substitute(condition, variables).strip()
    while expression.startswith("(") and expression.endswith(")"):
        depth = 0
        balanced = True
        for index, char in enumerate(expression):
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0 and index != len(expression) - 1:
                    balanced = False
                    break
        if not balanced:
            break
        expression = expression[1:-1].strip()
    if not expression:
        return False
    match = CONDITION_RE.match(expression)
    if match:
        left_text = match.group(1)
        right_text = match.group(3)
        operator = match.group(2)
        left_number = _as_number(left_text)
        right_number = _as_number(right_text)
        if left_number is not None or right_number is not None:
            # mysqltest evaluates conditions numerically; a value that is not a
            # number counts as 0 (``NULL`` == 0), which keeps polling loops
            # such as `if ($ready == 0) { dec $retry; }` converging
            left = 0 if left_number is None else left_number
            right = 0 if right_number is None else right_number
        else:
            left = _as_text(left_text)
            right = _as_text(right_text)
        if operator == "==":
            return left == right
        if operator == "!=":
            return left != right
        if operator == "<":
            return left < right
        if operator == ">":
            return left > right
        if operator == "<=":
            return left <= right
        if operator == ">=":
            return left >= right
        return False
    number = _as_number(expression)
    if number is not None:
        return bool(number)
    return bool(_as_text(expression))


# --------------------------------------------------------------------------- #
# output rendering
# --------------------------------------------------------------------------- #
def format_cell(value) -> str:
    if value is None:
        return "NULL"
    if isinstance(value, (bytes, bytearray)):
        return bytes(value).decode("utf-8", "surrogateescape")
    if isinstance(value, bool):
        return "1" if value else "0"
    return str(value)


def describe_error(exc):
    """Return ``(errno, "ERROR <sqlstate>: <message>")`` for a pymysql error."""
    errno = exc.args[0] if exc.args else None
    message = exc.args[1] if len(exc.args) > 1 else str(exc)
    if not isinstance(message, str):
        message = str(message)
    sqlstate = getattr(exc, "sqlstate", None)
    if not sqlstate:
        sqlstate = SQLSTATE_HINTS.get(errno, "HY000")
    return errno, "ERROR %s: %s" % (sqlstate, message)


class StatementDiff:
    """One statement whose rendered output differs from the .result file."""

    __slots__ = ("number", "line", "expected_start", "expected",
                 "actual_start", "actual", "offset", "lenient_offset", "note")

    def __init__(self, number, line, expected_start, expected, actual_start,
                 actual, offset, lenient_offset=None, note=""):
        self.number = number
        self.line = line
        self.expected_start = expected_start
        self.expected = expected
        self.actual_start = actual_start
        self.actual = actual
        self.offset = offset
        self.lenient_offset = lenient_offset
        self.note = note

    @property
    def indent_only(self):
        """True when the block matches once leading whitespace is ignored."""
        return self.offset is not None and self.lenient_offset is None


def parse_replace_regex(argument):
    """Parse one ``--replace_regex`` line into a list of (pattern, replacement).

    mysql-test accepts several space separated ``/pattern/replacement/[g]``
    pairs on one line, plus an optional ``g`` (or ``i``) flag after the closing
    slash, so both forms are supported here.  A backslash escapes the next
    character, which keeps ``\\/`` inside a pattern and ``\\1`` style back
    references intact: back references are valid in both mysql-test and Python,
    so the replacement text is used verbatim.
    """
    pairs = []
    text = argument
    i = 0
    while i < len(text):
        if text[i] != "/":
            i += 1
            continue
        i += 1
        parts = []
        value = []
        while i < len(text) and len(parts) < 2:
            if text[i] == "\\" and i + 1 < len(text):
                value.append(text[i:i + 2])
                i += 2
            elif text[i] == "/":
                parts.append("".join(value))
                value = []
                i += 1
            else:
                value.append(text[i])
                i += 1
        while i < len(text) and text[i] in "gi":
            i += 1
        if len(parts) < 2:
            break
        try:
            pairs.append((re.compile(parts[0]), parts[1]))
        except re.error:
            break
    return pairs


def apply_replace_regexes(lines, replaces):
    """Apply the ``--replace_regex`` rules of one statement, in order."""
    if not replaces:
        return list(lines)
    out = list(lines)
    for pattern, replacement in replaces:
        out = [pattern.sub(replacement, line) for line in out]
    return out


class CaseOutput:
    def __init__(self):
        self.lines = []
        # mirror of `lines` after the per statement --replace_regex rules: the .out
        # file keeps the raw text, the comparison uses this one
        self.compare_lines = []
        self.errors = 0          # unexpected errors (SQL or --exec)
        self.statements = 0      # echoed statements
        self.exec_runs = 0
        self.warnings_blocks = 0
        self.diffs = []          # per statement StatementDiff objects
        self.incomplete = ""

    def echo(self, text):
        new_lines = text.split("\n")
        self.lines.extend(new_lines)
        self.compare_lines.extend(new_lines)

    def extend(self, lines):
        self.lines.extend(lines)
        self.compare_lines.extend(lines)


def find_echo(expected, start, echo_lines, lenient=False):
    """First position >= start where ``echo_lines`` appears in ``expected``.

    With ``lenient`` the comparison ignores leading whitespace, which keeps the
    statement alignment alive when a baseline keeps the indentation of a few
    continuation lines (see ``--indent-tolerance``).
    """
    if not echo_lines:
        return None
    last = len(expected) - len(echo_lines)
    if last < 0:
        return None

    def matches(position):
        if not lenient:
            return expected[position:position + len(echo_lines)] == echo_lines
        for offset, line in enumerate(echo_lines):
            if expected[position + offset].lstrip() != line.lstrip():
                return False
        return True

    for position in range(start, last + 1):
        if matches(position):
            return position
    return None


def find_echo_for_block(expected, start, echo_lines):
    """Exact anchor match first, then a whitespace-insensitive one."""
    found = find_echo(expected, start, echo_lines, lenient=False)
    if found is None:
        found = find_echo(expected, start, echo_lines, lenient=True)
    return found


# --------------------------------------------------------------------------- #
# execution
# --------------------------------------------------------------------------- #
class RunState:
    __slots__ = ("output", "expected", "pointer", "pending_start", "tracking",
                 "unresynced", "query_failed")

    def __init__(self, output, expected):
        self.output = output
        self.expected = expected
        self.pointer = 0
        self.pending_start = 0
        self.tracking = expected is not None
        self.unresynced = False
        self.query_failed = False


class CaseRunner:
    def __init__(self, connection, options, exec_cwd=None):
        self.connection = connection
        self.options = options
        self.exec_cwd = Path(exec_cwd) if exec_cwd else EXEC_CWD
        self.variables = {}
        self.warnings_for_case = False

    # -- comparison -----------------------------------------------------
    def _flush(self, state, next_echo=None):
        """Compare everything emitted since the last flush with ``.result``."""
        block = state.output.compare_lines[state.pending_start:]
        if state.expected is not None and state.tracking:
            if next_echo is None:
                resync = len(state.expected)
            else:
                found = find_echo_for_block(state.expected, state.pointer,
                                            next_echo)
                if found is None:
                    resync = len(state.expected)
                    state.tracking = False
                    state.unresynced = True
                else:
                    resync = found
            slice_expected = state.expected[state.pointer:resync]
            offset = compare_lines(slice_expected, block,
                                   self.options.ignore_trailing_space, False)
            lenient = None
            if offset is not None:
                lenient = compare_lines(slice_expected, block,
                                        self.options.ignore_trailing_space, True)
            note = ""
            if state.unresynced and next_echo is not None:
                note = ("cannot resynchronise with the .result after this "
                        "statement")
            if offset is not None:
                state.output.diffs.append(
                    StatementDiff(len(state.output.diffs) + 1,
                                  self._current_line, state.pointer,
                                  slice_expected, state.pending_start, block,
                                  offset, lenient, note))
            state.pointer = resync
        state.pending_start = len(state.output.compare_lines)

    _current_line = 0

    # -- exec helpers ---------------------------------------------------
    def _cursor(self):
        return self.connection.cursor()

    @staticmethod
    def _close(cursor):
        try:
            cursor.close()
        except Exception:
            pass

    def _drain(self, cursor):
        while True:
            try:
                if not cursor.nextset():
                    return
            except Exception:
                return

    def _collect_warnings(self, output):
        cursor = self._cursor()
        try:
            cursor.execute("SHOW WARNINGS")
            rows = cursor.fetchall()
            self._drain(cursor)
        except pymysql.MySQLError:
            return
        finally:
            self._close(cursor)
        rendered = ["\t".join(format_cell(cell) for cell in row) for row in rows]
        if rendered:
            output.extend(["Warnings:"] + rendered)
            output.warnings_blocks += 1

    # -- node execution -------------------------------------------------
    def run(self, parsed, expected=None):
        output = CaseOutput()
        state = RunState(output, expected)
        self._exec_nodes(parsed.nodes, state)
        self._current_line = 0
        self._flush(state, next_echo=None)
        return output

    def _exec_nodes(self, nodes, state):
        for node in nodes:
            if isinstance(node, SqlNode):
                self._exec_sql(node, state)
            elif isinstance(node, ExecNode):
                self._exec_command(node, state)
            elif isinstance(node, SleepNode):
                self._exec_sleep(node)
            elif isinstance(node, LetNode):
                self._exec_let(node, state)
            elif isinstance(node, StepNode):
                self._exec_step(node)
            elif isinstance(node, WhileNode):
                self._exec_while(node, state)
            elif isinstance(node, IfNode):
                if evaluate_condition(node.condition, self.variables):
                    self._exec_nodes(node.body, state)

    def _exec_sleep(self, node):
        seconds = node.seconds * max(0.0, self.options.sleep_scale)
        if seconds > 0:
            time.sleep(seconds)

    def _exec_let(self, node, state):
        if node.source == "query":
            sql = substitute(node.value, self.variables)
            value, failed = self._query_scalar(sql, state)
            if failed:
                # mysqltest aborts on a failed query inside `let`; we record the
                # error and let the enclosing while loop stop instead of
                # spinning on a variable that never gets a value
                state.query_failed = True
        elif node.source == "var":
            value = self.variables.get(node.value, "")
        else:
            value = substitute(node.value, self.variables)
        self.variables[node.name] = str(value)

    def _exec_step(self, node):
        current = self.variables.get(node.name, "0")
        number = _as_number(current)
        number = 0 if number is None else number
        try:
            updated = int(number) + node.delta
        except (TypeError, ValueError):
            updated = node.delta
        self.variables[node.name] = str(updated)

    def _exec_while(self, node, state):
        iterations = 0
        limit = self.options.max_loop_iterations
        while evaluate_condition(node.condition, self.variables):
            iterations += 1
            if iterations > limit:
                message = ("while loop at script line %d hit the %d iteration "
                           "cap; the condition never became false"
                           % (node.line, limit))
                print("  %s" % message, flush=True)
                state.output.errors += 1
                state.output.incomplete = message
                return
            state.query_failed = False
            self._exec_nodes(node.body, state)
            if state.query_failed:
                message = ("while loop at script line %d abandoned: a query in "
                           "the loop body failed, so the loop cannot converge"
                           % node.line)
                print("  %s" % message, flush=True)
                if not state.output.incomplete:
                    state.output.incomplete = message
                return

    def _query_scalar(self, sql, state):
        """Run a ``let $v = `query`;`` query; returns ``(value, failed)``."""
        cursor = self._cursor()
        try:
            cursor.execute(sql)
            rows = cursor.fetchall() if cursor.description is not None else []
            self._drain(cursor)
        except pymysql.MySQLError as exc:
            _, line = describe_error(exc)
            print("    [let] query failed: %s" % line, flush=True)
            state.output.errors += 1
            return "", True
        finally:
            self._close(cursor)
        if not rows:
            return "", False
        return format_cell(rows[0][0]), False

    def _exec_command(self, node, state):
        state.output.exec_runs += 1
        argv = split_command(node.command)
        if not argv:
            return
        if Path(argv[0]).name.lower() in PYTHON_COMMAND_NAMES:
            argv[0] = self.options.python3 or sys.executable
        if self.options.verbose:
            print("    [exec] %s" % " ".join(argv), flush=True)
        try:
            completed = subprocess.run(
                argv, cwd=str(self.exec_cwd), stdin=subprocess.DEVNULL,
                capture_output=True, text=True, encoding="utf-8",
                errors="replace", timeout=self.options.exec_timeout)
            returncode = completed.returncode
            text = (completed.stdout or "") + (completed.stderr or "")
        except subprocess.TimeoutExpired:
            returncode = -1
            text = "exec timed out after %ss: %s" % (self.options.exec_timeout,
                                                     node.command)
        except OSError as exc:
            returncode = -1
            text = "exec failed to start: %s" % exc
        text = text.replace("\r\n", "\n").replace("\r", "\n").rstrip("\n")
        if text.strip():
            state.output.extend(text.split("\n"))
        if returncode != 0:
            state.output.errors += 1
            print("    [exec] exit %s" % returncode, flush=True)
            if self.options.stop_on_error:
                state.output.incomplete = ("aborted at script line %d "
                                           "(--exec failed)" % node.line)
                raise _AbortCase(state.output.incomplete)

    def _exec_sql(self, node, state):
        text = substitute(node.text, self.variables) if node.is_eval else node.text
        self._current_line = node.line
        compare_mark = len(state.output.compare_lines)
        self._flush(state, next_echo=text.split("\n"))
        state.output.echo(text)
        state.output.statements += 1
        if self.options.verbose:
            print("    [%4d] %s" % (node.line, text.split("\n")[0][:90]),
                  flush=True)

        cursor = self._cursor()
        try:
            cursor.execute(text)
            description = cursor.description
            rows = cursor.fetchall() if description is not None else []
            self._drain(cursor)
        except pymysql.MySQLError as exc:
            errno, error_line = describe_error(exc)
            expected_codes = node.expected_errors or []
            if expected_codes and (errno in expected_codes or errno is None):
                if 0 not in expected_codes:
                    state.output.extend(error_line.splitlines())
            else:
                state.output.extend(error_line.splitlines())
                state.output.errors += 1
                if self.options.verbose:
                    print("    unexpected error: %s" % error_line, flush=True)
                if self.options.stop_on_error:
                    state.output.incomplete = ("aborted at script line %d"
                                               % node.line)
                    print("  %s (--stop-on-error)" % state.output.incomplete,
                          flush=True)
                    raise _AbortCase(state.output.incomplete)
        else:
            if node.expected_errors and 0 not in node.expected_errors:
                state.output.extend(
                    ("Expected an error (%s) but the statement succeeded"
                     % ",".join(str(code) for code in node.expected_errors)
                     ).splitlines())
                state.output.errors += 1
            if description is not None:
                state.output.extend(["\t".join(str(column[0])
                                               for column in description)])
                for row in rows:
                    state.output.extend(["\t".join(format_cell(cell)
                                                   for cell in row)])
            if (self.warnings_for_case and node.warnings_on
                    and node.query_log_on):
                self._collect_warnings(state.output)
        finally:
            self._close(cursor)
        if node.replace_regexes:
            # --replace_regex rewrites the comparison mirror only: the .out file keeps
            # the real values so that a failure stays debuggable
            state.output.compare_lines[compare_mark:] = apply_replace_regexes(
                state.output.compare_lines[compare_mark:], node.replace_regexes)


class _AbortCase(Exception):
    def __init__(self, reason):
        super().__init__(reason)
        self.reason = reason


# --------------------------------------------------------------------------- #
# comparison / reporting
# --------------------------------------------------------------------------- #
def read_lines(path: Path):
    text = path.read_text(encoding="utf-8", errors="surrogateescape")
    return text.splitlines()


def compare_lines(expected, actual, ignore_trailing=True, ignore_leading=False):
    """Return the index of the first differing line, or None."""
    def normalise(line):
        if ignore_leading:
            line = line.lstrip()
        return line.rstrip() if ignore_trailing else line

    limit = max(len(expected), len(actual))
    for index in range(limit):
        left = normalise(expected[index]) if index < len(expected) else None
        right = normalise(actual[index]) if index < len(actual) else None
        if left != right:
            return index
    return None


def print_context(expected, actual, index, context=5, source="",
                  expected_label="expected", actual_label="actual"):
    """Print the first mismatch with ``context`` lines around it."""
    first = max(0, index - context)
    last = min(max(len(expected), len(actual)), index + context + 1)
    print("  first difference at line %d" % (index + 1))
    for label, lines in ((expected_label, expected), (actual_label, actual)):
        print("  --- %s ---" % label)
        for offset in range(first, last):
            marker = ">" if offset == index else " "
            if offset < len(lines):
                text = lines[offset]
            else:
                text = "<missing line>"
            print("  %s %5d | %s" % (marker, offset + 1, text))
    if index < len(expected) and index < len(actual):
        left = expected[index]
        right = actual[index]
        if left != right:
            print("  hint: %d chars expected vs %d chars actual"
                  % (len(left), len(right)))
    if source:
        print("  full actual output: %s" % source)


def print_statement_diff(diff, position, total, context=5,
                         expected_label="expected", actual_label="actual"):
    """Print one per-statement difference with context lines."""
    print("  --- diff %d/%d: statement #%d (script line %d) ---"
          % (position, total, diff.number, diff.line))
    if diff.note:
        print("  note: %s" % diff.note)
    print("  expected lines %d-%d, actual lines %d-%d, first difference at "
          "statement offset %d"
          % (diff.expected_start + 1, diff.expected_start + len(diff.expected),
             diff.actual_start + 1, diff.actual_start + len(diff.actual),
             diff.offset + 1))
    first = max(0, diff.offset - context)
    last = max(len(diff.expected), len(diff.actual))
    last = min(last, diff.offset + context + 1)
    for label, lines, base in ((expected_label, diff.expected, diff.expected_start),
                               (actual_label, diff.actual, diff.actual_start)):
        print("  --- %s ---" % label)
        for offset in range(first, last):
            marker = ">" if offset == diff.offset else " "
            if offset < len(lines):
                text = lines[offset]
            else:
                text = "<missing line>"
            print("  %s %5d | %s" % (marker, base + offset + 1, text))


def diff_statistics(expected, actual):
    """Count aligned lines: identical / indentation-only / everything else.

    Trailing whitespace is ignored, matching the comparison verdict.
    """
    aligned = min(len(expected), len(actual))
    identical = 0
    indent_only = 0
    for index in range(aligned):
        left = expected[index].rstrip()
        right = actual[index].rstrip()
        if left == right:
            identical += 1
        elif left.lstrip() == right.lstrip():
            indent_only += 1
    other = (max(len(expected), len(actual)) - aligned)
    other += aligned - identical - indent_only
    return identical, indent_only, other


def first_diff_text(diffs):
    """Compact one line description of the first real difference."""
    if not diffs:
        return None
    diff = diffs[0]
    expected_line = (diff.expected[diff.offset]
                     if diff.offset < len(diff.expected) else "<missing>")
    actual_line = (diff.actual[diff.offset]
                   if diff.offset < len(diff.actual) else "<missing>")
    text = ("statement #%d (script line %d): expected %s vs actual %s"
            % (diff.number, diff.line, _quote(expected_line, 60),
               _quote(actual_line, 60)))
    return text if len(text) <= 240 else text[:240] + "..."


def count_indent_only_lines(diff):
    """How many lines inside a diff differ only in leading whitespace."""
    count = 0
    for offset in range(min(len(diff.expected), len(diff.actual))):
        expected = diff.expected[offset]
        actual = diff.actual[offset]
        if expected != actual and expected.lstrip() == actual.lstrip():
            count += 1
    return count


def print_diff_summary(diff, position, total):
    """One compact line for a difference beyond --max-diffs."""
    expected_first = (diff.expected[diff.offset]
                      if diff.offset < len(diff.expected) else "<missing>")
    actual_first = (diff.actual[diff.offset]
                    if diff.offset < len(diff.actual) else "<missing>")
    print("  diff %d/%d: statement #%d (script line %d): expected %s vs actual %s"
          % (position, total, diff.number, diff.line,
             _quote(expected_first), _quote(actual_first)))


def _quote(text, limit=90):
    text = (text.replace("\\", "\\\\").replace("\t", "\\t")
            .replace("\r", "\\r").replace("\n", "\\n"))
    if len(text) > limit:
        text = text[:limit] + "..."
    return '"%s"' % text


# --------------------------------------------------------------------------- #
# echo-mode detection
# --------------------------------------------------------------------------- #
def check_echo(statements, expected, lenient=False):
    """Verify that every statement echo appears, in order, in ``expected``.

    ``eval`` statements are skipped: their text is only known after the
    ``$variables`` are substituted at run time.  With ``lenient`` the match
    ignores leading whitespace, which separates "the baseline indents this
    differently" from "the statement is missing".
    """
    pointer = 0
    problems = []
    for number, statement in enumerate(statements, start=1):
        if statement.is_eval:
            continue
        block = statement.echo_lines()
        found = find_echo(expected, pointer, block, lenient=lenient)
        if found is None:
            problems.append((number, statement, pointer))
        else:
            pointer = found + len(block)
    return problems


def choose_echo_mode(options, test_path, expected):
    """Pick the echo style that the baseline actually uses.

    ``verbatim`` is preferred; ``lstrip`` is used when it matches better.  The
    decision and the rejected counts are reported, never applied silently.
    """
    if options.echo_mode in ECHO_MODES:
        parsed = parse_test_file(test_path, options.echo_mode)
        return parsed, {"mode": options.echo_mode, "forced": True, "counts": {}}

    counts = {}
    parsed_by_mode = {}
    chosen = None
    for mode in ECHO_MODES:  # verbatim first
        parsed = parse_test_file(test_path, mode)
        problems = check_echo(parsed.anchors(), expected)
        counts[mode] = len(problems)
        parsed_by_mode[mode] = parsed
        if not problems:
            chosen = mode
            break
    if chosen is None:
        chosen = min(ECHO_MODES, key=lambda mode: counts[mode])
    return parsed_by_mode[chosen], {"mode": chosen, "forced": False,
                                    "counts": counts}


# --------------------------------------------------------------------------- #
# command line
# --------------------------------------------------------------------------- #
def build_arguments(argv=None):
    parser = argparse.ArgumentParser(
        description="Run seekdb mysqltest cases and diff against .result",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="ai_function defaults: %s\n--all adds: %s"
               % (", ".join(DEFAULT_CASES),
                  ", ".join(ALL_CASES[len(DEFAULT_CASES):])))
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--user", default="root")
    # nargs="?" + const="" so that a shell which eats empty arguments
    # (PowerShell and `--password ''`) still yields an empty password
    parser.add_argument("--password", nargs="?", const="", default=None,
                        metavar="PASSWORD",
                        help="password (default: $MYSQL_PWD or empty)")
    parser.add_argument("--database", default="test")
    parser.add_argument("--suite", default="ai_function")
    parser.add_argument("--case", action="append", default=None,
                        metavar="NAME",
                        help="run only this case (repeatable)")
    parser.add_argument("--all", action="store_true",
                        help="ai_function: the two DDL cases too; other suites: "
                             "every case in the suite")
    parser.add_argument("--list", action="store_true",
                        help="list the cases of the suite (alias of --list-cases)")
    parser.add_argument("--list-cases", action="store_true",
                        help="list the cases of the suite with statement counts")
    parser.add_argument("--keep-going", action="store_true",
                        help="continue with the remaining cases after a failure")
    parser.add_argument("--stop-on-error", action="store_true",
                        help="abort the current case at the first unexpected "
                             "SQL error (default: keep executing the case)")
    parser.add_argument("--max-diffs", type=int, default=3, metavar="N",
                        help="print the context of at most N per-statement "
                             "differences per case (default 3, 0 = summary only)")
    parser.add_argument("--echo-mode", default="auto", choices=("auto",) + ECHO_MODES,
                        help="statement echo style (default: auto-detect per case)")
    parser.add_argument("--indent-tolerance", default="auto",
                        choices=("auto", "on", "off"),
                        help="ignore leading-whitespace-only differences when "
                             "nothing else differs (default: auto)")
    parser.add_argument("--warnings", action="store_true",
                        help="always capture SHOW WARNINGS (default: only for "
                             "cases whose .result contains a Warnings: block)")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--check-echo", action="store_true",
                        help="only validate the parsed statement echo against "
                             "the .result file (no server needed)")
    parser.add_argument("--selftest", action="store_true",
                        help="offline self-test of parser/rendering/diffing "
                             "(no server needed)")
    parser.add_argument("--ignore-trailing-space", dest="ignore_trailing_space",
                        action="store_true", default=True)
    parser.add_argument("--strict-trailing-space", dest="ignore_trailing_space",
                        action="store_false")
    parser.add_argument("--context", type=int, default=5,
                        help="lines of context around the first difference")
    parser.add_argument("--summary-json", default=None, metavar="PATH",
                        help="write a machine readable per-case summary (JSON)")
    parser.add_argument("--output-dir", default=None,
                        help="where to write <case>.out (default: under %%TEMP%%)")
    parser.add_argument("--test-suite-root", default=str(TEST_SUITE_ROOT),
                        help="directory containing <suite>/t and <suite>/r")
    parser.add_argument("--exec-cwd", default=str(EXEC_CWD),
                        help="working directory for --exec commands")
    parser.add_argument("--exec-timeout", type=int, default=300,
                        help="timeout for one --exec command (seconds)")
    parser.add_argument("--python3", default=None,
                        help="interpreter used for 'python3 ...' in --exec "
                             "(default: this interpreter)")
    parser.add_argument("--sleep-scale", type=float, default=1.0,
                        help="multiply --real_sleep durations (debugging)")
    parser.add_argument("--max-loop-iterations", type=int, default=100000,
                        help="safety limit for while loops")
    parser.add_argument("--connect-timeout", type=int, default=10)
    parser.add_argument("--query-timeout", type=int, default=1800,
                        help="per statement timeout in seconds (0 = none)")
    return parser


def suite_directory(options) -> Path:
    return Path(options.test_suite_root) / options.suite


def suite_cases(options):
    """Every ``<case>.test`` of the selected suite, sorted."""
    test_dir = suite_directory(options) / "t"
    return sorted(path.stem for path in test_dir.glob("*.test"))


def selected_cases(options):
    if options.case:
        cases = []
        for name in options.case:
            if name == "all":
                cases.extend(ALL_CASES if options.suite == "ai_function"
                             else suite_cases(options))
            else:
                cases.append(name)
        return cases
    if options.all:
        if options.suite == "ai_function":
            return list(ALL_CASES)
        return suite_cases(options)
    if options.suite != "ai_function":
        raise SystemExit(
            "[error] suite %r has no default case list; pass --case <name> "
            "or --all (cases: %s)"
            % (options.suite, ", ".join(suite_cases(options))))
    return list(DEFAULT_CASES)


def connect(options):
    conversions = raw_conversions()
    return pymysql.connect(
        host=options.host,
        port=options.port,
        user=options.user,
        password=options.password,
        database=options.database,
        charset="utf8mb4",
        autocommit=True,
        conv=conversions,
        connect_timeout=options.connect_timeout,
    )


def preflight(options):
    """Return a connection or print a clear 'seekdb not running' message."""
    try:
        connection = connect(options)
    except pymysql.MySQLError as exc:
        print("[error] seekdb 未启动 / seekdb is not running: "
              "cannot connect to %s:%d as %r (%s)"
              % (options.host, options.port, options.user, exc), flush=True)
        print("[error] hint: start seekdb, or pass --host/--port/--user/"
              "--password that point at a running instance.", flush=True)
        return None

    timeout = options.query_timeout
    cursor = connection.cursor()
    try:
        if timeout and timeout > 0:
            try:
                cursor.execute("SET SESSION ob_query_timeout = %d"
                               % (timeout * 1000000))
            except pymysql.MySQLError:
                pass
        cursor.execute("SELECT 1")
        row = cursor.fetchone()
        if not row or format_cell(row[0]) != "1":
            print("[error] seekdb 未启动 / probe 'SELECT 1' returned %r" % (row,),
                  flush=True)
            connection.close()
            return None
    except pymysql.MySQLError as exc:
        print("[error] seekdb 未启动 / probe 'SELECT 1' failed: %s" % (exc,),
              flush=True)
        try:
            connection.close()
        except Exception:
            pass
        return None
    finally:
        try:
            cursor.close()
        except Exception:
            pass
    if options.verbose:
        print("[info] connected to %s:%d db=%r"
              % (options.host, options.port, options.database), flush=True)
    return connection


def resolve_paths(options, case):
    root = suite_directory(options)
    return (root / "t" / (case + ".test"),
            root / "r" / "mysql" / (case + ".result"))


def run_list_cases(options) -> int:
    """List every <case>.test of the suite with its statement count."""
    root = suite_directory(options)
    test_dir = root / "t"
    result_dir = root / "r" / "mysql"
    if not test_dir.is_dir():
        print("[error] no such suite directory: %s" % test_dir, flush=True)
        return 2
    tests = sorted(test_dir.glob("*.test"))
    if not tests:
        print("[error] no .test files under %s" % test_dir, flush=True)
        return 2
    print("[info] suite %r under %s" % (options.suite, root))
    print("  %-40s %6s %6s %6s  %s"
          % ("case", "stmts", "ctl", "exec", "notes"))
    total = 0
    for test_path in tests:
        case = test_path.stem
        parsed = parse_test_file(test_path, "lstrip")
        total += parsed.statements
        notes = []
        if case in DEFAULT_CASES:
            notes.append("default")
        elif case in ALL_CASES:
            notes.append("--all")
        if not (result_dir / (case + ".result")).exists():
            notes.append("MISSING .result")
        if parsed.notes:
            notes.append("%d parse note(s)" % len(parsed.notes))
        print("  %-40s %6d %6d %6d  %s"
              % (case, parsed.statements, parsed.control, parsed.commands,
                 ", ".join(notes)))
    print("  %d case(s), %d statement(s) in total" % (len(tests), total))
    return 0


# --------------------------------------------------------------------------- #
# main
# --------------------------------------------------------------------------- #
def main(argv=None) -> int:
    parser = build_arguments(argv)
    options = parser.parse_args(argv)
    if options.password is None:
        options.password = os.environ.get("MYSQL_PWD", "")
    cases = selected_cases(options)

    if options.list or options.list_cases:
        return run_list_cases(options)

    missing = []
    for case in cases:
        test_path, result_path = resolve_paths(options, case)
        if not test_path.exists():
            missing.append(str(test_path))
        if not result_path.exists():
            missing.append(str(result_path))
    if missing:
        for path in missing:
            print("[error] missing file: %s" % path, flush=True)
        return 2

    if options.output_dir:
        output_dir = Path(options.output_dir)
    else:
        output_dir = Path(tempfile.gettempdir()) / "seekdb_ai_function_tests"
    output_dir.mkdir(parents=True, exist_ok=True)

    if options.check_echo:
        return run_check_echo(options, cases)

    if options.selftest:
        return run_selftest(options)

    connection = preflight(options)
    if connection is None:
        return 2

    runner = CaseRunner(connection, options)
    results = []
    summary_entries = []

    def record(case, status, output=None, mode_info=None, expected=None,
               actual_path=None, first_diff=None):
        entry = {
            "name": case,
            "suite": options.suite,
            "status": status,
            "statements": 0 if output is None else output.statements,
            "expected_lines": 0 if expected is None else len(expected),
            "actual_lines": 0 if output is None else len(output.lines),
            "identical": 0,
            "indent_only": 0,
            "other": 0,
            "first_diff": first_diff,
            "echo_mode": None if not mode_info else mode_info.get("mode"),
            "warnings": bool(getattr(runner, "warnings_for_case", False)),
            "errors": 0 if output is None else output.errors,
            "output_file": None if actual_path is None else str(actual_path),
        }
        if output is not None and expected is not None:
            identical, indent_only, other = diff_statistics(expected,
                                                            output.compare_lines)
            entry["identical"] = identical
            entry["indent_only"] = indent_only
            entry["other"] = other
        summary_entries.append(entry)
        return entry

    def write_summary_json():
        if not options.summary_json:
            return
        passed = sum(1 for entry in summary_entries
                     if entry["status"] == "PASS")
        failed = sum(1 for entry in summary_entries
                     if entry["status"] == "FAIL")
        payload = {
            "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "host": options.host,
            "port": options.port,
            "database": options.database,
            "suite_root": str(suite_directory(options)),
            "passed": passed,
            "failed": failed,
            "cases": summary_entries,
        }
        path = Path(options.summary_json)
        if path.parent and not path.parent.exists():
            path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
                        encoding="utf-8")

    try:
        for position, case in enumerate(cases):
            test_path, result_path = resolve_paths(options, case)
            print("\n=== %s ===" % case, flush=True)
            print("  script: %s" % test_path, flush=True)
            print("  expect: %s" % result_path, flush=True)

            expected = read_lines(result_path)
            parsed, mode_info = choose_echo_mode(options, test_path, expected)
            for note in parsed.notes:
                print("  [note] %s" % note, flush=True)
            if mode_info.get("forced"):
                print("  echo mode: %s (forced by --echo-mode)"
                      % mode_info["mode"], flush=True)
            else:
                counts = mode_info["counts"]
                detail = ", ".join("%s: %d unmatched" % (mode, counts[mode])
                                   for mode in ECHO_MODES if mode in counts)
                print("  echo mode: %s (%s)" % (mode_info["mode"], detail),
                      flush=True)
            print("  %d statement(s), %d control step(s), %d --exec command(s)"
                  % (parsed.statements, parsed.control, parsed.commands),
                  flush=True)

            runner.warnings_for_case = bool(
                options.warnings
                or any(line.strip() == "Warnings:" for line in expected))
            if runner.warnings_for_case:
                print("  warnings: captured (SHOW WARNINGS after each statement)",
                      flush=True)
            runner.variables = {}

            try:
                output = runner.run(parsed, expected=expected)
            except _AbortCase as abort:
                output = None
                print("  %s" % abort.reason, flush=True)
                results.append((case, False, None, mode_info))
                record(case, "FAIL", mode_info=mode_info,
                       first_diff="aborted: %s" % abort.reason)
                if not options.keep_going:
                    print("  stopping: first failing case (use --keep-going to "
                          "run the rest)", flush=True)
                    for remaining in cases[position + 1:]:
                        results.append((remaining, None, None, None))
                        record(remaining, "SKIPPED")
                    break
                continue

            actual_path = output_dir / (case + ".out")
            actual_path.write_text("\n".join(output.lines) + "\n",
                                   encoding="utf-8", errors="surrogateescape")

            strict_index = compare_lines(expected, output.compare_lines,
                                         options.ignore_trailing_space, False)
            lenient_index = compare_lines(expected, output.compare_lines,
                                          options.ignore_trailing_space, True)
            real_diffs = [diff for diff in output.diffs if not diff.indent_only]
            indent_diffs = [diff for diff in output.diffs if diff.indent_only]

            tolerate = options.indent_tolerance in ("auto", "on")
            clean = strict_index is None and output.errors == 0
            tolerant_clean = (not clean and tolerate
                              and lenient_index is None and output.errors == 0)

            if clean:
                print("  PASS  %d statement(s), %d output line(s)"
                      % (output.statements, len(output.lines)), flush=True)
            else:
                total = len(real_diffs)
                shown = max(0, options.max_diffs)
                if total:
                    if shown:
                        print("  %d statement(s) differ from the baseline; "
                              "showing the first %d:" % (total, min(shown, total)),
                              flush=True)
                    else:
                        print("  %d statement(s) differ from the baseline; "
                              "summaries only (--max-diffs 0):" % total,
                              flush=True)
                    for number, diff in enumerate(real_diffs):
                        if number < shown:
                            print_statement_diff(
                                diff, number + 1, total, options.context,
                                expected_label="expected (%s)"
                                               % result_path.name,
                                actual_label="actual (<case>.out)")
                        else:
                            print_diff_summary(diff, number + 1, total)
                if strict_index is not None and not total:
                    print_context(expected, output.compare_lines, strict_index,
                                  options.context, str(actual_path),
                                  expected_label="expected (%s)"
                                                 % result_path.name,
                                  actual_label="actual (<case>.out)")
                if output.errors:
                    print("  %d unexpected error(s)" % output.errors)
                if output.incomplete:
                    print("  run %s" % output.incomplete)
                if indent_diffs and not tolerant_clean:
                    indent_lines = sum(count_indent_only_lines(diff)
                                       for diff in indent_diffs)
                    print("  %d statement(s) / %d line(s) differ only in "
                          "leading whitespace (--indent-tolerance %s)"
                          % (len(indent_diffs), indent_lines,
                             options.indent_tolerance), flush=True)

                if tolerant_clean:
                    indent_lines = sum(count_indent_only_lines(diff)
                                       for diff in indent_diffs)
                    print("  PASS  (leading-whitespace tolerant: %d line(s) in "
                          "%d statement(s) differ only in indentation)"
                          % (indent_lines, len(indent_diffs)), flush=True)
                    for diff in indent_diffs[:max(1, shown)]:
                        for offset in range(min(len(diff.expected),
                                                len(diff.actual))):
                            expected_line = diff.expected[offset]
                            actual_line = diff.actual[offset]
                            if (expected_line != actual_line
                                    and expected_line.lstrip() == actual_line.lstrip()):
                                print("    baseline line %d keeps its original "
                                      "indentation: %s"
                                      % (diff.expected_start + offset + 1,
                                         _quote(expected_line)), flush=True)
                    if len(indent_diffs) > max(1, shown):
                        print("    ... %d more statement(s) with indentation-only "
                              "differences" % (len(indent_diffs) - max(1, shown)),
                              flush=True)
                else:
                    print("  FAIL  (expected %d line(s), got %d line(s))"
                          % (len(expected), len(output.lines)), flush=True)
            ok = clean or tolerant_clean
            if ok and indent_diffs and not clean:
                pass
            results.append((case, ok, output, mode_info))
            record(case, "PASS" if ok else "FAIL", output=output,
                   mode_info=mode_info, expected=expected,
                   actual_path=actual_path,
                   first_diff=None if ok else first_diff_text(real_diffs))
            if not ok and not options.keep_going:
                print("  stopping: first failing case (use --keep-going to "
                      "run the rest)", flush=True)
                for remaining in cases[position + 1:]:
                    results.append((remaining, None, None, None))
                    record(remaining, "SKIPPED")
                break
    finally:
        try:
            connection.close()
        except Exception:
            pass
        write_summary_json()

    print("\n=== summary ===")
    failed = 0
    for case, ok, output, mode_info in results:
        if ok is None:
            state = "SKIPPED"
        elif ok:
            state = "PASS"
        else:
            state = "FAIL"
            failed += 1
        detail = "" if output is None else "  (%d statements)" % output.statements
        if mode_info and mode_info.get("mode"):
            detail += "  echo=%s" % mode_info["mode"]
        print("  %-32s %-8s%s" % (case, state, detail))
    if failed:
        print("\n%d case(s) failed" % failed)
        return 1
    print("\nall selected cases PASS")
    return 0


def run_check_echo(options, cases) -> int:
    print("[check-echo] validating parsed .test output against .result "
          "(no server involved)")
    failures = 0
    for case in cases:
        test_path, result_path = resolve_paths(options, case)
        expected = read_lines(result_path)
        counts = {}
        details = {}
        parsed = None
        lenient_clean = False
        for mode in ECHO_MODES:
            parsed = parse_test_file(test_path, mode)
            problems = check_echo(parsed.anchors(), expected)
            counts[mode] = len(problems)
            details[mode] = problems
            if not problems:
                lenient_clean = True
                break
        if not lenient_clean and parsed is not None:
            # exact echo of every statement failed: is it only indentation?
            lenient_problems = check_echo(parsed.anchors(), expected,
                                          lenient=True)
            lenient_clean = not lenient_problems
            if not lenient_clean:
                details["lenient"] = lenient_problems
        for note in parse_test_file(test_path, "lstrip").notes:
            print("  [note] %s: %s" % (case, note))
        detail = ", ".join("%s %d/%d unmatched"
                           % (mode, counts[mode], len(parsed.anchors()))
                           for mode in ECHO_MODES if mode in counts)
        if lenient_clean:
            best = min(counts, key=lambda mode: counts[mode])
            suffix = ("" if counts[best] == 0
                      else "  [%d statement(s) indent-only]"
                           % counts[best])
            print("  %-32s ok    %s%s" % (case, detail, suffix))
        else:
            failures += 1
            print("  %-32s FAIL  %s" % (case, detail))
            source = details.get("lenient") or details[min(counts, key=counts.get)]
            for number, statement, pointer in source[:5]:
                print("    statement #%d (line %d):" % (number, statement.line))
                for line in statement.echo_lines()[:6]:
                    print("      | %s" % line)
                print("    expected output resumes at line %d" % (pointer + 1))
    return 1 if failures else 0


# --------------------------------------------------------------------------- #
# offline self-test (no server, no seekdb build required)
# --------------------------------------------------------------------------- #
def _mysql_error(errno, sqlstate, message):
    exc = pymysql.OperationalError(errno, message)
    exc.sqlstate = sqlstate
    return exc


class _ScriptedCursor:
    """Minimal pymysql cursor stand-in driven by a table of scripted outcomes."""

    def __init__(self, table):
        self.table = table
        self.description = None
        self._rows = []

    def execute(self, sql):
        outcome = self.table.get(sql)
        if outcome is None:
            raise AssertionError("unscripted statement: %r" % sql)
        if isinstance(outcome, Exception):
            raise outcome
        header, rows = outcome
        self.description = (None if header is None
                            else [(name, None, None, None, None, None, None)
                                  for name in header])
        self._rows = list(rows)

    def fetchall(self):
        return list(self._rows)

    def fetchone(self):
        return self._rows[0] if self._rows else None

    def nextset(self):
        return False

    def close(self):
        pass


class _ScriptedConnection:
    def __init__(self, table):
        self.table = table

    def cursor(self):
        return _ScriptedCursor(self.table)

    def close(self):
        pass


SELFTEST_SCRIPT = """# selftest script
--disable_warnings
drop table if exists t1;
select ai_prompt('{0}', 'x');
select id, val from t1;
insert into t1 values (1);
call DBMS_AI_SERVICE.CREATE_AI_MODEL(
  'm1', '{
    "type": "completion"
  }');
--error 5083
select ai_prompt(1);
--error 0,11118
call DBMS_AI_SERVICE.DROP_AI_MODEL('gone');
--error 11118
call DBMS_AI_SERVICE.DROP_AI_MODEL('also-gone');
let $n = 2;
while ($n)
{
  dec $n;
}
eval select $n = 0 as loop_ok;
--real_sleep 0
--exec CMD_ECHO_MARKER
"""

SELFTEST_EXPECTED = [
    "drop table if exists t1;",
    "select ai_prompt('{0}', 'x');",
    "ai_prompt('{0}', 'x')",
    '{"args": ["x"], "template": "{0}"}',
    "select id, val from t1;",
    "id\tval",
    "1\tNULL",
    "insert into t1 values (1);",
    "call DBMS_AI_SERVICE.CREATE_AI_MODEL(",
    "'m1', '{",
    '    "type": "completion"',
    "  }');",
    "select ai_prompt(1);",
    "ERROR 22000: Invalid data type for the operation",
    "call DBMS_AI_SERVICE.DROP_AI_MODEL('gone');",
    "call DBMS_AI_SERVICE.DROP_AI_MODEL('also-gone');",
    "ERROR HY000: This AI model 'also-gone' was not found",
    "select 0 = 0 as loop_ok;",
    "loop_ok",
    "1",
    "EXEC-OK",
]

SELFTEST_TABLE = {
    "drop table if exists t1;": (None, []),
    "select ai_prompt('{0}', 'x');": (
        ["ai_prompt('{0}', 'x')"], [('{"args": ["x"], "template": "{0}"}',)]),
    "select id, val from t1;": (["id", "val"], [("1", None)]),
    "insert into t1 values (1);": (None, []),
    "call DBMS_AI_SERVICE.CREATE_AI_MODEL(\n'm1', '{\n    \"type\": \"completion\"\n  }');": (None, []),
    "select ai_prompt(1);": _mysql_error(
        5083, "22000", "Invalid data type for the operation"),
    "call DBMS_AI_SERVICE.DROP_AI_MODEL('gone');": _mysql_error(
        11118, "HY000", "This AI model 'gone' was not found"),
    "call DBMS_AI_SERVICE.DROP_AI_MODEL('also-gone');": _mysql_error(
        11118, "HY000", "This AI model 'also-gone' was not found"),
    "select 0 = 0 as loop_ok;": (["loop_ok"], [("1",)]),
}


def run_selftest(options) -> int:
    print("[selftest] offline parser/renderer/diff checks")
    failures = []

    def check(name, condition, detail=""):
        print("  %-42s %s %s" % (name, "ok" if condition else "FAIL", detail))
        if not condition:
            failures.append(name)

    temp_dir = Path(tempfile.mkdtemp(prefix="seekdb_ai_function_selftest_"))
    # --exec must be able to run something real: use this interpreter
    exec_command = ("%s -c \"print('EXEC-OK')\"" % sys.executable)
    script_path = temp_dir / "selftest.test"
    script_text = SELFTEST_SCRIPT.replace("CMD_ECHO_MARKER", exec_command)
    script_path.write_text(script_text, encoding="utf-8")

    parsed = parse_test_file(script_path, "lstrip")
    check("statement count", parsed.statements == 9, parsed.statements)
    check("control steps", parsed.control == 3, parsed.control)
    check("exec commands", parsed.commands == 1, parsed.commands)
    check("string-interior indentation kept",
          any('    "type": "completion"' in node.text
              for node in parsed.anchors()))
    check("continuation line outside string stripped",
          any(node.text.startswith("call DBMS_AI_SERVICE.CREATE_AI_MODEL(\n'm1'")
              for node in parsed.anchors()))

    verbatim = parse_test_file(script_path, "verbatim")
    check("verbatim keeps the continuation indent",
          any(node.text.startswith("call DBMS_AI_SERVICE.CREATE_AI_MODEL(\n  'm1'")
              for node in verbatim.anchors()))

    runner = CaseRunner(_ScriptedConnection(SELFTEST_TABLE), options,
                        exec_cwd=temp_dir)
    output = runner.run(parsed, expected=SELFTEST_EXPECTED)
    check("no unexpected errors", output.errors == 0, output.errors)
    check("no per-statement diffs", not output.diffs,
          [diff.number for diff in output.diffs])
    check("exec ran once", output.exec_runs == 1, output.exec_runs)

    index = compare_lines(SELFTEST_EXPECTED, output.lines)
    if index is None:
        check("rendered output matches", True, "%d lines" % len(output.lines))
    else:
        check("rendered output matches", False, "first diff at %d" % (index + 1))
        print_context(SELFTEST_EXPECTED, output.lines, index)

    # a single wrong value must be reported as exactly one diff, and the run
    # must still reach the last statement
    broken_table = dict(SELFTEST_TABLE)
    broken_table["select id, val from t1;"] = (["id", "val"], [("1", "WRONG")])
    broken = CaseRunner(_ScriptedConnection(broken_table), options,
                        exec_cwd=temp_dir).run(parsed, expected=SELFTEST_EXPECTED)
    check("continues after a mismatch", broken.statements == 9,
          broken.statements)
    check("one diff for one wrong value", len(broken.diffs) == 1,
          [diff.number for diff in broken.diffs])

    # an indentation-only difference must be recognised as such
    indented = list(SELFTEST_EXPECTED)
    indented[6] = "  1\tNULL"
    indent_table = dict(SELFTEST_TABLE)
    indent_table["select id, val from t1;"] = (["id", "val"], [("1", None)])
    indent_out = CaseRunner(_ScriptedConnection(indent_table), options,
                            exec_cwd=temp_dir).run(parsed, expected=indented)
    check("indent-only diff detected", len(indent_out.diffs) == 1,
          len(indent_out.diffs))
    if indent_out.diffs:
        check("indent-only diff classified",
              indent_out.diffs[0].indent_only, indent_out.diffs[0].lenient_offset)

    # variable substitution / conditions
    variables = {"n": "2"}
    check("condition truthy", evaluate_condition("($n)", variables))
    check("condition comparison",
          evaluate_condition("$n == 2", variables)
          and not evaluate_condition("$n != 2", variables))
    check("substitution leaves unknown vars",
          substitute("select '$.input', $n", variables) == "select '$.input', 2")
    # mysqltest polls with `if ($ready == 0)`: a NULL/failed value must compare
    # as 0 so the loop converges instead of spinning
    check("non numeric value compares as 0",
          evaluate_condition("$ready == 0", {"ready": "NULL"}))
    check("non numeric value is not 1",
          not evaluate_condition("$ready == 1", {"ready": "NULL"}))

    # diff reporting helpers must not crash
    mutated = list(SELFTEST_EXPECTED)
    mutated[6] = "1\t"
    check("diff detects NULL mismatch", compare_lines(mutated, output.lines) == 6)
    check("trailing space tolerated",
          compare_lines(["a  ", "b"], ["a", "b"]) is None)
    check("leading space tolerated",
          compare_lines(["  a"], ["a"], ignore_leading=True) is None
          and compare_lines(["  a"], ["a"]) == 0)

    # --replace_regex: one pair, a trailing mtr flag, several pairs on one line,
    # and a back reference in the replacement
    single = parse_replace_regex("/_[0-9]+/_0/")
    check("replace_regex single pair", len(single) == 1
          and apply_replace_regexes(["a_12"], single) == ["a_0"], single)
    flagged = parse_replace_regex("/__vid_[0-9]+/__vid_0/g")
    check("replace_regex tolerates the g flag",
          len(flagged) == 1 and apply_replace_regexes(["x__vid_12"], flagged) == ["x__vid_0"],
          flagged)
    multi = parse_replace_regex(
        "/__vid_[0-9]+(_[0-9]+)?/__vid_0/g /__type_[0-9]+_[0-9]+/__type_0_0/g")
    check("replace_regex multiple pairs", len(multi) == 2, multi)
    if len(multi) == 2:
        check("replace_regex multiple pairs applied",
              apply_replace_regexes(["__vid_12_7 __type_3_9"], multi)
              == ["__vid_0 __type_0_0"])
    backref = parse_replace_regex("/output\\(\\[F\\(([^)]+)\\)\\]\\)/output([\\1])/g")
    check("replace_regex back reference",
          len(backref) == 1
          and apply_replace_regexes(["output([F(x)])"], backref) == ["output([x])"],
          backref)

    try:
        script_path.unlink()
        temp_dir.rmdir()
    except OSError:
        pass

    if failures:
        print("[selftest] FAILED: %s" % ", ".join(failures))
        return 1
    print("[selftest] PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())