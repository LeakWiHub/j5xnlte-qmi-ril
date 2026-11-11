#!/bin/bash
# Host tests: codec (ASan+UBSan), transport vs fake modem (ASan+UBSan and TSan, N runs),
# then the aarch64 probe under qemu-user against tests/fake_modem (if qemu-aarch64 is available).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
B=build; mkdir -p $B
CC=${CC:-gcc}; RUNS=${RUNS:-10}
CFLAGS="-std=gnu11 -Wall -Wextra -Werror -g -Iqmi/include"
echo "== codec (ASan+UBSan)"
$CC $CFLAGS -fsanitize=address,undefined -fno-sanitize-recover=all tests/test_codec.c qmi/src/qmi_codec.c -o $B/test_codec && $B/test_codec
SRC="tests/test_transport.c qmi/src/qmi_dev.c qmi/src/qmi_codec.c qmi/src/qmi_log.c"
$CC $CFLAGS -fsanitize=address,undefined -fno-sanitize-recover=all $SRC -o $B/test_transport_asan -lpthread
$CC $CFLAGS -O1 -fsanitize=thread $SRC -o $B/test_transport_tsan -lpthread
for kind in asan tsan; do
  echo "== transport ($kind) x$RUNS"
  for i in $(seq 1 "$RUNS"); do timeout 120 $B/test_transport_$kind >$B/t.out 2>&1 || { cat $B/t.out; echo "FAILED run $i ($kind)"; exit 1; }; done
  tail -1 $B/t.out
done
QEMU=${QEMU:-$(command -v qemu-aarch64 || true)}
if [ -n "$QEMU" ]; then
  echo "== probe (aarch64, under qemu) vs fake modem"
  bash scripts/build-probe.sh >/dev/null
  for sim in present absent; do
    python3 -I tests/fake_modem/fake_modem.py --sim $sim -- "$QEMU" $B/qmi-probe-aarch64 --quiet-raw {DEV} > $B/probe-$sim.out
    grep -q "stage 5: PASS" $B/probe-$sim.out || { cat $B/probe-$sim.out; echo "FAILED probe sim=$sim"; exit 1; }
    echo "probe sim=$sim: stages 1-5 PASS"
  done
else
  echo "== probe test skipped (no qemu-aarch64; set QEMU=/path/to/qemu-aarch64)"
fi
echo "ALL TESTS PASSED"
