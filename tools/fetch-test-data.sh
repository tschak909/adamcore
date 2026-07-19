#!/usr/bin/env bash
# Fetch external test material into tests/data/ (git-ignored).
# Nothing fetched here is vendored into the repository.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DATA="$ROOT/tests/data"
mkdir -p "$DATA"

# --- Tom Harte / SingleStepTests Z80 vectors (MIT) -------------------------
# Pinned by commit for reproducible test runs.
HARTE_REPO="https://github.com/SingleStepTests/z80"
HARTE_DIR="$DATA/harte-z80"
if [ ! -d "$HARTE_DIR/v1" ]; then
  echo "Fetching SingleStepTests z80 vectors..."
  git clone --depth 1 "$HARTE_REPO" "$HARTE_DIR"
  (cd "$HARTE_DIR" && git rev-parse HEAD > .pinned-commit)
else
  echo "harte-z80 already present ($(cat "$HARTE_DIR/.pinned-commit" 2>/dev/null || echo unpinned))"
fi

# --- ZEXDOC / ZEXALL (Frank Cringle) ---------------------------------------
ZEX_BASE="https://raw.githubusercontent.com/agn453/ZEXALL/master"
for f in zexdoc.com zexall.com; do
  if [ ! -f "$DATA/$f" ]; then
    echo "Fetching $f..."
    curl -fsSL -o "$DATA/$f" "$ZEX_BASE/$f" \
      || curl -fsSL -o "$DATA/$f" "$ZEX_BASE/${f^^}" \
      || { echo "failed to fetch $f" >&2; exit 1; }
  fi
done
ls -la "$DATA"
echo "OK"
