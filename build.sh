#!/usr/bin/env bash
# Builds build/shakedown.exe (the window) and build/shakedown-cli.exe (the terminal program) with zig.
# Needs Python 3 + Pillow for the icon. Linux, macOS or Windows via Git Bash.
set -euo pipefail
cd "$(dirname "$0")"
ZIG="${ZIG:-zig}"
FLAGS="-target x86_64-windows-gnu -Os -s -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function"
LIBS="-lkernel32 -luser32 -lshell32 -ladvapi32 -lwevtapi -lpdh"
mkdir -p build
python3 tools/make_icon.py build/shakedown.ico
cp src/shakedown.rc build/
"$ZIG" cc $FLAGS src/cli.c -o build/shakedown-cli.exe $LIBS
"$ZIG" cc $FLAGS -municode -Wl,--subsystem,windows src/window.c build/shakedown.rc -o build/shakedown.exe $LIBS -lgdi32 -ldwmapi
ls -l build/shakedown.exe build/shakedown-cli.exe
