#!/bin/sh
# Builds and runs the RIL glue test (ASan+UBSan) against the independent Python fake modem.
set -eu
cd "$(dirname "$0")/.."
B=build/glue; mkdir -p $B
gcc -std=c11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Werror \
    -Iril/core -Iqmi/include tests/test_ril_glue.c ril/core/*.c qmi/src/qmi_codec.c qmi/src/qmi_dev.c qmi/src/qmi_log.c \
    -lpthread -o $B/test_ril_glue
FAKE_UIM_UNPROVISIONED=1 FAKE_NAS_NOSERVICE_FIRST=1 exec python3 tests/fake_modem/fake_ril_modem.py -- $B/test_ril_glue '{DEV}'
