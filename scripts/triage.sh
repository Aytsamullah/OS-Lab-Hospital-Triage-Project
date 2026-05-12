#!/usr/bin/env bash
# ============================================================
# Project : Hospital Patient Triage & Bed Allocator
# Script  : triage.sh
# Members : Member 1 (24F-0569), Member 2 (24F-0563)
# Purpose : Validate patient input and send it to admissions.
# Usage   : ./scripts/triage.sh <name> <age> <severity 1-10> [yes|no] [--stdout]
# ============================================================

set -euo pipefail

FIFO="/tmp/triage_fifo"
OUTPUT_MODE="fifo"

if [[ $# -gt 0 && "${!#}" == "--stdout" ]]; then
  OUTPUT_MODE="stdout"
  set -- "${@:1:$(($# - 1))}"
fi

if [[ $# -eq 0 ]]; then
  read -r -p "Patient name: " NAME
  read -r -p "Age: " AGE
  read -r -p "Severity (1-10): " SEVERITY
  read -r -p "Infectious? (yes/no): " INFECTIOUS
else
  NAME="${1:-}"
  AGE="${2:-}"
  SEVERITY="${3:-}"
  INFECTIOUS="${4:-no}"
fi

if [[ $# -lt 3 || $# -gt 4 ]]; then
  if [[ $# -ne 0 ]]; then
    echo "Usage: $0 <name> <age> <severity 1-10> [yes|no] [--stdout]" >&2
    echo "Or run $0 without arguments for interactive input." >&2
    exit 1
  fi
fi

if [[ -z "$NAME" || "$NAME" == *"|"* ]]; then
  echo "Invalid name" >&2
  exit 1
fi

if ! [[ "$AGE" =~ ^[0-9]+$ ]] || (( AGE < 1 || AGE > 130 )); then
  echo "Age must be 1-130" >&2
  exit 1
fi

if ! [[ "$SEVERITY" =~ ^[0-9]+$ ]] || (( SEVERITY < 1 || SEVERITY > 10 )); then
  echo "Severity must be 1-10" >&2
  exit 1
fi

case "${INFECTIOUS,,}" in
  yes|y|1|true) INFECTIOUS_FLAG=1 ;;
  no|n|0|false) INFECTIOUS_FLAG=0 ;;
  *)
    echo "Infectious flag must be yes/no" >&2
    exit 1
    ;;
esac

if (( SEVERITY >= 9 )); then
  PRIORITY=1
elif (( SEVERITY >= 7 )); then
  PRIORITY=2
elif (( SEVERITY >= 5 )); then
  PRIORITY=3
elif (( SEVERITY >= 3 )); then
  PRIORITY=4
else
  PRIORITY=5
fi

if (( PRIORITY <= 2 )); then
  CARE_UNITS=3
elif (( PRIORITY == 3 || INFECTIOUS_FLAG == 1 )); then
  CARE_UNITS=2
else
  CARE_UNITS=1
fi

RECORD="${NAME}|${AGE}|${SEVERITY}|${PRIORITY}|${CARE_UNITS}|${INFECTIOUS_FLAG}"

if [[ "$OUTPUT_MODE" == "stdout" ]]; then
  echo "$RECORD"
  exit 0
fi

if [[ ! -p "$FIFO" ]]; then
  echo "Admissions is not running. Run ./scripts/start_hospital.sh first." >&2
  exit 1
fi

echo "$RECORD" > "$FIFO"
echo "Submitted: $NAME priority=$PRIORITY care_units=$CARE_UNITS"
