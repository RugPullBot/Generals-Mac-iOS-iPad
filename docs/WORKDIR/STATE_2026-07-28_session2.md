# Project state — 2026-07-28 (session 2)

Branch: `fix/blocker5-residual-libm-trig` @ `6696b10cc`, pushed. Mac and Windows are both at that
commit with clean trees, so their `sourceID` matches and a LAN join will not be refused.

## Headline

The trig fix from session 1 only covered a fraction of the call sites. **69 libm calls in live
simulation code never went through GameMath.** They are now routed. Whether that closes the
mid-match divergence is NOT yet proven — that needs one cross-platform CRC comparison.

## What was actually wrong

Session 1 added `gamemath.h` and routed `WWMath::Sin/Cos` and the `*Trig` wrappers through it. That
covered `Thing::setOrientation`, which is why frame 0 went from 6 of 323 objects differing to 0.
It covered nothing else.

Verified by unbounded search, not assumed:

* `gamemath.h` had **exactly one include site in the whole tree** — `wwmath.h:40`.
* There is **no `#define`** redirecting `cosf`, `sinf` or `atan2` anywhere.

So these all still called Apple libm on arm64 and the MSVC UCRT on x64 — the two implementations
already measured 1 ULP apart near 45 degrees:

| where | count | what |
|---|---|---|
| `matrix3d.h` | 20 | `Rotate_X/Y/Z`, `Pre_Rotate_*`, `In_Place_Pre_Rotate_*`, `Set(axis,angle)` |
| `matrix3.h` | 14 | the same rotation helpers |
| `vector3.h` | 6 | `Rotate_X/Y/Z` |
| GameLogic `.cpp` | 29 | raw `atan2` — Locomotor (8), Weapon (4), ObjectCreationList (3), AIStates (2), DumbProjectileBehavior (2), JetAIUpdate (2), Geometry (2), + 6 singles |

None of it is rendering. `PhysicsBehavior::doPhysics` calls `Rotate_X/Y/Z` on the object transform
(`PhysicsUpdate.cpp:750-752`) then `setTransformMatrix` (`:817`) for every object with a nonzero
pitch/roll/yaw rate — tumbling debris, dying units, banking aircraft, projectiles — and the result is
the 48 bytes `Object::crc` hashes with no epsilon.

Left alone on purpose: `GameClient/Drawable.cpp:1613` (rendering, cannot reach the CRC) and the
`atan2` inside a comment at `TerrainLogic.cpp:1471`.

**Evidence these are live and not theoretical:** replaying the same `.rep` with the change produces
different simulation state from frame 255 onward.

## The reasoning error to not repeat

I opened by arguing "a diverged simulation cannot re-converge, so frames 377 and 461 healing means
something transient is being hashed." **That premise is false.** `Object::crc`
(`Object.cpp:3995-4098`) hashes nine fields per object and **never walks the behavior modules**. So
`PhysicsBehavior::m_vel`, every Locomotor internal, and the AI goal/path/turret state are all
invisible to it. A divergence incubates in unhashed state for hundreds of frames and only surfaces
when it leaks into one of those nine fields.

Frames 0-376 matching never meant the simulations matched. Any future reasoning from "the CRC agreed
until frame N" has to account for this.

## The iteration loop is fixed — this is the big process win

Reproducing this needed two people on two machines playing simultaneously, about an hour per attempt.
It does not. The engine already had `-headless -replay`, and a `.rep` holds the map, seed, slot list
and **both** players' command streams.

```bash
# same-build replay
cd ~/GeneralsX/GeneralsZH && ./run.sh -headless -replay 00000000.rep 2> mac.err
# cross-build, runs to the end instead of stopping at the first mismatch
GX_REPLAY_XPLAT=1 ./run.sh -headless -replay 00000000.rep 2> mac.err
grep '^\[GXCRC\]' mac.err > mac.crc     # then diff against the other machine
```

A 433-frame match replays in seconds, unattended. `[GXCRC]` fires every frame because
`isMPGameOrReplay` covers playback. **Verified deterministic:** three byte-identical runs before the
fix, two after.

Two fixes were needed to make it usable, both committed:

1. **Headless runs died in the CRC-mismatch path.** It calls `TheInGameUI->message()`, which walks
   the real text/font code even though `-headless` swaps in the dummy window manager. Measured on
   Windows across three replays: access violation 0xC0000005 at the first mismatch. Now guarded on
   `m_headless`.
2. **Playback stopped at the first mismatch**, discarding the shape of the divergence — which is the
   evidence. `GX_REPLAY_XPLAT=1` reports and keeps going. This also matters because changing
   simulation math invalidates a replay's embedded CRCs: without the flag the fixed build stops that
   `.rep` at frame 306 of 433; with it, all 433 frames run, exit 0, and both mismatches (304, 404)
   are reported rather than suppressed.

## Ruled out this session — do not re-derive

* **Game data mismatch.** Both peers load an **identical set of 40 archives** (basenames diffed from
  the `[ARCHPATH]` lines of a run on each machine). Not a mod/INI problem.
* **The replay version gate.** I assumed `exeCRC` was what blocked cross-platform replay. It is not:
  `readReplayHeader` (`Recorder.cpp:844`) has **no version check at all**, and `playbackFile` never
  calls `replayMatchesGameVersion`. The waiver added under `GX_REPLAY_XPLAT` is kept only because the
  replay-menu path does call it.

## Verified

| check | result |
|---|---|
| Mac build (`build-macos-zh.sh --build-only`) | exit 0 |
| iOS build (`cmake --build build/ios-vulkan --target z_generals`) | exit 0 |
| Windows build (`cb.bat win64 x64`) | exit 0 |
| Mac replay determinism | 3 runs byte-identical pre-fix, 2 post-fix |
| Raw `sinf`/`cosf` left in the 3 WWMath headers | 0 |
| Raw `atan2` left in ZH sim source | 2 (both intentional, listed above) |
| Steam folder untouched | `9793E5EE…4033`, identical before and after |
| Mac / Windows tree state | both `6696b10cc`, both clean |

## What is NOT proven

**That this fixes the desync.** It needs one cross-platform CRC comparison. Two ways:

1. **A LAN match** (~5 minutes of play). Both peers are already at the same commit with matching
   `sourceID`, so the join will work. Capture stderr on both, `grep '^\[GXCRC\]'`, diff. Previously
   the first differing frame was 255/377/541.
2. **Fully unattended**, once one blocker is cleared — see below.

## The one open blocker for the unattended loop

**Windows crashes replaying a Mac-recorded `.rep`, before frame 0** — `0xC0000005`, zero frames.
This predates my changes and my headless-UI guard did not address it, so it is a *different* crash
from the mismatch one.

What is known, measured:

* Windows runs its **own** replays headless fine (reached frames 116 and 113 before stopping at a
  legitimate CRC mismatch), so headless replay itself works on Windows.
* The Mac refuses a Windows replay **cleanly** (exit 1); Windows refuses the Mac's by **crashing**.
* Windows stops at the shell→game transition: last line is the `WinCreate default font` marker, and
  the Mac's next lines are `OSDisplaySetBusyState` then `[GXPOS]`. So it dies in map load / game
  start, before start-position selection.
* No crash dump is written (the handler does not fire in headless), and no debugger is installed on
  the box.
* Do **not** assume it is the map path encoding. The Mac replay encodes as
  `M=03maps/homeland%20alliance` and the Windows one as `M=00userdata/maps/tiny tactics zh v2`, but
  that difference is explained by the second file coming from a different/older recorder, not by a
  cross-platform bug. Unverified either way.

Next step for it: add stderr breadcrumbs through `GameEngine.cpp` game-start and the map load, or
install the Windows SDK debugging tools and get a real stack. Do not theorise a third time without
one of those.

## Also still open (unchanged from session 1)

* Windows cursor source fix (`Win32Mouse.cpp:376` uses a relative path ignoring `CNC_GENERALS_ZH_PATH`).
* JOIN FAILED modal clips its own text.
* Windows boots silent, no movies — audio/video stubbed at x64.
* `-autoload` crash. Note it loads the most recent **save**, not a skirmish, so it is not a route to
  a scripted headless match.
