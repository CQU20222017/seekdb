#!/usr/bin/env bash
# Incremental rebuild + restart the local instance after a source change.
set -uo pipefail
export PATH="$HOME/.cargo/bin:$PATH"

echo "########## sync ##########"
bash /mnt/d/seekdb-2026/script/wsl_sync.sh >/dev/null || { echo "[FAIL] sync"; exit 2; }

echo "########## incremental build ##########"
cd "$HOME/seekdb" || exit 3
make -C build_release -j4 seekdb 2>&1 | tail -n 25
status=${PIPESTATUS[0]}
echo "=== make exit=${status} ==="
if [ "$status" -ne 0 ]; then
  echo "[FAIL] build failed"
  exit "$status"
fi
ls -l build_release/src/observer/seekdb

echo "########## restart instance ##########"
pkill -f "observer/seekdb --nodaemon" 2>/dev/null
sleep 3
bash /mnt/d/seekdb-2026/script/wsl_start_instance.sh
exit 0