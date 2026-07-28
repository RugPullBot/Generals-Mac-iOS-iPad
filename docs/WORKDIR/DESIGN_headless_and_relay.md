# Headless host/join/start + relay — design and dropoff

Written 2026-07-28. Everything referenced here is on `main` unless marked NOT DONE.

## STATUS: the headless CLI is DONE and PROVEN cross-platform

**Mac headless host + Windows headless joiner, 3000 frames, zero differing.**
Reproduced twice. Harness: `scripts/test/xplat-lan-soak.sh`, which prints
`PASS: 3000 frames, zero differing` on its own.

```bash
FRAMES=3000 ./scripts/test/xplat-lan-soak.sh
```

Implemented in `GeneralsMD/.../Common/HeadlessMatch.{h,cpp}`, entered from `GameMain` as a third
branch beside `ReplaySimulation` — it drives the engine itself instead of `TheGameEngine->execute()`.
Flags: `-lanhost -lanjoin -lanmap -lanname -lanai -lanwait -lanframes -lantimeout`.
**ZH only.** `Generals/` has its own `CommandLine`/`GlobalData`/`GameMain` and needs the same work.

### The design's central assumption was right, but the cost was elsewhere

`LANAPI` really does expose the whole lifecycle as plain calls, and `OnGameStart` really is UI-free.
The work was NOT in the netcode. It was in eight silent failures, each of which presented to the
host as "no peer ever joined" — i.e. exactly like a network fault. They are listed under Traps
below. Budget for that shape of problem, not for protocol work.

### Still open on this path

* **Windows exits `0xC0000005` at the END of a headless run**, after every frame and CRC is
  written. Pre-existing (it already did this for `-replay`), now also on the LAN path. The soak
  deliberately does not gate on the Windows exit code — it compares CRC streams and frame counts,
  so a truncating crash would still show as a frame-count mismatch. Worth fixing; not blocking.
* `-lanwait` peers must be humans. Mixed human+AI slot layouts beyond `-lanai` are untested.

## Why this is smaller than it looks

`TheLAN` (LANAPI) already exposes the entire lobby lifecycle as plain method calls. The menus are
just callers. A headless driver calls the same methods and skips the UI entirely — **this is a
driver over an existing API, not new netcode.**

`Core/GameEngine/Include/GameNetwork/LANAPI.h`:

| call | line | purpose |
|---|---|---|
| `RequestGameCreate(name, isDirectConnect)` | 88 | host a game |
| `RequestGameJoinDirectConnect(ip)` | 80 | **join by IP — no discovery needed** |
| `RequestGameOptions(optsStr, isPublic, ip)` | 87 | set map + slot list (incl. AI slots) |
| `RequestAccept()` | 82 | mark ready |
| `RequestGameStart()` | 85 | start the match |
| `RequestGameLeave()` | 81 | leave cleanly |

`RequestGameJoinDirectConnect` is the important one: it bypasses UDP broadcast discovery, so peers
find each other by explicit IP. **That is what makes this work over the internet, not just LAN** —
and it is the same primitive the relay needs.

Slot config goes through `GameInfo::setSlot(slotNum, GameSlot)` with `SLOT_EASY_AI` / `SLOT_MED_AI` /
`SLOT_BRUTAL_AI` (`GameInfo.h:40-42`), serialised by `GameInfoToAsciiString` (`:278`) — the same
`US=1;M=...;S=H<name>,<ip>,...:CH,...` string already seen in replay headers. So "add 6 AIs" is
building that string and calling `RequestGameOptions`.

## The one real gotcha: pumping LANAPI

`TheLAN->update()` is currently called from `LanLobbyMenu.cpp:732` — i.e. **the lobby UI is what
drives the network pump**, and `TheLAN` itself is constructed there (`:517`) and at
`NetworkDirectConnect.cpp:211`. A headless path must construct and pump it itself. Do not assume
some engine subsystem does it; it does not.

## Proposed CLI

```
-headless                       (exists)
-lanhost   <gamename>           create a game and wait for peers
-lanjoin   <ip>                 join by direct IP
-lanmap    <mapname>            set map before starting
-lanai     <E|M|H>xN            fill N slots with AI of that difficulty
-lanwait   <peers>              wait until N peers are accepted, then start
-lanstart                       start immediately once ready
-lanframes <N>                  run N logic frames then exit (soak-test bound)
```

Add to the parse table in `CommandLine.cpp` (101 entries today, same pattern as `parseReplay`).
Note `parseReplay` also calls `rts::ClientInstance::setMultiInstance(TRUE)` and
`skipPrimaryInstance()` — a headless host/join wants the same, or the single-instance mutex blocks a
second copy on one box. **See the multi-instance warning below before enabling that.**

## Sketch of the driver

