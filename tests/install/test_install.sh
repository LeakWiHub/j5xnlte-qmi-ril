#!/bin/bash
# Tests install/enable/rollback logic against a fake device root + fake adb. This checks the SCRIPT LOGIC only;
# it says nothing about init/SELinux behaviour on a real phone.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
export ADB="$ROOT/tests/install/fake_adb.sh"
pass=0; fail=0
ok() { if eval "$2"; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL: $1"; fi; }

mkdev() { # fresh fake device with an old RIL running and holding the node
  export J5_ROOT="$W/dev$1"; rm -rf "$J5_ROOT"
  mkdir -p "$J5_ROOT"/{dev,props,vendor/etc/init,vendor/etc/vintf/manifest,vendor/bin/hw,proc/123/fd,data/local/tmp}
  : >"$J5_ROOT/dev/wwan0qmi0"; touch "$J5_ROOT/policy_ok"
  printf running >"$J5_ROOT/props/init.svc.vendor.ril-daemon"
  printf running >"$J5_ROOT/props/init.svc.zygote"
  ln -s /dev/wwan0qmi0 "$J5_ROOT/proc/123/fd/5"; echo vendor.ril-daemon >"$J5_ROOT/proc/123/svc"; printf 'rild\0' >"$J5_ROOT/proc/123/cmdline"
  printf '# ueventd\n/dev/other 0660 system system\n' >"$J5_ROOT/vendor/etc/ueventd.rc"
  echo "orig" >"$J5_ROOT/vendor/etc/init/old.rc"
  cp "$J5_ROOT/vendor/etc/ueventd.rc" "$W/ueventd.orig$1"
  printf 'ELF' >"$W/bin"
}
inst="$ROOT/scripts/install.sh"
two_rils() { # 0 if at any point both were started without the old one stopped in between
  awk '/^stop vendor.ril-daemon/{old=0} /^start vendor.ril-daemon/{old=1} /^start vendor.j5xnlte-ril/{if(old) bad=1} /^stop vendor.j5xnlte-ril/{} END{exit bad?0:1}' "$J5_ROOT/svc.log"; }

echo "== 1. happy path"
mkdev 1
$inst --preflight >"$W/o" 2>&1; ok "preflight ok" "[ $? = 0 ]"
ok "preflight lists the holder" "grep -q '123 rild' $W/o"
ok "preflight lists old candidate" "grep -q 'vendor.ril-daemon' $W/o"
$inst --install --binary "$W/bin" --stop-old "vendor.ril-daemon" >"$W/o" 2>&1; ok "install ok" "[ $? = 0 ]"
ok "binary installed" "[ -f $J5_ROOT/vendor/bin/hw/android.hardware.radio-service.j5xnlte-qmi ]"
ok "rc generated with stop line" "grep -q '    stop vendor.ril-daemon' $J5_ROOT/vendor/etc/init/init.j5xnlte-ril.rc"
ok "rc generated with start line" "grep -q '    start vendor.ril-daemon' $J5_ROOT/vendor/etc/init/init.j5xnlte-ril.rc"
ok "no placeholders left" "! grep -q '@' $J5_ROOT/vendor/etc/init/init.j5xnlte-ril.rc"
ok "vintf installed" "[ -f $J5_ROOT/vendor/etc/vintf/manifest/android.hardware.radio.j5xnlte-qmi.xml ]"
ok "ueventd got the node rule" "grep -q '/dev/wwan0qmi0 *0660 *radio *radio' $J5_ROOT/vendor/etc/ueventd.rc"
ok "ueventd: no 0666 anywhere" "! grep -q 0666 $J5_ROOT/vendor/etc/ueventd.rc"
ok "install did not switch" "[ \"\$(cat $J5_ROOT/props/init.svc.vendor.ril-daemon)\" = running ] && [ ! -e $J5_ROOT/svc.log ]"
$inst --install --binary "$W/bin" >"$W/o" 2>&1; ok "second install refused" "[ $? != 0 ]"
$inst --enable >"$W/o" 2>&1; ok "enable ok" "[ $? = 0 ]"
ok "old stopped" "[ \"\$(cat $J5_ROOT/props/init.svc.vendor.ril-daemon)\" = stopped ]"
ok "ours running" "[ \"\$(cat $J5_ROOT/props/init.svc.vendor.j5xnlte-ril)\" = running ]"
ok "old stopped before ours started" "[ \"\$(grep -n 'stop vendor.ril-daemon' $J5_ROOT/svc.log | head -1 | cut -d: -f1)\" -lt \"\$(grep -n 'start vendor.j5xnlte-ril' $J5_ROOT/svc.log | head -1 | cut -d: -f1)\" ]"
$inst --status >"$W/o" 2>&1; ok "status shows installed" "grep -q installed $W/o"
$inst --rollback >"$W/o" 2>&1; ok "rollback ok" "[ $? = 0 ]"
ok "binary removed" "[ ! -e $J5_ROOT/vendor/bin/hw/android.hardware.radio-service.j5xnlte-qmi ]"
ok "rc removed" "[ ! -e $J5_ROOT/vendor/etc/init/init.j5xnlte-ril.rc ]"
ok "vintf removed" "[ ! -e $J5_ROOT/vendor/etc/vintf/manifest/android.hardware.radio.j5xnlte-qmi.xml ]"
ok "ueventd restored byte-for-byte" "cmp -s $J5_ROOT/vendor/etc/ueventd.rc $W/ueventd.orig1"
ok "old RIL running again" "[ \"\$(cat $J5_ROOT/props/init.svc.vendor.ril-daemon)\" = running ]"
ok "ours stopped" "[ \"\$(cat $J5_ROOT/props/init.svc.vendor.j5xnlte-ril)\" = stopped ]"
ok "state dir removed" "[ ! -d $J5_ROOT/data/local/tmp/j5ril-state ]"
ok "unrelated file untouched" "[ \"\$(cat $J5_ROOT/vendor/etc/init/old.rc)\" = orig ]"
ok "never two RILs" "! two_rils"

echo "== 2. SELinux type missing -> install undoes itself"
mkdev 2; rm "$J5_ROOT/policy_ok"
$inst --install --binary "$W/bin" --stop-old vendor.ril-daemon >"$W/o" 2>&1; ok "install fails" "[ $? != 0 ]"
ok "nothing left behind" "[ ! -e $J5_ROOT/vendor/bin/hw/android.hardware.radio-service.j5xnlte-qmi ] && [ ! -e $J5_ROOT/vendor/etc/init/init.j5xnlte-ril.rc ] && [ ! -d $J5_ROOT/data/local/tmp/j5ril-state ]"
ok "ueventd untouched" "cmp -s $J5_ROOT/vendor/etc/ueventd.rc $W/ueventd.orig2"

echo "== 3. node held, no --stop-old -> enable refused, old RIL untouched"
mkdev 3
$inst --install --binary "$W/bin" >"$W/o" 2>&1; ok "install ok (no stop list)" "[ $? = 0 ]"
$inst --enable >"$W/o" 2>&1; ok "enable refused" "[ $? != 0 ]"
ok "old still running, ours not started" "[ \"\$(cat $J5_ROOT/props/init.svc.vendor.ril-daemon)\" = running ] && [ ! -e $J5_ROOT/svc.log ]"
$inst --rollback >/dev/null 2>&1

echo "== 4. our service fails to start -> automatic return to the old RIL"
mkdev 4; touch "$J5_ROOT/force_ours_fail"
$inst --install --binary "$W/bin" --stop-old vendor.ril-daemon >/dev/null 2>&1
$inst --enable >"$W/o" 2>&1; ok "enable fails" "[ $? != 0 ]"
ok "old RIL restored" "[ \"\$(cat $J5_ROOT/props/init.svc.vendor.ril-daemon)\" = running ]"
ok "ours stopped" "[ \"\$(cat $J5_ROOT/props/init.svc.vendor.j5xnlte-ril)\" = stopped ]"
$inst --rollback >/dev/null 2>&1; ok "files gone after rollback" "[ ! -e $J5_ROOT/vendor/etc/init/init.j5xnlte-ril.rc ]"

echo "== 5. SELinux Permissive -> preflight refuses"
mkdev 5; echo Permissive >"$J5_ROOT/enforce"
$inst --preflight >"$W/o" 2>&1; ok "preflight fails" "[ $? != 0 ]"
$inst --install --binary "$W/bin" >"$W/o" 2>&1; ok "install refused" "[ $? != 0 ]"
ok "nothing installed" "[ ! -e $J5_ROOT/vendor/etc/init/init.j5xnlte-ril.rc ]"

echo "== 5b. node missing: preflight warns, enable refuses, modules rc installed/removed"
mkdev 7; rm "$J5_ROOT/dev/wwan0qmi0"
$inst --preflight >"$W/o" 2>&1; ok "preflight does not fail on a missing node" "[ $? = 0 ]"
ok "preflight explains the modules" "grep -q rpmsg_wwan_ctrl $W/o"
$inst --install --binary "$W/bin" --stop-old vendor.ril-daemon >/dev/null 2>&1; ok "install ok" "[ $? = 0 ]"
ok "modules rc installed" "grep -q 'insmod /vendor/lib/modules/rmnet.ko' $J5_ROOT/vendor/etc/init/init.j5xnlte-modem-modules.rc"
$inst --enable >"$W/o" 2>&1; ok "enable refused without the node" "[ $? != 0 ]"
ok "old RIL untouched" "[ \"\$(cat $J5_ROOT/props/init.svc.vendor.ril-daemon)\" = running ] && [ ! -e $J5_ROOT/svc.log ]"
$inst --rollback >/dev/null 2>&1; ok "modules rc removed by rollback" "[ ! -e $J5_ROOT/vendor/etc/init/init.j5xnlte-modem-modules.rc ]"

echo "== 6. rollback with nothing installed"
mkdev 6; $inst --rollback >"$W/o" 2>&1; ok "no-op rollback ok" "[ $? = 0 ]"

echo "install tests: $pass passed, $fail failed"
[ $fail = 0 ]
