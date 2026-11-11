#!/bin/sh
# Runs the RIL API against responses recorded from the real modem (probe log). ASan+UBSan.
set -eu
cd "$(dirname "$0")/.."
B=build/real; mkdir -p $B
gcc -std=c11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wextra -Werror \
    -Iril/core -Iqmi/include tests/test_ril_real.c ril/core/*.c qmi/src/qmi_codec.c qmi/src/qmi_dev.c qmi/src/qmi_log.c \
    -lpthread -o $B/test_ril_real
python3 -I tests/fake_modem/replay_real.py -- $B/test_ril_real '{DEV}'
J5_REPLAY=sim python3 -I tests/fake_modem/replay_real.py -- $B/test_ril_real '{DEV}'
J5_REPLAY=registered python3 -I tests/fake_modem/replay_real.py -- $B/test_ril_real '{DEV}'
J5_REPLAY=online python3 -I tests/fake_modem/replay_real.py -- $B/test_ril_real '{DEV}'
