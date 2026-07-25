# Game Logic Architecture

Zero Hour's simulation subsystem: the deterministic frame loop, the `Object` /
module system, the AI state machines, and the input path that turns a mouse click
into an order.

Everything cited below is `GeneralsMD/` (the Zero Hour build target, `z_generals`)
unless the path says `Core/`. Line numbers are from the tree at the time of writing;
treat them as bookmarks, not contracts. Where the code is genuinely unclear I say so
rather than guessing.

---

## 1. How it fits together

Two "halves" of the engine, one process, one thread:

- **GameLogic** — the deterministic simulation. Owns `Object`s. Advances in fixed
  30 Hz *logic frames*. Every machine in a network game must produce bit-identical
  results, which is why almost everything in here is integer-or-fixed-step and why
  there is a per-frame CRC.
- **GameClient** — rendering, input, UI, audio, particles. Owns `Drawable`s. Runs
  once per *render frame*, which may be faster than the logic rate.

They communicate in exactly two directions:

```
GameEngine::execute()                        GameEngine.cpp:1107 — the one main loop
  |
  +-> GameEngine::update()                                        GameEngine.cpp:1058
  |     |
  |     +-> TheGameClient->UPDATE()      input + UI + DRAW of the PREVIOUS logic frame
  |     |
  |     +-> TheMessageStream->propagateMessages()   translator chain; survivors land
  |     |                                           on TheCommandList
  |     |
  |     +-> canUpdateGameLogic()? --no--> (skip the simulation this render frame)
  |     |         |
  |     |        yes
  |     |         v
  |     +-> TheGameLogic->UPDATE()                                 GameLogic.cpp:3734
  |               drains TheCommandList  -> logicMessageDispatcher -> AIGroup -> AIUpdate
  |               runs the sleepy update-module heap
  |               Object state mutated -> Drawable transform (Object::reactToTransformChange)
  |               m_frame++
  |
  +-> TheFramePacer->update()                       sleep/spin to the render FPS cap
```

- **Client → Logic**: `GameMessage`s. The client never touches an `Object` to give
  it an order; it posts a message, the message survives a translator chain, and
  `GameLogic::logicMessageDispatcher()` executes it on the logic side.
- **Logic → Client**: direct pointer calls. `Object` owns a `Drawable*` and pushes
  its transform straight into it (`Object.cpp:1858`). `GameLogic` even *creates* the
  drawable (`GameLogic.cpp:4405`). This is not a clean boundary and never was.

The five files you will spend all your time in:

| File | What lives there |
|---|---|
| `GeneralsMD/Code/GameEngine/Source/GameLogic/System/GameLogic.cpp` | the frame, object registry, sleepy-update scheduler |
| `Core/GameEngine/Source/GameLogic/System/GameLogicDispatch.cpp` | one `on*()` handler per network message |
| `GeneralsMD/Code/GameEngine/Source/GameLogic/Object/Object.cpp` | `Object` construction, module wiring, death |
| `GeneralsMD/Code/GameEngine/Source/GameLogic/Object/Update/AIUpdate.cpp` | the base AI update + all `aiDoCommand` plumbing |
| `GeneralsMD/Code/GameEngine/Source/GameClient/MessageStream/CommandXlat.cpp` | click/hotkey → `GameMessage` (5.5k lines, the ugly one) |

---

## 2. The frame loop and the frame pacer

### 2.1 `GameEngine::execute()`

`GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp:1107`. A plain
`while (!m_quitting)` that calls `update()` (wrapped in a `try`/`catch(...)` that
turns any escaped exception into a `RELEASE_CRASH`) and then
`TheFramePacer->update()` (`GameEngine.cpp:1177`).

Note the comment at `GameEngine.cpp:1179-1181`: **`TheDisplay->draw()` is not called
here.** Rendering is dispatched from inside `GameClient::update()`
(`GameClient.cpp:776`). Adding a draw call to the main loop double-presents.

### 2.2 `GameEngine::update()`

`GameEngine.cpp:1058`. Order matters:

1. `TheRadar`, `TheAudio`, `TheGameClient->UPDATE()` — the client update *includes*
   the draw of the state produced by the **previous** logic frame.
2. `TheMessageStream->propagateMessages()` (`GameEngine.cpp:1074`).
3. `TheNetwork->UPDATE()` if networked.
4. `canUpdateGameLogic()` decides whether the simulation advances this render frame.
5. If yes: `TheGameClient->step()` then `TheGameLogic->UPDATE()`
   (`GameEngine.cpp:1088-1089`).
6. If the logic is blocked but the game is not halted, `TheScriptEngine->UPDATE()`
   runs on its own (`GameEngine.cpp:1095`) so scripted camera moves still play
   during frozen time. In the normal path the script engine is updated from *inside*
   `GameLogic::update()` (`GameLogic.cpp:3788`) — the two are mutually exclusive, so
   scripts tick exactly once per iteration either way.

`GameClient::step()` → `Display::step()` → `View::stepView()`. Today the only thing
that uses it is the camera shake (`Core/GameEngineDevice/Source/W3DDevice/GameClient/W3DView.cpp:1351`),
deliberately kept on the fixed step so it snaps rather than interpolates.

### 2.3 `FramePacer` — the logic/render decoupling

`Core/GameEngine/Include/Common/FramePacer.h`, `Core/GameEngine/Source/Common/FramePacer.cpp`.

Constants: `BaseFps = 30` and `LOGICFRAMES_PER_SECOND = WWSyncPerSecond = 30`
(`Core/GameEngine/Include/Common/GameCommon.h:67-74`,
`Core/Libraries/Source/WWVegas/WWLib/WWCommon.h:70`). Thirty is baked into the
simulation — `SECONDS_PER_LOGICFRAME_REAL`, all the
`ConvertVelocityInSecsToFrames()`-style helpers, and every INI value expressed in
frames. It is not a tunable.

The gate is `GameEngine::canUpdateRegularGameLogic()` (`GameEngine.cpp:937`):

