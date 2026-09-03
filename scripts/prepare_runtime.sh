#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

mkdir -p \
  "$ROOT/.vntrader" \
  "$ROOT/vnpy_quickfix_gateway/runtime/client_log" \
  "$ROOT/vnpy_quickfix_gateway/runtime/client_store" \
  "$ROOT/vnpy_quickfix_gateway/runtime/server_log" \
  "$ROOT/vnpy_quickfix_gateway/runtime/server_store"

printf 'QuickFIX runtime directories prepared under %s\n' \
  "$ROOT/vnpy_quickfix_gateway/runtime"
