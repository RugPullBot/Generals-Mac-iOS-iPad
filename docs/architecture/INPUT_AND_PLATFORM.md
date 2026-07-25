# Input and the Platform Layer

How keyboard, mouse and iOS touch reach the game, and what `SDL3Main.cpp` does before
the engine exists.

Scope: the non-Windows (`#ifndef _WIN32`) path only. Everything below is Zero Hour
(`GeneralsMD/`). **The base-game copies under `Generals/` are a parallel, older fork of
the same files with zero iOS support** — `grep -c TARGET_OS_IPHONE` returns 0 for
`Generals/Code/GameEngineDevice/Source/SDL3GameEngine.cpp` and
`Generals/Code/Main/SDL3Main.cpp`, versus 8 and 7 for the Zero Hour versions. If you
change something here, it does **not** propagate.

> **Working-tree note (2026-07-25).** `SDL3GameEngine.cpp` (+235/-37) and `SDL3Main.cpp`
> (+43) have substantial uncommitted changes. Line numbers below refer to the **working
> tree**, not `HEAD`. If `git stash` has happened since, re-check the line numbers; the
> structural claims should survive.

---

## 1. How it fits together

```
                      SDL3Main.cpp  main()  [iOS: renamed SDL_main by <SDL3/SDL_main.h>]
                        |
                        |  crash handlers, stderr log sink, chdir, env vars   (iOS only)
                        |  CommandLine::parseCommandLineForStartup()
                        |  SDL_InitSubSystem(VIDEO|AUDIO), SDL_Vulkan_LoadLibrary
                        |  SDL_CreateWindow -> TheSDL3Window, ApplicationHWnd
                        |  argv injection: -xres/-yres, -autoload               (iOS only)
                        v
                      GameMain()  ->  CreateGameEngine()  ->  SDL3GameEngine
                        |
                        v
   GameEngine::execute()  (GameEngine.cpp:1107)  while (!m_quitting)
        |
        +-- SDL3GameEngine::update()                        SDL3GameEngine.cpp:816
        |     |
        |     +-- pollSDL3Events()                          SDL3GameEngine.cpp:871
        |     |     SDL_PollEvent loop:
        |     |       KEY_DOWN/UP        -> SDL3Keyboard::addSDLEvent()   (ring, 256)
        |     |       TEXT_INPUT         -> forwardTextInputEvent()  -> GWM_IME_CHAR
        |     |       MOUSE_*            -> SDL3Mouse::addSDLEvent()      (ring, 256)
        |     |       FINGER_* (iOS)     -> handleTouchEvent()  -> synthesised MOUSE_*
        |     |       WINDOW_FOCUS_*     -> TheMouse->loseFocus/regainFocus
        |     |     then: updateTouchLongPress()  (iOS, once per pump)
        |     |
        |     +-- (iOS) if backgrounded||inactive: SDL_Delay(50); return   <-- render gate
        |     |
        |     +-- GameEngine::update()   -> TheGameClient->update()
        |             TheKeyboard->update()        -> Keyboard::updateKeys() -> getKey()
        |             TheKeyboard->createStreamMessages()   -> MSG_RAW_KEY_*
        |             TheMouse->update()           -> Mouse::updateMouseData() -> getMouseEvent()
        |             TheMouse->createStreamMessages()      -> MSG_RAW_MOUSE_*
        |           TheMessageStream->propagateMessages()   -> translators
        |
        +-- TheFramePacer->update(), render, ...
```

Two facts worth internalising before touching anything:

1. **The device layer is a queue, not a callback.** `pollSDL3Events()` never touches game
   state directly; it copies raw `SDL_Event` structs into two fixed-size ring buffers.
   The engine drains them one frame later inside `GameClient::update()`
   (`GeneralsMD/Code/GameEngine/Source/GameClient/GameClient.cpp:607-622` — keyboard
   first, then mouse). Anything that inspects game state *at poll time* (the iOS gesture
   machine does: `TheShell`, `TheInGameUI`) is reading state from the previous frame.

2. **On iOS there is no mouse.** SDL's automatic touch→mouse synthesis is switched off
   (`SDL3Main.cpp:558`), so **every** mouse event the game sees on iOS is forged by the
   gesture translator in `SDL3GameEngine.cpp:143-659` and pushed through the same
   `SDL3Mouse::addSDLEvent()` entry point a real mouse would use.

`serviceWindowsOS()` (`SDL3GameEngine.cpp:846`) is a second entry into the same pump. It
is called from `Core/GameEngine/Source/GameClient/GUI/LoadScreen.cpp:161` to keep the OS
responsive during loads — see the buffering gotcha in §8.

### File map

