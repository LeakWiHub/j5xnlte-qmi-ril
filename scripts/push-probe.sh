#!/bin/bash
# Pushes the prebuilt probe to the phone and runs it as root. Needs adb + root (Magisk su).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$ROOT/prebuilt/qmi-probe-aarch64}"
LOG="probe-$(date +%Y%m%d-%H%M%S).log"
adb push "$BIN" /data/local/tmp/qmi-probe
adb shell chmod 755 /data/local/tmp/qmi-probe
adb shell 'su -c "/data/local/tmp/qmi-probe"' | tee "$LOG"
echo "saved to $LOG - send it back"
