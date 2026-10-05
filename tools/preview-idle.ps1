# Preview of the title's idle screen: builds title\src\idle_screen.cpp for the
# PC in WSL, draws the screen for a few states and writes them as PNG pictures
# to out\preview\.
#   .\tools\preview-idle.ps1
param([string]$Distro = 'Ubuntu-24.04')

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
if ($Root -notmatch '^([A-Za-z]):\\(.*)$') { throw 'The project must be on a drive WSL can see.' }
$WslRoot = "/mnt/$($Matches[1].ToLowerInvariant())/$($Matches[2].Replace('\', '/'))"

$script = @"
set -euo pipefail
work=~/mirage/idle-preview
rm -rf "`$work" && mkdir -p "`$work"
for f in '$WslRoot'/title/src/idle_screen.cpp '$WslRoot'/title/src/idle_screen.hpp '$WslRoot'/title/src/player.hpp '$WslRoot'/title/src/demo_renderer.cpp '$WslRoot'/title/src/demo_renderer.hpp '$WslRoot'/tests/host_idle_preview.cpp; do
    tr -d '\r' < "`$f" > "`$work/`$(basename "`$f")"
done
cd "`$work"
clang++-18 -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -o idle_preview demo_renderer.cpp idle_screen.cpp host_idle_preview.cpp
ASAN_OPTIONS=detect_leaks=0 ./idle_preview
mkdir -p '$WslRoot'/out/preview
python3 - '$WslRoot'/out/preview <<'PY'
import glob, os, struct, sys, zlib
for path in sorted(glob.glob('*.ppm')):
    data = open(path, 'rb').read()
    parts = data.split(b'\n', 3)
    width, height = map(int, parts[1].split())
    rgb = parts[3]
    raw = b''.join(b'\x00' + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    out = os.path.join(sys.argv[1], os.path.splitext(path)[0] + '.png')
    open(out, 'wb').write(png)
    print('wrote', out)
PY
exit 0 # (the pipe from PowerShell appends a CR; keep it inside this comment)
"@

$script -replace "`r", '' | wsl -d $Distro -- bash -s
if ($LASTEXITCODE -ne 0) { throw 'idle preview failed' }
