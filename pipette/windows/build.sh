#!/usr/bin/env bash
# Builds Pipette.exe — a real Windows app (own window, menus, Save/Open dialogs) using Microsoft Edge WebView2.
# Needs zig (pip install ziglang). The app (../app/index.html) and WebView2Loader.dll are embedded in the .exe.
set -euo pipefail
cd "$(dirname "$0")"
OUT=${OUT:-build}; mkdir -p "$OUT"
python3 -m ziglang cc -target x86_64-windows-gnu -O2 -s -municode -Wall -Wno-unused-parameter -Wno-missing-braces \
  -Wl,--subsystem,windows -o "$OUT/Pipette.exe" main.c Pipette.rc \
  -lole32 -lshell32 -luser32 -lgdi32 -lcomdlg32 -ladvapi32
echo "Built $OUT/Pipette.exe"
