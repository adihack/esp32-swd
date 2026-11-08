#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# r5 board test runner (MicroPython). Usage:
#   ./run.sh deploy            copy device/*.py to the board, then hard-reset it (fresh interpreter)
#   ./run.sh diag [A|B]        run the staged SWD diagnostics, log to logs/
#   ./run.sh levels [A|B]      print current EN states / line levels / VTref ADC
#   ./run.sh reset             hard reset (machine.reset), wait for the port to come back
#   ./run.sh repl              interactive REPL (Ctrl-] to leave)
#
# Never soft-reset this board: MicroPython's soft reset hangs the ESP32-S3 native USB here (twice on
# 2026-11-06, once while rebooting, once right after). Every command therefore runs with mpremote
# "resume" (no soft reset on connect); a fresh interpreter comes from a hard reset instead.
set -euo pipefail
cd "$(dirname "$0")"
PORT="${PORT:-/dev/ttyACM0}"
MPR="${MPREMOTE:-../../.venv/bin/mpremote}"
mp() { "$MPR" connect "$PORT" resume "$@"; }

hard_reset() {
  mp exec "import machine; machine.reset()" >/dev/null 2>&1 || true   # port vanishes mid-command
  sleep 2
  for _ in $(seq 1 40); do
    [ -e "$PORT" ] && { sleep 1; return 0; }
    sleep 0.25
  done
  echo "board did not come back on $PORT" >&2; return 1
}

case "${1:-}" in
  deploy)
    mp fs cp device/*.py :
    hard_reset
    ;;
  diag)
    ch="${2:-A}"
    log="logs/diag_${ch}_$(date +%Y%m%d_%H%M%S).log"
    mp exec "import diag; diag.run('$ch')" | tee "$log"
    echo "log: $log"
    ;;
  levels)
    mp exec "import diag; diag.levels('${2:-A}')"
    ;;
  reset)
    hard_reset
    ;;
  repl)
    mp repl
    ;;
  *)
    sed -n '2,7p' "$0"; exit 1
    ;;
esac
