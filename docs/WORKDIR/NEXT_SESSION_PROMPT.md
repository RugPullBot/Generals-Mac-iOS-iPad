# Kickoff prompt for the next session

Paste everything below the line into a fresh session.

---

Read these first, in this order:

1. `~/GeneralsX-src/docs/WORKDIR/STATE_2026-07-29_session6.md` — **START HERE.** The most recent
   dropoff. Online play works: a human played a real match across macOS + Windows + a headless Linux
   host, through our relay, over the internet — 14,756 frames, byte-identical streams, zero
   mismatches. It is also explicit about what is **built but never run**, which is a lot.
2. `~/GeneralsX-src/tasks/todo.md` — the ordered plan. Phases 1 and 2 are essentially done; phase 3
   onward is the work.
3. `~/GeneralsX-src/docs/WORKDIR/DESIGN_headless_and_relay.md` — the relay design, the table of eight
   silent failures, and the working VPS build recipe.
4. `~/GeneralsX-src/docs/WORKDIR/STATE_2026-07-28_session5.md` — the OpenAL hang and the
   macOS/Linux determinism proof, in full.
5. `git -C ~/GeneralsX-src log --oneline -30` — **the commit messages carry the *why*.** They are
   unusually detailed for this stretch; read the bodies, not just the subjects.

**Goal:** strangers on macOS, Windows, iPadOS and iOS find each other in a browser and play together,
off-LAN, with nothing hand-configured.

## HARD CONSTRAINT: someone may be playing right now

The relay runs as a service on the VPS and real people have used it. **Do not restart the relay, kill
anything named `GeneralsXZH`, or run a deploy script without asking.** Read-only SSH is fine.

## THE STATE OF PLAY

**Proven, do not re-investigate:**

| what | evidence |
|---|---|
| macOS and Linux simulate **identically** — 800 frames solo, no networking at all | `evidence/lanseed-solo-800-*` |
| macOS ↔ Windows on LAN, 3000 frames | `evidence/xplat-lan-3000-*` |
| macOS ↔ Linux over the relay, 1200 frames | `evidence/relay-inet-mac-vs-linux-1200-*` |
| three peers in one relay room, 900 frames | `evidence/relay-3peer-900-*` |
| Windows ↔ Linux over the relay, 600 frames | `evidence/relay-win-vs-linux-600-*` |
| **a human-played match, three platforms, 14,756 frames** | `evidence/relay-played-match-14756-*` |

Every one has its distinct-CRC count equal to its frame count (so the sim was working, not idling)
and `aiPlayers=1` with a `CH` slot in the serialised slot list (so the AI path was exercised). **Report
both alongside any future diff** — an earlier "identical" run had three distinct values across 600
frames and proved nothing.

**Built but never run — treat as unproven:**

* **Relay-assigned identities.** `GXWHO`/`GXYOU`, both halves plus the GUI path. Covered only by
  `tools/relay/test-lobby.js` (11/11). The 14,756-frame match ran four commits earlier with virtual
  addresses allocated **by hand**.
* **The Online button** (`508d6c563`) — never clicked in a built binary.
* **The Direct Connect game browser UI** — the committed browse evidence is the headless `-lanlist`
  path, not the dropdown.
* **8 peers in a room** — `MEMBERS_PER_ROOM = 8`, but the largest real game was 3.

**The sharpest known gap:** `Transport::setRelayRoom` has **zero call sites**. Every client uses
`prefs.getRelayRoom()` or the literal `"default"`, so one relay is **one game at a time**, and two
groups today land in the same room and corrupt each other's lobby.

**Suggested first move:** play one match with `LocalVirtualIP` removed from every `Options.ini`. It
proves the identity work, exercises the Online button and the browser on the way in, and is the last
thing standing between this and two strangers being able to play.

## The desyncs — FIXED, root-caused, do not re-open

**1. The cross-platform desync. 74,705 frames across three architectures, zero differing.**
Root cause: 69 libm calls the first `gamemath.h` work never reached, because `gamemath.h` had exactly
ONE include site in the whole tree (`wwmath.h:40`) and nothing `#define`s the libm names. 40 raw
`sinf`/`cosf` in `matrix3d.h`/`matrix3.h`/`vector3.h` (the `Rotate_*` helpers, reached every frame
from `PhysicsBehavior::doPhysics`) and 29 raw `atan2` in GameLogic. All routed now.

