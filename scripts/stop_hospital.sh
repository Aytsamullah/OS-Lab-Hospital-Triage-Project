#!/usr/bin/env bash
# ============================================================
# Project : Hospital Patient Triage & Bed Allocator
# Script  : stop_hospital.sh
# Members : Member 1 (24F-0569), Member 2 (24F-0563)
# Purpose : Stop admissions and remove IPC files.
# Usage   : ./scripts/stop_hospital.sh
# ============================================================

set -euo pipefail

PID_FILE="/tmp/hospital_admissions.pid"

if [[ -f "$PID_FILE" ]]; then
  PID="$(cat "$PID_FILE")"
  kill -TERM "$PID" 2>/dev/null || true
  sleep 1
fi

[[ -p /tmp/triage_fifo ]] && timeout 1 bash -c "echo SHUTDOWN > /tmp/triage_fifo" 2>/dev/null || true

rm -f /tmp/triage_fifo /tmp/discharge_fifo /tmp/hospital_admissions.pid
ipcrm -M 0xBEDF00D 2>/dev/null || true
rm -f /dev/shm/sem.sem_icu_limit /dev/shm/sem.sem_iso_limit 2>/dev/null || true

echo "Hospital stopped and IPC resources cleaned."
[[ -f logs/hospital_summary.txt ]] && cat logs/hospital_summary.txt