Model it on `ReplaySimulation::simulateReplaysInThisProcess` (`Core/GameEngine/Source/Common/
ReplaySimulation.cpp`), which is the proven headless loop: `TheGameClient->updateHeadless()` then
`TheGameLogic->UPDATE()` until done.

```
if (host)  TheLAN->RequestGameCreate(name, /*direct*/TRUE);
else       TheLAN->RequestGameJoinDirectConnect(ip);

while (!lobbyReady) { TheLAN->update(); sleepShort(); }   // pump - nothing else does it

if (host) {
    buildSlotList(map, aiSpec);                           // GameInfo::setSlot + GameInfoToAsciiString
    TheLAN->RequestGameOptions(opts, TRUE);
    waitForPeersAccepted(expectedPeers);
    TheLAN->RequestGameStart();
}
while (TheGameLogic->isInGame() && frames < limit) {
    TheGameClient->updateHeadless();
    TheGameLogic->UPDATE();
}
```

`[GXCRC]` already prints every frame in MP or replay (`GameLogic.cpp:3914`, gated on
`isMPGameOrReplay`), so two headless peers produce directly diffable CRC streams with no extra work.

## Relay

The relay and the anticheat server are **the same process**. A headless engine that receives
commands, validates them, simulates authoritatively and holds the true CRC is simultaneously:
NAT relay, desync arbiter, and cheat detector. Build it once.

Sizing, measured on the real engine (not estimated):

```
Apple M4:  2,037 logic frames/sec @ 432 objects = 67.9x realtime = 1.5% of one core
           scaled to 6,118 objects  ~5x realtime  ~20% of one core
Server x86 core is ~0.5-0.7x an M4 core -> budget 30-40% of one dedicated core per heavy match
```

Lockstep transmits **commands only**, a few KB/s per player — bandwidth is a non-issue, do not pay
for it. Single-core clock is the only CPU metric that matters; core count only buys concurrent
matches. Never use burstable/shared CPU: a throttled sim does not degrade gracefully, it stalls the
match for everyone.

### What the relay CANNOT do

**Maphack is unsolvable in lockstep.** Every client must know the whole world to simulate it; fog of
war is a rendering filter over data the client legitimately holds. No server check stops someone
drawing what is already in their memory. Catch it behaviourally instead: replays contain every
command, so re-simulate finished matches and look for reactions to units that were never visible.
That costs no latency and requires no client trust.

What the relay *can* do: reject illegal commands, rate-limit, hold the authoritative CRC so a
mismatch names the guilty peer instead of triggering a majority vote, and give clean desync
forensics.

## The eight silent failures, in the order they were hit

Every one of these looks identical from the host: a peer that never joined. Six needed the
JOINER's log to diagnose, and one needed bisecting against the known-good replay path. If a peer
never appears, read the other machine's log before touching anything.

| # | cause | how it presented |
|---|---|---|
| 1 | `LANbuttonPushed` — `LANAPI::update()` returns early on it, and `OnGameCreate`/`OnGameJoin` SET it; only `LanLobbyMenuInit` clears it | host goes deaf forever, immediately |
| 2 | `setMultiInstance(TRUE)` copied from `parseReplay` injects a synthetic `127.<instance>` IP that outranks `192.168.x`, and is unbindable on macOS | `failed to bind local IP 127.0.0.2` |
| 3 | `generalszh.exe` is a GUI-subsystem binary; PowerShell does not wait for it | instant exit 0, no output at all |
| 4 | `CNC_GENERALS_ZH_PATH` unset on the joiner | hangs forever at `[INI] ERROR: No files read from directory`, 1239 bytes of stderr. **Not session 0** — a headless replay fails the same way without it and succeeds from session 0 with it |
| 5 | binaries built BEFORE the commit — `sourceID` bakes in HEAD plus a dirty-file overlay at COMPILE time | join refused; `git rev-parse` agrees on both machines so the commit check passes |
| 6 | every slot defaults to `SLOT_CLOSED` (`GameSlot::reset`); the lobby UI is what opens them | joiner logs `Join Deny`, host logs nothing |
| 7 | host advertised map CRC/size of 0, so `GameInfo::setMapCRC` told every peer it lacked a map it had | peer reports "You do not have the map", never readies |
| 8 | log-only overrides of `OnAccept`/`OnHasMap`/`OnPlayerJoin` — these callbacks MUTATE lobby state, they are not UI | host logged `accept ... = 1` and still never counted it. The self-contradicting log is what cracked it |

Plus two that corrupted the RESULT rather than the run:

* **`LANEnableStartButton` dereferenced null widgets**, reached from `LANGameInfo::resetAccepted()`
  — a state operation. Crashed the host the instant a peer joined. Now guarded; a real latent bug,
  unreachable from the UI flow, and `Generals/` still has it.
