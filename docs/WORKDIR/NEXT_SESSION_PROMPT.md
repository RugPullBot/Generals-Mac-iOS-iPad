# Kickoff prompt for the next session

Paste everything below the line into a fresh session.

---

Read these first, in this order:

1. `~/GeneralsX-src/docs/WORKDIR/STATE_2026-07-28_session5.md` — **START HERE.** The most recent
   dropoff. Session 4's open question is **ANSWERED**: macOS and Linux simulate identically
   (800 frames, byte for byte), so the divergence seen online is **transport**. Build the relay.
2. `~/GeneralsX-src/docs/WORKDIR/DESIGN_headless_and_relay.md` — the relay design, the table of
   eight silent failures, and the working VPS build recipe.
3. `~/GeneralsX-src/docs/WORKDIR/STATE_2026-07-28_session4.md` — the headless CLI, the asset-root
   fix, and the rules that came out of them. Its "THE ONE OPEN QUESTION" is now closed by session 5.
4. `~/GeneralsX-src/docs/WORKDIR/STATE_2026-07-28_session3.md` — older, for the two desyncs only.
   Anything it says about the `.scb` workaround is superseded by session 4.
5. `git -C ~/GeneralsX-src log --oneline -30` — the commit messages carry the *why*.

**Goal:** Mac, iPad and Windows play together without desyncing — **DONE, see below** — and next,
online play through a relay server so it is not limited to one LAN. **The relay is now unblocked:
nothing is waiting on a determinism question any more.**

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

**The ROOT CAUSE IS NOW FIXED (2026-07-28 session 4), not just worked around.**
`Win32BIGFileSystem` resolved `CNC_GENERALS_ZH_PATH` and then discarded it: it never called
`TheLocalFileSystem->setAssetRootPath`, and `Win32LocalFileSystem` inherited the base-class no-op,
so every loose (non-BIG) file on Windows was cwd-relative only. `StdLocalFileSystem` has had the
asset-root fallback since 23/03/2026 behind a comment reading *"On Windows cwd == install dir so
this is never needed"* — false the moment the game runs from a staging folder.

One defect, three symptoms, all previously carrying their own staging workaround:
`Data\Scripts\SkirmishScripts.scb` (this desync), `Data\Cursors` (`Win32Mouse.cpp:376`), and
`MapsZH.big` (built-in maps absent from the map cache).

**Proof:** AI match, cross-platform, with `Data\Scripts` AND `MapsZH.big` both deleted from the
Windows run folder — **1500 frames, zero differing**. Evidence in
`docs/WORKDIR/evidence/xplat-lan-ai-1500-*`, including the slot list showing `CH` (Computer Hard)
in slot 2, because a soak that silently ran human-only would pass without exercising the bug at
all. The staging in `setup-run-win64.ps1` is now belt-and-braces, deliberately kept rather than
load-bearing.

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

**Mac headless host + Windows headless joiner, 3000 frames, zero differing. Reproduced.**

