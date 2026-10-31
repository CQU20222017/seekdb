#!/usr/bin/env bash
# Start a fresh local seekdb instance plus the mock AI service.
#
# A first-time start must not reuse a base dir: bootstrap refuses to run when the
# data_version file or the local slog directory already exists ("this observer is not
# empty"), so the whole base dir is recreated here.
set -uo pipefail

RUN_DIR="$HOME/seekdb_run"
BIN="$HOME/seekdb/build_release/src/observer/seekdb"
LOG="$RUN_DIR/seekdb.log"
MOCK_LOG="$RUN_DIR/mock8000.log"
PORT="${PORT:-2881}"

port_open() {
  python3 - "$1" <<'PY'
import socket, sys
s = socket.socket(); s.settimeout(1)
try:
    s.connect(("127.0.0.1", int(sys.argv[1]))); sys.exit(0)
except OSError:
    sys.exit(1)
PY
}

if port_open "$PORT"; then
  echo "seekdb already listening on $PORT"
else
  echo "=== fresh base dir and start on port $PORT ==="
  rm -rf "$RUN_DIR"
  mkdir -p "$RUN_DIR"
  nohup "$BIN" --nodaemon --port "$PORT" --base-dir "$RUN_DIR" --data-dir "$RUN_DIR/store" \
      > "$LOG" 2>&1 &
  echo $! > "$RUN_DIR/seekdb.pid"
  echo "pid=$(cat "$RUN_DIR/seekdb.pid")"
fi

mkdir -p "$RUN_DIR"
MOCK="$HOME/seekdb/tools/competition/ai_function_mock.py"
if [ -f "$MOCK" ] && ! port_open 8000; then
  echo "=== starting mock AI service on 8000 ==="
  nohup python3 "$MOCK" start 8000 > "$MOCK_LOG" 2>&1 &
  sleep 2
  tail -n 2 "$MOCK_LOG" 2>/dev/null
fi

echo "=== waiting for SQL layer (max 180s) ==="
for i in $(seq 1 90); do
  if python3 - "$PORT" <<'PY'
import sys
try:
    import pymysql
except ImportError:
    sys.exit(2)
try:
    conn = pymysql.connect(host="127.0.0.1", port=int(sys.argv[1]), user="root", password="",
                           connect_timeout=3, read_timeout=5, write_timeout=5)
    with conn.cursor() as cur:
        cur.execute("select 1")
        cur.fetchone()
    conn.close()
    sys.exit(0)
except Exception:
    sys.exit(1)
PY
  then
    echo "SQL ready after ~$((i * 2))s"
    break
  fi
  sleep 2
done

python3 - "$PORT" <<'PY'
import sys
try:
    import pymysql
except ImportError:
    print("pymysql MISSING"); sys.exit(0)
try:
    conn = pymysql.connect(host="127.0.0.1", port=int(sys.argv[1]), user="root", password="",
                           connect_timeout=3, read_timeout=10, write_timeout=5)
    with conn.cursor() as cur:
        cur.execute("select version()")
        print("version:", cur.fetchone())
    conn.close()
except Exception as exc:
    print("SQL probe failed:", exc)
PY
exit 0