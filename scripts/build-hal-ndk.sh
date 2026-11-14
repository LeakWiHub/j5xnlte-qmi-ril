#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Standalone build of the Radio HAL with the Android NDK (no AOSP tree).
#   scripts/build-hal-ndk.sh <NDK dir> <aidl binary> <.../radio/aidl/aidl_api> [out dir]
# The AIDL stubs are generated from the frozen V4 files with `aidl --lang=ndk`.
# NOT verified by the author (no NDK/aidl in the authoring environment): the first run is the first real build.
set -euo pipefail
NDK=${1:?NDK dir}; AIDL=${2:?aidl binary}; API=${3:?aidl_api dir}; OUT=${4:-build/hal-ndk}
HERE=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mkdir -p "$OUT" && cd "$OUT" && pwd)
LEVEL=34
BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
CXX=$BIN/aarch64-linux-android${LEVEL}-clang++
CC=$BIN/aarch64-linux-android${LEVEL}-clang
test -x "$CXX" || { echo "no NDK clang at $CXX"; exit 1; }
test -x "$AIDL" || { echo "no aidl at $AIDL"; exit 1; }
"$AIDL" --version 2>&1 | head -2 || true

PKGS="android.hardware.radio android.hardware.radio.config android.hardware.radio.data android.hardware.radio.messaging android.hardware.radio.modem android.hardware.radio.network android.hardware.radio.sim android.hardware.radio.voice"
INC=()
for p in $PKGS; do test -d "$API/$p/4" || { echo "missing $API/$p/4"; exit 1; }; INC+=(-I "$API/$p/4"); done

rm -rf "$OUT/gen" "$OUT/inc" "$OUT/obj"; mkdir -p "$OUT/gen" "$OUT/inc" "$OUT/obj"
for p in $PKGS; do
  hash=$(cat "$API/$p/4/.hash")
  mapfile -t files < <(find "$API/$p/4" -name '*.aidl' | sort)
  echo "== aidl $p: ${#files[@]} files, hash $hash"
  "$AIDL" --lang=ndk --structured --stability=vintf --version 4 --hash "$hash" \
      "${INC[@]}" -o "$OUT/gen" -h "$OUT/inc" "${files[@]}"
done

CFLAGS_COMMON=(-O2 -fPIE -Wall -Wno-unused-parameter -D__ANDROID_VENDOR__ -DANDROID_STRICT -Wno-deprecated-declarations)
INCS=(-I"$OUT/inc" -I"$HERE/qmi/include" -I"$HERE/ril/core" -I"$HERE/hal/src" -I"$HERE/hal/gen")
SHIM=(-idirafter "$HERE/tools/ndk-shim")
objs=()
for f in "$HERE"/qmi/src/qmi_codec.c "$HERE"/qmi/src/qmi_dev.c "$HERE"/qmi/src/qmi_log.c \
         "$HERE"/ril/core/ril_core.c "$HERE"/ril/core/ril_svc_basic.c "$HERE"/ril/core/ril_svc_ext.c; do
  o="$OUT/obj/$(basename "$f").o"; "$CC" "${CFLAGS_COMMON[@]}" "${INCS[@]}" -c "$f" -o "$o"; objs+=("$o")
done
n=0
for f in $(find "$OUT/gen" -name '*.cpp') "$HERE"/hal/src/hal_common.cc "$HERE"/hal/src/radio_core.cc \
         "$HERE"/hal/src/radio_net.cc "$HERE"/hal/src/voice_audio.cc "$HERE"/hal/src/main.cpp "$HERE"/tools/ndk-shim/stability_shim.cpp; do
  n=$((n+1)); o="$OUT/obj/$n-$(basename "$f").o"
  "$CXX" -std=c++20 "${CFLAGS_COMMON[@]}" "${INCS[@]}" "${SHIM[@]}" -c "$f" -o "$o"; objs+=("$o")
done
"$CXX" -pie -static-libstdc++ -Wl,--gc-sections -o "$OUT/android.hardware.radio-service.j5xnlte-qmi" "${objs[@]}" -lbinder_ndk -llog -ldl
"$BIN/llvm-strip" "$OUT/android.hardware.radio-service.j5xnlte-qmi" || true
file "$OUT/android.hardware.radio-service.j5xnlte-qmi" 2>/dev/null || true
ls -la "$OUT/android.hardware.radio-service.j5xnlte-qmi"
"$BIN/llvm-readelf" -d "$OUT/android.hardware.radio-service.j5xnlte-qmi" | grep NEEDED

# voice path holder (raw ALSA ioctls, no libs)
"$CC" -O2 -fPIE -pie -Wall -o "$OUT/voice_hold" "$HERE/tools/voice/voice_hold.c"
"$BIN/llvm-strip" "$OUT/voice_hold" || true
ls -la "$OUT/voice_hold"
