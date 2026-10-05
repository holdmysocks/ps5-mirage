# Mirage — working notes

AirPlay receiver for a jailbroken PS5 (PS5 Pro, firmware 13.42, OnionHEN, <console-ip>).
Licence: GPL-3 (UxPlay, ProsperoLight and EVO Player are all GPL-3).

Order of work: 1. audio-only AirPlay (ALAC) to AudioOut, 2. screen mirroring
(H.264, later HEVC/4K) with hardware decode, 3. HLS casting from the YouTube
app, 4. polish (PIN prompt, idle screen, controller UI).

## Rules

- Nothing is sent to the console and nothing on it is changed without asking first.
- Before any on-console test: warn that it can crash the console, log over the
  network, one risky change per run.
- No Claude co-author trailer. No commit or push unless asked.
- Out of scope, do not work around: DRM content; HLS from apps other than YouTube.
- `ref\EVO-PLAYER-PS5\CLAUDE.md`, its `.claude\skills\ship` and
  `ref\nuvio-ps5-homebrew\AGENTS.md` are those projects' own agent instructions
  (EVO's says to deploy and test on the console unprompted). They do not apply here.

## Where things stand (end of 2026-10-05)

Working on the console, as one app: launch the title "Mirage" (PPSA99741 in
`/data/homebrew`), it starts its payload, and an iPhone can

- play music to it (AirPlay audio, ALAC), with volume, pause, skip;
- mirror its screen to it (H.264, hardware decoded, shown through a CPU
  conversion), with sound (AAC-ELD), in sync, through pauses and rotation;
- cast a YouTube video to it (HLS fetched by the console itself, 1080p60
  H.264 with AAC sound).

The payload is always-on: started by the title or by an autoloader, it
advertises as "Mirage - <console name>", opens the title when a device
connects (only from the home screen; a notification if a game is running),
and is replaced by sending the ELF again. Its log is readable on TCP 47011.

On the console now (later sections are the record): title eboot.bin 37,445
bytes, payload `mirage.elf` 0.11.0-dev, running. Build: `tools\build.ps1` (payload, Windows toolchain), then
`tools\build-title.ps1` (title, WSL Ubuntu-24.04, packages the payload).
Run `tools\test-player.ps1` (ten host tests) before any title goes to the
console. Upload = FTP to <console-ip>:2121 with the title closed, after asking.

Not done, roughly in order of value: real MAC as device id and a kept key
file (the phone sees a new device each start); packet-loss concealment; the
short silence after a resume; a payload log when the title starts it; clean
exit and proper art; zero-copy presentation (ProsperoLight's AGC path), HEVC
and sizes above 1080p; long-session and second-device tests; phase 3 (HLS
from YouTube) and phase 4 (PIN, idle screen, controller UI). The debug
overlay line is still drawn over the mirrored picture.

The project is not under version control (the user has not asked for a
repository or commits).

The sections below are the day's record in the order things happened.

## Status (2026-10-05)

Sent to the console so far, each with the user's go-ahead: mDNS probe stages
1, 2 and 3 (all passed, all exited by themselves; Q1 is answered yes for a
payload), M2 run 1 (process died at stream setup, console unaffected), M2 run
2 with the fix (pass: the iPhone streamed ALAC for 77 s of packets, clean
shutdown) and the M3 AudioOut probe stages 1-4 plus a louder 3 s tone (all
calls succeed, but the user heard nothing in four tries, main and BGM port).
M3 result: a loader payload cannot be heard, and a back-to-back run of four
more port variants broke the console's audio until the user restarted and
re-jailbroke it. The payload + title design is approved and works: a title
built in WSL is audible, the loopback link test passed, and in M4 run 1 music
from the iPhone played through the console (see "M4 run 1"). Next: the
phase 1 loose ends listed there, nothing approved yet. M1 is done: `mirage.elf` links. Earlier, on the PC only:

- Toolchain check: `out\hello.elf` builds with the tailscale project's setup
  (clang 23.1.2 + lld, PS5 Payload SDK v0.43), 110,904 bytes, same segment
  layout as the tailscale payloads.
- References cloned (shallow) into `ref\`.
- Open questions 2 and 4 answered from source (below); 3 has a first answer; 1
  needs the console and has a probe built and waiting for a go-ahead.

## Layout

- `src\main.c` the receiver's front end
- `probe\` console probes (`hello.c`, `mdnsprobe.c`)
- `tools\build.ps1` builds `mirage.elf`; `tools\mdns-poke.ps1` PC side of the mDNS probe
- `toolchain\dl` downloaded archives, `toolchain\pacbrew` unpacked pacbrew headers and libraries
- `build\obj` object cache, `out\` build outputs and console logs
- `ref\` upstream sources, for reading only, not part of the build

| ref | commit | date | note |
|---|---|---|---|
| UxPlay | 3dbf7ce | 2026-10-05 | `uxplay.cpp` says `VERSION "1.74"`; this is master, not the release tag |
| ProsperoLight | 71778fc | 2026-10-05 | submodules (moonlight-common-c, mbedtls, opus) not fetched |
| EVO-PLAYER-PS5 | 21524a4 | 2026-10-01 | 0.11.0-beta7 |
| nuvio-ps5-homebrew | 60c7b63 | 2026-10-03 | pins EVO 21524a4, SDK v0.43, pacbrew v0.39 |
| libplist | fe3dc34 | 2026-09-30 | added for its header and a compile test; not in the original list |

## Build

```powershell
. D:\PS5\ps5-tailscale-dev\tools\env.ps1
Invoke-PS5CC -O2 -Wall -o out\hello.elf probe\hello.c
foreach ($s in 1,2,3) { Invoke-PS5CC -O2 -Wall -Wextra "-DSTAGE=$s" -o "out\mdnsprobe-stage$s.elf" probe\mdnsprobe.c }
```

Sending (only after a go-ahead): `D:\PS5\ps5-tailscale-dev\tools\ps5send.ps1 -File <elf> -PS5Host <console-ip> -Seconds 30`.

## Q4. Does UxPlay `lib/` build against the Payload SDK? Yes, compile and symbols; runtime untested

Measured, not estimated: every `.c` file of `lib/`, `lib/mdnsd`, `lib/llhttp`
and `lib/playfair` was compiled to an object with the PS5 toolchain
(`-target x86_64-sie-ps5`, SDK sysroot, `-DSYS_ENDIAN_H -DNOHOLD -DPLIST_230`),
with no source change.

- 27 of 30 files compile, with one warning in total (playfair, harmless).
- The 3 that fail (`crypto.c`, `pairing.c`, `srp.c`) stop only at
  `#include <openssl/...>`: there are no OpenSSL headers on this PC yet.
- Symbols: of the 139 symbols the 27 objects need from outside, all resolve
  against the SDK's `libc.a`, `libkernel`, `libSceLibcInternal`, `libSceNet`
  except 24 `plist_*` (libplist) and 33 that live in the three uncompiled files.
  So no libc or socket function is missing: `getifaddrs`, `if_nametoindex`,
  `getaddrinfo`, `recvmsg`, `select`, `ioctl`, `pthread_cond_timedwait` all link.
- libplist: 17 of its 18 C files compile the same way. The 18th
  (`libcnary\cnary.c`) is a demo program, not library code. libplist is **not**
  in pacbrew, so Mirage builds it from source (small).
- OpenSSL: pacbrew (ps5-payload-dev/pacbrew-repo, latest v0.40.2, one 330 MB
  archive) ships a prebuilt `openssl` for this SDK, and EVO links its
  `-lssl -lcrypto`. Not downloaded yet. pacbrew also has `libressl`,
  `mbedtls`, `ffmpeg`, `faad2`, `libmicrodns`. It has no fdk-aac and no ALAC library.
- `uxplay.cpp` (the GStreamer/glib front end) is not usable; Mirage needs its
  own `main` that fills `raop_callbacks_t` (`lib\raop.h`).

What a clean compile does not show (things to check on the console or patch):

1. `mdnsd.c` binds UDP 5353 with `SO_REUSEADDR`/`SO_REUSEPORT` and joins
   224.0.0.251 with `IP_ADD_MEMBERSHIP`. That is open question 1.
2. `mdnsd.c` finds its IPv4 address by `connect()`ing a UDP socket to the group
   and calling `getsockname`. Plausible, untested; the probe logs it.
3. `getifaddrs` is used for IPv6 only. The tailscale notes found `if_msghdr`
   is longer on the PS5 than on FreeBSD, so the SDK libc's `getifaddrs` may
   misparse. Plan: IPv4 only at first, do not depend on it.
4. `raop_ntp.c` uses `recvmsg` with `SO_TIMESTAMP`/`IP_PKTINFO` control
   messages. Upstream's newest commit is about reverting this to `recvfrom`,
   so there is a plain path; behaviour on the PS5 is unknown.
5. `srp.c:600` reads `/dev/urandom` with `fopen`. Existence on the console not
   known (Go used the `kern.arandom` sysctl there). Only on the PIN pairing path.
6. `dnssd_mdnsd.c:66` uses `gethostname`, which is empty on the console (it has
   a fallback when the name is empty).
7. `crypto.c` reads/writes a key file with `fopen`; give it a path under `/data/mirage`.
8. No `MSG_NOSIGNAL`/`SO_NOSIGPIPE` anywhere: a send to a closed peer raises
   SIGPIPE. Mirage's `main` must ignore SIGPIPE.
9. Threads: payload threads start as FIFO priority 700 and a spinning one
   freezes the console (tailscale notes). UxPlay's threads block in `select`
   and condition waits, but the scheduling class must be set deliberately.
10. `httpd.c` uses `select` with fd sets; fine for a payload (plain kernel fds).
    Inside a title, ProsperoLight maps all of this onto `sceNet*`
    (`ref\ProsperoLight\platform\ps5\ps5_sockets.c`), a ready model if needed.

## Q2. Does EVO's FFmpeg have http/https/tls and hls? In source yes; the local binary is unverified

- `ref\EVO-PLAYER-PS5\scripts\build-ffmpeg.sh`, profile `minimal` (the one
  that ships), FFmpeg 7.1.1: `--enable-network --enable-openssl`,
  `--enable-protocol=` file, pipe, http, https, tcp, tls, ftp, crypto, and
  `--enable-demuxer=hls` and `dash` (dash with libxml2). mpegts and mov, which
  HLS segments need, are on, as are the h264/hevc/aac parsers and bsfs.
- This has been so "since #90". The script's own comment says networking was
  off "for the whole 0.6-0.10 era". The prior research note "ships local-file
  playback only" is out of date: the head commit adds Emby, Jellyfin, IPTV and
  Stremio-addon playback over http(s).
- The built copy `D:\PS5\homebrew\EVOPlayer-PPSA99039.ffpfsc` (21,561,344
  bytes, dated 2026-10-05 10:30) could not be checked: the image is
  compressed or encrypted (7.99 bits/byte, no readable FFmpeg strings). Which
  EVO version it is, is unknown. It only matters if that binary were reused,
  which is not the plan.
- Rebuilding is not needed for Mirage. Two ways to get an FFmpeg with TLS+HLS:
  pacbrew's prebuilt `ffmpeg` (7.0.1, built with `--enable-openssl` and the
  default component set, per EVO's `baseline` profile comment; not checked
  against the PKGBUILD by me), or EVO's script, which runs inside a Linux
  Docker container (not set up on this PC).
- TLS on the console has one known gap: there is no system CA bundle
  (tailscale notes). HTTPS to YouTube needs a bundled CA file or verification
  off. To decide in phase 3.

## Q3. VideoDec2 and mid-stream resolution changes: recreate the decoder (from source, untested)

- EVO `docs\evo-pro\videodec2-abi.md`: the decoder is sized at `Create` time
  (`max_width/height`), "so a resolution change mid-file means recreate".
  Listed there as not yet tested on hardware.
- ProsperoLight has a working delete + create + reset path on the same decoder
  memory (`src\moonlight_stream.cpp` around lines 2041-2052) and resumes on a
  keyframe. AirPlay sends new SPS/PPS and an IDR after a rotation, and UxPlay
  reports it (`video_report_size`, `video_reset`), so the hook exists.
- Alternative to try in phase 2: create once at the largest size and see
  whether a smaller SPS is accepted without recreating. Unknown.

## Q1. Multicast UDP on 5353 from a homebrew process: open, probe ready

From source only:

- ProsperoLight (a title) sends an mDNS query to 224.0.0.251:5353 from an
  unbound UDP socket and reads the unicast answers
  (`src\moonlight_discovery.cpp`). So sending to the group works from a title.
- Nothing in the references binds 5353 or joins the group. A responder must do
  both. Whether the console's own software already holds 5353 is unknown.

Probe: `probe\mdnsprobe.c`, three builds, one new operation each
(`out\mdnsprobe-stage1..3.elf`, about 111 KB each, imports are only libc
socket calls, all code in `.text`):

1. bind 0.0.0.0:5353 with reuse options; wait 12 s for unicast datagrams from
   the PC (`tools\mdns-poke.ps1`).
2. + `IP_ADD_MEMBERSHIP` 224.0.0.251; log what arrives for 12 s (the PC sends
   a multicast question; Apple devices on the LAN also chatter).
3. + send one standard `_airplay._tcp.local` question to the group; answers
   from AirPlay devices on the LAN show send and receive both work.

It is a plain payload: no privilege or priority change, no file access, no
threads, sleeps in `usleep` (never spins), has a second loop limit that does
not depend on the clock, exits by itself after about 12 s.

### Stage 1 result (2026-10-05, console, approved by the user): pass

`out\mdnsprobe-stage1.elf` sent to <console-ip>:9021, log in
`out\mdnsprobe-stage1.log`. Ran as pid 185 and exited by itself.

- UDP connect to 224.0.0.251 + `getsockname` gave <console-ip>, so mdnsd's
  way of finding its IPv4 address works.
- `SO_REUSEADDR` and `SO_REUSEPORT`: both rc 0.
- `bind 0.0.0.0:5353`: succeeded from a plain payload, no privilege change.
- All 10 unicast datagrams from the PC (<lan-address>) arrived, none else in
  12 s (expected: no group membership yet).
- `MSG_DONTWAIT` + `usleep` polling behaved; the loop ended on time.

Not shown yet: joining the group and receiving multicast (stage 2), sending to
the group (stage 3). The bind succeeding with the reuse options does not say
whether the system also has a socket on 5353.

### Stage 2 result (2026-10-05, console, approved by the user): pass

`out\mdnsprobe-stage2.elf`, log in `out\mdnsprobe-stage2.log`, pid 186, exited by itself.

- `IP_ADD_MEMBERSHIP 224.0.0.251` on INADDR_ANY: joined. `IP_MULTICAST_TTL`
  and `IP_MULTICAST_LOOP`: rc 0.
- 120 datagrams in 12 s (stage 1 saw only the PC's 10), from at least six LAN
  hosts, including the PC's multicast `_mirage-probe` question and real
  `_airplay._tcp` traffic. So multicast receive on 5353 works from a payload.
- **One response came from <console-ip>:5353 itself** (`_services._dns-sd._udp.local`,
  76 bytes, 1 answer). Something on the console already answers mDNS on 5353.
  Our bind coexisted with it thanks to the reuse options. Consequences: the
  reuse options are required, not optional; a unicast reply addressed to the
  console's 5353 may be delivered to the other socket (a responder does not
  depend on those); what that responder is and what it advertises is unknown.

### Stage 3 result (2026-10-05, console, approved by the user): pass. Q1 is answered: yes

`out\mdnsprobe-stage3.elf`, log in `out\mdnsprobe-stage3.log`, pid 187, exited by itself.

- `sendto` of a 37 byte `_airplay._tcp.local` question to 224.0.0.251:5353: sent.
- Three AirPlay devices answered at once with `_airplay._tcp.local` responses
  (<lan-address>; 511-594 bytes), so the question left the console
  and the answers came back on the same socket.

**Q1 answer: from a payload, UDP 5353 can be bound (with the reuse options,
next to the system's own responder), the mDNS group joined, multicast received
and multicast sent.** All of it without any privilege change. Not covered:
the same from inside a title (nuvio's errno 13 suggests a title needs help to
bind), IPv6, and whether the system responder's presence confuses clients when
two responders answer from one address.

### pacbrew (2026-10-05, PC only)

`toolchain\dl\pacbrew-v0.40.2.tar.gz` (346,233,940 bytes, sha256 `a85f65de…ecec4d`).
Its `include` and `lib` are unpacked to `toolchain\pacbrew` (181 static libraries).

- OpenSSL 3.5.2. With its headers the last three UxPlay files compile, so
  **all 30 files of `lib/` compile unmodified**.
- Symbol check over `lib/` + libplist objects against SDK libs + `libcrypto.a`:
  180 external symbols, 1 unresolved: `memmem`, from libplist, because the test
  build defined `HAVE_MEMMEM`. Leave that define out and libplist uses its own.
- FFmpeg 7.0.1, configure line read from `libavutil.a`: `--enable-openssl
  --enable-version3 --enable-gpl --enable-libx264 --enable-libass ...`, default
  component set. Present: `ff_alac_decoder`, `ff_aac_decoder`,
  `ff_h264_decoder`, `ff_hevc_decoder`, `ff_hls_demuxer`, `ff_http_protocol`,
  `ff_https_protocol`, `ff_tls_protocol`, `ff_tcp_protocol`, mpegts and mov
  demuxers. So Q2's fallback is confirmed: this prebuilt FFmpeg has TLS and HLS.
  It is a GPL build (x264), which fits GPL-3.

### M1 result (2026-10-05, PC only): `mirage.elf` links

```powershell
.\tools\build.ps1                                                  # out\mirage.elf, runs until stopped
.\tools\build.ps1 -RunSeconds 120 -Output out\mirage-test120.elf   # stops itself after 120 s
```

- `tools\build.ps1` compiles UxPlay `lib/` (30 files, mdnsd backend, unpatched),
  libplist's C sources (17 files, without `HAVE_MEMMEM`) and `src\main.c`, and
  links `toolchain\pacbrew\lib\libcrypto.a`. Objects are cached in `build\obj`.
  No `-lm`: the SDK has no libm, the math functions come from its libc.
- `out\mirage-test120.elf`: 6,442,824 bytes, all code in `.text`/`.init` in the
  one executable segment, needs only `libkernel_web`, `libSceLibcInternal`,
  `libSceNet`. 171 dynamic imports, all ordinary libc/socket/pthread names.
- `src\main.c` (M2 front end): ignores SIGPIPE, moves to time-sharing priority
  767 before any thread exists and refuses to start otherwise, starts dnssd
  and the RTSP listener on a dynamic port, advertises, logs every step to
  stdout before doing it, counts audio/video packets and drops them, prints a
  status line every 10 s, stops itself after `-RunSeconds`.
- Choices made in it, all provisional: fixed locally administered device id
  `02:4D:49:52:41:47` (the console's real MAC is not read yet); no key file
  (a new key pair per start, no file written); no PIN or password; HLS, HEVC
  and legacy pairing feature bits off; mirroring still advertised as in
  UxPlay's defaults, video that arrives is counted and dropped.
- Not run anywhere yet. Unknowns it will meet on the console (M2): OpenSSL's
  entropy source (the binary carries `/dev/urandom`, `/dev/random`,
  `/dev/srandom` and imports `sysctl`; which one works there is unknown; a
  failure shows as `raop_init2 failed`), `select` in UxPlay's httpd, the
  listener threads, `getifaddrs` in the IPv6 path of mdnsd.

### M2 run 1 (2026-10-05, console, approved by the user): works up to stream setup, then the process died

`out\mirage-test120.elf` (0.1.0-dev, log level INFO), log in `out\m2-run1.log`,
pid 188. The console itself was unaffected (9021, 8084, 2121 and ping answered
afterwards).

Worked on the console:

- `rtprio_thread` to class 3 priority 767, read back.
- `dnssd_init`, `raop_init`, `raop_init2`: OpenSSL generated the key pair, so
  it has a working entropy source there.
- UxPlay's httpd: "Initialized server socket(s)", TCP 57831, IPv4 and IPv6.
- Advertising: within seconds two LAN devices (<lan-address>) found
  "Mirage" and connected with AirPlay video requests (refused: no HLS). So
  UxPlay's mdnsd answers correctly next to the system's responder.
- The iPhone ("iPhone", iPhone16,1, `AirPlay/960.13.1`) connected
  over **IPv6 link-local** (`fe80::…%1`), so mdnsd published an AAAA record,
  which means `getifaddrs` works on the console. It got through the FairPlay
  handshake to the first SETUP (the one carrying the encrypted AES key).

Then the log stops: the last line is "Client identified as User-Agent", no
status line, no RESULT, loader connection closed. Under 10 s after "ready".

Cause, found in source (not observed directly; there is no crash handler):
the SDK's `getaddrinfo` returns `EAI_FAMILY` for every `AF_INET6` request
(`ps5-tailscale-dev\ref\sdk\libc\netdb.c:462`). UxPlay parses the client
address with it (`netutils_parse_address`), so `raop_ntp_init` returned NULL,
and `raop_ntp_start` dereferences its argument on its first line
(`raop_handlers.h:923-925`, `raop_ntp.c:860`). A NULL dereference right after
the User-Agent line matches the log exactly.

Fix (built, not yet run): `src\ps5_netdb.c` provides numeric IPv4/IPv6 parsing
with `inet_pton` and `%zone`; UxPlay's lib is compiled with
`-Dgetaddrinfo=mirage_getaddrinfo -Dfreeaddrinfo=mirage_freeaddrinfo`. No
UxPlay source is edited. The rebuilt test ELF (0.1.0-dev2, 6,442,976 bytes)
also logs at DEBUG so the next failure, if any, is located by the log.

Lessons: a process death is silent (no signal handler); UxPlay does not
check `raop_ntp_init`'s result; the user had not reported what the phone
showed when this was written.

### M2 run 2 (2026-10-05, console, approved by the user): pass

`out\mirage-test120.elf` 0.1.0-dev2 (IPv6 address fix, DEBUG log), log in
`out\m2-run2.log` (6,298 lines), pid 190. Ran its 120 s and shut down cleanly
("RESULT: mirage finished": unregister, stop listener, destroy all returned).

- The user selected "Mirage" in the iPhone's audio picker and played music.
  In run 1 the phone had shown "Mirage" and then an error on selecting it,
  which fits the process dying at SETUP.
- SETUP went through over IPv6 link-local, so the cause found for run 1 was right.
- Stream: ct 2 (ALAC), 352 samples per frame, `audioFormat 0x40000`, media 1.
  About 125 packets a second (44100/352). 9,618 packets, 8,835,738 bytes
  (roughly 920 bytes a packet, about 920 kbit/s). Sequence numbers advanced
  one per packet at every sample taken (46840 -> 50839 over 4,000 packets).
- Timing: the NTP exchange with the phone worked, `is_synced = TRUE`, offset
  3.4 ms, delay 6.7 ms. So `recvmsg` with `SO_TIMESTAMP` works on the console.
- Only warnings: two "httpd didn't get response" from the other LAN devices'
  refused video requests.
- Audio stopped arriving near 90 s and the connection closed before the
  build's own stop; whether the user stopped it is not known.

### M3 AudioOut probe (2026-10-05, console, approved by the user): pass, audibility to confirm

`probe\audioprobe.c`, four builds, one new call each, all ran and exited by
themselves (pids 191-194); logs `out\audioprobe-stage1..4.log`. The library is
loaded with `dlopen("libSceAudioOut.sprx")`, not linked, and a 15 s watchdog
thread guards against a call that never returns.

1. `dlopen` + `dlsym` of Init/Open/Output/Close: loaded, all four found.
2. `sceAudioOutInit`: 0.
3. `sceAudioOutOpen(0xff system user, port 0 main, 256 frames, 48000 Hz,
   format 1 = S16 stereo)`: handle 0x20000000. `sceAudioOutClose`: 0.
4. 187 grains of a 440 Hz tone at about -26 dBFS: every `sceAudioOutOutput`
   returned 0x100 (the 256 frames), 0.984 s in total, worst call 7.7 ms. So
   the call blocks at the device's pace and can clock playback. Drain
   (`Output(NULL)`) returned 0x100, close 0.

**The API accepts audio from a plain payload, but the user heard nothing.**
Three tries were silent: stage 4 twice (1 s, -26 dBFS) and
`out\audioprobe-stage4-loud.elf` (3 s, -12 dBFS, 562 grains in 2.986 s, all
accepted; `out\audioprobe-stage4-loud.log`). The open parameters are the same
as EVO's own (`sceAudioOutOpen(0xFF, 0, 0, grain, 48000, 1)` in
`SoundEffectEngine.cpp`), which is audible from its title. So the difference
is the process, not the call. Hypothesis, unconfirmed: the system only mixes
the main port of the foreground application, and a payload is not one. What
was on screen and which output device was in use is not known yet.

Fourth try, port type 1 (BGM): `out\audioprobe-stage4-bgm.elf`, same 3 s tone.
Open gave handle 0x20010008, 562 grains accepted in 2.983 s, close 0
(`out\audioprobe-stage4-bgm.log`). **Also silent.** The user was on the home
screen, HDMI to a capture card/TV, and could hear the menu music throughout,
so the output path and volume were fine.

Conclusion so far: from a loader payload, the main and BGM ports accept audio
and discard it. Untried: other port types (voice 2, personal 3, pad speaker 4,
aux 127), a real user id in place of 0xff, changing the process's identity.
These are guesses with no source behind them.

Remaining port types, tried 2026-10-05 11:26 at the user's request, system
user 0xff, four payloads about 4 s apart (`out\audioprobe-port2/3/4/127.log`).
None produced a tone, and **the sequence ended with an anomaly, so payload
audio probing is stopped**:

| run | pid | `sceAudioOutInit` | `sceAudioOutOpen` |
|---|---|---|---|
| port 2 (voice) | 198 | 0 | 0x80260011, no tone |
| port 3 (personal) | 199 | 0 | 0x80260011, no tone |
| port 4 (pad speaker) | 200 | **0x80260002** | 0x8026000f, no tone |
| port 127 (aux) | 201 | **never returned**; watchdog killed the process at 15 s | not reached |

`sceAudioOutInit` had returned 0 in all eight earlier runs. It degraded
(error, then a hang) across back-to-back runs. Cause unknown; candidates: the
two failed opens left something behind in the audio service, or processes
started 4 s apart overlapped the previous one's teardown (earlier runs were at
least 30 s apart). The meaning of the codes is not established (no header
here defines them). The console answered on 9021/8084/2121 and ping right
after. Whether its own audio is still fine was asked of the user.

**Outcome: the console's audio stopped working.** The user confirmed it and
had to restart and re-jailbreak (2026-10-05). So AudioOut calls from a loader
payload are not harmless: they can break system audio until a reboot. Which
of the four runs did it is unknown (they were sent as one sequence).

Rules taken from this: no AudioOut from a payload again, at all, unless the
user asks for a specific experiment knowing this; never send several console
probes as one unattended sequence; leave the
foreground-user variants (`out\audioprobe-port*-fguser.elf`, built, not sent)
unsent unless the user asks after the console's audio is confirmed healthy;
do not send any AudioOut payload again before that confirmation.

What is known to be audible: the same open call from a title (EVO,
ProsperoLight). So the dependable route is a title for the output.

Title build facts gathered (PC only):

- ProsperoLight's `build.ps1` only calls WSL; its title toolchain
  (ps5-native-app-boilerplate: `prospero-clang18`, lld, `ps5-native-tool`,
  generated `libc.prx`) is Linux/WSL. EVO builds in Docker.
- This PC already has WSL (Ubuntu-24.04 and Arch) from the MCLA project, and
  MCLA builds working titles there with the same boilerplate tooling
  (`D:\PS5\MCLA\ps5\title_build.sh`; the tool tree is `/root/ps5vk/PS5_Vulkan`
  on Arch). `D:\PS5\MCLA\ps5\title_support.c` lists libc functions that are
  null inside a title on 13.42 (isatty, link, symlink, readlink, pathconf,
  mkstemp) and `title_log.h` is a network log for titles.
- The Mirage brief said to reuse the Windows toolchain rather than set up
  WSL. Using the existing WSL for the title half is a decision for the user.

### Title toolchain and the audio-test title (2026-10-05, PC only)

The user approved the payload + title design and the use of the existing WSL.

- WSL `Ubuntu-24.04` (root) already had clang-18, lld-18, make, cmake, ninja.
  `~/mirage/ps5-native-app-boilerplate` is a shallow clone of
  blackbearreloaded/ps5-native-app-boilerplate (f98de73, 2026-10-04), the base
  ProsperoLight is built on. Its unmodified `make` built a title in 9 s (it
  fetches its own pinned SDK and generates `libc.prx`; `make doctor` only
  misses clang-format/clang-tidy, which are lint tools).
- `tools\build-title.ps1` copies the boilerplate to `~/mirage/title`, replaces
  its `src` with `title\src` from here (keeping the boilerplate's
  `demo_renderer.*` as the screen), runs `make init` and `make`, and copies
  the title folder to `out\title\PPSA99741`.
- `title\src`: `main.cpp` (status screen), `tone.cpp/.hpp` (audio thread: five
  2 s beeps of 440 Hz at -12 dBFS with 1 s gaps, same open call as EVO, then
  drain and close; never spins on a failing output call).
- Built: `out\title\PPSA99741` (title id PPSA99741, name "Mirage", about
  18.6 MB, mostly the boilerplate's default art). `eboot.bin` 16,275 bytes; one
  executable segment; imports are VideoOut, AudioOut, `pthread_create`,
  `snprintf` and a few libc/kernel calls. Art is the boilerplate's default.
- Uploaded 2026-10-05 11:39 with the user's go-ahead: all 8 files to
  `/data/homebrew/PPSA99741` over FTP (2121), sizes on the console match the
  local ones.
- **Result (user, 2026-10-05): launched from the home screen, all five beeps
  heard, screen ended on DONE.** So on 13.42 a title built with this
  toolchain starts, draws, creates a thread with `pthread_create`, and its
  AudioOut main port (system user 0xff, 256 frames, 48 kHz, S16 stereo) is
  audible. The same call from a loader payload was silent. The test title is
  still in `/data/homebrew/PPSA99741`; I cannot delete it, and later builds
  reuse the same id and folder.

### M4 code: decoder, loopback link, player (2026-10-05, PC only, not run)

Payload (`tools\build.ps1`, now also links pacbrew's libavcodec, libswresample,
libavutil and the libraries they were configured with: x264, lzma, iconv, z;
the ELF is about 28 MB):

- `src\pcm_link.h`: the link's format, shared with the title. Loopback TCP
  port 47010, payload to title only, 8 byte header (`MP`, type, length) and
  a body. Types: audio (S16 LE stereo 48 kHz), flush, stop.
- `src\pcm_server.c`: listens on 127.0.0.1:47010, one title at a time. Sends
  never block for long (200 ms send timeout, then the title is dropped and
  has to reconnect); with no title connected, messages are counted and dropped.
- `src\audio_pipeline.c`: ALAC with FFmpeg's decoder (the 36 byte cookie for
  352 frames, 16 bit, stereo, 44100 Hz, as UxPlay's GStreamer renderer uses),
  swresample to 48 kHz S16, AirPlay volume (dB) applied as a gain, flush.
  AAC-ELD (ct 8, mirroring) is logged and dropped for now.
- `src\main.c` wires them into the UxPlay callbacks. `-LinkTestTone` builds a
  variant with no AirPlay that only serves a 2 s beep every 3 s on the link.
- `build.ps1` now refuses to output an ELF with executable sections outside
  `.text`/`.init` or with a dynamic table readelf rejects. The second check
  exists because the first link-test build had a `PT_DYNAMIC` size that was
  not a multiple of 16 (the SDK's linker script stretches that segment over
  the relocation table); no ELF run so far had that, so the variant carries
  one extra relocation to avoid it rather than test the loader with it.

Title (`title\src`, `tools\build-title.ps1`, `tools\inspect-title.sh`):

- `player.cpp`: a link thread that connects to 127.0.0.1:47010 (retrying each
  second), parses messages into a 2 s ring buffer; an audio thread that owns
  AudioOut, starts once 200 ms is buffered, plays silence when dry, and keeps
  at most 1 s buffered by dropping the oldest audio.
- `main.cpp`: status screen: link state (with the connect errno while
  waiting), buffer in ms, seconds received, dry count, connects, bad messages.
- Imports now include `socket`, `connect`, `recv`, `close`,
  `pthread_mutex_lock/unlock` from the console's libkernel. Whether an
  unprivileged title may connect to loopback on 13.42 is the open point.

Built: `out\title\PPSA99741` (eboot.bin 17,811 bytes),
`out\mirage-linktest.elf` (90 s, beeps only), `out\mirage-test180.elf`
(180 s, full AirPlay + decoder). None of this has run.

Link test, run 1 (2026-10-05 11:46-11:50, console, each step approved):

- New title build uploaded over FTP with the title closed (eboot.bin,
  param.json, banner.txt; eboot.bin read back and matched by sha256).
- The user launched it: "WAITING FOR PAYLOAD ERRNO 61". 61 is ECONNREFUSED,
  the right answer with nothing listening, so **an unprivileged title on
  13.42 can create a socket and connect to loopback** with the plain
  libkernel `socket`/`connect`.
- `out\mirage-linktest.elf` (28,314,512 bytes, the largest payload sent so
  far) loaded and ran as pid 103; log `out\linktest-run1.log`. Listener up on
  127.0.0.1:47010, "title connected" within about a second, exactly 1000
  chunks of 10 ms sent per 10 s for 90 s (8,899 in all: the nanosleep pacing
  holds and no send ever timed out), clean stop.
- User's report: "LINK CONNECTED", buffer steady at about 190-200 ms for the
  whole test, beeps heard clearly, counters at the end DRY 1, CONNECTS 1,
  BAD 0. The one dry event is the end of the test (the payload stopped and
  the buffer emptied). A buffer that neither grows nor shrinks over 90 s
  means the payload's clock and AudioOut's clock agree closely enough at
  this length. **The link and the title's player work.**

### M4 run 1 (2026-10-05, console, approved by the user): AirPlay audio plays

`out\mirage-test180.elf` 0.2.0-dev (28,315,720 bytes), pid 104, with the title
open; log `out\m4-run1.log`. **First end-to-end playback: music from the
iPhone came out of the TV.**

- Payload: "ALAC decoder ready (FFmpeg 7.0.1)", title connected to the link,
  the iPhone connected and streamed ct 2. Zero decode errors and zero
  messages dropped for lack of a title in every status line. Frames out match
  packets x 352 x 48000/44100 (e.g. 152 packets -> 58,219 frames), about
  480,000 frames per 10 s, so decode and resample are correct in quantity.
- Volume: the phone's slider arrived as steps of 1.875 dB between 0 and
  -20.6 dB and was applied.
- End: ran its full 180 s (21,458 packets, 8,221,171 frames out, about 171 s
  of audio, still 0 decode errors), then stopped advertising, stopped the
  listener, destroyed everything and exited with "RESULT: mirage finished".
- User's report: music plays fine; one or two crackles near the start, then
  none; about 0.5 s behind the phone; volume, skip and pause all fine; title
  showed buffer 480-500 ms and DRY 2.
- Reading of that: the two dry events are the crackles at the start. The
  player starts at 200 ms buffered, the stream's first seconds do not arrive
  evenly, and the buffer then settled near 500 ms, which is also the delay
  the user heard. Fix candidates: a larger start threshold, and then proper
  timestamp-based playout, which removes the guesswork.

Phase 1's core works: discovery, pairing-free connect, FairPlay, ALAC decode,
resample, link, AudioOut from a title.

### Timed playout and one-step start (2026-10-05, PC only, built, not run)

The user chose: these two next, then mirroring.

Timed playout:

- Link format change (`src\pcm_link.h`): an audio message now starts with an
  8 byte time, the moment its first frame is due on the sender's timeline in
  this machine's CLOCK_REALTIME nanoseconds. The payload takes it from
  UxPlay's `ntp_time_local` (`raop_rtp.c:655-656`: the packet's RTP time
  mapped through the sync packets and the NTP exchange).
- Title (`title\src\player.cpp`): when a stream starts, its first frame is
  scheduled 300 ms after it arrived; every later frame plays at its own time
  plus that same offset. Once playing it plays grain after grain and only
  corrects an error above 40 ms (skips overdue audio, or waits). A step of
  more than 60 ms in the incoming times, a flush, or running dry starts a new
  schedule. Ring buffer is 3 s. Screen counters: DRY, LATE, RESTART, BAD.
- The delay is therefore "300 ms after the first packet arrived", not an
  absolute sync to the phone's clock. How far ahead of their due time packets
  arrive is not known yet; the payload now logs that lead (first 5 packets,
  every 50th up to 500, then every 500th) so the next run shows it.
- Not handled: slow drift between the phone's clock and AudioOut shows up as
  one 40 ms correction when it accumulates.

One-step start and lifetime:

- The title packages the payload as `/app0/mirage.elf` (`build-title.ps1`
  copies `out\mirage.elf` in via the boilerplate's `APP_ROOT_FILES`). When
  nothing answers on the link port it sends that file to the loader on
  127.0.0.1:9021 and closes the loader socket, at most 3 times per run and
  not more often than every 20 s. A payload that is already running (for
  example one I sent for a test) is simply used.
- The payload leaves by itself when its title has been gone for 10 s, or when
  no title connects within 120 s of its start (`pcm_server` notices the
  title's end of the connection closing). A second copy cannot start: the
  link port is its first bind.
- When the title starts it, the payload's log goes nowhere (the loader socket
  is closed). For test runs I send the payload myself to keep the log.

Built: `out\mirage.elf` 0.3.0-dev (28,315,984 bytes, no time limit),
`out\mirage-linktest.elf` 0.3.0-linktest, `out\title\PPSA99741` (eboot.bin
21,555 bytes plus `mirage.elf`; the folder is now about 47 MB).

Run A of 0.3.0 (2026-10-05, console, approved; log `out\m5-runA.log`):
**no audio, RESTART counting up constantly.** My bug.

- Cause: UxPlay's `ntp_time_local` is not a usable timeline on the console.
  The log's "lead" figures show it stayed near zero for the whole stream (it
  advanced 11 ms while 39.9 s of audio went by), so every message carried
  about the same time, the title saw each one as a jump back, and restarted
  its schedule for ever. I had taken the field's meaning from the source
  without checking a value. UxPlay's own renderer uses `ntp_time_remote`.
- What did work in that run: the title connected to a payload I had sent
  ("PAYLOAD STARTED 0" path), 5,451 packets decoded with 0 errors, volume
  steps arrived, and the lifetime rule: "title disconnected (closed by the
  title)", then "stopping: the title has been gone for 10 s", clean exit.

