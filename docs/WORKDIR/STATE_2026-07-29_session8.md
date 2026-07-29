# Session 8 dropoff — the capacity guard landed, and the activity metric no longer measures a train

Supersedes `STATE_2026-07-29_session7.md`, which remains accurate except where corrected below.
Read with `evidence/networked-sim-freeze-diagnosis.md`, `DESIGN_headless_and_relay.md` and
`tasks/todo.md` (phase 10).

**How much to trust this document.** Every number here was produced by a command run this session
and is reproducible from a committed artifact. Where something is inferred rather than measured it
says so. "Not verified" at the bottom is a list of known gaps, not a certificate for everything else.

## Position

| | |
|---|---|
| HEAD | `e9e94fabe` — **rev 2199** |
| `origin/main` | `e9e94fabe` — **everything is pushed, 0 unpushed** |
| unpushed | none |
| dirty | `M references/fbraz3-dxvk` only — untouched all session, still carries its three uncommitted DXVK edits |
| worktrees | **two, and the second is not this session's.** `.claude/worktrees/nervous-babbage-9d03d2` is on branch `claude/nervous-babbage-9d03d2` at `4d1237692`, a task 49 instrumentation commit that is NOT an ancestor of `main`. See the collision section below before touching task 49. |

**The ZH SimID digest moved four times this session and NOTHING deployed can join HEAD.**
Measured per commit against a clean worktree, not inferred — full table in
`evidence/simid-digest-by-commit.md`:

| commit | ZH | joins the peers deployed at `c72eb8d96`? |
|---|---|---|
| `c72eb8d96` … `060752a15` | `0xB30B651C` | yes — **`060752a15` is the newest commit that still joins** |
| `4cb42f38e` (diagnostics) | `0x89C01A42` | no |
| `300f43652` (task 59) | `0xF6E71C2F` | no |
| `643b48a7c` (task 61) | `0xBB5A1FF3` | no |
| `e9e94fabe` (task 49 reconcile, **HEAD**) | `0xD33D97FD` | no |

`G` never moved (`0xE827650A`) — nothing this session touched a `Generals/` path.

**Consequence, and it is the first thing to do next session:** macOS, Linux and Windows are still
deployed at `c72eb8d96`. **All three must be rebuilt at HEAD together.** They will then agree with
each other at `0xD33D97FD` and none of them will join anything still at `c72eb8d96`. Mixing gives
`SOURCE_DIFFERS` — `engineID` matches (same epoch), `sourceID` does not.

## What was done

**0. The 75,000-frame soak was rescued from a scratchpad** (`060752a15`). All 100 `.crc` streams are
now in `evidence/xplat-soak-50x1500/`. Every pair was recomputed before committing rather than
trusted: 50 of 50 `cmp`-identical, every whole-file md5 matching the committed manifest on **both**
files, every line count 1500, 75,000 frames per platform. The manifest's md5 column is a
**whole-file** md5, not a value-column one.

New `PROVENANCE.tsv` records what the manifest never did — map, capacity, `aiPlayers`, occupied
slots, pinned seed, lobby legality per iteration — extracted from the stderr logs, which are not
committed (43 MB of `[INI]` spam). Shape: 10 maps × 5 seeds, all at `rev=2171 source=6A9BFF78`,
**epoch 1**, so this corpus cannot be extended by a run at HEAD.

**That provenance re-labelled 7 of the 50 rows.** `dust devil` ×3, `bitter winter` ×2, `desert fury`
×2 ran a 3-slot lobby on a 2-start map, so the second AI was inert. They still read 1500 distinct
because slot 1 always gets a real position. **The determinism claim is unaffected in all 50 rows** —
capacity cannot cause a cross-platform divergence, both platforms compute the identical wrong world.
What is now wrong is any per-row claim about how many AI were simulating.

**1. Task 59 — the engine now refuses an over-capacity lobby** (`300f43652`, evidence `57dc6bbe9`).
Guard only. Two parts:

* **Driver.** `HeadlessMatch::lobbyFitsTheMap` mirrors `LanGameOptionsMenu.cpp:249-258`. Two call
  sites: an *intent* pre-check at the end of `publishGameOptions` and the *authoritative* check on
  the real slot list just before `RequestGameStart`. Both are needed — `isOccupied()` is FALSE for
  `SLOT_OPEN`, so counting the slot list at publish time undercounts by `-lanwait`. `-lanoverfill`
  restores the old behaviour, loudly.
* **Engine.** `GameLogic.cpp` guards `farthestIndex < 0` and skips the `taken[farthestIndex] = TRUE`
  out-of-bounds stack write. `setStartPos(-1)` is deliberately left outside the guard.

**Why the OOB had been harmless:** `hasStartSpotBeenPicked` is declared immediately before `taken[]`
(`:924-925`) and is already TRUE by then, so under declaration-order layout the write was idempotent.
A coincidence of stack layout, not a guarantee.

