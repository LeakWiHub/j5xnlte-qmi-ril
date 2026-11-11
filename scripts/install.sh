#!/bin/bash
# Host-side installer (adb). Every step is separate so that nothing is switched by accident:
#   install.sh --preflight
#   install.sh --install --binary <path> [--stop-old "svc1 svc2"]   # copies files, does NOT switch RIL
#   install.sh --enable                                              # stops the named old services, starts ours, verifies; undoes itself on failure
#   install.sh --disable                                             # back to the old RIL, files stay
#   install.sh --rollback                                            # removes everything that was added, restores backups
#   install.sh --status
# Requires: adb root, a vendor partition that is writable (adb remount), the SELinux types from android/sepolicy
# already in the loaded policy (otherwise --install undoes itself). Not verified on a device.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ADB=${ADB:-adb}
ACTION=""; BIN=""; STOPOLD=""
while [ $# -gt 0 ]; do
  case $1 in
    --preflight|--install|--enable|--disable|--rollback|--status) ACTION=${1#--} ;;
    --binary) BIN=$2; shift ;;
    --stop-old) STOPOLD=$2; shift ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac; shift
done
[ -n "$ACTION" ] || { sed -n '2,12p' "$0"; exit 2; }

T=/data/local/tmp/j5ril-inst
$ADB wait-for-device
if [ "$ACTION" != status ] && [ "$($ADB shell id -u | tr -d '\r')" != 0 ]; then
  $ADB root >/dev/null || true; $ADB wait-for-device
  [ "$($ADB shell id -u | tr -d '\r')" = 0 ] || { echo "adb root is not available on this build" >&2; exit 1; }
fi
$ADB shell "mkdir -p $T"
$ADB push "$ROOT/scripts/device/j5ril-device.sh" $T/dev.sh >/dev/null
run() { $ADB shell "sh $T/dev.sh $*"; }

case $ACTION in
  preflight|status|enable|disable) run "$ACTION" ;;
  rollback) run rollback ;;
  install)
    [ -f "$BIN" ] || { echo "--binary <android.hardware.radio-service.j5xnlte-qmi built from Android.bp> is required" >&2; exit 2; }
    $ADB push "$BIN" $T/bin >/dev/null
    $ADB push "$ROOT/android/init.j5xnlte-ril.rc.in" $T/rc.in >/dev/null
    $ADB push "$ROOT/android/vintf/android.hardware.radio.j5xnlte-qmi.xml" $T/vintf.xml >/dev/null
    $ADB push "$ROOT/android/ueventd.rc.fragment" $T/uev >/dev/null
    $ADB push "$ROOT/android/init.j5xnlte-modem-modules.rc" $T/modrc >/dev/null
    # the vendor partition is read-only on a stock GSI setup; remount is the only way to add files to it
    $ADB remount >/dev/null 2>&1 || echo "note: adb remount failed/unnecessary; continuing, writes will fail if /vendor is read-only"
    run "install $T/bin $T/rc.in $T/vintf.xml $T/uev '$STOPOLD' $T/modrc"
    ;;
esac
