#!/bin/sh
# Regenerates hal/gen/*.h from the frozen V4 AIDL snapshots of the Android tree.
# usage: tools/gen-hal-base/regen.sh [path/to/hardware/interfaces/radio/aidl/aidl_api]
set -eu
cd "$(dirname "$0")/../.."
API="${1:-${ANDROID_BUILD_TOP:-/nonexistent}/hardware/interfaces/radio/aidl/aidl_api}"
python3 tools/gen-hal-base/gen_hal_base.py "$API" 4 hal/gen