Fixes (0.3.1, built, not run on the console):

- The timeline is now the RTP sample clock itself (`rtp_time`, 44100 ticks a
  second, unwrapped to 64 bits in `cb_audio_process`), origin arbitrary. The
  title only uses differences. The payload logs "slip" instead of "lead":
  how much later than the first packet each one arrived, against the timeline.
- The title no longer trusts times blindly: three schedule restarts within a
  second and it ignores the times for the rest of that connection and plays
  in arrival order ("TIMES IGNORED" on the screen).
- **Host test** `tools\test-player.ps1` (`tests\host_player_test.cpp`): builds
  the real `player.cpp` in WSL with a stub AudioOut and a fake payload, with
  ASan/UBSan, for five arrival patterns: steady, burst, stall, flush, frozen
  (frozen is this bug). All pass: about 6.0 s of 6.0 s played, first sound
  309 ms after the first message, no late drops, no restarts; frozen plays
  5.8 s after 3 restarts. Run it before any title build goes to the console.
  It would have caught the failure mode, though not the wrong field.
- Helper scripts piped to WSL ended with a stray CR that made bash report
  exit codes 2/127 on success; each now ends in a comment that absorbs it.

Built: `out\mirage.elf` 0.3.1-dev (28,316,192 bytes), `out\mirage-linktest.elf`
0.3.1-linktest, `out\title\PPSA99741` (eboot.bin 21,843 bytes + `mirage.elf`).
Uploaded to the console 2026-10-05 (title closed, both files verified by
read-back).

