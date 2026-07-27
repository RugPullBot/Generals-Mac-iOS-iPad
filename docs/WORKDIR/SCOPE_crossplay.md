# Scope — three-way cross-play: macOS + iPadOS + Windows x64

Goal, stated so it is unambiguously testable:

> **Karl's Mac, Karl's iPad and the Windows PC — all on the same WiFi — sit in one LAN lobby,
> start a match together, and play it to completion without a desync.**

Nothing short of that counts. "Windows compiles" is not progress toward this; it is a prerequisite
for the first step of it.

## Hardware actually available

| Peer | Detail |
|---|---|
| Mac mini M4 / 16 GB | build host, canonical repo at `~/GeneralsX-src`, game data at `~/GeneralsX/GeneralsZH/` |
| iPad Air 11-inch (M3), `iPad15,3` | paired; shows `unavailable` until plugged in and trusted |
| Windows 11 PC (`r0se-desktop`) | Karl's own machine. `User@192.168.10.89`, SSH key auth, MSVC 14.44 + ATL, clone at `C:\dev\GeneralsX` |

All three on one WiFi, so LAN discovery is available and the relay is not needed for this goal.

**The iOS Simulator cannot be the third peer.** It needs an `arm64-ios-simulator` build rather than
the `arm64-ios` device build we ship, and it shares the host Mac's network stack and IP — the LAN
lobby keys peers on IP, so a simulator peer and the Mac peer collide. Use the simulator for driving
touch input during single-device UI testing only.

## Blockers, in dependency order

### 1. Windows x64 must link  *(DONE — verified 2026-07-27 18:12)*
Both executables link at commit `be9491781`. Independently re-verified, not taken on report:
CONFIGURE EXIT 0, BUILD EXIT 0, 0 compiler errors, 0 LNK, 0 FAILED, last step
`Linking CXX executable GeneralsMD\Release\generalszh.exe`.
`generalszh.exe` 7,492,096 B and `generalsv.exe` 6,978,048 B, both `8664 machine (x64)` per dumpbin.
macOS and iOS both still build with exit 0. 5529 errors -> 0.

The implementing agent also caught its own fast build (~96 s) and re-ran with `SAGE_USE_CCACHE=OFF`
on a wiped tree, observing 16 concurrent cl.exe, to prove the objects were genuinely compiled.

NOT covered by this: the mod tools (cb.bat forces RTS_BUILD_*_TOOLS=OFF; never configured at x64),
and nothing has ever been RUN. See blocker 2.

### 2. Windows must RUN  *(DONE — boots AND plays a skirmish, verified 2026-07-27. Audio/video still stubbed.)*
**It boots.** `generalszh.exe` initialises all 42 engine subsystems in order — through `TheGameClient`,
`TheAI`, `TheGameLogic`, `TheGameState` — past `W3DDisplay::init()` (windowed), into the shell UI and
`ShellMenuScheme`, with 0 error lines and DXVK actively rendering (`Device : NVIDIA GeForce RTX 5090`,
6404 log lines, ~720 MB resident, ~10 s CPU in 45 s).

Reproduce with `scripts/build/windows/setup-run-win64.ps1 -Launch`. Three things had to be true, and
each failed silently and misleadingly on the way:

1. **The Miles/Bink stub DLLs must be staged beside the exe.** They are x64 DLLs built from source
   and deliberately named `mss32.dll` / `binkw32.dll` so they satisfy the import table. Retail Miles
   and Bink are 32-bit (`14C machine (x86)`) and can never load into an x64 process. Without the
   stubs the process dies at **`0xC0000135 STATUS_DLL_NOT_FOUND` before `WinMain`**, with zero bytes
   on stdout and stderr and nothing in the event log.
2. **`CNC_GENERALS_ZH_PATH` needs a TRAILING BACKSLASH.**
   `Win32LocalFileSystem::getFileListInDirectory` (`Win32LocalFileSystem.cpp:131-137`) builds its
   search as `originalDirectory + currentDirectory + searchName` with **no separator inserted**.
   Without it the mask becomes `...Zero Hour*.big`, `FindFirstFile` matches nothing, and the engine
   says `did not provide BIG files` then dies in INI loading. This is a real footgun worth fixing in
   source — see the note under blocker 3.