**2. The AI-only frame-0 desync. A missing data file, from a path-resolution defect — fixed at the
source.** `Win32BIGFileSystem` resolved `CNC_GENERALS_ZH_PATH` and then discarded it: it never called
`TheLocalFileSystem->setAssetRootPath`, and `Win32LocalFileSystem` inherited the base-class no-op, so
every loose (non-BIG) file on Windows was cwd-relative only. One defect, three symptoms:
`Data\Scripts\SkirmishScripts.scb` (this desync), `Data\Cursors`, and `MapsZH.big`. Proof by
intervention: with `Data\Scripts` **and** `MapsZH.big` deleted from the Windows run folder, 1500
frames zero differing (`evidence/xplat-lan-ai-1500-*`).

*The identifying signature:* every starting unit at the same radius from its Command Center but a
different **angle** means an RNG desync, not float drift. *And:* **SimID cannot catch this class of
bug** — `.scb` is not INI, so it is outside `m_iniCRC`; both peers reported identical
`engine`/`source`/`data`/`ordinal` while simulating different games. Extending the data fingerprint to
loose simulation-relevant files is still on the list.

## The tools

**Headless CLI (ZH only — `Generals/` still needs it).** `-lanhost -lanjoin -lanlist -lanmap -lanname
-lanai -lanwait -lanframes -lanseed -lantimeout`, in
`GeneralsMD/.../Common/HeadlessMatch.{h,cpp}`, entered from `GameMain` as a third branch beside
`ReplaySimulation`.

```bash
FRAMES=3000 ./scripts/test/xplat-lan-soak.sh     # prints PASS/FAIL on its own
```

**Cross-platform headless replay.** A `.rep` is a command log, so replaying re-runs the whole
simulation — one file on two platforms reproduces a cross-platform desync with no lobby, no network
and no second human, at 68x realtime. **Use it before asking anyone to play.**

```bash
cd ~/GeneralsX/GeneralsZH
GX_REPLAY_XPLAT=1 ./run.sh -headless -replay aitest.rep 2> mac.err
grep '^\[GXCRC\]' mac.err > mac.crc      # then cmp against the other machine
```

Caveat: the committed fixture `evidence/ai-hardai-fixture.rep` no longer loads against the current
engine.

## The three machines + the VPS

- **Mac mini M4** — repo `~/GeneralsX-src`, game `~/GeneralsX/GeneralsZH/`, LAN `192.168.10.51`.
  Build `./scripts/build/macos/build-macos-zh.sh --build-only` (drop `--build-only` if you cleared the
  CMake cache), deploy `./scripts/build/macos/deploy-macos-zh.sh`. `caffeinate -dimsu` is running.
