#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

"$ROOT/scripts/prepare_runtime.sh"
exec python -m vnpy_acrl_execution.run_trader "$@"

