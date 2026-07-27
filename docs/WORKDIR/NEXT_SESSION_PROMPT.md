# Kickoff prompt for the next session

Paste everything below the line into a fresh session.

---

Read these three files first, in this order, before doing anything else:

- `~/GeneralsX-src/docs/WORKDIR/SCOPE_crossplay.md` — the goal, the six blockers, the test ladder
- `~/GeneralsX-src/docs/WORKDIR/STATE_2026-07-27.md` — where the build stands and how it got here
- `git -C ~/GeneralsX-src log --oneline -20` — the commit messages carry the *why*, not just the what

**Goal:** my Mac, my iPad and my Windows PC — all on the same WiFi — sit in one LAN lobby, start a
match, and play it to completion without desyncing. Nothing short of that counts.

**All three machines are mine and reachable right now:**

- Mac mini M4 — you are on it. Repo `~/GeneralsX-src`, game data `~/GeneralsX/GeneralsZH/`
- iPad Air 11-inch (M3) — paired, `available` per `xcrun devicectl list devices`. Needs my hands:
  there is no way to drive a physical iOS device's touchscreen synthetically.
- Windows 11 `r0se-desktop` — `ssh User@192.168.10.89`, key auth, PowerShell is the default shell
  (use `;` not `&&`). Clone at `C:\dev\GeneralsX`, build `cmd /c C:\dev\cb.bat win64 x64`

## Rules I care about

1. **Do not start new investigations while a big task is unfinished.** Finish, then move.
2. **Verify before claiming.** Invoke `verification-before-completion` before saying anything is
   done — including anything a subagent reports. Gate on **exit codes**, never on grep counts.
3. **When something behaves unexpectedly, invoke `systematic-debugging` before your second theory.**
   Two wrong theories in a row means the tooling is lying. This paid for itself twice last session.
4. **Keep work in background workflows** so it continues between turns.
5. **Never let the Mac or iOS build regress.** Prove it with the build script's exit code.
6. **Count the game processes you launch.** `pkill -f GeneralsXZH`, then confirm with
   `pgrep -f GeneralsXZH | wc -l`. NOTE: macOS `pgrep` has **no `-c` flag** — the form in
   `~/.claude/CLAUDE.md` errors out and the usage message reads like "no processes".
7. **Never write into the Steam folder on Windows.** Generals Online + EAC live there.

## What landed last session (12 commits, `ef5fe51c6..011afed38`, all pushed)

- **Blocker 1 (Windows links) — re-verified independently**, not taken on report. Pure-mirror sync,
  `--target clean`, CONFIGURE 0 / BUILD 0, 0 errors, 1909 steps, both exes `8664 machine (x64)`.
- **Blocker 2 (Windows RUNS) — DONE.** Boots through all 42 subsystems and **plays a skirmish**
  (match clock `00:00:28.22`, 30 FPS, RTX 5090). Test ladder step 3 complete.
- **Blocker 4's open `Data/INI/INIZH.big` question — RESOLVED.** It is a retail SKU duplicate the
  engine already skips by name (`StdBIGFileSystem.cpp:663-671`, `Win32BIGFileSystem.cpp:661`).
  Nothing to copy, nothing to delete. The old doc claim that it "IS loaded" was wrong.
- **SimID instrumentation landed and proven at runtime** — it was computed and never surfaced.
- **Blocker 3 — MEASURED, and the premise did not hold.** See below.
- Four real bugs fixed: ccache `OFF` being a no-op, the asset-path trailing separator, the INI list
  ordering, and a Windows double-mount of the base-Generals archives.

## Where to pick up: TEST LADDER STEP 4 — Mac ↔ Windows LAN match

This is the next thing and it is the *only* remaining unknown that matters. Everything it depends on
is verified in place:

```
Mac      engine=4D31F2F2 source=F2CD5A84 data=FEAAE3F3 ordinal=9F43F7B5 asset=8E504171 platform=BF013216
Windows  engine=4D31F2F2 source=F2CD5A84 data=FEAAE3F3 ordinal=9F43F7B5 asset=8E504171 platform=7480F925
```

* All four **tier-1 deny** fields match → `SimIdCompare` returns `SIMID_OK` → **the join will be
  accepted, not refused.** Only `platformID` differs and that is tier-2 warn by design.
* `sizeof(LANMessage)=471` and `sizeof(SimIdWire)=36` on both → no wire-packing divergence between
  Apple clang arm64 and MSVC x64.
* Archive sets are now identical (40 mounted each), so **a desync can no longer be blamed on
  mismatched data — it points squarely at floating point, which is blocker 5.**

Host on one side, join from the other, play to a CRC interval, then to completion. The Windows side
can be driven headlessly with the harness described below; the Mac side needs equivalent driving.

## Open issues, in the order I would take them

1. **Blocker 5 — Mac↔Windows float determinism. THE remaining unknown.** Unmeasured. If step 4
   desyncs, this is why. The scope doc's plan: de-fold `SimulationMathCrc` (make its inputs
   `volatile`; 10 of its 17 libm call sites are constant-folded at -O2, so today it measures the
   compiler rather than libm) and compare across peers.
