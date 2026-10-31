#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Local seekdb instance manager for the competition verification flow.

The release binary is built to ``build_release/src/observer/seekdb.exe``.  This
helper knows how to start it in the foreground-daemon style the rest of the
tooling expects (``--nodaemon`` plus redirected logs and a pid file), wait
until the MySQL port really answers, probe its command line, run a statement
and shut it down again.

Command line surface of the binary (from
``src/observer/ob_command_line_parser.cpp``)::

    -P, --port <port>        MySQL protocol port (default 2881)
        --base-dir <dir>     work directory the process chdir()s into
        --data-dir <dir>     data directory, default ${base-dir}/store
        --redo-dir <dir>     redo directory
        --nodaemon           stay in the foreground
        --parameter k=v      repeatable server parameter
        --variable k=v       repeatable system variable
        --log-level <level>  e.g. INFO / WARN / ERROR
        --embedded           in-process mode
    -V, --version
    -h, --help
    (short options: ``P:Vh6``)

Sub commands::

    seekdb_server.py probe  [--binary PATH]
    seekdb_server.py start  [--binary PATH] [--port 2881] [--base-dir DIR]
                            [--data-dir DIR] [--redo-dir DIR]
                            [--parameter k=v ...] [--log-level INFO]
                            [--timeout 180]
    seekdb_server.py stop   [--base-dir DIR] [--rm]
    seekdb_server.py status [--base-dir DIR] [--port 2881]
    seekdb_server.py exec   --sql "create database if not exists test"
                            [--port 2881] [--user root] [--password '']
    seekdb_server.py dump-log [--base-dir DIR] [--lines 50]

Default ``--base-dir`` is ``%TEMP%/seekdb_local_<port>``; the pid file,
``seekdb.log`` and ``seekdb.state.json`` live there.

Single-node seekdb ships with ``root`` / empty password.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

try:
    import pymysql
except ImportError:  # pragma: no cover - optional, only `exec`/probes need it
    pymysql = None

REPO_ROOT = Path(__file__).resolve().parents[2]
#: build output is ``build_release/src/observer/seekdb`` on Linux and
#: ``.../seekdb.exe`` on Windows
DEFAULT_BINARY = (REPO_ROOT / "build_release" / "src" / "observer"
                  / ("seekdb.exe" if os.name == "nt" else "seekdb"))
DEFAULT_PORT = 2881

PID_NAME = "seekdb.pid"
STATE_NAME = "seekdb.state.json"
LOG_NAME = "seekdb.log"
CMD_NAME = "seekdb.cmd"

BUILD_HINT = ("build it first:  .\\build.ps1 release --init -j 10   "
              "(output goes to build_release/src/observer/seekdb.exe)")


# --------------------------------------------------------------------------- #
# helpers
# --------------------------------------------------------------------------- #
def default_base_dir(port: int) -> Path:
    return Path(tempfile.gettempdir()) / ("seekdb_local_%d" % port)


def port_open(host: str, port: int, timeout: float = 0.3) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def wait_port(host: str, port: int, timeout: float) -> float:
    start = time.time()
    while time.time() - start < timeout:
        if port_open(host, port):
            return time.time() - start
        time.sleep(max(0.05, min(0.2, timeout / 100.0)))
    raise TimeoutError("port %d did not open within %.0fs" % (port, timeout))


def sql_probe(host: str, port: int, user: str, password: str,
              timeout: float = 3.0) -> Exception | None:
    """Return None when ``SELECT 1`` works, else the exception.

    ``read_timeout``/``write_timeout`` matter here: a socket that accepts the
    connection but never sends the handshake would otherwise block forever.
    """
    if pymysql is None:
        return None
    budget = max(1.0, min(float(timeout), 5.0))
    try:
        connection = pymysql.connect(host=host, port=port, user=user,
                                     password=password,
                                     connect_timeout=max(1, int(budget)),
                                     read_timeout=budget,
                                     write_timeout=budget)
    except Exception as exc:
        return exc
    try:
        cursor = connection.cursor()
        cursor.execute("SELECT 1")
        cursor.fetchone()
        cursor.close()
    except Exception as exc:
        return exc
    finally:
        try:
            connection.close()
        except Exception:
            pass
    return None


def log_tail(path: Path, lines: int) -> str:
    if not path.exists():
        return "<no log file at %s>" % path
    try:
        content = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as exc:
        return "<cannot read %s: %s>" % (path, exc)
    tail = content[-lines:]
    return "\n".join(tail) if tail else "<log is empty>"