* **CRLF.** The Windows peer writes `\r\n`, so `diff` called a perfectly matching 1200-frame run a
  total desync from frame 0. The first clean cross-platform run was nearly recorded as a failure.
  The harness now strips CR.

**Rule this produced:** `LANAPI` mixes state mutation into its `On*` callbacks. In `HeadlessLANAPI`,
override ONLY what touches `TheShell` or `LANbuttonPushed`; delegate everything else to the base.

## RESOLVED: the working-directory path defect (2026-07-28)

The asymmetry described below has been **fixed at the source**, not worked around.
`Win32BIGFileSystem` resolved `CNC_GENERALS_ZH_PATH` and then discarded it — it never called
`TheLocalFileSystem->setAssetRootPath`, and `Win32LocalFileSystem` inherited the base no-op.
Windows now has the same asset-root fallback `StdLocalFileSystem` has had since 23/03/2026.

Verified: AI match, cross-platform, `Data\Scripts` and `MapsZH.big` BOTH deleted from the Windows
run folder — **1500 frames, zero differing** (`AI=Hx1 FRAMES=1500 ./scripts/test/xplat-lan-soak.sh`).
The slot list is committed with the CRC streams because the result means nothing without it:
skirmish scripts attach only for AI players, so a run that silently went human-only would pass
while never touching the bug.

The fallback is strictly additive — relative paths only, consulted only after the normal lookup
fails — and excludes writes, so a failed write is never redirected into the read-only Steam
install. `doesFileExist` got the same fallback as `openFile`, since the map cache probes before
opening and would otherwise disagree with it.

The staging in `setup-run-win64.ps1` is kept as belt-and-braces, no longer load-bearing.

## Environment asymmetry worth fixing properly (historical - see above)

The Windows run folder had **no `.big` files** and leaned on `CNC_GENERALS_ZH_PATH`, so its map
cache indexed only user maps from Documents — zero built-in `maps\` entries — while the Mac game
folder is self-contained (25 `.big`s including `MapsZH.big`). Worked around by staging
`MapsZH.big` into `C:\dev\GeneralsX-run\`. **That is a workaround**, and it is the same
working-directory-relative resolution class as the `SkirmishScripts.scb` desync and the
`Win32Mouse.cpp:376` cursor bug. `setup-run-win64.ps1` does not stage it, so it must be re-copied
after every restage. Fixing path resolution once would cover all three.

## Traps, all learned the hard way

* **`RTS_DEBUG_MULTI_INSTANCE` breaks LAN discovery.** `IPEnumeration.cpp:118-126` injects a loopback
  address per instance and 127.0.0.1 sorts above 192.168.x.x, so the peer advertises itself on
  loopback. To run several instances on one box, rank loopback last in `ipSortsBefore` first.
* **Same-platform instances cannot find determinism bugs.** Six headless Linux peers agree by
  construction; 80,000+ same-platform frames have never desynced. The soak test that matters is
  **Mac headless + Windows headless**, cross-architecture.
* **On Windows, SSH lands you in session 0**, where DXVK enumerates zero adapters and
  `W3DDisplay::init()` dies at 0xC0000005. Launch via a transient `schtasks /it` task. A pure
  headless run may avoid this, but verify rather than assume.
* **Never pipe a command whose failure matters.** `apt-get ... | tail` then `$?` reads *tail's*
  exit code. This cost a wasted VPS build cycle tonight. Capture `$?` before any pipe, and verify
  installs by checking the tool is present, not by trusting an exit code.
* **A headless run that starts and then goes silent is not necessarily stuck in the match loop.**
  It hung in `MultiPlayerLoadScreen::init`, pumping OpenAL while loading the map — upstream of the
  first frame. `State: R` in `/proc/<pid>/status` said it was spinning rather than blocked on I/O,
  and three `gdb -p <pid> -batch -ex "thread apply all bt"` samples named the line. **Attach to the
  live process before adding instrumentation**; the plan to log the match loop would have printed
  nothing at all. Root cause was `while (alGetError() != AL_NO_ERROR) {}` in
  `OpenALAudioStream::bufferData` — with no current context OpenAL Soft returns
  `AL_INVALID_OPERATION` forever, so the drain never terminated. Fixed in `aa776b7eb`.
* **Headless on Mac/Linux still builds the REAL audio backend.**
  `SDL3GameEngine::createAudioManager(Bool dummy)` does `(void)dummy;`, while the Win32 factory
  honours the flag and returns `MilesAudioManagerDummy`. Anything that assumes a headless server has
  no audio subsystem is wrong on these two platforms.

## VPS state — Ubuntu 24.04, BUILD IS GREEN (2026-07-28)

`163.5.210.131`, root, **key auth only — password auth is disabled and verified refused.**
The box was reinstalled as **Ubuntu 24.04.4 LTS** (4 cores, Xeon E5-2680 v4).
`79.110.49.24` in the panel is the hypervisor NODE, not the VM; do not log in there.

**`GeneralsXZH` builds and runs.** 197 MB ELF, all shared libraries resolved, starts headless
("Headless mode detected, skipping SDL3 video/Vulkan window initialization") and creates the
engine. Reproducible with `scripts/build/linux/build-linux-relay.sh`.

**RAM is still short on a FRESH install:** `MemTotal` 3.75 GB on a plan sold as "8 GB Dedicated",
with `virtio_balloon` bound at `/sys/bus/virtio/drivers/virtio_balloon/virtio0`. The reinstall
ruled out the old OS as the cause — it is plan-level config. Still worth a ticket: memory
reclaimed mid-match stalls the sim for every player.

### What 24.04 fixed for free

| blocker on 20.04 | 24.04 |
|---|---|
| CMake 3.16, project needs >= 3.25 | 3.28.3 from the distro — no manual build |
| GCC 9 | 13.3 from the distro — no toolchain PPA |
| glibc 2.31 | 2.39 |
| ffmpeg 4.2, no `AVFrame::ch_layout` | 6.x (libavcodec 60) — the blocker that made 20.04 hopeless |

### The three blockers 24.04 did NOT fix

Each surfaced only after the previous one was cleared, so budget for them in sequence.

1. **SDL3 `CheckX11` needs XTEST.** `Couldn't find dependency package for XTEST`. The full dep set
   is in the provisioning list below. Verify with `pkg-config --exists`, never by apt's exit code.