2. **Blocker 6 — three-way lobby.** Never tried, and the lobby keys on IP. **Not implied by two-peer
   success.** Needs the iPad physically present. Note Mac↔iPad is *bit-identical*
   (`SimulationMathCrc` `__text` byte-identical, 15/15 libm probes match), so the iPad is a clone of
   the Mac for determinism purposes — if Mac↔Windows holds, iPad↔Windows follows for free. The iPad
   is needed for the **lobby**, not for the math.
3. **Windows boots silent, no movies.** Audio/video are stubbed by construction (`SAGE_USE_OPENAL=OFF`
   at x64; Miles/Bink are 32-bit-only). Needs the OpenAL + FFmpeg path the Apple builds use. Quality
   gap, not a cross-play blocker — but it is the first thing anyone will notice playing on the PC.
4. **Render FPS / vsync (task #6).** The render-side unlock **already exists**: `changeMaxRenderFps`
   (`CommandXlat.cpp:186`) cycles through presets including `RenderFpsPreset::UncappedFpsValue`,
   bound to `MSG_META_*_MAX_RENDER_FPS` and mirrored in the Debug screen. The displayed `30` is just
   the `FramesPerSecondLimit` default from `GameData.ini`. What is missing is **refresh-rate / vsync
   matching**. **HARD CONSTRAINT: `LOGICFRAMES_PER_SECOND = WWSyncPerSecond = 30`
   (`WWCommon.h:70`, `GameCommon.h:68`) must NOT change** — it is lockstep-critical and is exactly
   why Generals Online (60 Hz logic + 2× multiplier) can never cross-play. Also do **not** raise it
   by editing `GameData.ini`: that file feeds `xferCRC`, so it moves `dataID` and every peer must
   then match byte for byte.
5. **The four retail checkpoints (task #5).** I argued they do **not** need re-deriving: `dataID`
   stayed `FEAAE3F3` across the canonicalisation, so the whole byte stream — and therefore every
   prefix of it, including all four mid-stream checkpoints — is unchanged. That is an argument, not
   a direct measurement, which is why the task is still open. If you want it measured, the corrected
   line numbers are GeneralsMD `GameEngine.cpp:597` / `:694` and Generals `:522` / `:579` (the ones
   in the old scope doc were stale). Note **on CRC mismatch nothing happens at all** — no log, no
   assert, no `else` branch — so these are a dev-time canary, not a runtime gate.
6. **Windowed pillarbox** — reproduces on Windows too, so it is not Apple-specific. Workaround:
   fullscreen, or `-win -xres 1600 -yres 900`.
7. **`-autoload` crash** — still undiagnosed. Would make Windows testing much cheaper if fixed.

## Tooling built last session — use it, do not re-derive it

* `scripts/build/windows/setup-run-win64.ps1 -Launch` — stages the exe, DXVK x64 DLLs, the Miles/Bink
  **stub** DLLs and a launcher into `C:\dev\GeneralsX-run\`, then runs it in the interactive session.
* `scripts/build/windows/steam-manifest.ps1` — read-only integrity manifest of the Steam install.
  Run before and after; `MANIFEST-SHA256` must be identical. Baseline: 404 files, 3,073,628,958 B,
  `9793E5EE7FCEDAF250C7403B6D7D01C0426F993B260EB233E49B079A27134033`. ~3 s, no excuse to skip it.
* Release-visible traces now exist: `[SIMID]`, `[ARCHIVES]`, `[ARCHPATH]`, `[ASSET_ROOT]`, `[INI]`.

## Traps that cost real time — do not rediscover these

* **`DEBUG_LOG` is `((void)0)` in every shipping preset.** `DEBUG_LOGGING` needs `ALLOW_DEBUG_UTILS`
  needs `RTS_DEBUG`, and both Apple and Windows presets build with `-DNDEBUG -DRTS_RELEASE`. Any
  diagnostic you want to see in a real run must be `fprintf(stderr, ...)`.
* **A GUI/Vulkan app must run in the INTERACTIVE session.** Over SSH you land in session 0, where
  DXVK enumerates **zero** adapters and `W3DDisplay::init()` dies at `0xC0000005`. Use a transient
  `schtasks /it` task and delete it afterwards. A session-1 window is invisible from an SSH session,
  so clicking *and* screenshotting must both happen inside a script running in that session. The
  desktop there is 1920x1080; `SystemInformation.VirtualScreen` queried from session 0 misreports
  1024x768, so never compute click coordinates from an SSH-side query.
* **The SimID source digest hashes `git ls-tree` at HEAD PLUS a dirty-file overlay.** A build from a
  dirty tree gets a different `sourceID` than one from the clean commit. This looked exactly like a
  platform divergence twice. **Compare only builds made at the same clean commit** (check `rev=`).
* **Grepping build logs for `ERROR` matches `WW3D_ERROR_OK`**, which is a *success* line, and invents
  a phantom error on a clean boot. Use `\bERROR\b`.
* **`grep -c` exits 0 when it finds matches**, so `build && grep -c error && commit` commits on
  failure. Gate on the build's own exit code.
* **The Windows clone must be a pure mirror**: `git fetch && git reset --hard origin/main && git clean -fd`,
  never `pull`.
* **Fixing a silent failure can expose a second bug it was masking.** The trailing-separator fix
  turned a dead code path live, which revealed a pre-existing double-mount. Expect this pattern.

## Deferred, not in scope

Keybind remapping UI in settings. Real feature, wants its own design pass after cross-play works.
