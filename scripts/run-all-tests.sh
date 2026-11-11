#!/bin/bash
# Everything that can run on a Linux host. Needs: gcc, g++, python3 (+ clang for the aidl type-check in run-hal-test.sh
# if it asks for it), an AOSP checkout or the radio AIDL frozen API directory for the HAL test (see run-hal-test.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
bash scripts/run-tests.sh
bash scripts/run-glue-test.sh
bash scripts/run-real-frames-test.sh
bash scripts/run-hal-test.sh "$@"
bash tests/install/test_install.sh
echo "ALL HOST TESTS PASSED (none of this exercises a real modem, device, binder ABI or SELinux)"
