#!/usr/bin/env bash
# Full desktop test gate: Harte Z80 vectors, ZEXDOC/ZEXALL, scripted BoIP peer.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
cmake -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j >/dev/null
[ -d tests/data/harte-z80/v1 ] || tools/fetch-test-data.sh
./build/harte_runner tests/data/harte-z80
./build/zex_runner tests/data/zexdoc.com
./build/zex_runner tests/data/zexall.com
./build/boip_fake_fujinet
echo "ALL TEST GATES PASSED"