```cpp
if (useFastMode || !enabled || logicTimeScaleFps >= maxRenderFps)
    return true;                              // one logic tick per render frame
else {
    const Real targetFrameTime = 1.0f / logicTimeScaleFps;
    m_logicTimeAccumulator += min(TheFramePacer->getUpdateTime(), targetFrameTime);
    if (m_logicTimeAccumulator >= targetFrameTime) {
        m_logicTimeAccumulator -= targetFrameTime;
        return true;
    }
}
return false;
```

Read that carefully, because it has three non-obvious consequences:

1. **If the logic time scale is disabled, the simulation runs at the render rate.**
   That is the original 2003 behaviour, preserved on purpose
   (`FramePacer.cpp:196-197`: *"Returns uncapped value to align with the render
   update as per the original game behavior."*). Uncap the FPS limit in Options and
   the game literally plays faster. GeneralsX papers over the common case at init:
   if `m_framesPerSecondLimit > BaseFps` it force-enables the logic time scale at 30
   (`GameEngine.cpp:743-748`, with a comment explaining exactly this hazard).
2. **The accumulator is clamped to one step** (`min(getUpdateTime(), targetFrameTime)`).
   There is no catch-up. A render frame that takes 200 ms advances the simulation by
   one logic frame, not six. Under render load the game goes into slow motion rather
   than stuttering forward — and `getActualLogicTimeScaleOverFpsRatio()` clamps to
   `min(1.0f, ...)` (`FramePacer.cpp:209`) with a comment saying the logic rate is
   currently capped by the render rate. Logic can never outrun rendering.
3. **Network games bypass all of it.** `canUpdateNetworkGameLogic()`
   (`GameEngine.cpp:921`) only asks `TheNetwork->isFrameDataReady()`. Pacing in MP
   is the network's job.

`TheFramePacer->update()` calls `FrameRateLimit::wait(maxFps)`
(`Core/GameEngine/Source/Common/FrameRateLimit.cpp:46`), which sleeps to within ~2 ms
and then spins. `RenderFpsPreset::UncappedFpsValue == 1000000` is the sentinel for
"don't limit".

### 2.4 What "paused" and "frozen" mean

Three distinct concepts, easy to conflate:

- `GameEngine::isTimeFrozen()` (`GameEngine.cpp:326`) — scripted/debug time freeze.
  Blocks logic but **not** the script engine. Forced off in network games.
- `GameEngine::isGameHalted()` (`GameEngine.cpp:348`) — network stall, or
  `TheGameLogic->isGamePaused()` in single player. Blocks logic *and* scripts.
- `GameLogic::setGamePausedInFrame()` (`GameLogic.cpp:4456`) — arms a deferred pause
  that fires from `GameLogic::preUpdate()` (`GameLogic.cpp:3989`) when the target
  frame arrives.

`GameLogic::hasUpdated()` (`GameLogic.h:140`) tells the renderer whether the logic
actually advanced this render frame; `W3DDisplay.cpp:2061` feeds it to `WW3D::Sync()`
so W3D animations only advance on real logic ticks.

---

## 3. The message stream: how a click becomes an order

### 3.1 The types

`GeneralsMD/Code/GameEngine/Include/Common/MessageStream.h`.

- `GameMessage` (`:96`) — a type enum plus a `std::vector<GameMessageArgument*>` of
  tagged unions. Doubly linked into whichever `GameMessageList` owns it.
- `GameMessageList` (`:696`) — plain intrusive list.
- `MessageStream : GameMessageList` (`:743`) — adds the priority-ordered translator
  chain. There is exactly one, `TheMessageStream`.
- `CommandList : GameMessageList` (`:802`) — the queue the logic drains. One
  instance, `TheCommandList`.
- `GameMessageTranslator` (`:732`) — one virtual:
  `translateGameMessage(const GameMessage*) → KEEP_MESSAGE | DESTROY_MESSAGE`.

The message enum is partitioned by numeric range, and the ranges are load-bearing:

| Range | Meaning |
|---|---|
| `MSG_RAW_MOUSE_BEGIN`…`MSG_RAW_MOUSE_END` | raw device events from `Mouse::createStreamMessages()` |
| `MSG_MOUSE_*_CLICK` | "refined" clicks synthesised by `MetaEventTranslator` |
| `MSG_BEGIN_META_MESSAGES`…`MSG_END_META_MESSAGES` (`:156`, `:441`) | virtual keystrokes for key remapping. Explicitly **never** sent over the network |
| `MSG_*_HINT` (`:442`-`:479`) | UI feedback (cursor shape, "you could attack this"). Client-only |
| `MSG_BEGIN_NETWORK_MESSAGES = 1000` … `MSG_END_NETWORK_MESSAGES = 1999` (`:497`, `:619`) | **the actual orders.** These are the only things the logic executes |

The fixed `= 1000` and the large comment above it exist so that `#ifdef`s upstream
cannot shift the numeric values of anything recorded into a replay file
(`MessageStream.h:483-495`). Adding a message inside that range, or wrapping one in
an `#ifdef`, breaks replay compatibility silently. Debug-only network messages get
their own sub-range at `MSG_BEGIN_DEBUG_NETWORK_MESSAGES = 1900`.

Every `GameMessage` stamps itself with the **local** player on construction
(`MessageStream.cpp:57`: `m_playerIndex = ThePlayerList->getLocalPlayer()->getPlayerIndex();`).
There is no null guard — `MessageStream::isReadyForMessages()` (`:848`) exists
precisely because `ThePlayerList` may not be up yet.

### 3.2 The translator chain

Registered in `GameClient::init()` (`GameClient.cpp:293-311`), lower priority first:

| Prio | Translator | Job |
|---|---|---|
| 10 | `WindowTranslator` | feed events to the GUI window manager; eat anything a widget consumed (`WindowXlat.cpp:168`) |
| 20 | `MetaEventTranslator` | keys → `MSG_META_*`; raw button-up → refined `MSG_MOUSE_*_CLICK` (`MetaEvent.cpp:439`) |
| 25 | `HotKeyTranslator` | control-bar hotkeys |
| 30 | `PlaceEventTranslator` | structure placement mode → `MSG_DOZER_CONSTRUCT[_LINE]` (`PlaceEventTranslator.cpp:264-280`) |
| 40 | `GUICommandTranslator` | control-bar button presses |
| 50 | `SelectionTranslator` | picking/marquee → `MSG_CREATE_SELECTED_GROUP` etc. (`SelectionXlat.cpp:369`) |
| 60 | `LookAtTranslator` | camera scroll/rotate/zoom (`LookAtXlat.cpp:199`) |
| 70 | `CommandTranslator` | context commands → `MSG_DO_*` (`CommandXlat.cpp:2473`) |
| 100 | `HintSpyTranslator` | consumes `MSG_*_HINT` to drive cursors/tooltips (`HintSpy.cpp:42`) |
| 999999999 | `GameClientMessageDispatcher` | the gate — see below |

`MessageStream::propagateMessages()` (`MessageStream.cpp:1074`) is
**translator-major, message-minor**: the outer loop is over translators, the inner
loop over the whole message list. So translator N sees every message, including ones
appended by translator N−1, before translator N+1 sees anything.

Two details that follow from that and that you will trip over:

- `next = msg->next()` is read **after** `translateGameMessage()` returns
  (`MessageStream.cpp:1090-1091`). A translator may therefore `insertMessage()`
  immediately after the current message and have the *same* translator pass pick the
  new one up. `MetaEventTranslator::onMouseEvent()` relies on this
  (`MetaEvent.cpp:514`).
- Messages appended with `appendMessage()` go to the **end** of the list, so within
  a single propagation pass they are processed *after* everything already queued.
  `CommandTranslator::m_teamExists` is maintained by watching selection messages go
  by (`CommandXlat.cpp:3776`, `:3793`, `:3801`), which means it reflects a selection
  made in the same frame only if the selection message precedes the order message in
  the stream. In practice this works because `SelectionTranslator` destroys the click
  when it consumes it for selection (`SelectionXlat.cpp:719`), so a click either
  selects *or* orders, never both.

`GameClientMessageDispatcher::translateGameMessage()`
(`GameClientDispatch.cpp:42-56`) is the whole reason the ranges exist:

```cpp
if (type >= MSG_BEGIN_NETWORK_MESSAGES && type <= MSG_END_NETWORK_MESSAGES) return KEEP_MESSAGE;
if (type == MSG_NEW_GAME || type == MSG_CLEAR_GAME_DATA)                    return KEEP_MESSAGE;
if (type == MSG_FRAME_TICK)                                                  return KEEP_MESSAGE;
return DESTROY_MESSAGE;
```

Everything else — every raw event, every meta message, every hint — dies here.
Only the survivors are handed to `TheCommandList` (`MessageStream.cpp:1106`).

**If you add a new order and it silently does nothing, this is why.** It must be
inside the 1000–1999 range.

### 3.3 Worked example: right-click on the ground → units move

1. SDL3 → `Mouse::createStreamMessages()` (from `GameClient::update()`,
   `GameClient.cpp:621`) posts `MSG_RAW_MOUSE_RIGHT_BUTTON_UP`.
2. `WindowTranslator` (10) asks the window manager. If the cursor was over the
   control bar the event is consumed and returns `DESTROY_MESSAGE`
   (`WindowXlat.cpp:349-353`). Otherwise it survives.
3. `MetaEventTranslator` (20) inserts a refined `MSG_MOUSE_RIGHT_CLICK` carrying a
   pixel *region*, collapsed to a point if the drag was under
   `TheMouse->m_dragTolerance` (`MetaEvent.cpp:498-528`). Region vs. point is how
   downstream code distinguishes a click from a marquee.
4. `SelectionTranslator` (50) ignores right-clicks in the default mouse scheme.
5. `CommandTranslator` (70), `case MSG_MOUSE_RIGHT_CLICK` (`CommandXlat.cpp:3915`):
   converts the pixel to a world position with `TheTacticalView->screenToTerrain()`,
   picks whatever drawable is under the cursor, drops it if the object is
   effectively dead, then calls `evaluateContextCommand(draw, &pos, DO_COMMAND)`
   (`CommandXlat.cpp:3955`).
6. `evaluateContextCommand()` (`CommandXlat.cpp:1551`) is the context-sensitive
   cursor/order brain: ~500 lines deciding attack vs. enter vs. repair vs. capture
   vs. move, gated on `KINDOF_*` flags, `CommandButton` options, and player
   relationships. With no target object it falls through to
   `issueMoveToLocationCommand()` (`CommandXlat.cpp:991`), which picks between
   `MSG_ADD_WAYPOINT` / `MSG_DO_ATTACKMOVETO` / `MSG_DO_FORCEMOVETO` /
   `MSG_DO_ATTACK_OBJECT` / `MSG_DO_MOVETO` based on the current `TheInGameUI` mode
   and appends the message with the world location.
7. `GameClientMessageDispatcher` keeps `MSG_DO_MOVETO` (it is ≥ 1000).
8. `propagateMessages()` moves it to `TheCommandList`.
9. Next logic tick, `GameLogic::update()` → `processCommandList(TheCommandList)`
   (`GameLogic.cpp:3842`) → `logicMessageDispatcher(msg, nullptr)`
   (`GameLogic.cpp:2700`).
10. `logicMessageDispatcher()` (`GameLogicDispatch.cpp:366`) builds
    `currentlySelectedGroup` **from the logic-side player's selection**, not from the
    message (`GameLogicDispatch.cpp:387-409`), strips out anything the issuing player
    does not own, then dispatches to `onDoMoveto()` (`GameLogicDispatch.cpp:1325`).
11. `AIGroup::groupMoveToPosition()` (`AIGroup.cpp:1591`) does the formation /
    tightening / sort-near-to-far work and finally calls
    `ai->aiMoveToPosition(&dest, cmdSource)` per unit (`AIGroup.cpp:1785`).
12. That lands in `AIUpdateInterface::aiDoCommand()` → `privateMoveToPosition()` →
    a state change on the unit's `AIStateMachine`.

**The critical bit is step 10.** Orders do not carry their target set. The logic
reads the *player's* selection, which is itself replicated by separate
`MSG_CREATE_SELECTED_GROUP` / `MSG_REMOVE_FROM_SELECTED_GROUP` /
`MSG_DESTROY_SELECTED_GROUP` messages (`GameLogicDispatch.cpp:2028`, `:2047`, `:2064`).
Selection is therefore *two* pieces of state that must be kept in lockstep: the
client's `TheInGameUI` selection (updated immediately, for cursor and UI) and the
logic's `Player::m_currentSelection` squad (updated only when the selection message
reaches the logic). `GameLogic::selectObject()` (`GameLogic.cpp:2767`) can push the
other way, but only when `affectClient` is true — and `onCreateSelectedGroup()` does
*not* pass it, because the client already selected locally.