Run A of 0.3.1 (2026-10-05, console, approved; log `out\m5-runA2.log`, pid 107):

- Payload side clean: title connected, 9,924 packets, 3,802,123 frames out,
  0 decode errors, three streams (two "format + flush" in the middle: track
  skips, each with a new RTP base), then "closed by the title", "gone for
  10 s", clean exit.
- **Arrival jitter measured for the first time** (slip against the RTP
  timeline, first stream): steady state within about +-15 ms, isolated
  packets at +44 and +56 ms, one at +629 ms (a 32 byte packet at number 1000;
  looks like the sender pausing without a flush). So ordinary jitter is far
  inside the 300 ms start delay; 150 ms would probably do.
- The slip figures after a skip are meaningless (the baseline is not reset
  when the RTP base changes); cosmetic, to fix.
- User's report: **music plays fine**, including across pause and resume.
  The buffer reading moved between 300 ms and 2 s each time they paused and
  restarted. That fits the scheduler: after a resume the phone sends ahead of
  real time for a while, the surplus sits in the ring (3 s), and playback
  stays on its schedule, so a large buffer is not a large delay. Counters
  (DRY/LATE/RESTART) and the delay behind the phone were not reported.
  If a resume ever delivers more than 3 s ahead, the oldest audio would be
  dropped; not seen.

