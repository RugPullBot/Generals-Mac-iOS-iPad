# Kickoff prompt for the next session

Paste everything below the line into a fresh session.

---

## What this project is

**GeneralsX** — a cross-platform port of Command & Conquer: Generals Zero Hour, at
`~/GeneralsX-src` on the Mac mini. The goal: **strangers on macOS, Windows x64, iPadOS and iOS find
each other in a browser and play together, off-LAN, with nothing hand-configured.** Linux is the
*server* platform (the relay), not a client target.

Matchmaking runs through a small UDP relay we wrote (`tools/relay/relay.js`, Node). The **running**
service is not the repo copy: it is deployed separately to `/opt/gxrelay/relay.js` and run by systemd
as `gxrelay` (`tools/relay/README.md:135-197`). Syncing the VPS git clone does not update it.
The game is **lockstep**: every peer simulates the whole world and only orders cross the wire, so a
one-bit simulation difference between two platforms desyncs the match. Most of this project's history
is finding and killing those. Two are fixed and root-caused (69 unrouted libm calls; a Windows
path-resolution defect that hid `Data\Scripts\SkirmishScripts.scb`) — **do not re-open them.**

## Read these first, in this order

1. `~/GeneralsX-src/docs/WORKDIR/STATE_2026-07-29_session7.md` — **START HERE.** Current state,
   evidence table with reproduced numbers, the traps, ranked open work, and an explicit
   "Not verified" section. **The evidence-table numbers were recomputed from the committed `.crc`
   files; prose claims outside that table were not all re-derived.** "Not verified" lists the known
   gaps — it is not a guarantee that everything else was checked. Three prose claims that sat
   *outside* it turned out to be wrong; they are listed under "Corrections".
2. `~/GeneralsX-src/docs/WORKDIR/evidence/networked-sim-freeze-diagnosis.md` — read this BEFORE
   `networked-sim-freeze.meta.txt`, which describes a bug that turned out not to exist. The newest finding, and
   the one that decides how much of the networked corpus means anything.
3. `~/GeneralsX-src/tasks/todo.md` — the ordered plan; the short list is at the top.
4. `~/GeneralsX-src/docs/WORKDIR/DESIGN_headless_and_relay.md` — relay design, the table of eight
   silent failures, the VPS build recipe.
5. `~/GeneralsX-src/tools/relay/README.md` — the wire protocol, the settings, and **how the live
   relay is actually deployed** (systemd unit `gxrelay`, `/opt/gxrelay/relay.js`, `:135-197`).
6. `git -C ~/GeneralsX-src log --oneline -30` — **the commit bodies carry the *why*** and are unusually
   detailed here. Session 7 ran as four parallel sessions; the history is the only complete record.

## HARD CONSTRAINTS

**Someone may be playing right now.** The relay is a live service on the VPS and real people have used
it. Do not restart the relay, kill anything named `GeneralsXZH`, or run a deploy script without
asking. Read-only SSH is fine. Restarting `gxrelay` **drops every game in progress** — that is the
mechanism behind this rule, not politeness.

**Two commits are unpushed.** HEAD is `6377956d8` (rev 2189); `origin/main` is `c72eb8d96` (rev 2187).
The Linux and Windows clones sync with `git fetch && git reset --hard origin/main`, so **they cannot
reach HEAD until it is pushed** — and `git push` is an external action that needs Karl's OK. This
blocks the freeze reproduction too: it runs the committed `scripts/test/relay-8peer.sh` on the VPS.

**The compatibility gate — read before you build anything.**

* macOS, Linux and Windows are **all deployed from `c72eb8d96`**, probed at runtime:
  `rev=2187 engine=FD486019 source=B30B651C data=1839A83F ordinal=9F43F7B5 epoch=2`
  (`evidence/xplat3-5ai-1500.meta.txt:14-16`). They interoperate; a verified 1500-frame three-platform
  match is committed.
* **iOS/iPadOS is not.** Every iOS artifact embeds git SHA `2e226bf3a` (rev 2171, 18 commits back), and
  `SIMID_EPOCH` went 1 → 2 inside that range. The epoch feeds `engineID` (`SimulationId.cpp:100`),
  which is the first compare (`:553`), so the iPad is refused with `ENGINE_DIFFERS`. It is the one
  platform that must be rebuilt before it can join anything.
* **What is actually compared is not the commit.** `revision` is informational and never compared
  (`SimulationId.h:39`; `SimIdCompare` at `SimulationId.cpp:548-562`). `sourceID` is a SHA256 over a
  fixed path list **plus a dirty overlay** (`resources/gitinfo/simsourcedigest_watcher.cmake:66-105`
  and `:146-190`). So: **an identical tree at a different commit joins fine; a dirty clone at the
  right commit does not.** One uncommitted edit under `Core/GameEngine/{Include,Source}/{Common,GameLogic,GameNetwork}`,
  `GeneralsMD/Code/GameEngine/{Include,Source}/{Common,GameLogic,GameNetwork}`,
  `Core/Libraries/{Include,Source/WWVegas/WWMath}`, `cmake/`, `triplets/`, `CMakePresets.json`,
  `vcpkg.json` or `vcpkg-lock.json` moves `sourceID` and denies every join, silently.
  `git reset --hard`, never `pull`.

## What to do next, in priority order

