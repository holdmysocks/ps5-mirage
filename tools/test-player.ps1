# Host test of the title's player: builds title\src\player.cpp with a stub
# AudioOut in WSL and runs it against a fake payload for each arrival pattern.
#   .\tools\test-player.ps1
# Run it before any title build goes to the console.
param([string]$Distro = 'Ubuntu-24.04')

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
if ($Root -notmatch '^([A-Za-z]):\\(.*)$') { throw 'The project must be on a drive WSL can see.' }
$WslRoot = "/mnt/$($Matches[1].ToLowerInvariant())/$($Matches[2].Replace('\', '/'))"

$script = @"
set -euo pipefail
work=~/mirage/host-test
rm -rf "`$work" && mkdir -p "`$work"
for f in '$WslRoot'/title/src/player.cpp '$WslRoot'/title/src/player.hpp '$WslRoot'/title/src/video.cpp '$WslRoot'/title/src/video.hpp '$WslRoot'/title/src/vdec.cpp '$WslRoot'/title/src/vdec.hpp '$WslRoot'/title/src/mirror_view.cpp '$WslRoot'/title/src/mirror_view.hpp '$WslRoot'/src/pcm_link.h '$WslRoot'/tests/host_player_test.cpp '$WslRoot'/tests/host_tiled_test.cpp '$WslRoot'/title/src/demo_renderer.cpp '$WslRoot'/title/src/demo_renderer.hpp; do
    tr -d '\r' < "`$f" > "`$work/`$(basename "`$f")"
done
cd "`$work"
clang++-18 -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -pthread -o player_test player.cpp video.cpp vdec.cpp mirror_view.cpp host_player_test.cpp
# The decoder's memory lives as long as the process; that is not a leak to report.
export ASAN_OPTIONS=detect_leaks=0
# The frame's tiled layout.
clang++-18 -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -o tiled_test demo_renderer.cpp host_tiled_test.cpp
status=0
./tiled_test || status=1
for pattern in steady music burst stall gap hold resume flush frozen video rotate late film; do
    ./player_test "`$pattern" || status=1
done
exit `$status # (the pipe from PowerShell appends a CR; keep it inside this comment)
"@
($script -replace "`r", '') | wsl.exe -d $Distro -- bash -s
if ($LASTEXITCODE -ne 0) { throw 'player host test failed' }
