# DIAGNOSIS — "networked simulation freeze" (task 56)

Written 29/07/2026 at HEAD `6377956d8`. Read-only phase: **no source was edited, no game was run
by the author of this document.** Every claim below is labelled MEASURED (someone ran a command
and read the output) or INFERRED (someone read code and reasoned). Where a measurement was made by
another agent rather than by me, it says so and names what is missing to make it reproducible.

---

## BOTTOM LINE

**There is no "networked simulation freeze" in the engine.** The symptom is manufactured by two
independent, individually-boring defects that happen to interlock:

- **Defect A — the driver overfills the lobby.** `HeadlessMatch` never bounds the lobby by the
  map's player capacity, and it puts humans in the LOW slots and AI in the HIGH ones. On a
  2-start map with 2 human peers, both start positions go to headless humans (which by design
  issue no orders) and **every AI is assigned `startPos = -1`, gets no Command Center, and is
  inert for the rest of the match.** Solo runs put the first AI in slot 1, so exactly one AI
  always gets a base — which is why solo looked healthy.
- **Defect B — the activity metric is confounded.** "distinct CRC values" is satisfied by any
  moving object. Alpine Assault carries a scripted train that drives itself with no player and no
  AI. Alpine therefore read `1499/1500 distinct — ACTIVE` while being **exactly as AI-less as the
  maps that read FROZEN.** Alpine was never a working control.

Neither defect is a lockstep bug. Lockstep, transport and determinism were never broken here.
The three-platform corpus is not invalidated as *networking* evidence; a large part of it is
invalidated as *simulation* evidence.

