# Project state — 2026-07-28 (session 3, overnight)

Branch: `main`. Everything below is committed and pushed. If this session is gone, start here.

## Headline: the cross-platform desync is FIXED and PROVEN

**Mac (arm64 macOS) + iPad (arm64 iOS) + Windows (x86_64), one LAN match, 74,705 frames, ZERO
differing.** Later the same night a 2-peer run reached **72,669 frames / 40m22s, also zero**. That
is the goal of this whole workstream, met and measured.

Cause was 69 libm calls the earlier `gamemath.h` work never reached, because `gamemath.h` had
**exactly one include site in the whole tree** (`wwmath.h:40`) and there is no `#define`
redirecting any libm name:

* 40 raw `sinf`/`cosf` in `matrix3d.h`, `matrix3.h`, `vector3.h` — the `Rotate_X/Y/Z`,
  `Pre_Rotate_*`, `In_Place_Pre_Rotate_*` and `Set(axis,angle)` helpers, reached every frame from
  `PhysicsBehavior::doPhysics` (`PhysicsUpdate.cpp:750-752`) then `setTransformMatrix` (`:817`).
* 29 raw `atan2` in GameLogic — Locomotor (8), Weapon (4), ObjectCreationList (3), and singles
  elsewhere.

Verification method that works, reuse it: capture stderr on both peers, `grep '^\[GXCRC\]'`, diff
frame by frame. Check the count of DISTINCT CRC values as a control — 64,833 distinct out of 65,223
proves the sim was actually active rather than idling.

## THE OPEN BUG: any match with an AI desyncs at frame 0

Fully reproducible, 3 of 3:

| setup | frames | outcome |
|---|---|---|
| 3 humans, no AI | 74,705 | clean |
| 2 humans, no AI | 6,017 | clean |
| 1v1v1 + one **Hard AI** | 234 | **diverges at frame 0** |
| 8 players with AI | 233 | **diverges at frame 0** |
| 8 players with AI | 235 | **diverges at frame 0** |

The ~234-frame length is just the 100-frame CRC checkpoint noticing and halting. The divergence is
at frame 0 — the peers build **different starting worlds** before simulating anything.

Hard evidence from the frame-0 `[GXOBJ]` trace (emitted for frames 0-10 by default):

* Mac created **452** starting objects, Windows **446**.
* First object already differs: mac `id=226 ChinaCommandCenter` vs win `id=223 ChinaVehicleDozer`.
  IDs offset by 3, so one peer allocated more objects earlier.

**Ruled out by measurement — do not re-derive:**

* **Not builds or data.** SimID identical on both: `engine=4D31F2F2 source=0384B81D data=FEAAE3F3
  ordinal=9F43F7B5`. Both load an identical set of 40 BIG archives.
* **Not start positions.** `[GXPOS]` traces match exactly on both peers, before and after the pick.
* **Not random armies.** The clean 74,705-frame 3-way run was itself on Random armies.
* **Not `AISkirmishPlayer::adjustBuildList`'s raw sin/cos.** This looked perfect — AI-only, at map
  load before frame 1, raw libm at an unconditional 135 degrees. A probe compiled on both toolchains
  with the game's own FP flags shows mac and win agree BIT FOR BIT at every angle that site can
  produce. Probe and both outputs are committed at `docs/WORKDIR/evidence/trigconf*`. Re-run it
  rather than re-arguing it.

**Next step:** cross-platform replay playback (below) makes this reproducible offline. The fixture
already exists: `~/Library/Application Support/GeneralsX/GeneralsZH/Replays/00000000.rep`, map
`twilight flame`, slot list `HMegumis-Mac-...:CH,...` — one human, one **Computer Hard**. Replay it
headless on Mac and Windows, diff frame 0, and `[GXOBJ]` names the object.

## The harness: headless replay

```bash
cd ~/GeneralsX/GeneralsZH
./run.sh -headless -replay 00000000.rep 2> mac.err                    # same-build
GX_REPLAY_XPLAT=1 ./run.sh -headless -replay 00000000.rep 2> mac.err  # cross-build, runs to the end
grep '^\[GXCRC\]' mac.err > mac.crc
```

Runs the real `GameLogic`, no window, no input, no network. Measured **2,037 logic frames/sec on
M4 = 67.9x realtime**. Verified deterministic: 3 byte-identical runs.

`GX_REPLAY_XPLAT=1` also stops playback halting at the first CRC mismatch, which matters because
changing sim math invalidates a replay's embedded CRCs.

**Blocker, cause found, fix not yet written:** a Mac-recorded `.rep` crashes Windows with
`0xC0000005` before frame 0. `Recorder` reads and writes strings with `readWideChar()` /
`writeChar(const WideChar*)`, which move `sizeof(WideChar)` bytes — and `WideChar` is `wchar_t`,
**4 bytes on macOS/clang, 2 on Windows/MSVC**. Confirmed in the file itself: the Mac replay contains
`4c00 0000 6100 0000 7300 0000 7400 0000` = `L a s t` at four bytes each. Windows reads those as
2-byte chars, desynchronises mid-header and runs into garbage.

Blast radius is small — `readWideChar`/`writeChar(WideChar*)` are called in **exactly one file**,
`Recorder.cpp`, in each of GeneralsMD and Generals. Fix is a fixed 2-byte encoding, which also makes
replays match the retail Windows format. Existing Mac `.rep` files will stop loading; they are
already useless cross-platform.

