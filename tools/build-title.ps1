# Build the Mirage title in WSL (Ubuntu-24.04) with ps5-native-app-boilerplate.
#   .\tools\build-title.ps1
# The work tree is ~/mirage/title inside WSL: a copy of the boilerplate
# (~/mirage/ps5-native-app-boilerplate) whose src\ is replaced by title\src
# from here, plus the boilerplate's demo renderer. The finished title folder
# is copied back to out\title\<TITLEID>. Nothing is sent to the console.
# The payload (out\mirage.elf, from tools\build.ps1) is packaged with it.
param(
    [string]$TitleId = 'PPSA99741',
    [string]$AppName = 'Mirage',
    [string]$Distro = 'Ubuntu-24.04',
    # The payload the title starts at launch; packaged as /app0/mirage.elf.
    [string]$Payload = 'out\mirage.elf',
    # Bring the hardware decoder up but decode nothing (first console test of the decoder).
    [switch]$DecoderBringupOnly,
    # Draw a line of figures over the mirrored picture (testing).
    [switch]$DebugOverlay,
    # Also pack the title as an .ffpfsc image (for a release): out\release\<TITLEID>.ffpfsc.
    # The first time, the boilerplate fetches MkPFS (github.com/PSBrew/MkPFS, pinned) into its work tree.
    [switch]$Ffpfsc
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
if ($Root -notmatch '^([A-Za-z]):\\(.*)$') { throw 'The project must be on a drive WSL can see.' }
$WslRoot = "/mnt/$($Matches[1].ToLowerInvariant())/$($Matches[2].Replace('\', '/'))"

$PayloadFile = Join-Path $Root $Payload
if (-not (Test-Path $PayloadFile)) { throw "missing $Payload; build it first with tools\build.ps1" }
$WslPayload = $WslRoot + '/' + $Payload.Replace('\', '/')

$MakeTarget = if ($Ffpfsc) { 'ffpfsc' } else { 'app' }
$Definitions = (@(if ($DecoderBringupOnly) { 'MIRAGE_VDEC_BRINGUP_ONLY=1' }; if ($DebugOverlay) { 'MIRAGE_DEBUG_OVERLAY=1' }) -join ' ')
$script = @"
set -euo pipefail
base=~/mirage/ps5-native-app-boilerplate
work=~/mirage/title
[ -d "`$base" ] || { echo "missing `$base (see NOTES.md)" >&2; exit 2; }
if [ ! -d "`$work" ]; then
    cp -a "`$base" "`$work"
    rm -rf "`$work/.git" "`$work/dist" "`$work/build/app.ninja" "`$work/build/llvm-pie.elf" "`$work/build/eboot.elf"
fi
cd "`$work"
# Our sources replace the demo's main; the demo renderer stays as the screen.
find src -maxdepth 1 -type f ! -name 'demo_renderer.*' -delete
for f in '$WslRoot'/title/src/* '$WslRoot'/src/pcm_link.h; do tr -d '\r' < "`$f" > "src/`$(basename "`$f")"; done
cp '$WslPayload' mirage.elf
# The home screen tile and backgrounds (tools\make-art.py writes them).
for f in icon0.png pic0.dds pic1.dds; do
    [ -f '$WslRoot'/title/sce_sys/"`$f" ] || { echo "missing title/sce_sys/`$f: run python tools\make-art.py" >&2; exit 2; }
    cp '$WslRoot'/title/sce_sys/"`$f" sce_sys/"`$f"
done
# libSceVideodec2 is not among the SDK's import stubs: build one from the
# link stub, the way ProsperoLight's build does, and hand it to the build.
sdk=.deps/native/ps5-payload-sdk
[ -x "`$sdk/bin/prospero-lld" ] || make deps >/dev/null
mkdir -p build/obj build/stubs
tr -d '\r' < '$WslRoot/title/stubs/videodec2_link_stub.c' > build/videodec2_link_stub.c
PS5_PAYLOAD_SDK="`$(pwd)/`$sdk" sh tooling/prospero-clang18 -std=c11 -O2 -fPIC -ffunction-sections -fdata-sections -c build/videodec2_link_stub.c -o build/obj/libSceVideodec2_link_stub.o
"`$sdk/bin/prospero-lld" --shared -soname libSceVideodec2.prx -o build/stubs/libSceVideodec2.so build/obj/libSceVideodec2_link_stub.o
# This boilerplate version links every .so in the SDK's stub directory and
# has no option for more, so the stub joins them (in this work tree's SDK only).
cp build/stubs/libSceVideodec2.so "`$sdk/target/lib/libSceVideodec2.so"
make init TITLE_ID='$TitleId' APP_NAME='$AppName' >/dev/null
rm -rf dist
make $MakeTarget APP_ROOT_FILES=mirage.elf APP_DEFINITIONS='$Definitions' 2>&1 | tail -n 25
rm -rf '$WslRoot/out/title/$TitleId'
mkdir -p '$WslRoot/out/title'
cp -r "dist/$TitleId" '$WslRoot/out/title/$TitleId'
if [ '$MakeTarget' = ffpfsc ]; then
    [ -f "dist/$TitleId.ffpfsc" ] || { echo 'no .ffpfsc was made' >&2; exit 3; }
    mkdir -p '$WslRoot/out/release'
    cp "dist/$TitleId.ffpfsc" '$WslRoot/out/release/$TitleId.ffpfsc'
    echo "copied to out/release/$TitleId.ffpfsc"
fi
echo "copied to out/title/$TitleId" # (the pipe from PowerShell appends a CR; keep it inside this comment)
"@
$script = $script -replace "`r", ''
$script | wsl.exe -d $Distro -- bash -s
if ($LASTEXITCODE -ne 0) { throw "title build failed ($LASTEXITCODE)" }
Get-ChildItem -Recurse -File (Join-Path $Root "out\title\$TitleId") | ForEach-Object { '{0,10:N0}  {1}' -f $_.Length, $_.FullName.Substring($Root.Length + 1) }