def print_log_tail(path: Path, lines: int, reason: str) -> None:
    print("[seekdb] %s; last %d line(s) of %s:" % (reason, lines, path), flush=True)
    for line in log_tail(path, lines).splitlines():
        print("    | %s" % line, flush=True)


def read_state(base_dir: Path) -> dict:
    try:
        return json.loads((base_dir / STATE_NAME).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def read_pid(base_dir: Path):
    for name in (PID_NAME,):
        try:
            return int((base_dir / name).read_text(encoding="ascii").strip())
        except (OSError, ValueError):
            continue
    state = read_state(base_dir)
    pid = state.get("pid")
    return pid if isinstance(pid, int) else None


def process_alive(pid: int) -> bool:
    if pid is None or pid <= 0:
        return False
    if os.name == "nt":
        try:
            result = subprocess.run(["tasklist", "/FI", "PID eq %d" % pid,
                                     "/NH"], capture_output=True, text=True,
                                    timeout=15)
        except (OSError, subprocess.SubprocessError):
            return False
        return str(pid) in result.stdout
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def resolve_binary(binary: str | Path) -> Path:
    path = Path(binary)
    if not path.is_absolute():
        path = (REPO_ROOT / path)
    return path


# --------------------------------------------------------------------------- #
# probe
# --------------------------------------------------------------------------- #
def command_probe(options) -> int:
    binary = resolve_binary(options.binary)
    print("[seekdb] binary: %s" % binary, flush=True)
    if not binary.exists():
        print("[seekdb] FAIL: binary not found. " + BUILD_HINT, flush=True)
        return 2
    print("[seekdb] size: %d bytes" % binary.stat().st_size, flush=True)

    status = 0
    for label, argument in (("--version", "-V"), ("--help", "-h")):
        try:
            completed = subprocess.run([str(binary), argument],
                                       capture_output=True, text=True,
                                       timeout=options.timeout)
        except subprocess.TimeoutExpired:
            print("[seekdb] %s timed out after %ss" % (label, options.timeout),
                  flush=True)
            status = 1
            continue
        except OSError as exc:
            print("[seekdb] %s failed to run: %s" % (label, exc), flush=True)
            status = 1
            continue
        print("\n=== %s (exit %d) ===" % (label, completed.returncode), flush=True)
        for stream_name, stream in (("stdout", completed.stdout),
                                    ("stderr", completed.stderr)):
            if stream and stream.strip():
                for line in stream.rstrip("\n").splitlines():
                    print("  %s | %s" % (stream_name, line), flush=True)
        if completed.returncode != 0:
            status = 1
    return status


# --------------------------------------------------------------------------- #
# start / stop / status
# --------------------------------------------------------------------------- #
def command_start(options) -> int:
    base_dir = Path(options.base_dir) if options.base_dir else default_base_dir(options.port)
    data_dir = Path(options.data_dir) if options.data_dir else base_dir / "store"
    redo_dir = Path(options.redo_dir) if options.redo_dir else None
    host = "127.0.0.1"

    if port_open(host, options.port):
        print("[seekdb] port %d is already accepting connections; assuming an "
              "instance is already running and skipping the start step"
              % options.port, flush=True)
        print("[seekdb]   (use `status`/`stop` with --base-dir to manage it)",
              flush=True)
        return 0

    binary = resolve_binary(options.binary)
    if not binary.exists():
        print("[seekdb] FAIL: binary not found: %s" % binary, flush=True)
        print("[seekdb] " + BUILD_HINT, flush=True)
        return 2

    base_dir.mkdir(parents=True, exist_ok=True)
    for directory in (data_dir, redo_dir):
        if directory is not None:
            directory.mkdir(parents=True, exist_ok=True)
    if data_dir.resolve() == base_dir.resolve():
        print("[seekdb] FAIL: --data-dir must differ from --base-dir", flush=True)
        return 2
    if redo_dir is not None and redo_dir.resolve() in (base_dir.resolve(),
                                                      data_dir.resolve()):
        print("[seekdb] FAIL: --redo-dir must differ from base/data dir", flush=True)
        return 2

    argv = [str(binary), "--nodaemon", "--port", str(options.port),
            "--base-dir", str(base_dir.resolve()),
            "--data-dir", str(data_dir.resolve())]
    if redo_dir is not None:
        argv += ["--redo-dir", str(redo_dir.resolve())]
    if options.log_level:
        argv += ["--log-level", options.log_level]
    for parameter in options.parameter or []:
        argv += ["--parameter", parameter]
    for extra in options.extra_arg or []:
        argv.append(extra)

    (base_dir / CMD_NAME).write_text(" ".join(argv) + "\n", encoding="utf-8")
    log_path = base_dir / LOG_NAME
    log_stream = log_path.open("ab")
    print("[seekdb] starting: %s" % " ".join(argv), flush=True)
    print("[seekdb] log: %s" % log_path, flush=True)

    kwargs = {}
    if os.name == "nt":
        kwargs["creationflags"] = (subprocess.CREATE_NEW_PROCESS_GROUP
                                   | subprocess.DETACHED_PROCESS)
    else:
        kwargs["start_new_session"] = True
    try:
        process = subprocess.Popen(argv, cwd=str(base_dir), stdin=subprocess.DEVNULL,
                                   stdout=log_stream, stderr=subprocess.STDOUT,
                                   **kwargs)
    except OSError as exc:
        print("[seekdb] FAIL: cannot launch the binary: %s" % exc, flush=True)
        return 2
    finally:
        log_stream.close()

    (base_dir / PID_NAME).write_text(str(process.pid), encoding="ascii")
    state = {
        "pid": process.pid,
        "port": options.port,
        "host": host,
        "base_dir": str(base_dir.resolve()),
        "data_dir": str(data_dir.resolve()),
        "redo_dir": None if redo_dir is None else str(redo_dir.resolve()),
        "binary": str(binary),
        "argv": argv,
        "started_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
    }
    (base_dir / STATE_NAME).write_text(json.dumps(state, indent=2, sort_keys=True),
                                       encoding="utf-8")

    deadline = time.time() + options.timeout
    try:
        wait_port(host, options.port, max(1.0, options.timeout))
    except TimeoutError as exc:
        print("[seekdb] FAIL: %s" % exc, flush=True)
        print_log_tail(log_path, 50, "server did not open the MySQL port")
        _kill(process.pid)
        return 1

    # the listener may come up before SQL is ready: confirm with SELECT 1
    sql_ready = pymysql is None
    last_error = None
    while not sql_ready and time.time() < deadline:
        last_error = sql_probe(host, options.port, options.user, options.password)
        if last_error is None:
            sql_ready = True
            break
        if process.poll() is not None:
            print("[seekdb] FAIL: the server exited with code %s"
                  % process.returncode, flush=True)
            print_log_tail(log_path, 50, "startup aborted")
            return 1
        time.sleep(0.2)

    elapsed = time.time() - (deadline - options.timeout)
    if process.poll() is not None:
        print("[seekdb] FAIL: the server exited with code %s" % process.returncode,
              flush=True)
        print_log_tail(log_path, 50, "startup aborted")
        return 1
    if not sql_ready:
        print("[seekdb] FAIL: port %d is open but 'SELECT 1' never succeeded "
              "within %ss (last error: %s)" % (options.port, options.timeout,
                                               last_error), flush=True)
        print_log_tail(log_path, 50, "SQL layer not ready")
        _kill(process.pid)
        return 1
    print("[seekdb] ready in %.1fs (pid %d, port %d, base-dir %s)"
          % (elapsed, process.pid, options.port, base_dir), flush=True)
    print("[seekdb] default credentials: user=%r password='' (single-node mode), "
          "connect with host 127.0.0.1" % options.user, flush=True)
    return 0


def _kill(pid: int) -> None:
    if pid is None:
        return
    if os.name == "nt":
        subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"],
                       capture_output=True, text=True, timeout=30)
        return
    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    for _ in range(100):
        if not process_alive(pid):
            return
        time.sleep(0.05)
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def command_stop(options) -> int:
    base_dir = Path(options.base_dir) if options.base_dir else default_base_dir(options.port)
    state = read_state(base_dir)
    port = state.get("port", options.port)
    host = state.get("host", "127.0.0.1")
    pid = read_pid(base_dir)

    if pid is None and not port_open(host, port):
        print("[seekdb] no pid file under %s and port %d is closed; nothing to stop"
              % (base_dir, port), flush=True)
        if options.rm:
            return _remove_dir(base_dir)
        return 0

    if pid is not None:
        print("[seekdb] stopping pid %d (port %d)" % (pid, port), flush=True)
        _kill(pid)
    else:
        print("[seekdb] no pid file; waiting for port %d to close" % port, flush=True)

    for _ in range(300):
        if not port_open(host, port):
            break
        time.sleep(0.05)
    still_open = port_open(host, port)
    if still_open:
        print("[seekdb] WARN: port %d is still accepting connections; something "
              "else may own it" % port, flush=True)
    else:
        print("[seekdb] stopped (port %d closed)" % port, flush=True)

    for name in (PID_NAME,):
        try:
            (base_dir / name).unlink()
        except OSError:
            pass

    data_dir = Path(state.get("data_dir") or (base_dir / "store"))
    print("[seekdb] data directory kept at %s" % data_dir, flush=True)
    print("[seekdb]   pass --rm to delete the whole base dir (%s) as well"
          % base_dir, flush=True)
    if options.rm:
        return _remove_dir(base_dir)
    return 0


