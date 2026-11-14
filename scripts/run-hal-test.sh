#!/bin/sh
# Host test of the HAL layer (real HAL + QMI sources, AIDL type-check mock, Python fake modem).
# usage: scripts/run-hal-test.sh [path/to/hardware/interfaces/radio/aidl/aidl_api]
# NOT a binder/ABI test and NOT a device test. Needs g++ (ASan/UBSan runtime) and python3.
set -eu
cd "$(dirname "$0")/.."
API="${1:-${ANDROID_BUILD_TOP:-/nonexistent}/hardware/interfaces/radio/aidl/aidl_api}"
[ -d "$API" ] || { echo "aidl_api dir not found: $API (pass it as argument)" >&2; exit 2; }
B=build/haltest; rm -rf $B; mkdir -p $B
python3 tools/aidl-mock/gen_aidl_headers.py "$API" 4 $B/mock >/dev/null
python3 tools/gen-hal-base/gen_hal_base.py "$API" 4 $B/base >/dev/null
python3 tools/gen-hal-base/gen_hal_base.py "$API" 4 $B/rec --recorders >/dev/null
if ! diff -rq $B/base hal/gen >/dev/null; then echo "hal/gen is stale: run tools/gen-hal-base/regen.sh" >&2; exit 1; fi
SAN="-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined"
for f in ril/core/*.c qmi/src/qmi_codec.c qmi/src/qmi_dev.c qmi/src/qmi_log.c; do
  gcc -std=c11 $SAN -Wall -Wextra -Werror -Iril/core -Iqmi/include -c $f -o $B/$(basename $f .c).o
done
g++ -std=c++20 $SAN -Wall -Wextra -Werror -Ihal/src -Ihal/gen -Itests/hal -I$B/rec -I$B/mock -Itools/aidl-mock/mock-ndk -Iril/core -Iqmi/include \
    tests/hal/test_hal.cc hal/src/hal_common.cc hal/src/radio_core.cc hal/src/radio_net.cc hal/src/voice_audio.cc $B/*.o -lpthread -o $B/test_hal
exec python3 tests/fake_modem/fake_ril_modem.py -- $B/test_hal '{DEV}'
