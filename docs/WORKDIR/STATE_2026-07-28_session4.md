# Session 4 dropoff — headless CLI done, online play working, one open question

Read with `DESIGN_headless_and_relay.md` (traps table + VPS recipe) and `NEXT_SESSION_PROMPT.md`.

## DONE and PROVEN

**1. Headless host/join CLI.** `-lanhost -lanjoin -lanmap -lanname -lanai -lanwait -lanframes
-lantimeout -lanseed`, in `GeneralsMD/.../Common/HeadlessMatch.{h,cpp}`, entered from `GameMain`
as a third branch beside `ReplaySimulation`. **ZH only** — `Generals/` still needs it.

Mac hosts + Windows joins: **3000 frames, zero differing**. With an AI: **1500 frames, zero
differing**. Harness `scripts/test/xplat-lan-soak.sh` prints its own PASS/FAIL.

```bash
FRAMES=3000 ./scripts/test/xplat-lan-soak.sh
AI=Hx1 FRAMES=1500 ./scripts/test/xplat-lan-soak.sh
```

**2. The AI desync root cause is FIXED, not worked around.** `Win32BIGFileSystem` resolved
`CNC_GENERALS_ZH_PATH` and then discarded it — it never called
`TheLocalFileSystem->setAssetRootPath`, and `Win32LocalFileSystem` inherited the base no-op, so
every loose (non-BIG) file on Windows was cwd-relative only. One defect, three symptoms:
`Data\Scripts\SkirmishScripts.scb` (the AI desync), `Data\Cursors`, and `MapsZH.big`.
Proven with `Data\Scripts` AND `MapsZH.big` DELETED from the Windows run folder — the exact
configuration that used to give 13,919/13,919 differing frames. Evidence including the slot list
(`CH` in slot 2, so the AI provably existed) in `docs/WORKDIR/evidence/xplat-lan-ai-1500-*`.

**3. Linux relay host builds and runs.** VPS reinstalled as Ubuntu 24.04.4. `GeneralsXZH` 197 MB,
all libs resolved, simulates 200 frames headless. `scripts/build/linux/build-linux-relay.sh`.

**4. Online play over the internet works at the lobby level.** A Mac behind NAT joined a match
hosted on the VPS public IP:
```
[LAN] NAT: host sees us as 185.115.100.15, local is 192.168.10.51
[GXLAN] joined game
```

**5. macOS arm64 vs Linux x86-64 agree from frame 0** — 60 frames identical over the internet.
`docs/WORKDIR/evidence/xplat-inet-mac-vs-linux-60-*`.

**6. VPS security.** Key auth only; password auth verified refused (`Permission denied
(publickey)`). Cloud image ships `60-cloudimg-settings.conf` forcing passwords ON, and sshd honours
the FIRST occurrence of a keyword — so the override lives in `10-hardening.conf`, which sorts
first. Backup `/root/sshd_config.bak-*`. **The key is now the only way in.**

## THE ONE OPEN QUESTION — resolve this before building the relay

**Mac↔Linux over the internet agree exactly for frames 0-61 and diverge at frame 62.** Same frame
in two runs with *different seeds*; frame counts identical both times (mac 371, linux 375). That is
systematic, not packet loss. Two readings needing opposite responses:

* **In-game transport is not traversing NAT** (the lobby does). Then the relay is the fix and the
  simulation is fine. Supporting: the lobby uses 8086 where the joiner initiates so NAT holds the
  mapping, while gameplay uses 8088 with peers addressing each other from the slot list — where
  the Mac is listed by a public IP whose 8088 mapping was never created.
* **macOS and Linux genuinely diverge at frame 62.** Then it must be fixed FIRST: a relay that
  arbitrates CRCs is worthless if the platforms disagree.

**The decisive experiment is built but NOT finished.** `-lanseed <n>` pins the match seed so two
SOLO headless runs (no network at all) are directly comparable:

```bash
# Mac  - DONE: 800 frames, 800 CRC lines, SD=424242 confirmed in the options string
cd ~/GeneralsX/GeneralsZH && ./run.sh -headless -lanhost solo \
  -lanmap 'maps\twilight flame\twilight flame.map' -lanai Hx1 -lanseed 424242 -lanframes 800
# Linux - NOT FINISHED, see below
ssh root@163.5.210.131 'bash /root/solohost.sh'   # writes /root/soloLinux.err
```
Mac output: `/private/tmp/.../scratchpad/soloMac.err` (regenerate if gone).
Then `grep '^\[GXCRC\]'` both and diff. **Identical ⇒ transport, so build the relay.
Divergent ⇒ real determinism bug, fix that first.**

### Blocker on that experiment

The Linux solo run reached `[GXLAN] game starting` and then produced **zero `[GXCRC]` lines** while
still running — the Mac produced 800 from the identical command. It is NOT the audio-quit bug
(fixed in `48e47feff`); that one exited immediately, this one hangs after starting. Unknown whether
it is stalled or simulating without emitting CRC. **Start here next session** — instrument the
match loop on Linux (log `isInGame()` / `getFrame()` per iteration) rather than guessing.

## Fixes landed this session (all pushed)

| commit | what |
|---|---|
| `047d5a5dd` | the headless CLI itself |
| `8c7d7c764` | open the peer slots — every join was denied `GAME_FULL` |
| `d667cf9e5` | advertise map CRC/size, or every peer "does not have the map" |
| `f23668cd4` | delegate the LANAPI callbacks that MUTATE state |
| `f5c7159d6` | null-guard `LANEnableStartButton` — crashed the host on every join |
| `2ccdc108e` | **Win32 asset-root fallback — the AI desync root cause** |
| `48e47feff` | missing music must not quit the engine in headless |
| `91dc1fcc5` | "not yet in game" is not "game over" |
| `d9ebe45cf` | report the frame high-water mark, not the post-teardown counter |
| `40adf749a` | LANAPI's 5 s join deadline is a LAN figure |
| **`2ccdc108e`** | see above — retires three staging workarounds |
| NAT | `handleJoinAccept` accepts an accept addressed to our PUBLIC ip (direct-connect only) |

## Rules this session added

1. **`LANAPI` mixes state mutation into its `On*` callbacks.** `OnAccept` → `setAccept()`,
   `OnHasMap` → `setMapAvailability()`, `OnPlayerJoin` → `resetAccepted()`. In `HeadlessLANAPI`
   override ONLY what touches `TheShell` or `LANbuttonPushed`; delegate the rest.
2. **Commit, THEN build, on BOTH machines.** `sourceID` bakes HEAD plus a dirty-file overlay in at
   COMPILE time, so matching `git rev-parse` proves nothing. I got this wrong twice. The soak now
   refuses to run when a binary predates the last commit touching the digested dirs.
3. **`pgrep -f <name>` matches your own command line.** It reported a running game that did not
   exist, twice. Use `ps -eo comm | grep -cx GeneralsXZH`.
4. **Never sample a running process and report it as a result.** I read the host at frame 59 and
   told Karl it had stopped there; it ran to 374. Wait for exit.
5. **A zero-work run must be a FAILURE, not exit 0.** Making that loud is what surfaced the
   headless audio-quit bug one step later.
6. **When two platforms agree, that proves nothing.** macOS and Windows both hid the
   "not yet in game" race for thousands of frames; Linux exposed it immediately.

## Still open (not blocking)

* Windows exits `0xC0000005` at the END of a headless run, after all frames and CRCs are written.
* `Generals/` (base game) has no headless CLI and still needs the `LANEnableStartButton` guard
  compiled — the `macos-vulkan` target cannot build here (`glslangValidator` missing), so that
  one-line guard is committed but NOT compile-verified.
* `RTS_BUILD_OPTION_FFMPEG=OFF` cannot work with `SAGE_USE_OPENAL=ON` — `CMakeLists.txt:305`
  compiles `FFmpegFile.cpp` when OFF but links ffmpeg only when ON. Latent on every platform.
* The committed replay fixture no longer loads against the current engine.
* iPad is plugged in and connected (`Karl's iPad`, iPad15,3) but NOT reinstalled this session.
* **VPS RAM: 3.75 GB of a sold 8 GB, virtio_balloon bound, on a FRESH install** — plan-level, ticket
  still unraised. **Rotate the root password in the panel** — it is inert for SSH but still the
  console password and it was pasted in chat.