2. **Nearly every tool target is a Windows GUI program.** 17 of 17 under `Core/Tools`, plus
   GUIEdit / WorldBuilder / wdump, all pulling `afxwin.h` / `commctrl.h`. The `*_TOOLS` options
   default to **ON** and `linux64-deploy` overrides none of them, so the build compiled thousands
   of game objects and then died in an editor. Turn them all off (the script does).
   The two MFC ones are additionally gated on MSVC in `Core/Tools/CMakeLists.txt`, since an
   `afxwin.h` target can never build off-MSVC whatever the flags say.
3. **`RTS_BUILD_OPTION_FFMPEG=OFF` cannot work with `SAGE_USE_OPENAL=ON`.** This contradicts the
   earlier advice in this doc, which said OFF was right for a server because a relay decodes no
   video. It is not: `Core/GameEngineDevice/CMakeLists.txt:305` *compiles*
   `Source/VideoDevice/FFmpeg/FFmpegFile.cpp` when the option is OFF — OpenALAudioCache needs it
   for **audio** decoding — while the ffmpeg libraries are linked only when it is ON. OFF
   therefore compiles ffmpeg-calling code and links nothing, dying at the final link of
   `GeneralsXZH` with ~30 undefined `av_*`/`avcodec_*` symbols. **This is a latent bug on every
   platform, not a Linux quirk**, and the honest fix is to make the OFF path link ffmpeg too —
   left alone here because it needs verifying on all three platforms.

### Provisioning (reproducible)

```
apt: build-essential cmake ninja-build git pkg-config curl zip unzip tar ca-certificates
     autoconf automake libtool nasm yasm
     libx11-dev libxext-dev libxrandr-dev libxi-dev libxcursor-dev libxfixes-dev
     libxss-dev libxkbcommon-dev libwayland-dev libdecor-0-dev libgl1-mesa-dev
     libpng-dev libjpeg-dev libtiff-dev libwebp-dev
     libav*-dev libsw*-dev libasound2-dev
     libxtst-dev libxxf86vm-dev libxinerama-dev libdrm-dev libgbm-dev
     libpulse-dev libudev-dev libdbus-1-dev libibus-1.0-dev libsndio-dev
vcpkg at /root/vcpkg, FULL clone   # --depth 1 fails: the manifest pins a baseline commit
repo  at /root/GeneralsX
```

`Acquire::ForceIPv4 "true"` in `/etc/apt/apt.conf.d/99force-ipv4` was required on 20.04 (DNS
returned IPv6-only mirrors with no working IPv6). Set again on 24.04 as a precaution; not
re-tested as necessary.

Build detached with `setsid nohup` — a plain `nohup ... &` over SSH does not survive.

### Still worth doing: a real headless preset

The X11/SDL3 stack was installed only to satisfy `SAGE_USE_SDL3=ON`, which a display-less relay
has no use for. Dropping SDL3/OpenAL/ffmpeg needs a deliberate headless preset rather than a flag
flip, because they are manifest dependencies in `vcpkg.json`. That would delete most of this
dependency surface.

