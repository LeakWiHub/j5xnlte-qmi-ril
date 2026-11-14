#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Mobile-data experiment: tries the HAL's data-setup variants one after another (data.conf knobs), toggles mobile
# data, pings through wwan0 and records modem/kernel counters. Result: /sdcard/j5-datatest.txt
#   sh j5x-datatest.sh            (root shell; the HAL must be running; needs a SIM with mobile data enabled in Android)
unset LD_PRELOAD LD_LIBRARY_PATH
export PATH=/vendor/bin:/system/bin:/system/xbin:$PATH
MODDIR=${0%/*}
ST=/data/local/tmp/j5ril-state; CONF=$ST/data.conf
OUT=${J5_OUT:-/sdcard/j5-datatest.txt}
LOGF=$MODDIR/j5ril.log
mkdir -p "$ST"
: > "$OUT"
say() { echo "$*" | tee -a "$OUT"; }
# kernel side: per-packet driver messages (channel open, "Data for inactive channel", unsupported channel, wakeups)
[ -d /sys/kernel/debug/dynamic_debug ] || mount -t debugfs none /sys/kernel/debug 2>/dev/null
echo 'module qcom_bam_dmux +p' > /sys/kernel/debug/dynamic_debug/control 2>/dev/null && say "bam_dmux dyndbg on" || say "bam_dmux dyndbg unavailable"
for combo in "on none" "off none" "on mux1" "off mux1"; do
  set -- $combo
  say ""; say "=================== wda=$1 bind=$2 ==================="
  printf 'wda=%s\nbind=%s\n' "$1" "$2" > "$CONF"
  svc data disable; sleep 4
  dmesg -c >/dev/null 2>&1
  svc data enable; sleep 25
  say "--- HAL log (tail)"; tail -12 "$LOGF" | grep -iE "knobs|WDA|bind|data|call" | tail -8 | tee -a "$OUT" >/dev/null
  say "--- wwan0"; ip -4 addr show wwan0 2>&1 | grep -E "inet|state|wwan0" | tee -a "$OUT"
  say "--- counters before"; grep wwan0 /proc/net/dev | tee -a "$OUT"
  say "--- ping 8.8.8.8 via wwan0"; ping -I wwan0 -c 4 -W 3 8.8.8.8 2>&1 | tail -4 | tee -a "$OUT"
  say "--- counters after"; grep wwan0 /proc/net/dev | tee -a "$OUT"
  say "--- dmesg bam/dmux"; dmesg | grep -iE "bam|dmux" | tail -15 | tee -a "$OUT"
done
rm -f "$CONF"
say ""; say "done; data.conf removed (HAL defaults restored). File: $OUT"
