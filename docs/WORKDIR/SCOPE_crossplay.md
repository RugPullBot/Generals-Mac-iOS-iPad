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
| Windows 11 PC | `User@192.168.10.89`, SSH key auth, MSVC 14.44 + ATL, clone at `C:\dev\GeneralsX` |

All three on one WiFi, so LAN discovery is available and the relay is not needed for this goal.

**The iOS Simulator cannot be the third peer.** It needs an `arm64-ios-simulator` build rather than
the `arm64-ios` device build we ship, and it shares the host Mac's network stack and IP — the LAN
lobby keys peers on IP, so a simulator peer and the Mac peer collide. Use the simulator for driving
touch input during single-device UI testing only.

## Blockers, in dependency order

### 1. Windows x64 must link  *(in progress)*
Tracked by the `win64-drive-to-link` workflow. See `STATE_2026-07-27.md` for how the error count
came down and the two process traps that cost rebuilds.

### 2. Windows must RUN
Never attempted. Game data is present at
`C:\Program Files (x86)\Steam\steamapps\common\Command & Conquer Generals - Zero Hour`.
Expect a second wave of failures here that compilation cannot predict — asset path resolution, the
D3D8 entry point resolving to DXVK's `d3d8.dll` (drop the official x64 DLLs beside the exe; no
source change needed, `dx8wrapper.cpp:575` already uses `LoadLibrary`), and audio, since Miles is
32-bit-only and x64 must take the OpenAL path the Apple builds use.

### 3. SimID will falsely refuse Mac <-> Windows  *(HARD BLOCKER — this one bites silently)*
`m_iniCRC` is **not platform-neutral**. `FileSystem::getFileListInDirectory` merges entries whose
separators are `/` from the local filesystem and `\` from archives; `/` is 0x2F and `\` is 0x5C, so
the two sort differently, and `INI::loadDirectory` feeds `xferCRC` in exactly that set order. Worse,
the set dedupes by exact string, so a loose override that also exists in a `.big` is one entry on
Windows and two on Apple.

Identical game data therefore produces a different `dataID` on Windows, and the join is refused with
no remedy the user can apply.

Fix: canonicalise at the single merge point in `FileSystem::getFileListInDirectory` — collect into a
vector, normalise separators and case, sort, unique, then hand `INI::loadDirectory` the normalised
order. **This changes `m_iniCRC` on macOS too**, so it must land in the same commit as re-deriving
all four hardcoded retail checkpoints: `0xA1E7F8E6` and `0x6209AF6E` (GeneralsMD
`GameEngine.cpp:581`/`:660`), `0x2E876341` and `0xD9A74E13` (Generals `:501`/`:538`). Miss that and
`verifyNameKeyID` silently stops firing.

### 4. Archive parity
The PC has four archives the Mac does not:
`!HotkeysLeikezeIndicatorsZH.big`, `!HotkeysLeikezeZH.big`,
`340_ControlBarPro-Fix1440ZH.big`, `340_ControlBarPro1440ZH.big`.
Base data **is** identical — `INIZH.big` SHA-256 matches across both. SimID will (correctly) refuse
until the sets match. Karl's call which direction; copying to the Mac also brings the high-resolution
ControlBar Pro art the render work wanted.

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
3. Windows solo — boots, loads a map, plays a skirmish
4. Mac <-> Windows — the real unknown. Play to a CRC interval, then to completion
5. All three — the goal
6. A full match with no desync

## Open questions for Karl

1. Archive parity direction — copy the four mods to the Mac, or remove them from the PC?
2. Is his friend's PC in scope for this round, or only Karl's three devices?
3. If Mac <-> Windows desyncs on floating point, how far to chase it? Adopting deterministic math is
   a multi-week project against an unmerged upstream PR.

## Instrumentation to add early — cheap, and prevents blind debugging

* Log the computed SimID (protocol / engineID / sourceID / dataID) at startup and on every join
  attempt, both sides. Right now it is computed and never surfaced, so a refusal cannot be diagnosed.
* Log the peer's SimID on refusal, with which field differed.
* Run the de-folded `SimulationMathCrc` at startup and log it, so a three-way float mismatch is
  visible before a match rather than inferred from a desync.
