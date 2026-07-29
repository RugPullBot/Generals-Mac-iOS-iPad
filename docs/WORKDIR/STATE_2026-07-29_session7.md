# Session 7 dropoff — three platforms in one legal match; the "freeze" was ours; iOS is stale

Supersedes `STATE_2026-07-29_session6.md`. Read with `DESIGN_headless_and_relay.md`,
`tasks/todo.md` and `tools/relay/README.md`.

**How much to trust this document.** Every number in the evidence table was recomputed from the
committed `.crc` files. Prose claims outside that table were **not** all re-derived — two rounds of
review found three that were wrong (a Windows frame total, a frame-count "convention", and a
`CNC_GENERALS_ZH_PATH` rule), all of which had been stated as settled fact. A fourth and larger one
was found after that: this document's previous lead result and its "networked simulation freeze"
were **both artifacts of our own harness overfilling 2-player maps**, and both are corrected below. Treat a prose claim with
a code citation as checkable and a prose claim without one as a lead. **"Not verified"** at the
bottom lists what is known to be unchecked; it is not a guarantee that everything else was.

## Position

| | |
|---|---|
| HEAD | `1566959a9` — **rev 2190** (`git rev-list --count HEAD`) |
| `origin/main` | `c72eb8d96` — rev 2187 |
| unpushed | **3 commits** — `68eff86c4` (three-platform lobby test), `6377956d8` (the soak), `1566959a9` (the capacity diagnosis + harness guard). **None touches a SimID digest path**, so a build at HEAD still joins the deployed peers. |
| dirty | `M docs/WORKDIR/NEXT_SESSION_PROMPT.md`, `M tasks/todo.md`, `M references/fbraz3-dxvk`, `?? this file` |
| worktrees | one |
| deployed | macOS, Linux, Windows all at `c72eb8d96` rev 2187 epoch 2, **verified identical at runtime**. iOS/iPadOS is stale and cannot join. |

**`M references/fbraz3-dxvk` is not a pointer bump.** The submodule carries three uncommitted source
edits — `src/vulkan/vulkan_loader.cpp`, `src/wsi/sdl3/wsi_platform_sdl3_funcs.h`,
`src/wsi/sdl3/wsi_window_sdl3.cpp` — plus an untracked `subprojects/.wraplock`
(`git -C references/fbraz3-dxvk status --porcelain`). They are **outside** the SimID digest path
list, so they cannot affect join compatibility. They **are** on the macOS render path that ships as
`libdxvk_d3d8.dylib`, which puts them next door to tasks 49 (destroyed structures keep old visuals)
and 50 (1-2 s freezes, DXVK pipeline compilation the leading hypothesis). **A `git submodule update`
would discard them.** What they change and why they are uncommitted is an **open question** — nobody
this session read the diffs.

**THE OPERATIONAL FACT.** macOS, Linux and Windows are all deployed from `c72eb8d96` and were probed
at runtime today: `rev=2187 engine=FD486019 source=B30B651C data=1839A83F ordinal=9F43F7B5 epoch=2`
on all three, `platformID` differing by design
(`evidence/xplat3-5ai-1500.meta.txt:14-16`). **iOS/iPadOS is not.** Every iOS artifact on this Mac
embeds git SHA `2e226bf3a` — rev 2171, **18 commits back** — and `SIMID_EPOCH` went 1 → 2 inside that range
(`git diff 2e226bf3a..HEAD -- Core/GameEngine/Include/Common/GameDefines.h`). The epoch feeds
`engineID` (`SimulationId.cpp:100`) and `engineID` is the *first* tier-1 compare
(`SimulationId.cpp:553`), so the iPad is refused with `ENGINE_DIFFERS` before `sourceID` or `dataID`
is looked at. It cannot be nursed along. Exactly what has to happen:

```bash
cmake --preset ios-vulkan
cmake --build build/ios-vulkan --target z_generals          # THIS is the step that has been skipped twice
strings -a build/ios-vulkan/GeneralsMD/GeneralsXZH.app/GeneralsXZH | grep -aoE '^[0-9a-f]{40}$' | head -1
./scripts/build/ios/package-ios-zh.sh --install             # USB, never WiFi
```

`package-ios-zh.sh:70-76` checks only that the binary **exists** — no freshness, no commit check —
then copies it (`:152`). That is how a stale engine shipped to the device twice.

**There are four stale iOS artifacts, not one, and three of them look sideloadable.** All were
checked with `strings -a <bin> | grep -aoE '^[0-9a-f]{40}$' | head -1`:

| artifact | mtime | embedded SHA |
|---|---|---|
| `build/ios-vulkan/GeneralsMD/GeneralsXZH.app/GeneralsXZH` | Jul 29 04:53 | `2e226bf3a` |
| `build/ios-package/GeneralsXZH.app/GeneralsXZH` | Jul 29 04:54 | `2e226bf3a` |
| `GeneralsXZH-ios.ipa` (repo root, 2.1 GB) | Jul 27 03:51 | — not opened; predates both binaries |
| `GeneralsXZH-ios-codeonly.ipa` (repo root, 21 MB) | Jul 27 05:05 | — not opened; predates both binaries |