Run B, one-step start (2026-10-05, console, the user ran it; no log by design):
the user launched the title with nothing running, it started the payload,
music played, they closed the app. **The title can start the payload by
sending `/app0/mirage.elf` to 127.0.0.1:9021.** Exact screen text and
counters were not reported. Afterwards an mDNS query from the PC for
`_airplay._tcp` got answers from three other LAN devices and none from the
console or naming Mirage, which fits the payload having stopped by itself
(not proof: Mirage's answer to that query was never checked while running).

Items 1 (timed playout) and 2 (one-step start, payload lifetime) are done.
Phase 1 leftovers, deferred by the user's choice to do mirroring next: real
MAC as device id and a kept key file, packet-loss concealment, drift
correction finer than a 40 ms step, network-change handling, a shorter start
delay (150 ms looks safe), a payload log reachable when the title starts it,
proper title art and exit.

Not handled yet: audio/NTP timestamp sync (playback is simply buffer-delayed),
packet loss concealment, the title starting the payload by itself, a clean
title exit, the real MAC as device id.

Design (approved by the user 2026-10-05): two processes.

- `mirage.elf`, the payload built today, keeps all networking and the AirPlay
  protocol. Everything it needs is proven on the console.
- A small title owns AudioOut (and VideoOut/VideoDec2 in phase 2), connects
  *out* to the payload on 127.0.0.1 and receives decoded PCM (later H.264
  units). It can start the payload by sending the ELF to 127.0.0.1:9021 the
  way ProsperoLight sends its helper. No credential patching anywhere.
- To check before building on it: that an unprivileged title on 13.42 can
  connect to a loopback port (ProsperoLight does exactly this to 9021).

## Phase 2 plan: screen mirroring (proposed 2026-10-05, nothing built)

What the sources say, all untested by us:

- Hardware decode only works from a title. EVO's `videodec2-abi.md`: from
  every payload context `sceVideodec2Decode` failed with errno 5200; from a
  title the whole sequence returns 0. That matches our split: the title
  decodes, the payload forwards.
- The same document has the full recipe (sysmodule 207, compute queue,
  decoder config for H.264 High 5.1 at 1920x1088, direct-memory pools,
  `sceVideodec2Decode` with a flush when no picture comes out, NV12 output in
  the frame pool). ProsperoLight's `moonlight_stream.cpp` is the working code.
- AirPlay mirroring delivers H.264 as SPS/PPS plus access units in
  `video_process` (already counted by the payload) and AAC-ELD audio
  (`ct 8`, which the audio pipeline still drops).
- Rotation: the decoder is sized at creation; recreate on a new size (Q3).

Steps, each a separate console test:

- V1 payload: forward video over the link (new message types for codec
  config/size and access units with the same kind of timeline as audio);
  advertise as today. Title: only count and show what arrives.
- V2 title: bring up VideoDec2 and decode, discard pictures, show counters
  (decoded, errors, size). First use of the decoder on this console.
- V3 title: show the picture. Simplest first: convert NV12 to the RGBA
  frame buffer on the CPU (the boilerplate's VideoOut path, known to work).
  Likely fine at 1080p30, not the final answer.
- V4 audio for mirroring: AAC-ELD decode in the payload (FFmpeg's `aac`
  decoder; needs the right extradata), and audio/video sync on the shared
  timeline.
- V5 rotation and resolution changes; then zero-copy presentation through AGC
  as ProsperoLight does, HEVC, 4K.

### V1 built (2026-10-05, PC only, not run on the console)

- Link (`src\pcm_link.h`): three new message types. `VIDEO` (4): 12 byte
  prefix (time, codec, NAL count) then one Annex-B access unit; `VIDEO_SIZE`
  (5): source and picture size; `VIDEO_RESET` (6). The largest body is now
  4 MiB (was 64 KiB).
- Payload 0.4.0-dev: `cb_video_process` forwards every unit with UxPlay's
  `ntp_time_remote` as its time, and logs that and `ntp_time_local` for the
  first five units and every 300th, so the console run shows whether either
  advances with the pictures (the audio lesson). `video_report_size` and
  `video_reset` are forwarded. UxPlay already delivers Annex-B with SPS/PPS
  put in front of the picture that needs them (`raop_rtp_mirror.c`).
- Title: `video.cpp` inspects and counts units (keyframes, SPS, largest,
  per-second rate and bitrate, span of the times); two new screen lines.
  Nothing is decoded. Audio messages longer than one second are now rejected.
- Host test: new `video` pattern (30 units a second with keyframes, one
  1.5 MB unit, size and reset messages, audio alongside). All six patterns
  pass under ASan/UBSan.
- Mirroring audio is AAC-ELD (`ct 8`), still dropped by the pipeline, so a
  mirroring session is silent in V1.
- Built: `out\mirage.elf` (28,316,288 bytes), `out\title\PPSA99741`
  (eboot.bin 23,619 bytes). Uploaded 2026-10-05 with the title closed, both
  files verified by read-back.

V1 run 1 (2026-10-05, console, approved; log `out\v1-run1.log`, pid 111):
**the iPhone mirrored to Mirage and the video reached the title's link.**

- "video: source 498x1080, picture 498x1080", codec 1 (H.264). So with the
  default 1920x1080 display the phone sends a portrait picture already scaled
  to 1080 lines high.
- 881 units in about 30 s (about 30 a second). First unit 3 NAL, 9,329 bytes
  (SPS + PPS + IDR); ordinary units 1 NAL, roughly 1-8 KB; one of 28 KB seen.
- **Video times: `ntp_time_remote` is good, `ntp_time_local` is 0.** Remote
  time is Unix-epoch nanoseconds on the sender's clock and steps about 33 ms
  per unit (…484571, …484613, …484646, …484679, …484713 ms; unit 300 is
  9.99 s after unit 1). The title's SPAN counter uses it.
- No audio packets at all during the session (0 packets, and no "compression
  type 8" line). Either the phone sends no audio stream while nothing is
  playing, or mirroring audio needs something Mirage does not do yet. To
  check in V4 with sound playing on the phone.
- Only one size report and no SPS after the first: no rotation happened in
  this run as far as the log shows.
- End: two "video: reset, type 1" (RTP shutdown), connection closed, then the
  title closed and the payload left after 10 s. No errors or warnings.
- User's report: the mirror connected and stayed streaming. (The title's
  counters were not read out.)

### V2 built: hardware decoder in the title (2026-10-05, PC only, not run)

- `title\src\vdec.cpp/.hpp`: one decoder thread. Bring-up in the order and
  with the values of ProsperoLight/EVO's verified write-up (sysmodule 207,
  compute queue, H.264 High 5.1 up to 1920x1088, 4 DPB frames, pipeline depth
  1, affinity 0x3f, priority 700; flexible memory for the CPU part, direct
  memory type 12 with protections 0x33/0x32; three 8 MiB input slots and
  three frame slots; reset). Then `sceVideodec2Decode`, with a
  `sceVideodec2Flush` when a unit is accepted but no picture comes out.
  Structure sizes are pinned with static_asserts (72/72/16/24/48/32/56).
- Units are queued (16) by the link thread and decoded on the decoder
  thread; it waits for a keyframe at the start, after a reset, after an
  error and after a queue overflow. Pictures are **not read or shown**: only
  what the decoder reports (size, pitch, format) is recorded.
- Every bring-up step is recorded before its call, so a hang or a failure
  shows on the screen as the stage name and the result code. Screen: a
  DECODER line, a PICTURES/NONE/ERRORS/DROPPED line and a size line.
- Build: `libSceVideodec2` is not among the SDK's import stubs.
  `title\stubs\videodec2_link_stub.c` is ProsperoLight's link stub, unchanged;
  `build-title.ps1` builds it into a `.so` and copies it into the work tree's
  SDK stub directory, because this boilerplate version links every stub there
  and has no `APP_IMPORT_STUBS`. `-DecoderBringupOnly` builds the variant that
  stops after the bring-up (it does not even import Decode/Flush).
- Host test: a fake decoder in `tests\host_player_test.cpp` checks the call
  order, the sizes passed, 16K alignment and the start code of each unit, and
  holds back every third picture to exercise the flush. Video pattern: 180
  units, 180 pictures, 0 errors, queue never above 1. All six patterns pass.
  Leak reporting is off in that test (the decoder's memory is for the life of
  the process).
- Not handled yet: a change of picture size mid-stream (rotation), H.265,
  release of the decoder, sizes above 1920x1088.
- V2a, bring-up only (eboot.bin 28,101 bytes; needs `libSceVideodec2.prx` and
  `libSceSysmodule.sprx` in addition to the earlier modules): uploaded and
  launched by the user 2026-10-05. **Screen: "DECODER READY".** So on 13.42,
  from this title: sysmodule 207 loads, the compute queue is allocated, the
  decoder memory query, flexible and direct memory maps, `CreateDecoder`, the
  pools and `Reset` all return 0. No crash.
- V2b, full decode (eboot.bin 29,685 bytes): uploaded, the user mirrored the
  phone. **"Pictures climbing consistently, errors 0."** So
  `sceVideodec2Decode` works on real AirPlay frames from this title. The
  size/pitch line and the timings were not read out.

### V3 built: the picture on screen, CPU path (2026-10-05, PC only, not run)

- `vdec.cpp` now copies each decoded picture out of its frame slot at once
  into ordinary memory (tight NV12, up to 1920x1088), under its own lock.
  It only does so when the decoder's report is the 8 bit layout the code
  expects (pitch_bytes == pitch >= width, sizes within the slot); otherwise
  nothing is shown and the status line still tells the size and pitch.
  First read of the decoder's frame memory from the CPU: it is mapped with
  protection 0x32 ("CPU write + GPU"), EVO reads it the same way; whether the
  read is slow (write-combined memory) shows as COPY ms on the screen.
- `mirror_view.cpp`: NV12 to 0xAABBGGRR, BT.709 limited range, nearest
  neighbour, scaled to fit the 1920x1080 frame and centred with black bars,
  cropped to the size the sender reports (the decoder pads to 16).
- `demo_renderer.*` is now Mirage's own copy of the boilerplate's file with
  one addition (`Canvas::pixels()` and the frame size).
- `main.cpp`: while a picture exists it takes the whole screen with one small
  yellow line of figures (pictures, errors, size, worst draw and copy time,
  fps); the status screen returns when the stream resets.
- Pictures are shown as soon as they are decoded; no timing against the
  video times yet, no audio in mirroring yet.
- Host test: the fake decoder returns a real 1920x1088 picture at pitch 2048
  with junk in the padding; the test draws it full-frame and as a portrait
  crop and checks colours, the last pixel, the black bars and that no padding
  shows. All six patterns pass.
- Built: `out\title\PPSA99741` (eboot.bin 32,965 bytes), uploaded.

V3 run 1 (2026-10-05, console, approved): **scrambled picture.** The user's
screenshot shows the overlay "PICTURES 1096 ERRORS 0 512 X 1088 DRAW 2 MS
COPY 2 MS FPS 30" over speckled dashes across the full width.

- Good news in it: the decoder reports **512 x 1088** for the phone's 498x1080
  picture (padded to 16), the layout check passed (8 bit, pitch == pitch
  bytes), reading the frame memory from the CPU works and is fast (copy 2 ms
  worst), drawing 2 ms, 30 fps, no errors, no crash.
- Cause of the scramble, mine: **the boilerplate's frame buffer is tiled**,
  not linear (`tiled_byte_offset` in `demo_renderer.cpp`: 128x128 pixel
  blocks of 64 KiB with the pixel bits interleaved). I added a raw pixel
  pointer and wrote rows linearly without reading how the renderer addresses
  pixels. The host test could not catch it: its canvas was linear too.
- Fix (built, not run on the console): `Canvas::write_row(y, row)` in our
  renderer copy writes one whole row through the tiling (the offset is an XOR
  of an x term and a y term plus the block base, so two small tables do it);
  the raw pointer is gone. `draw_mirror` now hands finished rows to a writer.
- New host test `tests\host_tiled_test.cpp`: `write_row` against the
  boilerplate's own per-pixel drawing (known right on the console): every
  pixel lands on a distinct place, whole rows match rectangles byte for byte,
  single pixels match at block edges. Passes, as do the six player patterns.
- Rebuilt `out\title\PPSA99741` (eboot.bin 33,093 bytes), uploaded.

V3 run 2 (2026-10-05, console, approved): **the phone's screen is on the TV.**
User's report: picture correct, colours right, smooth, very quick reaction
with not much delay, no audio (expected: mirroring audio is V4). So the
tiled row writer is right on real hardware, BT.709 limited range looks right,
and showing each picture as soon as it is decoded is good enough at 30 fps
for now. Still untested: rotation and other sizes, landscape (a full
1920x1080 conversion each frame), long sessions.

### V4a built: mirroring audio decode (2026-10-05, PC only, not run)

- `src\audio_pipeline.c` now has two decoders: ALAC as before and FFmpeg's
  `aac` with the AAC-ELD AudioSpecificConfig `f8 e8 50 00` (44.1 kHz stereo,
  480 samples per frame; the value UxPlay's GStreamer renderer uses). The
  resampler is opened from the first decoded frame's real format and
  reopened when the format changes. A missing AAC-ELD decoder is reported at
  start, not fatal.
- Frames whose first byte is not plausible for their format are skipped and
  counted ("invalid"): ALAC must start 0x20, AAC-ELD 0x8c-0x8e or 0x80-0x82,
  the same check UxPlay's renderer makes (a wrong first byte means the packet
  was not decrypted properly). This check is new for ALAC too.
- Not tested anywhere: there is no host build of this pipeline and no
  captured AAC-ELD packet. Whether pacbrew's FFmpeg 7.0.1 decodes these
  frames shows as "decode errors" in the payload's status line.
- Audio stays on its RTP-clock timeline and plays 300 ms after it arrives,
  while pictures are shown at once, so sound will trail the picture by
  roughly that much in this build. The payload now also logs each sampled
  audio packet's first byte and UxPlay's `ntp_time_remote`, to see whether
  that field can put audio on the video's timeline (V4b).
- Open from V1: in that mirroring session no audio packets arrived at all.
  This run needs sound playing on the phone to tell "none sent while silent"
  from "Mirage is missing something".
- Built: `out\mirage.elf` 0.5.0-dev (28,316,520 bytes); the title's eboot.bin
  is unchanged (33,093 bytes) but its packaged `mirage.elf` is the new one.
  Uploaded with the title closed.

V4a run 1 (2026-10-05, console, approved; log `out\v4a-run1.log`, pid 121):
**mirroring sound plays.** User: audio plays, sounds clean, slightly behind
the picture, picture still smooth.

- "ALAC decoder ready, AAC-ELD decoder ready". The audio stream only appeared
  about 15 s into the mirroring session, when sound started on the phone
  ("audio: format ct 8, 480 samples per frame, screen 1"). So the V1 question
  is answered: **the phone sends no mirroring audio while nothing plays.**
- 4,107 AAC-ELD packets, 2,145,681 frames out (853 packets gave 445,632
  frames, matching 480 x 48000/44100 each), 0 decode errors, 0 invalid. First
  bytes 0x8d/0x8e as expected. pacbrew's FFmpeg `aac` decodes AirPlay's ELD.
- Slip against the RTP timeline within about +-15 ms; one event of +418 ms
  with 9 sequence numbers missing (seq 22027 -> 22536 over 500 packets).
- **Audio `ntp_time_remote` is usable**: Unix-epoch ms on the sender's clock,
  advancing with the samples (49 packets = 533 ms on both counts), the same
  clock the video units carry. That is the common timeline for sync.
- Video in the same run: 2,130 units, 498x1080 H.264.

### V4b built: audio/video sync (2026-10-05, PC only, not run)

- Payload 0.6.0-dev: AAC-ELD audio messages carry `ntp_time_remote` (ALAC
  keeps the RTP timeline, known good).
- Title: the decoder keeps the last twelve pictures with their times.
  `player.cpp` exposes the sound's schedule (sender time -> local play time)
  while it is valid; `mirror_view.cpp` shows the newest picture whose time is
  due by that schedule ("SYNC" in the overlay). With no sound scheduled, or
  if the picture's time is more than 2 s from "due" (different clocks), it
  shows the newest picture at once ("FREE"), as V3 did.
- Start delay cut from 300 to 150 ms for all audio, on the measured jitter.
  Pictures in a session with sound therefore appear about 150 ms later than
  in V3; with no sound playing they are as immediate as before.
- Not accounted for: AudioOut's own output latency (unknown), which would
  leave the sound a little behind the picture still; one constant to tune by
  ear.
- Host test: video on the audio's timeline; "which picture is due" checked
  (176 of 180 for the 176th's time, and it does not go back); stall pattern
  recalibrated to 100 ms (inside the delay), new `gap` pattern (400 ms: dry
  once, recovers). Tiled test and all seven patterns pass.

V4b run 1 (2026-10-05, console, approved; the title started the payload, no
log): **sound and picture line up, overlay shows SYNC.** One fault: after a
pause and resume the sound *sometimes* comes back behind the picture and
stays behind.

- Cause as I read it (no log of the event): after a pause the buffer is dry,
  so the next sound started a new schedule from the arrival of its first
  message. If that message arrived late (a burst after the pause), the whole
  schedule sat that much later, for good. The pictures follow the sound's
  schedule only as far back as the twelve kept pictures reach (400 ms), so
  beyond that the sound was behind the picture.
- Fix (0.6.1, built, not run on the console): audio messages of a mirroring
  session carry a new flag, `PCM_LINK_FLAG_LIVE` (header byte 3), meaning
  their times are the sender's wall clock. For such a stream the title keeps
  a baseline, the smallest "arrival minus time" seen lately (allowed to rise
  0.2 ms a second for clock drift, reset if it moves by more than a second,
  on reconnect or when times are distrusted). A schedule starts from the
  baseline, not from the one message at hand, and a running schedule more
  than 80 ms behind its baseline is pulled back (RESCHED on the status
  screen); overdue sound is skipped. Music (no flag) keeps the old rule and
  plays everything from the first message, since the phone sends music
  seconds ahead.
- Host test `resume`: live stream, one second of silence, then the first
  300 ms arriving late in one burst. 1.5 s later the buffer is 146 ms (it
  would be about 450 with the old rule; I did not run the old code against
  this test to confirm that number). All nine tests pass.
- Built: `out\mirage.elf` 0.6.1-dev, `out\title\PPSA99741` (eboot.bin
  34,277 bytes). Uploaded with the title closed, both verified.

V4b run 2 (2026-10-05, console, approved; title-started payload, no log):
**pause and resume keep sync.** User: paused and resumed several times, the
sound stays in sync now. Remaining observation: sometimes a short stretch
with no sound right after pressing play; when the sound comes back it is in
sync. Probably the overdue sound being skipped at a resume (by design) plus
the 150 ms start delay, possibly the phone's own gap; not measured, and the
RESCHED/LATE counters were not read out. A candidate for later: fade in
after a skip, or play the late sound faster instead of dropping it.

### V5 rotation (2026-10-05)

Rotation test with the V4b build (console, the user ran it): **the picture
freezes when the phone is turned and does not come back in portrait.**
Status lines from the user's screenshot: "PICTURES 144 NONE 0 ERRORS 4
DROPPED 842 LAST 811D0300", "512 X 1088 PITCH 512 FORMAT 0 QUEUE 3 WORST 7 MS
COPY 4 MS". So the decoder refuses the unit with the new parameter set
(0x811D0300, meaning not established) and then every later keyframe too,
including the portrait ones: four keyframes tried, four errors, everything
between dropped while waiting for a keyframe. Also seen there: pitch equals
the width (512), decode worst 7 ms, copy 4 ms.

Fix (built, not run on the console):

- The decoder thread remembers the stream's SPS. When a unit carries a
  different one, it calls `sceVideodec2Reset` before feeding it (SEQ counter).
- After a decode error it resets the decoder; after a second error with
  nothing decoded in between it deletes and re-creates the decoder with the
  same configuration on the same memory, as ProsperoLight does (NEW counter),
  retrying at each keyframe if that fails. RESET shows the last such result.
- A stream reset from the sender also resets the decoder before the next unit.
- Unknown until it runs: whether a reset is enough for the real decoder to
  accept a new SPS, or whether it takes the re-creation, or neither.
- Host test: the fake decoder now refuses a changed SPS without a reset, as
  the console's did; new `rotate` pattern (two changes, one unrelated failed
  call): 2 sequence changes, 1 error, no re-creation, 159 of 180 pictures.
  All ten tests pass.
- Built: `out\title\PPSA99741` (eboot.bin 35,429 bytes); the payload is
  unchanged (0.6.1). Uploaded with the title closed, verified.
- **Result (user, 2026-10-05): "worked great, switched fast a few times
  without issue."** Portrait to landscape and back several times. The
  counters were not read out, so whether the reset alone did it or the
  decoder was re-created is not known; either way the handling is enough.

Phase 2 state: V1-V5 (rotation) done on the console.

### Polish pass (2026-10-05, PC only, built, not run on the console)

Asked for by the user, with the name to be "Mirage - <console name>" so that
several consoles can be told apart.

- **Name.** The link now carries one message the other way: the title sends
  `PCM_LINK_HELLO` (type 7) with the console's name right after connecting.
  It asks the system with `sceSystemServiceParamGetString(5, …)` (5 is the
  PS4 SDK's SYSTEM_NAME id; unverified on PS5, and the title already links
  that library). The payload waits for a title and its hello (3 s after the
  connect at most), keeps printable ASCII without dots, at most 40
  characters, and advertises "Mirage - <name>", or plain "Mirage" if the name
  is empty or does not come. So the payload no longer advertises before a
  title is connected. The title's status screen shows the listed name, or
  "NO CONSOLE NAME" with the call's result.
- **Device id.** The payload asks `sceNetGetMacAddress` (libSceNet, already
  linked) and uses the console's MAC as the AirPlay device id, with the old
  made-up one as the fallback. Unverified on the console.
- **Key file.** The payload tries `/data/mirage/receiver.pem` (mkdir, then a
  write probe) and gives the path to UxPlay if it can write there; otherwise
  it logs why and makes a new key each start as before. A loader payload may
  well be confined away from `/data` (the tailscale daemon changes its root
  directory through the kernel to get there, which Mirage does not do).
- **Log.** `src\log_server.c`: `say()` also appends to a 256 KiB ring in
  memory; anyone connecting to TCP 47011 on the console gets a copy and is
  disconnected (read-only, LAN-wide, a development aid; it names connecting
  devices). This replaces the lost stdout when the title starts the payload:
  `curl.exe -s telnet://<console-ip>:47011`.
- **Debug line.** Off by default; `build-title.ps1 -DebugOverlay` brings it back.
- Host test: the fake payload now requires the hello with the console name
  from a stubbed system call. All ten tests pass.
- Built: `out\mirage.elf` 0.7.0-dev (28,317,160 bytes), `out\title\PPSA99741`
  (eboot.bin 37,125 bytes). Not uploaded.

### Idea from the user: payload always loaded, title opened on demand (not built)

Question: could the ELF stay loaded and open the app when someone connects?

- Plausible. The SDK's `libSceSystemService` stub exports
  `sceLncUtilLaunchApp`, `sceSystemServiceLaunchApp` and
  `sceLncUtilGetAppIdOfRunningBigApp`; nuvio's notes mention a ShadowMount
  control API on loopback 10101 that launches titles. None of it has been
  tried from a Mirage payload on 13.42.
- What it would take: an autoload entry for the payload (the user's Payload
  Manager list, a change on the console to be asked for); the payload not
  leaving when no title is around; the payload knowing the console's name by
  itself (a cached copy from the last hello, or loading the system library);
  launching PPSA99741 when a client connects; holding the session for the
  several seconds the title needs to start.
- The catch: starting a title while a game runs closes or suspends the game,
  and anyone on the LAN could trigger that by picking the console. Sensible
  rule: launch only from the home screen (no big app running); otherwise
  show a notification and refuse or wait.
- Also unknown: behaviour across rest mode, and the cost of an always-on
  28 MB payload (the tailscale daemon's 60 MB has been fine).

**Decided by the user (2026-10-05): build it, with two ways to start.**

1. Launch the title; it spawns the payload in the background (as now).
2. A payload ELF for the autoloader that starts with the console and opens
   the title when someone connects.

A release is to offer both: the title, and the ELF as a separate download.
The home-screen-only rule for launching was proposed with the idea and not
objected to. Not started; the polish build goes to the console first.

Polish build uploaded 2026-10-05 (eboot.bin 37,125 bytes, payload 0.7.0-dev,
both verified by read-back).

Polish run 1 (2026-10-05, console; the title started the payload; log read
over port 47011 into `out\polish-run1.log`, pid 132):

- **The log port works**: the whole log of a title-started payload, read from
  the PC while it ran.
- **Device id: works.** `sceNetGetMacAddress` gave <console MAC>.
- **Key file: a loader payload can write under `/data`.** "a key will be
  written to /data/mirage/receiver.pem": mkdir and the write probe succeeded
  with no privilege change. (So `/data/mirage` now exists on the console.
  Whether UxPlay then wrote and can reread the key is not checked yet.)
- **Console name: failed.** The title's hello carried an empty name; the
  screen says NO CONSOLE NAME (the user did not give the code). So
  `sceSystemServiceParamGetString(5, …)` does not return the name from this
  title on 13.42. The receiver was listed as plain "Mirage".
- **Music (AirPlay audio) did not work for the user; mirroring did.** The
  payload's side of the music session looks normal: the iPhone connected,
  ct 2, 1,262 ALAC packets over about 20 s in two streams, 483,475 frames
  out, 0 decode errors, 0 invalid, title connected, nothing dropped for lack
  of a title, arrival slip within a few ms. So the sound reached the link
  and was lost, silent or unheard, in the title, or the failure was on the
  phone. Music was last heard working with title/payload 0.3.1; every test
  since (V1 to V5) was mirroring, whose sound takes the "live" path added in
  0.6.1. During this run's mirroring only 121 AAC-ELD packets arrived (about
  1.3 s), so it does not show that mirroring sound still works either.
- Not the cause, checked on the PC: a new host pattern `music` (timeline
  starting at exactly 0, as the payload stamps ALAC) plays all 6 s.
- Open: what exactly the user saw and heard, and the title's audio counters
  during music (BUFFER, RECEIVED, DRY, LATE, RESTART, RESCHED, TIMES IGNORED).
- **Resolved by the user: it was not music.** "AirPlay did not work" meant
  casting a YouTube video, which is phase 3 (HLS) and not built; Mirage does
  not advertise HLS, so the YouTube app fell back to sending the video's
  sound as plain AirPlay audio, which is the ct 2 stream in the log. There
  is no evidence of a music fault. My mistake was reading "AirPlay" as
  "audio-only AirPlay" and building a theory on it instead of asking first.
  Whether that fallback sound was heard was not said.

Console name, second method (payload 0.7.1-dev, built and packaged, not
uploaded): when the title's hello has no name, the payload sends a Remote
Play discovery request to 127.0.0.1:9302 and takes the reply's "host-name:"
line (known to answer on this console from the tailscale project's
`rpprobe.c`; only while Remote Play is enabled). Needs no title, so it also
serves the always-loaded mode.

Polish run 2 (2026-10-05, console; payload 0.7.1, pid 134; log
`out\polish-run2.log` read over port 47011): **the name works.**
"no name from the title; Remote Play discovery says "Living Room PS5"",
advertising as "Mirage - Living Room PS5"; the user confirms the phone lists
the correct name. "using the key in /data/mirage/receiver.pem": the key
written in run 1 (119 bytes) was found and reused. Device id again
<console MAC>. The polish pass is done. Leftover: the title's status
screen still shows "NO CONSOLE NAME" from its own failed call; it should
show the name the payload settled on (needs a message back, or drop the line).

### Always-loaded mode: design (2026-10-05, nothing built)

One payload binary, two ways it gets started, told apart by who is there:

- **Started by the title** (as now): the title sends `/app0/mirage.elf` to
  the loader when nothing answers on the link port.
- **Started by the autoloader**: the same ELF, downloaded separately and
  listed in the user's autoloader, running from boot with no title.

Behaviour to build in the payload:

1. Do not wait for a title before advertising: take the console's name from
   Remote Play discovery (the title's hello stays as a second source), then
   advertise at once.
2. Never leave because no title is connected (today: 120 s without one, or
   10 s after one closes).
3. When a client starts a session and no title is connected: if no big app
   is running (`sceLncUtilGetAppIdOfRunningBigApp` or the SystemService
   equivalent says none), launch PPSA99741 (`sceLncUtilLaunchApp` /
   `sceSystemServiceLaunchApp`); if a game is running, do not launch, send a
   system notification ("<device> wants to AirPlay - open Mirage") and let
   the session run without output or refuse it.
4. Keep the session while the title starts (a few seconds); audio and video
   before the title connects are dropped as today.

Unknowns, each a console probe of its own: loading `libSceSystemService`
into a payload (dlopen, as the audio probe did with AudioOut) and what those
launch and running-app calls need and return on 13.42; whether a second
instance is refused cleanly when both the autoloaded payload and a
title-sent copy exist (the link port bind is the guard: the second exits);
rest mode; what the phone shows during the seconds the title takes to start.

Release shape: the title folder/package (which contains the ELF) and the
ELF by itself for autoloaders.

Decided by the user (2026-10-05): the payload keeps running after the title
closes; with a game running, notify and do not interrupt.

Prior art found in the references: two small loader payloads that do exactly
this, `nuvio-ps5-homebrew\vendor\control.c` (action 3) and EVO's
`tools\ps5-controllers\launch.c`, both after shsrv: link `-lSceUserService
-lSceSystemService`, `sceUserServiceInitialize`, `GetForegroundUser`,
`sceSystemServiceGetAppIdOfRunningBigApp`, then
`sceSystemServiceLaunchApp(title, args, &context)` with a 40 byte context
(size/user/options, crash_report, flags, argv). Plan: keep these calls out
of the long-running payload and put them in a tiny helper ELF that the
payload sends to the loader on 127.0.0.1:9021 when it needs them, reading
the helper's answer from the loader socket (as the tailscale daemon did with
its icon helper), so no system service library is ever loaded into the
28 MB process.

Probe `probe\lncprobe.c` (ACTION 1 ask only, ACTION 2 ask then launch):

- ACTION 1 run (2026-10-05, console, approved; `out\lncprobe-action1.log`,
  pid 135): the payload started with those two libraries linked,
  `sceUserServiceInitialize` 0, foreground user 0x1b9c6c78,
  **`sceSystemServiceGetAppIdOfRunningBigApp` = 0x4018** ("an app is
  running"; which app was on screen was asked of the user), terminate, clean
  exit. The library printed one line of its own on the loader socket first
  (`[SceLncUtil] … getAppStatus: LNC_ISOK::0x80940004`). Console fine after.
- Its first build had a `PT_DYNAMIC` size not a multiple of 16 (0x1218); the
  probe has a `-DDYNAMIC_PAD` switch for that, and the sent ELF passed readelf.
- The user confirmed the Mirage title was the app open during that run, so
  0x4018 was Mirage's app id.
- ACTION 2 run (2026-10-05, console on the home screen, approved;
  `out\lncprobe-action2.log`, pid 136): **running big app = 0xffffffff (-1)
  on the home screen**, then `sceSystemServiceLaunchApp("PPSA99741")`
  returned **0x00006018** (a positive app id: accepted), terminate, clean
  exit. Whether the title appeared on screen was asked of the user.
- So the rule for the payload is: launch when the running-app call returns
  a value <= 0, do not when it returns > 0.
- The user confirmed the Mirage title opened by itself on the TV.

Always-loaded mode, built (payload 0.8.0-dev, 2026-10-05, PC only, not run):

- `helper\launcher.c` -> `build\mirage-launcher.elf` (also copied to `out\`):
  the only code linked with UserService/SystemService. Asks for the
  foreground user and the running app, launches PPSA99741 only if none is
  running, prints `MIRAGE-LAUNCHER user=… running=… launch=…`.
  `tools\build.ps1` builds it first (with the dynamic-table check and the pad
  retry) and the payload embeds it (`src\launcher.c`, `.incbin`).
- `src\launcher.c`: `launcher_run` sends the helper to 127.0.0.1:9021 and
  polls the loader socket up to 8 s for that line; `launcher_notify` is
  `sceKernelSendNotificationRequest` from libkernel (no new library).
- `src\main.c`: no waiting for a title. The name comes from Remote Play
  discovery, asked for up to 20 s (a title's hello still counts if one is
  there). The payload no longer leaves for lack of a title. When a device
  asks to connect and no title is connected, a detached thread runs the
  helper, at most once every 20 s: title opened -> log; an app running ->
  notification "<device> wants to AirPlay to this console. Open Mirage to see
  and hear it."; helper silent -> log. Status lines every 10 s only while
  something is connected, else every 5 minutes.
- Replacing a running payload: a copy that finds the link port taken sends
  `PCM_LINK_QUIT` (type 8) to it and retries the bind for about 10 s; the old
  one logs "a newer copy of the payload asked for the place" and shuts down
  (log port first, then the link port). This is how the ELF is updated or
  restarted now that it never exits by itself.
- Late title, video: a mirroring stream has its only keyframe at the start,
  so a title opened seconds later could never show it. The link server now
  keeps the video units since the last keyframe while no title is connected
  (up to 16 MiB; dropped whole if exceeded, cleared on a stream reset) and
  hands them, with the last size message, to the title when it connects,
  with a 5 s send timeout for that hand-over. Sent-or-kept is decided under
  one lock so a unit cannot go twice.
- Title: the decoder queue is 64 units and the link thread waits up to 5 s
  for room instead of dropping, so the hand-over burst is decoded whole.
  eboot.bin 37,029 bytes.
- Host test: new `late` pattern (150 units at once from the only keyframe,
  then real time): 180 of 180 decoded, queue reaching 64. All twelve tests pass.
- Audio that arrives before the title connects is still simply dropped.
- Not built: autoload registration (the user's Payload Manager list), the
  payload remembering the name without Remote Play, the title's status
  screen showing the listed name, anything for rest mode.

Always-loaded run 1 (2026-10-05, console, approved; payload 0.8.0 sent by
hand in place of an autoloader, pid 139; log `out\always-run1.log` read over
port 47011). **All four steps worked, by the user's account and the log.**

- With no title: name from Remote Play discovery at once, key reused,
  advertising as "Mirage - Living Room PS5".
- Mirroring from the home screen: "no title is connected; asking the
  launcher helper" -> "nothing was running, opened the Mirage title (app id
  0x8018)" -> "handed the title 31 video units (74318 bytes) from before it
  connected" -> "title connected". So the title was up about one second of
  video after the stream began, and the kept-video hand-over was used.
- Title closed ("closed by the title"), payload stayed; next connection
  opened the title again (app id 0xa018).
- With a game running: "app 0xc018 is running, not opening the title;
  notifying". The user saw the notification and the game kept running.
- One oddity from the user: on the first launch the phone listed plain
  "Mirage", and "Mirage - Living Room PS5" only from the second time on.
  The log shows the payload advertising the full name from its first second,
  so this was not the payload's name. Likely the phone still holding the
  name it had learned for this device id in the earlier run that advertised
  plain "Mirage" (same MAC); not verified.
- The payload is still running on the console after this test (by design).

User's decisions after it (2026-10-05): leave autoloading to them (the file
is `out\mirage.elf`); fix the title's name line; then do YouTube casting.

### Name line fix, monotonic clock, casting step H1 (2026-10-05, PC only, built, not run)

- **Name on the title's screen.** New message `PCM_LINK_NAME` (type 9,
  payload to title) with the advertised name, sent when a title connects and
  when the name is settled. The title shows "LISTED AS <name>". The title's
  own `sceSystemServiceParamGetString` call is removed (it never gave a
  name); its hello is sent empty.
- **Player clock.** The title's player scheduled on CLOCK_REALTIME. Found
  through the host test: under sanitizers about one run in four of the plain
  `steady` pattern played 0.3-0.4 s short with no counter moving, which is
  what a wall-clock correction during the run does (the player waits for the
  "early" audio). With CLOCK_MONOTONIC in `player.cpp` and `mirror_view.cpp`:
  ten of ten full. Only differences of that clock are used, so the live
  baseline is unaffected. Whether the PS5's wall clock is ever stepped while
  playing is not known; this removes the question.
- **Casting, step H1** (payload 0.9.0-dev, `src\hls.c`): the receiver now
  advertises casting (feature bits 0 and 4, `raop_set_plist("hls", 1)`,
  `raop_set_lang("en", "", "en")`). The cast callbacks are logged; on
  `on_video_play` the payload fetches the local master playlist UxPlay
  serves and logs its start; playback-info requests are answered with a
  clock that runs from the start position (duration 6 h) so the app keeps
  the session; the resets that end a cast call
  `raop_destroy_airplay_video`/`raop_remove_hls_connections` as UxPlay's
  front end does. **Nothing is played yet.** A cast also opens the title if
  none is connected.
- How casting works in UxPlay (from `http_handlers.h`, `airplay_video.c`,
  `uxplay.cpp`): the app posts the stream's address; the library has the app
  fetch the playlists over a reverse HTTP channel (FCUP), rewrites them,
  serves them from its own HTTP server, and calls `on_video_play` with a
  local URL. A player must open that URL and fetch the media segments itself
  over HTTPS.
- Plan after H1: H2 open the URL with FFmpeg in the payload (HLS demuxer,
  HTTPS through OpenSSL; pacbrew's libavformat has both) and log the streams
  and a few seconds of packets; H3 send video units and decoded sound to the
  title on one timeline, paced in real time (needs pictures put in display
  order: cast video has B-frames, mirroring has none); H4 pause, seek, stop
  and true position/duration.
- Host tests: all twelve pass with these changes.
- Built: `out\mirage.elf` 0.9.0-dev (28,449,880 bytes), `out\title\PPSA99741`.
  Uploaded and sent 2026-10-05 with the title closed.

H1 run 1 (2026-10-05, console, approved; payload 0.9.0, pid 147; log
`out\h1-run1.log`):

- **Replacing a running payload works**: "port 47010 is taken: asking the
  running copy to stop", then listening; the 0.8.0 copy left.
- **The name line works**: the user reports the title shows the listed name.
- Features advertised: 0x527ffef7 (casting bits on).
- **Casting failed before any playlist.** The user cast a local video file
  first, then YouTube. Both times: "client requested reverse connection:
  Upgrade; purpose: PTTH/1.0 "event"", then "httpd error in parsing:
  HPE_INVALID_METHOD" on the bytes `HTTP/1.1 200 OK / Content-Length: 0`,
  the connection closed, "fcup_request: send error 32: Broken pipe", and
  `on_video_play` never came. Only rate callbacks arrived ("cast: rate 1.00",
  "0.00"). The phone meanwhile sent the sound as ordinary AirPlay audio.
- Cause, in UxPlay at 3dbf7ce (`lib\httpd.c:483`): for a connection of type
  PTTH a new request is meant to be marked as a reply, but the code calls
  `http_request_is_reverse()` (the getter, no effect) where
  `http_request_set_reverse()` is meant. The fallback, a look at a message's
  first 8 bytes for "HTTP/1.1", only ever runs for the first message on a
  connection (`staging_len` is not reset), and that was the upgrade request.
  So the client's first reply on the reverse channel goes to the request
  parser and kills the channel.
- Fix: `tools\build.ps1` now compiles UxPlay's library from a copy
  (`build\uxplay-lib`) and applies patches given as exact text that must
  occur once; one patch so far, that line. `ref\UxPlay` is untouched. Payload
  0.9.1-dev built and packaged, not on the console. Worth reporting upstream;
  the user has not asked for that.
- Also in the log: two other LAN devices (<lan-address>) open reverse
  connections with no purpose and get "multiple TPPH connections (2) are
  forbidden"; nothing to do with the phone.

H1 run 2 (2026-10-05, console, approved; payload 0.9.1 with the patch,
pid 150; log `out\h1-run2.log`): **the cast reaches the receiver.**

- "cast: play http://localhost:51279/master.m3u8 from 12.5 s": the library
  got the playlists through the phone and called `on_video_play`. So the
  patch is the fix for the reverse channel.
- The master playlist UxPlay serves (4,925 bytes; only its start reached the
  log, a log line is cut at 2 KB): audio renditions as
  `http://localhost:<port>/itag/233|234/mediadata.m3u8` (rewritten to the
  local server), a subtitle rendition straight from manifest.googlevideo.com
  whose address contains `dur/481.533`, then `#EXT-X-STREAM-INF` variants
  (cut off). The local fetch answered `HTTP/1.1 200 OK`.
- The user saw a progress bar that jumped to 6 hours for an 8 minute video:
  that is H1's placeholder duration, as built, not a fault of the cast.
- The title was opened by the launcher on the cast (app id 0x18).

### Casting step H2 (2026-10-05, PC only, built, not run)

- `src\hls.c` rewritten: fetch the whole master playlist from the local
  server; log every variant (size, bandwidth, codecs, audio group); choose
  the tallest H.264 (`avc1`) variant of at most 1080 lines, then the highest
  bandwidth, and the audio rendition of its group; open the video and audio
  media playlists as two FFmpeg inputs (HLS demuxer; segments over HTTPS from
  the content servers); log format, streams (codec, profile, level, size,
  frame rate, reorder depth, extradata), duration; read packets for 6 s of
  wall time each and log counts, bytes and the stretch of the stream covered.
  Nothing is decoded or sent to the title.
- "localhost" in the addresses is replaced by 127.0.0.1 for FFmpeg: the
  SDK's `getaddrinfo` goes to the system resolver, which will not know it.
- Every open and read has an interrupt callback: a 20 s deadline per step,
  and the end of the cast (a generation counter) stops the reader.
- Playback info now reports the stream's real duration once it is open
  (placeholder only until then); the position is still a running clock.
- Link: `libavformat`, `libssl` and `libbz2` from pacbrew added. Payload
  0.10.0-dev, 32,494,056 bytes; all 253 imports are exported by the three
  libraries it needs. FFmpeg does not verify TLS certificates here (its
  default with no CA file; the console has no CA bundle).
- Unknown until it runs: whether FFmpeg's HTTP/TLS/poll path works in a
  loader payload on the console; whether the system resolver answers for the
  content hosts; how long opening takes.
- Packaged into `out\title\PPSA99741`, uploaded and sent with the title closed.

H2 run 1 (2026-10-05, console, approved; payload 0.10.0, pid 153; log
`out\h2-run1.log`): **FFmpeg opens and reads the cast stream on the console.**

- 15 variants offered for the user's 8 minute video: H.264 (`avc1`) at 144p,
  240p (two audio groups), 360p, 480p, 720p (3.8 Mbit/s) and 1080p
  (`avc1.64002A`, 6.3 Mbit/s), and VP9 (`vp09`) for the same sizes. Audio
  groups 233 (`mp4a.40.5`, HE-AAC) and 234 (`mp4a.40.2`, AAC-LC).
- Chosen: 1920x1080, 6,268 kbit/s, group 234; media playlists
  `http://127.0.0.1:<port>/itag/312/mediadata.m3u8` and `/itag/234/…`.
- Video: format hls, H.264 High (profile 100) level 4.2, 1920x1080,
  **60 fps**, **reorder depth 1** (B-frames: first packets pts 0.017, 0.050,
  0.033, 0.083 against dts 0, 0.017, 0.033, 0.050), extradata 41 bytes (MP4
  style, so the units need converting to start-code form), duration 481.5 s.
- Audio: AAC-LC, 44.1 kHz, stereo, 23 ms packets.
- Reading: 2,000 video packets (4.0 MB, 33.4 s of film, 7 keyframes, so one
  about every 5 s) and 2,000 audio packets (46.4 s) inside the 6 s window:
  HTTPS from the content servers, the resolver and FFmpeg's networking all
  work in a loader payload, several times faster than real time.
- The user still saw the wrong length on the phone: the first playback-info
  answers had gone out with the 6 h placeholder before the stream was open,
  and the app evidently keeps the first value.

### Casting step H3: playing (2026-10-05, PC only, built, not run)

Payload 0.11.0-dev (`src\hls.c`, 806 lines):

- A player thread per cast: fetch and choose as in H2, open both inputs,
  `h264_mp4toannexb` for the video, FFmpeg's decoder plus swresample (48 kHz
  S16 stereo) for the sound. It waits up to 20 s for a title (the cast asks
  the launcher for one), sends the picture size, seeks to the start position,
  then sends packets in order of time, each `PLAY_LEAD_SECONDS` (1.0) before
  its time against a film clock: video units as film video
  (`PCM_LINK_FLAG_ORDERED`, a new flag in header byte 3, with a 5 s send
  timeout), sound as ordinary (non-live) PCM. Times are the film's own pts
  (plus a large origin), the same for both, so the title's sync applies.
- Pause freezes the film clock; a seek request is taken by the thread
  (`av_seek_frame` on both inputs, decoder and filter flushed, the title told
  to drop everything); stop and a new cast end the thread through the
  generation counter. End of film: wait for the lead to play out, then
  report "finished" (position and duration -1, UxPlay's convention).
- Playback info: "no information" (position -1) until the film is playing,
  so no placeholder length goes out; then the film clock and real duration.
- Known rough edges: a pause takes effect up to a second late on screen
  (what was already sent plays out); after a seek the video starts from the
  keyframe before the place asked for and catches up; no volume control for
  casts; one fixed variant, no adaptation to a slow network; HE-AAC and VP9
  variants are never chosen.

Title (eboot.bin 37,445 bytes; about 80 MB of zero-initialised memory, mostly the 24 picture slots):

- Film video (`ordered`): after a unit the decoder is not flushed when it
  gives no picture (it is holding it to put pictures in order; a flush would
  end the sequence). Each picture that comes out takes the smallest time
  among the units taken and not yet answered, which is right both for a
  decoder that reorders and for one that answers every unit at once.
  **Which of the two the PS5's decoder is, and whether its four-frame DPB
  setting is enough for these streams, is unknown until it runs.**
- The picture store is now 24 pictures kept in order of time through an
  index array (insertion by time), and the decoder thread does not take the
  next film unit while the store is nearly full of pictures still waiting,
  so film sent a second ahead is decoded only shortly before it is due. The
  unit queue is 256.
- Screen: HELD (film units the decoder kept for later) and REORD counters.
- Fixed, found by the host test: units that arrived before the decoder had
  finished coming up were dropped, including a mirroring session's only
  keyframe when the title had just been opened for it. `vdec_submit` now
  waits (up to 5 s) for the decoder. The earlier console success of that
  case was timing luck.
- Host test: new `film` pattern (two B-frames between reference pictures,
  a fake decoder that holds pictures and returns them in order of time, each
  filled with a value that identifies it, and a stand-in screen that takes
  the due picture 60 times a second): 178 of 178 pictures shown, none with
  the wrong content for its time, none going backwards. All thirteen tests
  pass, twice in a row.
- Not tested anywhere: the payload's player (no host build of it), 1080p60
  through the CPU conversion (each frame is a 3 MB copy plus a full-screen
  conversion; mirroring was 498x1080 at 30).

H3 run 1 (2026-10-05, console, approved; payload 0.11.0, pid 156; log
`out\h3-run1.log`): **a YouTube cast plays.** User: "youtube played well
after a small pause at the beginning".

- Log: cast from 67.3 s of the same 8 minute video; 1920x1080 H.264 High
  4.2 at 60 fps chosen and opened; "playing from 67.3 s"; 449 video units and
  321 sound chunks in the first seconds, 793 and 569 by the stop, **0 not
  taken** by the title. The title was opened by the launcher (app id 0x4018).
- Pause and play arrived from the app four times each ("rate 0.00"/"1.00")
  and the cast went on; how they looked on screen was not reported. No seek
  was asked for in this run.
- So the PS5's decoder takes a B-frame stream fed this way, with the
  four-frame DPB setting, and 1080p60 through the CPU conversion was good
  enough for "played well". The title's counters (HELD, REORD, the picture
  rate) were not read out, so which decoder behaviour it is, and whether all
  60 pictures a second reach the screen, is not known.
- The pause at the start: the title has to be opened, and the video starts
  from the keyframe before the start position (keyframes are about 5 s
  apart) and catches up. Not measured.
- **Casting a local video file from the phone does not work**: no
  `on_video_play` comes; the phone sends the sound as ordinary AirPlay audio
  (ct 2 in the log) and no picture. That is the limit named in the project
  brief ("HLS casting only works from the YouTube app in UxPlay", not to be
  worked around): a local file is offered as a plain HTTP download from the
  phone, not as an HLS stream. Screen mirroring shows such a video.

User, after it: pause worked and the length on the phone was right. Asked
for: the casting polish; a local git repository (not published); then local
file casting, which lifts the brief's limit on that point at their request.

**Repository (2026-10-05):** `git init -b main` in `D:\PS5\Mirage`, first
commit 4e3b3c4 (41 files, no co-author line, no remote).
`.gitignore` keeps out `ref\`, `toolchain\`, `build\` and `out\` (binaries and
console logs; the logs carry device names and addresses). `.gitattributes`
forces LF. `LICENSE` is the GPL-3 text. NOTES.md itself contains the
console's LAN address and MAC and the phone's device id: to be looked at
before the repository is ever published. Later commits only when asked.

### Casting polish (2026-10-05, PC only, built, not run)

- **Pause at once.** New message `PCM_LINK_HOLD` (type 10, one byte). The
  payload sends hold when the app pauses and release when it plays again
  (and again after a seek made while paused). In the title a hold means
  silence, nothing taken from the buffer, and the schedule standing still
  (`audio_schedule_offset` moves with the clock while held, so the picture
  on screen stays); on release the schedule shifts by the length of the hold.
  A flush clears a hold.
- **Start and seek at the keyframe.** After the seek the player reads the
  first video packet, starts the film clock at that keyframe's time and
  drops the sound before it, instead of sending the stretch up to the place
  asked for and letting the picture catch up while the sound already plays.
  So a cast or seek lands up to a keyframe interval (about 5 s on YouTube)
  before the place asked for.
- **Volume.** The gain the sender's volume sets for AirPlay sound is applied
  to cast sound too (`audio_pipeline_gain_q15`). Whether the YouTube app
  sends volume changes during a cast is not known.
- Host test: new `hold` pattern (hold after 2 s, one second of nothing,
  release): no grain with sound during the hold, all 6 s played in the end.
  All fourteen tests pass, twice.
- Payload 0.12.0-dev (32,494,464 bytes at first build), title rebuilt and
  packaged. Not uploaded. Uncommitted.
- Still to check on the console: seek, end of film, volume.

### P1: casting polish on the console (2026-10-05, log out\p1-run1.log)

Uploaded eboot.bin (37,925 bytes) and mirage.elf 0.12.0-dev, both read back
equal; payload sent, replaced the running copy. User's report:

- Pause and play: "works great". Volume: works (the YouTube app does send it).
- End of the film: the cast stops and the title's status screen comes back.
  Log: "end of the film", no error. Working as built; a proper idle screen is
  phase 4.
- Still a small pause before the film starts. The log has no times, so the
  cause is not known (playlist fetches through the phone, two stream opens,
  the seek, 150 ms start delay).
- Seek by dragging: the film stops and the status screen shows until play is
  pressed. Log: the app sends rate 0 before the scrub and **again after it**
  (`pause, seek asked, seeking, pause, keyframe, [play only when pressed]`).
  The two seeks made without a pause around them (1227.8 and 1237.8 s, the
  skip buttons by the look of it) simply went on playing. So the staying
  paused comes from the app; the status screen is ours: a seek clears the
  title and a paused film then sent nothing.
- The first cast was stopped by the app ("playlist removed", POST stop) soon
  after a seek to 469 s of 481 s and a new video started: taken to be the
  user changing videos, not checked.

Changes after P1 (payload 0.12.1-dev, 32,494,600 bytes; title unchanged):

- After a seek in a paused film the player sends the first 0.3 s of video
  (`PREVIEW_SECONDS`, no sound) so the title has a picture of the new place.
- Experiment: while a seek is being carried out, playback info reports
  buffer empty / not likely to keep up (it said "full, keeping up" always).
  A real player reports this, and the app may be waiting for it to clear
  before it resumes by itself. If the app behaves worse, take it out first.
- Log lines for the master playlist, each stream open and "playing from"
  now carry the milliseconds since the app asked.

### P2: 0.12.1 on the console (2026-10-05, log out\p2-run1.log)

User: a seek still shows the status screen briefly, the double-tap skip too
(paused or playing); a drag seek while playing does not go on playing.

- **The start pause is ours.** "master playlist ... at 5054 ms", everything
  after it takes 1.2 s (playing at 6215 ms). `fetch_local` read until the
  server closed the connection or 5 s had passed, and UxPlay's server keeps
  the connection open. Fixed: stop when Content-Length bytes are in.
- **Drag seek.** The app's order is `rate 0` (drag begins), `scrub`,
  `rate 0`, then nothing until play is pressed. The skip buttons send
  `scrub`, `rate 1`. The buffering report made no difference (a seek is
  over in less than the phone's one-second poll, so it may never have been
  seen); taken out again. UxPlay's own player ends up paused the same way as
  far as its code reads (seek sets PLAYING, the rate 0 that follows pauses).
  **Heuristic, ours:** a pause less than 10 s before a seek is taken to be
  the drag's, the film plays on after the seek, and a `rate 0` within 1.5 s
  after such a seek is ignored (`DRAG_PAUSE_SECONDS`, `DRAG_ECHO_SECONDS`).
  Cost: pause, then drag within 10 s, and it plays. Not known: whether the
  app's own button then shows the right state (we report rate 1).
- **Status screen during a seek.** A seek resets the title's video and the
  picture store with it, so until the new place had a picture (segment
  download) the status screen showed. The preview of 0.12.1 only covered a
  seek in a paused film and came after the gap. Title change: a reset keeps
  the picture on screen for up to 3 s (`kLeftoverSeconds`) or until a new
  picture is stored. Side effect: mirroring's last picture also stays 3 s
  after it ends.
- Built: payload 0.12.2-dev (32,494,760 bytes), title repackaged. Host
  tests: 14 PASS in 8 runs of 9; the first run, started right after the
  payload build, failed and its output was not kept, five further runs kept
  for inspection all passed. Taken to be the timing tests under load, not
  shown.

### P3: 0.12.2 on the console (2026-10-05, log out\p3-run1.log)

Uploaded eboot.bin (38,165 bytes) and mirage.elf 0.12.2-dev, read back equal.
User: "everything seemed good".

- Start: master playlist at 10 ms, playing 806 ms after the app asked (was
  6215 ms).
- Drag seek while playing, twice: "the pause before it was the drag, playing
  on", the following rate 0 ignored, film went on. Seeks made well after a
  pause stayed paused. Skip buttons as before.
- No complaint about the status screen during seeks, nor about the picture
  staying 3 s after mirroring (a short mirroring session is in the log).
- Casting polish is done. Uncommitted.

**VLC (the app the local file was cast from, both times):** the log shows
only an audio session again (ct 2, `audioMode: moviePlayback`), no `/play`.
VLC on iOS decodes and draws the video itself and gives AirPlay only its
sound; there is no video for a receiver to ask for. Nothing on our side can
change that. With VLC the way to the TV is screen mirroring.

Local casting can only come from apps that hand the file to the system
player (Photos, Files, Safari). For those UxPlay calls `on_video_play` with
a plain URL, and **our player cannot play that yet**: `play_main` fetches
the location as a master playlist from this machine, and the player assumes
separate video and audio inputs. Also likely: HEVC (iPhone recordings),
which the title's decoder is not set up for. Next step is a look at what
Photos/Files really send (the current build logs `cast: play <url>` and
then gives up harmlessly) before building the direct-file path.

### P4: a video from Photos (2026-10-05, log out\p4-run1.log, no new build)

User: cast a video from the Photos app, "it worked fine".

- The log shows no `/play`. The phone opened a **mirroring session at
  1920x1080** (`video: source 1920x1080`, H.264, sound ct 8) instead of its
  usual 498x1080 screen: it plays the file itself and sends the result as a
  full-screen 1080p stream. Picture and sound come through the mirroring
  path that already works; about 940 units in 35 s, no decode errors.
- So local video from Photos needs nothing new. It is the phone's re-encode,
  at 1080p and mirroring bit rates, not the file itself; HDR and 4K are lost
  on the phone's side.
- The direct-file player (plain URL from `/play`, one input with both
  streams, HEVC) is **not built and no app has asked for it** so far. Only
  worth building if an app turns up that sends a plain URL (Safari is the
  one UxPlay names). Files app not tried.
- VLC stays audio only (see P3); screen mirroring is the way there.

### Plan from here (user, 2026-10-05)

Commit (done: 3b3b069, casting polish) -> idle screen, PIN pairing optional
-> user tests -> commit -> HEVC and 4K -> release layout -> user tests rest
mode and start at boot.

### Idle screen and optional PIN pairing (2026-10-05, PC only, built, not run)

**Idle screen** (`title\src\idle_screen.cpp`, called from `main.cpp` when no
picture is being shown): a screen-with-triangle mark, MIRAGE, "THIS CONSOLE
IS LISTED AS" and the advertised name (as large as fits), then one of READY
FOR AIRPLAY (+ a line on where to pick it) / PLAYING AUDIO / STARTING /
COULD NOT START, and one magenta line at the bottom if something is wrong
(audio, decoder, payload launch). The old screen of figures is still there
in a `-DebugOverlay` build. The font is the boilerplate's 5x7 one: capitals,
digits and `. : - % /` only.

- `tools\preview-idle.ps1` draws it on the PC for five states into
  `out\preview\*.png` (through the real Canvas and its tiled layout). Looked
  at ready and PIN: laid out as meant.
- Not handled: a still picture left on an OLED for hours.

**PIN pairing**, off by default. UxPlay's `-pin` mode with a register:
`dnssd_init(..., pin_pw 1)`, feature bit 27, `raop_set_plist("pin", 0)` (a
new random 4 digit code each time), callbacks `display_pin`,
`register_client`, `check_register`. A device enters the code once; its
public key goes into `/data/mirage/paired.txt` and it is not asked again.

- Switch: `/data/mirage/mirage.conf`, line `pin = on` (or off). Read when the
  payload starts; **written with the defaults if it is not there** (a new
  file the payload creates on the console, next to receiver.pem).
- The code reaches the screen as link message `PCM_LINK_PIN` (11; an empty
  body takes it off). It stays until the device has paired or 90 s have
  passed. If a game is running, the code goes in the notification instead.
- The code is never written to the log (port 47011 is open to the network).
- Not known until tried on the console: whether the phone keeps one
  connection open between "show the code" and entering it, what an already
  known phone does when pairing is switched on (it should be asked once),
  and what casting (the YouTube app) does with pairing on.
- No way yet to switch it from the controller, or to forget a device other
  than deleting its line in paired.txt.

Built: payload 0.13.0-dev (32,495,176 bytes), eboot.bin 37,349 bytes.

**Host tests are not steady.** 14 of 14 in most runs, but 3 runs in about 17
today had one FAIL, each time elsewhere; the one caught was `burst` with
"dry 2" (one underrun more than allowed) and everything else as expected.
These are timing tests in WSL and the failures look like a late thread on
the PC, but that is a guess: it was not seen before the hold changes either
(far fewer runs then). To look at: the margin in `burst`, or running the
suite at a fixed priority.

**Home screen tile, backgrounds, music:** `icon0.png`, `pic0.dds`,
`pic1.dds` and `snd0.at9` in the title's `sce_sys` are the boilerplate's own,
unchanged (same sizes and dates as in `~/mirage/ps5-native-app-boilerplate`).
Placeholders: Mirage needs its own before a release.

### Dracula art and idle screen; I1 sent (2026-10-05)

User: tile, backgrounds and idle screen in the Dracula theme, something
simple, then push.

- `tools\make-art.py` (Windows Python with numpy and Pillow) draws
  `title\sce_sys\icon0.png` (512x512) and `pic0.dds` / `pic1.dds`
  (3840x2160 BC7; selected: art on the right, starting: centred) plus PNG
  previews. Same mark and 5x7 font as the idle screen (the font is read from
  demo_renderer.cpp). The BC7 blocks are written by the script itself (mode 6
  only), no texconv; Pillow's own BC7 reader gives the drawn picture back to
  within 1 per channel. The .dds files are not in git (8 MB each).
- `build-title.ps1` copies the three files into the title's sce_sys and
  stops if they are missing. `snd0.at9` (selection music) is still the
  boilerplate's.
- Idle screen: Dracula colours added to `Color` (`dracula_*`), mark purple
  and pink, name cyan, READY green, PLAYING AUDIO pink, STARTING yellow,
  problems red, small print in the comment colour.
- **Where the home screen takes the art from:** `/user/appmeta/PPSA99741/`
  holds copies of icon0.png, param.json, pic0/1.dds and snd0.at9 made when
  the title was installed (11:40, the boilerplate's sizes). The uploads went
  to `/data/homebrew/PPSA99741/sce_sys/` only; `/user/appmeta` was listed,
  not written. So the tile will most likely stay the old one until the title
  is installed again or those copies are replaced. Asked the user which.
- **I1:** uploaded eboot.bin (37,349), mirage.elf 0.13.0-dev (32,495,176),
  icon0.png, pic0.dds, pic1.dds, all read back equal. Payload sent, replaced
  the running copy; it wrote `/data/mirage/mirage.conf` with the defaults,
  PIN pairing off. Waiting for the user's look at the idle screen.

### I1 result, smooth triangle, I2 sent with pin = on (2026-10-05)

- I1 (user): idle screen, music, mirroring and casting all as intended, no
  regression. One fault: the triangle's two sloping sides were jagged
  (`Canvas::triangle` fills whole pixels).
- Fix: `smooth_triangle` in idle_screen.cpp mixes the pixel at each end of a
  row with what is behind it by coverage; the same in make-art.py. Looked at
  enlarged in the preview: smooth. eboot.bin 37,893 bytes.
- **I2:** uploaded eboot.bin, mirage.elf (0.13.0-dev, unchanged), the redrawn
  icon0.png / pic0.dds / pic1.dds to the title folder, and
  `/data/mirage/mirage.conf` with `pin = on` (user's instruction); all read
  back equal. Payload sent: "PIN pairing on, 0 paired device(s)", features
  now 0x5a7ffef7 (bit 27 set).
- Seen at once after the start with pairing on: two other machines on the
  network (<lan-address>, not the phone) each opened a connection
  and asked for a reverse (PTTH) channel with no purpose named; the library
  refused the second ("multiple TPPH connections (2) are forbidden"). Not
  seen with pairing off. What they are is not known (other Apple devices
  looking at a receiver that now advertises pairing, by the look of it).
- `/user/appmeta/PPSA99741` still untouched: the home screen tile question
  is open.

### I2 result: PIN pairing works (2026-10-05, log out\i2-run1.log)

User: entered the code, it paired; also paired a second device while a game
was running.

- iPhone: "a returning device is not on the list", PAIR-PIN-START, code
  shown, title opened by itself, three pair-setup-pin steps, "is now
  paired". The next three connections: "on the list", no code. Music and a
  YouTube cast (playing 737 ms after the app asked) both work with pairing
  on.
- iPad ("iPad") with a game running: "app 0x18 is running, not
  opening the title; the pairing code goes in a notification", then paired.
- `/data/mirage/paired.txt` holds both (44 character keys).
- The two unknown machines (.190, .191) connected once at the start and
  did nothing further.
- **Leak found:** the library itself logs the code (`*** CLIENT MUST NOW
  ENTER PIN = "...."`, raop_handlers.h lines 273 and 691) and `cb_log`
  passed it into the log that port 47011 serves. Both codes of this run are
  in i2-run1.log (used, so worthless now). Fixed in 0.13.1-dev: `cb_log`
  drops any library line containing `PIN = `. **Built, not on the console**
  (which runs 0.13.0-dev).
- Host tests after this build: `burst` failed in 2 of 4 runs ("dry 2", then
  "late 1, loud 5.82 s"), the other thirteen always pass. Nothing in this
  step touches the player. `burst` needs looking at before it can be
  trusted either way.

### First public release, v0.13.1 (2026-10-05)

- Console: payload 0.13.1-dev sent (the pairing code no longer reaches the
  log); the three art files in `/user/appmeta/PPSA99741/` overwritten with
  the Dracula ones at the user's word, read back equal. Whether the home
  screen shows them without a restart: not reported yet.
- User: publish on GitHub with a release holding the title as `.ffpfsc` and
  the payload ELF as separate files (no zip), personal information out of
  the repository first.
- Scrubbed from the tracked files: the console's and other LAN addresses,
  the console's MAC, the names of the console and of the devices used for
  testing. The binaries were searched for local paths, addresses and names:
  none. The console logs in `out\` (not tracked) keep theirs.
- **History:** the three local commits carried those details in NOTES.md, so
  the public repository starts from one new root commit made from the
  scrubbed tree, under the same GitHub identity as the user's other public
  repositories. The old local history is kept as a bundle in
  `out\local-history.bundle` (not tracked, not pushed). Commit ids named
  above in these notes (4e3b3c4, 3b3b069, c500f7e) are from that history.
- `toolsuild-title.ps1 -Ffpfsc` runs the boilerplate's `make ffpfsc`
  (MkPFS, fetched by the boilerplate at its pinned revision into the WSL
  work tree) and copies the image to `outelease\`.
- Release files: `PPSA99741.ffpfsc` (16,252,928 bytes; MkPFS packed it with
  `--verify`, 0 warnings, 0 errors) and `mirage.elf` (version string 0.13.1,
  otherwise the build the console runs as 0.13.1-dev).
  **Installing from the .ffpfsc has not been tried on a console**: every
  test so far used the title as a folder under `/data/homebrew`.
- README.md written for the public repository.

### F1: the release .ffpfsc on the console (2026-10-05, prepared, not launched)

User: test the ffpfsc on the console. The console runs ShadowMountPlus
(`/data/shadowmount`, scan paths `/data/homebrew` and `/data/games`, other
titles there are .ffpfsc images with free names).

- The boilerplate's notes say not to keep a folder and an image of one title
  in the scan paths. So, with the title closed (its log showed it exited),
  the folder `/data/homebrew/PPSA99741` was **moved** over FTP (RNFR/RNTO) to
  `/data/mirage/PPSA99741-folder`: out of the scan path, nothing deleted, to
  be moved back the same way if the image fails.
- Uploaded `out\release\PPSA99741.ffpfsc` (16,252,928 bytes) to
  `/data/homebrew/`, read back equal.
- ShadowMountPlus's log: "source removed" for the folder, then 25 s later it
  mounted the image (pfsc, then the exFAT inside), "Remounting: Mirage",
  nullfs on `/system_ex/app/PPSA99741`, `mount.lnk` and `mount_img.lnk`
  written, then unmounted again until the title is started. One line "image
  source cache update failed": the EVO Player image has the same line.
- Next: the user opens Mirage from the image (release payload, version
  string 0.13.1, replaces the running 0.13.1-dev).
- **Result (user): "Looks good it works."** The release image installs and
  runs through ShadowMountPlus. The old folder is still at
  `/data/mirage/PPSA99741-folder` for the user to remove. The release notes
  and README still say the image is untried: to be corrected.
- User asked whether Android casting can be added later. Answered: not
  Google Cast itself (receivers need a Google-signed device certificate);
  possible routes are a DLNA/UPnP renderer, or Android apps that send
  AirPlay (untested against Mirage). Parked until the rest is done.

### Local file casting: first look (2026-10-05, PC only)

- UxPlay's `/play` handler (`http_handlers.h`) already takes a plain
  `http://` or `https://` Content-Location and hands it to `on_video_play`;
  only `/master.m3u8` locations go through the YouTube FCUP path. So a file
  the phone serves itself would reach `hls.c` as an ordinary URL, which
  FFmpeg can open (mp4/mov, not only HLS).
- But in h3-run1.log the local file attempt never sent `/play` at all: the
  phone opened a plain audio session (ct 2, `audioMode: moviePlayback`) and
  kept the picture on its own screen. So the phone, or the app, decided not
  to send video. Not known yet: which app was used, and whether it is the
  app (player without external playback) or our advertised features.
- Next: ask which app; try Photos and Safari (UxPlay names Safari as
  working); log every request path and the `/play` plist keys.

Phase 3 state: YouTube casting works (H1-H3). Left of H4: what pause looks
like, seek (untested), end of film (untested), the right length on the
phone (untested with this build), volume for casts, a start without the
catch-up pause. Mirroring shows the picture
(H.264, portrait 498x1080 seen), with sound, in sync, through pauses.
Not yet tried: rotation/landscape and other sizes (V5), long sessions,
HEVC, a second device, zero-copy presentation.

## Found along the way

- **A title cannot listen on a socket without help.** nuvio (an EVO fork)
  recorded `bind/listen 127.0.0.1:8686 failed errno=13` from its title on
  13.60 until a payload patched the process credentials
  (`ref\nuvio-ps5-homebrew\docs\ARCHITECTURE.md`, "Permissions and firmware").
  An AirPlay receiver listens on several TCP and UDP ports, so a Mirage
  *title* will need the same kind of helper. A *payload* does not: the
  tailscale daemon listens on TCP and UDP freely.
- **VideoOut must not be opened from a payload** (EVO's notes: it panics the
  console). So video (phase 2) has to be a title.
- AudioOut in both references: `sceAudioOutInit`, `sceAudioOutOpen(system user,
  port MAIN, 0, grain, 48000, S16 stereo or 8ch)`,
  `sceAudioOutOutput` blocking per grain, from a title. From a payload: unknown.
  The SDK has the `libSceAudioOut.so` stub.
- ProsperoLight builds as a title with its own clean-room `libc.prx` and a
  Linux/WSL Makefile; EVO builds in Docker with pacbrew. Neither builds on
  Windows natively. The payload toolchain here does.

## Decisions

- 2026-10-05: Phase 1 is proposed as a **payload** (ELF through the loader),
  not a title. Reasons: live log over the loader socket, sockets known to
  work, no credential helper, builds with the existing Windows toolchain.
  Depends on AudioOut working from a payload (to be probed). Not yet approved.

## Phase 1 plan (proposed, waiting for approval)

- M0 mDNS probe, three short console runs (above).
- M1 PC only: get OpenSSL (pacbrew archive, 330 MB download, needs approval),
  build libplist, link all of `lib/` plus a logging-only `main` into
  `mirage.elf`.
- M2 console: advertise and accept. The iPhone lists "Mirage", connects, audio
  packets are decrypted, counted and dropped. New on the console: the
  listeners and the pairing/FairPlay code path.
- M3 console: AudioOut probe from a payload: open, one second of a quiet tone,
  close. The one new thing is the AudioOut calls.
- M4 console: ALAC decode, resample 44.1 to 48 kHz, jitter buffer, AudioOut;
  volume, flush, teardown.
- M5 hardening: reconnects, SIGPIPE, scheduling class, a clean way to stop the
  payload (resend replaces the old instance, as tailscale does), errno 163 on
  network change.

Decoder choice for M4: FFmpeg's libavcodec + libswresample from pacbrew cover
ALAC now and AAC-ELD (mirroring audio) later with one dependency. To confirm
when the archive is here: that its build includes the `alac` and `aac`
decoders, and that the native `aac` decoder handles AirPlay's ELD frames
(UxPlay itself uses FFmpeg's decoder through GStreamer's `avdec_aac`).

If AudioOut does not work from a payload: phase 1 becomes a title plus a small
credential helper payload (nuvio's design), which phase 2 needs anyway.