**Confidence: HIGH on the mechanism, MEDIUM-HIGH on the corpus claim** — see
[§1.5 What is not yet nailed down](#15-what-is-not-yet-nailed-down). The single biggest gap is
that the decisive runs exist only as an agent's report and were never written to
`docs/WORKDIR/evidence/`. **Committing them is the highest-value next action in this whole
document.**

---

## 0. WHY THE FORMAL "SURVIVED" LIST IS EMPTY, AND WHY I AM NOT TREATING THAT AS THE ANSWER

Read this before §1 or the rest will look like it contradicts the process.

Five load-bearing hypotheses went to adversarial review. **All five were marked REFUTED.** Four of
the five refutations turn on one shared premise, stated most bluntly in the verdict on H1:

> "So ALPINE ASSAULT IS A 2-START, numPlayers=2 MAP, byte-for-byte in the same class as the two
> frozen maps. […] The mechanism is blind to the only variable that changed, so it cannot be the
> cause of the split."

That reasoning is *valid* and its map data is *correct* (I reproduced it myself — see §1.2). It
fails only because it takes the committed `distinct` column at face value as a measure of
simulation activity. The adversary had no machine access and could not check that assumption. The
empirical agent did check it, and measured it false.

So the refutations are **stale, not wrong**. They correctly killed the code-reading-only version
of the hypothesis, which genuinely could not explain alpine. The empirical work then supplied the
missing piece — alpine is not an exception, it is a bad measurement — and with that piece the
mechanism explains every row of the freeze table with nothing left over.

I am recording this as a judgment call, explicitly: **I am overriding an adversarial REFUTED on
the strength of measurements the adversary did not have.** If §1.5's outstanding items come back
the other way, this document is wrong and the adversary was right.

### Provenance of the evidence classes

| Class | Who | Reproducible today? |
|---|---|---|
| Source line references, greps | me, this session | Yes — commands in-line below |
| Map cache / map contents | me, this session (re-derived independently) | Yes — §1.2 |
| Committed CRC streams, `*.meta.txt` | prior sessions | Yes — in `docs/WORKDIR/evidence/` |
| **`net-bw` / `net-kf900` / `net-bw-noai` / `net-dm` / `net-tf` / alpine-900 object counts** | **empirical agent, this session** | **NO — never committed. See §1.5.** |

---

## 1. THE FREEZE

### 1.1 Defect A — the driver overfills the lobby and the surplus players get nothing

Every link below is MEASURED-BY-READING at HEAD `6377956d8` unless marked otherwise.

**A1. The headless driver has no capacity check.** The GUI LAN lobby refuses to start an
over-capacity game:

```
GeneralsMD/.../GUICallbacks/Menus/LanGameOptionsMenu.cpp:250-256
    const MapMetaData *md = TheMapCache->findMap( myGame->getMap() );
    if (!md || md->m_numPlayers < numUsers)      →  LAN:TooManyPlayers, no start
```

`HeadlessMatch::driveLobby` calls `TheLAN->RequestGameStart()` at
`GeneralsMD/.../Common/HeadlessMatch.cpp:705` with no equivalent. `publishGameOptions` already
holds the `MapMetaData*` at `HeadlessMatch.cpp:587` and reads `m_CRC` and `m_filesize` from it —
but not `m_numPlayers`. An unbounded grep of `HeadlessMatch.cpp` for
`m_numPlayers|adjustSlotsForMap|TooManyPlayers` returns exactly one hit, the `findMap` at :587.
The `adjustSlotsForMap()` that the GUI path calls (`LanGameOptionsMenu.cpp:882`,
`LanMapSelectMenu.cpp:473`) is never called from the headless path.

**A2. Humans get the low slots, AI get the high ones.**

```
HeadlessMatch.cpp:620   for (Int i = 1; i <= TheGlobalData->m_lanWaitPeers && i < MAX_SLOTS; ++i)   // SLOT_OPEN for peers
HeadlessMatch.cpp:629   Int nextSlot = 1 + TheGlobalData->m_lanWaitPeers;                            // AI start here
```

`m_lanWaitPeers` defaults to 0 (`GlobalData.cpp:1022`). `scripts/test/relay-8peer.sh:115-116`
launches the host with `-lanwait "$((PEERS - 1))"`. So:

- **solo** (`-lanwait 0`): slot 0 = host human, **slot 1 = first AI**
- **networked, PEERS=2**: slot 0 = host, slot 1 = joiner, **slots 2..6 = the five AI**

**A3. Start positions are capped at the map's count and handed out in slot-index order.**

```
GameLogic.cpp:880-885   Int numPlayers = MAX_SLOTS;
                        const MapMetaData *md = TheMapCache->findMap( game->getMap() );
                        if (md) numPlayers = md->m_numPlayers;
```

The live branch is the `#else` at `GameLogic.cpp:1014` (`#if 0` at :945, `#endif` at :1126 — I
checked the preprocessor boundaries; several earlier write-ups cited line numbers from the dead
`#if 0` twin at :963/:993-995, which is why two hypotheses quote *different* line numbers for the
same bug).

**A4. When positions run out, the slot is silently assigned `-1`.**

```
GameLogic.cpp:1070   Int farthestIndex = -1;
GameLogic.cpp:1072   for (posIdx = 0; posIdx < numPlayers; ++posIdx) { if (taken[posIdx]) continue; ... }
GameLogic.cpp:1101   DEBUG_ASSERTCRASH(farthestIndex >= 0, ("Couldn't find a farthest spot!"));   // ((void)0) in release
GameLogic.cpp:1102   slot->setStartPos(farthestIndex);        // stores -1
GameLogic.cpp:1103   taken[farthestIndex] = TRUE;             // taken[-1] — OOB write, see §4 row 7
```

`DEBUG_ASSERTCRASH` is `((void)0)` in release (`Core/GameEngine/Include/Common/Debug.h:206`), so
nothing is logged.

**A5. `startPos = -1` means no base and no units.**

```
GameLogic.cpp:1555   d.setInt(TheKey_multiplayerStartIndex, slot->getStartPos());
GameLogic.cpp:612    waypointName.format("Player_%d_Start", startPos+1);   // -1 → "Player_0_Start"
GameLogic.cpp:620    Waypoint *waypoint = findNamedWaypoint(waypointName); // null, no map defines it
GameLogic.cpp:621    DEBUG_ASSERTCRASH(waypoint, ...);                     // no-op in release
GameLogic.cpp:622-623  if (!waypoint) return;                              // no CC, no dozer, nothing
```

**A6. No Command Center means the skirmish AI never gets a build list.**

```
AISkirmishPlayer.cpp:970-985   scan all objects for this player's KINDOF_COMMANDCENTER
AISkirmishPlayer.cpp:986-989   if (!foundStart) { DEBUG_LOG("Couldn't find starting command center
                               for ai player."); return; }
```

The early return is above the code that relocates the build list to the player's base and marks
the CC entry `setInitiallyBuilt`. `AISkirmishPlayer::newMap`'s build loop then builds nothing.
`AIPlayer::update` (`AIPlayer.cpp:3032-3049`) runs every frame on a player that owns no object, no
builder and no factory, and issues no order — forever. Crucially, **the AI is fully constructed
and its scripts are attached**, which is exactly why every counter in the original evidence file
(`aiPlayers=5 computers=7 sides=11 sidesWithScripts=7`) looked identical between the working and
frozen runs. None of those counters counts start positions.

**A7. Why solo looked healthy.** Combining A2 and A3: solo puts the first AI in slot 1, so on a
2-start map it always gets position 2 and behaves normally. One active AI is enough to keep the
CRC changing every frame. Networked pushes every AI past the human slots, and headless humans
issue no orders (unbounded grep of `HeadlessMatch.cpp` for `appendMessage|MSG_DO_|createOrder`
returns nothing — the driver has no order path at all). Result: a genuinely empty world.

**Prediction this makes, and the measurement that matches it (MEASURED by the empirical agent, not
re-run by me):** spawned Command Centers should equal `min(occupied lobby slots, map numPlayers)`,
and activity should track it exactly.

| run | map | map numPlayers | CCs | distinct | verdict |
|---|---|---|---|---|---|
| net-bw | bitter winter | 2 | 2 | 4 / 300 | FROZEN |
| net-kf900 | killing fields | 2 | 2 | 4 / 900 | FROZEN |
| **net-bw-noai** (all 5 AI removed) | bitter winter | 2 | 2 | 3 / 300 | FROZEN — *identical* |
| net-dm | dark mountain | 4 | 4 | 300 / 300 | **ACTIVE, harness PASS** |
| net-tf | twilight flame | 8 | 7 | 300 / 300 | **ACTIVE, harness PASS** |

`net-bw-noai` is the cleanest single datum in the corpus: **deleting all five AI from the frozen
run changed nothing.** They were contributing literally zero. That is not "the AI is broken in a
network game"; that is "the AI was never in the game".

### 1.2 Defect B — Alpine Assault's train made a dead world read as ACTIVE

**B1. All three "repro" maps are 2-player maps, including alpine.** MEASURED by me, this session,
from `Maps\MapCache.ini` extracted out of `~/GeneralsX/GeneralsZH/MapsZH.big`:

| map | numPlayers | `Player_N_Start` entries | fileSize | fileCRC |
|---|---|---|---|---|
| alpine assault | **2** | 2 | 275491 | `0xDEA9E8E4` |
| bitter winter | **2** | 2 | 225694 | `0x6C90F128` |
| killing fields | **2** | 2 | 171691 | `0xCB1FCEA1` |
| dark mountain | 4 | 4 | 154660 | `0x19FED63C` |
| twilight flame | 8 | 8 | 400412 | `0xF0F7E3EB` |

The size/CRC columns match what the running game reported in
`evidence/networked-sim-freeze.meta.txt` (`alpine MC=DEA9E8E4 MS=275491`,
`bitter winter MC=6C90F128 MS=225694`), so the cache the game used is the cache I read.

**B2. Alpine — and only alpine — carries a self-driving train.** MEASURED by me, on the
refpack-decompressed `.map` bodies:

| map | `TrainEngine` tokens | `TrainTrack` tokens |
|---|---|---|
| Alpine Assault | **2** | 588 |
| Bitter Winter | 0 | 0 |
| Killing Fields | **0** | 36 (track scenery, no engine) |
| Twilight Flame | 0 | 0 |

**B3. The train moves with no player, no order and no AI.** `RailroadBehavior` is a
`PhysicsBehavior`/UpdateModule, not an `AIUpdate`
(`GeneralsMD/.../Include/GameLogic/Module/RailroadGuideAIUpdate.h:186`). Its constructor sets the
locomotive to `ACCELERATE` at spawn (`RailroadGuideAIUpdate.cpp:162`) and its `update()` returns
`UPDATE_SLEEP_NONE` unconditionally, integrating track distance every frame
(`RailroadGuideAIUpdate.cpp:669-856`). `GameLogic.cpp:4428-4437` hashes every object in `m_objList`
into the per-frame CRC and `Object::crc` hashes the transform as raw bytes
(`Object.cpp:4007`). **One moving train produces ~N distinct values in N frames with a completely
dead simulation.**

**B4. And that is exactly what happened.** MEASURED by the empirical agent (not re-run by me):
networked alpine assault, PEERS=2 AI=Hx5, 900 frames → **899 distinct CRCs, and object count
223 at frame 5, 223 at frame 450, 223 at frame 890.** Zero objects created or destroyed in 900
frames. Per-frame first-differing-object analysis names `id=223 TrainCabUngarrisonable` on
**296 of 296** consecutive frame pairs. The solo control on the same map and AI spec: 224 → 226
objects, with `Nuke_ChinaBarracks (id=225)` built by frame 5.

So `xplat3-5ai-1500` (alpine, 1500 frames, 1499 distinct, "ACTIVE") and
`relay-8peer` RUN 4 (alpine, 1499/1500, "ACTIVE") were **measuring a train**. Alpine is not the
exception that refutes Defect A; alpine is Defect A with a decoration on top.

### 1.3 The two defects together explain every row, with nothing left over

| observation | explanation |
|---|---|
| solo ACTIVE on every map, every AI count | slot 1 is always an AI → exactly one AI gets a base (A2/A7) |
| networked FROZEN on bitter winter, killing fields | 2 starts, both consumed by humans → zero AI have a base (A2-A6) |
| networked "ACTIVE" on alpine | same as above; the train carries the CRC (B1-B4) |
| networked ACTIVE on twilight flame / dark mountain | capacity ≥ occupied slots → the AI get real positions |
| all peers hold the *same* frozen value; no desync | every step is a deterministic function of map + shared `GameInfo`; all peers compute the identical wrong answer |
| four frames of activity, then constant | initial object placement and settle, then nothing left that moves |
| one lone change at ~frame 153 | UNKNOWN — see §1.5. Not explained, and I will not invent one. |
| "AI count ruled out — Hx3/Hx5/Hx7 all 600/600" | a null control: on a 2-start map only ever *one* AI mattered, so varying the count varied nothing (see §1.4 prediction P4) |

### 1.4 Predictions — including ones nobody has checked

A mechanism that only explains the past is a story. These are the things it forbids.

**P1 (free, no rebuild, decisive on the precondition).** The binary already prints the number.
`GameLogic.cpp:1047` emits `[GXPOS] slot=%d numPlayers=%d seedCRC=%u (before random pick)` once
per match (it is inside `if (!hasStartSpotBeenPicked)`, so exactly one line). Grep any frozen
run's `peer0.err`. **Prediction: `numPlayers=2` with 7 occupied slots.** `numPlayers=8` on bitter
winter kills the whole diagnosis and points instead at a map-cache lookup failure.

**P2 (free).** Set `GX_TRACE_LO=0 GX_TRACE_HI=0` (`GameLogic.cpp:4413-4423`) on a frozen repro and
count `[GXOBJ]` lines by owner. **Prediction: exactly `min(occupied, numPlayers)` players own a
Command Center, and they are the lowest-numbered occupied slots.**

**P3 (one 2-minute run — NOT YET RUN).** `PEERS=2 FRAMES=300 AI= (EMPTY)
MAP='maps\alpine assault\alpine assault.map' ./scripts/test/relay-8peer.sh` — alpine with **no AI
at all**. **Prediction: still ~299/300 distinct.** If it collapses to ~3 like the killing-fields
idle run, Defect B is dead and alpine's activity really was the AI — which would break the whole
account. This is the cheapest single run that can falsify this document.

**P4 (one run — NOT YET RUN, and the sharpest untested consequence).** Solo bitter winter,
same seed, `Hx3` vs `Hx7`. Under this diagnosis only ONE AI ever has a base on a 2-start map
regardless of how many are in the lobby, so the two `[GXCRC]` streams should be **identical or
near-identical**, not merely "both 600 distinct". If they differ substantially, extra AI *are*
contributing solo and A2/A7 is wrong. This also directly re-tests the original evidence file's
"AI count ruled out" line, which this diagnosis claims was a null control.

**P5 (already satisfied, per the empirical agent).** Any networked run on a map with
`numPlayers ≥ occupied slots` is ACTIVE. dark mountain (4) and twilight flame (8) both came back
300/300 PASS.

**P6.** Add a third human peer to twilight flame at `PEERS=8 AI=Hx1` → predict 8 CCs. Not run.

### 1.5 What is not yet nailed down

Stated plainly rather than papered over.

1. **The decisive runs are not committed.** `net-bw`, `net-kf900`, `net-bw-noai`, `net-dm`,
   `net-tf` and the alpine-900 object-count analysis exist only as an agent's report in a
   transcript. There is no `.crc`, no `.meta.txt`, no `[GXOBJ]` dump for any of them in
   `docs/WORKDIR/evidence/`. **Until they are committed this diagnosis rests on an
   unverifiable claim, which is precisely the failure mode task 55 exists to punish.** Commit
   them before acting on §3.
2. **The lone state change at ~frame 153** in the bitter-winter stream is unexplained. Two
   Command Centers and two dozers exist in that world (per the CC-count measurement), so *some*
   deterministic one-shot is plausible, but I have not identified it and will not guess.
3. **`f=` was never awk'd on an actual frozen stream.** The frame column is strictly consecutive
   with a pinned value column on the three committed idle streams (`xplat-lan-3000-*`,
   `xplat-inet-*-60-*`, `relay-8peer-idle-1500-*`), which is what "world stopped, frames didn't"
   should look like — but the frozen runs' own `.crc` files were never saved, so nobody has
   checked. See item 1.
4. **The `[GXAI]` counter line's provenance.** `aiPlayers=5 computers=7 sides=11
   sidesWithScripts=7 skirmishSides=14` is quoted as "identical in both cases", and it is what
   rules out the `forceHuman` and `setPlayerType(PLAYER_HUMAN)` hypotheses (§2 rows 5-6). The
   only `[GXAI]` line committed anywhere in the tree is
   `evidence/ai-desync-mac.txt:2139`, which reads `aiPlayers=1` and belongs to a different
   investigation. If the quoted counters did NOT come from a frozen run, rows 5-6 of §2 reopen.
5. **The seed was never pinned on the networked arm.** MEASURED by me: `grep -n lanseed
   scripts/test/relay-8peer.sh` exits 1 — the flag appears nowhere in the script. So every
   networked run seeded from `GetTickCount` and map-vs-seed were confounded in the original
   matrix. This does not damage the diagnosis (the capacity sweep in P5 holds the harness
   constant and varies only the map), but it does mean the original file's "not the seed" line
   was not established by the control it cites.

### 1.6 If you only run one thing

**P3** (alpine, networked, zero AI, 300 frames, ~2 minutes). It is the one run that can kill this
diagnosis outright, and it is the run that decides whether every "alpine + networked" row in the
corpus needs re-labelling. Follow it with **P4** (solo Hx3 vs Hx7 stream comparison), which costs
two more solo runs and re-tests the original evidence file's own control.

---

## 2. RULED OUT — do not re-tread

| # | Hypothesis | Killed by | Class |
|---|---|---|---|
| 1 | The whole `GameLogic::update` is gated in a network game (`canUpdateNetworkGameLogic` / `isFrameDataReady` / `isStalling` / `isTimeFrozen` / pause latch / logic-time-scale) | Every such gate wraps the entire `TheGameLogic->UPDATE()` call (`GameEngine.cpp:1216-1219`), and `[GXCRC]` (`GameLogic.cpp:3965`) and the sole `m_frame++` (`GameLogic.cpp:4138`) are both inside it — a gate would freeze the frame *number*, not just the value. Frame numbers advance. Also: `isTimeFrozen` returns false unconditionally when `TheNetwork != nullptr` (`GameEngine.cpp:338-339`) and `isGamePaused` is only consulted when `TheNetwork == nullptr` (`GameEngine.cpp:359-370`) — both gates are *weaker* networked. | MEASURED-BY-READING + MEASURED on committed streams. Caveat: §1.5 item 3. |
| 2 | An explicit network/multiplayer/local-player branch inside the AI order path | Unbounded grep over `GameLogic/AI/` for `TheNetwork\|isLocalPlayer\|getLocalPlayer\|isInMultiplayerGame\|isInInternetGame\|isInLanGame\|m_isLocal\|getGameMode\|TheRecorder\|TheGameInfo\|headless` returns **zero hits** (exit 1). | MEASURED. **Nuance:** `Player::update` *is* conditional — `Player.cpp:670-672` is `if (m_ai) m_ai->update();`. The gate is `m_ai == nullptr`, set upstream, not a network test in the AI. |
| 3 | The map (as an intrinsic property) | Every standard map is ACTIVE solo at every AI count (`networked-sim-freeze.meta.txt`). What varies is the map's *capacity relative to the lobby*, not the map. | MEASURED (prior session) |
| 4 | The seed | Solo at the frozen run's own seed (900977) → 300/300 ACTIVE. | MEASURED (prior session), **but the control is weak** — see §1.5 item 5. Superseded rather than load-bearing: the capacity sweep explains everything without invoking the seed. |
| 5 | `forceHuman` silently converts every AI to a brainless human (`Player.cpp:806-826` → `setPlayerType` deletes `m_ai` at `Player.cpp:742-743`) | `aiPlayers=5` in the frozen run — `isSkirmishAIPlayer()` requires `m_ai` to exist and be a skirmish brain (`GameLogic.cpp:1675-1682`). If `m_ai` had been deleted the count would be lower. | MEASURED (prior session), **conditional on §1.5 item 4** |
| 6 | `PlayerList::newGame`'s `!setLocal` fallback flips a player to `PLAYER_HUMAN` and deletes its brain (`PlayerList.cpp:182-195`) | Same counter as row 5. | Same caveat as row 5 |
| 7 | AI scripts never attach / `SkirmishScripts.scb` fails to load | `sidesWithScripts=7 skirmishSides=14`, identical both ways. | MEASURED (prior session), same caveat |
| 8 | The `oldFactionsOnly` skip (`GameLogic.cpp:2197-2199`) — the `continue` that produces the same CC-less player | Gated on `isInInternetGame()`, i.e. `m_gameMode == GAME_INTERNET` (`GameLogic.h:519`). The headless LAN/direct-connect path is `GAME_LAN`. Provably inert here. **Keep on file:** session 7 wired "Online" to a real WOL lobby, and the in-code comment records that this exact branch once caused "skirmish games would get no command centers upon start". | INFERRED (code read) |
| 9 | Map-cache lookup failure at match start → `numPlayers` falls back to `MAX_SLOTS` | Would print `Could not find map "%s"` (`GameLogic.cpp:887`), and would produce MORE positions, not fewer. The measured CC counts equal the cache's `numPlayers` exactly on four maps. Falsified by P1/P2 in either direction. | INFERRED + MEASURED (CC counts) |
| 10 | A regression landed between rev 2171 and rev 2187 (the adversary's replacement theory for Defect B) | Twilight flame is ACTIVE **at HEAD** (`net-tf`, 300/300, PASS) — the same map that was active at revs 2131-2171. No regression window. | MEASURED by the empirical agent — subject to §1.5 item 1 |
| 11 | `map.ini` presence as the per-map discriminator | Only bitter winter ships one; alpine and killing fields both lack one, yet they land on opposite sides of the split. | MEASURED (adversary) |
| 12 | `ScriptEngine::m_endGameTimer` stuck at exactly 0 (`ScriptEngine.cpp:5549-5568`) | No evidence anywhere records the timer, and a dead script engine would not stop `AIPlayer::update`, which runs from `AI::update`, not the script engine. Not the cause; a latent absorbing state worth a guard. | INFERRED |
| 13 | `taken[-1]` OOB corrupting the sim | A layout-dependent OOB write executed by three different toolchains would produce **divergent** state. Measured: all peers agree exactly. Real defect, not this symptom. | INFERRED + MEASURED (no desync) |

---

## 3. THE PROPOSED FIX — described, **not applied**

Staged deliberately so that the zero-risk work lands first and the simulation is touched last, if
at all.

### Stage 1 — measurement integrity (no engine change, no CRC risk). Do this first.

Nothing else should be believed until the metric is fixed, because the metric is what produced the
false alpine control in the first place.

1. **Replace "distinct CRC values" as the activity gate.** It is satisfied by any ambient mover.
   Gate instead on something a train cannot fake:
   - **object create/destroy count over the run** (from the `[GXOBJ]` trace), and
   - **spawned-player count vs occupied lobby slots** — die unless they match.
   Keep the distinct count as a reported number; stop using it as a verdict.
2. **Assert the lobby was actually exercised.** `xplat-3platform-lobby.sh:310` computes
   `AI_PLACED` and never reads it, while :326 prints the *requested* `$AI` in the PASS line.
   Parse the `S=` field of `game options:` (`HeadlessMatch.cpp:670-673`), count `CE`/`CM`/`CH`,
   and fail unless it matches.
3. **Add a frame floor.** `xplat-3platform-lobby.sh:280` truncates to the shortest stream with no
   floor; `relay-8peer.sh:199` computes `DISTINCT0` over the *full* peer0 stream but thresholds it
   against `SHORTEST/2`. Both are in §4.
4. **Commit the §1.5 item-1 artifacts.**

**Blast radius:** test scripts only. Zero effect on any binary, any baseline, any replay.
**What could go wrong:** a stricter gate will retroactively re-classify committed rows —
`xplat3-5ai-1500`, `relay-8peer` RUN 4 and every other alpine networked row become
INCONCLUSIVE-as-simulation-evidence (they remain valid *networking* evidence). That is the correct
outcome, not a regression, but it must be announced rather than discovered.

### Stage 2 — refuse the impossible lobby (driver only, still no simulation change)

Add to `HeadlessMatch::publishGameOptions` (`HeadlessMatch.cpp`, next to the existing `findMap` at
:587) the check the GUI already performs at `LanGameOptionsMenu.cpp:250-256`: count occupied
non-observer slots, compare against `md->m_numPlayers`, and **refuse to start with an explicit log
line** naming both numbers.

**Blast radius:** headless matches only. A legal match — one the GUI would also have started —
executes byte-identically, so **no CRC baseline is affected.**

**What could go wrong, and it is not nothing:**
- It makes today's default harness invocations *fail rather than lie*: `relay-8peer.sh` defaults
  to `MAP='maps\killing fields\killing fields.map'` (2 players) at `PEERS=8`
  (`relay-8peer.sh:39,41`), and `xplat-3platform-lobby.sh` defaults to alpine with
  `-lanwait 2 -lanai Hx5`. **Both must have their default maps changed to a high-capacity map in
  the same commit** (twilight flame / death valley / destruction station, all `numPlayers=8`) or
  the whole suite goes red.
- The adversary's objection stands and should be honoured: a *clamp* or hard refusal deletes the
  ability to reproduce historical runs. Provide an explicit `-lanoverfill` escape hatch that
  restores today's behaviour, so old configurations remain reproducible for archaeology while the
  default is safe.
- **Do not "fix" this by clamping the AI count silently.** A silent clamp reintroduces exactly the
  class of bug being fixed: the run would report five AI and play with two.

### Stage 3 — engine hardening (TOUCHES SIMULATION — highest bar, do last, maybe never)

`GameLogic.cpp:1101-1103` uses `farthestIndex` without checking the `-1` it was initialised to at
:1070, and `GameLogic.cpp:1110/1121-1122` initialises `closestIdx = 0` with no sentinel, so the
teamed branch silently assigns a **duplicate** position instead of `-1`. There is also a live
`taken[-1] = TRUE` OOB write at :1103.

**The only safe form of this fix is a guard that refuses/logs — never one that reassigns.**

- **Safe:** `if (farthestIndex < 0) { log loudly; leave startPos as-is; }` plus removing the
  `taken[farthestIndex]` write when the index is negative. In a lobby that passed Stage 2 this
  branch is unreachable, so **no legal match changes behaviour and no baseline moves.**
- **UNSAFE — do not do this:** wrapping to position 0, reusing a taken position, or "sensibly"
  distributing surplus players. Any of those changes which units appear where, which changes the
  frame-0 world CRC, which invalidates **every `.crc` in `docs/WORKDIR/evidence/`** — 32 files
  including the 93,017-frame three-platform stream and the 50×1500 soak. Worse, this engine
  defaults `RETAIL_COMPATIBLE_CRC` to 1 (`Core/GameEngine/Include/Common/GameDefines.h:86-87`),
  so start-position assignment is part of retail replay compatibility, not just this project's
  internal baselines. A change here silently breaks retail 1.04 replays.
- **Sequencing matters:** land Stage 2 first. Then Stage 3 is provably a no-op on every legal
  match and can be verified as such by re-running one soak and diffing against the existing
  baseline byte-for-byte. Landing Stage 3 first means you cannot tell a real behaviour change from
  the intended one.

### Stage 4 — optional, and only if someone wants the surplus players to *work*

Making an over-capacity lobby actually playable (wrapping start positions, sharing spots) is a
**design change to the simulation**, not a bug fix. It changes match outcomes, breaks retail
compatibility, and invalidates every baseline. The GUI's answer to this configuration is "refuse",
and matching the GUI is the correct scope. **Recommendation: do not do Stage 4.**

---

## 4. THE OTHER BUGS

Ranked by (damage to conclusions) × (likelihood already firing). "Real?" means: does the defect
exist in the code as described.

| # | Bug | Real? | file:line | Smallest fix | Blast radius |
|---|---|---|---|---|---|
| 1 | **"distinct CRC values" measures ambient map furniture, not simulation.** One scripted train on alpine yields 1499/1500 with a dead world. | **YES** — MEASURED (train tokens by me; object-count invariance by empirical agent) | `scripts/test/relay-8peer.sh:199-205`; `xplat-3platform-lobby.sh:321`; `GameLogic.cpp:4428-4437` | Gate on object create/destroy and on spawned-player count instead | Test scripts; re-labels a large part of the committed corpus |
| 2 | **`HeadlessMatch` never bounds the lobby by map capacity** (Defect A's driver half). Starts games no GUI can produce. | **YES** — MEASURED (unbounded grep: one `findMap` hit, no `m_numPlayers`, no `adjustSlotsForMap`) | `HeadlessMatch.cpp:587,620,629,705`; guard exists at `LanGameOptionsMenu.cpp:250-256` | Mirror the GUI check; refuse with a named log line | Headless only; needs harness default maps changed in the same commit |
| 3 | **`relay-8peer.sh` idle gate uses two different denominators.** `DISTINCT0` over the full peer0 stream, threshold `SHORTEST/2`. Can print the impossible line `distinct (peer0): 1500 of 3` and exit PASS. | **YES** — MEASURED by me (:199 vs :201) | `scripts/test/relay-8peer.sh:199,201,211` | `head -n "$SHORTEST"` before the `sort -u` | One script; may re-classify past `relay-8peer` rows |
| 4 | **Every multi-peer harness truncates to the shortest stream with no floor.** A peer that dies at frame 3 silently redefines the run as a 3-frame experiment and prints PASS. | **YES** — MEASURED (verdict logic replayed against synthetic 1500/1500/3 streams → `PASS: 3 frames`, exit 0) | `xplat-3platform-lobby.sh:280,283,301,321`; `xplat-3platform-soak.sh:151,157`; `xplat-determinism-soak.sh:170` | `[ "$N" -ge "$FRAMES" ]` (or a 90% floor) before the PASS branch | Test scripts |
| 5 | **The three soak harnesses disagree about whether an idle sim is a failure.** `xplat-determinism-soak.sh` only WARNS, still counts the frames, still can PASS. `xplat-lan-soak.sh` has no idle check at all — and it produced `xplat-lan-ai-1500`, cited as *the* macOS↔Windows determinism proof. | **YES** — code read | `xplat-determinism-soak.sh:179,186-190,209`; `xplat-lan-soak.sh` (no distinct computation) | Make the warn increment `FAILED`; add a gate to `xplat-lan-soak.sh` | Test scripts |
| 6 | **`AI_PLACED` computed and thrown away; PASS line asserts the *requested* AI count as observed.** A soak that silently ran human-only reports "+ Hx5 AI". | **YES** — MEASURED (assigned once at :310, never referenced) | `xplat-3platform-lobby.sh:310,326` | Parse `S=` from `game options:` (`HeadlessMatch.cpp:670-673`), count `CE/CM/CH`, die on mismatch | One script |
| 7 | **`taken[-1] = TRUE`** — OOB stack write, once per starved slot, three toolchains, on the exact line that starves the AI. Siblings: `closestIdx = 0` silently duplicates position 0 (`:1110,1121-1122`); `if (posIdx >= 0 \|\| posIdx >= numPlayers)` at `:938` is almost certainly meant to be `&&`. | **YES** — code read; runtime effect UNKNOWN (stack layout is a codegen choice, not an ABI guarantee; empirically benign here since no desync was observed) | `GameLogic.cpp:1101-1103,1110,1121-1122,938` | Guard on `farthestIndex < 0` and skip the write; do **not** reassign | **Simulation-adjacent** — see §3 Stage 3. Guard-only is a no-op on legal matches |
| 8 | **`-lanseed` is the only lan flag with no validation, and `atoi`'s failure value (0) is also its "unset" sentinel.** `-lanseed garbage` silently un-pins the experiment with no log line. Siblings all validate and `exit(1)`. | **YES** — MEASURED (`parseLanSeed` at `CommandLine.cpp:619-629` vs `parseLanWait` at `:587-599`) | `CommandLine.cpp:619-629`; `HeadlessMatch.cpp:662-666` | Validate like the siblings; separate `m_lanSeedSet` flag so 0 is a legal seed | CLI parse only. Harness-side stopgap: assert `seed forced to $SEED` in the log |
| 9 | **Audio configuration is a lockstep input.** With `RETAIL_COMPATIBLE_CRC=1`, `GetGameLogicRandomValueUnchanged` advances the shared seed; `AudioEventRTS::generateFilename` calls it; `AudioManager::addAudioEvent`'s `isOn()` early-outs return *before* that, with no logical-audio carve-out (unlike the adjacent `notForLocal` path, which has one plus a compensating re-check). A server with no audio device calls `setOn(false, AudioAffect_All)`. | **YES** — code read, chain verified end to end. NOT observed firing; 93k frames did not trip it | `GameAudio.cpp:412-432,439-466`; `RandomValue.cpp:325-328`; `AudioEventRTS.cpp:341-343,390-391,408-409`; `GameDefines.h:86-87`; `OpenALAudioManager.cpp:1546,1554,1560` | Give the `isOn()` returns the same logical-audio carve-out the `notForLocal` return already has | **Simulation.** Any change here moves the RNG stream and invalidates baselines. Diagnose first with a per-frame RNG call counter |
| 10 | **`SDL3GameEngine::createAudioManager` ignores `dummy`.** Windows honours it (`MilesAudioManagerDummy`); Mac/Linux always get `OpenALAudioManager`. The entire three-platform corpus was collected with two different audio subsystems. Compounds row 9. | **YES** — MEASURED (`(void)dummy;` then unconditional `new OpenALAudioManager()`) | `SDL3GameEngine.cpp:1370-1383`; cf. `Win32GameEngine.h:112-117` | Honour `dummy`, matching Win32 — one line | Removes an uncontrolled variable from the determinism experiment; may change audio behaviour in non-headless SDL3 builds. Task 38 is **not** cosmetic |
| 11 | **Windows `0xC0000005` at teardown (task 36).** `WinMain` *returns* (`:958`) after `shutdownMemoryManager()` (`:941`), so the CRT then runs `ObjectPoolClass` static dtors against freed pool memory. macOS already dodges this exact class with `_exit()` (`SDL3Main.cpp:812-818`). The fault is outside `WinMain`'s `try/catch`, and `MiniDumper::shutdownMiniDumper()` at `:952` has already run — which is why four sessions produced no crash dump. | **Real defect; attribution INFERRED** (no Windows access, not measured) | `WinMain.cpp:941,952,958`; `SDL3Main.cpp:812-818`; `mempool.h:145,156,200-213` | `ExitProcess(exitcode);` immediately before `WinMain.cpp:958`, mirroring SDL3Main | Windows exit path only. **Free decisive test:** if a `Crash*.dmp` exists after a 0xC0000005 run, the fault is *inside* the try block and this attribution is wrong |
| 12 | **`GameMain` deletes `TheFramePacer` before `TheGameEngine`**, and `2e226bf3a` added an unguarded `TheFramePacer->enableLogicTimeScale(FALSE)` on a teardown-reachable path. Reachability after the delete is **not established**. Same shape: `GameEngine.cpp:292` unguarded `TheGameResultsQueue->endThreads()` in a destructor whose subsystem is created inside a `try`-wrapped `init()`. | Ordering defect: YES. Reachability: **UNKNOWN** | `GameMain.cpp:67-69`; `GameLogicDispatch.cpp:310`; `GameEngine.cpp:292` | Swap the two deletes so the pacer outlives the engine; add null guards | Shutdown only. Not the cause of row 11 — the crash predates `2e226bf3a` |
| 13 | **AWOL players block victory (task 34).** `hasSinglePlayerBeenDefeated` tests only `hasAnyObjects`/`hasAnyUnits`/`hasAnyBuildings` and never consults connection state. | **YES** — code read, confirmed unfixed at HEAD | `VictoryConditions.cpp:328` | Add a connection-state term | **Simulation.** `VictoryConditions::update` calls `killPlayer`, so any fix mutates state all peers must agree on. Must use replicated state, never local connection state |
| 14 | **`Generals/` (base game) has no headless LAN driver (task 39).** `-headless` exists (`:1150`, `parseHeadless` at `:416`); zero hits for `lanhost`/`lanjoin`/`lanai`/`lanseed`. | **YES** — MEASURED (unbounded grep) | `Generals/.../CommandLine.cpp:416,1150` | Port the `HeadlessMatch` flags | New surface; no effect on ZH |
| 15 | **Uncommitted DXVK edits are outside the SimID digest.** Three working-tree changes in `references/fbraz3-dxvk` (MoltenVK dlopen paths ×2, `SDL_GetWindowSizeInPixels`) build the `libdxvk_d3d8.dylib` that carried every macOS result; they exist nowhere in git and a `git submodule update` destroys them. **They are cleared as suspects for tasks 49/50** — none touches texture/material/draw or pipeline/shader-cache state. | **YES** — MEASURED (`git diff` in the submodule, all 13 changed lines read) | `references/fbraz3-dxvk/src/vulkan/vulkan_loader.cpp`, `.../wsi/sdl3/wsi_platform_sdl3_funcs.h`, `.../wsi/sdl3/wsi_window_sdl3.cpp` | Commit them, or prove they are not load-bearing by stashing and rebuilding | Reproducibility of the macOS render path. Same class as the missing `C:\dev\cb.bat` (task 54a) |

### 4.1 CALLOUT — defects that make a broken run look like a passing one

These are the ones that cost this project a week, and they are structurally the same bug repeated
in five places: **a verdict computed over a quantity other than the one being claimed.**

- **Row 1** — a dead simulation reads ACTIVE because a train moves. This is what produced the
  false alpine control and, through it, the entire "networked simulation freeze" framing.
- **Row 3** — `distinct (peer0): 1500 of 3` and exit 0.
- **Row 4** — a peer that dies at frame 3 turns a 1500-frame experiment into a 3-frame one, and
  the script says PASS.
- **Row 5** — one soak warns where three siblings fail; one has no idle gate at all, and it is the
  one that produced the headline determinism proof.
- **Row 6** — the PASS line asserts the AI count that was *asked for*, never the one observed.

**Common fix pattern:** every verdict line must be computed from the same denominator it prints,
and every claim in a PASS message must be derived from an observation in the log, never from an
input parameter. A cheap enforcement: make each harness print the tuple it gated on
(`frames_requested / frames_compared / distinct / objects_created / ai_observed`) and refuse to
print PASS if any element is absent.

---

## APPENDIX — commands used for the measurements attributed to me

```bash
# map capacity and start-waypoint counts, from the shipped cache inside MapsZH.big
#   (BIG extraction → Maps\MapCache.ini → parse numPlayers + Player_N_Start per entry)
#   alpine 2/2, bitter winter 2/2, killing fields 2/2, dark mountain 4/4, twilight flame 8/8

# train content, on the refpack-decompressed map bodies
strings -a "Alpine Assault.map.bin"  | grep -c TrainEngine   # 2
strings -a "Bitter Winter.map.bin"   | grep -c TrainEngine   # 0
strings -a "Killing Fields.map.bin"  | grep -c TrainEngine   # 0   (36 TrainTrack, scenery only)
strings -a "Twilight Flame.map.bin"  | grep -c TrainEngine   # 0

# preprocessor boundaries — the LIVE branch is the #else
awk 'NR>=876 && NR<=1135 && (/^#if/||/^#else/||/^#endif/) {print NR": "$0}' \
  GeneralsMD/Code/GameEngine/Source/GameLogic/System/GameLogic.cpp
#   945: #if 0   1014: #else   1126: #endif

# the networked arm never pinned a seed
grep -n lanseed scripts/test/relay-8peer.sh ; echo $?   # exit 1, no matches
```

Note on `grep -c`: it exits 0 when it finds matches, so none of the above gates a subsequent
command on its exit status. Absence claims above were made with unbounded greps — no `head -N`.
