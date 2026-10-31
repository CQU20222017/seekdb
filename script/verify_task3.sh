#!/usr/bin/env bash
# Stage verification for competition task 3 (AI column): sync -> build ->
# instance -> mock -> cases -> summary, in one command.
#
# Run it inside WSL (the seekdb binary and the mock must share one loopback):
#
#   bash /mnt/d/seekdb-2026/script/verify_task3.sh
#   bash /mnt/d/seekdb-2026/script/verify_task3.sh --no-build          # skip compile
#   bash /mnt/d/seekdb-2026/script/verify_task3.sh --case ai_column_ddl --case ai_prompt
#   bash /mnt/d/seekdb-2026/script/verify_task3.sh --keep-instance     # reuse 2881
#   bash /mnt/d/seekdb-2026/script/verify_task3.sh --stop-all          # stop mock+instance at the end
#
# Default cases: the five ai_column cases plus ai_prompt and
# ai_function_enhancement.  Exit code 0 when every case passes, 1 otherwise.
set -uo pipefail

REPO_WIN="${REPO_WIN:-/mnt/d/seekdb-2026}"
REPO_WSL="${REPO_WSL:-$HOME/seekdb}"
PORT="${SEEKDB_PORT:-2881}"
MOCK_PORT="${MOCK_PORT:-8000}"
JOBS="${JOBS:-4}"
CASE_TIMEOUT="${CASE_TIMEOUT:-1800}"

DO_SYNC=1
DO_BUILD=1
KEEP_INSTANCE=0
STOP_ALL=0
MAX_DIFFS=2
VERBOSE=0
BUILD_WAIT="${BUILD_WAIT:-1800}"
CASES=()

DEFAULT_CASES=(
  ai_column_ddl
  ai_column_t1_dml
  ai_column_vector_index
  ai_column_retry_strict
  ai_column_worker
  ai_prompt
  ai_function_enhancement
)

usage() {
  # print only the leading comment block (the doc header)
  awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' \
      "${BASH_SOURCE[0]}"
  echo
  echo "options:"
  echo "  --no-sync           skip the rsync from $REPO_WIN to $REPO_WSL"
  echo "  --no-build          skip 'make -C build_release -j$JOBS seekdb'"
  echo "  --case NAME         run only this case (repeatable)"
  echo "  --keep-instance     reuse an instance already listening on $PORT"
  echo "  --stop-all          stop the mock and the instance we started"
  echo "  --max-diffs N       context blocks shown for a failing case (default $MAX_DIFFS)"
  echo "  --jobs N            compile parallelism (default $JOBS)"
  echo "  --verbose           echo each case's full log path"
  echo "  -h, --help          this text"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --no-sync) DO_SYNC=0 ;;
    --no-build) DO_BUILD=0 ;;
    --case) shift; CASES+=("${1:-}") ;;
    --case=*) CASES+=("${1#*=}") ;;
    --keep-instance) KEEP_INSTANCE=1 ;;
    --stop-all) STOP_ALL=1 ;;
    --max-diffs) shift; MAX_DIFFS="${1:-2}" ;;
    --jobs) shift; JOBS="${1:-4}" ;;
    --verbose) VERBOSE=1 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "[verify][FAIL] args     unknown option: $1" >&2; usage; exit 2 ;;
  esac
  shift
done