| File | Role |
|---|---|
| `GeneralsMD/Code/Main/SDL3Main.cpp` | `main()`, process bootstrap, SDL/Vulkan/window creation, iOS environment setup |
| `GeneralsMD/Code/GameEngineDevice/Source/SDL3GameEngine.cpp` | Event pump, iOS lifecycle, iOS touch→mouse gesture machine, subsystem factories |
| `GeneralsMD/Code/GameEngineDevice/Include/SDL3GameEngine.h` | `SDL3GameEngine` declaration; `extern SDL_Window* TheSDL3Window` |
| `.../Source/SDL3Device/GameClient/SDL3Mouse.cpp` | Mouse ring buffer, `SDL_Event`→`MouseIO`, coordinate scaling, `.ani` cursor loading |
| `.../Source/SDL3Device/GameClient/SDL3Keyboard.cpp` | Keyboard ring buffer, scancode→`KeyDefType` |
| `.../Include/W3DDevice/GameClient/W3DGameClient.h:138-163` | `createKeyboard()`/`createMouse()` factories (inline in the header) |
| `ios/Stub/Info.plist` | Landscape-only, `UIFileSharingEnabled`, `UIApplicationSupportsIndirectInputEvents` |
| `scripts/build/ios/package-ios-zh.sh` | Signs the bundle, embeds `GameData/`, installs via `devicectl` |

No gamepad support exists: `SDL_InitSubSystem` is called with `SDL_INIT_VIDEO | SDL_INIT_AUDIO`
only (`SDL3Main.cpp:560`).

---

## 2. `SDL3Main.cpp` — process bootstrap

### Ordering (this order is load-bearing)

| Line | Step | Why here |
|---|---|---|
| 335-336 | `__argc`/`__argv` globals | `CommandLine.cpp:1416-1421` reads these instead of `GetCommandLineA()` on non-Windows |
| 345-414 | iOS: stderr sink + crash handlers | Must precede everything that can log or crash |
| 416-513 | iOS: `chdir`, `DXVK_STATE_CACHE_PATH`, options seeding, Documents tidy-up | The engine resolves all game data relative to CWD |
| 523-527 | Critical sections | Required by `AsciiString`/memory pool |
| 530 | `initMemoryManager()` | Required by `NEW` |
| 535 | `TheVersion = NEW Version` | `GameEngine::init()`→`updateWindowTitle()` dereferences it |
| 541 | `CommandLine::parseCommandLineForStartup()` | Populates `TheGlobalData` (needed for the `-headless` check on 544) |
| 554-559 | iOS: `SDL_HINT_TOUCH_MOUSE_EVENTS=0` | Must be set before `SDL_InitSubSystem` |
| 560 | `SDL_InitSubSystem(VIDEO\|AUDIO)` | |
| 566-572 | `DXVK_WSI_DRIVER=SDL3`, Vulkan ICD filter, OpenAL driver filter | Before `SDL_Vulkan_LoadLibrary` |
| 576 | `SDL_Vulkan_LoadLibrary(nullptr)` | Failure is a warning, not fatal |
| 583-594 | `SDL_CreateWindow` (1024x768, `VULKAN\|RESIZABLE\|HIDDEN`, `+HIGH_PIXEL_DENSITY` on iOS) | |
| 603 | `ApplicationHWnd = (HWND)TheSDL3Window` | The engine treats `SDL_Window*` as an opaque `HWND` |
| 606-694 | iOS: argv injection | **After** startup parsing, **before** `GameMain()` |
| 699 | `GameMain()` | |
| 746 | `_exit(exitcode)` | Deliberately skips C++ global destructors (see comment at 740-745) |

### iOS argv injection — the timing trick

`SDL3Main.cpp:606-694` rewrites `__argv`/`__argc` to append `-xres`/`-yres` (derived from
`SDL_GetWindowSizeInPixels`) and, if a file or folder named `autoload` exists in
`Documents`, `-autoload`.

This works **only because those three flags live in the second parse table.**
`CommandLine.cpp:1157` declares `paramsForStartup[]` and `:1180` declares
`paramsForEngineInit[]`; `-xres`/`-yres` are at `:1185-1186` and `-autoload` at `:1193`,
all in the *engine-init* table, which is parsed much later from
`GameEngine.cpp:530`. The startup table has already been consumed at `SDL3Main.cpp:541`.

**Trap:** injecting anything from `paramsForStartup[]` here (e.g. `-headless`, `:1164`)
would be silently ignored — the startup parse is guarded by
`m_hasParsedCommandLineForStartup` (`CommandLine.cpp:1486-1488`) and never runs twice.

Two smaller notes on the injection:

- The `userSetRes` guard (`:617-622`) scans `__argv`, which on an icon-launched iOS app
  contains only the executable path — so the injection effectively always happens.
- It is gated on `winW > winH` (`:628`), i.e. landscape. The Info.plist restricts the app
  to landscape, so this is belt-and-braces, but a portrait build would silently fall back
  to the engine's 4:3 default.
- `-autoload` matching is deliberately case-insensitive and accepts a *directory*
  (`:671-679`) because the iOS Files app will not create an empty file.

### iOS stderr sink (`SDL3Main.cpp:345-414`)

