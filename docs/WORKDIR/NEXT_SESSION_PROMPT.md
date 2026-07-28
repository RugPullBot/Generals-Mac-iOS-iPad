# Kickoff prompt for the next session

Paste everything below the line into a fresh session.

---

Read these first, in this order:

- `~/GeneralsX-src/docs/WORKDIR/STATE_2026-07-28_session3.md` — what the overnight session established
- `~/GeneralsX-src/docs/WORKDIR/DESIGN_headless_and_relay.md` — the headless/relay design and VPS state
- `git -C ~/GeneralsX-src log --oneline -30` — the commit messages carry the *why*

**Goal:** Mac, iPad and Windows play together without desyncing — **DONE, see below** — and next,
online play through a relay server so it is not limited to one LAN.

## What is FIXED and PROVEN (do not re-investigate)

**1. The cross-platform desync. 74,705 frames across three architectures, zero differing.**
Root cause: 69 libm calls the first `gamemath.h` work never reached, because `gamemath.h` had
exactly ONE include site in the whole tree (`wwmath.h:40`) and nothing `#define`s the libm names.
40 raw `sinf`/`cosf` in `matrix3d.h`/`matrix3.h`/`vector3.h` (the `Rotate_*` helpers, reached every
frame from `PhysicsBehavior::doPhysics`) and 29 raw `atan2` in GameLogic. All routed now.

**2. The AI-only frame-0 desync. A MISSING DATA FILE.**
`Data\Scripts\SkirmishScripts.scb` was absent from the Windows run folder.
`SidesList::prepareForMP_or_Skirmish` (`SidesList.cpp:520`) opens it by a path relative to the
WORKING DIRECTORY, which on Windows is `C:\dev\GeneralsX-run` — the Mac's working directory is its
own game folder, so only the Mac loaded it. Every delayed-eval script draws once from the SHARED
simulation RNG (`ScriptEngine.cpp:6892`), so the Mac made 91 draws and Windows 0, and the streams
were permanently offset. First visible symptom: every starting unit at the same radius from its
Command Center but a different ANGLE — that signature is what identifies an RNG desync rather than
float drift. Proof by intervention: 13,919 of 13,919 frames differing → **0**, same binaries.
It was AI-only because skirmish scripts attach only for AI players.

Fixed by staging the file in `setup-run-win64.ps1`. **That is a workaround.** The real defect is
that loose data files are opened by working-directory-relative paths ignoring
`CNC_GENERALS_ZH_PATH` — same bug as `Win32Mouse.cpp:376` (the cursor issue). Fixing path
resolution once would cover both and anything else silently missing.

**SimID cannot catch this class of bug.** `.scb` files are not INI, so they are outside `m_iniCRC`.
Both peers reported identical `engine`/`source`/`data`/`ordinal` while simulating different games.
Extending the data fingerprint to cover loose simulation-relevant files would turn a silent desync
into a refused join. **Worth doing.**

## The tool that makes everything else fast

Cross-platform headless replay now works. A `.rep` is a command log, not a recording, so replaying
re-runs the whole simulation. One file played on two platforms reproduces a cross-platform desync
with no lobby, no network, no second human, at **68x realtime**.

```bash
cd ~/GeneralsX/GeneralsZH
GX_REPLAY_XPLAT=1 ./run.sh -headless -replay aitest.rep 2> mac.err
grep '^\[GXCRC\]' mac.err > mac.crc      # then diff against the other machine
```

This is what let the AI desync be solved alone in minutes. **Use it before asking anyone to play.**
Fixture committed: `docs/WORKDIR/evidence/ai-hardai-fixture.rep` (one human + one Computer-Hard).

It was blocked until this session by a format bug: replay strings were written with native
`wchar_t`, **4 bytes on macOS/clang and 2 on Windows/MSVC**, so a Mac `.rep` crashed Windows at
`0xC0000005`. Now pinned to UTF-16, which also matches retail. Old Mac-written `.rep` files no
longer load; they were never readable elsewhere anyway.

## THE HEADLESS CLI IS DONE AND PROVEN (2026-07-28 session 4)

**Mac headless host + Windows headless joiner, 1200 frames, zero differing. Reproduced twice.**

```bash
FRAMES=1200 ./scripts/test/xplat-lan-soak.sh     # prints PASS/FAIL on its own
```

`-lanhost -lanjoin -lanmap -lanname -lanai -lanwait -lanframes -lantimeout`, implemented in
`GeneralsMD/.../Common/HeadlessMatch.{h,cpp}` and entered from `GameMain` as a third branch beside
`ReplaySimulation`. **ZH only** — `Generals/` needs the same treatment.