def _remove_dir(base_dir: Path) -> int:
    """Delete a base dir, but only one this script created."""
    if not base_dir.exists():
        print("[seekdb] %s does not exist; nothing to delete" % base_dir, flush=True)
        return 0
    markers = (base_dir / STATE_NAME, base_dir / PID_NAME, base_dir / CMD_NAME)
    if not any(marker.exists() for marker in markers):
        print("[seekdb] refusing to delete %s: it has no %s/%s/%s marker, so it "
              "was not created by this script" % (base_dir, STATE_NAME, PID_NAME,
                                                  CMD_NAME), flush=True)
        return 2
    resolved = base_dir.resolve()
    if resolved.parent == resolved:
        print("[seekdb] refusing to delete a filesystem root: %s" % resolved,
              flush=True)
        return 2
    print("[seekdb] deleting %s" % resolved, flush=True)
    shutil.rmtree(resolved, ignore_errors=True)
    if resolved.exists():
        print("[seekdb] WARN: %s still exists (files in use?)" % resolved,
              flush=True)
        return 1
    return 0


def command_status(options) -> int:
    base_dir = Path(options.base_dir) if options.base_dir else default_base_dir(options.port)
    state = read_state(base_dir)
    port = state.get("port", options.port)
    host = state.get("host", "127.0.0.1")
    pid = read_pid(base_dir)
    listening = port_open(host, port)
    alive = process_alive(pid) if pid is not None else False
    print("[seekdb] base-dir : %s" % base_dir)
    print("[seekdb] pid      : %s" % (pid if pid is not None else "<none>"))
    print("[seekdb] process  : %s" % ("running" if alive else "not running"))
    print("[seekdb] port     : %d %s" % (port, "listening" if listening
                                         else "closed"))
    if state:
        print("[seekdb] data-dir : %s" % state.get("data_dir"))
        print("[seekdb] started  : %s" % state.get("started_at"))
    return 0 if listening else 1