An icon-launched app's stderr goes nowhere reachable, so a `funopen()` sink is installed
over `stderr`:

- Writes to `~/Documents/generals-stderr.log`; the previous session's log is renamed to
  `generals-stderr-prev.log` first (`:362`) — a memory-kill leaves no OS crash report, so
  the prior log is often the only evidence.
- Filters three known per-frame spam prefixes: `[GX-ISSUE144]`, `[INI] `, `warn:  D3D8De`
  (`:377-382`).
- Caps the file at 8 MB (`:375`), after which only lines starting `err:`/`ERROR`/`FATAL`
  are kept.
- `*stderr = *sink;` (`:405`) is the classic Darwin struct-copy over the `FILE`. It works
  because on Darwin `stderr` is a `FILE *` object, not a macro — this is not portable.
- Line-buffered (`:406`) so a crash still flushes recent lines.
- `DXVK_LOG_LEVEL=none` is set at `:350` for the same reason (hundreds of MB per session).

The filter matches on the *start of a write*, which is only reliably a line start because
of `_IOLBF`. A `fprintf` without a trailing newline, or a line longer than the buffer,
will split and defeat the prefix match. It is a heuristic, not a parser.

### Crash handlers (`SDL3Main.cpp:89-148`)

`SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE, SIGTRAP` plus `std::set_terminate`. Each writes
a name and a `backtrace_symbols_fd()` dump to both the raw log fd (`s_crashLogFd`, handed
over at `:410`) and `STDERR_FILENO`, then restores `SIG_DFL` and re-raises so iOS still
records its own crash report.

Everything in the handler is async-signal-safe (`write()`, no `printf`, no malloc) — keep
it that way. Note `SA_RESETHAND | SA_NODEFER` (`:143`) and the absence of a `sigaltstack`:
a stack-overflow `SIGSEGV` cannot be reported, because the handler needs stack.

`s_crashLogFd` is the raw fd, deliberately *not* the `FILE *` sink — the sink's write
callback is not signal-safe.

### iOS working directory and asset layout (`SDL3Main.cpp:416-513`)

- Preferred: `<bundle>/GameData`, derived from `dirname(argv[0])` (`:428-437`). Read-only,
  signed, self-contained.
- Fallback: `~/Documents` (`:446-454`), used by `--dev` packages built without assets.
- `DXVK_STATE_CACHE_PATH` → `~/Library/Caches` (`:463`), purgeable and not backed up.
- First-run seeding of `Options.ini` from `DefaultOptions.ini` into
  `~/Library/Application Support/GeneralsX/GeneralsZH` (`:468-479`) — the 2003 auto-detect
  drops unknown GPUs to Low.
- A **one-shot** tidy-up (`:486-510`) deletes shipped-asset copies left in `Documents` by
  older installs, guarded by a `.bundle-assets-tidied` sentinel. `Maps` is deliberately
  excluded — that is where user maps live. `Documents` is user-visible via the Files app,
  so anything added later must never be touched.

### Desktop-only workarounds

- `FilterSoftwareVulkanICDs()` (`:202-255`) sets `VK_DRIVER_FILES` to hardware-only ICDs,
  dodging a `libvulkan_lvp.so` static-init crash under Mesa/LLVM 20.x. Skipped if the user
  already set `VK_DRIVER_FILES`/`VK_ICD_FILENAMES`.
- `FilterPipeWireOpenAL()` (`:279-302`) is `#if defined(__linux__)` only. Its own comment
  (`:288-291`) admits it is best-effort: openal-soft reads these env vars from a static
  constructor before `main()`, so the authoritative fix is in the launch scripts.

---

## 3. `SDL3GameEngine` — the event pump

`pollSDL3Events()` (`SDL3GameEngine.cpp:871-1021`) handles, and only handles:

| SDL event | Action |
|---|---|
| `QUIT`, `WINDOW_CLOSE_REQUESTED` | `m_quitting = true` |
| `WINDOW_FOCUS_GAINED` | `m_IsActive=true`; `TheMouse->regainFocus()` + `refreshCursorCapture()` |
| `WINDOW_FOCUS_LOST` | `m_IsActive=false`; `SDL_StopTextInput`; `TheMouse->loseFocus()` |
| `DID_ENTER_BACKGROUND` / `DID_ENTER_FOREGROUND` (iOS) | mirror the focus handling for mouse state |
| `WINDOW_MOUSE_ENTER` / `LEAVE` | `TheMouse->onCursorMovedInside/Outside()` |
| `KEY_DOWN` / `KEY_UP` | F9 → `AudioDebugDump`; then `SDL3Keyboard::addSDLEvent()` |
| `TEXT_INPUT` | `forwardTextInputEvent()` |
| `MOUSE_MOTION/BUTTON_DOWN/BUTTON_UP/WHEEL` | iOS: drop if `which == SDL_TOUCH_MOUSEID`; then `SDL3Mouse::addSDLEvent()` |
| `FINGER_DOWN/MOTION/UP/CANCELED` (iOS) | `handleTouchEvent()` |
| `WINDOW_RESIZED` | `handleWindowEvent()` — **empty, a TODO** (`:1152-1156`) |

