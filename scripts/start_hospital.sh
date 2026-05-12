#!/usr/bin/env bash
# ============================================================
# Project : Hospital Patient Triage & Bed Allocator
# Script  : start_hospital.sh
# Members : Member 1 (24F-0569), Member 2 (24F-0563)
# Purpose : Build and start admissions in background.
# Usage   : ./scripts/start_hospital.sh [best|first|worst]
# ============================================================

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

STRATEGY="${1:-best}"
PID_FILE="/tmp/hospital_admissions.pid"

case "$STRATEGY" in
  best|first|worst) ;;
  *) echo "Use strategy: best, first, or worst" >&2; exit 1 ;;
esac

make all
mkdir -p logs

if [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
  echo "Hospital already running with PID $(cat "$PID_FILE")"
  exit 0
fi

./bin/admissions --strategy "$STRATEGY" > logs/hospital_output.txt 2>&1 &
PID=$!
echo "$PID" > "$PID_FILE"
sleep 1

echo "============================================================"
echo "Hospital Patient Triage & Bed Allocator"
echo "Ward capacity: ICU=4, Isolation=4, General=12, Total units=32"
echo "Allocation strategy: $STRATEGY"
echo "============================================================"
echo "Hospital started with PID $PID"
echo "Run: ./scripts/triage.sh \"Ali\" 30 9 no"
