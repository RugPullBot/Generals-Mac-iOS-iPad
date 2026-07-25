# Data and Assets

How Zero Hour finds, loads and overrides its game data: BIG archives, the virtual
file system, INI, string tables, the map cache, and save games.

Everything below was read out of the working tree at commit `325ea7a6c` (which has
uncommitted local changes in `GameEngine.cpp`, `CommandLine.cpp`, `GlobalData.cpp`,
`SDL3Main.cpp` and `SDL3GameEngine.cpp` — line numbers reflect the working tree, not
the commit). Paths are relative to the repo root. Line numbers are cited for anything
non-obvious; if a claim has no citation, treat it as commentary.

---

## 1. How it fits together (read this first)

```
                          TheFileSystem  (Core/GameEngine/.../FileSystem.cpp)
                                 |
                 +---------------+----------------+
                 |                                |
      TheLocalFileSystem                 TheArchiveFileSystem
      (loose files on disk)              (all *.big mounted into ONE tree)
      StdLocalFileSystem                 StdBIGFileSystem  -> StdBIGFile -> RAMFile
```

* **One flat namespace.** Every `.big` found at startup is exploded into a single
  shared directory tree (`ArchiveFileSystem::m_rootDirectory`). Callers never name an
  archive; they ask for `"Data\INI\Object\AmericaVehicle.ini"` and the tree resolves it.
  All keys in that tree are **lower-cased** (`ArchiveFileSystem.cpp:134`,
  `ArchiveFile.cpp:103`), so archive lookups are effectively case-insensitive.
* **Loose files beat archives, always.** `FileSystem::openFile` tries
  `TheLocalFileSystem` first and only falls through to the archives when that returns
  `nullptr` (`FileSystem.cpp:180-217`). Same for `doesFileExist`
  (`FileSystem.cpp:245-271`) and `getFileInfo` (`FileSystem.cpp:307-317`).
  This is the single most useful lever for modding and debugging.
* **Within the archives, first-mounted wins** — see §3, it is not what most people
  assume.
* **INI is loaded in a fixed, hard-coded order** at engine init
  (`GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp:448-711`). Later loads of
  the same block name overwrite earlier ones. There is no dependency graph; the order
  in that function *is* the spec.
* **`FileInstance`** (`FileSystem.h:68`, a `UnsignedByte`) lets a caller reach *past*
  the winner to a shadowed copy of the same path. Instance 0 = the file the game
  normally sees; instance 1 = the next one down. The GameText CSF fallback (§5,
  `GameText.cpp:921`, `:968`) is currently the **only** caller that passes a non-zero
  instance; everything else defaults to 0.

Rough startup order (from `GameEngine::init`):

| Step | Where | Note |
|---|---|---|
| `TheLocalFileSystem` init | `GameEngine.cpp:448` | |
| `TheArchiveFileSystem` init — mounts every `*.big` | `GameEngine.cpp:459` | must be after local FS (`StdBIGFileSystem.cpp:492`) |
| `Data\INI\Default\GameData` then `Data\INI\GameData` | `GameEngine.cpp:470` | |
| auto-generated `SagePatch.ini` from user data dir | `GameEngine.cpp:473-512` | GeneralsX addition |
| command line (`-mod`, …) parsed | `GameEngine.cpp:530` | |
| **`TheArchiveFileSystem->loadMods()`** | `GameEngine.cpp:532` | mod BIGs mount *here*, not earlier |
| Water / Weather INI | `GameEngine.cpp:545-548` | |
| `TheGameText` (CSF/STR) | `GameEngine.cpp:563` | |
| Science, Multiplayer, Terrain, Roads, PlayerTemplate, FXList, Weapon, OCL, Locomotor, SpecialPower, DamageFX, Armor, **Object**, Upgrade, AIData, Crate, CommandMap … | `GameEngine.cpp:579-691` | each via `initSubsystem(path1, path2)` |
| `TheGameStateMap`, `TheGameState` | `GameEngine.cpp:707-708` | |

---

## 2. The BIG archive format

Parsed by `StdBIGFileSystem::openArchiveFile`
(`Core/GameEngineDevice/Source/StdDevice/Common/StdBIGFileSystem.cpp:522-622`).

```
offset  size  field
0x00    4     "BIGF"                      (StdBIGFileSystem.cpp:58, 543)
0x04    4     total archive size          read but never used  (551)
0x08    4     number of entries, BIG-ENDIAN (559-560, betoh)
0x0C    4     (skipped)
0x10    ...   entry table starts here     (570, hard-coded seek to 0x10)

per entry:
        4     file offset, BIG-ENDIAN     (577, 581)
        4     file size,   BIG-ENDIAN     (575-580)
        n     NUL-terminated path, backslash-separated, e.g. "data\ini\object\foo.ini"
```

Notes and traps:

* Offsets/sizes are **big-endian**; `betoh()` from `Utility/endian_compat.h` converts.
  The 4-byte archive size at 0x04 is read *without* conversion (`:551`) — harmless
  only because nothing reads it.
* The path is read one byte at a time into a `char buffer[_MAX_PATH]` with **no bounds
  check** (`:589-592`). A malformed or hostile `.big` overflows the stack buffer.
  Same in the Win32 variant. Worth knowing before you point the loader at
  user-supplied archives.
* Entries are split into `path` + `filename`, the filename is lower-cased (`:600`),
  and inserted into a per-archive tree by `ArchiveFile::addFile`
  (`Core/GameEngine/Source/Common/System/ArchiveFile.cpp:97-123`). Note `:122` is a
  plain `map::operator[]` assignment — **inside a single BIG, a later entry with the
  same path silently replaces the earlier one.**
* Opening a member file reads the **entire member into RAM**
  (`RAMFile::openFromArchive`, `Core/GameEngine/Source/Common/System/RAMFile.cpp:209-233`)
  unless `File::STREAMING` is set, in which case you get a `StreamingArchiveFile`
  that seeks within the still-open archive handle (`StdBIGFile.cpp:71-73`). Only the
  audio path passes `STREAMING`.
* Opening a member with `File::WRITE` copies it out to a *local* file and returns
  that instead (`StdBIGFile.cpp:86-98`). Archives are never written.
* `closeArchiveFile` is only ever meaningfully used for `Music.big`
  (`ArchiveFileSystem.h:46`, `StdBIGFileSystem.cpp:624-643`); it asserts if you try
  to close anything else.

---

## 3. ArchiveFileSystem: load order and override semantics

This is the part that decides which mod wins. Read it carefully — the answer is
counter-intuitive.

### The data structure

```cpp
typedef std::multimap<AsciiString, ArchiveFile*> ArchivedFileLocationMap;  // ArchiveFileSystem.h:88
class ArchivedDirectoryInfo {
    ArchivedDirectoryInfoMap m_directories;
    ArchivedFileLocationMap  m_files;      // filename -> N archives that contain it
};
```

A **multimap**: every archive containing `foo.ini` gets an entry. Lookup takes
`equal_range(name)` and advances `instance` steps
(`stl::get_range`, `Core/Libraries/Source/WWVegas/WWLib/STLUtils.h:126-142`), so
"instance 0" is simply *the first element of the equal range*
(`ArchiveFileSystem.cpp:318-331`).

### Who ends up first

`ArchiveFileSystem::loadIntoDirectoryTree(archiveFile, overwrite)`
(`ArchiveFileSystem.cpp:118-211`) decides insertion position:

```cpp
if (overwrite)  fileIt = dirInfo->m_files.find(token);   // hint = first equal element
else            fileIt = dirInfo->m_files.end();         // hint = end
dirInfo->m_files.insert(fileIt, std::make_pair(token, archiveFile));  // :169
```

* `overwrite == FALSE` → hint `end()` → appended at the upper bound of the equal
  range → **the archive mounted first stays at instance 0 and wins.**
* `overwrite == TRUE` → hint = first equal element → inserted *before* it →
  **the newly mounted archive becomes instance 0 and wins.**

(Verified empirically with a standalone `std::multimap` repro; both libc++ and
libstdc++ place a hinted insert immediately before the hint for equivalent keys.
Strictly speaking `std::multimap::find` is only required to return *an* equivalent
element, not the first — the `overwrite` path leans on implementation behaviour.
It works on every mainstream STL, but it is not standard-guaranteed.)

### Which archive is mounted first

`StdBIGFileSystem::loadBigFilesFromDirectory` (`StdBIGFileSystem.cpp:651-683`) asks
the local FS for the file list and iterates it in order. That list is a
`std::set<AsciiString, rts::less_than_nocase<AsciiString>>`
(`FileSystem.h:66`, comparator at `STLTypedefs.h:238-244`, backed by `_stricmp`).

**A `std::set` iterates in sorted order. Therefore BIG files in one directory are
mounted in case-insensitive alphabetical order, and with the default
`overwrite == FALSE`, the alphabetically FIRST archive wins.**

So `AAAMod.big` overrides stock content; `ZZZMod.big` does not. If you have absorbed
the "name it zzz so it loads last" folklore from another engine, unlearn it here.
(This matches retail: the pre-multimap code did `if (not found || overwrite)`, see
`git show 6748e68e7^:Core/.../ArchiveFileSystem.cpp:153`.)

### Directory-level order (Zero Hour)

`StdBIGFileSystem::init` (`:491-511`):