Not handled: `SDL_EVENT_TERMINATING`, `SDL_EVENT_LOW_MEMORY`, `SDL_EVENT_WINDOW_MINIMIZED`.
SDL's own header states TERMINATING and LOW_MEMORY "must be handled in a callback set with
`SDL_AddEventWatch()`" — see §8.

The `event.motion.which == SDL_TOUCH_MOUSEID` test at `:972` is applied to button and
wheel events too, via the union. That is safe but non-obvious: `SDL_MouseMotionEvent`,
`SDL_MouseButtonEvent` and `SDL_MouseWheelEvent` all place `SDL_MouseID which` immediately
after `windowID` (verified in `build/ios-vulkan/_deps/sdl3-src/include/SDL3/SDL_events.h:459,
478, 498`), so all three alias to the same offset.

### Text input (`:1024-1087`)

`updateTextInputState()` is called once before the poll loop and again after **every**
event (`:877` and `:1009`). It calls `SDL_StartTextInput`/`SDL_StopTextInput` based on
whether `TheWindowManager->winGetFocus()` has the `GWS_ENTRY_FIELD` style.

**On iOS this is what raises and dismisses the software keyboard** — SDL's UIKit backend
implements `SDL_StartTextInput` by making a hidden `UITextField` first responder
(`sdl3-src/src/video/uikit/SDL_uikitviewcontroller.m:494-497,725-731`). There is no other
mechanism; if you break the focus tracking, on-device text entry dies silently.

`forwardTextInputEvent()` decodes UTF-8 by hand (`DecodeNextUtf8Codepoint`, `:664-715`) and
posts each codepoint as `GWM_IME_CHAR` to `m_TextInputFocusWindow`. Non-BMP codepoints and
UTF-16 surrogates are dropped (`:1076-1082`) because `WideChar` is 16-bit.

### iOS app lifecycle (`:67-123`, `:787-791`, `:819-828`)

Two atomics, `s_appBackgrounded` and `s_appInactive`, set from an `SDL_AddEventWatch`
callback registered in `init()` at `:790`. `SDL3GameEngine::update()` checks
`iosShouldPauseRendering()` **after** polling (`:824`) and, if set, does `SDL_Delay(50)`
and returns without calling `GameEngine::update()` — no sim, no render, but events keep
draining.

The stated reason (`:81-90`): acquiring a Metal drawable while iOS owns the `CAMetalLayer`
drives MoltenVK into an unrecoverable surface state across repeated suspend/switcher
cycles.

An event *watch* rather than the poll loop is correct, and SDL says so explicitly:
`SDL_events.h:92-112` marks `TERMINATING`, `LOW_MEMORY`, and all four
`WILL/DID_ENTER_BACKGROUND/FOREGROUND` events as "must be handled in a callback set with
`SDL_AddEventWatch()`".

**But the two-flag design is more redundant than the comment implies.** In SDL3's UIKit
backend, `applicationWillResignActive` maps to `SDL_OnApplicationWillEnterBackground()`
(`SDL_uikitevents.m:81-84`), which sends `SDL_EVENT_WINDOW_MINIMIZED`, calls
`SDL_SetKeyboardFocus(NULL)` — which emits `SDL_EVENT_WINDOW_FOCUS_LOST`
(`src/events/SDL_keyboard.c:347`) — *and* sends `SDL_EVENT_WILL_ENTER_BACKGROUND`
(`src/video/SDL_video.c:6195-6205`). So opening Control Center already sets
`s_appBackgrounded`; `s_appInactive` never fires alone. It is harmless insurance, not a
distinct code path. Conversely `SDL_OnApplicationDidEnterForeground` (`SDL_video.c:6217-6228`)
sends `DID_ENTER_FOREGROUND` *before* restoring keyboard focus, so both flags clear on
resume in the order the watcher expects.

The watch is never removed (no `SDL_RemoveEventWatch`), which is fine given `_exit()`.

---

## 4. `SDL3Mouse`

### Ring buffer

`SDL_Event m_eventBuffer[256]` with `m_nextFreeIndex`/`m_nextGetIndex`
(`SDL3Mouse.h:100-104`). Empty slots are marked with the `SDL_EVENT_FIRST` (== 0) sentinel.

- `addSDLEvent()` (`:828-858`) filters to the four mouse types, then **silently drops** the
  event if the buffer is full (`:843-848`). Real capacity is 255, not 256, because
  full is `next+1 == get`.
- `getMouseEvent()` (`:582-601`) is the `Mouse` virtual the engine drains through. It
  returns `MOUSE_NONE` on the sentinel.
- `Mouse::updateMouseData()` (`Core/.../Input/Mouse.cpp:152-187`) drains until `MOUSE_NONE`
  or 256 events, then sets `m_eventsThisFrame = index - 1`.

