# Architecture Documentation

GeneralsX is EA's 2003 *Command & Conquer: Generals — Zero Hour* engine (~1.6M lines
of VC6-era C++, GPL v3) running natively on macOS and iOS. The strategy of the port is
**keep the 2003 engine, replace the platform underneath it**: the game still issues
Direct3D 8 calls, still parses `.big` archives and `.ini` files, still runs a fixed
30 Hz deterministic logic frame — but D3D8 is now implemented by DXVK → Vulkan →
MoltenVK → Metal, windowing and input by SDL3, audio by OpenAL instead of Miles, video
and audio decode by FFmpeg, and fonts by FreeType. Layering runs: **game code**
(`GeneralsMD/Code/GameEngine/`) → **device layers** (`*/GameEngineDevice/`, the
swappable seam where SDL3/OpenAL/W3D backends plug in) → **Westwood libraries**
(`Core/Libraries/Source/WWVegas/`, notably the WW3D2 renderer) → **compat shims**
(`GeneralsMD/Code/CompatLib/`, Win32 APIs reimplemented on POSIX) → **third-party
runtime**. Code shared by both games lives in `Core/`; anything that diverged lives in
`GeneralsMD/` — and **editing the wrong copy is the most common way to lose an hour
here**.

```
       Game logic + client  ── GameLogic (30 Hz, deterministic) / GameClient (render rate)
                │
       Device layers ─────── SDL3GameEngine · OpenALAudioManager · W3DDisplay · Std*FileSystem
                │
       WWVegas libs ──────── WW3D2 (DX8Wrapper is the single D3D choke point) · WWLib
                │
       Compat + runtime ──── CompatLib · DXVK → Vulkan → MoltenVK → Metal · OpenAL · FFmpeg
```

---

## The documents

| Document | What it covers |
|---|---|
| [RENDERING.md](RENDERING.md) | The W3D renderer from `W3DDisplay::init()` down to Metal: device bring-up, the `DX8Wrapper` choke point, the pillarbox offscreen-RT path, textures and the D3DX shims, fonts, and what "shader" means in each of its two unrelated senses. |
| [AUDIO.md](AUDIO.md) | The OpenAL backend that replaced Miles: the sample/3D-sample/stream split, the full lifecycle of one sound, the decoded-sample cache, FFmpeg streaming with its refill/underrun/EOF state machine, and the two different volume formulas. |
| [INPUT_AND_PLATFORM.md](INPUT_AND_PLATFORM.md) | `SDL3Main.cpp` process bootstrap (load-bearing ordering, iOS argv injection, crash handlers, stderr sink), the `SDL3GameEngine` event pump and its ring buffers, and the iOS touch→mouse gesture machine in full. |
| [GUI_AND_MENUS.md](GUI_AND_MENUS.md) | The retained-mode window system: the `.wnd` format and its parser, `GameWindow`/`GameWindowManager`, every gadget type and its message protocol, name-based callback binding via `FunctionLexicon`, and the `Shell` screen stack. Includes a recipe for adding controls in code, since the `.wnd` editor is Windows-only. |
| [GAME_LOGIC.md](GAME_LOGIC.md) | The simulation half: the frame loop and `FramePacer`, the message-stream translator chain that turns a click into an order, the sleepy update scheduler, `Object`/`Drawable` and the module system, AI state machines, and what determinism actually requires. |
| [DATA_AND_ASSETS.md](DATA_AND_ASSETS.md) | How the game finds and overrides data: the BIG format, the one flat VFS namespace and its load-order/override rules, INI parsing and override chains, `.csf`/`.str` string tables, the map cache, and save games. |
| [BUILD_AND_PACKAGING.md](BUILD_AND_PACKAGING.md) | The four parallel dependency mechanisms (vcpkg, FetchContent, meson `ExternalProject`, pre-installed SDKs), both CMake presets, how DXVK and MoltenVK are obtained, and the macOS and iOS packaging scripts end to end. |
| [COMPRESSION.md](COMPRESSION.md) | The five codecs behind `CompressionManager`, how a four-byte magic alone selects the decoder, which call sites take untrusted input (map enumeration, peer packets), and the destination-bounding work on RefPack with its verification evidence. |
| [NETWORKING.md](NETWORKING.md) | The LAN stack: why cross-architecture lockstep against a retail x86 binary cannot work, this fork's own ungated sim divergences, the POSIX socket defects that turned a failed bind into a main-thread hang, and how to actually set up an iPad↔macOS Direct Connect game. |

