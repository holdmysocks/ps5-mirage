#!/usr/bin/env bash
# Inspect the title built in ~/mirage/title (run inside WSL): segments, the
# sections that are executable, the modules it needs and what it imports.
set -euo pipefail
cd ~/mirage/title
elf=build/llvm-pie.elf
[ -f "$elf" ] || { echo "no $elf; run tools/build-title.ps1 first" >&2; exit 1; }
echo "elf: $elf"
echo "segments:"
readelf -lW "$elf" | grep -E '^ +(LOAD|DYNAMIC)'
echo "executable sections: $(readelf -SW "$elf" | sed 's/^ *\[ *[0-9]*\] *//' | awk '$7 ~ /X/ {print $1}' | tr '\n' ' ')"
echo "needed: $(readelf -d "$elf" | grep NEEDED | sed 's/.*\[//;s/\]//' | tr '\n' ' ')"
echo "imports: $(nm -D -u "$elf" | awk '{print $2}' | tr '\n' ' ')"
exit 0 # (piped from PowerShell, which appends a CR; keep it inside this comment)