1. `loadPrimaryGameAssets` (`:317-423`) mounts everything in the **Zero Hour** asset
   directory, resolving that directory by:
   `CNC_GENERALS_ZH_PATH` env → `GENERALSX_ASSET_PATH` (compat) → `CNC_ZH_INSTALLPATH`
   (legacy) → `Options.ini` `[Paths] AssetPath` (cwd, then exe dir) → registry
   `InstallPath` → `<exedir>/../Resources` → exe dir → cwd. First one that yields at
   least one `.big` wins.
2. The resolved directory is pushed into the local FS as an asset-root fallback
   (`:504-506`, `StdLocalFileSystem.cpp:425-431`) so that loose files like
   `Data\Scripts\SkirmishScripts.scb` resolve even when cwd ≠ game dir.
3. `loadBaseGeneralsAssetsForZH` (`:425-481`) then mounts the **base Generals**
   directory (env `CNC_GENERALS_PATH` → … → sibling `../Generals` → `ZH_Generals`).

Both use `overwrite = FALSE`, and ZH goes first, so **ZH assets shadow base Generals
assets** for any shared path. That is the intended behaviour and the reason the order
of those two calls matters.

`loadBigFilesFromDirectory` also hard-skips `Data\INI\INIZH.big`
(`:659-667`) because some SKUs shipped it twice and the duplicate breaks CRC.

### Mods

`ArchiveFileSystem::loadMods()` (`ArchiveFileSystem.cpp:213-237`), driven by `-mod`
(`GeneralsMD/Code/GameEngine/Source/Common/CommandLine.cpp:1072-1116`, registered at
`:1191` in `paramsForEngineInit`):

* `-mod <file.big>` → mounted with `overwrite = TRUE` → instance 0 for every path it
  contains → beats everything.
* `-mod <dir>` → `loadBigFilesFromDirectory(dir, "*.big", TRUE)` (`:233`) → **every**
  BIG in that directory is inserted with `overwrite = TRUE`. Because each one is
  pushed to the *front*, the alphabetically **last** BIG in a mod directory wins —
  the exact reverse of the rule inside the game directory. If you ever have two BIGs
  in a mod folder claiming the same path, this asymmetry will bite you.
* A relative `-mod` argument is resolved against the **user data directory**, not cwd
  (`CommandLine.cpp:1082-1084`).
* Because `loadMods()` runs at `GameEngine.cpp:532`, which is *after*
  `Data\INI\Default\GameData` + `Data\INI\GameData` are parsed at `:470`, a mod BIG
  **cannot override `GameData.ini`**. Everything loaded from `:545` onwards
  (Water, Weather, GameText, Object, Weapon, …) *is* overridable by a mod BIG.

---

## 4. INI

### The reader

`Core/GameEngine/Include/Common/INI.h`, `Core/GameEngine/Source/Common/INI/INI.cpp`.
The format is not Windows `.ini`: there are no `[Section]` headers. It is
`BlockType Name` … fields … `End`.

```
Object AmericaTankCrusader
  Draw = W3DTankDraw ModuleTag_01
    ...
  End
End
```

* **Block dispatch**: the first token of a line is looked up in `theTypeTable`
  (`INI.cpp:91-155`, 62 entries: `Object`, `Weapon`, `Armor`, `FXList`, `CommandSet`,
  `MappedImage`, `AudioEvent`, `MapCache`, `Locomotor`, `Upgrade`, …). The lookup is
  `strcmp` — **case-sensitive** (`findBlockParse`, `INI.cpp:354-365`). `object` will
  not match `Object`.
* An **unknown block type throws** `INI_UNKNOWN_TOKEN` and aborts the file
  (`INI.cpp:456-461`).
* **Field dispatch** is also `strcmp` (`findFieldParse`, `INI.cpp:368-391`). A parse
  table may end with a `{nullptr, proc, ...}` catch-all sentinel that receives the
  unmatched token as `userData` (`:381-386`) — that is how e.g. module tags work.
* An **unknown field is silently ignored in release builds**: it is only a
  `DEBUG_CRASH` (`INI.cpp:1607`), which compiles to `((void)0)` when `DEBUG_CRASHING`
  is off (`Debug.h:205`). A typo'd field name in a mod produces no diagnostic in a
  shipping build. A missing `End` *does* throw (`INI.cpp:1622`), and `End` is matched
  case-insensitively (`:1569`).
* **Line handling** (`INI::readLine`, `INI.cpp:494-561`): `;` starts a comment and
  truncates the line; control characters below 0x20 become spaces; tabs trigger an
  assert (`:527`); lines longer than `INI_MAX_CHARS_PER_LINE` (1028, `INI.h:60`) are
  silently truncated in release (`:550-553`). Default token separators are
  `" \n\r\t="` (`INI.h:253`).