# --------------------------------------------------------------------------- #
# exec / dump-log
# --------------------------------------------------------------------------- #
def command_exec(options) -> int:
    if pymysql is None:
        print("[seekdb] FAIL: pymysql is required for `exec` "
              "(python -m pip install pymysql)", flush=True)
        return 2
    statements = options.sql or []
    if not statements:
        print("[seekdb] FAIL: nothing to run, pass --sql", flush=True)
        return 2
    # bound the handshake first: a socket that accepts but never speaks MySQL
    # would otherwise sit on the longer read timeout used for real statements
    probe_error = sql_probe(options.host, options.port, options.user,
                            options.password,
                            timeout=min(5, max(1, options.timeout)))
    if probe_error is not None:
        print("[seekdb] FAIL: cannot connect to %s:%d as %r (%s)"
              % (options.host, options.port, options.user, probe_error), flush=True)
        print("[seekdb] hint: start the instance with `start`, or check --port",
              flush=True)
        return 3
    try:
        connection = pymysql.connect(host=options.host, port=options.port,
                                     user=options.user,
                                     password=options.password,
                                     database=options.database or None,
                                     charset="utf8mb4", autocommit=True,
                                     connect_timeout=options.timeout,
                                     read_timeout=options.query_timeout,
                                     write_timeout=options.query_timeout)
    except Exception as exc:
        print("[seekdb] FAIL: cannot connect to %s:%d as %r (%s)"
              % (options.host, options.port, options.user, exc), flush=True)
        print("[seekdb] hint: start the instance with `start`, or check --port",
              flush=True)
        return 3
    status = 0
    try:
        for statement in statements:
            cursor = connection.cursor()
            try:
                cursor.execute(statement)
                if cursor.description is not None:
                    header = [column[0] for column in cursor.description]
                    print("[seekdb] %s" % "\t".join(str(name) for name in header),
                          flush=True)
                    for row in cursor.fetchall():
                        print("[seekdb] %s" % "\t".join(
                            "NULL" if cell is None else str(cell) for cell in row),
                            flush=True)
                print("[seekdb] ok: %s" % statement.strip().splitlines()[0][:120],
                      flush=True)
            except pymysql.MySQLError as exc:
                print("[seekdb] FAIL: %s\n           %s" % (statement.strip(), exc),
                      flush=True)
                status = 1
            finally:
                try:
                    cursor.close()
                except Exception:
                    pass
    finally:
        try:
            connection.close()
        except Exception:
            pass
    return status