### 3.4 The network detour

`Core/GameEngine/Source/GameNetwork/Network.cpp`. In a network game
`Network::update()` (`:689`) runs *after* `propagateMessages()` and does:

1. `GetCommandsFromCommandList()` (`:468`) — walks `TheCommandList`, and for anything
   `isTransferCommand()` **removes and deletes it locally** after handing it to the
   connection manager stamped with `getExecutionFrame() = currentFrame + m_runAhead`
   (`:489-495`).
2. Later, when every peer's commands for a frame have arrived,
   `RelayCommandsToCommandList()` puts them all back on `TheCommandList` (`:596`,
   `:679`) and sets `m_frameDataReady`, which is what unblocks
   `canUpdateNetworkGameLogic()`.

So **in multiplayer your own orders do not execute on the frame you issued them** —
they round-trip through the network layer and execute `m_runAhead` frames later, on
every machine simultaneously. This is the classic lockstep design; it is also why any
client-side prediction (voice responses, cursor feedback) is fired at message-creation
time in the translators, e.g. `pickAndPlayUnitVoiceResponse()` in
`CommandXlat.cpp:1040`.

Replays use the same door: `RecorderClass::appendNextCommand()` reads a message from
the file and appends it to `TheCommandList` (`Recorder.cpp:1478`).