### Translation and coordinate scaling

`translateEvent()` (`:869-916`) dispatches to `translateMotionEvent`/`translateButtonEvent`/
`translateWheelEvent`, then **overwrites `result->pos`** with the output of
`scaleMouseCoordinates()` (`:912-915`).

That overwrite makes the `SDL_GetMouseState()` call inside `translateWheelEvent`
(`:748-752`) dead code — its result is always replaced by the scaled
`event.wheel.mouse_x/mouse_y`. Do not "fix" the wheel position by editing
`translateWheelEvent`; edit `translateEvent`.

`scaleMouseCoordinates()` (`:778-817`) maps SDL window **points** to the game's internal
resolution:

- If `TheDisplay->getViewportRect()` returns true, the raw point is offset and clamped into
  the pillarbox rect and scaled by `internal/pillarbox`. `W3DDisplay::getViewportRect`
  forwards to `DX8Wrapper::Pillarbox_Get_Rect` (`Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp:296-305`),
  which divides by `s_pixelDensity` — so the rect it returns is in *points*, matching the
  raw event coordinates. The units are consistent; this is easy to misread as a bug.
- Otherwise a plain `internal/windowSize` ratio.
- If `SDL_GetWindowFromID(windowID)` fails or `TheDisplay` is null, **it returns the raw
  coordinates unscaled** and logs nothing. On iOS, where internal resolution is 3× window
  points, that is a silent 3× error. This is why the synthetic events must carry a valid
  `windowID` (see the comment at `:200-204`).

Timestamps are converted from SDL nanoseconds to milliseconds (`:658, :677, :756`).

Button events set `MBS_DoubleClick` only on `down && clicks >= 2` (`:708, :720, :732`); the
comment at `:704-707` records why doing it on UP left `GadgetPushButton` stuck in
`WIN_STATE_SELECTED`.

### Cursors, capture, visibility

- `init()` sets `m_inputMovesAbsolute = TRUE` (`:375`). Without it `Mouse::processMouseEvent`
  uses `MOUSE_MOVE_RELATIVE` and *adds* the absolute coordinate to the tracked position each
  event, so the internal cursor drifts away instantly.
  Note the call ordering in `GameClient::init()`: `initCursorResources()` runs at
  `GameClient.cpp:323` but `TheMouse->init()` only at `:408`.
- `.ani` cursors are parsed by hand as RIFF/ACON (`:116-356`) and turned into
  `SDL_CreateAnimatedCursor`. Linux has an extra `SDL_CreateColorCursor` fallback
  (`:313-325`) that Darwin does not.
- `m_directionFrame` is initialised to 0 (`:143`) and **never written again**. Directional
  cursors (`MAX_2D_CURSOR_DIRECTIONS == 8`) therefore always render direction 0.
- `setVisibility()` (`:501-511`) always calls `Mouse::setVisibility(TRUE)` regardless of the
  argument, because there is no W3D-drawn cursor on this path and `setCursor()` falls back
  to `NORMAL` when `!m_visible`. Consequence: `TheMouse->isVisible()` is permanently true,
  and anything that tries to hide the cursor (cinematics, etc.) cannot.
- `capture()`/`releaseCapture()` (`:534-571`) call `SDL_CaptureMouse` + `SDL_SetWindowMouseGrab`
  and notify the base class via `onCursorCaptured()`, which `LookAtTranslator::canScrollAtScreenEdge()`
  (`LookAtXlat.cpp:115-118`) requires for screen-edge scrolling.

---

## 5. `SDL3Keyboard`

Same ring-buffer shape (`SDL_Event m_eventBuffer[256]`, sentinel `SDL_EVENT_FIRST`).

- `addSDLEvent()` (`:182-210`) drops SDL auto-repeat (`event.key.repeat`) at `:194`, because
  the engine has its own repeat in `Keyboard::checkKeyRepeat()` and the two together
  double-applied edits like Backspace.
- `getKey()` (`:127-171`) sets `keyDownTimeMsec = timeGetTime()` rather than the SDL
  timestamp (`:164`) so it shares a clock with `checkKeyRepeat()`.
- `getCapsState()` returns 0 unconditionally (`:116-121`), so `KEY_STATE_CAPSLOCK` is never
  set in `m_modifiers`.
- `getKeyboard()` returns `nullptr` (`:103-108`).

### `translateScanCodeToKeyVal` — the sharp edge

`SDL3Keyboard.h:71` declares it `KeyVal translateScanCodeToKeyVal(unsigned char scan)` and
the body casts back with `(SDL_Scancode)scan` (`:248`). Every scancode the table maps
happens to be ≤ 230 (`SDL_SCANCODE_RALT`), so nothing currently truncates — but SDL3
scancodes run well past 255, and any future entry above that will alias onto a low
scancode. **Widen the parameter before adding media/keypad/international keys.**

