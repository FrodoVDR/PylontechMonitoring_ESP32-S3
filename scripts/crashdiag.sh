#!/usr/bin/env zsh
set -euo pipefail

HOST="${1:-192.168.88.20}"
BASE="http://${HOST}"

log_data="$(curl -fsS "${BASE}/api/log")"
log_level="$(curl -fsS "${BASE}/api/log/level" 2>/dev/null || true)"
log_monitoring="$(curl -fsS "${BASE}/api/monitoring")"

print_section() {
  local title="$1"
  echo
  echo "=== ${title} ==="
}

echo "Crash-Diagnose fuer ${HOST}"

print_section "Syslog Status"
if [[ -n "${log_level}" ]]; then
  echo "${log_level}" | tr -d '\r'
else
  echo "Konnte /api/log/level nicht lesen"
fi

print_section "Monitoring Status"
if [[ -n "${log_monitoring}" ]]; then
  echo "${log_monitoring}" | tr -d '\r'
else
  echo "Konnte /api/log/level nicht lesen"
fi

print_section "Reset / Crash Marker"
echo "${log_data}" | grep -E "RESET REASON|LAST CRASH STAGE|LAST CRASH SNAPSHOT|crashes=|PANIC|WATCHDOG|Guru Meditation|Backtrace|abort\(" || \
  echo "Keine Crash-Marker im aktuellen Logpuffer gefunden"

print_section "Letzte 40 Zeilen"
echo "${log_data}" | tail -40

print_section "Scheduler/UART Telemetrie"
last_diag="$(echo "${log_data}" | grep "Diag: q=" | tail -1 || true)"
if [[ -n "${last_diag}" ]]; then
  echo "${last_diag}"
else
  echo "Keine Diag-Zeile gefunden"
fi

print_section "Auffaellige UART/BAT Warnungen (letzte 20)"
echo "${log_data}" | grep -E "UART failed|incomplete frame|parser rejected frame|retry limit reached" | tail -20 || \
  echo "Keine auffaelligen UART/BAT Warnungen gefunden"

echo