---

## 4. `GameLogic::update()` in detail

`GameLogic.cpp:3734`. In order:

| Line | Step |
|---|---|
| 3744 | `setFPMode()` — reset the FPU because the graphics driver may have changed it |
| 3782 | `TheGameClient->setFrame(now)` — the client's frame counter mirrors the logic's |
| 3788 | `TheScriptEngine->UPDATE()` |
| 3794 | `TheTerrainLogic->UPDATE()` — must be after scripts, before objects (bridge state) |
| 3799-3827 | CRC generation for replays/MP, appended as `MSG_LOGIC_CRC` |
| 3837 | `TheRecorder->UPDATE()` |
| 3842 | `processCommandList(TheCommandList)` — **all player orders execute here** |
| 3878-3927 | the sleepy update loop — **all object behaviour happens here** |
| 3933 | `TheAI->UPDATE()` → `Pathfinder::processPathfindQueue()` + `ThePlayerList->UPDATE()` |
| 3938 | `TheBuildAssistant->UPDATE()` |
| 3943 | `ThePartitionManager->UPDATE()` |
| 3951 | `processDestroyList()` — deferred deletion |
| 3954 | `TheCommandList->reset()` |
| 3956-3958 | `TheWeaponStore`, `TheLocomotorStore`, `TheVictoryConditions` |
| 3962-3968 | walk every object, re-enable anything whose disable timer expired |
| 3978 | `m_frame++` |

Pathfinding running *after* the object updates matters: a unit that requests a path
during its update gets serviced at the end of the same frame, but only if the budget
holds. `Pathfinder::processPathfindQueue()`
(`Core/GameEngine/Source/GameLogic/AI/AIPathfind.cpp:6040`) drains the queue only
`while (m_cumulativeCellsAllocated < PATHFIND_CELLS_PER_FRAME)`. Overflow stays
queued; that is the "waiting for path" state you see in the AI metrics.

### 4.1 The sleepy update scheduler

This is the single cleverest thing in the subsystem and the thing most likely to bite
you.

`GameLogic` keeps `std::vector<UpdateModulePtr> m_sleepyUpdates` (`GameLogic.h:425`)
maintained as a **binary min-heap keyed on wake frame**. Each `UpdateModule` stores
`m_nextCallFrameAndPhase` — the absolute wake frame shifted left 2 bits, ORed with a
2-bit `SleepyUpdatePhase` (`UpdateModule.h:82-90`, `:199-206`). So the heap sorts by
frame first and by phase within a frame, for free. `m_indexInLogic` on each module
is the module's own index into the heap, kept in sync by every swap.

The loop (`GameLogic.cpp:3878-3927`):

```cpp
while (!m_sleepyUpdates.empty()) {
    UpdateModulePtr u = peekSleepyUpdate();          // heap root
    if (u->friend_getNextCallFrame() > now) break;   // everyone else is asleep
    ...
    sleepLen = u->update();
    u->friend_setNextCallFrame(now + sleepLen);
    rebalanceSleepyUpdate(0);                        // sift the root back down
}
```

Note it never pops — it rewrites the root's key and re-sifts, which is why
`update()` **must not return 0**. `UPDATE_SLEEP_NONE == 1` means "wake next frame";
`UPDATE_SLEEP_FOREVER == 0x3fffffff` (~414 days at 30 fps) means "don't call me until
something wakes me". Returning 0 would re-run the module forever in the same frame;
there is a `DEBUG_ASSERTCRASH` plus a defensive clamp at `GameLogic.cpp:3915-3917`.

Waking a sleeping module is `UpdateModule::setWakeFrame()` →
`GameLogic::friend_awakenUpdateModule()` (`GameLogic.cpp:3145`). It is
**protected on purpose** — a module may only wake *itself*. Calling it from inside
your own `update()` is rejected with a `DEBUG_CRASH` (`GameLogic.cpp:3151-3155`);
the return value of `update()` is the only way to reschedule yourself. There is also
a subtle special case at `:3160-3167`: if you are already awake this frame,
`setWakeFrame(UPDATE_SLEEP_NONE)` is a no-op so it cannot cancel the call you are
already owed.

`ALLOW_NONSLEEPY_UPDATES` is compiled out (`GameLogic.h:50` defines
`NO_ALLOW_NONSLEEPY_UPDATES`); the `#ifdef`'d `m_normalUpdates` list is dead code the
original author asked to be deleted before ship (`GameLogic.h:41-49`). It never was.