The map covers: Esc, Return, KP_Enter, Space, Tab, Backspace, Delete, Home, End, PageUp,
PageDown, L/R Shift/Ctrl/Alt, four arrows, F1–F12, digits 0–9, letters A–Z. Everything
else returns `KEY_NONE` (`:327-330`).

That is not a harmless gap. `Keyboard::updateKeys()` drains with
`while (m_keys[index++].key != KEY_NONE)` (`Core/.../Input/Keyboard.cpp:131`), so a
`KEY_NONE` **terminates the drain for that frame**. Pressing an unmapped key (backtick,
minus, equals, brackets, comma, period, slash, numpad digits, Insert, CapsLock, …) is not
merely ignored — it truncates the rest of that frame's key batch. The remaining events stay
in the ring and are picked up next frame, so the symptom is a one-frame stall rather than
lost input, but it is worth knowing when debugging "my key does nothing".

---

## 6. iOS touch → mouse: the gesture machine

All in the anonymous namespace at `SDL3GameEngine.cpp:143-659`, driven by
`handleTouchEvent()` (`:331`) from the poll loop plus `updateTouchLongPress()` (`:645`) once
per pump.

State lives in a single file-static `TouchState s_touch` (`:189`). Finger coordinates arrive
normalised 0..1 and are multiplied by `SDL_GetWindowSize()` — **points**, matching what
`scaleMouseCoordinates` expects.

### Constants (`:191-196`)

| Name | Value | Meaning |
|---|---|---|
| `LONG_PRESS_MS` | 600 | stationary single finger → right click |
| `TAP_DEAD_ZONE_PX` | 8 | Manhattan travel that turns a tap into a drag |
| `GESTURE_COMMIT_PX` | 24 | travel needed to lock a two-finger gesture into zoom or pan |
| `PINCH_STEP_RATIO` | 0.06 | 6 % spread change per wheel tick |
| `SCROLL_STEP_PX` | 36 | vertical centroid travel per wheel tick in shell menus |
| `TWO_FINGER_TAP_MS` | 250 | max duration of a two-finger "dismiss" tap |

### Phases (`:146-154`)

`IDLE → PENDING → {DRAGGING | LONGPRESSED | PINCH | SCROLL | PAN} → IDLE`

`PINCH` additionally carries an `Intent` (`UNDECIDED | ZOOMING | PANNING`, `:183-186`) that
is decided once per gesture and then held — see the comment at `:176-182` for why
per-event classification failed.

### The full gesture map

| Gesture | Synthesised input | Code |
|---|---|---|
| **1 finger down** | `MOUSE_MOTION` to the touch point only. No button. | `:354-375` |
| **1 finger tap** (lift before 600 ms, < 8 px) | `MOTION` + `LMB down` + `LMB up`, all at the *press* point | `:558-571` |
| **1 finger drag** (≥ 8 px) | `MOTION` to press point, `LMB down` there, then `MOTION` tracking the finger; `LMB up` on lift | `:435-449`, `:572-574` |
| **1 finger hold 600 ms, stationary** | `MOTION` + `RMB down` + `RMB up` at the press point; phase → `LONGPRESSED`, everything swallowed until lift | `:645-657` |
| **1 finger touch cancelled** (call, notification, palm) | nothing — explicitly *not* a tap | `:562-564` |
| **2 fingers, shell menu up** | phase `SCROLL`: `MOUSE_WHEEL` ±1 per 36 px of vertical centroid travel. No button ever. Content follows the fingers (drag down = wheel-up = earlier entries). | `:275-280`, `:450-467` |
| **2 fingers, in game, spread ≥ 24 px and ≥ centre travel** | intent `ZOOMING`: `MOUSE_WHEEL` ±1 per 6 % spread change. Camera untouched, no button. | `:485-486`, `:504-515` |
| **2 fingers, in game, centre travel ≥ 24 px first** | intent `PANNING`: rebase, `MOTION` + `RMB down` at the current centroid, then `MOTION` tracking the centroid | `:487-501`, `:516-521` |
| **2 fingers, below both thresholds** | **nothing is emitted** — this is the fix for camera creep | `:522-524` |
| **2 finger quick tap** (lift ≤ 250 ms, still `UNDECIDED`) | `MOTION` + `RMB down` + `RMB up` — deselect / cancel / back | `:599-608` |
| **3rd finger during `PINCH`, normal play** | phase `PAN`: `MOTION` to the 3-finger centroid, `RMB down` if not already held; cursor tracks the 3-finger centroid | `:409-412`, `:315-329`, `:529-542` |
| **3rd finger during `PINCH`, building placement pending** | `MOTION` + `LMB down` at the 2-finger centroid → the engine's `m_placeAnchorInProgress` rotate gesture; cursor then follows **finger 3 alone**; `LMB up` on lift commits the building at that facing | `:397-408`, `:533-538`, `:576-581` |
| **2 fingers lifted during placement, not after a zoom** | `MOTION` + `LMB down` + `LMB up` at the centroid = place the building here | `:609-624` |
| **4 fingers down** (any phase) | `AudioDebugDump("four-finger tap")` — dumps the OpenAL diagnostic ring to the log | `:344-347` |
| **5 fingers down** | synthetic `ESC` key down+up straight into `SDL3Keyboard::addSDLEvent` | `:348-353`, `:244-262` |
| **F9** (hardware keyboard, all platforms) | `AudioDebugDump("F9 pressed")` | `:946-949` |