* When an `Xfer*` is supplied, every line is fed through it (`INI.cpp:556-560`).
  That is how `xferCRC` in `GameEngine::init` builds `TheGlobalData->m_iniCRC`
  (`GameEngine.cpp:444, 722-723`) — the multiplayer "same data?" check. **Anything you
  add to a CRC'd INI directory changes the CRC and will mismatch against unmodded
  clients.**

### Load types (`INI.h:47-53`)

| Type | Meaning |
|---|---|
| `INI_LOAD_OVERWRITE` | create new, or load *over* the existing instance. The default everywhere. |
| `INI_LOAD_CREATE_OVERRIDES` | create a *new* instance chained off the existing one as an override. Only `map.ini`/`solo.ini` use it. |
| `INI_LOAD_MULTIFILE` | keep loading into the existing instance (merge). Used for `CommandMapDebug`, and for the English `Language.ini` fallback. |

### File vs. directory

`INI::loadFileDirectory(name, ...)` (`INI.cpp:193-246`) is the modern entry point:
for `"Data\INI\Object"` it loads **`Data\INI\Object.ini` first**, then **every
`*.ini` under `Data\INI\Object\`** via `loadDirectory`. It throws
`INI_CANT_OPEN_FILE` if nothing at all was read (`:236-241`).

`INI::loadDirectory` (`INI.cpp:253-300`) is deliberately two-pass: files directly in
the directory first, then files in subdirectories (`:274-297`), each pass in the
sorted order of the `FilenameList` set. The comment at `:272` says why — determinism
across machines in a network game.

**This is the clean mod hook.** Dropping `Data/INI/Object/ZZZMyUnits.ini` (loose, or
inside a mod BIG) adds content without touching a stock file. Files inside the
directory sort alphabetically, and later files overwrite earlier ones with the same
block name, so a `zz`-prefixed file wins *at the INI level* — the opposite of the BIG
mounting rule. Keep the two rules separate in your head.

### The `Default\` convention

`SubsystemInterfaceList::initSubsystem` takes two paths
(`Core/GameEngine/Source/Common/System/SubsystemInterface.cpp:154-195`) and loads
`path1` then `path2`, both `INI_LOAD_OVERWRITE` (`:176-190`). By convention
`path1 = Data\INI\Default\Foo` (base values) and `path2 = Data\INI\Foo`
(the real definitions), e.g. `GameEngine.cpp:579-643`. Some subsystems pass
`nullptr` for `path1` (Rank, Weapon, Locomotor, DamageFX, Armor).

### Override chains (`Overridable`)

`GeneralsMD/Code/GameEngine/Include/Common/Overridable.h:40-113`. Data objects that
support per-map overrides derive from `Overridable`, which is a singly-linked list:
`getFinalOverride()` walks to the tail (`:59-65`), `markAsOverride()` flags an entry
for cleanup (`:95-98`), and `deleteOverrides()` unlinks the chain on reset
(`:101-113`). Consumers must call `getFinalOverride()` rather than using the head.

The parse side, e.g. `INI::parseObjectDefinition`
(`GeneralsMD/.../Common/Thing/ThingFactory.cpp:380-440`):

```
existing? no  -> newTemplate(), markAsOverride() if CREATE_OVERRIDES
existing? yes && loadType != CREATE_OVERRIDES -> DEBUG_CRASH "Duplicate factionunit"
existing? yes && loadType == CREATE_OVERRIDES -> newOverride(existing)
```

Note the duplicate-definition case is *only* a `DEBUG_CRASH` — in release, redefining
an `Object` in a second INI file just replaces it, no warning.

### Per-map INI

`GameLogic.cpp:2561-2586` (GeneralsMD): when a map is loaded, the engine looks next to
the `.map` for `map.ini`, then `solo.ini`, and loads each with
`INI_LOAD_CREATE_OVERRIDES`; then `map.str` for map-local strings. If the map came
from a save game it uses the *pristine* map path instead (`:2540-2543`). There is a
`@todo` at `:2570` noting that directory-style INI loading is not yet supported for
maps because map transfer would need changing.

---

## 5. String tables: `.csf` vs `.str`

`Core/GameEngine/Source/GameClient/GameText.cpp`, interface in
`Core/GameEngine/Include/GameClient/GameText.h`.

Two file names, supplied by the platform main
(`GeneralsMD/Code/Main/SDL3Main.cpp:179-181`; the Win32 main has the same pair at
`WinMain.cpp:81-82`):

```cpp
const Char *g_csfFile = "data/%s/generals.csf";  // %s = GetRegistryLanguage()
const Char *g_strFile = "data/Generals.str";
```

Note SDL3Main deliberately lower-cases `generals.csf` for case-sensitive hosts and
uses forward slashes.

### Selection order — the surprising bit

`GameTextManager::init` (`GameText.cpp:293-436`):

```cpp
if ( m_useStringFile && getStringCount( g_strFile, m_textCount ) )   // :318
        format = STRING_FILE;
