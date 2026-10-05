# Build mirage.elf: UxPlay's protocol library + libplist + src\main.c, linked
# against pacbrew's OpenSSL. Uses the toolchain of D:\PS5\ps5-tailscale-dev.
#   .\tools\build.ps1                       # out\mirage.elf, runs until stopped
#   .\tools\build.ps1 -RunSeconds 120       # test build that stops itself
#   .\tools\build.ps1 -Name "Living Room" -LogLevel 7
param(
    [string]$Version = '0.1.0-dev',
    [string]$Name = 'Mirage',
    [int]$RunSeconds = 0,
    [int]$LogLevel = 6,
    [string]$Output = 'out\mirage.elf',
    [switch]$LinkTestTone,   # no AirPlay: only serve beeps to the title
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Dev = 'D:\PS5\ps5-tailscale-dev'
$Sdk = "$Dev\toolchain\ps5-payload-sdk"
$Clang = "$Dev\toolchain\llvm\bin\clang.exe"
$Pacbrew = "$Root\toolchain\pacbrew"
$UxRef = "$Root\ref\UxPlay\lib"
$Ux = "$Root\build\uxplay-lib"   # a copy of $UxRef with Mirage's patches applied (below)
$Plist = "$Root\ref\libplist"
$Obj = "$Root\build\obj"

if (-not (Test-Path "$Pacbrew\lib\libcrypto.a")) { throw "pacbrew is not unpacked at $Pacbrew (see NOTES.md)" }
if ($Clean -and (Test-Path $Obj)) { Remove-Item -Recurse -Force $Obj }
New-Item -ItemType Directory -Force "$Obj\ux", "$Obj\plist", "$Obj\app", (Split-Path -Parent (Join-Path $Root $Output)) | Out-Null

# UxPlay's library is built from a copy, so that ref\ stays as cloned. Each
# patch is an exact piece of text that must be there once: if upstream changes
# the spot, the build stops instead of silently building something else.
$UxPatches = @(
    @{ File = 'httpd.c'
       # The reverse (PTTH) connection a cast uses: every message on it after
       # the upgrade is a reply from the client and must not go to the request
       # parser. Upstream calls the getter where the setter is meant, so the
       # first reply ("HTTP/1.1 200 OK") is parsed as a request, fails with
       # HPE_INVALID_METHOD, the connection is dropped and no playlist is ever
       # fetched (seen on the console, 2026-10-05, UxPlay 3dbf7ce).
       Old  = "                if (connection->type == CONNECTION_TYPE_PTTH) {`n                    http_request_is_reverse(connection->request);`n                }"
       New  = "                if (connection->type == CONNECTION_TYPE_PTTH) {`n                    http_request_set_reverse(connection->request); // Mirage: upstream has _is_reverse here, a no-op`n                }" }
)
$Stamp = Join-Path $Ux '.mirage-source'
$RefHead = (git -C "$Root\ref\UxPlay" rev-parse HEAD 2>$null)
$PatchKey = "$RefHead|" + (($UxPatches | ForEach-Object { $_.File + $_.Old + $_.New }) -join '|').Length + '|' + (($UxPatches | ForEach-Object { $_.New }) -join '|')
if (-not (Test-Path $Stamp) -or ([IO.File]::ReadAllText($Stamp)) -ne $PatchKey) {
    $UxObjects = Join-Path $Obj 'ux'
    if (Test-Path $Ux) { [IO.Directory]::Delete($Ux, $true) }
    Copy-Item -Recurse $UxRef $Ux
    foreach ($p in $UxPatches) {
        $file = Join-Path $Ux $p.File
        $text = [IO.File]::ReadAllText($file) -replace "`r`n", "`n"
        $count = ([regex]::Matches($text, [regex]::Escape($p.Old))).Count
        if ($count -ne 1) { throw "UxPlay patch for $($p.File) expected its text once, found it $count times" }
        [IO.File]::WriteAllText($file, $text.Replace($p.Old, $p.New))
    }
    [IO.File]::WriteAllText($Stamp, $PatchKey)
    # Objects compiled from the previous copy are stale.
    if (Test-Path $UxObjects) { [IO.Directory]::Delete($UxObjects, $true) }
    New-Item -ItemType Directory -Force $UxObjects | Out-Null
}

$env:SCE_PROSPERO_SDK_DIR = $Sdk
$Common = @(
    '-target', 'x86_64-sie-ps5', '-fvisibility-nodllstorageclass=default',
    '-isysroot', $Sdk, '-isystem', "$Sdk\target\include",
    '-fno-stack-protector', '-fno-plt', '-femulated-tls', '-O2', '-c'
)

function Compile([string]$Source, [string]$Object, [string[]]$Flags) {
    $src = Get-Item $Source
    if ((Test-Path $Object) -and ((Get-Item $Object).LastWriteTimeUtc -gt $src.LastWriteTimeUtc)) { return }
    & $Clang @Common @Flags -o $Object $src.FullName
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $Source" }
}

# UxPlay lib with its built-in mDNS responder (lib\mdnsd), not the Avahi/Bonjour backend.
$UxFlags = @('-Wall', '-Wno-unused', '-Wno-sometimes-uninitialized',
    '-Dgetaddrinfo=mirage_getaddrinfo', '-Dfreeaddrinfo=mirage_freeaddrinfo',  # src\ps5_netdb.c
    '-DSYS_ENDIAN_H', '-DNOHOLD', '-DPLIST_210', '-DPLIST_230', '-DOPENSSL_API_COMPAT=0x10101000L',
    '-I', $Ux, '-I', "$Ux\llhttp", '-I', "$Ux\playfair", '-I', "$Ux\mdnsd",
    '-I', "$Plist\include", '-I', "$Pacbrew\include")
$UxSources = @(Get-ChildItem "$Ux\*.c") + @(Get-ChildItem "$Ux\mdnsd\*.c") + @(Get-ChildItem "$Ux\llhttp\*.c") + @(Get-ChildItem "$Ux\playfair\*.c")
foreach ($f in $UxSources) { Compile $f.FullName "$Obj\ux\$($f.Directory.Name)_$($f.BaseName).o" $UxFlags }

# libplist (C part only). HAVE_MEMMEM is left out on purpose: the SDK libc has none.
$PlistFlags = @('-w', '-DHAVE_STRNDUP=1', '-DHAVE_STRERROR=1', '-DHAVE_GMTIME_R=1', '-DHAVE_LOCALTIME_R=1',
    '-DHAVE_TIMEGM=1', '-DHAVE_TM_TM_GMTOFF=1', '-DHAVE_TM_TM_ZONE=1', '-DLIBPLIST_STATIC',
    '-DPACKAGE_VERSION="2.7.0"', '-I', "$Plist\include", '-I', "$Plist\src", '-I', "$Plist\libcnary\include")
$PlistSources = @(Get-ChildItem "$Plist\src\*.c") + @(Get-ChildItem "$Plist\libcnary\node.c", "$Plist\libcnary\node_list.c")
foreach ($f in $PlistSources) { Compile $f.FullName "$Obj\plist\$($f.BaseName).o" $PlistFlags }

# The launcher helper: a tiny payload of its own that the main payload carries
# inside and sends to the ELF loader when it has to open the title. It is the
# only part linked with the system service libraries.
. "$Dev\tools\env.ps1"
$Readelf = "$Dev\toolchain\llvm\bin\llvm-readelf.exe"
$Helper = "$Root\build\mirage-launcher.elf"
$HelperOk = $false
foreach ($pad in @(), @('-DDYNAMIC_PAD')) {
    Invoke-PS5CC -O2 -Wall -Wextra @pad -o $Helper "$Root\helper\launcher.c" -lSceUserService -lSceSystemService
    if (-not (cmd /c "`"$Readelf`" -d `"$Helper`" 2>&1" | Select-String 'warning')) { $HelperOk = $true; break }
}
if (-not $HelperOk) { throw "readelf rejects the dynamic table of the launcher helper, with and without the pad" }
$HelperStray = & $Readelf -SW $Helper 2>$null | Select-String ' AX | WAX ' | ForEach-Object { ($_.Line.Trim() -split '\s+')[2] } | Where-Object { $_ -notin '.text', '.init' }
if ($HelperStray) { throw "launcher helper: executable sections outside .text: $($HelperStray -join ' ')" }
Copy-Item $Helper (Join-Path $Root 'out\mirage-launcher.elf') -Force

# The front end is always recompiled: its settings come from the command line.
$AppFlags = @('-Wall', '-Wextra', "-DMIRAGE_VERSION=`"$Version`"", "-DMIRAGE_NAME=`"$Name`"",
    "-DMIRAGE_RUN_SECONDS=$RunSeconds", "-DMIRAGE_LOG_LEVEL=$LogLevel", '-I', $Ux, '-I', "$Plist\include",
    '-I', "$Pacbrew\include", "-DLAUNCHER_ELF=`"$($Helper.Replace('\', '/'))`"")
if ($LinkTestTone) { $AppFlags += @('-DMIRAGE_LINK_TEST_TONE', '-Wno-unused-function') }
Remove-Item -Force "$Obj\app\*.o" -ErrorAction SilentlyContinue
foreach ($f in Get-ChildItem "$Root\src\*.c") { Compile $f.FullName "$Obj\app\$($f.BaseName).o" $AppFlags }

. "$Dev\tools\env.ps1"
$Objects = (Get-ChildItem "$Obj\app\*.o", "$Obj\ux\*.o", "$Obj\plist\*.o").FullName
$Out = Join-Path $Root $Output
# FFmpeg from pacbrew for the audio decoder; its libavcodec names every codec, so
# the libraries those were configured with have to be on the line as well.
# libavformat (the HLS demuxer and HTTPS, for casting) comes first, and brings libssl.
$Ffmpeg = @('avformat', 'avcodec', 'swresample', 'avutil', 'x264', 'lzma', 'bz2', 'iconv', 'z', 'ssl') | ForEach-Object { "$Pacbrew\lib\lib$_.a" }
Invoke-PS5CC -o $Out @Objects @Ffmpeg "$Pacbrew\lib\libcrypto.a" -lpthread

# Checks before anything may go to the console: all code in the executable
# segment's sections, and a dynamic table readelf accepts.
$Readelf = "$Dev\toolchain\llvm\bin\llvm-readelf.exe"
$Stray = & $Readelf -SW $Out 2>$null | Select-String ' AX | WAX ' | ForEach-Object { ($_.Line.Trim() -split '\s+')[2] } | Where-Object { $_ -notin '.text', '.init' }
if ($Stray) { throw "executable sections outside .text: $($Stray -join ' ')" }
$Warnings = cmd /c "`"$Readelf`" -d `"$Out`" 2>&1" | Select-String 'warning'
if ($Warnings) { throw "readelf rejects the dynamic table of ${Out}: $($Warnings[0].Line.Trim())" }
Get-Item $Out | ForEach-Object { '{0}  {1:N0} bytes' -f $_.FullName, $_.Length }