Verified red-green on a real build (`evidence/task59-capacity-guard.meta.txt`): alpine+Hx2 refuses
with **zero** CRC frames; alpine+Hx1 (exactly at capacity) runs; twilight+Hx5 runs; `-lanoverfill`
runs and the engine guard fires **once, for slot=2** — the predicted surplus player — while the same
run reports `aiPlayers=2`.

**Part 2 proven a no-op by measurement:** the parent commit was rebuilt as an unguarded control and
run on the identical legal match (twilight flame, Hx5, seed 424242, 300 frames). Byte-identical,
md5 `af5fdd3ed66fecfb6a1e7dfce2691ae2`.

**2. Task 61 — the activity metric no longer measures a train** (`643b48a7c`,
`evidence/task61-activity-metric.meta.txt`).

New `[GXACT]` trace in `GameLogic.cpp` beside `[GXCRC]`, off unless `GX_ACTIVITY=<frame period>`.
Reports per player `kind:objects/structures`, kind ∈ `A` skirmish AI / `H` human playable / `-`
neutral, observer, dead or empty slot. **Only `A` and `H` are judged** — `ThePlayerList` is not the
lobby, it always carries a neutral entry owning the map scenery plus one per empty slot.

**The measurement that justifies it:** legal twilight+Hx5 and overfilled alpine+Hx2 **both score 300
distinct of 300**. `[GXACT]` flags `p4` in the second and nothing in the first. The window that
catches it is frame 0 — `m_frame % period == 0` is always true there — because by frame 50 the
engine has marked the starved AI dead and its tag drops to `-`.

**Proven inert:** same seed with and without `GX_ACTIVITY` gives a byte-identical `[GXCRC]` stream.

Five harness defects fixed in the same pass, all the same bug — a verdict computed over a quantity
other than the one being claimed:

* `relay-8peer.sh` counted `DISTINCT0` over the full stream against a `SHORTEST/2` threshold, so it
  could print `distinct (peer0): 1500 of 3` and exit PASS.
* **No harness had a frame floor.** Now 90% of requested, in all five.
* `xplat-3platform-lobby.sh`'s `AI_PLACED` was a junk grep nothing read, while the PASS line printed
  the *requested* AI count. Now parsed from the `S=` field and the run FAILS on mismatch.
* `xplat-determinism-soak.sh` treated an idle sim as a warning and passed anyway.
* `xplat-lan-soak.sh` had **no idle check and no floor at all** — and it produced `xplat-lan-ai-1500`,
  cited as *the* macOS↔Windows determinism proof.

**3. Harness capacity fallout, shipped with task 59.** New shared `scripts/test/lib-map-capacity.sh`
(case-insensitive, which the old inline table was not — `relay-maptransfer.sh` passes
`Maps\Alpine Assault\...` and the old lowercase patterns silently returned "unknown"). Default maps
fixed in `relay-8peer.sh` (killing fields, holds 2, at PEERS=8), `relay-maptransfer.sh` (alpine,
needed 3) and `winhost.ps1` (alpine, needed 3). `xplat-3platform-soak.sh` now filters its rotation
up front and names every excluded map; its old comment claiming most maps "freeze in a NETWORKED
match" was the overfilled lobby and now says so.

## Corrections to things this project believed

| claim | reality |
|---|---|
| `4cb42f38e`'s own commit message: the digest moves to `0x0CFC0F61` | **Wrong, and already pushed.** `0x0CFC0F61` was measured on the DIRTY tree. `SimDigestCollect` hashes the `ls-tree` text plus a `#dirty` overlay, so committing the same content moves those SHAs into the tree listing and changes the input. Committed, it is `0x89C01A42`. Corrected in `evidence/simid-digest-by-commit.md` and in `465075c00`. **Rule: a digest measured on a dirty tree never equals the digest of the same content committed.** |
| session 7: "a build at HEAD can join the deployed peers" | True when written, for a clean tree at `73e192787`. Four uncommitted debug edits arrived after that, three inside the digest paths. See the table above. |
| `relay-maptransfer.sh`: alpine is "a map whose AI actually moves", per its 1499 distinct | The 1499 was the train. With 3 occupied slots on a 2-start map the AI there had no Command Center at all. |
| `xplat-3platform-soak.sh`: most maps "freeze in a NETWORKED match", do not prune them | There is no freeze. 3 humans + Hx5 needs 8 slots and most of those maps hold 2. |

## Open work, ranked

**1. Rebuild and redeploy all three desktops at HEAD.** Blocking for everything networked. They are
at `c72eb8d96`; HEAD is `0xD33D97FD`. Rebuild macOS, Linux and Windows *together* — a partial
rebuild leaves peers that cannot join each other. Everything is already pushed; the Linux and Windows
clones sync with `git fetch && git reset --hard origin/main`.