`package-ios-zh.sh --install` over USB is the **only** supported path. The two `.ipa` files are stale
build products from two days before the epoch bump — **regenerate or delete them, do not sideload
them.** Reaching for the obvious-looking `.ipa` is exactly how rule 4 ("verify the ARTIFACT, not the
git tree") gets violated a third time.

**A build at HEAD *can* join the three deployed desktops.** This corrects the loudest claim in the
previous draft of this document. `revision` is informational and never compared
(`Core/GameEngine/Include/Common/Diagnostic/SimulationId.h:39`; `SimIdCompare` at
`SimulationId.cpp:548-562` reads tag/engine/source/data/ordinal and never `revision`). `sourceID` is
a SHA256 over a fixed path list plus a dirty overlay (`resources/gitinfo/simsourcedigest_watcher.cmake`,
paths at `:66-105`, overlay at `:146-190`, hash at `:204`). Running that script verbatim against this
working tree as it stands — dirty files and all — produces `ZH=0xB30B651C`, byte-identical to what
the deployed `c72eb8d96` binaries report. `68eff86c4` touches only `docs/WORKDIR/evidence/*` and
`scripts/test/xplat-3platform-lobby.sh`, neither of which is in the digest path list. The real gate
is **same tree content under the digest paths, and clean** — not same commit.

## The lead result: three platforms, five Hard AI, a LEGAL lobby, and a control

Commit `1566959a9`, harness `scripts/test/xplat-3platform-lobby.sh`, evidence
`evidence/xplat3-tf-legal-1500.*`. **This supersedes `xplat3-5ai-1500`**, which ran on a 2-player
map with 8 players in it — see the next section.

| | |
|---|---|
| lobby | 8 of 8 **legal** slots — `gxlinux` (host) + `gxmac` + `gxwin` + 5x Computer Hard |
| map | `maps\twilight flame\twilight flame.map` — **capacity 8**, measured |
| seed | 606060 |
| frames | 1500 on each of the three peers |
| md5 | `b29a9b44bd3a5ff68e77aef25f966baa` on all three (CRC **value column**) |
| `cmp` | byte-IDENTICAL on all three pairs (mac/linux, mac/win, linux/win) |
| differing | 0 |
| distinct | **1500 of 1500** |
| AI | `aiPlayers=5 computers=7 sides=11 sidesWithScripts=7 skirmishSides=14` |

**The control is the part that matters, and it is new.** A high distinct count does NOT by itself
mean the AI did anything — an ambient mover with no player attached produces one. So run the same
networked match with no AI and see whether the activity survives:

| networked, 2 peers, 300 frames | 0 AI | 5 Hard AI |
|---|---|---|
| `twilight flame` (capacity 8) | **3 distinct** | **300 distinct** |
| `alpine assault` (capacity 2) | **299 distinct** | **299 distinct** |

On a legal map the activity vanishes without the AI, so the AI are demonstrably simulating. On
alpine the number is identical with and without them.

**Scope, stated rather than implied: one map, one match, one seed, 1500 frames.** It falsifies
"cross-platform play is guaranteed to desync". It says nothing about the tail.

**The 75,000-frame soak did not contribute to this and this does not extend it.** That soak is
macOS vs Linux, solo headless, zero packets, at `2e226bf3a`. Two different claims. Do not merge.

### The Windows asymmetry, counted properly

An earlier draft said "roughly 2,100 frames total"; that was wrong and dropped two sets this same
document lists as determinism evidence. Recounted from the committed streams:

| set | Windows frames | distinct | counts as |
|---|---|---|---|
| `xplat3-tf-legal-1500-win.crc` | 1500 | 1500 | active, **legal lobby** |
| `xplat3-5ai-1500-win.crc` | 1500 | 1499 | **overfilled map — see below** |
| `xplat-lan-ai-1500-win.crc` | 1500 | 1499 | active (twilight flame, 1 AI) |
| `relay-winhost-600-windows.crc` | 600 | 599 | active |
| `relay-win-vs-linux-600-windows.crc` | 600 | 600 | active |
| `xplat-lan-3000-win.crc` | 3000 | **3** | idle — not evidence |
| `relay-3platform-93017-windows.crc` | 93,017 | 93,016 | **real** — see below |

**`relay-3platform-93017` is genuine and is the largest artifact in the corpus.** All three streams
are 93,017 lines with the identical value-column md5 `f65722d3252acf0737cb4b0e5ba3f13f`. It ran on
`twilight flame` with **4 players**, comfortably inside capacity, which is exactly why it worked.
Earlier drafts listed its provenance as disputed; it is not.

## There is no "networked simulation freeze" — the harness was overfilling 2-player maps

The previous draft of this document led with a networked freeze that reframed the whole corpus.
**It does not exist.** It was two of our own defects stacked, and together they manufactured it.
Full write-up: `evidence/networked-sim-freeze-diagnosis.md`; corrected summary here.

**Defect 1 — the lobby is never bounded by map capacity.** `populateRandomStartPosition` caps
positions at `md->m_numPlayers` (`GameLogic.cpp:882-885`) and marks every index past it as already
taken (`:928`). Surplus players keep `startPos=-1`, `placeNetworkBuildingsForPlayer` fails to place
a Command Center, and `AISkirmishPlayer::adjustBuildList` finds none and returns. The AI is
constructed, has a brain, has scripts, and **owns nothing** — while the run still reports
`aiPlayers=5` and completes normally. Solo looked healthy because it puts the first AI in slot 1,
so one AI always got a base.

**Measured map capacities** (load each map, read `md->m_numPlayers`):

| capacity 8 | capacity 4 | capacity 3 | capacity 2 |
|---|---|---|---|
| death valley | dark mountain | cairo commandos | alpine assault, bitter winter |
| destruction station | | | desert fury, dust devil |
| twilight flame | | | final crusade, killing fields |

**Only three standard maps hold 8 players.** Every "8-player" test in this project before
`1566959a9` used `alpine assault`, which holds **two**.

**Defect 2 — "distinct CRC values" does not mean the AI did anything.** `alpine assault` carries a
train. `RailroadBehavior` self-moves every frame with no player and no AI attached, and
`Object::crc` hashes the transform. So alpine scored 1499/1500 distinct with its AI inert — and
alpine was the control every other map was judged against.

The falsifier, run directly rather than taken on trust, is the 0-AI table in the previous section:
alpine gives **299 distinct with five Hard AI and 299 with none**.

**What this invalidates.** Any claim of the form "the AI were active because the distinct count was
high" that rests on an `alpine assault` run. That includes the previous lead result
(`xplat3-5ai-1500`) as an *AI* result — it remains a valid three-platform **determinism** result,
because three compilers agreeing bit-for-bit for 1500 frames is not explainable by a train.

**What survives untouched.** The 75,000-frame macOS-vs-Linux soak (solo, no networking, 10 maps,
50 seeds), `relay-3platform-93017` (twilight flame, 4 players, legal), and every cross-platform
determinism claim. The bug was in what we *measured*, not in the engine's lockstep.

**The harness now refuses an overfilled map in preflight** (`xplat-3platform-lobby.sh`), verified
red-green: it rejects alpine+Hx5 ("holds 2 players but this lobby needs 8") and accepts twilight
flame+Hx5. **The engine is unchanged** — nothing in `1566959a9` touches simulation, so every CRC
baseline in `evidence/` remains comparable.

**Still open, and the first thing to decide:** whether the *engine* should also refuse or clamp an
over-capacity lobby. The GUI already checks (`LanGameOptionsMenu.cpp:250-256`); `HeadlessMatch`
does not. Any engine-side change here reassigns start positions and would invalidate every CRC
baseline in `evidence/` plus retail 1.04 replay compatibility, so it is a guard-only change or
nothing.

## Evidence table

Frame counts below are **CRC lines**. Every number here was recomputed from the committed `.crc`
files.

**The engine's `simulated N frames` counter versus the line count — the actual rule, measured.** An
earlier draft claimed the counter is always one less "because frame 0 emits a line". That is false
for most of the corpus, and the stated cause cannot be right (frame 0 emits a line in the 600-frame
runs too). Grepped from every committed meta:

| set | CRC lines | `simulated N frames` | ended on |
|---|---|---|---|
| `lanseed-solo-800` | 800 | 800 | `-lanframes` budget |
| `relay-3peer-900` | 900 | 900 | `-lanframes` budget |
| `relay-inet-mac-vs-linux-1200` | 1200 | 1200 | `-lanframes` budget |
| `relay-win-vs-linux-600` | 600 | 600 | `-lanframes` budget |
| `relay-winhost-600` | 600 | 600 | `-lanframes` budget |
| `relay-3platform-93017` | 93,017 | 93,016 | game over |
| `relay-played-match-14756` | 14,757 | 14,756 | game over |

So: **a run bounded by `-lanframes` reports a counter equal to its CRC line count. The two matches
that ended on game-over rather than the frame budget report one less.** Why the two differ is an
**open question** — `d9ebe45cf` ("report the frame high-water mark, not the post-teardown counter")
is the obvious suspect but nobody has checked whether it is the dividing line for these files. Do
not "correct" a `-lanframes` run's numbers to make them off by one; they are not.

| set | shape | lines | distinct | kind | streams committed |
|---|---|---|---|---|---|
| `xplat3-5ai-1500` | **macOS + Linux + Windows** + 5 Hard AI, relay | 1500 | **1499** | determinism | 3 of 3, `cmp` identical |
| `xplat-soak-50x1500` | 50 solo matches, macOS vs Linux, no packets | 75,000 | 1470–1500 | determinism | **0 of 100 — see below** |
| `relay-3platform-93017` | played match, 3 endpoints | 93,017 | 93,016 | determinism | 3 of 3, `cmp` identical |
| `relay-played-match-14756` | played match | 14,757 | 14,757 | determinism | **2 of 3** (Windows uncaptured) |
| `relay-tworooms-3000` roomA | Linux host + macOS, room A | 3000 | 3000 | determinism + isolation | 2 of 2 |
| `relay-tworooms-3000` roomB | Linux host + macOS, room B | 3000 | 2999 | determinism + isolation | 2 of 2, differs from roomA |
| `relay-winhost-600` | **Windows as host** + Linux joiner | 600 | 599 | determinism | 2 of 2, `cmp` identical |
| `relay-7peer-ai-1500` | 7 headless + 1 AI | 1500 | 1499 | determinism | **1 of 7** (peer0) |
| `relay-3peer-900` | Linux host + Linux peer + macOS | 900 | 900 | determinism | 3 of 3 |
| `relay-inet-mac-vs-linux-1200` | macOS + Linux over the relay | 1200 | 1200 | determinism | 2 of 2 |
| `relay-win-vs-linux-600` | Windows joiner + Linux host | 600 | 600 | determinism | 2 of 2 |
| `lanseed-solo-800` | 2 solo runs, seed pinned 424242 | 800 | 800 | determinism, zero networking | 2 of 2 |
| `xplat-lan-ai-1500` | macOS ↔ Windows on LAN, AI active | 1500 | 1499 | determinism | 2 of 2 |
| `relay-8peer-idle-1500` | 8 headless peers, one netns each | 400 (head sample) | **3** | **transport only — sim idle** | 1 of 8, sample |
| `xplat-lan-3000` | macOS ↔ Windows on LAN | 3000 | **3** | **transport only — sim idle** | 2 of 2 |
| `xplat-inet-mac-vs-linux-60` | macOS ↔ Linux over the internet | 60 | **3** | **transport only — sim idle** | 2 of 2 |

Four rows to read carefully:

* **Every networked row above needs its MAP read alongside its distinct count**, since the freeze
  finding. The active networked rows are on `alpine assault` (`xplat3-5ai-1500`,
  `relay-winhost-600`, `relay-7peer-ai-1500`) or `twilight flame` (`xplat-lan-ai-1500`,
  `relay-3platform-93017`, `relay-played-match-14756`). `relay-8peer` is on `killing fields` — a map
  now known to freeze networked — which means its distinct = 3 may be the freeze rather than "eight
  peers with no AI". Its **network** claims (8 joins on each port, room 8/8, lockstep to 1500) are
  unaffected; they were never determinism claims.
* **`xplat-lan-3000` was in the previous dropoff's "Proven, do not re-investigate" table with no
  caveat, and it is an idle sim** — 3 distinct values across 3000 frames, one covering 2,996 of them.
  Identical to the 8-peer signature the same document correctly rejected two rows above it. The real
  macOS↔Windows determinism proof is `xplat-lan-ai-1500` (1499 distinct, `CH` slot in
  `xplat-lan-ai-1500-slotlist.txt`), which was not in the table. Swapped here. Note no slot list is
  committed for `xplat-lan-3000` itself, so "no AI" is the harness default, not an observation.
* **`xplat-soak-50x1500` has no committed raw artifact.** Only the meta, manifest and console are in
  git. The 100 `.crc` streams exist **only** at
  `/tmp/claude-501/-Users-administrator-GeneralsX-src/1f6becd6-6780-4ff5-b682-a13c6ea6d85e/scratchpad/soak50`
  — another session's scratchpad, 100 files, 3.9 MB, still present as of this writing. All 50 pairs
  were recomputed from there today (0 differing, distinct 1470–1500, all 50 manifest md5s reproduce
  against both the mac and the linux file), so the numbers are real. But the project's largest single
  claim currently rests on a directory that a reboot deletes.
* **`relay-7peer-ai-1500` "21/21 pairs" and `relay-8peer` "28/28 pairs" cannot be checked.** peer0
  reproduces exactly in the 7-peer case (1500 lines, 1499 distinct, whole-file md5
  `ad956b152ddd9846b4dbdb5d9e89ddfc`); the other six streams are absent from the repo and from every
  surviving scratchpad. For the 8-peer run only a 400-line head sample survives, so its quoted md5
  `c0c918356475555c750e091c2ad82c0c` is not reproducible either. What *is* independently verified for
  the 8-peer run is the network half: `relay-8peer-relay.txt` shows 8 joins on 8086 and 8 on 8088,
  virtual IPs `10.42.0.1`–`.8` on both ports, room reaching `(8/8)`. `relay-8peer.sh:153` writes
  `peer$i.crc` per peer, so preserving all of them next run is free.

## Three harness bugs that produced convincing WRONG answers today

All three are launch-shape faults, all three present as something else entirely, and all three are
now documented in `scripts/test/xplat-3platform-lobby.sh` at the line that fixes them. The next
session will re-hit them the moment it writes a new harness.

**1. The relative-path loose scan → `DATA_DIFFERS`.** `xplat-3platform-lobby.sh:189-193`.
`SimulationId`'s loose-data scan hands **relative** directories to `getFileListInDirectory`
(`Core/GameEngine/Source/Common/Diagnostic/SimulationId.cpp:275`, consumed at `:316`), which
`LocalFileSystem` resolves against the **process cwd**. Launch the Linux peer from `/root` instead of
`/root/gamedata` and it scans nothing, `data-loose` changes, `dataID` changes, and every joiner is
refused with `verdict=DATA_DIFFERS` while `engineID` and `sourceID` match perfectly. That reads
exactly like a genuine cross-platform data mismatch — the thing this project has spent two sessions
chasing — and is not one. **Every peer must be launched with cwd set to its game directory.** macOS
gets this free (`run.sh` does its own `cd`, `deploy-macos-zh.sh:245`), Windows gets it from `go.bat`
(`setup-run-win64.ps1:128`), Linux has to be told.

**2. The ssh subshell that never closes the channel.** `xplat-3platform-lobby.sh:194-197`.
`ssh host "cd X && setsid game > out 2> err &"` backgrounds the whole and-list in a subshell whose
own stdout is **still the ssh channel**. ssh never sees EOF and blocks until the match ends. The
launch looks like a hang, the room token is never read, and the run dies in the join phase. The
redirections have to belong to the subshell, not to the game:
`ssh host "( cd X && exec setsid game ) < /dev/null > out 2> err & echo started"`.

**3. OpenSSH on Windows kills the process tree at session close.** `xplat-3platform-lobby.sh:225-230`.
Detaching the Windows peer on the far side does not work: a `Start-Process` launch dies within
milliseconds of ssh returning and leaves a **zero-byte stderr file**, which reads as "the joiner
never started" rather than "something killed it". The only shape observed to survive is running the
game **synchronously inside a held-open ssh session** and backgrounding that whole ssh invocation on
the Mac side. The SimID probe already had this shape; the joiner had to be rewritten into it.

Related and already known, restated because it is the same class: `pkill -f` matches our own ssh
command line and kills the remote shell (exit 255). Use `pkill -x GeneralsXZH`, count with
`ps -eo comm | grep -cx GeneralsXZH`, and **confirm the count** — `verify_clean()` at
`xplat-3platform-lobby.sh:107-119`.

*All `xplat-3platform-lobby.sh` line numbers in this document are for the committed file at HEAD
(`6377956d8`), which includes the `SEED`/`SKIP_FINGERPRINT` additions. An earlier draft cited an
uncommitted copy and was 6-13 lines off throughout.*

## Windows headless works — the GUI mouse-click driving was never necessary

This is an unlock, not a bugfix. Earlier sessions drove the Windows peer through the GUI with
scripted mouse clicks and a `schtasks /it` session-1 task, on the belief that Windows headless was
broken. It was not. It needs **`CNC_GENERALS_ZH_PATH` set, pointing at the Steam data install**.
Without it the engine stalls forever right after `[INI] ERROR: No files read from directory`. With
it, it reaches `[SIMID]` in about five seconds. That variable is mandatory and fails silently.

**The trailing backslash is NOT required, and saying it was is a correction to this document.** An
earlier draft called its absence fatal, in caps, in two places. The separator has been appended by
the engine since `ac5dcfc50` (rev 2061, 2026-07-27):
`Core/GameEngineDevice/Source/Win32Device/Common/Win32BIGFileSystem.cpp:282-296` normalises the
directory before `loadBigFilesFromDirectory`, and its own comment names the reason. `git merge-base
--is-ancestor ac5dcfc50 c72eb8d96` exits 0, so it is in **every** deployed build — and also in the
stale iOS `2e226bf3a`. Two committed harnesses set the variable **without** a trailing backslash and
produced good evidence: `scripts/test/winhost.ps1:13` (→ `relay-winhost-600`, 600 frames, `cmp`
identical) and `scripts/test/xplat-lan-soak.sh:22` (→ `xplat-lan-ai-1500`). A caps-locked rule that
the repo's own working scripts violate is worse than no rule: it invites someone to "fix" a working
script, or to blame the backslash for a Windows hang that has another cause (rule 16).

The raw concatenation that once needed it is real but its citation was stale: it is
`Win32LocalFileSystem.cpp:209-212` (`asciisearch = originalDirectory; concat(currentDirectory);
concat(searchName)`), not `:131-137`, which in the current tree is an unrelated asset-root retry
inside `openFile`. **`setup-run-win64.ps1:31-32` carries the same stale citation** and should be
fixed there too. Keeping the trailing slash in `setup-run-win64.ps1:30-36` / `go.bat:124` /
`xplat-3platform-lobby.sh:71` is fine as belt-and-braces; it is just not load-bearing.

Consequence: the whole Windows side of a three-platform run is scriptable from the Mac. No GUI, no
mouse, no session-1 task. `scripts/test/winhost.ps1` remains useful for a Windows-**hosted** run
because a map argument with both spaces and backslashes does not survive bash → ssh → PowerShell
quoting, but the *reason* it existed — "Windows can only be driven interactively" — is gone.

Still open and unchanged: the Windows peer reaches its frame limit, logs `simulated N frames`,
flushes, and **then** faults on teardown with `0xC0000005`. Frame data is complete, the CRC stream is
the verdict, and the harness deliberately does not gate on the exit code
(`xplat-3platform-lobby.sh:249-252`). The crash is real and undiagnosed. Task 36.

## Open work, ranked

**0. Push `68eff86c4`, `6377956d8` and `1566959a9`, and rescue the soak artifacts.** Blocking, needs Karl.
The Linux and Windows clones sync with `git fetch && git reset --hard origin/main`, so they
physically cannot reach HEAD until it is pushed — `git push origin main` is an external action, ask
first. Note the freeze reproduction above runs `scripts/test/relay-8peer.sh` on the VPS, which needs
the VPS clone at HEAD. Separately, copy `.../scratchpad/soak50/*.crc` (100 files, 3.9 MB) into
`docs/WORKDIR/evidence/` and commit, or the 75,000-frame claim becomes permanently unreproducible
prose.

**1. Decide whether the ENGINE should refuse an over-capacity lobby.** The "freeze" is solved and
the harness is guarded (`1566959a9`), but the engine still silently accepts more players than the
map has start positions and hands the surplus `startPos=-1`. The GUI checks
(`LanGameOptionsMenu.cpp:250-256`); `HeadlessMatch` does not. **This is a guard-only change or
nothing**: anything that reassigns start positions invalidates every CRC baseline in `evidence/`
and retail 1.04 replay compatibility. Decide the shape before writing code. Task 56.

**1b. Re-run the corpus on legal maps.** Every pre-`1566959a9` "8-player" result used a 2-player
map. `xplat3-tf-legal-1500` is the corrected shape; the rest of the networked corpus should be
re-run on `death valley` / `destruction station` / `twilight flame` and the old rows re-labelled.
Cheap, mechanical, and it is what makes the volume claims mean something.

**1c. Fix the activity metric itself.** "distinct CRC values" is satisfied by any ambient mover —
that is what manufactured the freeze. A real gate needs to attribute change to *players*, not to
the world. Until then, **every** activity claim needs the 0-AI control run alongside it, which is
now the house pattern (`evidence/xplat3-tf-legal-1500.meta.txt`). Related harness defects worth
fixing in the same pass, all of which can make a broken run look like a passing one:
`relay-8peer.sh` compares `DISTINCT0` over the full stream against a `SHORTEST/2` threshold
(`:199-205`), and every multi-peer harness truncates to the shortest stream with **no floor**
(`xplat-3platform-lobby.sh:280`, `xplat-3platform-soak.sh`, `xplat-determinism-soak.sh:170`) — so a
peer dying at frame 3 silently redefines a 1500-frame experiment as a 3-frame one and prints PASS.

**2. iOS/iPadOS.** The only platform off epoch 2. Commands at the top of this document. Tasks 26-28.

**3. Client chat — the smallest well-defined piece of work in the tree.** The relay half is done and
tested (`5292259c7`; `node tools/relay/test-lobby.js` exits 0 with `52 checks, 52 passed, 0 failed`,
reproduced rather than trusted). The client half does not exist: an **unbounded** grep for
`GXCHT|GXSAY` across the repo hits `tools/relay/relay.js`, `test-lobby.js`, `tools/relay/README.md`,
`tasks/todo.md` and the WORKDIR docs — **zero `.cpp` or `.h` files**. The chat box is deliberately
inert (`WOLLobbyMenu.cpp:1170` `textEntryChat->winEnable(FALSE)`, `:1207` pushes
`GUI:RelayLobbyNoChat`), and `Transport.h` exposes no chat API — only `sendRelayRegistration` (`:105`)
and `enterRelayRoom` (`:178`). One send call, one receive hook. Task 47.

**Before testing a client against the live relay, confirm what the live relay is running.** The
running service is **not** the repo copy: `tools/relay/README.md:135-197` documents the deploy as a
systemd unit `gxrelay` executing `/opt/gxrelay/relay.js`, installed by
`scp tools/relay/relay.js root@VPS:/opt/gxrelay/relay.js`. Syncing `/root/GeneralsX` with
`git reset --hard` does **not** update it. Whether the live VPS actually matches that documented
layout is itself unconfirmed from here — check, do not assume:

```bash
ssh root@163.5.210.131 'systemctl status gxrelay; grep -c GXCHT /opt/gxrelay/relay.js'
# read the NUMBER. grep -c exits 0 when it finds matches - do not gate on it (rule 5).
journalctl -fu gxrelay        # live traffic
```

Redeploying it is `scp tools/relay/relay.js root@163.5.210.131:/opt/gxrelay/relay.js && systemctl
restart gxrelay` — **the one action that drops every game in progress. Needs Karl's OK.**

**4. Click the Online button.** `MainMenu.cpp:1585` pushes `Menus/WOLCustomLobby.wnd` in the
`onlineID` branch, guarded only by the generic transition debounce — no feature flag. The relay-mode
switch then holds: `TheGameSpyPeerMessageQueue` is assigned non-null at exactly one place
(`PeerDefs.cpp:615`, inside the full `SetUpGameSpy` chain) and nothing on the Online path calls it, so
`WOLLobbyMenu.cpp:1105` `s_relayLobby = (TheGameSpyPeerMessageQueue == nullptr)` evaluates TRUE. The
path is reachable and unguarded — which is precisely why "never drawn" is the risk. ~870 unobserved
lines now own the entire Online path, and the Direct Connect browser that has actually carried real
matches is no longer what that button opens. Tasks 25, 41-45.

**5. The run-ahead measurement.** Task 48 delivered the lobby; the measurement it was supposed to
carry was **not taken**, and no render-fps-vs-`m_frameRate` data exists anywhere in
`docs/WORKDIR/evidence/`. The code chain was re-derived line by line by two independent audits and
holds at every step:

* `canUpdateGameLogic()` is called once per engine-loop iteration, i.e. once per rendered frame
  (`GeneralsMD/.../Common/GameEngine.cpp:1212`); `update()` ticks logic at most once (`:1216-1220`),
  no catch-up loop.
* With `TheNetwork != nullptr` it takes `canUpdateNetworkGameLogic()` (`:1011-1024`), which is
  `TheNetwork->isFrameDataReady()` **or** `m_localStatus == NETLOCALSTATUS_LEFT`
  (`Core/.../Network.cpp:842-844`). No time accumulator.
* The single-player path already has the decoupling: `canUpdateRegularGameLogic()` (`:1027-1059`)
  drives logic from `m_logicTimeAccumulator` at `BaseFps` (`GameCommon.h:67`).
* The shared rate is the **minimum render fps** across peers: `FrameMetrics.cpp:90`
  `m_fpsList[...] = TheDisplay->getAverageFPS()` → `ConnectionManager.cpp:1404` →
  `getMinimumFps()` (`:1556-1568`) → `clamp(MIN_LOGIC_FRAMES, minFps, m_framesPerSecondLimit)`
  (`:1421`, whose own comment reads *"this clamps the logic time scale fps in network games"*) →
  `setFrameRate(minFps)` to everyone (`:1457`).
* It lands as the pacing divisor: `Network.cpp:656` `m_frameRate = msg->getFrameRate()`, `:798`
  `frameDelay = m_perfCountFreq / m_frameRate`.

**Inferred from code, not measured.** The conclusion — one client's render hitches throttle the
simulation for everybody — follows from the chain but nobody has instrumented a match. Worth knowing
while designing the measurement: `LANAPICallbacks.cpp:260` sets `m_useFpsLimit = false` when a network
game starts, so render runs uncapped during a match while `m_framesPerSecondLimit` still serves as the
clamp ceiling. Task 46.

**6. Map transfer.** Broken in headless with or without a relay, and correctly localised: the host's
transfer mask is empty (`FileTransfer.cpp:249-256` builds it from `isHuman() && !hasMap()`, `:257-258`
returns TRUE without transferring). New this session and not in the previous draft: **positive proof
the host's `OnHasMap` never fired.** `HeadlessMatch.cpp:226-232` logs `peer %d.%d.%d.%d does NOT have
the map` whenever status is FALSE, and that line is **absent** from the host log in
`relay-maptransfer-failure.txt` while the joiner's own local line is present. That narrows the search
to the three early-outs in `LANAPIhandlers.cpp::handleHasMap`: `:911` `if (!m_inLobby && m_currentGame)`,
`:917` the mapNameCRC equality, `:922-926` the senderIP-to-slot match. **Resist the relay-address
hypothesis** — the identical failure reproduces on plain LAN with no relay. Second live hypothesis,
also unproven: the transfer is UI-driven (`FileTransfer.cpp:260-262` `TheShell->hideShell()`, a
`MapTransferLoadScreen`, `ls->init()`) and headless never pumps that load screen. Decisive experiment
unchanged: the same missing-map join from a **UI client on macOS**. Task 30.

**7. Finish the three-platform soak.** `scripts/test/xplat-3platform-soak.sh` (committed in
`6377956d8`, 175 lines) drives `xplat-3platform-lobby.sh` once per iteration across an 11-map
rotation (`:79-91` as committed) with pinned seeds (`SEED_BASE + 977n`, default 900000), skips the
~90 s fingerprint probes after iteration 0, and counts an iteration whose distinct count is under
half its frame count as INCONCLUSIVE rather than a pass. **It has now been executed** — and its first
run found the freeze, which is why the volume it was built to collect does not exist yet. A long soak
is only worth its wall clock once the freeze is understood; until then it would measure `alpine
assault` over and over. The working tree adds an uncommitted `ONLY_MAP` filter (+16 lines) so a
long-haul run can pin the rotation to alpine while the freeze stands; commit it with the docs.
Task 53, now downstream of task 56.

**8-11, unchanged and carried forward.** Windows `0xC0000005` teardown fault (task 36); destroyed
structures keeping old visuals (49); 1-2 s freezes (50); AWOL players blocking victory (34,
`VictoryConditions.cpp:328` is purely asset-based, and **`VictoryConditions::update` calls
`p->killPlayer()`** so any fix mutates simulation state and must use state all peers agree on);
defeated observers never reaching the score screen (35); `unrouted` no longer always zero (51).

**Cleanups.** `origin/fix/blocker5-residual-libm-trig` still exists remotely (deleted locally,
confirmed via `git branch -r`) — a one-line push-delete. And **copy `C:\dev\cb.bat` into the repo**:
`find` plus an unbounded grep confirm it exists nowhere in the tree — **doc mentions only, no file
anywhere** — so the exact Windows build command is machine-local. Same shape, and not previously
listed: **`/root/vpsbuild.sh`** is asserted to equal `scripts/build/linux/build-linux-relay.sh` and
nobody has diffed them (`diff /root/vpsbuild.sh scripts/build/linux/build-linux-relay.sh` settles
it). Those two are the reproducibility gaps of the four platforms.

## Corrections to the previous draft of this document

These were wrong or overstated **when written**, not merely stale. Fixed above; recorded here so the
same sentences do not come back.

| claim | reality |
|---|---|
| "the `LANEnableStartButton` null-deref fixed in `GeneralsMD/` is still live in `Generals/`" | Mirrored in `a2e959ee0` (rev 2126, a full day earlier). `Generals/.../LanGameOptionsMenu.cpp:388` carries the identical guard. |
| map transfer fails "character-for-character identically, **host included**" | The **joiner** transcripts are byte-identical; the host halves differ by one line (`[GXLAN] starting match`, a relay-mode artifact). The load-bearing conclusion — the failure is not the relay's — is unaffected. The overstatement originates in `relay-maptransfer.meta.txt:45`. |
| `DoAnyMapTransfers` pumps `ls->update()` | No such call exists — `grep -n "update()" Core/GameEngine/Source/GameNetwork/FileTransfer.cpp` exits 1. The pump is `ls->processProgress()` (`:91-109`) against `TheNetwork->getFileTransferProgress()` (`:87`), and it lives in `doFileTransfer` (`:41`), not in `DoAnyMapTransfers` (`:244`). |
| `RTS_BUILD_OPTION_FFMPEG` conflict at `CMakeLists.txt:305` | The root `CMakeLists.txt` is 183 lines. The site is `Core/GameEngineDevice/CMakeLists.txt:305`. |
| "`Generals/` has no headless CLI" | It has `-headless` (`Generals/.../CommandLine.cpp:1150`, `parseHeadless` at `:416`) and the full `m_headless` plumbing. What is missing is the **headless LAN driver** — `-lanhost`/`-lanjoin`/`-lanai`/`-lanseed` are ZH-only. |
| "a peer at a different commit is refused outright" | `revision` is never compared. Same tree content under the digest paths joins fine at a different commit; **a dirty clone at the *right* commit does not**. |
| "`build/ios-vulkan` has no `GeneralsMD/GeneralsXZH` binary" | It is inside the bundle: `build/ios-vulkan/GeneralsMD/GeneralsXZH.app/GeneralsXZH`, 38,823,560 bytes. Stale, not missing. |
| "Windows appears in roughly **2,100 frames total**" | **~4,200 active-sim frames** — it dropped `xplat-lan-ai-1500` (1500/1499) and `relay-win-vs-linux-600` (600/600), both of which this same document lists as determinism evidence. Only 1500 of the 4,200 have Windows in a match with *both* other platforms. The wrong figure is also baked into `6377956d8`'s commit message and `xplat-3platform-soak.sh:10`. |
| "the engine's `simulated N frames` counter is one less, **because frame 0 emits a line**" | False for five of the seven metas that carry a counter, and the stated cause cannot be right. A `-lanframes`-bounded run reports a counter **equal** to its line count; only the two game-over matches report one less. Table above. |
| "`CNC_GENERALS_ZH_PATH` needs a **trailing backslash** or the engine stalls forever" | The variable is mandatory; the backslash is not. `Win32BIGFileSystem.cpp:282-296` appends the separator itself, since `ac5dcfc50` — an ancestor of every deployed build. `winhost.ps1:13` and `xplat-lan-soak.sh:22` both omit it and produced committed evidence. The `Win32LocalFileSystem.cpp:131-137` citation was stale too; the raw concat is `:209-212`. |
| harness line numbers cited against an uncommitted working-tree copy | `xplat-3platform-lobby.sh` is now committed at HEAD with the `SEED`/`SKIP_FINGERPRINT` diff applied, so the citations in this document resolve. The earlier draft's were 6-13 lines off. |

## Not verified

Each line says why it could not be checked and what would check it. **This is a list of known gaps,
not a certificate that everything outside it was verified** — three of this document's own prose
claims were wrong while sitting outside this section (see "Corrections" above).

* **The WOL lobby has never been drawn.** Commit `9c6a89f2c` says so itself, and no artifact in
  `docs/WORKDIR/evidence/` confirms or refutes it — the newest artifacts there are the headless
  three-platform run. Confirming needs a GUI launch.
* **`evidence/ai-hardai-fixture.rep` "no longer loads against the current engine."** The file exists
  (3588 bytes). Confirming needs an engine launch, and the claim was written about a 4f327f5dc-era
  engine that has since moved twice.
* **The four runtime bug reports** — destroyed structures keeping old visuals, 1-2 s freezes with DXVK
  pipeline compilation as the leading hypothesis, AWOL players blocking victory, defeated observers
  never reaching the score screen, terrain draw-window fix not visually verified. No screenshot, cache
  measurement or freeze trace is committed. The one code-anchored part (`VictoryConditions.cpp:328` is
  asset-based) *is* confirmed; the observed behaviour is not.
* **VPS RAM (3.75 GB on an "8 GB Dedicated" plan), the live relay's state, and whether the deployed
  relay is running the chat build.** All need a remote connection. Commit `5292259c7` says the chat
  build has not been deployed; nothing measured today contradicts or confirms that. **Confirm before
  testing a client against it** — the check is in item 3 of "Open work". Also unconfirmed from here:
  that the live VPS matches the systemd/`/opt/gxrelay` layout `tools/relay/README.md:135-197`
  documents. The README is the deploy recipe, not an observation of the running box.
* **`/root/vpsbuild.sh` equals `scripts/build/linux/build-linux-relay.sh`.** Asserted in the redeploy
  recipe, never diffed. Machine-local, same shape as the `cb.bat` gap. If they have drifted, the
  Linux build runs with unknown flags. One line settles it: `diff /root/vpsbuild.sh
  scripts/build/linux/build-linux-relay.sh`.
* **`RTS_BUILD_OPTION_FFMPEG=OFF` "cannot work" with `SAGE_USE_OPENAL=ON` — not re-observed at HEAD,
  but not unwitnessed either.** The CMake is consistent with it —
  `Core/GameEngineDevice/CMakeLists.txt:305-309` compiles `FFmpegFile.cpp` when the option is OFF
  while `pkg_check_modules(FFMPEG REQUIRED ...)` only runs when it is ON (`:312`). And
  `scripts/build/linux/build-linux-relay.sh:13-18` records the failure concretely and
  contemporaneously: *"the build dies at the final link of `GeneralsXZH` with ~30 undefined
  `av_*`/`avcodec_*` symbols"*. Nobody has reconfigured that combination at HEAD, so treat it as
  reported-and-plausible rather than re-measured — but do **not** spend a VPS build cycle
  re-deriving it.
* **The bare `cp` into `/root/gamedata` works because the copied ELF resolves through the build-tree
  RPATH.** Inferred: nothing in the tree overrides CMake's default build-tree RPATH for `z_generals`
  (unbounded grep for `BUILD_RPATH|INSTALL_RPATH|SKIP_BUILD_RPATH|BUILD_WITH_INSTALL_RPATH|CMAKE_INSTALL_RPATH`
  hits only `Patches/SagePatch/CMakeLists.txt:74`), and `deploy-linux-zh.sh:156` has to patchelf
  `$ORIGIN` precisely because the build-tree RPATH would otherwise be baked in. **Practical
  consequence: do not delete `/root/GeneralsX/build/linux64-deploy`.** Check with
  `readelf -d /root/gamedata/GeneralsXZH | grep -E 'RPATH|RUNPATH'` and
  `ldd /root/gamedata/GeneralsXZH | grep -c 'not found'`.
* **`C:\dev\cb.bat` contents.** Machine-local, not in the tree. The `vcvars64.bat` requirement is
  consistent with `CMakePresets.json`'s `win64` `"architecture": {"value":"x64","strategy":"external"}`
  plus a Ninja Multi-Config generator — with `strategy: external` the caller must have set up the
  compiler environment, which is a complete mechanical explanation for the reported
  `Cannot open include file: 'time.h'` — but the batch file has not been read.
* **`relay-3platform-93017` is a three-*platform* result.** Three distinct endpoints did participate
  (`relay-3platform-93017-relay.txt`: `10.42.0.1` from the VPS-local host, `10.42.0.2` and `10.42.0.4`
  both from `185.115.100.15`, consistent with a Mac and a Windows box behind one NAT). But no
  Windows-side log is committed for that run, and since all three `.crc` files are byte-identical,
  nothing distinguishes the "windows" file from a copy — its provenance is the filename. For "Windows
  is measured", cite `relay-winhost-600` or `relay-win-vs-linux-600`, which carry a live Windows SimID.
* **`relay-played-match-14756` as three platforms.** Only two streams are committed; session 6's own
  addendum says that match's Windows peer was present but uncaptured.
* **What the three uncommitted `references/fbraz3-dxvk` edits do.** They exist
  (`git -C references/fbraz3-dxvk status --porcelain`), they are outside the SimID digest, and they
  are on the deployed macOS render path. Nobody read the diffs this session. Read them before running
  `git submodule update`, which would discard them.
* **Why `-lanframes` runs and game-over runs report the frame counter differently.** `d9ebe45cf` is
  the obvious suspect and was not checked against these specific evidence files.
* **Whether the networked freeze is relay-specific or a regression.** `xplat-lan-ai-1500` is an active
  networked AI match on `twilight flame`, but it was plain LAN (no relay) and built at `892738bc2`
  (rev 2131). Both differences from the frozen runs are untested. No committed networked result on
  `twilight flame` exists at HEAD.
* **Whether `relay-8peer`'s distinct = 3 is "no AI" or the freeze.** It ran on `killing fields`, which
  freezes networked. Re-running it on `alpine assault` with the same no-AI lobby would separate the
  two; nobody has.
* **Minor:** `relay-win-vs-linux-600-linux.meta.txt` ends at `[GXAI] afterScriptNewMap` with no
  completion line — the log capture was truncated, not the run (the `.crc` is a complete 600 contiguous
  frames). And `lanseed-solo-800` used `rev=2143` on mac vs `rev=2141` on linux; same `sourceID`
  (`AD022C38`), which is the field that gates compatibility, so the result stands, but "same build"
  would be an overstatement.

## Waiting on Karl, not on the next session

* **Push `68eff86c4` and `6377956d8`** — external action, and it blocks the Linux and Windows clones
  (including the VPS-only freeze reproduction, which runs the committed `relay-8peer.sh`).
* **Raise the VPS RAM ticket.** `MemTotal` 3.75 GB on a plan sold as "8 GB Dedicated",
  `virtio_balloon` bound, survived a clean reinstall. Memory reclaimed mid-match stalls the sim for
  every player. (**UNVERIFIED from here** — carried forward from an earlier session, no remote
  contact this session.)
* **The relay is a live service on `163.5.210.131` and people have played on it.** Assume someone is
  playing before restarting it or killing anything named `GeneralsXZH`. Restarting `gxrelay` drops
  every game in progress, which is why redeploying the relay needs an explicit OK and not just a
  "nobody's on right now".