else if ( getCSFInfo( csfFile, ... ) )                               // :322
        format = CSF_FILE;
```

`m_useStringFile` is initialised to `g_useStringFile` in debug builds and to a
**hard-coded `TRUE` in release** (`GameText.cpp:258-264`). The only switch that clears
it, `-UseCSF`, is inside `#if defined(RTS_DEBUG)`
(`CommandLine.cpp:184-188`, registered at `:1213`).

**Consequence: if `Data/Generals.str` exists anywhere the VFS can see it — loose file
or inside any BIG — it completely replaces the compiled `.csf`, in every build
configuration, with no way to disable it in a release build.** Also, `m_language` is
never set on the STR path (it is only read from the CSF header, `:932`), and the
CSF fallback table described below is skipped entirely (`:384` gates on
`format == CSF_FILE`).

### CSF binary format

`CSFHeader` (`GameText.cpp:110-119`), all little-endian `Int`s:

```
"CSF " magic (:76, checked at :928)   version (:80, >=2 carries langid)
num_labels    num_strings    skip    langid
[skip] bytes are seeked over (:989-992)
then, num_labels times:
  "LBL "  num_strings  labelLen  labelBytes
    num_strings times:
      "STR " or "STRW"  charCount  UTF-16 chars (bitwise NOT'd)
      if "STRW": waveLen  waveBytes   (the speech/audio file reference)
```

* Strings are stored **bitwise-inverted** UTF-16; the reader un-inverts
  (`:1052-1065`). On non-Windows the mask is `& 0x0000FFFF` because `wchar_t` is
  32-bit there, and the raw read goes through a `uint16_t` staging buffer
  (`:1035-1049`). This is a GeneralsX port fix — don't "simplify" it away.
* Only the **first** string per label is kept (`:1060`, `:1096`).
* Labels go into a `StringLookUp` array sorted with `qsort` and searched with
  `bsearch` (`:368-381`, `:1386`).

### The fallback instance (GeneralsX addition)

`GameText.cpp:383-434`: after loading CSF instance 0, the manager loads the **same
path at `FileInstance 1`** — i.e. the CSF in the *next* archive down — into a second
table. `fetch()` then searches, in order: main table → map table → fallback table
(`:1386-1397`). This exists so a mod that ships a partial `generals.csf` does not
blank out every string it did not translate.

### `.str` text format

`parseStringFile` (`GameText.cpp:1132-1222`): label on its own line, then a
`"quoted string"` line (with `\n`, `\t` escapes and an optional trailing wave-file
token), then `END`. `//` starts a comment. Duplicate labels are a `DEBUG_CRASH` only.
`getStringCount` (`:873-910`) counts `END` tokens and then **adds 500 slots of slack**
(`:907`), so the string array always contains 500 empty entries.

### Map strings

`initMapStringFile` (`:1233-1259`) builds a separate table from `<mapdir>/map.str`
(triggered at `GameLogic.cpp:2582-2586`). Note that `GameTextManager::reset()`
(`:483-492`) frees **only** the map table, not the main one — the main table survives
across games.

---

## 6. Map cache

`Core/GameEngine/Include/GameClient/MapUtil.h:96-132`,
`Core/GameEngine/Source/GameClient/MapUtil.cpp`. Parse side is
`GeneralsMD/Code/GameEngine/Source/Common/INI/INIMapCache.cpp`.

`MapCache` is a `std::map<AsciiString, MapMetaData>` keyed by the **lower-cased map
path**. `MapCache.ini` (`MapUtil.cpp:343`) is a normal INI file full of `MapCache`
blocks — the block name is a quoted-printable-encoded path
(`INIMapCache.cpp:144-146`), and the fields (`fileSize`, `fileCRC`, `timestampLo/Hi`,
`numPlayers`, `extentMin/Max`, `nameLookupTag`, `Player_N_Start`, `supplyPosition`,
`techPosition`) are declared at `INIMapCache.cpp:103-133`.

Two directories (`MapUtil.cpp:346-356`):

* system maps: literally `"Maps"` (relative)
* user maps: `<userDataDir>/Maps`

`MapCache::updateCache()` (`:439-490`) runs, in order:

1. Optionally *build* the system `MapCache.ini` from disk — release builds require
   `-buildmapcache` (`CommandLine.cpp:665-669`, `:1307`); debug builds do it whenever
   a `Maps` folder exists (`:448-455`).
2. Load the **user** `MapCache.ini`.
3. Scan user maps on disk; rewrite the user cache if anything changed.
4. Load the **system** `MapCache.ini` last, "to prevent munkees getting rowdy"
   (`:483-488`) — i.e. official metadata deliberately overwrites any user entry with
   the same key.

Freshness is decided by `size == cached size && cachedCRC != 0` (`:618`) — the
timestamp fields are written and read but **not** compared. Cache misses call
`loadMap()` and a full `calcCRC()` (`:678`), which is slow, so a broken size match
means a stall at every menu open.

