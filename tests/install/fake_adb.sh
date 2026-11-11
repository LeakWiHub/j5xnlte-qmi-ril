#!/bin/bash
# Minimal fake `adb` for tests: "device" = directory $J5_ROOT, paths are mapped into it. Test-only.
case "$1" in
  wait-for-device|root|remount) exit 0 ;;
  push) mkdir -p "$J5_ROOT$(dirname "$3")"; cp "$2" "$J5_ROOT$3" ;;
  shell)
    shift; cmd="$*"
    case "$cmd" in
      "id -u") echo 0 ;;
      "mkdir -p "*) mkdir -p "$J5_ROOT${cmd#mkdir -p }" ;;
      "sh /data/local/tmp/j5ril-inst/dev.sh"*)
        c=${cmd#sh /data/local/tmp/j5ril-inst/dev.sh}
        # path arguments of the device script live under /data/local/tmp/... of the fake root
        c=${c//\/data\/local\/tmp\/j5ril-inst/$J5_ROOT\/data\/local\/tmp\/j5ril-inst}
        eval "J5_TEST=1 J5_ROOT='$J5_ROOT' J5_NODE=/dev/wwan0qmi0 bash $J5_ROOT/data/local/tmp/j5ril-inst/dev.sh $c" ;;
      *) echo "fake adb: unhandled shell command: $cmd" >&2; exit 99 ;;
    esac ;;
  *) echo "fake adb: unhandled: $*" >&2; exit 99 ;;
esac
