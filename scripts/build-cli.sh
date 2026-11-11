#!/bin/sh
# Builds ril-cli. On the phone, in Termux:  pkg install -y clang && sh scripts/build-cli.sh   (bionic, native aarch64)
# On a Linux host (for the fake-modem test):  sh scripts/build-cli.sh host
set -eu
cd "$(dirname "$0")/.."
OUT=${OUT:-build/ril-cli}
mkdir -p "$(dirname "$OUT")"
CC=${CC:-clang}; command -v "$CC" >/dev/null 2>&1 || CC=gcc
$CC -std=gnu11 -O2 -Wall -Wextra -Iril/core -Iqmi/include tools/ril-cli/ril_cli.c ril/core/*.c qmi/src/qmi_codec.c qmi/src/qmi_dev.c qmi/src/qmi_log.c -lpthread -o "$OUT"
echo "built $OUT"