**0. Rescue the soak artifacts.** The 75,000-frame headline has **no committed raw artifact** — only
the meta/manifest/console are in git. The 100 `.crc` streams exist only at
`/tmp/claude-501/-Users-administrator-GeneralsX-src/1f6becd6-6780-4ff5-b682-a13c6ea6d85e/scratchpad/soak50`
(3.9 MB). All 50 pairs were re-verified from there and every number holds, but a reboot deletes it.
Copy into `docs/WORKDIR/evidence/` and commit.

**1. The "networked freeze" is SOLVED — it was our own harness. Two follow-ups remain.**
Commit `1566959a9`, write-up `evidence/networked-sim-freeze-diagnosis.md`.

The harness put 8 players (3 humans + 5 Hard AI) into maps that hold **2**.
`populateRandomStartPosition` caps positions at `md->m_numPlayers` (`GameLogic.cpp:882-885`) and
marks everything past it as taken (`:928`); surplus players keep `startPos=-1`, get no Command
Center, and their AI owns nothing — while the run still reports `aiPlayers=5` and completes.

**Measured capacities. Only three standard maps hold 8:** `death valley`, `destruction station`,
`twilight flame`. `dark mountain` 4, `cairo commandos` 3, and `alpine assault`, `bitter winter`,
`desert fury`, `dust devil`, `final crusade`, `killing fields` all hold **2**.

And `alpine assault` — the control every other map was judged against — carries a **train**, whose
transform is hashed into the object CRC, so it scored 1499/1500 distinct with its AI inert:

| networked, 2 peers, 300 frames | 0 AI | 5 Hard AI |
|---|---|---|
| `twilight flame` (capacity 8) | **3 distinct** | **300 distinct** |
| `alpine assault` (capacity 2) | **299 distinct** | **299 distinct** |

Remaining work: (a) decide whether the **engine** should refuse an over-capacity lobby — the GUI
checks (`LanGameOptionsMenu.cpp:250-256`), `HeadlessMatch` does not; **guard-only or nothing**,
since reassigning start positions invalidates every CRC baseline and retail 1.04 replay
compatibility. (b) Re-run the networked corpus on legal maps and re-label the old rows. (c) Fix the
activity metric — "distinct CRC" is satisfied by any ambient mover; until then run the 0-AI control
alongside every activity claim. Task 56.

**2. iOS/iPadOS rebuild + reinstall.** The only platform that cannot join. See the redeploy section.

**3. Client chat.** Smallest well-defined piece of work in the tree. The relay half is done and tested
(`node tools/relay/test-lobby.js` → `52 checks, 52 passed, 0 failed`). The client half does not exist:
an unbounded grep for `GXCHT|GXSAY` hits **zero `.cpp`/`.h` files**. The chat box is deliberately inert
(`WOLLobbyMenu.cpp:1170` `winEnable(FALSE)`, `:1207` pushes `GUI:RelayLobbyNoChat`) and `Transport.h`
exposes no chat API — only `sendRelayRegistration` (`:105`) and `enterRelayRoom` (`:178`). One send
call, one receive hook. Task 47.

**Confirm the deployed relay is running the chat build before testing against it.** `5292259c7` says
it was not deployed and nobody has checked since. The running relay is **not** the repo copy —
`tools/relay/README.md:135-197` deploys it as systemd unit `gxrelay` executing
`/opt/gxrelay/relay.js`, so `git reset --hard` in `/root/GeneralsX` does **not** update it:

```bash
ssh root@163.5.210.131 'systemctl status gxrelay; grep -c GXCHT /opt/gxrelay/relay.js'
# read the NUMBER. grep -c exits 0 WHEN IT FINDS MATCHES - never gate on it (rule 5).
journalctl -fu gxrelay
```

Redeploy is `scp tools/relay/relay.js root@163.5.210.131:/opt/gxrelay/relay.js && systemctl restart
gxrelay` — **that restart drops every game in progress. Needs Karl's OK.** Whether the live box
actually matches the README's layout is unconfirmed from here; check before assuming.

**4. Click the Online button.** `MainMenu.cpp:1585` now pushes `Menus/WOLCustomLobby.wnd`, unguarded.
~870 lines of never-drawn UI own the whole Online path, and the Direct Connect browser that carried
real matches is no longer what that button opens. A first run must check: the screen draws at all;
list columns are the right way round; Join logs `relayJoinSelectedGame - moving from room ... to ...`
and reaches the host's accept; Host reaches `Menus/LanGameOptionsMenu.wnd`; **Back returns to the main
menu rather than the GameSpy login screen.** Tasks 25, 41-45.

**5. Take the run-ahead measurement.** See the lead below. The measurement, not a patch. Task 46.

**6. Map transfer.** Broken in headless with or without a relay. The host's transfer mask is empty
(`FileTransfer.cpp:249-258`) and the host's `OnHasMap` **never fired** — `HeadlessMatch.cpp:226-232`
would have logged `does NOT have the map` and did not. That narrows it to three early-outs in
`LANAPIhandlers.cpp::handleHasMap` (`:911`, `:917`, `:922-926`). **Do not chase the relay-assigned
virtual IP** — the identical failure reproduces on plain LAN with no relay. Decisive experiment: the
same missing-map join from a **UI client on macOS**. Task 30.

**7. Finish `scripts/test/xplat-3platform-soak.sh`** (committed in `6377956d8`; **it has now been
run** — that run is what found the freeze). Windows has ~4,200 frames of active-sim evidence (1500
`xplat3-5ai` + 1500 `xplat-lan-ai` + 600 `winhost` + 600 `win-vs-linux`), of which only **1500** have
Windows in a match with *both* other platforms, on one map with one seed — against 75,000
macOS-vs-Linux, plus the genuine 93,017-frame three-platform run (`relay-3platform-93017`, twilight
flame, 4 players, all three streams md5 `f65722d3252acf0737cb4b0e5ba3f13f`). A long soak closes the
remaining gap and is no longer blocked — but it MUST run on capacity-8 maps. Measured throughput is
**17 logic frames/sec networked**, so a 30-minute match is ~53 minutes of wall clock. Task 53.