**2. Task 60 — re-run the networked corpus on capacity-8 maps, with `GX_ACTIVITY` set.** This is now
the main open item and it is what validates both of this session's changes in the configuration they
were built for. Nothing this session exercised the guard or the trace on a **networked** lobby;
everything was solo macOS. Re-label the pre-`1566959a9` rows as their replacements land.

**3. Task 62 — long-haul soak, capacity-8 maps only.** 17 logic fps networked, so a 30-min match is
~53 min wall and 50 matches is ~44 h. Downstream of 1 and 2.

**4. iOS/iPadOS is still stale and still cannot join.** Every iOS artifact embeds `2e226bf3a`
(rev 2171, epoch 1). Unchanged from session 7; the commands are in that document. The gap is now
larger, not smaller.

**5-11 unchanged from session 7:** client chat (task 47), the Online button (25, 41-45), run-ahead
measurement (46), map transfer (30), Windows `0xC0000005` teardown (36), destroyed structures
keeping old visuals (49), 1-2 s freezes (50), AWOL players blocking victory (34), defeated observers
(35), `unrouted` (51). Note the task-49 instrumentation is now committed and env-gated —
`GX_DRAWDBG`, `GX_RUBBLESWEEP`, `GX_ALLYHOST` — see `4cb42f38e`.

## A PARALLEL SESSION IS WORKING IN THIS REPO, AND ITS EDITS COLLIDED WITH THIS ONE

`GeneralsMD/.../Common/HeadlessMatch.cpp` was rewritten twice mid-session by something that was not
this session — once adding a `#include "GameClient/View.h"` that HEAD has never contained, and later
reverting the file to exactly HEAD, silently discarding the task 59 edits already in it. It was
caught only because the build did not relink and the object file was newer than the source: **a
`grep -c` for the new symbols returned 0 on a file that had been successfully edited minutes
earlier.** The edits were re-applied and committed immediately.

**Root cause found, and it is not an editor.** `git worktree list` shows a second worktree,
`.claude/worktrees/nervous-babbage-9d03d2`, on branch `claude/nervous-babbage-9d03d2`, whose single
commit `4d1237692` is *"debug(draw): instrumentation for the destroyed-structure rendering report"* —
the **same task 49 work** this session found loose in the main working tree and committed as
`4cb42f38e`. That session was editing in the MAIN tree, and moved its work onto its own branch while
this session was working.

**Consequence for `4cb42f38e`: it captured another session's work in progress, not a finished
change.** The two versions are not the same:

| file | `4cb42f38e` (this session's snapshot) | `4d1237692` (the other session) |
|---|---|---|
| `W3DModelDraw.cpp` | +244 | +245 |
| `HeadlessMatch.cpp` | +77 | +88 |
| `GameMain.cpp` | +6 | +6 — identical content |
| `ActiveBody.cpp` | +49 | +49 — identical content |

Nothing is lost — the fuller version is on its branch — but `main` now carries a partially-written
snapshot of it. **RECONCILED in `e9e94fabe`.** `main` now carries `4d1237692`'s version. It was NOT a plain
checkout: that branch predates task 59, so taking its `HeadlessMatch.cpp` wholesale would have
deleted the capacity guard. `W3DModelDraw.cpp` was taken whole; `HeadlessMatch.cpp` was merged by
hand. The branch itself is untouched and can be deleted once its owner is done with it.

**The rule this produced:** more than one agent may be writing to this tree. Verify edits are still
on disk immediately before building, gate on the build's own relink rather than its exit code, and
commit early. A build that does nothing and exits 0 is indistinguishable from one that succeeded.

## Not verified

* **Nothing this session ran networked.** Every measurement — the guard, the trace, the no-op proof
  — is solo macOS. The two capacity call sites agree by construction, but that agreement is reasoned,
  not measured. Same for `[GXACT]` across peers: the harnesses read one peer's log on the argument
  that lockstep makes them identical, which is true but untested for this line.
* **The harness code that consumes `[GXACT]` has not run against a real multi-peer run.**
  `starved_players` was exercised against locally produced logs only.
* **`-lanoverfill` has not been used on a networked lobby**, only solo.
* **The frame floor (90%) has never fired in anger.** It is arithmetic on numbers the harnesses
  already computed, but no run has yet been rejected by it.
* **`xplat-3platform-soak.sh`'s rotation filter was tested by replicating its block**, not by running
  the soak, which needs three machines. It kept 3/4/5/0 maps at `AI=Hx5/Hx1/none/Hx9`, matching the
  measured capacity table exactly.
* **The `references/fbraz3-dxvk` submodule edits are still unread.** Untouched this session. A
  `git submodule update` still destroys them.
* Everything session 7 listed as unverified that is not named above remains unverified.