3. **It must run in the INTERACTIVE session.** Launched over SSH you land in session 0, where DXVK
   enumerates **zero** adapters and `W3DDisplay::init()` dies at `0xC0000005`. A transient
   `schtasks /it` task reaches the logged-on session; the script creates and deletes it.

Steam install proven untouched across every run: 404 files / 3,073,628,958 B,
`MANIFEST-SHA256 9793E5EE7FCEDAF250C7403B6D7D01C0426F993B260EB233E49B079A27134033` identical before
and after (`scripts/build/windows/steam-manifest.ps1`).

**Visually confirmed at the main menu** (screenshot captured from the interactive session): the
animated 3D shell map renders at 30 FPS with SOLO PLAY / MULTIPLAYER / LOAD / OPTIONS / CREDITS /
EXIT GAME, window title `GeneralsX 2055 By RugPullBot for Command & Conquer (TM) Generals Zero Hour
Version 1.04`. To reproduce the capture, run the game under `schtasks /it` and screenshot from a
script inside that same session — you cannot see a session-1 window from an SSH session.

**The windowed pillarbox reproduces on Windows too** — the scene is rendered inset with a border,
matching the known open problem. Workaround remains fullscreen, or `-win -xres 1600 -yres 900`.

**Skirmish plays — test ladder step 3 is DONE.** Driven end to end by synthetic input in session 1:
main menu -> SOLO PLAY -> SKIRMISH -> PLAY GAME. The match runs, with the clock at `00:00:28.22`,
a GLA command centre and infantry on a fully textured map, `$10000` credits, command bar and radar
populated, 30 FPS, working set climbing 756 -> 845 MB as the map loaded. Screenshots at every step.

The menu is fully interactive: the skirmish setup screen lists the real host name `r0se-DESKTOP`,
loads the map cache and renders a map preview (`[RANK] AKAs Magic ZH v1 map (2)`).

Technique worth keeping — a session-1 window is invisible from an SSH session, so both the clicking
and the screenshotting must happen inside a script that `schtasks /it` runs in that session. Desktop
is 1920x1080 there; `SystemInformation.VirtualScreen` queried from session 0 misreports it as
1024x768, so do not compute click coordinates from an SSH-side query.

Still open within this blocker — audio and video are stubbed BY CONSTRUCTION, so the Windows build
currently boots **silent, with no intro movies**: the build links `miles-sdk-stub` and `bink-sdk-stub`,
whose functions return and do nothing. Real Miles and Bink are 32-bit only and can never be dropped
in at x64, so Windows needs the OpenAL + FFmpeg path the Apple builds use. Note
`Core/GameEngineDevice/CMakeLists.txt` adds `Source/VideoDevice/FFmpeg/FFmpegFile.cpp`
unconditionally whenever `SAGE_USE_OPENAL` is set, and the Windows configure has no FFmpeg yet. Game data is present at
`C:\Program Files (x86)\Steam\steamapps\common\Command & Conquer Generals - Zero Hour`.
Expect a second wave of failures here that compilation cannot predict — asset path resolution, the
D3D8 entry point resolving to DXVK's `d3d8.dll` (drop the official x64 DLLs beside the exe; no
source change needed, `dx8wrapper.cpp:575` already uses `LoadLibrary`), and audio, since Miles is
32-bit-only and x64 must take the OpenAL path the Apple builds use.

### 3. SimID will falsely refuse Mac <-> Windows  *(MEASURED 2026-07-27 — IT DOES NOT. Latent, not active.)*

**This was predicted from code reading and is now measured on both machines. It does not happen.**
With SimID instrumentation landed (commit `a48345bfe`), both peers at the same clean commit, printed
from a real boot on each machine:

```
Mac      rev=2059 engine=4D31F2F2 source=4D98C2D4 data=FEAAE3F3 ordinal=9F43F7B5
         parse=CD8F767F asset=8E504171 platform=BF013216
Windows  rev=2059 engine=4D31F2F2 source=4D98C2D4 data=FEAAE3F3 ordinal=9F43F7B5
         parse=CD8F767F asset=8E504171 platform=7480F925
```

`SimIdCompare` (`SimulationId.cpp:273-287`) denies on `engineID`, `sourceID`, `dataID`, `ordinalID`.
**All four are identical, so the verdict is `SIMID_OK` and the join is accepted.** `platformID` is the
only difference and it is tier2 — it raises `SIMID_WARN_PLATFORM` and posts a chat warning, by design.

Two further things this measurement settles:

* **`m_iniCRC` matches** (`FEAAE3F3` both sides) — the separator divergence does not fire here, because
  both installs are archive-only. The bug needs a loose `.ini` AND an archive entry in the *same*
  directory before `/` and `\` can mix in one set. So it is **latent, not active**.
* **The wire layout matches** — `sizeof(LANMessage)=471` and `sizeof(SimIdWire)=36` on both, so Apple
  clang arm64 and MSVC x64 agree on packing. `assetID` matching also proves the mounted `.big` sets
  are identical across Mac and the PC.

Beware of one trap when repeating this: the digest hashes `git ls-tree` at HEAD **plus a dirty-file
overlay**, so a build from a dirty working tree gets a different `sourceID` than a build from the clean
commit. The first comparison run showed `sourceID` differing purely for that reason (Mac `rev=2058`
dirty vs Windows `rev=2059` clean) and it looked exactly like a platform divergence. **Compare only
builds made at the same clean commit.**

The fix below is still worth doing — a loose INI override is a completely ordinary thing for a player
or a mod to add, and the day one appears the join breaks with no remedy the user can apply. But it is
**no longer a prerequisite for the first three-way match**, and it should not block the test ladder.

---

Original analysis, still accurate as a description of the latent bug:

`m_iniCRC` is **not platform-neutral**. `FileSystem::getFileListInDirectory` merges entries whose
separators are `/` from the local filesystem and `\` from archives; `/` is 0x2F and `\` is 0x5C, so
the two sort differently, and `INI::loadDirectory` feeds `xferCRC` in exactly that set order. Worse,
the set dedupes by exact string, so a loose override that also exists in a `.big` is one entry on
Windows and two on Apple.

Identical game data therefore produces a different `dataID` on Windows, and the join is refused with
no remedy the user can apply.

**Design settled 2026-07-27 — normalise to FORWARD SLASH, separators only.**

* **Case needs no work.** `FilenameList` is `std::set<AsciiString, rts::less_than_nocase<AsciiString>>`
  (`FileSystem.h:66`) and `less_than_nocase<AsciiString>` uses `compareNoCase` (`STLTypedefs.h:238-244`),
  so the set already dedupes and orders case-insensitively. Only the separator is broken.
* **Forward slash is the only viable canonical form.** The archive lookup is already separator-agnostic —
  `ArchiveFileSystem::getArchivedDirectoryInfo` tokenises on `"\\/"` and lowercases first
  (`ArchiveFileSystem.cpp:319-320`). Win32 accepts `/` in paths, and Apple's local FS cannot accept `\`.
  So `/` works for all three consumers; `\` would break the Apple local filesystem.
* **A 1:1 character substitution is safe for the existing pointer arithmetic.** `INI::loadDirectory`
  does `tempname = (*it).str() + dirName.getLength()` (`INI.cpp:278`, `:291`) to strip the prefix, and
  its subdirectory test already checks *both* separators (`:280`, `:293`). Replacing `\` with `/`
  in place leaves every length unchanged, so none of that shifts.
* Apply it at the single merge point, `FileSystem::getFileListInDirectory` (`FileSystem.cpp:286-291`),
  after both producers have run — there are exactly **three** producers that insert into a
  `FilenameList` (`ArchiveFile.cpp:179`, `Win32LocalFileSystem.cpp:150`, `StdLocalFileSystem.cpp:319`),
  verified by unbounded search, so the merge point covers all of them.

Note the archive path also lowercases leaf filenames (`StdBIGFileSystem.cpp:604`,
`Win32BIGFileSystem.cpp:598`) while the local path does not — harmless for ordering because the
comparator is case-insensitive, but worth knowing before touching this.

**This changes `m_iniCRC` on macOS too**, so it must land in the same commit as re-deriving
all four hardcoded retail checkpoints: `0xA1E7F8E6` and `0x6209AF6E` (GeneralsMD
`GameEngine.cpp:597`/`:694`), `0x2E876341` and `0xD9A74E13` (Generals `:522`/`:579`). Miss that and
`verifyNameKeyID` silently stops firing.

### 4. Archive parity  *(largely DONE)*
All four community archives are now installed on Mac and iPad, SHA-256 verified against the PC
originals: `!HotkeysLeikezeIndicatorsZH.big`, `!HotkeysLeikezeZH.big`,
`340_ControlBarPro-Fix1440ZH.big`, `340_ControlBarPro1440ZH.big`. Mac and iPad are byte-for-byte
identical at 41 archives; the Mac launches clean with them (3775 log lines, 0 INI errors).

Decision taken: ship all four everywhere by default. The `CommandButton.ini` inside the Leikeze
indicators archive was the one desync candidate, because ScienceType is a NameKey ordinal shipped
raw over the wire - but that risk comes from peers MISMATCHING, not from the mod. Identical on all
three peers means identical ordinals, so shipping it everywhere is safer than having it on one.

What ControlBar Pro 1440 actually is: 38 `.wnd` layouts plus 22 `HeaderTemplate.ini` /
`Language.ini` files - a UI re-authored for high-DPI. This is very likely a genuine fix for the
"UI looks low-res" complaint, independent of the pillarbox issue.

**~~ONE FILE STILL UNRESOLVED~~ — RESOLVED 2026-07-27. Action required: none.**

`Data/INI/INIZH.big` is a **retail SKU artifact**, and the engine **already skips it by name**.

The previous note said "It IS loaded: `StdBIGFileSystem.cpp:658` passes `searchSubdirectories = TRUE`".
That was wrong. `searchSubdirectories` makes it *found*, not *loaded* — the very next thing the loop
does is skip it, at `StdBIGFileSystem.cpp:663-671` (and the Win32 twin at `Win32BIGFileSystem.cpp:661`):

```cpp
#if RTS_ZEROHOUR
// TheSuperHackers @bugfix bobtista 18/11/2025 Skip duplicate INIZH.big in Data\INI to prevent CRC mismatches.
// English, Chinese, and Korean SKUs shipped with two INIZH.big files (one in Run directory, one in Run\Data\INI).
if (it->endsWithNoCase("Data\\INI\\INIZH.big") || it->endsWithNoCase("Data/INI/INIZH.big")) {
    it++; continue;          // <-- before openArchiveFile is ever called
}
#endif
```

Verified rather than assumed: `RTS_ZEROHOUR=1` is set at `GeneralsMD/Code/CMakeLists.txt:46,49`, and
both guard string literals are present in all three shipped binaries (macOS, iOS, **and** the Windows
x64 exe).

Its contents corroborate the SKU-duplicate explanation exactly: 99 entries, **every one of them also
present** in the root archive's 135, with 37 files at *older, different* content (`Armor.ini`,
`CommandSet.ini`, `ObjectCreationList.ini`, `Locomotor.ini`, and every General file). It is the stale
shipped duplicate, which is precisely why upstream added the skip.

**Therefore: do not copy it to the PC and do not delete it from the Apple side.** It contributes
nothing to `m_iniCRC` because it is never opened, and `INI::loadDirectory` masks on `*.ini` anyway so
a `.big` cannot reach the CRC by name either. The 41-vs-40 archive count is harmless and expected.

### 5. Mac <-> Windows determinism — unmeasured
Mac <-> iPad is proven. Mac <-> Windows is **not**, and nobody upstream has ever built x64 Windows to
find out.

Reasons for optimism, all verified: every `#if defined(_MSC_VER) && defined(_M_IX86)` gate in
`wwmath.h` excludes x64, so MSVC x64 compiles the same portable C that arm64 clang does, and
`Inv_Sqrt` becomes `1.0f/(float)sqrt(val)` on both. `RETAIL_COMPATIBLE_CRC=1` plus
`BitFlagsIO.h:218`'s `sizeof(this)` matches for free between two 64-bit peers.