def command_dump_log(options) -> int:
    base_dir = Path(options.base_dir) if options.base_dir else default_base_dir(options.port)
    log_path = base_dir / LOG_NAME
    if not log_path.exists():
        print("[seekdb] no log at %s" % log_path, flush=True)
        return 1
    print(log_tail(log_path, options.lines), flush=True)
    return 0


# --------------------------------------------------------------------------- #
# entry point
# --------------------------------------------------------------------------- #
def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Start/stop/probe a local seekdb instance",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="default binary: %s\ndefault base-dir: %%TEMP%%/seekdb_local_<port>"
               % DEFAULT_BINARY)
    subparsers = parser.add_subparsers(dest="command")

    def add_common(sub, with_binary=False):
        sub.add_argument("--port", type=int, default=DEFAULT_PORT)
        sub.add_argument("--base-dir", default=None)
        sub.add_argument("--user", default="root")
        sub.add_argument("--host", default="127.0.0.1")
        if with_binary:
            sub.add_argument("--binary", default=str(DEFAULT_BINARY),
                             help="path to seekdb(.exe)")

    probe = subparsers.add_parser("probe", help="run '<binary> -h' and '-V'")
    add_common(probe, with_binary=True)
    probe.add_argument("--timeout", type=int, default=60)

    start = subparsers.add_parser("start", help="start a local instance")
    add_common(start, with_binary=True)
    start.add_argument("--data-dir", default=None,
                       help="default: <base-dir>/store")
    start.add_argument("--redo-dir", default=None)
    start.add_argument("--parameter", action="append", default=None,
                       metavar="KEY=VALUE", help="server parameter (repeatable)")
    start.add_argument("--extra-arg", action="append", default=None,
                       help="extra raw argument (repeatable)")
    start.add_argument("--log-level", default=None)
    start.add_argument("--timeout", type=int, default=180,
                       help="seconds to wait for readiness (default 180)")
    start.add_argument("--password", default="")

    stop = subparsers.add_parser("stop", help="stop the instance")
    add_common(stop, with_binary=False)
    stop.add_argument("--rm", action="store_true",
                      help="also delete the base dir (data included)")

    status = subparsers.add_parser("status", help="report instance state")
    add_common(status, with_binary=False)

    execute = subparsers.add_parser("exec", help="run SQL statements")
    add_common(execute, with_binary=False)
    execute.add_argument("--password", nargs="?", const="", default=None,
                         metavar="PASSWORD",
                         help="password (default: $MYSQL_PWD or empty)")
    execute.add_argument("--database", default=None)
    execute.add_argument("--sql", action="append", default=None,
                         metavar="SQL", help="statement to run (repeatable)")
    execute.add_argument("--timeout", type=int, default=10)
    execute.add_argument("--query-timeout", type=int, default=600)

    dump = subparsers.add_parser("dump-log", help="print the tail of seekdb.log")
    add_common(dump, with_binary=False)
    dump.add_argument("--lines", type=int, default=50)
    return parser


def main(argv=None) -> int:
    parser = build_parser()
    options = parser.parse_args(argv)
    if not options.command:
        parser.print_help()
        return 2
    handlers = {
        "probe": command_probe,
        "start": command_start,
        "stop": command_stop,
        "status": command_status,
        "exec": command_exec,
        "dump-log": command_dump_log,
    }
    handler = handlers[options.command]
    if getattr(options, "password", None) is None:
        options.password = os.environ.get("MYSQL_PWD", "")
    return handler(options)


if __name__ == "__main__":
    sys.exit(main())