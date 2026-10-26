#!/usr/bin/env bash
# Builds Pipette.exe (Windows launcher) with zig (pip install ziglang).
set -euo pipefail
cd "$(dirname "$0")"
OUT=${OUT:-build}; mkdir -p "$OUT"
cat > "$OUT/html.S" <<ASM
  .section .rdata,"dr"
  .globl pipette_html
pipette_html:
  .incbin "$(cd ../app && pwd)/index.html"
  .byte 0
  .globl pipette_html_len
  .p2align 2
pipette_html_len:
  .long $(wc -c < ../app/index.html)
ASM
python3 -m ziglang cc -target x86_64-windows-gnu -O2 -s -municode -Wl,--subsystem,windows -o "$OUT/Pipette.exe" launcher.c "$OUT/html.S" Pipette.rc -lshell32 -luser32
echo "Built $OUT/Pipette.exe"
