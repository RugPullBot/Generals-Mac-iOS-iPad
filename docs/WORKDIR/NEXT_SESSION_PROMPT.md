# Kickoff prompt for the next session

Paste everything below the line into a fresh session.

---

Read these first, in this order, before doing anything else:

- `~/GeneralsX-src/docs/WORKDIR/STATE_2026-07-27_step4.md` — what happened last session and the
  measurement that matters. Start here.
- `~/GeneralsX-src/docs/WORKDIR/SCOPE_crossplay.md` — the goal and the six blockers. Blockers 1-4
  are done; ignore its "blocker 5 is unmeasured" line, that is now stale.
- `git -C ~/GeneralsX-src log --oneline -12` — the commit messages carry the *why*.

**Goal:** my Mac, my iPad and my Windows PC — all on the same WiFi — sit in one LAN lobby, start a
match, and play it to completion without desyncing. Nothing short of that counts.

## Where things actually stand

**Test ladder step 4 is DONE, and it gave a negative result.** Mac and Windows discovered each
other, the join was accepted, the match started, both players built a unit and a structure — and
then they desynced. Measured identically on both peers:

```
[CRC] MISMATCH frame=105 crcs=2 players=2
[CRC]   player=2 crc=FF47FF63
[CRC]   player=3 crc=471A6F1A
[CRC] DESYNC CONFIRMED - divergence frame~=100 noticedAtFrame=105 runAhead=4
```

`crcs=2 players=2` means **both peers reported** — this is a real state divergence, not a lost
message. Blocker 5 is no longer the unknown; it is the problem.

Everything else is verified in place: both peers build `source=47E7D750` from the same clean commit,
40 archives each, `assetID 8E504171` on both, and SimID accepts the join with only the tier-2
platform warning.

## THE task: blocker 5 — Mac↔Windows float determinism

**Do this first, before bisecting anything:** de-fold `SimulationMathCrc` (make its inputs
`volatile` — 10 of its 17 libm call sites are constant-folded at -O2, so today it measures the
compiler rather than libm) and compare the value across the two peers. That single measurement
separates "Apple libm and MSVC UCRT disagree" from "something else diverged", and it is much cheaper
than hunting frames.

Two hard constraints when you try to get finer resolution:

* **The CRC interval cannot go below 100 in a shipping build.** `NET_CRC_INTERVAL` is 1 under
  DEBUG_CRC and 100 otherwise (`Core/.../Network.cpp:57-60`), `GameInfo::setCRCInterval` clamps to
  <=100 (`GameInfo.h:219`), and `-NetCRCInterval`'s parse body *and* table entry are both inside
  `#ifdef DEBUG_CRC`. So frame 105 is just the first time anyone looked — the divergence may be at
  frame 1.
* **You cannot enable DEBUG_CRC on one side to see more.** The `TheModuleFactory` block at
  `GameLogic.cpp:4293-4305` is inside `#ifdef DEBUG_CRC`, so such a build hashes different bytes and
  mismatches a release peer *by construction*. Any added resolution must be release-safe — lower the
  clamp, or fprintf-only output that never touches the xfer stream.

Also: `GameLogic::getCRC` calls `setFPMode()` (`GameLogic.cpp:4222`), and
`docs/architecture/GAME_LOGIC.md:785` says arm64 cannot reproduce the x87 `_PC_24` baseline. Expect
to locate the divergence, not to fix it with an FPU-mode tweak.

## The three machines

- **Mac mini M4** — you are on it. Repo `~/GeneralsX-src`, game data `~/GeneralsX/GeneralsZH/`,
  LAN IP `192.168.10.51` (en0). Build: `./scripts/build/macos/build-macos-zh.sh --build-only`
  then `./scripts/build/macos/deploy-macos-zh.sh`. Launch: `cd ~/GeneralsX/GeneralsZH && ./run.sh -win -xres 1600 -yres 900`.
  Drive it with **`cliclick`** (installed, `/opt/homebrew/bin/cliclick`) plus `screencapture -x`.
  Window lands at (61,30) size 1600x932, so **client origin is (61,62)**; menu buttons are at client
  x=1287, y=200/260/320/... Raise it with
  `osascript -e 'tell application "System Events" to set frontmost of (first process whose name is "GeneralsXZH") to true'`.
