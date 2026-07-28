# Kickoff prompt for the next session

Paste everything below the line into a fresh session.

---

Read these first, in this order:

- `~/GeneralsX-src/docs/WORKDIR/STATE_2026-07-28.md` — what happened overnight and what is fixed
- `~/GeneralsX-src/docs/WORKDIR/SCOPE_crossplay.md` — the goal and the six blockers (blockers 1-4 and
  6 are done; its blocker-5 text is stale, see below)
- `git -C ~/GeneralsX-src log --oneline -25` — the commit messages carry the *why*

**Goal:** my Mac, my iPad and my Windows PC — all on one WiFi — sit in one LAN lobby, start a match,
and play it to completion without desyncing.

## Where things stand

**The three-way lobby works.** Mac + iPad + Windows have sat in one LAN lobby and started a match
together. That was blocker 6 and test-ladder step 5, both previously untried.

**The cross-platform desync root cause was found and FIXED.** Apple's libm and MSVC's UCRT return
results ONE ULP apart for `cosf`/`sinf` near 45 degrees. `Thing::setOrientation` wrote those raw
results into each object's transform, and `Object::crc` hashes those 48 bytes with no epsilon — so
one bit killed the match before frame 1.

The fix gives the engine its own trig (`Core/Libraries/Source/WWVegas/WWMath/gamemath.h`) built only
from IEEE-754 basic arithmetic, which is correctly rounded and therefore identical on every
platform. `WWMath`'s `Sin/Cos/Tan/ASin/ACos/Atan/Atan2` and the `*Trig` wrappers all route to it.

**Result, measured:**

| | before | after |
|---|---|---|
| frame-0 object transforms differing | 6 of 323, then 4 | **0 of 323** |
| first CRC checkpoint | MISMATCH at 105, every time | **ok** through 205-505 |
| identical frames | ~0 | 255-539 of 433-733 |
| desync onset | frame 9 | frame 255-541, mid-match |

## THE remaining task: the mid-match divergence

Both peers now start **bit-identical** and drift apart during play. This is an ordinary lockstep bug,
not a cross-platform maths problem.

Observed first differing frame: **255** in one run, **377** in another. It moves, so do not hardcode
a window.

**The method that worked, reuse it:**

1. Run a match. `[GXCRC] f=N v=XXXXXXXX` is printed every frame on both peers; pull both logs and
   diff to find the first differing frame.
2. Re-run with `GX_TRACE_LO` / `GX_TRACE_HI` set around that frame on BOTH peers. That makes
   `[GXOBJ] f= id= tmpl= running=` print the running CRC after every object, so the diff names the
   exact object that goes first — this is what found the tree.
3. `[GXMTX]` dumps the raw transform bits on the first traced frame. A `bitdelta=1` means a rounding
   difference; anything larger means a different code path or value.

Ruled out already, do not re-derive:
* **libm** — `[MATHCRC] 97B538BF` identical on both peers with 14 real libm calls executing.
* **Start positions** — both peers print identical RNG state either side of the pick
  (`[GXPOS] slot=0 numPlayers=4 seedCRC=782092363` then `posIdx=2 seedCRC=2348106595`).
* **Load-time state** — frame 0 hashes 0 of 323 objects differing.

One unexplained detail worth keeping: in one run, frames 377 and 461 differed for exactly ONE frame
each and then RE-CONVERGED, before splitting permanently at 541. A rounding difference does not heal
itself, so that pattern may have a separate cause — possibly ordering or timing.

## The three machines

- **Mac mini M4** — repo `~/GeneralsX-src`, game data `~/GeneralsX/GeneralsZH/`, LAN `192.168.10.51`.
  Build `./scripts/build/macos/build-macos-zh.sh --build-only`, deploy
  `./scripts/build/macos/deploy-macos-zh.sh`, run `cd ~/GeneralsX/GeneralsZH && ./run.sh -win -xres 1600 -yres 900`.
  Drive with `./scripts/build/macos/drive-macos-zh.sh` (cliclick; queries the window position at
  runtime because it moves between launches).
