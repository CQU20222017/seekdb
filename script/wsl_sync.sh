#!/usr/bin/env bash
# Copy the Windows working tree into the WSL build tree (fast, repeatable) and
# normalize line endings for the files a Linux build actually executes.
#
# The repository stays authoritative on the Windows side; this script mirrors the
# sources that a build needs into ~/seekdb so compilation runs on a native Linux
# filesystem instead of the 9p mount. The Windows checkout uses core.autocrlf=true,
# so shell scripts and the dependency profile arrive with CRLF and must be converted:
# a CRLF shebang makes bash fail with "/usr/bin/env: 'bash\r': No such file".
set -euo pipefail

SRC="${1:-/mnt/d/seekdb-2026}"
DST="${2:-$HOME/seekdb}"

mkdir -p "$DST"

rsync -a --info=stats2 \
  --exclude 'build_release/' \
  --exclude 'build_sanity/' \
  --exclude 'build_rpm/' \
  --exclude 'deps/3rd/' \
  --exclude 'deps/pkg-cache/' \
  --exclude '*.log' \
  "$SRC"/ "$DST"/

cd "$DST"
converted=0
while IFS= read -r -d '' file; do
  if grep -qU $'\r' "$file" 2>/dev/null; then
    sed -i 's/\r$//' "$file"
    converted=$((converted + 1))
  fi
done < <(find . -path ./.git -prune -o -type f \
           \( -name '*.sh' -o -name '*.deps' -o -name '*.py' -o -name '*.inc' \
              -o -name '*.yaml' -o -name '*.yml' -o -name '*.in' \) -print0)

chmod +x build.sh deps/init/dep_create.sh 2>/dev/null || true

echo "--- synced to $DST (normalized $converted files) ---"
du -sh "$DST"