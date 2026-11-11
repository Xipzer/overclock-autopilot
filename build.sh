#!/usr/bin/env bash
# Builds build/shakedown.exe with zig (Linux, macOS or Windows via Git Bash).
set -euo pipefail
cd "$(dirname "$0")"
ZIG="${ZIG:-zig}"
mkdir -p build
"$ZIG" cc -target x86_64-windows-gnu -Os -s -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
  src/shakedown.c -o build/shakedown.exe \
  -lkernel32 -luser32 -lshell32 -ladvapi32 -lwevtapi -lpdh
ls -l build/shakedown.exe