`RequestGameJoinDirectConnect` takes a hostname as well as an IP (it goes through `ResolveIP`), so
the off-LAN primitive the relay needs is already exercised.

Read `DESIGN_headless_and_relay.md` for the eight silent failures this cost. All eight look
identical from the host — "no peer never joined" — so **when a peer does not appear, read the
OTHER machine's log first**. Two rules came out of it that will bite again:

1. **`LANAPI` mixes state mutation into its `On*` callbacks.** `OnAccept` is where `setAccept()`
   happens; `OnHasMap` is where `setMapAvailability()` happens; `OnPlayerJoin` calls
   `resetAccepted()`. In `HeadlessLANAPI` override ONLY what touches `TheShell` or
   `LANbuttonPushed` and delegate the rest, or the host silently stops tracking its own lobby.
2. **Commit, THEN build, on BOTH machines.** `sourceID` bakes HEAD plus a dirty-file overlay in at
   COMPILE time, so matching `git rev-parse` output proves nothing. The soak now refuses to run
   when either binary predates the last commit that touched the digested source dirs.

Also: the Windows run folder needs `MapsZH.big` staged into it (`setup-run-win64.ps1` does not do
this, so re-copy after every restage) and `CNC_GENERALS_ZH_PATH` set, or the joiner hangs forever
at `[INI] ERROR: No files read from directory` with 1239 bytes of stderr. That is NOT the session-0
problem — a headless replay fails identically without it and succeeds from session 0 with it.

## THE NEXT TASK: the relay

Read `DESIGN_headless_and_relay.md` — it has the full plan. The short version:

**This is smaller than it looks.** `LANAPI` already exposes the whole lobby lifecycle as plain
calls; the menus are just callers. A headless driver calls the same methods:

| call | purpose |
|---|---|
| `RequestGameCreate(name, isDirectConnect)` | host |
| `RequestGameJoinDirectConnect(ip)` | **join by IP, no UDP discovery — this is what enables off-LAN play** |
| `RequestGameOptions(opts, isPublic)` | set map + slots (incl. AI) |
| `RequestAccept()` / `RequestGameStart()` | ready / start |

**The one real gotcha:** `TheLAN->update()` is pumped from `LanLobbyMenu.cpp:732` and `TheLAN` is
constructed there too — **the UI currently drives the network pump.** A headless path must construct
and pump it itself. Model the loop on `ReplaySimulation::simulateReplaysInThisProcess`, which is the
proven headless pattern.

Proposed CLI: `-lanhost <name>`, `-lanjoin <ip>`, `-lanmap`, `-lanai <E|M|H>xN`, `-lanwait <peers>`,
`-lanframes <N>`. Add to the table in `CommandLine.cpp` (101 entries, follow `parseReplay`, which
also calls `setMultiInstance(TRUE)` + `skipPrimaryInstance()`).

**The relay and the anticheat server are the same process.** A headless engine that receives
commands, validates them, simulates authoritatively and holds the true CRC is simultaneously NAT
relay, desync arbiter and cheat detector. Build it once.

Measured sizing: **2,037 logic frames/sec on M4 = 67.9x realtime** at 432 objects; ~5x realtime at
6,118 objects; a server x86 core is ~0.5-0.7x an M4 core, so budget **30-40% of one dedicated core
per heavy match**. Lockstep sends commands only — a few KB/s per player, bandwidth is a non-issue.
Single-core clock is the only CPU metric that matters. **Never use burstable CPU**: a throttled sim
does not degrade gracefully, it stalls the match for everyone.

**Maphack is unsolvable in lockstep** — every client must know the whole world to simulate it. Catch
it behaviourally by re-simulating replays, not by trying to prevent it.

## The three machines + the VPS

- **Mac mini M4** — repo `~/GeneralsX-src`, game `~/GeneralsX/GeneralsZH/`, LAN `192.168.10.51`.
  Build `./scripts/build/macos/build-macos-zh.sh --build-only` (drop `--build-only` if you cleared
  the CMake cache), deploy `./scripts/build/macos/deploy-macos-zh.sh`.
  `caffeinate -dimsu` is running; `sleep`/`disksleep` are 0. Monitor may be off — that is fine.