Placement mode is detected once, at the start of the two-finger gesture:
`suppressRightButton = (TheInGameUI->getPendingPlaceType() != nullptr)` (`:297-298`). While
it is set, the right button is never pressed, because RMB cancels placement.

### Two design decisions that explain most of the code

**Nothing is emitted on finger-down except a motion.** A finger landing could still become
a tap, a drag-box, a long-press, or the first finger of a pinch. A premature LMB down+up is
a real click to the game (it sets a rally point when a production building is selected), so
all button output is deferred (`:356-358`).

**The finger-down motion exists purely for hover.** The comment at `:366-373` is the good
one: the Generals Challenge general-select buttons are checkboxes that ignore a click unless
`WIN_STATE_HILITED` was set by a prior mouse-enter. A real mouse hovers before it clicks; a
synthetic tap that teleports and clicks in the same instant never hilites, so only the
default item ever responded. The motion on finger-down gives the GUI a frame or two of hover
before the tap commits.

---

## 7. Where a touch actually ends up

For the three-finger camera pan, the chain is:

1. `beginPan` sends `RMB down` at the centroid → `MSG_RAW_MOUSE_RIGHT_BUTTON_DOWN`.
2. `LookAtTranslator` records `m_anchor` at that pixel and `setScrolling(SCROLL_RMB)`
   (`LookAtXlat.cpp:257-266`).
3. Every `MSG_FRAME_TICK` thereafter computes `vec = m_currentPos - m_anchor`, normalises it,
   scales by `vecLength` and the scroll factors, and calls `TheTacticalView->scrollBy()`
   (`LookAtXlat.cpp:455-481`).
4. `View::scrollBy` adds the delta to the camera's world position
   (`Core/GameEngine/Source/GameClient/View.cpp:152-157`).

So RMB scroll is **velocity-based**: the camera keeps moving for as long as the cursor sits
away from the anchor, with no further input. That is exactly why the code refuses to press
RMB until the gesture has committed to panning (`:302-308`) — pressing it on the second
finger-down meant every pinch ran on top of a live scroll.

Zoom: `MSG_RAW_MOUSE_WHEEL` → `LookAtXlat.cpp:414-421`, which reads **only** argument 1 (the
spin) and discards the appended cursor position. Every build, desktop included, zooms toward
the camera's look-at point rather than the pointer. Anchoring zoom at the pointer is an
engine change, not a platform-layer one.

---

## 8. Gotchas

**The gesture map comment at the top of the file is stale, and inverted.**
`SDL3GameEngine.cpp:136-138` claims the three-finger drag makes "terrain follow the fingers
rather than running from them". That was true of commit `82ddc73a2`, which mirrored the
synthetic cursor about the gesture anchor (`2*anchor - centroid`). Commit `d16c306ce`
explicitly reverted the mirror — "the engine's right-drag scroll direction was correct all
along" — and the current code sends the raw centroid (`:517`, `:539`). Combined with
`View::scrollBy` adding the delta to the camera position, dragging fingers right moves the
*camera* right, so terrain slides left, i.e. it runs *away* from the fingers. Meanwhile the
shell-menu `SCROLL` path genuinely is content-follows-finger (`:451-455`). **The same file
now uses opposite scroll conventions for menus and for the world, and the header comment
describes neither correctly.** Fix the comment before it misleads someone into "fixing" the
direction again.

**Every synthetic mouse event has `timestamp == 0`.** `sendSyntheticMouse` does `SDL_zero(ev)`
(`:206`) and never sets `timestamp`, so `MouseIO::time` is 0 for all touch input
(`SDL3Mouse.cpp:658/677/756`). `Mouse::isClick()` (`Core/.../Input/Mouse.cpp:389-405`)
returns false if `currentMouseClick - previousMouseClick > m_dragToleranceMS`; with both
zero, that clause is permanently false. **On iOS the engine's click-vs-drag test degenerates
to a pure distance test.** Today that is mostly benign (a stationary hold becomes a
long-press anyway), but a three-finger pan that returns near its start will be classified as
a right *click* by `SelectionXlat`/`CommandXlat` and deselect your units. If you ever need
duration semantics on touch, set `ev.*.timestamp = SDL_GetTicksNS()` in `sendSyntheticMouse`.