- **Windows 11 `r0se-desktop`** — `ssh User@192.168.10.89`, key auth, PowerShell default (use `;`
  not `&&`), SSH session is elevated. Clone `C:\dev\GeneralsX`, build `cmd /c C:\dev\cb.bat win64 x64`.
  Run folder `C:\dev\GeneralsX-run\`. **Use `scripts/build/windows/drive-run-win64.ps1`** — read its
  header, it documents four failure modes that each cost an hour.
- **iPad Air 11-inch (M3)** — currently on an OLD build and therefore **correctly refused** at the
  join ("their build is too old to report a build identity"). Rebuild and redeploy it before step 5.
  Needs Karl's hands; a physical iOS touchscreen cannot be driven synthetically.

## Rules I care about

1. **Do not start new investigations while a big task is unfinished.** Finish, then move.
2. **Verify before claiming.** Invoke `verification-before-completion` before saying anything is
   done — including anything a subagent reports. Gate on **exit codes**, never on grep counts.
3. **When something behaves unexpectedly, invoke `systematic-debugging` before your second theory.**
   It paid for itself twice again last session.
4. **Keep work in background workflows** so it continues between turns.
5. **Never let the Mac or iOS build regress.** Prove it with the build script's exit code.
6. **Count the game processes you launch.** `pkill -f GeneralsXZH`, then confirm with
   `pgrep -f GeneralsXZH | wc -l`. macOS `pgrep` has **no `-c` flag**.
7. **Never write into the Steam folder on Windows.** Run `scripts/build/windows/steam-manifest.ps1`
   before and after; baseline `MANIFEST-SHA256 9793E5EE7FCEDAF250C7403B6D7D01C0426F993B260EB233E49B079A27134033`.

## Machine state that is NOT in git

* **Windows Firewall rule is active and required:** `GeneralsX LAN test (Claude, removable)` —
  inbound UDP 8086+8088, program-scoped to `C:\dev\GeneralsX-run\generalszh.exe`, RemoteAddress
  192.168.10.0/24. Without it the Mac's announcements are dropped silently
  (`DefaultInboundAction=Block` on all three profiles). Remove with
  `Remove-NetFirewallRule -DisplayName "GeneralsX LAN test (Claude, removable)"`.
* **Windows `Options.ini` `IPAddress` was `10.5.0.2`** — the NordVPN NordLynx address, which is a
  live interface, so the game matched it and announced itself somewhere the Mac cannot reach. Now
  `192.168.10.89`; original at `Options.ini.bak-claude` under
  `C:\Users\User\Documents\Command and Conquer Generals Zero Hour Data\`. **Check this first if
  discovery ever breaks again.**

## Traps that cost real time — do not rediscover these

* **`DEBUG_LOG` is `((void)0)` in every shipping preset.** Anything you need to see in a real run
  must be `fprintf(stderr, ...)`. `[SIMID]`, `[LAN]`, `[CRC]`, `[ARCHIVES]`, `[INI]` already exist.
* **Never capture the game's stderr through a pipe.** `ProcessStartInfo` + `RedirectStandardError`
  plus a `Task.Run` drain HANGS THE GAME: a PowerShell scriptblock cast to `[Action]` runs on a
  thread-pool thread with no runspace, so the body never executes, nothing reads the pipe, its buffer
  fills, and the child blocks on its next stderr write — a live process at ~80 MB, flat CPU, titled
  "(Not Responding)". Redirect to a file with the shell.
* **The Windows game window does not land in the same place twice** — three launches, three
  positions. Use client-relative coordinates via `ClientToScreen`.
* **A move and a press in one `SendInput` batch clicks the WRONG control.** The shell resolves the
  hovered control once per frame, so pressing in the same batch hits the *previously* hovered one.
  Clicking NETWORK's coordinate that way opened ONLINE. Move, dwell ~900 ms, then press separately.
  A human click works because it is always a move followed many frames later by a press.
* **`SendInput` silently returns 0** if the INPUT struct size is wrong. Do not hand-pad it.
* **Windows desktop is 3640x1920 across two monitors with `VirtualScreen Y=-475`** (DISPLAY2 is a
  portrait 1080x1920 at y=-475), so **screenshot pixel = screen coord + 475**. Queried over SSH from
  session 0 it lies and reports 1024x768 — never compute coordinates from an SSH-side query.
* **A GUI/Vulkan app must run in the INTERACTIVE session** via a transient `schtasks /it` task;
  clicking and screenshotting must both happen inside that session.
* **`DXVK_LOG_LEVEL=none` on Windows too** — otherwise DXVK writes one stderr line per frame
  (17,253 lines in a run that never left the main menu) and buries the traces.
* **The SimID source digest hashes `git ls-tree` at HEAD PLUS a dirty-file overlay**, but only over
  simulation paths (`resources/gitinfo/simsourcedigest_watcher.cmake:66-105`) — docs commits are
  free, a dirty tree is not. **Compare only builds made at the same clean commit.**
* **Grepping build logs for `ERROR` matches `WW3D_ERROR_OK`**, a success line. Use `\bERROR\b`.
* **`grep -c` exits 0 when it finds matches.** Gate on the build's own exit code.
* **The Windows clone must be a pure mirror**: `git fetch && git reset --hard origin/main && git clean -fd`.

## Known issues, not blockers

* **Windows renders no in-game cursor** — hand-driving the PC is near impossible, so use the
  synthetic driver. `UseAlternateMouse` is NOT the fix; it is the alternate *control scheme*.
* Windows boots silent with no movies — audio/video stubbed by construction at x64.
* Windowed pillarbox reproduces on Windows too. Workaround: fullscreen, or `-win -xres 1600 -yres 900`.
* `-autoload` crash still undiagnosed.
* Keybind remapping UI — deferred, wants its own design pass after cross-play works.