if [ ${#CASES[@]} -eq 0 ]; then
  CASES=("${DEFAULT_CASES[@]}")
fi

RUN_ROOT="$(mktemp -d /tmp/seekdb_t3_run.XXXXXX)"
LOGS="$RUN_ROOT/logs"
mkdir -p "$LOGS"

STEP_NAME=""
STEP_T0=0
STEP_FAILED=0
CASE_FAILURES=0
INSTANCE_RESTARTS=0
INSTANCE_STARTED=0
MOCK_STARTED=0
INSTANCE_BASE="$RUN_ROOT/base"
INSTANCE_DATA="$RUN_ROOT/data"

step_begin() { STEP_NAME="$1"; STEP_T0=$SECONDS; }
step_ok() {
  printf '[verify][ok]   %-9s (%ds)%s\n' "$STEP_NAME" $((SECONDS-STEP_T0)) "${1:+  $1}"
}
step_fail() {
  STEP_FAILED=1
  printf '[verify][FAIL] %-9s (%ds)%s\n' "$STEP_NAME" $((SECONDS-STEP_T0)) "${1:+  $1}"
}

port_open() {
  # $1 = port; true when something is listening on the loopback
  (exec 3<>"/dev/tcp/127.0.0.1/$1") >/dev/null 2>&1
}

port_owner_pid() {
  ss -ltnp 2>/dev/null | awk -v p=":$1" '$4 ~ p {print}' \
    | sed -n 's/.*pid=\([0-9]\+\).*/\1/p' | head -1
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

#: every (re)start gets its own base-dir: bootstrap refuses a non-empty data dir
INSTANCE_GEN=0
start_instance() {
  INSTANCE_GEN=$((INSTANCE_GEN + 1))
  INSTANCE_BASE="$RUN_ROOT/base.$INSTANCE_GEN"
  INSTANCE_DATA="$RUN_ROOT/data.$INSTANCE_GEN"
  rm -rf "$INSTANCE_BASE" "$INSTANCE_DATA"
  mkdir -p "$INSTANCE_BASE" "$INSTANCE_DATA"
  python3 tools/competition/seekdb_server.py start \
      --binary "$BINARY" --port "$PORT" \
      --base-dir "$INSTANCE_BASE" --data-dir "$INSTANCE_DATA" \
      --timeout 300 >> "$LOGS/instance.log" 2>&1 || return 1
  port_open "$PORT" || return 1
  return 0
}

echo "task 3 verification"
echo "  repo (wsl) : $REPO_WSL"
echo "  repo (win) : $REPO_WIN"
echo "  seekdb     : 127.0.0.1:$PORT"
echo "  mock       : 127.0.0.1:$MOCK_PORT"
echo "  artifacts  : $RUN_ROOT"
echo "  cases      : ${CASES[*]}"

# ---------------------------------------------------------------------------
# 1) sync
# ---------------------------------------------------------------------------
step_begin sync
if [ "$DO_SYNC" = "1" ]; then
  if [ ! -f "$REPO_WIN/script/wsl_sync.sh" ]; then
    step_fail "missing $REPO_WIN/script/wsl_sync.sh"
  elif bash "$REPO_WIN/script/wsl_sync.sh" > "$LOGS/sync.log" 2>&1; then
    step_ok "$(grep -m1 'synced to' "$LOGS/sync.log" | cut -c1-66)"
  else
    tail -15 "$LOGS/sync.log"
    step_fail "rsync failed, see $LOGS/sync.log"
  fi
else
  step_ok "skipped (--no-sync)"
fi
[ "$STEP_FAILED" = "1" ] && { echo; echo "stopped at step: $STEP_NAME"; exit 1; }

if [ ! -d "$REPO_WSL" ]; then
  echo "[verify][FAIL] repo     $REPO_WSL does not exist" >&2
  exit 1
fi
cd "$REPO_WSL" || exit 1

# ---------------------------------------------------------------------------
# 2) build
# ---------------------------------------------------------------------------
step_begin build
BINARY="$REPO_WSL/build_release/src/observer/seekdb"
if [ "$DO_BUILD" = "1" ]; then
  # never run two makes on one tree: wait for a concurrent build to finish
  waited=0
  while pgrep -f "make -C build_release" >/dev/null 2>&1; do
    if [ "$waited" = "0" ]; then
      echo "[verify] another build is running, waiting for it to finish..."
    fi
    sleep 5
    waited=$((waited + 5))
    if [ "$waited" -ge "$BUILD_WAIT" ]; then
      echo "[verify] gave up waiting after ${BUILD_WAIT}s"
      break
    fi
  done
  make -C build_release "-j$JOBS" seekdb > "$LOGS/build.log" 2>&1
  rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "--- last 30 lines of make ($LOGS/build.log) ---"
    tail -30 "$LOGS/build.log"
    step_fail "make exited with $rc"
  else
    step_ok "$(tail -1 "$LOGS/build.log" | cut -c1-70)"
  fi
else
  step_ok "skipped (--no-build)"
fi
[ "$STEP_FAILED" = "1" ] && { echo; echo "stopped at step: $STEP_NAME"; exit 1; }

if [ ! -x "$BINARY" ]; then
  echo "[verify][FAIL] binary   $BINARY is missing or not executable" >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# 3) instance
# ---------------------------------------------------------------------------
step_begin instance
if port_open "$PORT"; then
  if [ "$KEEP_INSTANCE" = "1" ]; then
    step_ok "reusing the instance already listening on $PORT"
  else
    pid="$(port_owner_pid "$PORT")"
    echo "[verify] port $PORT is busy (pid ${pid:-?}), restarting with a fresh base-dir"
    echo "[verify]   pass --keep-instance to reuse it instead"
    if [ -n "${pid:-}" ]; then
      kill -TERM "$pid" 2>/dev/null
      for _ in $(seq 1 200); do
        port_open "$PORT" || break
        sleep 0.1
      done
      port_open "$PORT" && kill -KILL "$pid" 2>/dev/null
      for _ in $(seq 1 100); do
        port_open "$PORT" || break
        sleep 0.1
      done
    fi
    if port_open "$PORT"; then
      step_fail "could not free port $PORT"
    fi
  fi
fi

if [ "$STEP_FAILED" = "0" ] && ! port_open "$PORT"; then
  if start_instance; then
    INSTANCE_STARTED=1
    step_ok "$(grep -m1 'ready in' "$LOGS/instance.log" | tail -1 | cut -c1-70)"
  else
    tail -25 "$LOGS/instance.log"
    step_fail "seekdb did not become ready, see $LOGS/instance.log"
  fi
fi
[ "$STEP_FAILED" = "1" ] && { echo; echo "stopped at step: $STEP_NAME"; exit 1; }

# ---------------------------------------------------------------------------
# 4) mock
# ---------------------------------------------------------------------------
step_begin mock
if port_open "$MOCK_PORT"; then
  step_ok "reusing the mock already listening on $MOCK_PORT"
else
  python3 tools/competition/ai_function_mock.py start "$MOCK_PORT" \
      > "$LOGS/mock.log" 2>&1
  if [ $? -ne 0 ] || ! port_open "$MOCK_PORT"; then
    tail -15 "$LOGS/mock.log"
    step_fail "mock did not start on $MOCK_PORT, see $LOGS/mock.log"
  else
    MOCK_STARTED=1
    step_ok "$(grep -m1 'started on' "$LOGS/mock.log" | cut -c1-70)"
  fi
fi
[ "$STEP_FAILED" = "1" ] && { echo; echo "stopped at step: $STEP_NAME"; exit 1; }
# only count the requests of this run
python3 tools/competition/ai_function_mock.py reset "$MOCK_PORT" >/dev/null 2>&1

# ---------------------------------------------------------------------------
# 5) cases
# ---------------------------------------------------------------------------
step_begin cases
suite_of() {
  # the suite is decided by where the .test lives
  if [ -f "$REPO_WSL/tools/deploy/mysql_test/test_suite/ai_column/t/$1.test" ]; then
    echo ai_column
  else
    echo ai_function
  fi
}

for case_name in "${CASES[@]}"; do
  case_suite="$(suite_of "$case_name")"
  json="$LOGS/$case_name.json"
  log="$LOGS/$case_name.log"
  rm -f "$json"
  printf '[verify] running %-28s (suite %s)\n' "$case_name" "$case_suite"
  timeout "$CASE_TIMEOUT" python3 tools/competition/run_ai_function_tests.py \
      --host 127.0.0.1 --port "$PORT" --user root --password= \
      --database test --suite "$case_suite" --case "$case_name" \
      --max-diffs "$MAX_DIFFS" --keep-going \
      --summary-json "$json" > "$log" 2>&1
  rc=$?
  if [ "$rc" = "0" ]; then
    case_state="PASS"
  else
    case_state="FAIL"
    CASE_FAILURES=$((CASE_FAILURES + 1))
  fi
  printf '[verify]   %-28s %s (exit %d, log %s)\n' \
      "$case_name" "$case_state" "$rc" "$log"
  if [ "$rc" != "0" ]; then
    echo "--- $case_name: first differences (--max-diffs $MAX_DIFFS) ---"
    if grep -q '^  --- diff ' "$log"; then
      sed -n '/^  --- diff /,$p' "$log" | head -60
    elif grep -q '^\[error\]' "$log"; then
      head -4 "$log"
    else
      grep -E '^  (first difference|note:|[0-9]+ statement)' "$log" | head -20
    fi
    echo "--- end of $case_name differences ---"
  fi
  [ "$VERBOSE" = "1" ] && { echo "--- full log: $log ---"; cat "$log"; }

  # someone (another pipeline, a crash) may have taken the instance down
  if ! port_open "$PORT"; then
    echo "[verify] WARN instance on $PORT is gone after $case_name, restarting it"
    if start_instance; then
      INSTANCE_STARTED=1
      INSTANCE_RESTARTS=$((INSTANCE_RESTARTS + 1))
      echo "[verify]   restarted: $(grep -m1 'ready in' "$LOGS/instance.log" | tail -1 | cut -c1-80)"
    else
      echo "[verify]   restart failed, the remaining cases will fail" 
      tail -10 "$LOGS/instance.log"
    fi
  fi
done

if [ "$CASE_FAILURES" -gt 0 ]; then
  step_fail "$CASE_FAILURES of ${#CASES[@]} case(s) differ from the baseline"
else
  step_ok "${#CASES[@]} case(s) passed"
fi

# ---------------------------------------------------------------------------
# 6) summary
# ---------------------------------------------------------------------------
step_begin summary
INSTANCE_RESTARTS="$INSTANCE_RESTARTS" python3 - "$LOGS" "$MOCK_PORT" "${CASES[@]}" <<'PY'
import json
import os
import sys
from pathlib import Path

log_dir = Path(sys.argv[1])
mock_port = sys.argv[2]

entries = []
for path in sorted(log_dir.glob("*.json")):
    try:
        entries.append(json.loads(path.read_text(encoding="utf-8")))
    except (OSError, ValueError):
        continue

reported = {}
for payload in entries:
    for case in payload.get("cases", []):
        reported[case.get("name")] = case

# every requested case gets a row, even when the runner never reported one
# (instance gone, timeout, bad arguments, ...)
for name in sys.argv[3:]:
    if name not in reported:
        log_path = log_dir / (name + ".log")
        reason = "does not exist"
        if log_path.exists():
            for line in log_path.read_text(encoding="utf-8",
                                          errors="replace").splitlines():
                if line.startswith("[error]"):
                    reason = line[8:].strip()
                    break
            else:
                reason = "no result reported (see %s)" % log_path.name
        reported[name] = {"name": name, "status": "FAIL",
                          "first_diff": reason[:200]}

cases = [reported[name] for name in sys.argv[3:] if name in reported]
for name in sorted(reported):
    if name not in sys.argv[3:]:
        cases.append(reported[name])

print("=== task3 verification summary ===")
width = max([len(case.get("name", "")) for case in cases] + [26])
passed = failed = skipped = 0
for case in cases:
    name = case.get("name", "?")
    status = case.get("status", "?")
    if status == "PASS":
        passed += 1
        print("%-*s %-4s  %d stmts / %d/%d lines (%d indent-only)"
              % (width, name, status, case.get("statements", 0),
                 case.get("identical", 0) + case.get("indent_only", 0),
                 case.get("expected_lines", 0), case.get("indent_only", 0)))
    elif status == "SKIPPED":
        skipped += 1
        print("%-*s %-4s  not run" % (width, name, status))
    else:
        failed += 1
        print("%-*s %-4s  first diff: %s"
              % (width, name, status, case.get("first_diff") or "differs"))

# mock request totals for this run
events = Path("/tmp/seekdb_ai_function_mock_%s/events.jsonl" % mock_port)
counting = {"chat": 0, "embeddings": 0, "generate": 0, "embed": 0,
            "rerank": 0, "other": 0}
total = 0
if events.exists():
    for line in events.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            event = json.loads(line)
        except ValueError:
            continue
        path = str(event.get("path", ""))
        if path.startswith("/__control/"):
            continue
        total += 1
        if path.endswith("/chat/completions"):
            counting["chat"] += 1
        elif path.endswith("/embeddings"):
            counting["embeddings"] += 1
        elif path.endswith("/api/generate"):
            counting["generate"] += 1
        elif path.endswith("/api/embed"):
            counting["embed"] += 1
        elif path.endswith("/rerank"):
            counting["rerank"] += 1
        else:
            counting["other"] += 1
detail = ", ".join("%s %d" % (key, value)
                   for key, value in counting.items() if value)
print("mock: %d requests (%s)" % (total, detail or "none"))

print()
print("OVERALL: %dPASS / %dFAIL%s"
      % (passed, failed, "" if not skipped else " / %dSKIPPED" % skipped))
restarts = int(os.environ.get("INSTANCE_RESTARTS", "0") or 0)
if restarts:
    print("note: the seekdb instance was restarted %d time(s) during this run"
          % restarts)
sys.exit(0 if failed == 0 and passed > 0 else 1)
PY
summary_rc=$?

if [ "$summary_rc" -eq 0 ] && [ "$CASE_FAILURES" -eq 0 ]; then
  step_ok "all cases passed"
  overall=0
else
  step_fail "at least one case differs from the baseline"
  overall=1
fi

# ---------------------------------------------------------------------------
# 7) how to clean up (the mock and the instance are kept on purpose)
# ---------------------------------------------------------------------------
echo
if [ "$STOP_ALL" = "1" ]; then
  if [ "$MOCK_STARTED" = "1" ] || port_open "$MOCK_PORT"; then
    python3 tools/competition/ai_function_mock.py stop "$MOCK_PORT" >/dev/null 2>&1
    echo "[verify] mock on $MOCK_PORT stopped (--stop-all)"
  fi
  if [ "$INSTANCE_STARTED" = "1" ]; then
    python3 tools/competition/seekdb_server.py stop --port "$PORT" \
        --base-dir "$INSTANCE_BASE" --rm >/dev/null 2>&1
    echo "[verify] instance on $PORT stopped and its base-dir removed (--stop-all)"
  fi
else
  echo "[verify] mock left running on $MOCK_PORT (reused by the next run)"
  echo "[verify]   stop it:  python3 tools/competition/ai_function_mock.py stop $MOCK_PORT"
  if [ "$INSTANCE_STARTED" = "1" ]; then
    echo "[verify] instance left running on $PORT, base-dir $INSTANCE_BASE"
    echo "[verify]   stop it:  python3 tools/competition/seekdb_server.py stop --port $PORT --base-dir $INSTANCE_BASE --rm"
    echo "[verify]   logs:      $INSTANCE_BASE/seekdb.log"
  else
    echo "[verify] instance reused, not owned by this run (left untouched)"
  fi
fi
echo "[verify] artifacts: $RUN_ROOT"
exit "$overall"