#!/bin/bash
# Cross-builds the libc-free aarch64 diagnostic. Needs: clang (with the aarch64 target) and lld.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLANGINC="$(clang -print-resource-dir)/include"
mkdir -p "$ROOT/build"
clang --target=aarch64-linux-gnu -std=gnu11 -O2 -Wall -Wextra -Werror \
  -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -nostdlib -nostdinc \
  -isystem "$CLANGINC" -I"$ROOT/tools/qmi-probe/stub" -I"$ROOT/qmi/include" \
  -static -fuse-ld=lld -Wl,-z,norelro -Wl,--build-id=none \
  "$ROOT/tools/qmi-probe/qmi_probe.c" "$ROOT/qmi/src/qmi_codec.c" \
  -o "$ROOT/build/qmi-probe-aarch64"
echo "built: $ROOT/build/qmi-probe-aarch64"