There is a per-frame `getDisabledTypesToProcess()` filter around each `update()` call
(`GameLogic.cpp:3898-3907`). Note the two different semantics behind
`RETAIL_COMPATIBLE_CRC`: retail runs the update if the disabled mask has *any*
intersection with the module's mask; the fixed version requires the module's mask to
cover *all* set disabled bits. This changes behaviour, so it is gated.

### 4.2 Deferred destruction

Nothing is deleted mid-frame. `GameLogic::destroyObject()` (`GameLogic.cpp:4130`)
fires the `DestroyModule`s, sets `OBJECT_STATUS_DESTROYED`, kills the AI's locomotor
goal and path (the comment at `:4149-4150` explains why: the state machine destructor
would otherwise call virtuals on already-deleted modules), and pushes onto
`m_objectsToDestroy`.

`processDestroyList()` (`GameLogic.cpp:2607`) then, at end of frame, removes each
object's update modules from the heap and deletes the object. It does this in two
passes with an explicit comment about why (`:2635-2644`): erasing from a heap shuffles
other elements, so it must collect the victims first and then erase by index.

---

## 5. `Object` and `Drawable`

Both derive from `Thing` (`Include/Common/Thing.h:83`), which owns the transform and
the cached position. `Object : Thing, Snapshot` (`Object.h:159`) is the logic-side
entity; `Drawable : Thing, ...` (`Drawable.h:294`) is the client-side one. They are
1:1 and cross-linked (`GameLogic::bindObjectAndDrawable`, `GameLogic.cpp:4417`).

An `Object` is:

- an `ObjectID` and a slot in `GameLogic::m_objVector` (a direct-indexed vector, grown
  by doubling — `GameLogic.cpp:4031-4032`), plus a node in the intrusive `m_objList`;
- a `BehaviorModule** m_behaviors` — a nullptr-terminated array (`Object.h:748`);
- a handful of cached interface pointers into that array for hot paths: `m_ai`,
  `m_body`, `m_contain`, `m_physics`, `m_stealth` (`Object.h:751-756`). These are
  *duplicates* of entries in `m_behaviors`, as the comment on `m_ai` says;
- a `PartitionData*` (spatial index), `RadarObject*`, `ExperienceTracker*`,
  `WeaponSet`, team/player membership, status and disabled bitmasks.

### Construction order (`Object::Object`, `Object.cpp:179`)

1. Allocate `m_behaviors` with room for
   `tt->getBehaviorModuleInfo().getCount() + NUM_SLEEP_HELPERS + 1` (`Object.cpp:302-306`).
2. Create the **helpers first**, "even before Behaviors! -- in case a module needs to
   call something that uses them" (`Object.cpp:316-400`): SMC, status-damage,
   subdual-damage, repulsor, defection, weapon-status, firing-tracker,
   temp-weapon-bonus. Each is conditional on template properties.
3. Create the INI-declared behaviour modules via
   `TheModuleFactory->newModule(...)` (`Object.cpp:410`), caching `m_body`,
   `m_contain`, `m_stealth`, `m_ai`, `m_physics` as they go, with
   `DEBUG_ASSERTCRASH` on duplicates — **an object may have at most one AI module**
   (`Object.cpp:438-443`).
4. Terminate with `*curB = nullptr` (`Object.cpp:453`).
5. Second pass: `onObjectCreated()` on every module, "to allow for inter-Module
   resolution" (`Object.cpp:475-478`). This is where a module may safely look up its
   siblings.
6. `TheGameLogic->registerObject(this)` (`Object.cpp:492`) — links into the object
   list and pushes every `UpdateModule` into the sleepy heap
   (`GameLogic.cpp:4058-4102`).
7. `TheGameLogic->sendObjectCreated(this)` (`Object.cpp:505`) — which creates the
   `Drawable` (`GameLogic.cpp:4405`). The ordering here was changed by an upstream
   bugfix (`Object.cpp:501-503`): the drawable must exist before `CreateModule`s run.

`NUM_SLEEP_HELPERS = 8` (`Object.h:737`) and exactly eight helpers can be created.
There is zero slack in that array.

### Death vs. destruction

They are separate.

- `Object::kill()` (`Object.cpp:2041`) is just "apply unblockable damage equal to max
  health".
- `ActiveBody::attemptDamage()` notices `m_currentHealth <= 0 && m_prevHealth > 0`
  and calls `Object::onDie()` (`ActiveBody.cpp:667-675`).
- `Object::onDie()` (`Object.cpp:4674`) fans out to every `DieModule`
  (`Object.cpp:4687-4692`), then does the radar/EVA/spawner/rebuild-hole bookkeeping.
- Actually removing the object is a *`DieModule`'s* decision: `DestroyDie::onDie()`
  calls `TheGameLogic->destroyObject()` (`DestroyDie.cpp:54-60`). A unit with
  `SlowDeathBehavior` instead stays alive-but-dead for a while.
- `DieMuxData` (`DieModule.h:50`) lets each die module filter on death type,
  veterancy level, and required/exempt status bits — which is how one template can
  have a different corpse per death type.

---

## 6. The module system

`Include/Common/Module.h`. The hierarchy:

```
MemoryPoolObject + Snapshot
 └── Module                     (Module.h:178)  — owns a const ModuleData*
      ├── ObjectModule          (Module.h:240)
      │    └── BehaviorModule   (BehaviorModule.h:143)  — the giant getXxx() interface bag
      │         ├── UpdateModule    (UpdateModule.h:130)
      │         │    ├── ObjectHelper    (ObjectHelper.h:37)
      │         │    └── AIUpdateInterface (AIUpdate.h:233)  and ~50 others
      │         ├── DieModule       (DieModule.h:83)
      │         ├── DamageModule / CollideModule / CreateModule / DestroyModule
      │         ├── BodyModule / ContainModule / UpgradeModule / SpecialPowerModule
      └── DrawableModule → DrawModule, ClientUpdateModule
```

There are 222 module headers under `Include/GameLogic/Module/`.

