#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

"$ROOT/scripts/prepare_runtime.sh"

BINARY="$ROOT/QuickFIX/lib/ordermatch"
CONFIG="$ROOT/vnpy_quickfix_gateway/configs/ordermatch_latest_local.cfg"

if [[ ! -x "$BINARY" ]]; then
  printf 'Missing %s; build QuickFIX as described in README.md first.\n' "$BINARY" >&2
  exit 1
fi

exec "$BINARY" "$CONFIG"

