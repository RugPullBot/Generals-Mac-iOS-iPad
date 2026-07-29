# `xplat-soak-50x1500` — the raw streams, rescued from a scratchpad

**What this is.** The 100 `[GXCRC]` streams behind the project's largest determinism claim:
50 solo headless matches, each 1500 frames, run on macOS and on Linux, compared pairwise.
75,000 frames per platform, 150,000 total.

**Why it is here.** Until 2026-07-29 these files existed *only* in another session's scratchpad
(`/tmp/claude-501/.../1f6becd6-.../scratchpad/soak50`). Only the meta, manifest and console were in
git, so the largest claim in the project rested on a directory that a reboot deletes. The three
summary files remain in the parent directory unchanged:
`../xplat-soak-50x1500.meta.txt`, `../xplat-soak-50x1500-manifest.txt`,
`../xplat-soak-50x1500-console.txt`.

## Verified on rescue, not assumed

Every pair was recomputed from these files before they were committed:

| check | result |
|---|---|
| pairs present | 50 of 50 (`NNN.mac.crc` + `NNN.linux.crc`) |
| `cmp` mac vs linux | **identical, all 50** |
| whole-file md5 vs `../xplat-soak-50x1500-manifest.txt` | matches on **both** files, all 50 |
| line count vs manifest `frames` | 1500, all 100 files |
| distinct values vs manifest `distinct` | matches, all 50 (range 1470–1500) |
| total frames | **75,000** per platform |

The manifest's `md5` column is the **whole-file** md5 of either stream (they are byte-identical),
not a value-column md5. Reproduce any row with `md5 -q NNN.mac.crc`.

## Provenance — `PROVENANCE.tsv`

Extracted from the per-iteration stderr logs, which were **not** committed (43 MB, almost entirely
`[INI]` load spam). One row per iteration: map, map capacity, `aiPlayers`, occupied slots, lobby
legality, pinned seed, frames, distinct, md5, pair verdict.

Shape of the run: **10 maps × 5 seeds**, seed pinned per iteration (`SEED_BASE + 977n`, base
700000), single build on both platforms —
`rev=2171 engine=4D31F2F2 source=6A9BFF78`, i.e. `2e226bf3a`, **epoch 1**. That is *not* the
epoch-2 build deployed today, so this corpus cannot be extended by a run at current HEAD; it is a
self-contained result at rev 2171.

## Read this before citing the row: 7 of the 50 were over-capacity

Cross-referencing each iteration's map against its measured capacity (see
`../networked-sim-freeze-diagnosis.md` §1.2) shows **7 iterations ran a 3-slot lobby on a 2-start
map**: `dust devil` ×3, `bitter winter` ×2, `desert fury` ×2 — all with `aiPlayers=2`.

In those 7, slot 0 is the host human and slots 1–2 are the AI. Only two start positions exist, so
**the second AI got `startPos=-1`, no Command Center, and was inert for the whole match** — the
same defect that manufactured the "networked freeze". They still score 1500 distinct because slot 1
always receives a real position, so one AI was always live. This is degradation, not a freeze.

**What that does and does not invalidate:**

* **The determinism claim stands, all 50 rows.** macOS and Linux computed the identical world
  byte-for-byte, including the identical *wrong* one. Capacity cannot cause a cross-platform
  divergence — it is the same deterministic function of the same shared `GameInfo` on both sides.
* **Any per-row claim about how many AI were simulating is wrong for those 7 rows.** They ran with
  one live AI, not two. `PROVENANCE.tsv` marks them `OVER-CAPACITY`.
* The remaining 43 rows are legal lobbies.

Nothing here needed re-running: the defect is in what was measured, not in what the engine did.
