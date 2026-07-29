# SimID source digest, measured per commit

Measured 2026-07-29 by running the real script in script mode against a **clean** worktree
checked out at each commit — not by reasoning about which paths changed.

```bash
git worktree add --detach /tmp/head-clean <commit>
cmake -DSIMDIGEST_BUILD_TIME=TRUE \
      -DSIMDIGEST_WORKING_DIR=/tmp/head-clean \
      -DGIT_EXECUTABLE=$(which git) \
      -DSIMDIGEST_ALLOW_NO_GIT=OFF \
      -DSIMDIGEST_PRE_CONFIGURE_FILE=/tmp/head-clean/resources/gitinfo/simsourcedigest.cpp.in \
      -DSIMDIGEST_POST_CONFIGURE_FILE=/tmp/sd.cpp \
      -P /tmp/head-clean/resources/gitinfo/simsourcedigest_watcher.cmake
```

| commit | rev | ZH | G | joins the peers deployed at `c72eb8d96`? |
|---|---|---|---|---|
| `c72eb8d96` | 2187 | `0xB30B651C` | `0xE827650A` | yes — *was* deployed until 2026-07-29 |
| `68eff86c4` | 2188 | `0xB30B651C` | `0xE827650A` | yes |
| `6377956d8` | 2189 | `0xB30B651C` | `0xE827650A` | yes |
| `1566959a9` | 2190 | `0xB30B651C` | `0xE827650A` | yes |
| `73e192787` | 2191 | `0xB30B651C` | `0xE827650A` | yes |
| `060752a15` | 2192 | `0xB30B651C` | `0xE827650A` | **yes — newest commit that still joins** |
| `4cb42f38e` | 2193 | `0x89C01A42` | `0xE827650A` | **NO — `SOURCE_DIFFERS`** |
| `465075c00` | 2194 | `0x89C01A42` | `0xE827650A` | no — `SOURCE_DIFFERS` |
| `300f43652` | 2195 | `0xF6E71C2F` | `0xE827650A` | no — `SOURCE_DIFFERS` |
| `57dc6bbe9` | 2196 | `0xF6E71C2F` | `0xE827650A` | no — `SOURCE_DIFFERS` |
| `643b48a7c` | 2197 | `0xBB5A1FF3` | `0xE827650A` | no — `SOURCE_DIFFERS` |
| `baed11d79` | 2198 | `0xBB5A1FF3` | `0xE827650A` | no — `SOURCE_DIFFERS` |
| `e9e94fabe` | 2199 | `0xD33D97FD` | `0xE827650A` | no — `SOURCE_DIFFERS` |
| `3ee0723dd` | 2200 | `0xD33D97FD` | `0xE827650A` | no — **this is what is now deployed** |

`G` never moves: nothing in this range touches a `Generals/` path. Re-verified through `3ee0723dd`
— the only non-`GeneralsMD/` source change in `4cb42f38e..3ee0723dd` is to
`Core/GameEngineDevice/`, which is **not** one of the digested `Core/` paths
(`simsourcedigest_watcher.cmake` digests `Core/GameEngine/{Include,Source}/{Common,GameLogic,GameNetwork}`,
`Core/Libraries/Include` and `Core/Libraries/Source/WWVegas/WWMath` only), and no
`cmake/`, `triplets/`, `CMakePresets.json` or `vcpkg*.json` changed in that range either.

## Current deploy state — all three peers rebuilt and verified 2026-07-29

All three are now deployed at `3ee0723dd` and **agree**. Each line below was measured at runtime on
its own box this session with `-headless -lanhost <name> -lanframes 5`, not inferred:

| peer | binary | `source` | `platform` |
|---|---|---|---|
| macOS | `~/GeneralsX/GeneralsZH/GeneralsXZH` | `D33D97FD` | `BF013216` |
| Linux VPS | `/root/gamedata/GeneralsXZH` | `D33D97FD` | `97FE082D` |
| Windows | `C:\dev\GeneralsX-run\generalszh.exe` | `D33D97FD` | `7480F925` |

`engine=FD486019 data=1839A83F ordinal=9F43F7B5 parse=CD8F767F asset=8E504171`,
`data-ini=FEAAE3F3 data-loose=300665B3`, `epoch=2 tag=53494431`, `rev=2200` on all three.
`platform` is the OS tag and is deliberately **not** part of the compatibility verdict.

The static measurement agrees with all three: running the script-mode command above against a clean
worktree at `3ee0723dd` prints `ZH=0xD33D97FD G=0xE827650A`.

Two deploy-path traps found while doing this, both worth keeping:

* `scripts/build/linux/deploy-linux-zh.sh` installs to `$HOME/GeneralsX/GeneralsZH`, but the
  three-platform harness runs the Linux peer from `VPS_GAME=/root/gamedata`
  (`xplat-3platform-lobby.sh:87`). On the VPS `$HOME/GeneralsX/GeneralsZH` holds only an empty
  `Data/`, so running the deploy script alone leaves the harness on the **old** binary and the run
  silently tests a stale peer. The Linux binary must be copied to `/root/gamedata/GeneralsXZH`.
* There is no Docker or Podman on the VPS and no `docker` binary on the Mac, so
  `docker-build-linux-zh.sh` cannot run on either box. The VPS builds natively:
  `cmake --build build/linux64-deploy --target z_generals` against its existing Ninja cache.

## Build strategy that followed from the old `B30B651C` deploy — HISTORY, superseded

Kept for the reasoning, not as current advice. When this was written, the deployed macOS, Linux and
Windows binaries all reported `source=B30B651C`:

* **To join the already-deployed peers without redeploying anything, build `060752a15`.**
  It is the newest commit that still produces `0xB30B651C`, and it carries the whole soak rescue.
* To build at current `main` (`4cb42f38e`), **every** peer must be rebuilt at `4cb42f38e`. They
  will agree with each other at `0x89C01A42` but none of them will join a binary still deployed
  at `c72eb8d96`. Mixing the two is refused before `sourceID` is even reached on `engineID`
  equality — no, more precisely: `engineID` matches (same epoch), `sourceID` does not, and
  `SimIdCompare` reports `SOURCE_DIFFERS`.

## CORRECTION to `4cb42f38e`'s own commit message

That message states the digest moves `0xB30B651C -> 0x0CFC0F61`. **The `0x0CFC0F61` is wrong.**

`0x0CFC0F61` was measured against the *dirty working tree* — the four edits present but
uncommitted. `SimDigestCollect` hashes the `git ls-tree` text **plus** a `#dirty` overlay listing
each modified path against its `git hash-object` SHA (`simsourcedigest_watcher.cmake:146-190`).
Committing the identical file contents moves those SHAs from the overlay into the tree listing and
leaves the overlay empty, so the hashed input text differs and the digest necessarily differs too.

**Committed, `4cb42f38e` measures `0x89C01A42`.** The commit message's *conclusion* is unaffected
and still correct: a binary built from it is refused by the peers deployed at `c72eb8d96`.

The general rule worth keeping: **a digest measured on a dirty tree never equals the digest of the
same content committed.** Quote a digest only with the tree state it was measured on.