**Suggested reading order** if you are new: `BUILD_AND_PACKAGING` (get it building) →
`INPUT_AND_PLATFORM` (how the process starts) → `GAME_LOGIC` (how a frame works) →
then whichever of `RENDERING` / `AUDIO` / `GUI_AND_MENUS` / `DATA_AND_ASSETS` your task
touches.

---

## Where do I look if I want to change X

| I want to… | Read | Start in the code at |
|---|---|---|
| **Add a UI control** | GUI_AND_MENUS §8 (runtime recipe), §4 (gadget catalogue), §5 (callback binding) | `GeneralsMD/.../GUI/GUICallbacks/Menus/MainMenu.cpp` — two in-tree precedents; `GameWindowManager::gogoGadget*` |
| **Change touch behaviour** | INPUT_AND_PLATFORM §6 (full gesture map), §7 (where a touch ends up) | `GeneralsMD/Code/GameEngineDevice/Source/SDL3GameEngine.cpp:143-659` |
| **Fix an audio problem** | AUDIO §3 (lifecycle), §5 (streaming/underrun), §8 (gotchas) | `Core/GameEngineDevice/Source/OpenALAudioDevice/` — not the dead twin under `GeneralsMD/` |
| **Override game data** | DATA_AND_ASSETS §8 (six methods, least→most invasive), §3 (load order) | Loose files beat every archive; `SagePatch.ini` for `GameData` tweaks |
| **Add a command-line flag** | INPUT_AND_PLATFORM §2 (the two parse tables and their timing), DATA_AND_ASSETS §3 | `CommandLine.cpp:1157` `paramsForStartup[]` (parsed in `SDL3Main`) vs `:1180` `paramsForEngineInit[]` (parsed in `GameEngine::init`) — picking the wrong table is a silent no-op |
| **Change rendering** | RENDERING §4 (dispatch model), §5 (frame lifecycle), §6 (pillarbox), §11 (gotchas) | `Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp` — every D3D call funnels here |
| **Adjust the build** | BUILD_AND_PACKAGING §1 (which mechanism owns which dependency), §2 (presets), §6–7 (packaging scripts) | `CMakePresets.json`, `cmake/config-build.cmake`, `scripts/build/{ios,macos}/` |

---

## Porting history and methodology

These architecture documents describe *what the code is now*. For *how it got that way*:

- **[`../port/PORTING_PLAYBOOK.md`](../port/PORTING_PLAYBOOK.md)** — the case study:
  every decision, problem and fix in porting this specific game to iOS.
- **[`../port/PORTING_PATTERNS.md`](../port/PORTING_PATTERNS.md)** — the generalized
  methodology (translate vs. shim vs. swap vs. stub, portability bug taxonomy,
  determinism gates) for when there is no existing port to build on.
- **[`../port/IPAD_BUILD_JOURNAL.md`](../port/IPAD_BUILD_JOURNAL.md)** — a narrated
  clean-machine build onto an iPad, including all three upstream defects that break
  the documented setup procedure and their root causes.
- **[`../port/RELEASE_CHECKLIST.md`](../port/RELEASE_CHECKLIST.md)** — pre-release gates.

---

## Conventions used in these documents

- Non-obvious claims cite `file:line`. Line numbers were read at the time of writing
  and drift; **the quoted identifier or enclosing function name is the reliable
  anchor**, so grep for it rather than trusting the number.
- Several documents were written against a working tree with uncommitted changes to
  `SDL3Main.cpp`, `SDL3GameEngine.cpp`, `GameEngine.cpp`, `CommandLine.cpp` and
  `GlobalData.cpp`; those say so in their headers.
- Every document ends with a **Gotchas** section ordered by how much time the mistake
  is likely to cost, and a short honest list of things the author could not pin down.
  Both are worth reading before you start, not after.