The known risk: `Thing::setOrientation` writes raw `cosf`/`sinf` results into the 48 bytes
`Object::crc` hashes with no epsilon, and `Matrix3D::Rotate_Z` and siblings call them directly. If
Apple's libm and MSVC's UCRT disagree by one ULP on a reachable input, the game desyncs within ~100
frames. bobtista measured that **32-bit** Windows cannot reach arm64 parity under any x87 control
word; x64 removes x87 entirely, which is why it may work — but it is unmeasured.

Measure it before playing: de-fold `SimulationMathCrc` (make its inputs `volatile`; 10 of its 17
libm call sites are currently constant-folded at -O2, so it measures the compiler) and compare the
value across all three peers.

### 6. Three-way LAN lobby — untested
Two peers has worked. Three has never been tried, and the lobby keys on IP.

## Test ladder — do not skip steps

1. Mac <-> Mac (two instances, different ports) — cheapest SimID smoke test
2. Mac <-> iPad — known-good baseline; proves nothing regressed
3. Windows solo — boots, loads a map, plays a skirmish  *(DONE 2026-07-27)*
4. Mac <-> Windows — the real unknown. Play to a CRC interval, then to completion
5. All three — the goal
6. A full match with no desync

## Open questions for Karl

1. Archive parity direction — copy the four mods to the Mac, or remove them from the PC?
2. ~~Friend's PC~~ - ANSWERED: there is no friend's PC. Three devices, all Karl's, all on one LAN.
3. If Mac <-> Windows desyncs on floating point, how far to chase it? Adopting deterministic math is
   a multi-week project against an unmerged upstream PR.