**Interface discovery is by downcast-via-virtual-getter, not RTTI.**
`BehaviorModuleInterface` (`BehaviorModule.h:98-139`) declares ~35 `getXxx()`
methods; `BehaviorModule` implements all of them returning `nullptr`
(`BehaviorModule.h:156` onwards), and each concrete module overrides the one or two that
apply. So "does this object have an AI?" is
`for (b : m_behaviors) if (b->getAIUpdateInterface()) ...`. Ugly, cheap, and the
reason that adding a new cross-cutting interface means editing `BehaviorModule.h`.

**Registration** is a static table in
`ModuleFactory::init()` (`Source/Common/Thing/ModuleFactory.cpp:318`), one
`addModule(ClassName)` line per module. The macro (`ModuleFactory.h:107-113`) wires
up four things the `MAKE_STANDARD_MODULE_*` macros generated on the class:
`friend_newModuleInstance`, `friend_newModuleData`, `getModuleType()`, and
`getInterfaceMask()`. Lookup is by a name key decorated with the module type
(`ModuleFactory::makeDecoratedNameKey`, `:600`) so an object module and a
drawable module may share a name.

**Module data** is parsed from INI once per template into a `ModuleData` subclass and
shared by every instance (`Module::m_moduleData` is `const`). Per-instance state lives
on the module. `getModuleTagNameKey()` is the `ModuleTag_*` string from INI and is
what save games use to match modules back up.

`ObjectHelper` (`ObjectHelper.h:37`) is worth knowing: it is a plain `UpdateModule`
that constructs itself asleep forever and exposes `sleepUntil()`. The eight built-in
helpers use it to piggyback deferred work onto the same scheduler without appearing in
INI.

---

## 7. AI

### 7.1 `AIUpdateInterface`

`Include/GameLogic/Module/AIUpdate.h:233`,
`Source/GameLogic/Object/Update/AIUpdate.cpp`.

It is an `UpdateModule` **and** an `AICommandInterface`. The command interface is
public (`aiMoveToPosition`, `aiAttackObject`, …); every one funnels into
`aiDoCommand(const AICommandParms*)` (`AIUpdate.cpp:2622`), a big switch that calls
the matching **`protected virtual privateXxx()`**. Subclasses override the `private*`
methods, callers use the `ai*` methods. That is the extension point — see the comment
"yes, protected, NOT public" at `AIUpdate.h:241`.

`aiDoCommand` first asks `isAllowedToRespondToAiCommands()` (`AIUpdate.cpp:2592`),
which rejects commands for dead units, for AI-controlled units in the `MM_Mood_Sleep`
mood, and for templates with `m_forbidPlayerCommands` (added for the Spectre Gunship
— see the comment at `:2612-2615`).

`AIUpdateInterface::update()` (`AIUpdate.cpp:1002`):

1. run the `AIStateMachine`, translating its `StateReturnType` into a sleep length
   (`:1015-1028`);
2. handle "movement complete" — destroy the path, snap to the pathfinder goal
   (`:1035-1062`);
3. re-queue for a path if the queue-delay frame has arrived (`:1065-1078`);
4. tick up to `MAX_TURRETS` `TurretAI`s, unless disabled/dead (`:1082-1099`);
5. force-transition to `AI_DEAD` if the unit died outside the state machine
   (`:1102-1111`) — the comment says this must happen *outside* the machine update to
   avoid corruption;
6. `doLocomotor()` (`:1114`), which is what actually moves the object;
7. return the minimum of all the sleep requests.

`SLEEPY_AI` is defined (`AIUpdate.cpp:74`) so the computed sleep is honoured; with it
off every AI would return `UPDATE_SLEEP_NONE` and run every frame.

### 7.2 The state machine

`Include/Common/StateMachine.h`. A `State` returns a `StateReturnType`:
`STATE_CONTINUE = 0`, `STATE_SUCCESS = -1`, `STATE_FAILURE = -2`, and **any positive
value is a sleep in frames** (`StateMachine.h:63-76`). `IS_STATE_SLEEP(ret)` is
`(Int)ret > 0`. Each state declares its on-success and on-failure successor plus
arbitrary `onCondition()` transitions.

`AIStateMachine`'s state table is one long `defineState()` block in its constructor
(`AIStates.cpp:672-729`) — ~45 states, `AI_IDLE` first because *"order matters: first
state is the default state"* (`AIStates.cpp:684`).

### 7.3 `DozerAIUpdate` and `WorkerAIUpdate` — construction

`DozerAIUpdate : AIUpdateInterface, DozerAIInterface`
(`Include/GameLogic/Module/DozerAIUpdate.h:195`) adds a *second*, private state
machine (`DozerPrimaryStateMachine`) on top of the normal AI machine, plus a
three-slot task queue `m_task[DOZER_NUM_TASKS]` indexed by
`DOZER_TASK_BUILD / REPAIR / FORTIFY` (`DozerAIUpdate.h:281-291`).

`DozerAIUpdate::update()` (`DozerAIUpdate.cpp:1562`):

```
createMachines();                       // lazily, on first update, not in the ctor
result = AIUpdateInterface::update();   // normal AI first
if (dead) return UPDATE_SLEEP_NONE;
validate/cancel the current task;
m_dozerMachine->updateStateMachine();   // then the dozer machine
return UPDATE_SLEEP_NONE;               // never sleeps
```

The lazy `createMachines()` is deliberate: the comment at `:1570-1573` says the
machines can only be built once every module on the object exists, which is not true
in the constructor.

The build path, end to end:

1. `PlaceEventTranslator` posts `MSG_DOZER_CONSTRUCT` with template ID, position and
   angle (`PlaceEventTranslator.cpp:266-272`) — but only after
   `TheBuildAssistant->isLocationLegalToBuild()` passes client-side (`:220-231`).
2. `GameLogic::onDozerConstruct()` (`GameLogicDispatch.cpp:1883`) resolves the single
   selected object and calls `TheBuildAssistant->buildObjectNow()`.