- **Windows 11 `r0se-desktop`** — `ssh User@192.168.10.89`, PowerShell (`;` not `&&`).
  Clone `C:\dev\GeneralsX`, build `cmd /c C:\dev\cb.bat win64 x64`, stage
  `scripts/build/windows/setup-run-win64.ps1`, run folder `C:\dev\GeneralsX-run\`.
  Launch the game with `C:\dev\gxrun_session1.ps1` (a transient `schtasks /it` task).
  **Straight from SSH you land in session 0**, where DXVK enumerates zero adapters and
  `W3DDisplay::init()` dies at `0xC0000005`. `standby-timeout-ac` is 0; session 1 is active.
- **iPad Air 11-inch (M3)** — **UNPLUGGED and on an older build.** `sourceID` has moved, so it
  cannot join until replugged and reinstalled: `./scripts/build/ios/package-ios-zh.sh --install`.
  Install over USB, never WiFi.
- **VPS `163.5.210.131`** — root, SSH key installed. **Password auth is now DISABLED** (2026-07-28):
  the leaked password was never rotated, so `PasswordAuthentication no` +
  `PermitRootLogin prohibit-password` were set instead, which needs no new secret. Verified both
  ways — key auth succeeds, password auth returns `Permission denied (publickey)`. Backup at
  `/root/sshd_config.bak-20260728-115835`. **Key auth is now the ONLY way in — do not remove the
  key.** `79.110.49.24` in the panel is the hypervisor NODE, not the VM; do not log in there. Ubuntu 20.04.6 (EOL, glibc 2.31), Xeon E5-2680 v4, 4 cores.
  **RAM is not what was sold:** dmidecode says 8192 MB and the kernel saw `8100900K/8388064K` at
  boot, but `MemTotal` is 3941808 kB — ~4.3 GB held by `virtio_balloon`
  (`/sys/bus/virtio/drivers/virtio_balloon/virtio0`, built-in so it does not show in `lsmod`).
  A plan sold as "8 GB Dedicated" is ballooned. **Raise a ticket** — memory reclaimed mid-match
  would stall the sim for every player.

### FIRST ACTION ON THE VPS: reinstall it as Ubuntu 24.04

**Do not keep fighting 20.04.** It blocked the build at every layer and the last one is fatal without
building ffmpeg from source:

| blocker on 20.04 | 24.04 |
|---|---|
| CMake 3.16, project needs >= 3.25 | 3.28 |
| GCC 9 | GCC 13 |
| glibc 2.31 - no `strlcpy`, never any `wcslcpy` | 2.39 |
| **ffmpeg 4.2 - no `AVFrame::ch_layout`, which the OpenAL audio manager requires** | 6.x |
| EOL since April 2025 | supported to 2029 |

`SAGE_USE_OPENAL=OFF` is NOT an escape: the build then falls back to the Miles path and needs
`mss.h`, a proprietary SDK header that does not exist on Linux. So OpenAL must stay ON, so ffmpeg
5.1+ is mandatory. The savoury1 ffmpeg PPAs do not resolve on focal ("held broken packages").

24.04 is also what `build-linux.yml` and `replay-tests.yml` use, so it is the proven configuration.
The box is empty; a reinstall costs nothing. Afterwards redo: SSH key, `Acquire::ForceIPv4`, vcpkg
full clone, and the apt list below (most of which 24.04 already satisfies).

### VPS build recipe (done on 20.04; most of it becomes unnecessary on 24.04)

```
apt: Acquire::ForceIPv4 "true" in /etc/apt/apt.conf.d/99force-ipv4   # DNS returns IPv6-only
                                                                     # mirrors and the box has no v6
CMake 3.28.6 to /usr/local/bin        # distro 3.16 < the project's required 3.25
g++-11 from ppa:ubuntu-toolchain-r/test
vcpkg at /root/vcpkg, FULL clone      # --depth 1 fails: the manifest pins a baseline commit
apt: autotools, nasm/yasm, SDL3 X11 dev deps, libpng/jpeg/tiff/webp-dev,
     libav*/libsw*-dev  (libdecor-0-dev does not exist on 20.04 and is not needed)