## Instrumentation to add early — cheap, and prevents blind debugging

* Log the computed SimID (protocol / engineID / sourceID / dataID) at startup and on every join
  attempt, both sides. Right now it is computed and never surfaced, so a refusal cannot be diagnosed.
* Log the peer's SimID on refusal, with which field differed.
* Run the de-folded `SimulationMathCrc` at startup and log it, so a three-way float mismatch is
  visible before a match rather than inferred from a desync.

## Windows must run in its own folder — do not contaminate the Steam install

Karl plays Generals Online on the side, which runs `EAC_LaunchGeneralsOnline.exe` (Easy Anti-Cheat)
against their signed `GeneralsOnlineZH.exe` in the Steam directory.

**Never place our unsigned DXVK `d3d8.dll`/`d3d9.dll` beside that exe.** Best case Generals Online
breaks; worst case EAC flags the account. The plan is:

    C:\dev\GeneralsX-run\        our exe + our DXVK DLLs + our dxvk.conf + our logs
    Steam\...\Zero Hour\         READ-ONLY, untouched, EAC-clean

The engine supports this already - asset resolution consults `CNC_GENERALS_ZH_PATH` first
(`StdBIGFileSystem.cpp:326`), and user data resolves to Documents via `FOLDERID_Documents`
(`GlobalData.cpp:1397+`), not the install directory.

Still to pin down and verify, rather than assume: the DXVK shader cache, `*_d3d9.log`, and the map
cache - anything defaulting to the working directory. Verification standard: hash the Steam folder
before and after a run; a single changed byte is a bug.

## Deferred feature (not in scope for cross-play)

Keybind remapping UI in settings - let players rebind keys themselves. Real feature, wants its own
design pass. Karl raised it while deciding to ship the Leikeze hotkey archive.