**`s_touch` is a process-global that nobody resets.** `SDL3GameEngine::reset()` (`:802`) does
not clear it, and neither does anything else. A phase left mid-gesture across a level
transition persists. The `activeFingers` counter (`:344`, `:547-549`) is likewise only
decremented by `FINGER_UP`/`FINGER_CANCELED`; if iOS ever suspends mid-touch without
delivering those, the 4- and 5-finger triggers drift out of alignment for the rest of the
session.

**Lifting the *second* finger during a three-finger pan drops the machine to `IDLE` while
two fingers are still on the glass.** `:550-556` accepts finger2's `FINGER_UP` in `PAN`, the
`PAN` case releases RMB, and `:637` unconditionally sets `phase = IDLE`. Subsequent motion
from the still-down fingers is swallowed until every finger lifts and a new gesture starts.
Not harmful, but it explains "the camera stops responding until I lift everything".

**A rested finger followed by a second finger fires a spurious right-click.** `PENDING` is
both "maybe a tap" and "first finger of a two-finger gesture". Rest one finger for 600 ms
before landing the second and `updateTouchLongPress` (`:647`) will already have emitted the
deselect RMB click.

**Input during load screens is buffered, and past 255 events silently dropped.**
`LoadScreen.cpp:161` calls `serviceWindowsOS()` → `pollSDL3Events()`, which fills the mouse
and keyboard rings, but `Mouse::update()`/`Keyboard::update()` are not running, so nothing
drains. `SDL3Mouse::addSDLEvent` (`:843-848`) and `SDL3Keyboard::addSDLEvent` (`:199-203`)
both return without logging when full. The long-press timer also fires during loads.

**`SDL_EVENT_TERMINATING` and `SDL_EVENT_LOW_MEMORY` are not handled at all.** SDL's header
(`SDL_events.h:92-97`) says both must be handled in an event watch — the watch exists
(`:790`) but does not switch on them. On an iOS memory kill the process just dies; the only
reason anything survives is the line-buffered log sink and the `-prev.log` rotation.

**`handleWindowEvent` is empty.** `SDL_EVENT_WINDOW_RESIZED` reaches `:1152` and does
nothing. Resizing a desktop window does not tell the graphics subsystem or update
`TheDisplay`, so mouse scaling and rendering stay on the old internal resolution. iOS is
immune only because `UIRequiresFullScreen` + landscape-only means the size never changes.

**`scaleMouseCoordinates` fails open.** A bad `windowID` or a null `TheDisplay` returns raw
coordinates with no log line (`SDL3Mouse.cpp:780-784`). On iOS that is a silent 3× offset on
every click. Any new synthetic-event path must set a real `SDL_GetWindowID(window)`.

**Widening the scancode map requires widening the signature.**
`translateScanCodeToKeyVal(unsigned char)` currently happens to be safe; see §5.

**`m_visible` is pinned true.** See §4 — nothing can hide the cursor on this path.

**Screen-edge scrolling is live on touch, but the band is 3 px.**
`canScrollAtScreenEdge()` needs `isCursorCaptured()` (`LookAtXlat.cpp:115-118`), which
`SDL3Mouse::capture()` does set via `onCursorCaptured(true)` (`:550`), and the default
`CursorCaptureMode` includes fullscreen game and menu (`Mouse.h:150-153`). The synthetic
cursor parks wherever the last touch was. `edgeScrollSize` is 3 *internal* pixels
(`LookAtXlat.cpp:80`), which at an injected internal width of ~2868 is about one point — so
this is a theoretical rather than practical hazard. I have not reproduced it on device;
flagging it because the reasoning is not obvious from either file alone.

---

## 9. Things I could not establish

- **Whether the two-finger pan/zoom thresholds feel right on an iPad** as opposed to a
  phone: `GESTURE_COMMIT_PX` and `SCROLL_STEP_PX` are absolute point values with no
  DPI or screen-size term. There is no evidence in the tree of tuning for anything but the
  author's device.
- **What `Data/INI/Mouse.ini` actually sets** for `DragTolerance` / `DragToleranceMS`
  (`Core/.../Input/Mouse.cpp:110-112`). Those values live in the shipped `.big` archives,
  not the repo, and they decide how forgiving the click-vs-drag test is.
- **Whether SDL delivers `FINGER_UP`/`FINGER_CANCELED` for fingers still down when iOS
  suspends the app.** SDL does emit `FINGER_CANCELED` from `touchesCancelled:`
  (`sdl3-src/src/video/uikit/SDL_uikitview.m:404,434`), but whether UIKit calls it on
  suspension was not verified. This determines whether the `activeFingers` drift described
  in §8 is real.
- `SDL3Keyboard::translateKeyEvent` is declared (`SDL3Keyboard.h:73`) but has no definition
  in the `.cpp`; it appears to be dead. Likewise `SDL3GameEngine::handleKeyboardEvent` /
  `handleMouse*Event` (`:1093-1147`) and the `addSDL3Mouse*Event` / `addSDL3KeyEvent`
  wrappers are labelled legacy and have no callers I could find in the non-Windows path.
  I did not remove or exhaustively prove them unreachable.
