# Headless host/join/start + relay — design and dropoff

Written 2026-07-28. Everything referenced here is on `main` unless marked NOT DONE.

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

## VPS state

`163.5.210.131`, root, SSH key installed (`~/.ssh/id_ed25519`). **Password auth still enabled and
the password was pasted in chat — rotate it.** `79.110.49.24` in the panel is the hypervisor NODE,
not the VM; do not log in there.

* Ubuntu 20.04.6 (EOL since April 2025, glibc 2.31), Xeon E5-2680 v4 (2016 Broadwell), 4 cores.
* Installed: CMake 3.28.6 (`/usr/local/bin`, distro's 3.16 is below the project's 3.25 minimum),
  GCC/G++ 11.5 from `ppa:ubuntu-toolchain-r/test`, vcpkg at `/root/vcpkg` (full clone — a `--depth 1`
  clone fails, the manifest pins a baseline commit that shallow history lacks), autotools, nasm/yasm,
  SDL3's X11 dev deps. `libdecor-0-dev` does not exist on 20.04 and is not required.
* `Acquire::ForceIPv4 "true"` in `/etc/apt/apt.conf.d/99force-ipv4` — **required**: DNS returns
  IPv6-only records for the Ubuntu mirrors and the box has no working IPv6, so apt hangs without it.
* Build script `/root/vps_build2.sh`, log `/root/build.log`. Launch detached with `setsid nohup`;
  a plain `nohup ... &` over SSH did not survive.

**RAM is not what was sold.** dmidecode reports 8192 MB and the kernel saw `8100900K/8388064K` at
boot, but `MemTotal` is 3941808 kB — ~4.3 GB missing, reclaimed by `virtio_balloon` (bound at
`/sys/bus/virtio/drivers/virtio_balloon/virtio0`, built into the kernel so it does not appear in
`lsmod`). A plan advertised as "8 GB Dedicated" is being ballooned. Worth a support ticket, and it
matters operationally: memory reclaimed mid-match would stall the sim for every player.

## Status of the Linux build

NOT DONE. Failing in configure, being worked through:
1. ~~CMake too old~~ fixed (3.28.6)
2. ~~`VCPKG_ROOT` unset~~ fixed
3. ~~shallow vcpkg clone~~ fixed (unshallowed)
4. ~~alsa needs autotools~~ fixed (the piped-exit-code bug above)
5. ~~SDL3 needs X11 dev packages~~ fixed
6. in progress — rerun and read `/root/build.log`

For a headless server, consider whether SDL3/OpenAL/ffmpeg are needed at all. Dropping them would
remove most of the dependency surface, but they are manifest dependencies in `vcpkg.json` and the
preset sets `SAGE_USE_SDL3=ON` / `SAGE_USE_OPENAL=ON`, so it needs a deliberate headless preset
rather than a flag flip.
