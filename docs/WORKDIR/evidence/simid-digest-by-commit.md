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
| `c72eb8d96` | 2187 | `0xB30B651C` | `0xE827650A` | yes — **this is what is deployed** |
| `68eff86c4` | 2188 | `0xB30B651C` | `0xE827650A` | yes |
| `6377956d8` | 2189 | `0xB30B651C` | `0xE827650A` | yes |
| `1566959a9` | 2190 | `0xB30B651C` | `0xE827650A` | yes |
| `73e192787` | 2191 | `0xB30B651C` | `0xE827650A` | yes |
| `060752a15` | 2192 | `0xB30B651C` | `0xE827650A` | **yes — newest commit that still joins** |
| `4cb42f38e` | 2193 | `0x89C01A42` | `0xE827650A` | **NO — `SOURCE_DIFFERS`** |

`G` never moves: nothing in this range touches a `Generals/` path.

## Build strategy that follows

The deployed macOS, Linux and Windows binaries all report `source=B30B651C`. So:

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
