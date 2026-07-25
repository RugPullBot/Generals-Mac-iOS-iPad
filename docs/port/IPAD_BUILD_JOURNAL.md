# iPad Build Journal — clean-machine run, 2026-07-25

A complete record of building this port from a fresh clone onto an **iPad Air 11" (M3)**,
including every failure encountered and its root cause.

The headline: **the documented setup procedure in `README.md` cannot succeed on a clean
machine.** Three separate upstream defects break it, in sequence. All three are fixed in
this fork; see [Upstream defects](#upstream-defects) below.

Result: the game runs. Menus, terrain, water, unit sprites and general portraits all
render correctly at native panel resolution.

---

## 1. Environment

| | |
|---|---|
| Host | Mac mini, Apple Silicon (arm64), 16 GB, macOS 26.5.2 |
| Xcode | 26.2 (17C52), iPhoneOS 26.2 SDK |
| Target | iPad Air 11" M3 (`iPad15,3`), iPadOS 26.5.2, 8 GB |
| Signing | Free personal team — 7-day provisioning profile |
| Vulkan SDK | LunarG 1.4.350.1 |
| MoltenVK | v1.4.1 (dynamic, from Khronos releases) |
| Assets | Steam app `2732960`, ~2.8 GB, 36 `.big` archives |

The 8 GB of unified memory matters: the known iPad failure mode is OS termination past
roughly 3 GB resident. A 4 GB iPad is a materially worse target.

---

## 2. Procedure that actually works

Corrected end-to-end. Differences from `README.md` are called out inline.

```sh
brew install cmake ninja meson pkgconf xcodegen
brew install --cask steamcmd
git clone https://github.com/microsoft/vcpkg ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg
```

Install the **LunarG** Vulkan SDK (not Homebrew's). Note that
`https://sdk.lunarg.com/sdk/download/latest/mac/vulkan_sdk.dmg` now serves a **zip**
containing an installer `.app`, despite the `.dmg` extension — `hdiutil attach` fails with
`image not recognized`. Unzip it instead.

```sh
export VULKAN_SDK=$HOME/VulkanSDK/1.4.350.1/macOS
```

The iOS preset hard-requires `$VULKAN_SDK/lib/MoltenVK.xcframework/ios-arm64/libMoltenVK.a`
for link-time resolution; the dynamic framework fetched separately covers runtime `dlopen`.

```sh
git clone <fork> GeneralsX-src && cd GeneralsX-src

# NOTE --recursive. Without it the build fails; see Defect 1.
git submodule update --init --recursive references/fbraz3-dxvk

./scripts/build/ios/fetch-moltenvk.sh
./scripts/build/ios/stage-fonts.sh
./scripts/get-assets.sh <steam_username>

cmake --preset ios-vulkan
cmake --build build/ios-vulkan --target z_generals

GX_TEAM_ID=<team> GX_BUNDLE_ID=<bundle> ./scripts/build/ios/package-ios-zh.sh --install
```

**The clone path must not contain spaces.** See Defect 4.

### Verify artifacts, not exit codes

`PORTING_PATTERNS.md` §5 is right and worth repeating. Confirm the cross-build really
produced iOS binaries rather than silently falling back to host:

```sh
ar x build/ios-vulkan/vcpkg_installed/arm64-ios/lib/libz.a adler32.c.o
otool -l adler32.c.o | grep -A3 LC_BUILD_VERSION
#  platform 2      <- 2 = iOS. 1 = macOS means silent host fallback.
#  minos 16.0
```

Confirmed for this run: `platform 2`, `minos 16.0`, `sdk 26.2`, arm64.

---

## 3. Timings

Apple Silicon M3, 16 GB, ninja default parallelism.

| Stage | Wall clock |
|---|---|
| vcpkg arm64-ios deps (18 static libs; FFmpeg dominates) | 281 s |
| Reconfigure with deps cached | 3 s |
| Engine compile, 1290 targets | ~15 min |
| Package: assemble, bundle 2.7 GB, sign inside-out | ~4 min |
| `devicectl` install over USB | ~1 min |

Engine binary: 37 MB, arm64, `platform 2`, minos 16.0.

---

## Upstream defects

Four defects in the upstream repository, ordered by where they bite. Each blocks a clean
first-time setup.

### Defect 1 — DXVK nested submodules are never initialised

**Severity: blocks every fresh clone.**

`README.md` instructs:

```sh
git submodule update --init references/fbraz3-dxvk
```

This is not recursive. DXVK vendors its headers as its *own* submodules:

```
include/vulkan        -> KhronosGroup/Vulkan-Headers
include/spirv         -> KhronosGroup/SPIRV-Headers
include/native/directx-> Joshua-Ashton/mingw-directx-headers
subprojects/libdisplay-info
```

All four come up empty, and meson dies during the DXVK cross-configure:

```
Check usable header "vulkan/vulkan.h" : NO
meson.build:43:2: ERROR: Problem encountered: Missing Vulkan-Headers
```

**Fix:** `git submodule update --init --recursive references/fbraz3-dxvk`.

Worth considering a configure-time guard that checks for
`references/fbraz3-dxvk/include/vulkan/include/vulkan/vulkan.h` and fails with a pointed
message, since the meson error does not suggest the cause.

### Defect 2 — packaging never registers the target device

**Severity: install is rejected on any device Xcode has not already registered.**

`package-ios-zh.sh` builds the provisioning shell with:

```sh
-destination 'generic/platform=iOS'
```

A generic destination gives Xcode no specific device, so `-allowProvisioningUpdates` has no
device to register and simply reuses whatever cached profile exists. On a machine that has
previously paired *some other* iOS device, the resulting profile authorises that device and
not the one you are installing to — and a development profile only installs on devices
listed in it.

Observed here: profile contained one UDID, belonging to an unrelated iPhone that had been
paired months earlier. The target iPad was absent.

**Workaround** — force registration once, then repackage:

```sh
xcodebuild -project ios/GeneralsXZH.xcodeproj -scheme GeneralsXZH \
  -destination "platform=iOS,id=<device-udid>" \
  -allowProvisioningUpdates build
```

**Fix:** when a device is connected, target it by UDID rather than using a generic
destination.

### Defect 3 — device detection matches a state string `devicectl` no longer emits

**Severity: `--install` always fails, silently.**

```sh
DEVICE_ID=$(xcrun devicectl list devices 2>/dev/null | awk '/connected/{print $(NF-2); exit}')
```

Modern `devicectl` reports the state as **`available`**, not `connected`. `DEVICE_ID` comes
back empty.

The script does have an error path for that case, but it is unreachable: under
`set -euo pipefail` the failing pipeline terminates the script *before* the `if [[ -z ... ]]`
check runs. Net effect is an exit status of 1 with the last output line being
`==> Installing to connected device` and no diagnostic at all.

**Fix:** match both `available` and `connected`, and make the empty-device-id branch
actually reachable.

### Defect 4 — `-L` path is emitted unquoted by FFmpeg's configure

**Severity: any clone under a path containing a space.**

Not strictly this repo's bug — FFmpeg's configure does not quote `--extra-ldflags=-L<path>` —
but it surfaces here because the build passes an absolute path straight through:

```
clang: error: no such file or directory: 'work/GeneralsX/build/ios-vulkan/vcpkg_installed/arm64-ios/lib'
```

The path was `/Users/<user>/Desktop/claude work/GeneralsX/...`; it split at the space.
Fails ~3 s into the vcpkg step.

**Fix:** document the constraint. A configure-time check rejecting
`CMAKE_SOURCE_DIR` containing whitespace would save an obscure hunt.

---

## Runtime defect — `MISSING: 'GUI:CustomMission'`

**Severity: cosmetic. Platform-independent — affects macOS and Linux builds too.**

The main menu renders a button labelled `MISSING: 'GUI:CustomMission'`.

The Steam re-release adds a **Custom Mission** feature. Its pieces are distributed as:

| Archive | Contents |
|---|---|
| `PatchWindow.big` | `Window\Menus\MainMenu.wnd` — the button, `TEXT = "GUI:CustomMission"` |
| `PatchINI.big` | window transitions referencing `MainMenu.wnd:ButtonCustomMission` |
| `PatchData.big` | **`Data\Patch.str`** (87 bytes) — the two strings |

`Data/Patch.str` in full:

```
GUI:CustomMission
"CUSTOM MISSION"
END

GUI:StartCustomMission
"START GAME"
END
```

The engine never loads it. `GameText.cpp` knows exactly two string sources:

```cpp
const Char *g_strFile = "data\\Generals.str";
const Char *g_csfFile = "data\\%s\\Generals.csf";
```

and selects between them exclusively (`GameText.cpp:318`):

```cpp
if ( m_useStringFile && getStringCount( g_strFile, m_textCount ) )   // Generals.str
else if ( getCSFInfo( csfFile.str(), m_textCount, m_language ) )     // generals.csf
```

`Patch.str` appears nowhere in the codebase — zero references. `GUI:CustomMission` is
likewise absent from `Data\English\generals.csf` in `EnglishZH.big`, so the label can never
resolve and the engine falls back to its `MISSING: '<label>'` placeholder.

**Fix:** after the primary string table loads, additionally parse `Data/Patch.str` when
present and merge its entries. The existing `parseStringFile` path already handles the
format; it needs to become additive rather than exclusive.

---

## Host-environment notes

Not repo bugs, but each cost real time and is likely to recur.

**`find` shadowed on `PATH`.** `~/.local/bin/find` was a two-line wrapper around
libc-database's `find`, and `~/.local/bin` sorted first on `PATH`. Shell scripts invoking
bare `find` got the CTF tool:

```
Usage: .../libc-database/find name address [name address ...]
grep: db/*.symbols: No such file or directory
```

This silently broke `stage-fonts.sh`, which locates the extracted Liberation fonts with
`find`. Mitigated with a shim directory containing a symlink to `/usr/bin/find`, prepended
to `PATH` for build invocations only. Scripts wanting robustness here should use absolute
paths for `find`.

**Gatekeeper quarantine on steamcmd.** The Homebrew cask arrives with 29 quarantined files;
macOS presents a "Move to Trash" dialog per binary and kills the process:

```sh
xattr -dr com.apple.quarantine /opt/homebrew/Caskroom/steamcmd/<version>
```

---

## Signing and expiry

Free personal team. The generated profile:

```
Name:       iOS Team Provisioning Profile: <bundle-id>
Created:    2026-07-25
Expires:    2026-08-01        <- exactly 7 days
Entitlements: get-task-allow = true
```

Seven days is the free-tier ceiling; the paid Apple Developer Program yields one year. After
expiry the app stops launching until re-signed and reinstalled.

Note that `ProvisionedDevices` is authoritative — an app signed against a profile that does
not list the target device will be refused at install time. See Defect 2.

---

## Bundle layout as shipped

```
GeneralsXZH.app                     2.7 GB
├── GeneralsXZH                     37 MB, arm64, platform 2, minos 16.0
├── Frameworks/
│   ├── MoltenVK.framework          runtime dlopen target for DXVK
│   ├── libdxvk_d3d8.0.dylib
│   ├── libdxvk_d3d9.0.dylib
│   ├── libSDL3.0.dylib
│   ├── libSDL3_image.0.dylib
│   ├── libopenal.1.dylib
│   └── libgamespy.dylib
└── GameData/                       2.7 GB
    ├── *.big                       20 archives
    ├── ZH_Generals/                base-game data
    ├── fonts/                      arial, arialbold, couriernew, timesnewroman
    ├── dxvk.conf                   samplerAnisotropy = 16
    └── DefaultOptions.ini          seeds High LOD on first run
```

### On `--dev` mode

`--dev` skips the 2.7 GB asset copy, and the engine then falls back to `~/Documents`
(`SDL3Main.cpp:361`). Attractive for iteration, and for shrinking update downloads from
~3 GB to ~100 MB. Two caveats make it unsuitable as a shipping configuration without
further work:

1. Fonts, `dxvk.conf` and `DefaultOptions.ini` are all copied inside the same
   `if [[ "${DEV_MODE}" != "1" ]]` block, so a `--dev` build has none of them. No fonts
   means no text at all — the script's own error message says as much.
2. `DefaultOptions.ini` seeding is gated on `usingBundleData` (`SDL3Main.cpp:380`). In
   Documents mode it never runs, so the 2003-era GPU auto-detect applies and drops unknown
   hardware to Low LOD with quarter-resolution textures — which `PORTING_PLAYBOOK.md`
   identifies as the leading cause of "looks worse than my PC".

Also note the one-time tidy-up at `SDL3Main.cpp:405`: a bundle-mode launch **deletes**
`.big` files, `Data`, `ZH_Generals`, `fonts` and `dxvk.conf` from `Documents`. Installing a
bundled build after an external-assets build therefore destroys the externally-staged
assets. The two layouts cannot be mixed.

A proper external-assets mode would bundle fonts and config (about 1.5 MB) while leaving
only the `.big` archives external, and would ungate the options seeding.

---

## Runtime defects found on device

### Options > Extras crashed the process

**Severity: hard crash on a stock install. Platform-independent.**

Captured in the session log:

```
Shell::doPush() called with layoutFile='Menus/ExtrasMenu.wnd'
About to call TheWindowManager->winCreateLayout('Menus/ExtrasMenu.wnd')
winCreateLayout returned: 0x0
<log ends — process gone>
```

A successful push always logs `Shell::doPush() completed successfully` afterwards.
This one does not.

Three compounding causes:

1. `Shell::doPush` checked the layout for null with `DEBUG_ASSERTCRASH`, which
   compiles away in release builds. `linkScreen()` then dereferenced the null.
   *Every* push of an absent `.wnd` was an unexplained crash.
2. The Extras button is created unconditionally at runtime, but
   `Window\Menus\ExtrasMenu.wnd` exists in **none** of the 36 archives of a stock
   Steam install — base-game `ZH_Generals/` set included.
3. Nothing reported the fatal signal, so the log simply stopped mid-line.

Fixed: `doPush` refuses a null layout and names it; the button is created only
when the layout exists; fatal signals now self-report (below).

### Credits screen could not be exited

**Severity: soft-lock on touch platforms.**

`CreditsMenuInput` handled exactly one input — `GWM_CHAR` with `KEY_ESC`. There is
no on-screen back button, and iOS has no ESC key, so the only escape was killing
the app. Now `GWM_LEFT_UP`/`GWM_RIGHT_UP` dismiss it too. Verified on device.

### Fatal errors left no trace

Async-signal-safe handlers for `SIGSEGV`, `SIGBUS`, `SIGABRT`, `SIGILL`, `SIGFPE`,
`SIGTRAP` and `std::terminate` now write the cause plus a backtrace to the session
log, then restore the default action and re-raise so the OS still files its own
crash report. Handlers use raw `write()` only — no `printf`, no allocation.

Pull the log from a device with:

```sh
xcrun devicectl device copy from --device <core-device-uuid> \
  --domain-type appDataContainer --domain-identifier <bundle-id> \
  --source Documents --destination ./logs
```

### UNSOLVED — thin white slivers above menu button text

Small fragments of unrelated glyphs appear a few pixels above the text on menu
buttons (`MULTIPLAYER`, `CREDITS`, `EXIT GAME`, `CREATE GAME`, `DIRECT CONNECT`).
They appear only above *some* letters.

**Three theories investigated and disproven.** Recorded so they are not re-run:

| Theory | Why it is wrong |
|---|---|
| `d3d9.samplerAnisotropy = 16` in `dxvk.conf` forcing filtering on the font sampler | DXVK guards it: `if (samplerAnisotropy != -1 && cState.minFilter > D3DTEXF_POINT)` — point samplers are excluded (`d3d9_device.cpp:7112`) |
| Font atlas sampled with bilinear filtering | The engine requests point sampling (`render2dsentence.cpp:366-368`) and `FILTER_TYPE_NONE` maps to `D3DTEXF_POINT` (`texturefilter.cpp:126`) |
| Atlas rows packed with no vertical gutter, so a strip samples the row above | Added a gutter matching the existing horizontal `TEXTURE_OFFSET`; **the artifact was unchanged**. Reverted. |

Remaining lead: the half-texel UV inset, still commented out at
`render2dsentence.cpp:539` (`//uv_rect.Bottom += 0.5f;`), combined with UVs
normalised by `desc.Width` on both axes.

**Recommended next step: build the macOS target first.** It gives a seconds-long
iteration loop instead of a ~5 minute device cycle, and immediately settles
whether this is iOS-specific at all.

---

## Open items

- [x] Defect 1 — `--recursive` in README
- [x] Defect 2 — target the connected device by UDID
- [x] Defect 3 — accept `available`; make the error branch reachable
- [x] Missing-layout crash guard in `Shell::doPush`
- [x] Gate the Extras button on its layout existing
- [x] Fatal-signal reporting with backtrace
- [x] Credits dismissable by tap
- [x] Two-finger scroll in shell menus
- [ ] Defect 4 — reject source paths containing whitespace at configure time
- [ ] Menu text slivers (see above — build macOS first)
- [ ] `Patch.str` loading so `GUI:CustomMission` resolves. Needs a fourth string
      table in `GameText` (the existing three are fixed-size arrays behind sorted
      `bsearch` LUTs) *and* a decision on the button itself: `CustomMission` has
      zero handlers in the codebase, so fixing only the label yields a
      legitimate-looking button that silently does nothing
- [ ] Proper external-assets packaging mode (bundle fonts + config, externalise
      `.big`) — would cut update downloads from ~3 GB to ~100 MB
- [ ] HD asset mods. GenPatcher itself is a Windows deployment fixer and mostly
      irrelevant; the wanted pieces are data-only community packs (Control Bar Pro
      HD, AI Graphics & Lighting Remake, 4X Upscale). All are `.big`/textures/
      `.wnd`/INI and portable as-is. Sequence: UI and control bar first (small,
      high impact), measure resident memory, then decide on full 4x textures
      against the ~3 GB termination ceiling
- [ ] Investigate the two documented runtime issues: >3 GB memory termination, and
      backgrounding crashes mid-game
- [ ] Measure in-match frame rate (menu showed 28 fps against a 30 cap)
