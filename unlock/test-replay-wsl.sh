#!/usr/bin/env bash
# Replay a profiler REPORT.TXT through the real unlock checks, on the host.
# No hardware access, no EFI: it compiles unlock/checks.c for Linux and feeds it
# the exact bytes a read-only profiler run captured on the target machine.
#
# Usage:
#   bash unlock/test-replay-wsl.sh evidence/RUN00-REPORT.TXT
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CC="${CC:-clang}"
command -v "$CC" >/dev/null 2>&1 || CC=/usr/lib/llvm-18/bin/clang

REPORT="${1:-$ROOT/evidence/RUN00-REPORT.TXT}"
[ -f "$REPORT" ] || { echo "usage: $0 REPORT.TXT (missing: $REPORT)" >&2; exit 2; }

mkdir -p "$ROOT/build"
"$CC" -std=c11 -O2 -Wall -Wextra -Wno-macro-redefined -fshort-wchar \
  -I "$ROOT/unlock" \
  -I "$ROOT/deps/edk2/MdePkg/Include" \
  -I "$ROOT/deps/edk2/MdePkg/Include/X64" \
  "$ROOT/unlock/tests/replay_report.c" "$ROOT/unlock/checks.c" \
  -o "$ROOT/build/replay_report"

exec "$ROOT/build/replay_report" "$REPORT"