`writeCacheINI` (`:363-434`) uses a raw `fopen` (`:372`), not `TheFileSystem`. Reads
go through `TheFileSystem` (`:531-542`) which has the archive tree and the asset-root
fallback; the write does not. On macOS/Linux, where cwd is usually not the game
directory, the system-map cache is therefore read from one place and written to
another. Only relevant with `-buildmapcache`, but it is a real inconsistency.

---

## 7. Save games and `GameState`

`GeneralsMD/Code/GameEngine/Source/Common/System/SaveGame/GameState.cpp` and
`GameStateMap.cpp`.

* Location: `<userDataDir>/Save/` (`GameState.cpp:781-791`), extension `.sav`
  (`:72`), auto-named `########.sav` or `<desc>_%04d.sav` (`:452-540`).
* Format is a flat sequence of **named, length-prefixed blocks**, written by
  `GameState::xferSaveData` (`:1416-1530`):

  ```
  repeat:
     AsciiString blockName        e.g. "CHUNK_GameState"
     Int         blockSize        (XferBlockSize, Xfer.h:110)
     <blockSize bytes of snapshot data>
  until blockName == "SG_EOF"     (:71, :1483)
  ```

  `XferSave::beginBlock` writes a zero placeholder and remembers the offset;
  `endBlock` seeks back and patches the real size
  (`Core/GameEngine/Source/Common/System/XferSave.cpp:165-245`). `XferLoad` reads the
  size and uses it to skip unknown blocks
  (`XferLoad.cpp:121-150`).
* The block registry is `GameState::addSnapshotBlock` calls at `:327-344`:
  `CHUNK_GameState`, `CHUNK_Campaign`, `CHUNK_GameStateMap`, `CHUNK_TerrainLogic`,
  `CHUNK_TeamFactory`, `CHUNK_Players`, `CHUNK_GameLogic`, `CHUNK_Radar`,
  `CHUNK_ScriptEngine`, `CHUNK_SidesList`, `CHUNK_TacticalView`, `CHUNK_GameClient`,
  `CHUNK_InGameUI`, `CHUNK_Partition`, `CHUNK_ParticleSystem`, `CHUNK_TerrainVisual`,
  `CHUNK_GhostObject`. A second list, `SNAPSHOT_DEEPCRC_LOGICONLY` (`:347-352`), reuses
  the same machinery for CRC debugging.
* Because blocks are name-keyed and size-prefixed, **unknown blocks are skippable** —
  the format tolerates additive change. Individual blocks version themselves with
  `xferVersion` (`Xfer.h:56, 142`); e.g. `GameState::xfer` is at version 2
  (`:1642-1660`).
* `SAVE_FILE_TYPE_MISSION` saves write only the GameState and Campaign blocks
  (`:1447-1449`) — they are checkpoints, not full state.