This is also why CI keeps separate `linux_*.rep` and `macos_*.rep` sets rather than sharing one.

## Other fixes landed tonight

* **Terrain draw window ignored camera height** (`W3DView::updateTerrain`). Sized from camera PITCH
  only, and zoom scales the camera position uniformly so pitch is invariant under zoom — the window
  stayed ~1350 world units at every zoom. Past the design height it stops covering the view and the
  rest renders black, with objects still drawn because the heightmap is `Set_Force_Visible(TRUE)`
  and bounded solely by the draw size while props are frustum-culled with whole-map bounds. Fixed
  with a camera-height term snapped to the 1+32k tiling. Client-only, outside the digest.
  **NOT visually verified yet** — needs someone to zoom out and look.
* **Options "Debug" button removed.** Created at hardcoded (320,528), which at 1600x900 landed on
  the Scroll Speed slider and ate its clicks. The floating debug overlay is untouched and is still
  the way into the Debug screen.
* **Headless CRC-mismatch crash.** `TheInGameUI->message()` on mismatch killed headless runs on
  Windows. Guarded on `m_headless`.
* **22 raw `sin`/`cos`/`tan` in both games' GameLogic** routed through the deterministic
  `Sin`/`Cos`/`Tan` gateway (`Lib/trig.h` -> `Trig.cpp` -> `WWMath::SinTrig` -> GameMath). Kept as
  hygiene; measured behaviour-neutral at every angle tested. **Not** the AI fix.

## Known-open, root-caused, not fixed

* **AWOL players block victory.** `hasSinglePlayerBeenDefeated` (`VictoryConditions.cpp:328`) is
  purely asset-based and never consults connection state, so a player who uses **Exit to lobby**
  leaves their base standing and nobody can ever win. **Surrender** works because
  `surrenderQuitMenu` sends `MSG_SELF_DESTRUCT` (`QuitMenu.cpp:164`); `exitQuitMenu` does not.
  CAUTION: `VictoryConditions::update` calls `p->killPlayer()`, so it mutates simulation state.
  Any fix must use state all peers agree on, or it will desync. Do not make victory depend on
  locally-observed disconnect timing without first proving disconnect is frame-synchronised.
* Defeated observers are never pulled to the score screen.
* `%` unescaped in the SagePatch.ini generator (`GameEngine.cpp:518`) — writes `~5more` instead of
  `~5% more`, and is UB (reads a vararg that was never passed).

## Machine state

* **Mac** — `sleep 0`, `disksleep 0`, plus `caffeinate -dimsu` running. Monitor may be off.
* **Windows** `192.168.10.89` — `standby-timeout-ac 0`, session 1 active, monitor may be off.
  Launch the game with `schtasks /it` via `C:\dev\gxrun_session1.ps1`; **straight from SSH you land
  in session 0 where DXVK enumerates zero adapters and dies at 0xC0000005**.
* **iPad** — UNPLUGGED, and on an older build. `sourceID` has moved since, so it cannot join until
  replugged and reinstalled with `./scripts/build/ios/package-ios-zh.sh --install`.
* **VPS** — `163.5.210.131`, root, provisioning at time of writing. `79.110.49.24` in the panel is
  the hypervisor NODE, not the customer VM; do not log into it. The root password was pasted in
  chat and should be rotated.
* Steam folder untouched: manifest `9793E5EE7FCEDAF250C7403B6D7D01C0426F993B260EB233E49B079A27134033`.

## VPS sizing, measured

Single-threaded sim. **Single-core clock is the only CPU metric that matters**; core count only buys
concurrent matches.

```
M4:  2,037 logic frames/sec @ 432 objects = 67.9x realtime = 1.5% of one core
     scaled to 6,118 objects  ~5x realtime ~20% of one core
     a server x86 core is ~0.5-0.7x an M4 core -> budget 30-40% of one core per heavy match
```

Relay-only needs the cheapest box that exists (lockstep sends commands only, a few KB/s per player).
An authoritative simulating server wants 2-4 **dedicated** (never burstable) cores, 4-8 GB, 25 GB
disk, Linux. Ubuntu 24.04 — that is what `build-linux.yml` and `replay-tests.yml` use.

## Lessons worth keeping

* **Grep filters can hide the evidence that disproves you.** I claimed `TerrainDrawDistanceScale`
  was dead code based on a grep that excluded every line containing `GlobalData` — and the actual
  usage line is `TheGlobalData->m_terrainDrawDistanceScale`. It is live, applied at
  `W3DView.cpp:3753`. Filter by PATH, not by line content.
* **A subagent's confidence is not evidence.** The determinism sweep and its adversarial verifier
  both rated the AISkirmishPlayer trig finding high-confidence. Both were right that the code was
  raw libm and reachable. Neither checked the only thing that mattered — whether the two platforms
  actually differ there. They do not. Compile the probe.
* **"The CRC agreed until frame N" does not mean the simulations agreed.** `Object::crc` hashes nine
  fields per object and never walks the behavior modules, so velocity, Locomotor internals and AI
  goal/path state are all invisible to it. Divergence can incubate unhashed for hundreds of frames.
* **Commit before building the binaries that will face each other**, or `sourceID` differs and the
  join is refused. Cost a rebuild tonight.