- **Windows 11 `r0se-desktop`** — `ssh User@192.168.10.89`, PowerShell (`;` not `&&`).
  Clone `C:\dev\GeneralsX`, build `cmd /c C:\dev\cb.bat win64 x64`, stage
  `scripts/build/windows/setup-run-win64.ps1`, run folder `C:\dev\GeneralsX-run\`. Launch with
  `C:\dev\gxrun_session1.ps1` (a transient `schtasks /it` task) — **straight from SSH you land in
  session 0**, where DXVK enumerates zero adapters and `W3DDisplay::init()` dies at `0xC0000005`.
  The joiner needs `CNC_GENERALS_ZH_PATH` set or it hangs at `[INI] ERROR: No files read from
  directory` with 1239 bytes of stderr. **The dev build sets `RTS_DEBUG_MULTI_INSTANCE`** so it can
  run alongside retail — that used to disable relay mode silently, fixed in `15d18ed30`.
- **iPad Air 11-inch (M3)** — **four commits behind and not reinstalled.** The engine binary in
  `build/ios-vulkan/GeneralsMD/GeneralsXZH.app/` and the package in `build/ios-package/` both predate
  `973aed2d4`. `sourceID` has moved, so it cannot join at all. **Rebuild the `ios-vulkan` preset
  first**, then `./scripts/build/ios/package-ios-zh.sh --install`, over USB, never WiFi. The
  packaging script **does not build the engine** — it copies whatever binary is on disk.
- **iPhone** — offline. Not built, not installed, never tested.
- **VPS `163.5.210.131`** — root, SSH key installed, Ubuntu 24.04.4 LTS, Xeon E5-2680 v4, 4 cores,
  build GREEN, `gdb` installed. **Password auth is DISABLED — key auth is the ONLY way in, do not
  remove the key.** Backup at `/root/sshd_config.bak-20260728-115835`. `79.110.49.24` in the panel is
  the hypervisor NODE, not the VM; do not log in there.
  **RAM is still not what was sold, on a fresh install:** `MemTotal` 3.75 GB on a plan sold as "8 GB
  Dedicated", `virtio_balloon` bound. **Karl needs to raise a ticket** — memory reclaimed mid-match
  stalls the sim for every player, and there are real players now.

### VPS build recipe — WORKING and reproducible

`scripts/build/linux/build-linux-relay.sh` (mirrored at `/root/vpsbuild.sh`) configures and builds.
`GeneralsXZH` comes out a ~197 MB ELF with every library resolved. Deploy with
`cp build/linux64-deploy/GeneralsMD/GeneralsXZH /root/gamedata/` — **but not while someone is playing.**

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

**Use the preset** — a bare `cmake -S . -B` defaults to Unix Makefiles and collides with the preset's
Ninja. Build detached with `setsid nohup`; a plain `nohup ... &` over SSH does not survive.
Turn the tool targets OFF (`RTS_BUILD_*_TOOLS=OFF`, `*_EXTRAS=OFF`) — nearly every one is a Windows
GUI program pulling `afxwin.h`, they default to ON, and `linux64-deploy` overrides none of them.
**`RTS_BUILD_OPTION_FFMPEG` must be ON**, despite a relay decoding no video:
`Core/GameEngineDevice/CMakeLists.txt:305` *compiles* `FFmpegFile.cpp` when the option is OFF (OpenAL
audio decoding needs it) but links the ffmpeg libraries only when it is ON. Latent on every platform.

## Relay topology facts that will bite

* **The relay and a relay-mode peer cannot share a machine.** Relay mode forces a wildcard bind (a
  virtual address can never be bound), so the game collides with the relay's own listener and the host
  dies with `UDP::Bind failed on 0.0.0.0:8086`.
* **To test more clients than you have machines, use one network namespace per peer.** Multi-instance
  disables relay mode on macOS/Linux by design.
* Ports: **8086 lobby, 8088 in-game.** Both go through the relay; both need a registration.
* Wire format: `[ 'GXR1' ][ src virtual IP ][ dst virtual IP ][ untouched game packet ]`, big-endian,
  **inside** the 1100-byte payload budget. The relay never reads past byte 12.
* Control datagrams, plaintext, recognised by tag alone: `GXRLY` (register/keepalive), `GXADV`
  (advertise, host, every 5 s), `GXLIST` / `GXGAME` (browse), `GXWHO` / `GXYOU` (identity).
* **No teardown message anywhere, on purpose.** A host that crashes cannot send one, and that is the
  case that has to work — rooms and listings expire on the sweep instead.

## Known-open, root-caused, NOT fixed

* **AWOL players block victory.** `hasSinglePlayerBeenDefeated` (`VictoryConditions.cpp:328`) is purely
  asset-based and never consults connection state, so "Exit to lobby" leaves your base standing and
  nobody can win. `GameLogic::quit()` does send `MSG_SELF_DESTRUCT` for multiplayer, but there is an
  early `return` when `canOpenQuitMenu()` is true and a `!isInSkirmishGame()` gate; which branch is hit
  needs a trace. **CAUTION:** `VictoryConditions::update` calls `p->killPlayer()`, so any fix mutates
  simulation state and must use state all peers agree on or it desyncs. **More likely to be hit now
  that people can actually play online.**
* Defeated observers are never pulled to the score screen.
* Windows exits `0xC0000005` at the END of a headless run, after all frames and CRCs are written.
* `%` unescaped in the SagePatch.ini generator (`GameEngine.cpp:518`) — writes `~5more`, and is UB.
* `SDL3GameEngine::createAudioManager(Bool dummy)` does `(void)dummy;` — headless on Mac/Linux runs the
  full OpenAL backend. Win32 returns `MilesAudioManagerDummy`; there is no OpenAL dummy to return.
* Terrain draw-window fix (`W3DView::updateTerrain`) is **still not visually verified** — zoom out and
  look.
* `Generals/` (base game): no headless CLI, and the `LANEnableStartButton` null-deref fixed in
  `GeneralsMD/` is still live there.

## Rules that earned their place

1. **Verify the ARTIFACT, not the git tree.** `package-ios-zh.sh` does **not** build the engine — it
   errors only if the binary is missing, then copies whatever is there. A stale engine shipped to the
   device **twice**. A clean `git rev-parse` says nothing about what is inside a `.app`; check the
   binary's mtime against the commit, or read `SIMID source=` out of the running artifact.
2. **Gate on the command's own exit code, before any pipe.** `apt-get ... | tail` then `$?` reads
   *tail's* exit code. `grep -c` exits 0 **when it finds matches**, so `build && grep -c error &&
   commit` commits on failure. `a && b && c || echo OK` prints OK when `a` fails — that reported a
   verification as passed when the command had never run.
3. **Never chain a deploy after an unchecked build.** `build ; scp` and `build | tail && deploy` both
   ship the *previous* binary when the build fails, and the deploy succeeding is what you see.
4. **Commit BEFORE building the binaries that will face each other.** `sourceID` bakes HEAD plus a
   dirty-file overlay in at *compile* time, so matching `git rev-parse` proves nothing. The mirror
   image bit too: editing the tree *after* committing made the Mac compile a fix that was never in the
   commit, and the Linux build broke on it.
5. **`paste a b | awk '$2!=$4'` compares shifted columns.** Once `paste` joins two `[GXCRC]` lines the
   fields renumber — `$2` is `f=0`, `$4` is `[GXCRC]`. It reported 603 differing frames while printing
   rows that were plainly identical. Use `$3` vs `$6`, or just `cmp` two equal-length files. Also
   **strip CR** — the Windows peer writes `\r\n` and once made a perfect 1200-frame run look like a
   desync from frame 0.
6. **An "identical" run with few distinct CRC values is not evidence.** A 600-frame relay run came out
   identical with **three** distinct values: two idle bases. Always report the distinct-value count and
   the serialised slot list next to the diff.
7. **Never `pkill -f` / `pgrep -f` for the game.** `-f` matches the FULL command line including your
   own, so `ssh root@host '... pkill -f GeneralsXZH ...'` kills the remote shell and ssh returns 255.
   Use `pkill -x GeneralsXZH`, count with `ps -eo comm | grep -cx GeneralsXZH`, and **confirm the
   count** — a `pkill` that killed nothing looks identical to one that worked.
8. **Attach to a live process before adding printfs.** A hung headless run was root-caused in three
   `gdb -p <pid> -batch -ex "thread apply all bt"` samples; `State: R` in `/proc/<pid>/status` had
   already ruled out an I/O block. The plan to instrument the match loop would have found nothing,
   because the hang was upstream of it. Corollary: **a hang that stops at a byte-identical point is
   deterministic, not flaky** — diff a failing log against a passing one first.
9. **When a run mysteriously works, suspect the invocation, not the fix.** The first clean Linux run in
   session 5 differed from the hanging one only in being detached. Reproduce the ORIGINAL command
   before concluding anything.
10. **`LANAPI` mixes state mutation into its `On*` callbacks.** `OnAccept` calls `setAccept()`,
    `OnHasMap` calls `setMapAvailability()`, `OnPlayerJoin` calls `resetAccepted()`. In
    `HeadlessLANAPI` override ONLY what touches `TheShell` or `LANbuttonPushed` and delegate the rest.
11. **"The CRC agreed until frame N" does not mean the simulations agreed.** `Object::crc` hashes nine
    fields per object and never walks the behavior modules, so velocity, Locomotor internals and AI
    goal/path state are invisible. Divergence can incubate unhashed for hundreds of frames.
12. **A subagent's confidence is not evidence, and grep filters hide the evidence that disproves you.**
    "`TerrainDrawDistanceScale` is dead code" came from a grep excluding every line containing
    `GlobalData` — the real usage is `TheGlobalData->m_terrainDrawDistanceScale`, live at
    `W3DView.cpp:3753`. Filter by PATH, and when concluding something does NOT exist, search unbounded.
13. **Never write into the Steam folder.** Baseline manifest
    `9793E5EE7FCEDAF250C7403B6D7D01C0426F993B260EB233E49B079A27134033` — unchanged.