* **The map is embedded in the save.** `GameStateMap::xfer`
  (`GameStateMap.cpp:249-395`) writes the whole `.map` file into the stream
  (`embedPristineMap` `:74-130` on first save, `embedInUseMap` `:132-180` afterwards)
  and, on load, extracts it back out to a scratch `.map` in the Save directory
  (`:393`, cleaned by `clearScratchPadMaps` `:458`). Map paths are normalised to
  portable prefixes `Save\`, `Maps\`, `UserData\Maps\`
  (`GameState.cpp:875-900`) so saves move between machines.

---

## 8. How to actually add or override data

Ordered from least invasive to most.

1. **Loose files.** Drop the file at the same VFS path as the archived one. Loose
   always beats archived (`FileSystem.cpp:180-217`). Best for iteration; no CRC
   implications beyond the INI CRC (§4).
   Example: `Data/INI/Object/AmericaVehicle.ini` next to the executable (or under the
   resolved asset root — the local FS falls back to it for relative paths,
   `StdLocalFileSystem.cpp:77-83`).
2. **New INI file in a `*.ini` directory.** `Data/INI/Object/MyStuff.ini`. Loaded by
   `loadFileDirectory` after `Object.ini` (`INI.cpp:193-246`), so redefining a block
   name replaces it. Nothing stock is edited.
3. **`-mod mymod.big`.** Guaranteed to win over every stock archive
   (`ArchiveFileSystem.cpp:213-229`, `overwrite = TRUE`) — but remember it mounts at
   `GameEngine.cpp:532`, too late for `GameData.ini`.
4. **A BIG dropped into the game directory.** Wins only if its filename sorts
   **before** the stock archives it is competing with (§3). `!MyMod.big` or
   `AAAMod.big`, not `zzz`.
5. **Per-map overrides.** `map.ini` / `solo.ini` next to the `.map`, loaded with
   `INI_LOAD_CREATE_OVERRIDES` (`GameLogic.cpp:2561-2578`); these are properly
   unwound on reset via `Overridable::deleteOverrides`.
6. **`SagePatch.ini`** in the user data dir — a GeneralsX-specific hook auto-created
   at `GameEngine.cpp:473-512` and loaded with `INI_LOAD_OVERWRITE` right after
   `GameData.ini`. The cleanest place for local `GameData` tweaks, and the only way to
   override `GameData` at all short of editing the stock file.

### What filename ordering implies, summarised

| Context | Rule | Cite |
|---|---|---|
| BIGs in the game directory | case-insensitive alphabetical, **first wins** | `FileSystem.h:66`, `ArchiveFileSystem.cpp:158-169` |
| BIGs in a `-mod` directory | case-insensitive alphabetical, **last wins** | `ArchiveFileSystem.cpp:233` + `:158-161` |
| `-mod` single BIG vs everything | mod wins | `ArchiveFileSystem.cpp:217-222` |
| ZH archives vs base Generals archives | ZH wins | `StdBIGFileSystem.cpp:498-509` |
| Entries inside one BIG | last entry wins | `ArchiveFile.cpp:122` |
| INI files inside a `Data\INI\<X>\` directory | alphabetical, **last wins**; `<X>.ini` is loaded before the directory | `INI.cpp:206-231`, `:274-297` |
| `Data\INI\Default\X` vs `Data\INI\X` | `Default` first, so `X` wins | `SubsystemInterface.cpp:176-190` |
| Loose file vs any archive | loose wins | `FileSystem.cpp:180-217` |

---

## 9. Gotchas

Ordered roughly by how likely they are to cost you a day.

1. **`.BIG` in upper case is invisible on macOS/Linux.**
   `StdLocalFileSystem::getFileListInDirectory` filters with
   `iter->path().extension() == searchExt` (`StdLocalFileSystem.cpp:313`), an exact
   `std::filesystem::path` comparison, which is case-sensitive on POSIX. The mask
   `"*.big"` therefore does not match `INIZH.BIG`. The Win32 implementation used
   `FindFirstFile` with the mask, which is case-insensitive
   (`Win32LocalFileSystem.cpp:137`). Same applies to `"*.ini"` and `"*.map"`. If an
   install "has no BIG files", check the casing first.
   The same comparison also ignores the *stem* of the mask — `"Foo*.ini"` would match
   every `.ini`. Nothing passes such a mask today, but don't add one.

2. **Subdirectory recursion in the Std local FS loses path components.**
   `StdLocalFileSystem.cpp:342-345` recurses with `tempsearchstr = <leaf name only>`,
   and the search path is rebuilt as `originalDirectory + currentDirectory`
   (`:284-286`). The Win32 version accumulates the path instead
   (`Win32LocalFileSystem.cpp:170-176`). Consequences:
   * When the directory is passed as `originalDirectory` with a trailing separator —
     which is what `FileSystem::getFileListInDirectory` (`FileSystem.cpp:289`) and
     `INI::loadDirectory` (`INI.cpp:270-271`) do — the **first** level of recursion is
     correct and deeper levels are not.
   * When the directory is passed as `currentDirectory` — which is what
     `loadBigFilesFromDirectory` does (`StdBIGFileSystem.cpp:654`) — `originalDirectory`
     is empty and recursion searches the bare subdirectory name **relative to cwd**.
     So the `Data\INI\INIZH.big` skip at `StdBIGFileSystem.cpp:663` can only ever fire
     when cwd happens to be the game directory.
   I have read this carefully but not instrumented it; if you are chasing "my BIG in a
   subfolder is not loading", this is the first place to look.

3. **Path separators are not normalised between the two file systems.** The local FS
   returns forward-slash paths on POSIX (`StdLocalFileSystem.cpp:294-297, 317`) while
   the archive FS returns whatever the caller passed plus backslashes
   (`ArchiveFile.cpp:159-176`). Both go into the *same* `FilenameList` set, whose
   comparator is `_stricmp` (`STLTypedefs.h:238-244`) and therefore treats `/` and `\`
   as different characters. A file that exists both loose and archived can appear
   **twice** in the list, and `INI::loadDirectory` would parse it twice. Unverified at
   runtime; easy to test by putting a duplicate `.ini` on disk and counting the
   `[INI] load(...)` traces.

4. **`Data/Generals.str` silently beats the CSF in release builds** and there is no
   way to turn it off (`GameText.cpp:261-263`, `:318`; `-UseCSF` is debug-only,
   `CommandLine.cpp:184`, `:1213`). If localisation "reverted to English", look for a
   stray `.str`.

5. **Mods cannot override `GameData.ini`** — `loadMods()` at `GameEngine.cpp:532` runs
   after `GameData` is parsed at `:470`. Use `SagePatch.ini` (`:473-512`) instead.

6. **Unknown INI fields are silent in release.** `DEBUG_CRASH` compiles out
   (`Debug.h:205`, `INI.cpp:1607`). So do duplicate `Object` definitions
   (`ThingFactory.cpp:403`) and duplicate string labels (`GameText.cpp:1163`, `:1294`).
   Validate mods in a debug build.

7. **Anything you add to a CRC'd INI directory changes `m_iniCRC`.** The `xferCRC`
   passed to most `initSubsystem` calls hashes every line of every file loaded
   (`INI.cpp:556-560`, `GameEngine.cpp:444`, `:722-723`). There are hard-coded expected
   CRCs guarded by `RETAIL_COMPATIBLE_CRC` at `GameEngine.cpp:575` and `:654`.
   Modded clients will not match stock clients in multiplayer.

8. **Debug `fprintf(stderr, ...)` spam is compiled into all configurations** in the
   data path: `INI::load` prints per-file and every 100 lines
   (`INI.cpp:399, 420-423, 466`), `loadFileDirectory`/`loadDirectory` print per call
   (`:196, 216, 229, 256`), `initSubsystem` prints five lines per subsystem
   (`SubsystemInterface.cpp:160-194`), `parseCSF` prints every 500 labels
   (`GameText.cpp:1109`). With thousands of INI files this is a measurable
   startup cost and it is not behind `DEBUG_LOG`. Cleaning it up is a self-contained
   task.

9. **The BIG entry-path read has no bounds check** (`StdBIGFileSystem.cpp:589-592`).
   Fine for trusted retail archives, not fine for anything downloaded.

10. **The file-existence cache can go stale.** `ENABLE_FILESYSTEM_EXISTENCE_CACHE`
    defaults to 1 (`GameDefines.h:144-145`) and `FileSystem::doesFileExist` memoises
    negative answers (`FileSystem.cpp:273-279`). Files created behind the VFS's back —
    e.g. `MapCache::writeCacheINI`'s raw `fopen` (`MapUtil.cpp:372`), or the
    `SagePatch.ini` bootstrap (`GameEngine.cpp:479-509`) — do not invalidate it. I did not
    find a live bug from this, but any new "write a file then check it exists" code
    should use `TheFileSystem`.

11. **`INI::isValidINIFilename` is dead code** — defined at `INI.cpp:161-167`,
    declared at `INI.h:389`, called from nowhere. Filtering is done by the `"*.ini"`
    mask instead (which, per gotcha #1, is extension-only and case-sensitive on POSIX).

12. **`GetRegistryLanguage()` caches its first answer forever** (`registry.cpp:450-463`,
    a function-local `static`). On non-Windows it is resolved from `CNC_ZH_LANGUAGE`,
    then a registry shim, then by probing for `<Language>ZH.big`
    (`registry.cpp:201-241`) in `CNC_GENERALS_ZH_PATH` / `CNC_GENERALS_PATH` /
    `CNC_ZH_INSTALLPATH` / cwd — in a fixed precedence order with `brazilian` first.
    It drives `Data/%s/generals.csf`, `Data/%s/CommandMap`, `Data/%s/Language`,
    `Data/%s/Art/W3D/`, `Data/%s/Art/Textures/` and localised audio, so getting it
    wrong misroutes a lot at once.

---

## 10. Where to look next

| Question | File |
|---|---|
| Which BIG did this file come from? | `ArchiveFileSystem::loadIntoDirectoryTree`, enable `ENABLE_FILESYSTEM_LOGGING` (`FileSystem.h:105-107`) — it logs shadowing pairs (`ArchiveFileSystem.cpp:171-207`) |
| Where do `.w3d` / `.tga` / `.dds` lookups go? | `GameFileClass::Set_Name`, `GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DFileSystem.cpp:165-330` — localised dir → `Art/W3D/` / `Art/Textures/` → test art → user data → map previews → cross-extension `.tga`↔`.dds` retry |
| What is in a `.map`? | `GeneralsMD/Code/GameEngine/Source/Common/System/DataChunk.cpp` — a chunked format with a string table of chunk names at the head (`:592-599`) |
| Full INI block-type list | `Core/GameEngine/Source/Common/INI/INI.cpp:91-155` |
| Per-block field tables | `Core/GameEngine/Source/Common/INI/INI*.cpp` and `GeneralsMD/Code/GameEngine/Source/Common/INI/INI*.cpp` |
| User data dir resolution | `GeneralsMD/Code/GameEngine/Source/Common/GlobalData.cpp:1378-1476` — macOS: `~/Library/Application Support/GeneralsX/GeneralsZH/`; Linux: XDG |
| Asset root resolution | `StdBIGFileSystem.cpp:317-481`, tagged `[ASSET_ROOT]` in stderr output |