```bash
FRAMES=3000 ./scripts/test/xplat-lan-soak.sh     # prints PASS/FAIL on its own
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

Also: the Windows joiner needs `CNC_GENERALS_ZH_PATH` set or it hangs forever at
`[INI] ERROR: No files read from directory` with 1239 bytes of stderr. That is NOT the session-0
problem — a headless replay fails identically without it and succeeds from session 0 with it.

`setup-run-win64.ps1` now stages `MapsZH.big` as well (verified from a clean state). Without it the
Windows map cache indexes only the user maps under Documents, finds zero built-in `maps\...`
entries, and the joiner reports "You do not have the map" for a map it can play in single player.
**That is the THIRD instance of the same defect**, after `Data\Scripts` (the AI desync) and
`Data\Cursors`: loose data opened by a working-directory-relative path that ignores
`CNC_GENERALS_ZH_PATH`. Fixing the engine's path resolution once retires all three workarounds and
is probably the highest-value cleanup left.

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
  key.** `79.110.49.24` in the panel is the hypervisor NODE, not the VM; do not log in there.
  **Reinstalled as Ubuntu 24.04.4 LTS** (session 4), Xeon E5-2680 v4, 4 cores. `gdb` installed in
  session 5. **The build is GREEN.**
  **RAM is still not what was sold, on a FRESH install:** `MemTotal` 3.75 GB on a plan sold as
  "8 GB Dedicated", with `virtio_balloon` bound at
  `/sys/bus/virtio/drivers/virtio_balloon/virtio0`. The reinstall ruled out the old OS as the
  cause, so it is plan-level config. **Raise a ticket** — memory reclaimed mid-match stalls the sim
  for every player.

### VPS build recipe — WORKING and reproducible

`scripts/build/linux/build-linux-relay.sh` (mirrored on the box at `/root/vpsbuild.sh`) configures
and builds. `GeneralsXZH` comes out a 197 MB ELF with every library resolved; it hosts and
simulates headless. Deploy with `cp build/linux64-deploy/GeneralsMD/GeneralsXZH /root/gamedata/`.

```
apt: build-essential cmake ninja-build git pkg-config curl zip unzip tar ca-certificates
     autoconf automake libtool nasm yasm gdb
     libx11-dev libxext-dev libxrandr-dev libxi-dev libxcursor-dev libxfixes-dev
     libxss-dev libxkbcommon-dev libwayland-dev libdecor-0-dev libgl1-mesa-dev
     libpng-dev libjpeg-dev libtiff-dev libwebp-dev
     libav*-dev libsw*-dev libasound2-dev
     libxtst-dev libxxf86vm-dev libxinerama-dev libdrm-dev libgbm-dev
     libpulse-dev libudev-dev libdbus-1-dev libibus-1.0-dev libsndio-dev
vcpkg at /root/vcpkg, FULL clone   # --depth 1 fails: the manifest pins a baseline commit
repo  at /root/GeneralsX           # keep it a PURE MIRROR:
                                   # git fetch && git reset --hard origin/main, never pull
```

**Use the preset** — a bare `cmake -S . -B` defaults to Unix Makefiles and collides with the
preset's Ninja. Build detached with `setsid nohup`; a plain `nohup ... &` over SSH does not survive.

Turn the tool targets OFF (`RTS_BUILD_*_TOOLS=OFF`, `*_EXTRAS=OFF`). Nearly every one is a Windows
GUI program pulling `afxwin.h`/`commctrl.h`, they default to **ON**, and `linux64-deploy` overrides
none of them — so the build compiles thousands of game objects and then dies in an editor a relay
does not ship.

**`RTS_BUILD_OPTION_FFMPEG` must be ON**, despite a relay decoding no video. This REVERSES the older
advice that OFF suits a server. `Core/GameEngineDevice/CMakeLists.txt:305` *compiles*
`FFmpegFile.cpp` when the option is OFF — OpenALAudioCache needs it for **audio** decoding — but
links the ffmpeg libraries only when it is ON, so OFF dies at the final link of `GeneralsXZH` with
~30 undefined `av_*`/`avcodec_*` symbols. **Latent on every platform, not a Linux quirk.**

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
6. **Never `pkill -f` / `pgrep -f` for the game.** `-f` matches the FULL command line, including
   your own — `ssh root@host '... pkill -f GeneralsXZH ...'` kills the remote shell running the
   command and the ssh returns 255. Recorded in session 4 for `pgrep`, hit twice again in session 5
   for `pkill`. Use `pkill -x GeneralsXZH` and count with `ps -eo comm | grep -cx GeneralsXZH`.
   A `pkill` that killed nothing looks identical to one that worked — always confirm the count.
7. **Give every check its own exit-code test.** `a && b && c || echo OK` prints OK when `a` fails.
   That reported a verification as passed in session 5 when the command had never run. Same family
   as `grep -c` exiting 0 on a match.
8. **Attach to a live process before adding printfs.** A hung headless run was root-caused in three
   `gdb -p <pid> -batch -ex "thread apply all bt"` samples; `State: R` in `/proc/<pid>/status` had
   already ruled out an I/O block. The plan to instrument the match loop would have found nothing,
   because the hang was upstream of it.
7. Never write into the Steam folder. Baseline manifest
   `9793E5EE7FCEDAF250C7403B6D7D01C0426F993B260EB233E49B079A27134033` — unchanged.