- **Windows 11 `r0se-desktop`** — `ssh User@192.168.10.89`, PowerShell (use `;` not `&&`), elevated.
  Clone `C:\dev\GeneralsX`, build `cmd /c C:\dev\cb.bat win64 x64`, stage with
  `scripts/build/windows/setup-run-win64.ps1`, run folder `C:\dev\GeneralsX-run\`.
  Drive with `scripts/build/windows/drive-run-win64.ps1` — read its header, it documents five
  failure modes that each cost an hour.
- **iPad Air 11-inch (M3)** — full self-contained build installed (`com.karlhaykal.generalszh`).
  Package with `./scripts/build/ios/package-ios-zh.sh --install`. **Install over USB, not WiFi** —
  a 2.8 GB install over `transportType: localNetwork` fails with "Connection interrupted" every time.

## Rules I care about

1. Do not start new investigations while a big task is unfinished.
2. **Verify before claiming.** Gate on exit codes, never grep counts.
3. When something behaves unexpectedly, invoke `systematic-debugging` before the second theory.
4. Keep work in background workflows.
5. Never let the Mac or iOS build regress. Prove it with the build script's exit code.
6. Count game processes. `pkill -f GeneralsXZH` then `pgrep -f GeneralsXZH | wc -l`. macOS `pgrep`
   has no `-c`.
7. Never write into the Steam folder. `scripts/build/windows/steam-manifest.ps1` before and after;
   baseline `9793E5EE7FCEDAF250C7403B6D7D01C0426F993B260EB233E49B079A27134033`.

## Mistakes made overnight — do not repeat these

* **ALWAYS COMMIT BEFORE BUILDING THE BINARIES THAT WILL FACE EACH OTHER.** I built the Mac from a
  dirty tree TWICE, giving it a different `sourceID` than Windows, which refuses the join. The
  digest hashes `git ls-tree` at HEAD **plus a dirty-file overlay**. Check `source=` matches on both
  peers BEFORE asking anyone to play.
* **Check whether the game is already running before diagnosing a launch failure.** The engine takes
  a named mutex shared with retail Generals and Generals Online, so the second instance exits 1 with
  no crash, no message, nothing past the command-line parse. I read that as a crash in new code and
  reverted working changes chasing it.
* **`RTS_DEBUG_MULTI_INSTANCE` BREAKS LAN DISCOVERY.** It looks like it only renames the mutex, and
  the narrow socket bind it enables really is `#ifndef _WIN32` — but `IPEnumeration.cpp:118-126`
  also injects a **loopback** address per instance, and 127.0.0.1 sorts below 192.168.x.x so it
  becomes the head of the list and the peer advertises itself on loopback. If you want it, rank
  loopback last in `ipSortsBefore` first.
* **Do not mask a desync in the CRC.** Quantising the transform before hashing makes the warning go
  away while the peers keep simulating different worlds — a detected desync becomes an undetected
  one.
* **Do not enable `DEBUG_CRC` to investigate.** The `TheModuleFactory` block at `GameLogic.cpp` is
  inside `#ifdef DEBUG_CRC`, so such a build hashes different bytes and desyncs by construction.
* Four confident theories died to measurement overnight: "SimID will refuse the join", "it's the
  train", "libm is exonerated", "start positions are swapped". Measure before asserting.

## Other traps

* **`DEBUG_LOG` is `((void)0)` in every shipping preset.** Use `fprintf(stderr, ...)`. Existing
  traces: `[SIMID] [LAN] [CRC] [GXCRC] [GXOBJ] [GXMTX] [GXPOS] [GXTRACE] [ARCHIVES] [INI]`.
* **Never capture the game's stderr through a pipe** — a PowerShell scriptblock cast to `[Action]`
  never runs, nothing drains the pipe, and the child blocks forever on its next write. Redirect to a
  file with the shell.
* **The game window moves between launches** on both platforms. Use client-relative coordinates.
* **Move, dwell ~900 ms, THEN press.** The shell resolves the hovered control once per frame, so a
  move bundled with the press hits the previously hovered control.
* **Force the game foreground before clicking** and abort if it fails — `SetForegroundWindow` from a
  background process is silently ignored, and clicks land in whatever is on top.
* **Windows desktop is 3640x1920 with `VirtualScreen Y=-475`**; screenshot pixel = screen coord +
  475. Queried over SSH from session 0 it lies and says 1024x768.
* `DXVK_LOG_LEVEL=none` on Windows too, or DXVK writes one stderr line per frame.

## Machine state not in git

* **Windows Firewall rule, required:** `GeneralsX LAN test (Claude, removable)` — inbound UDP
  8086+8088, program-scoped, RemoteAddress 192.168.10.0/24. Without it the Mac's announcements are
  dropped silently.
* **Windows `Options.ini` `IPAddress`** was `10.5.0.2` (NordVPN) and is now `192.168.10.89`. Backup
  at `Options.ini.bak-claude`. **Check this first if discovery ever breaks.**
* **`C:\dev\GeneralsX-run\Data\Cursors\`** — 52 `.ANI` files copied from the Steam install so the
  Windows build renders a cursor. This is a WORKAROUND; the real bug is `Win32Mouse.cpp:376` using a
  relative `data\cursors\...` path that ignores `CNC_GENERALS_ZH_PATH`.

## Also open

* Windows cursor source fix (see above).
* The JOIN FAILED modal clips its own text — the message is correct and useful, the window is too
  small.
* Windows boots silent, no movies — audio/video stubbed by construction at x64.
* `-autoload` crash, undiagnosed.
