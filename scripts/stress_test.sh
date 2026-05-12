#!/usr/bin/env bash
# ============================================================
# Project : Hospital Patient Triage & Bed Allocator
# Script  : stress_test.sh
# Members : Member 1 (24F-0569), Member 2 (24F-0563)
# Purpose : Generate and send patient arrivals quickly.
# Usage   : ./scripts/stress_test.sh [count]
# ============================================================

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

COUNT="${1:-20}"

if ! [[ "$COUNT" =~ ^[0-9]+$ ]] || (( COUNT < 1 )); then
  echo "Usage: $0 [positive patient count]" >&2
  exit 1
fi

for ((i = 1; i <= COUNT; i++)); do
  NAME="Patient_${i}"
  AGE=$((18 + (i * 7) % 65))
  SEVERITY=$((1 + (i * 3) % 10))
  if (( i % 4 == 0 )); then
    INFECTIOUS="yes"
  else
    INFECTIOUS="no"
  fi
  ./scripts/triage.sh "$NAME" "$AGE" "$SEVERITY" "$INFECTIOUS" &
  sleep 0.1
done

wait
echo "${COUNT}-patient stress test submitted."