cmake --preset linux64-deploy -DRTS_BUILD_OPTION_FFMPEG=OFF
```

**Use the preset** — a bare `cmake -S . -B` defaults to Unix Makefiles and collides with the
preset's Ninja. Build with `setsid nohup`; a plain `nohup ... &` over SSH does not survive.
`RTS_BUILD_OPTION_FFMPEG=OFF` is right for a server: ffmpeg is iOS-gated in the manifest so Linux
wants *system* ffmpeg, and `FFmpegVideoPlayer.cpp` uses the 5.1+ `AVFrame::ch_layout` API while
20.04 ships 4.2. A relay decodes no video.

## Bugs fixed this session that were NOT desyncs

* **Terrain draw window ignored camera height** (`W3DView::updateTerrain`). Sized from camera PITCH
  only, and zoom scales the camera position uniformly so pitch is invariant under zoom — the window
  stayed ~1350 world units at every zoom, and past the design height the rest renders black with
  objects still drawn over it (the heightmap is `Set_Force_Visible(TRUE)` and bounded solely by the
  draw size; props are frustum-culled with whole-map bounds). **NOT VISUALLY VERIFIED — zoom out
  and look.**
* **Three Linux build breaks**, all masked by one wrong assumption: `config-build.cmake` defined
  `HAVE_STRLCPY`/`HAVE_WCSLCPY` for every UNIX. glibc gained `strlcpy` only in 2.38 and has NEVER
  had `wcslcpy`. Now detected with `check_symbol_exists`. That then exposed unconditional
  `strlcpy`/`strlcat` definitions in BOTH trees' `CompatLib/socket_compat.h`, now behind
  `GX_COMPAT_PROVIDE_STRLCPY`. **CI runs `ubuntu-latest` (glibc 2.39) where `strlcpy` exists, so CI
  stays green while every older-distro build breaks** — that is why this survived.
* Options "Debug" button removed (hardcoded at 320,528, landed on the Scroll Speed slider and ate
  its clicks). The floating debug overlay is untouched and is still the way into the Debug screen.
* Headless CRC-mismatch crash — `TheInGameUI->message()` killed headless runs; guarded on
  `m_headless`.

## Known-open, root-caused, NOT fixed

* **AWOL players block victory.** `hasSinglePlayerBeenDefeated` (`VictoryConditions.cpp:328`) is
  purely asset-based and never consults connection state, so "Exit to lobby" leaves your base
  standing and nobody can win. `GameLogic::quit()` DOES send `MSG_SELF_DESTRUCT` for multiplayer,
  but there is an early `return` when `canOpenQuitMenu()` is true and a `!isInSkirmishGame()` gate.
  Which branch was hit needs a trace. **CAUTION:** `VictoryConditions::update` calls
  `p->killPlayer()`, so it mutates simulation state — any fix must use state all peers agree on or
  it will desync.
* Defeated observers are never pulled to the score screen.
* Windows still exits `0xC0000005` at the END of a headless replay run (after all frames complete
  and CRCs are written) — cosmetic for now, but it is a real crash.
* `%` unescaped in the SagePatch.ini generator (`GameEngine.cpp:518`) — writes `~5more`, and is UB.
* Windows cursor path (`Win32Mouse.cpp:376`) — same relative-path class as the `.scb` bug.

## Rules that earned their place

1. **Verify before claiming. Gate on the command's own exit code.**
   `apt-get ... | tail` then `$?` reads *tail's* exit code — that cost a wasted VPS build cycle this
   session. Capture `$?` before any pipe, and verify installs by checking the tool is present.
2. **Grep filters can hide the evidence that disproves you.** "TerrainDrawDistanceScale is dead
   code" came from a grep excluding every line containing `GlobalData` — and the real usage line is
   `TheGlobalData->m_terrainDrawDistanceScale`. It is live at `W3DView.cpp:3753`. Filter by PATH.
3. **A subagent's confidence is not evidence.** A determinism sweep and its adversarial verifier
   both rated the `AISkirmishPlayer` trig finding high-confidence and both were right that the code
   was raw libm and reachable — but neither checked whether the two platforms actually DIFFER there.
   They do not. A probe compiled on both toolchains settled it in minutes.
   Evidence: `docs/WORKDIR/evidence/trigconf*`.
4. **"The CRC agreed until frame N" does not mean the simulations agreed.** `Object::crc` hashes
   nine fields per object and never walks the behavior modules, so velocity, Locomotor internals and
   AI goal/path state are invisible. Divergence can incubate unhashed for hundreds of frames.
5. **Commit before building the binaries that will face each other**, or `sourceID` differs and the
   join is refused. The digest hashes HEAD plus a dirty-file overlay.
6. Count processes: `pkill -f GeneralsXZH` then `pgrep -f GeneralsXZH | wc -l`. macOS `pgrep` has
   no `-c`.
7. Never write into the Steam folder. Baseline manifest
   `9793E5EE7FCEDAF250C7403B6D7D01C0426F993B260EB233E49B079A27134033` — unchanged.