## THE STATE OF PLAY

Frame counts below are **CRC lines**. A `-lanframes`-bounded run's own `simulated N frames` counter
**equals** its line count; only the two game-over matches (93,017 and 14,757) report one less. Full
table with distinct counts, maps and stream-completeness in the STATE doc.

**Read the STATE doc's "Not verified" section before citing any row below** — several carry
provenance or stream-completeness caveats that do not fit in this table.

**Determinism — proven, do not re-investigate:**

| what | map | frames | distinct | evidence |
|---|---|---|---|---|
| **macOS + Linux + Windows + 5 Hard AI, LEGAL lobby** | twilight flame (cap 8) | 1500 | **1500** | `evidence/xplat3-tf-legal-1500.*` |
| same, but on a 2-player map — determinism only, AI were inert | alpine assault (cap 2) | 1500 | 1499 | `evidence/xplat3-5ai-1500.*` |
| 50 solo matches, macOS vs Linux, **zero packets** | 10 maps | 75,000 | 1470-1500 | `evidence/xplat-soak-50x1500.*` (raw `.crc` NOT in git) |
| a played match, 3 endpoints, one md5 | twilight flame | 93,017 | 93,016 | `evidence/relay-3platform-93017-*` (no Windows-side log; that stream's provenance is its filename) |
| two hosts in two rooms on one relay, isolated | — | 3000 ×2 | 3000 / 2999 | `evidence/relay-tworooms-3000.*` |
| **Windows as HOST** + Linux joiner via relay | alpine assault | 600 | 599 | `evidence/relay-winhost-600.*` |
| 7 peers + an AI | alpine assault | 1500 | 1499 | `evidence/relay-7peer-ai-1500-*` (peer0 only; "21/21 pairs" not recomputable) |
| macOS ↔ Windows on LAN, **AI active** | twilight flame | 1500 | 1499 | `evidence/xplat-lan-ai-1500-*` — the real macOS↔Windows proof |
| macOS ↔ Linux over the relay | — | 1200 | 1200 | `evidence/relay-inet-mac-vs-linux-1200-*` |
| two solo runs, seed pinned, zero networking | — | 800 | 800 | `evidence/lanseed-solo-800-*` (mac rev 2143 vs linux rev 2141; same `sourceID`) |

**Transport proofs only — the sim was idle, these are NOT determinism evidence:**

| what | map | frames | distinct | evidence |
|---|---|---|---|---|
| eight peers in one room, room filled 8/8 | killing fields | 1500 per the meta; only a **400-line head sample** is committed | **3** (396 of 400 identical in the sample) | `evidence/relay-8peer.meta.txt`, `-idle-1500-peer0-head400.crc` |
| macOS ↔ Windows on LAN | — | 3000 | **3** | `evidence/xplat-lan-3000-*` (no slot list committed — "no AI" is the harness default, not an observation) |
| macOS ↔ Linux over the internet | — | 60 | **3** | `evidence/xplat-inet-mac-vs-linux-60-*` |

**Read every networked row's MAP and CAPACITY alongside its distinct count.** The 8-peer row ran on
`killing fields`, which holds 2 players, so its distinct = 3 is an overfilled lobby with no working
AI rather than "eight peers with no AI". Its network claims (8 joins per port, room 8/8,
lockstep to 1500) are unaffected — they were never determinism claims.

**Report the distinct-CRC count next to every future diff.** Three distinct values across 3000 frames
means agreement proves nothing, and this project has shipped that result as a pass more than once.

**Built but NEVER OBSERVED — treat as broken until someone looks:**

* **The WOL lobby UI** (`9c6a89f2c`, 906 insertions, 671 in `WOLLobbyMenu.cpp`). Zero Hour's own
  `WOLCustomLobby.wnd` — which ships unused in `WindowZH.big` — fed from our relay's `GXGAME` replies.
  No GameSpy reimplementation: the screens are pure presentation and do not care where rows came from.
  Compile-verified only. `NetworkDirectConnect.wnd` is deliberately **not** retired.
* **The relay chat channel** (`5292259c7`). `GXCHT <room> <text>` in, `GXSAY <room> <senderVirtualIP>
  <text>` out. Sender stamped **by the relay** — the wire format has no sender field, because a client
  that can name itself can name the host. Rate-limited, sanitised, never logged. 52/52 tests.
* **The refusal the loose-data digest would cause.** The digest itself now *does* run at runtime —
  `data-loose=300665B3` identical on three platforms — but nobody has seen it deny a join.

**Broken and known:** the engine silently accepts over-capacity lobbies and gives the surplus players `startPos=-1` (item 1
above). Map transfer in headless. Windows exits `0xC0000005` at the **end** of a headless run, after
every frame and CRC is written.

## THE BIGGEST UNACTED LEAD — lag, and it is not a networking change

In a network game `canUpdateNetworkGameLogic()` (`GeneralsMD/.../Common/GameEngine.cpp:1011-1024`)
ticks **one logic frame per RENDERED frame** — no time accumulator, unlike the single-player path
`canUpdateRegularGameLogic()` (`:1027-1059`), which has had one since the frame-pacer work. On top of
that, the shared logic rate is the **minimum render fps across all peers**:
`FrameMetrics.cpp:90` (`TheDisplay->getAverageFPS()`) → `ConnectionManager.cpp:1404` →
`getMinimumFps()` (`:1556-1568`) → `clamp(MIN_LOGIC_FRAMES, minFps, m_framesPerSecondLimit)` (`:1421`,
commented *"this clamps the logic time scale fps in network games"*) → `setFrameRate(minFps)` to
everyone (`:1457`) → `Network.cpp:656`, `:798` `frameDelay = m_perfCountFreq / m_frameRate`.

**So one client's render hitches throttle the simulation for every player.** The simulation is nowhere
near the bottleneck: **2,037 logic frames/sec = 67.9× realtime** on the M4
(`DESIGN_headless_and_relay.md:122`).

**Inferred from code, not measured.** Both audits re-derived every line reference and every one held,
but nobody has instrumented a match. **First step is the measurement:** log each peer's render fps
against the negotiated `m_frameRate` during a real match. Note `LANAPICallbacks.cpp:260` sets
`m_useFpsLimit = false` when a network game starts, so render runs uncapped while
`m_framesPerSecondLimit` still serves as the clamp ceiling. Task 46.

## The machines

- **Mac mini M4** (primary) — repo `~/GeneralsX-src`, game `~/GeneralsX/GeneralsZH/`, LAN
  `192.168.10.51`. `caffeinate -dimsu` is running.
- **Windows 11 `r0se-desktop`** — `ssh User@192.168.10.89`, PowerShell (`;` not `&&`). Clone
  `C:\dev\GeneralsX`, run folder `C:\dev\GeneralsX-run\`, DXVK 2.6 x64 at `C:\dev\dxvk\dxvk-2.6\x64`
  (a machine prerequisite, **not** build output, and outside the SimID digest). **Windows headless
  works** — it needs `CNC_GENERALS_ZH_PATH` **set**, pointing at the Steam data install, or the
  engine stalls forever after `[INI] ERROR: No files read from directory`. **A trailing backslash is
  harmless but NOT required** — `Win32BIGFileSystem.cpp:282-296` appends the separator itself, since
  `ac5dcfc50`, which is an ancestor of every deployed build; `scripts/test/winhost.ps1:13` and
  `scripts/test/xplat-lan-soak.sh:22` both omit it and produced committed evidence. (An earlier draft
  of this prompt called its absence fatal, in caps. It was wrong, and the `Win32LocalFileSystem.cpp:131-137`
  citation it carried is stale — the raw concat is `:209-212`, and `setup-run-win64.ps1:31-32` still
  has the old number.) No GUI mouse-driving is needed any more. PowerShell does not wait for
  `generalszh.exe` on its own; **both `Start-Process -Wait` and `& cmd /c "generalszh.exe ..."`
  block**, and over ssh the `cmd /c` form is the one that survives session close
  (`xplat-3platform-lobby.sh:237`). A map argument with spaces **and** backslashes does not survive
  bash → ssh → PowerShell quoting — put it in a `.ps1` on the box (`scripts/test/winhost.ps1`).
- **iPad Air 11-inch (M3)** — behind and **epoch 1**; cannot join anything. Rebuild before packaging.
- **iPhone** — offline, never built, never tested. iOS is why `MAX_UDP_PAYLOAD_SIZE` is 1100
  (`NetworkDefs.h:65`) and that constraint has never met a real cellular link.
- **VPS `163.5.210.131`** — root, Ubuntu 24.04.4, Xeon E5-2680 v4, 4 cores. Repo `/root/GeneralsX`
  (a **pure mirror**), game `/root/gamedata`, relay + build host. **Password auth is DISABLED — key
  auth is the only way in, do not remove the key.** Backup at `/root/sshd_config.bak-20260728-115835`.
  `79.110.49.24` in the panel is the hypervisor NODE, not the VM; do not log in there. Namespaces
  `gx`, `gx2`..`gx8` exist for multi-peer tests, one /24 each. The relay runs here as systemd unit
  **`gxrelay`** from `/opt/gxrelay/relay.js` — separate from the git clone. **UNVERIFIED, carried
  forward from an earlier session:** RAM is 3.75 GB on a plan sold as "8 GB Dedicated" with
  `virtio_balloon` bound — Karl needs to raise a ticket. Nobody re-measured it this session.

## Redeploy: the exact commands

Only iOS needs this today. The full four-platform sequence is here because the next one will be
atomic. Pick one target commit `$C`; every machine ends at `$C`, **clean**.

**Step 0 — push (blocking, needs Karl's OK).** `git push origin main` — **two commits**,
`68eff86c4` and `6377956d8`. The remote clones cannot reach an unpushed commit.

**macOS** (repo `~/GeneralsX-src`):
```bash
cd /Users/administrator/GeneralsX-src
git status --porcelain                                   # must be clean under the digest paths
./scripts/build/macos/build-macos-zh.sh --build-only      # drop --build-only if the CMake cache was cleared
./scripts/build/macos/deploy-macos-zh.sh
```
`build-macos-zh.sh:126,134` → `cmake --preset macos-vulkan`, then
`cmake --build build/macos-vulkan --target z_generals`. The deploy **`rm -f`s the old binary before
copying** (`deploy-macos-zh.sh:87`) — overwriting in place keeps the inode and taskgated SIGKILLs the
next launch as "Code Signature Invalid". `build/macos-vulkan/libdxvk_d3d8.0.dylib` does not exist and
never will under `--target z_generals`; the Meson fallback at `deploy-macos-zh.sh:56-61` is the live
path. Do not "fix" it.

**Linux** (VPS, repo `/root/GeneralsX`):
```bash
ssh root@163.5.210.131
cd /root/GeneralsX
git fetch --all && git reset --hard $C && git status --porcelain    # MUST be empty
diff /root/vpsbuild.sh scripts/build/linux/build-linux-relay.sh     # UNVERIFIED that these match - check once
setsid nohup bash /root/vpsbuild.sh > /root/vpsbuild.log 2>&1 &
# wait for "=== VPS BUILD DONE ===" (build-linux-relay.sh:41)
pkill -x GeneralsXZH; pgrep -c -x GeneralsXZH                       # must print 0 before overwriting
cp build/linux64-deploy/GeneralsMD/GeneralsXZH /root/gamedata/
```
**Do NOT use `scripts/build/linux/deploy-linux-zh.sh`** — it hardcodes `$HOME/GeneralsX/GeneralsZH`
(`:19-34`) and cannot target `/root/gamedata`. **Do NOT delete `/root/GeneralsX/build/linux64-deploy`**:
the copied ELF is believed to resolve `libdxvk_d3d8.so`/`libSDL3.so` through the build-tree RPATH
(inferred from the absence of any RPATH override in the tree, not measured — check with
`readelf -d /root/gamedata/GeneralsXZH | grep -E 'RPATH|RUNPATH'`).
**`RTS_BUILD_OPTION_FFMPEG` must be ON** despite a relay decoding no video:
`Core/GameEngineDevice/CMakeLists.txt:305` *compiles* `FFmpegFile.cpp` when the option is OFF (OpenAL
audio decode needs it) but links the ffmpeg libraries only when it is ON.
`scripts/build/linux/build-linux-relay.sh:13-18` records what OFF actually did: *"the build dies at
the final link of `GeneralsXZH` with ~30 undefined `av_*`/`avcodec_*` symbols"*. Not re-observed at
HEAD — but do not burn a VPS build cycle re-deriving it.

**Windows** (repo `C:\dev\GeneralsX`):
```powershell
cd C:\dev\GeneralsX
git fetch --all; git reset --hard $C; git status --porcelain
cmd /c C:\dev\cb.bat win64 x64          # sets up vcvars64 + VCPKG_ROOT; NOT in the repo
powershell -ExecutionPolicy Bypass -File scripts\build\windows\setup-run-win64.ps1
```
No `-Launch` — that fires an interactive `schtasks` GUI run you do not want for a redeploy.
`cb.bat` lives only on that box; **copy it into the repo this session**, it is the largest
reproducibility gap of the four platforms. The `win64` preset sets
`"architecture": {"value":"x64","strategy":"external"}` with a Ninja Multi-Config generator, so the
caller must already have run `vcvars64.bat` — that is why a bare PowerShell build fails with
`Cannot open include file: 'time.h'`.

**iOS/iPadOS**:
```bash
cd /Users/administrator/GeneralsX-src
cmake --preset ios-vulkan
cmake --build build/ios-vulkan --target z_generals
ls -l build/ios-vulkan/GeneralsMD/GeneralsXZH.app/GeneralsXZH        # confirm the mtime MOVED
strings -a build/ios-vulkan/GeneralsMD/GeneralsXZH.app/GeneralsXZH | grep -aoE '^[0-9a-f]{40}$' | head -1
./scripts/build/ios/package-ios-zh.sh --install                       # USB, never WiFi
```
`package-ios-zh.sh:70-76` checks only that the binary **exists** and then copies it (`:152`) — it does
**not** build the engine. A stale engine has shipped to the device twice this way. One-time prereqs it
hard-fails on: MoltenVK (`scripts/build/ios/fetch-moltenvk.sh`), fonts
(`scripts/build/ios/stage-fonts.sh`), `ios/config/{dxvk.conf,Options.ini}`, `ios/signing.local.env`.

**Four stale iOS artifacts exist, and three of them look sideloadable. They are not.**

| artifact | mtime | embedded SHA |
|---|---|---|
| `build/ios-vulkan/GeneralsMD/GeneralsXZH.app/GeneralsXZH` | Jul 29 04:53 | `2e226bf3a` |
| `build/ios-package/GeneralsXZH.app/GeneralsXZH` | Jul 29 04:54 | `2e226bf3a` |
| `GeneralsXZH-ios.ipa` (repo root, 2.1 GB) | Jul 27 03:51 | predates both binaries |
| `GeneralsXZH-ios-codeonly.ipa` (repo root, 21 MB) | Jul 27 05:05 | predates both binaries |

`package-ios-zh.sh --install` over USB is the **only** supported path. The two `.ipa` files are stale
build products from before the epoch bump — **regenerate or delete them, never sideload them.**
Reaching for the obvious-looking `.ipa` is how rule 4 gets violated a third time.

### Confirming a deploy took

**Gate 1 — commit identity, no launch, no data install:**
```bash
strings -a <binary> | grep -aoE '^[0-9a-f]{40}$' | head -1
```
The full SHA is baked in via `resources/gitinfo/gitinfo.cpp.in:20`. Verified working: the deployed
macOS binary returns `c72eb8d96e6d2274fcf015cbf46aa78f49b818ba`; both iOS binaries return
`2e226bf3a13b7b5751f48fe3d3e40d7aab093191`. No equivalent one-liner has been worked out for Windows
(whether `strings(1)` is available on that box is unchecked) — use gate 2 there.

**Gate 2 — the `[SIMID]` line (authoritative; it reports exactly what the join compares).** It is an
unconditional `fprintf(stderr, ...)` (`SimulationId.cpp:461-470`, deliberately not `DEBUG_LOG`, which
compiles away in shipping presets) called from `GameEngine::init`, so **any** launch prints it.
Compare `engine=`, `source=`, `data=`, `ordinal=`. `platform=`, `parse=` and `asset=` are WARN-only.

```bash
# macOS — the cd matters (see the cwd trap below)
cd ~/GeneralsX/GeneralsZH && timeout 180 ./run.sh -headless -lanhost simidprobe -lanframes 5 2>&1 \
  | grep -a -m1 '\[SIMID\] local'; pkill -x GeneralsXZH; pgrep -c -x GeneralsXZH   # confirm 0

# Linux
cd /root/gamedata && CNC_GENERALS_ZH_PATH=/root/gamedata \
  timeout 180 ./GeneralsXZH -headless -lanhost simidprobe -lanframes 5 2>&1 \
  | grep -a -m1 '\[SIMID\] local'; pkill -x GeneralsXZH
```
```powershell
# Windows - CNC_GENERALS_ZH_PATH must be SET or it hangs forever. The trailing backslash is
# optional (Win32BIGFileSystem.cpp:282-296 appends it); kept here only for symmetry with go.bat.
$env:CNC_GENERALS_ZH_PATH = "C:\Program Files (x86)\Steam\steamapps\common\Command & Conquer Generals - Zero Hour\"
$env:DXVK_LOG_LEVEL = "none"
Start-Process cmd -ArgumentList "/c","generalszh.exe -headless -lanhost simidprobe -lanframes 5 2> C:\dev\simidprobe.err" `
  -WorkingDirectory "C:\dev\GeneralsX-run" -WindowStyle Hidden
# poll C:\dev\simidprobe.err for "[SIMID] local" (~5 s), then Stop-Process
```
iOS has no terminal: `SDL3Main.cpp:339-415` swaps stderr for a capped sink at
`$HOME/Documents/generals-stderr.log` inside the app container (previous run renamed to
`-prev.log`). Pull with `xcrun devicectl device copy from --device <uuid> --domain-type
appDataContainer --domain-identifier <GX_BUNDLE_ID> --source Documents --destination ./logs`.

**Gate 3 — the real one.** `FRAMES=1500 ./scripts/test/xplat-3platform-lobby.sh` asserts all three
fingerprints match *before* the lobby opens (`:139-183`), which is the only check that fails with a
readable message instead of a connection timeout. Use `SKIP_FINGERPRINT=1` only when you have already
asserted the builds this session — the probes are ~90 s of a short run's wall clock.

## The tools

**Headless CLI (ZH only).** `-lanhost -lanjoin -lanlist -lanmap -lanname -lanai -lanwait -lanframes
-lanseed -lantimeout`, in `GeneralsMD/.../Common/HeadlessMatch.{h,cpp}`, entered from `GameMain` as a
third branch beside `ReplaySimulation`. `GENERALSX_LANROOM` picks a room when a host address is
ambiguous. `Generals/` (base game) has `-headless` but **not** the LAN driver.

```bash
FRAMES=1500 ./scripts/test/xplat-3platform-lobby.sh                  # mac + linux + windows + 5 Hard AI
ITERATIONS=20 ./scripts/test/xplat-3platform-soak.sh                 # the same, repeated over an 11-map rotation
ITERATIONS=50 FRAMES=1500 ./scripts/test/xplat-determinism-soak.sh   # 75,000-frame solo soak
FRAMES=1500 ./scripts/test/xplat-lan-soak.sh                         # macOS <-> Windows on LAN; made xplat-lan-ai-1500
PEERS=2 FRAMES=300 AI=Hx5 ./scripts/test/relay-8peer.sh              # N peers, one netns each; freeze repro
SRC_MAP='...' ./scripts/test/relay-maptransfer.sh                    # runs ON the relay host
./scripts/test/winhost.ps1                                           # Windows as host, run it ON the box
node tools/relay/test-lobby.js                                       # 52 checks, no hardware needed
```

`xplat-lan-soak.sh` is the harness behind `xplat-lan-ai-1500`, the cited macOS↔Windows determinism
proof, and it was missing from every dropoff doc until now. Env: `MAC_GAME`, `WIN_DATA` (the Steam
install path), `MAP` (defaults to `twilight flame`), `FRAMES`. `xplat-3platform-soak.sh` env:
`ITERATIONS`, `FRAMES`, `AI`, `SEED_BASE`, `OUT`; map rotation at `:79-91` as committed. It also has
an **uncommitted** `ONLY_MAP` substring filter (+16 lines in the working tree) — set
`ONLY_MAP=alpine` to get real frames while the freeze stands, leave it empty to check whether the
freeze is fixed.

`tools/bigtool.py` lists and extracts entries from a `.big` archive (299 in `MapsZH.big`).

**Cross-platform headless replay.** A `.rep` is a command log, so replaying re-runs the whole
simulation — one file on two platforms reproduces a cross-platform desync with no lobby, no network
and no second human, at 68× realtime. Caveat: the committed fixture `evidence/ai-hardai-fixture.rep`
is reported not to load against the current engine (unverified — needs a launch).

## Relay topology facts that will bite

* **The relay and a relay-mode peer cannot share a machine.** Relay mode forces a wildcard bind (a
  virtual address is on no interface), so the game collides with the relay's own listener:
  `UDP::Bind failed (status -6) for IP 0.0.0.0 port 8086`, `transportInit=FAILED`, before frame 0. Use
  `ip netns exec`. This bit the determinism soak on its first run and made every iteration report
  "no frames simulated", which reads like a headless bug rather than a setup one.
* **To test more clients than you have machines, one network namespace per peer.** Multi-instance
  disables relay mode by design (`Transport.cpp:166-168`). Give each peer its own `XDG_DATA_HOME` —
  peers write `Network.ini` and `MapPreviews` into the user data dir and N sharing one directory race.
* **Rooms are address spaces.** Every host is `10.42.0.1` **in its own room**, so a host address alone
  cannot identify a game. `-lanjoin <ip>` exits 1 and names the candidates once two rooms exist; use
  `GENERALSX_LANROOM`. The UI does not hit this because a picked row carries its room.
* Ports: **8086 lobby, 8088 in-game.** Both go through the relay; both need a registration.
* Wire format: `[ 'GXR1' ][ src virtual IP ][ dst virtual IP ][ untouched game packet ]`, big-endian,
  **inside** the 1100-byte payload budget. The relay never reads past byte 12.
* Control datagrams, plaintext, recognised by tag alone: `GXRLY` (register/keepalive), `GXADV`
  (advertise, host, every 5 s), `GXLIST`/`GXGAME` (browse), `GXWHO`/`GXYOU` (identity), `GXCHT`/`GXSAY`
  (chat — **no client sends it yet**).
* **No teardown message anywhere, on purpose.** A host that crashes cannot send one, and that is the
  case that has to work — rooms and listings expire on the sweep. `ADVERT_TIMEOUT_MS` is 15 s, so a
  browse landing in that window after a host dies returns a stale or short list.
* **No auth, no names, no moderation.** Registration is `GXRLY <room> <virtualIP>`. Whoever knows a
  room token is in that room. The chat channel is sender-stamped and rate-limited — the anti-abuse
  floor, not moderation.

## Known-open, root-caused, NOT fixed

* **Networked matches freeze on every map tested except `alpine assault`.** Not a desync — all peers
  hold the same frozen value; the world stops changing after ~4 frames. Solo runs of the same map,
  seed and AI spec are fully active. `evidence/networked-sim-freeze.meta.txt`. Task 56.
* **Map transfer is broken in headless**, with or without a relay. Task 30.
* **Windows exits `0xC0000005`** at the END of a headless run, after all frames and CRCs are written.
* **Destroyed structures keep their old visuals.** Client-side only; all peers agree they are
  destroyed. Decisive next step: the same building on Windows — broken on both means game logic,
  broken only on macOS means the DXVK/MoltenVK path.
* **1-2 s freezes during play.** Simulation, network and DXVK file logging ruled out. Leading
  hypothesis is DXVK pipeline compilation (cold 90 KB cache, written during play) — **inferred, not
  proven.** Free check: do they reduce as the cache warms?
* **AWOL players block victory.** `hasSinglePlayerBeenDefeated` (`VictoryConditions.cpp:328`) is purely
  asset-based and never consults connection state. **CAUTION:** `VictoryConditions::update` calls
  `p->killPlayer()`, so any fix mutates simulation state and must use state all peers agree on.
* Defeated observers are never pulled to the score screen.
* `SDL3GameEngine::createAudioManager(Bool dummy)` does `(void)dummy;` — headless on Mac/Linux runs the
  full OpenAL backend. Win32 returns `MilesAudioManagerDummy`; there is no OpenAL dummy.
* Terrain draw-window fix is **still not visually verified** — zoom out and look.
* `unrouted` is no longer always zero (1 on 8086, 2 on 8088 out of ~6900 forwarded). Teardown is the
  leading explanation, read off timing, not proven.
* `origin/fix/blocker5-residual-libm-trig` still exists remotely. One-line push-delete.

## Rules that earned their place

1. **cwd is load-bearing on every platform.** `SimulationId`'s loose-data scan hands **relative**
   directories to `getFileListInDirectory` (`SimulationId.cpp:275`, consumed `:316`), resolved against
   the process cwd. Wrong cwd → scans nothing → `data-loose` changes → `dataID` changes → every joiner
   refused with `verdict=DATA_DIFFERS` while `engineID` matches perfectly. **It reads exactly like a
   genuine cross-platform data mismatch and is not one.** `cd` to the game dir, always.
2. **`ssh host "cd X && setsid game >f 2>g &"` never returns.** The and-list backgrounds into a
   subshell whose stdout is still the ssh channel, so ssh never sees EOF and blocks until the match
   ends — the launch looks like a hang. Wrap it: `ssh host "( cd X && exec setsid game ) < /dev/null
   > f 2> g & echo started"`.
3. **OpenSSH on Windows kills the process tree at session close.** A `Start-Process` launch dies
   within milliseconds of ssh returning and leaves a **zero-byte stderr file**, which reads as "the
   joiner never started". Run the game synchronously inside a held-open ssh session and background
   that ssh invocation on the near side.
4. **Verify the ARTIFACT, not the git tree.** `package-ios-zh.sh` does not build — it errors only if
   the binary is missing, then copies whatever is there. A stale engine shipped to the device twice.
   `strings -a <bin> | grep -aoE '^[0-9a-f]{40}$' | head -1` is the cheap gate; `[SIMID]` is the
   authoritative one.
5. **Gate on the command's own exit code, before any pipe.** `grep -c` exits 0 **when it finds
   matches**, so `build && grep -c error && commit` commits on failure. `a && b && c || echo OK` prints
   OK when `a` fails. Never chain a deploy after an unchecked build.
6. **Commit BEFORE building the binaries that will face each other.** `sourceID` bakes HEAD plus a
   dirty-file overlay in at *compile* time, so a matching `git rev-parse` proves nothing.
7. **`paste a b | awk '$2!=$4'` compares shifted columns.** Once `paste` joins two `[GXCRC]` lines the
   fields renumber. It reported 603 differing frames while printing rows that were plainly identical.
   Use `$3` vs `$6`, or `cmp` two equal-length files. Also **strip CR** — the Windows peer writes
   `\r\n` and once made a perfect 1200-frame run look like a desync from frame 0.
8. **An "identical" run with few distinct CRC values is not evidence.** Always report the distinct
   count and the serialised slot list next to the diff. **A harness that greens an idle run is
   manufacturing evidence** — `relay-8peer.sh` and `xplat-3platform-lobby.sh` exit INCONCLUSIVE on a
   low distinct count rather than printing PASS.
9. **An AI is not automatically active in a NETWORKED match.** `-lanai Hx1` gave **4** distinct values
   in 1500 frames on killing fields and **1499** on alpine assault; five Hard AI on killing fields
   gave 4 of 1500 with `aiPlayers=5 sidesWithScripts=7`. **The old explanation — "skirmish scripts
   attach only for AI players and not every map carries them" — is falsified.** The solo control runs
   killing fields at 600/600 distinct with the same AI spec and `sidesWithScripts=7` identical in
   both runs, so the AI *are* placed and scripted. This is the networked freeze (item 1), not a map
   property. The practical advice stands: use `alpine assault`, and always report the distinct count.
   Related: the lobby always reports eight slots regardless of map.
10. **A map is identified by CRC and size, never by name.** Renaming a stock map to make it "custom"
    is silently a no-op: the joiner resolves it locally, nothing transfers, and you get a
    byte-identical 600-frame run at 599 distinct values that looks exactly like a pass. Pad the file.
    A loose map is cached under its **full lowercased absolute path**, not the relative archive key.
11. **N peers on one machine all have the same name.** The LAN name falls back to `gethostname`, and a
    **network** namespace does not isolate the hostname — that is a **UTS** namespace. The host answers
    the second peer onward with Join Deny code **3** = `RET_DUPLICATE_NAME` and every joiner exits
    having simulated nothing. Nothing in the symptom points at naming. Use per-peer `-lanname`. Code
    **4** is `RET_CRC_MISMATCH` — a different build, identical symptom.
12. **A host started over ssh must be detached.** `setsid nohup ... < /dev/null`. Plain background jobs
    take SIGHUP at session close, and a browse inside the 15 s advert window afterwards returns a short
    list that presents exactly like a broken game list.
13. **A script that has never run has never had its assumptions checked — and running it early pays.**
    `xplat-determinism-soak.sh` sat committed for a session then failed 100% of iterations on its first
    execution — on a rule this project had already written down and applied everywhere else.
    `xplat-3platform-soak.sh` was run immediately and its **first** execution found the networked
    freeze, which is worth more than the volume it was built to collect.
14. **Never `pkill -f` / `pgrep -f` for the game.** `-f` matches the FULL command line including your
    own, so `ssh root@host '... pkill -f GeneralsXZH ...'` kills the remote shell and ssh returns 255.
    Use `pkill -x GeneralsXZH`, count with `ps -eo comm | grep -cx GeneralsXZH`, and **confirm the
    count** — a `pkill` that killed nothing looks identical to one that worked.
15. **Attach to a live process before adding printfs.** A hung headless run was root-caused in three
    `gdb -p <pid> -batch -ex "thread apply all bt"` samples. Corollary: a hang that stops at a
    byte-identical point is deterministic, not flaky — diff a failing log against a passing one first.
16. **When a run mysteriously works, suspect the invocation, not the fix.** Reproduce the ORIGINAL
    command before concluding anything.
17. **"The CRC agreed until frame N" does not mean the simulations agreed.** `Object::crc` hashes nine
    fields per object and never walks the behavior modules, so velocity, Locomotor internals and AI
    goal/path state are invisible. Divergence can incubate unhashed for hundreds of frames.
18. **A subagent's confidence is not evidence, and grep filters hide the evidence that disproves you.**
    Filter by PATH, and when concluding something does NOT exist, search **unbounded**.
19. **Anchor a truncation assertion at the end of the string.** Asserting the relay's 100-char chat cut
    with `/A+/` over the whole line matched the `A` in the `GXSAY` **tag** and reported a correct
    100-character message as one character. It failed while the relay was right.
20. **Preserve every stream a multi-peer run writes.** `relay-8peer` and `relay-7peer-ai-1500` quote
    "28/28 pairs" and "21/21 pairs" that nobody can recompute, because only peer0 was kept. And the
    75,000-frame soak's raw `.crc` files are not in git at all.
21. **Never write into the Steam folder.** Baseline manifest
    `9793E5EE7FCEDAF250C7403B6D7D01C0426F993B260EB233E49B079A27134033` — **UNVERIFIED**, carried
    forward from `STATE_2026-07-27_step4.md` / `STATE_2026-07-28_session2.md`. No script in the tree
    recomputes it, and it was not re-measured this session.
22. **An "identical" networked run on a map other than `alpine assault` may be a frozen world, not a
    passing one.** See item 1. Report the map next to the distinct count, not just the frame count.