3. `BuildAssistant::buildObjectNow()` (`Source/Common/System/BuildAssistant.cpp:321`)
   re-validates server-side (`:342`, with the comment *"Need to validate that we can
   make this in case someone fakes their CommandSet"*), clears/moves blocking objects,
   then for a real dozer calls `ai->aiIdle()` followed by
   `ai->construct(...)` (`:364-365`).
4. `DozerAIUpdate::construct()` (`DozerAIUpdate.cpp:1630`) creates the structure
   `Object` immediately with `OBJECT_STATUS_UNDER_CONSTRUCTION`, withdraws the money,
   flattens the terrain *before* adding it to the pathfind map (`:1714-1721` — the
   ordering is called out in a comment), sets it to 1 hit point, and finally
   `newTask(DOZER_TASK_BUILD, obj)` (`:1739`).
5. The dozer machine walks `DOZER_SELECT_BUILD_DOCK_LOCATION` →
   `DOZER_MOVING_TO_BUILD_DOCK_LOCATION` → `DOZER_DO_BUILD_AT_DOCK`
   (`DozerAIUpdate.cpp:500-524`), and in the last sub-state adds
   `100.0f / calcTimeToBuild()` percent and a proportional slice of max health **per
   logic frame** (`:531-543`).
6. At 100% it clears the construction status bits, calls
   `Player::onStructureConstructionComplete()`, and only *then* runs every
   `CreateModule::onBuildComplete()` (`:594-599`). `onCreate` fired at placement time;
   `onBuildComplete` fires now. Modules that need a finished building must use the
   latter.

`WorkerAIUpdate : AIUpdateInterface, DozerAIInterface, SupplyTruckAIInterface, WorkerAIInterface`
(`WorkerAIUpdate.h:118`) is a dozer *and* a harvester. It runs **three** machines: a
top-level `WorkerStateMachine` that picks `AS_DOZER` or `AS_SUPPLY_TRUCK`
(`WorkerAIUpdate.cpp:70-71`), and then delegates to the dozer machine or the supply
truck machine (`WorkerAIUpdate.cpp:265-303`).

The dozer/worker code is **copy-pasted, not shared**, and the source says so
repeatedly:

> `// !!! NOTE: If you modify this you must modify the worker too !!!`
> `// !!! Graham: Please please please have inspiration for how to *not* duplicate this code`
> — `DozerAIUpdate.cpp:1637-1638`

> `// NOTE: If you edit module data you must do it in both the Dozer *AND* the Worker`
> — `DozerAIUpdate.h:169`, `WorkerAIUpdate.h:59`

Any dozer fix you make almost certainly needs the same edit in `WorkerAIUpdate.cpp`.

---

## 8. Determinism

Everything in the logic must be reproducible bit-for-bit across machines and across
replay playback.

- **Two RNGs.** `GameLogicRandomValue()` (`Core/GameEngine/Include/GameLogic/LogicRandomValue.h:41`)
  is part of the synchronised state; `GameClientRandomValue()`
  (`ClientRandomValue.h`) is not. Using the client RNG in logic code, or vice versa,
  is a desync. The macros capture `__FILE__`/`__LINE__` so CRC logs can point at the
  call site.
- **CRC.** `GameLogic::getCRC(CRC_RECALC)` (`GameLogic.cpp:4183`) xfers every object
  plus the RNG seed through an `XferCRC`. It runs on a fixed cadence
  (`GameLogic.cpp:3799-3827`) and is posted as `MSG_LOGIC_CRC` so peers can compare.
  `processCommandList()` does the comparison (`GameLogic.cpp:2703-2753`).
- **FPU state.** `setFPMode()` (`GameLogic.cpp:203`) is called at the top of
  `GameLogic::update()` and anywhere the logic touches the graphics driver, because
  D3D is allowed to clobber the control word. On Windows it sets round-to-nearest and
  **24-bit precision** (`_PC_24`). The GeneralsX port added a non-Windows branch
  (`GameLogic.cpp:226-243`) that sets `FE_TONEAREST` and clears the x87 precision
  bits on x86 — but on **arm64 there is no x87 and no MXCSR**, so both of those
  blocks compile out and only `fesetenv`/`fesetround` apply. Apple Silicon computes
  in true IEEE single/double where the original ran at 24-bit x87 precision. Replays
  and cross-platform MP against Windows cannot be assumed bit-identical. If you are
  chasing a desync on macOS/iOS, start here.
- **`RETAIL_COMPATIBLE_CRC` / `RETAIL_COMPATIBLE_AIGROUP`.** Several genuine bug
  fixes are compiled out under these flags precisely because fixing them changes the
  simulation (e.g. the disabled-mask semantics at `GameLogic.cpp:3899-3907`, the
  dozer's invalid-target check at `DozerAIUpdate.cpp:1607-1610`). Check which side of
  the `#if` you are on before concluding something is broken.
- **No `std::` iteration-order dependence, no wall-clock, no floats from timers** in
  logic code. `TheFramePacer`'s real-time values are for the *client*.

---

## 9. Gotchas

Ordered roughly by how much time they will cost you.

1. **A new order message outside 1000–1999 is silently deleted.**
   `GameClientMessageDispatcher` (`GameClientDispatch.cpp:44-55`) drops everything
   else before it can reach `TheCommandList`. And once you add one inside the range
   you have changed every subsequent enum value, which breaks replay files — that is
   what the shouty comment at `MessageStream.h:483-495` is about. Append at the end
   of the range, never insert.

2. **Orders read the selection from the logic, not from the message.**
   `logicMessageDispatcher()` rebuilds `currentlySelectedGroup` from
   `Player::getCurrentSelectionAsAIGroup()` every time (`GameLogicDispatch.cpp:387-393`).
   If your new command needs a target set, either it is the current selection or you
   must put explicit `ObjectID`s in the message. There are consequently two
   selections in the engine (client `TheInGameUI`, logic `Player::m_currentSelection`)
   and they are synchronised only by messages.

3. **`update()` must never return 0, and must never call `setWakeFrame()` on
   itself.** Returning 0 spins the scheduler; `friend_awakenUpdateModule()`
   explicitly refuses and `DEBUG_CRASH`es if `u == m_curUpdateModule`
   (`GameLogic.cpp:3151-3155`). Reschedule via the return value only.

4. **An object destroyed mid-frame keeps updating for the rest of that frame.**
   `destroyObject()` only queues onto `m_objectsToDestroy` (`GameLogic.cpp:4159`);
   the update modules are not removed from the sleepy heap until
   `processDestroyList()` runs at `GameLogic.cpp:3951`. Any module scheduled for the
   current frame that has not yet run will still run, on an object flagged
   `OBJECT_STATUS_DESTROYED`. Defensive `isEffectivelyDead()` / `isDestroyed()`
   checks scattered through the update modules exist for this reason. If you write a
   new update module that can outlive its target, check.

5. **`processDestroyList()` silently drops sleepy updates past 256 per object.**
   `const Int MAX_SUO = 256; ... if (u->friend_getObject() == currentObject && numSUO < MAX_SUO)`
   (`GameLogic.cpp:2646-2657`). Beyond that the module stays in the heap while the
   object is deleted — a dangling pointer. No real template comes close, but there is
   no assert either.

6. **The helper array has exactly zero slack.** `NUM_SLEEP_HELPERS = 8`
   (`Object.h:737`) and `Object::Object` creates up to exactly eight helpers
   (`Object.cpp:316-400`). Adding a ninth without bumping the constant overruns
   `m_behaviors`.

7. **Logic can never run faster than the render loop, and never catches up.**
   `canUpdateRegularGameLogic()` clamps the accumulator to one step
   (`GameEngine.cpp:959`) and `getActualLogicTimeScaleOverFpsRatio()` clamps the
   ratio to `min(1.0f, …)` (`FramePacer.cpp:209`). A render hitch is a simulation
   hitch. Conversely, with the logic time scale *disabled* the simulation ticks once
   per rendered frame, so an uncapped renderer runs the game at fast-forward — see
   the GeneralsX workaround at `GameEngine.cpp:743-748`.

8. **Rendering shows the previous logic frame.** `TheGameClient->UPDATE()` (which
   draws) runs before `TheGameLogic->UPDATE()` in `GameEngine::update()`
   (`GameEngine.cpp:1073` vs `:1089`).

9. **There is no interpolation.** `Object::reactToTransformChange()` copies the
   transform straight into the drawable (`Object.cpp:1858-1861`). At 60 fps with a
   30 Hz simulation, unit positions update on every second rendered frame. Anything
   that looks smooth (camera, shake, some fades) is smooth because the *client*
   animates it, not because positions are lerped.

10. **Some client-side per-frame effects are frame-rate dependent and some are not.**
    `TintEnvelope::update()` (`Drawable.cpp:5609`) and the stealth material fade
    (`Drawable.cpp:2656`) were retrofitted with
    `TheFramePacer->getActualLogicTimeScaleOverFpsRatio()`. The drawable opacity fade
    a few hundred lines earlier still does a bare `++m_timeElapsedFade`
    (`Drawable.cpp:1201-1210`) inside `updateDrawable()`, which is called once per
    *render* frame from `GameClient::update()` (`GameClient.cpp:738`). Assume any
    per-frame client effect you have not personally checked is uncorrected.

11. **In multiplayer your own orders execute `m_runAhead` frames late.**
    `Network::GetCommandsFromCommandList()` deletes them locally and re-injects them
    later (`Core/GameEngine/Source/GameNetwork/Network.cpp:468-486`, `:596`). Never
    assume an order took effect on the frame you posted it.

12. **Dozer and Worker are duplicated source.** See §7.3. Fix both.

13. **`GameMessage`'s constructor dereferences `ThePlayerList->getLocalPlayer()`
    unconditionally** (`MessageStream.cpp:57`). Constructing a message before the
    player list exists crashes. `MessageStream::isReadyForMessages()` is the guard,
    and it is not called automatically.

14. **`arm64` floating point is not the retail floating point.** See §8. This is the
    porting-relevant one and it is not fixable by tweaking `setFPMode()` — arm64 has
    no 24-bit precision mode.

15. **iOS backgrounding stops the whole engine, not just rendering.**
    `SDL3GameEngine::update()` returns before `GameEngine::update()` when
    `iosShouldPauseRendering()` (`GeneralsMD/Code/GameEngineDevice/Source/SDL3GameEngine.cpp:816-829`),
    so the logic does not tick either. Harmless in single player because the
    accumulator is clamped; it would drop a network game.

---

## 10. Things I could not pin down

Honest gaps, with pointers rather than guesses:

- **`SleepyUpdatePhase` in practice.** The enum exists
  (`PHASE_INITIAL/PHYSICS/NORMAL/FINAL`, `UpdateModule.h:82-90`) and is packed into
  the heap key, but I did not find a module overriding `getUpdatePhase()` to anything
  other than the default `PHASE_NORMAL`. The header warns you off changing it. If you
  need to know whether the mechanism is live, grep for `getUpdatePhase` overrides
  before relying on ordering within a frame.
- **`TurretAI` and `Locomotor`.** I traced their call sites
  (`AIUpdate.cpp:1090-1098`, `AIUpdate.cpp:2127` `doLocomotor()`) but not their
  internals. `Source/GameLogic/AI/TurretAI.cpp` and
  `Source/GameLogic/Object/Locomotor.cpp` are the entry points; `Locomotor` is where
  physics-ish movement actually happens and deserves its own document.
- **`PartitionManager`.** Referenced constantly (spatial queries, shroud, threat
  maps) and updated at `GameLogic.cpp:3943`, but I did not read
  `Source/GameLogic/Object/PartitionManager.cpp`. Anything about vision, shroud, or
  "find nearest enemy" lives there.
- **The `Snapshot`/`Xfer` save-game protocol.** `GameLogic::xfer()`
  (`GameLogic.cpp:5040`) and the object TOC (`:4874`) are how save games and the CRC
  are both implemented. The versioning rules for adding a field to a module are real
  and strict; read an existing module's `xfer()` before adding state.
- **`ScriptEngine`.** Ticked first every logic frame and able to issue AI commands
  with `CMD_FROM_SCRIPT`, but out of scope here. `Source/GameLogic/ScriptEngine/`.
- **Why `AIGroup` has two lifetime models.** `RETAIL_COMPATIBLE_AIGROUP` switches
  between a manually-destroyed raw pointer and a `RefCountPtr`
  (`AI.cpp:445-463`, `GameLogicDispatch.cpp:389-402`). The refcounted version is
  presumably the leak fix, but I did not verify the original leak or whether the two
  paths are behaviourally identical.
