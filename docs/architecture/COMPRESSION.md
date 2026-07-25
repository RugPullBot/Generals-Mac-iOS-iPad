# Compression and untrusted input

How the four compression codecs are selected and decoded, which of them can be
reached from data the player did not author, and what was changed to stop a
crafted stream from writing past its destination buffer.

Written 2026-07-25 against commit `09b8045c4` plus the working-tree changes
described in "What changed" below. Line numbers drift — grep the quoted
identifier, not the number.

---

## 1. The dispatch layer

Everything funnels through two functions in
[`Core/Libraries/Source/Compression/CompressionManager.cpp`](../../Core/Libraries/Source/Compression/CompressionManager.cpp):

```
CompressionType getCompressionType( const void *mem, Int len )
Int             decompressData( void *src, Int srcLen, void *dest, Int destLen )
```

`getCompressionType` is a `memcmp` chain over the first **four bytes** of the
buffer. That four-byte magic is the *only* thing that selects the codec — there
is no separate manifest, no per-file registry, no cross-check against the
container. Whoever controls those four bytes controls which decoder runs.

| Magic | `CompressionType` | Decoder | Destination bounded? |
|---|---|---|---|
| `EAR\0` | `COMPRESSION_REFPACK` | `REF_decode` (EAC RefPack) | **was: no** → now yes |
| `EAB\0` | `COMPRESSION_BTREE` | `BTREE_decode` | no → **now refused** |
| `EAH\0` | `COMPRESSION_HUFF` | `HUFF_decode` | no → **now refused** |
| `NOX\0` | `COMPRESSION_NOXLZH` | `DecompressMemory` | yes (takes `destLen`) |
| `ZL1\0`…`ZL9\0` | `COMPRESSION_ZLIB1..9` | zlib `uncompress` | yes (in/out length) |

`decompressData` accepts a `destLen` parameter. Before this change it **passed
that value to zlib and NOXLZH and silently discarded it for all three EAC
codecs** — they were called as `REF_decode(dest, src+8, &slen)`, with no
argument through which the destination extent could be expressed.

The `+8` skips the game's own 8-byte header (4-byte magic, 4-byte uncompressed
size); the codec's own header starts after it.

## 2. Why this is reachable from untrusted data

Three call sites reach `decompressData`:

- **`GeneralsMD/…/Common/System/DataChunk.cpp:98`** — every `.map` file. Map
  *enumeration* decompresses each map it finds in order to read its metadata,
  so this runs at startup against every map present on disk, including maps a
  peer transferred during a lobby session that the player never opened.
- **`Core/GameEngine/Source/GameNetwork/ConnectionManager.cpp:843`** — inbound
  peer packets. This is the sharper edge: no file has to reach disk first.
- **`Core/Tools/Compress/Compress.cpp:190`** — the standalone dev tool.

The producing side never varies: `getPreferredCompression()` returns
`COMPRESSION_REFPACK` unconditionally, and WorldBuilder only reuses whatever
codec a map already declared (`WorldBuilderDoc.cpp:253`, falling back to
`getPreferredCompression()` when the dict key is absent or out of range).

## 3. What the RefPack decoder actually did

`REF_decode` in
[`EAC/refdecode.cpp`](../../Core/Libraries/Source/Compression/EAC/refdecode.cpp)
parses the stream's own declared uncompressed length (`ulen`) out of the header
— and then never uses it. Every write is a bare `*d++` and every read a bare
`*s++`, across four opcode forms:

| Opcode form | Literal bytes | Back-reference copy | Max written |
|---|---|---|---|
| short (`!(first&0x80)`) | `first&3` (0–3) | `((first&0x1c)>>2)+3` | ~13 |
| int (`!(first&0x40)`) | `second>>6` (0–3) | `(first&0x3f)+4` | ~69 |
| very int (`!(first&0x20)`) | `first&3` (0–3) | `((first&0x0c)>>2<<8)+forth+5` | ~1031 |
| literal / eof | up to 112 / `first&3` | — | 112 |

The loop terminates only on the eof opcode. A stream that declares
`ulen = 4096` and then supplies 80 literal opcodes writes 8960 bytes into a
4096-byte allocation. Nothing in the decoder or its caller notices.

The back-reference is also unvalidated: `ref = d-1-offset` with a 17-bit
offset can point *before* the start of the destination, so the copy reads
out-of-bounds heap and splices it into the output.

## 4. What changed

### 4.1 `REF_decode_bounded`

A new entry point beside the original, declared in `refcodex.h`:

```c
int GCALL REF_decode_bounded(void *dest, int destsize,
                             const void *compresseddata, int compressedsize,
                             int *compressedsizeout);
```

The original `REF_decode` is left untouched so existing callers and the dev
tools keep working. The bounded variant:

- takes **both** extents and gates every read and write against them. Each
  opcode's run lengths are known before its writes, so the checks are exact,
  not conservative.
- rejects a declared `ulen` greater than `destsize` up front rather than
  part-way through.
- rejects a back-reference that underflows the start of the destination.
- returns `0` on any violation, matching the existing failure contract.

`CompressionManager::decompressData` now calls it, passing the `destLen` it had
been discarding.

### 4.2 BTREE and HUFF are refused

Both remain unbounded, and `BTREE_chase` additionally recurses over a
`left`/`right` table indexed by stream-supplied bytes — a cyclic tree exhausts
the stack. Rather than rewrite two more decoders with no test corpus to
validate against, `decompressData` now refuses both magics.

This costs nothing measurable. A scan of the 37 BIG archives in a full Zero
Hour install found **171 maps: 166 `EAR\0`, 4 uncompressed `CkMp`, 1 `ZL5\0` —
no `EAB` or `EAH` anywhere**. Nothing in the tree ever requests either codec.
The trade is that `Core/Tools/Compress` can no longer decompress those two
formats; it has no inputs of them either.

If a genuine BTREE/HUFF asset ever appears, bound those decoders the way
RefPack is bounded before re-enabling the path.

## 5. Verification

Three separate checks, all in-tree reproducible against
`Core/Libraries/Source/Compression/EAC/`:

**Round-trip equivalence — 64 trials, 0 failures.** Four content shapes (constant
runs, byte ramp, PRNG noise, mixed) × 16 sizes from 1 byte to 70 000. For each,
the bounded decoder's output was compared byte-for-byte against the original
`REF_decode`, and both against the pre-compression source. Return value and
bytes-consumed matched in every case. The bounded decoder is not merely safe,
it is output-identical.

**Guard-page mutation fuzz — 150 000 streams, 0 overruns.** Valid streams were
mutated (truncation, bit flips, byte splats, forced long runs), then decoded
with both the source and destination buffers placed flush against a `PROT_NONE`
guard page, so a single byte of overrun on either side raises SIGSEGV. Result:
35 901 accepted, 114 099 refused, zero faults, zero out-of-range return values.
The 24% acceptance rate matters — it confirms the deep decode paths were
actually exercised rather than everything being rejected at the header.

**Differential proof.** A hand-built stream declaring `ulen = 4096` that
actually expands to 8960 bytes, decoded into a guard-page-backed 4096-byte
buffer:

```
bounded decoder ... returned 0 (refused, no fault)
legacy decoder  ... SIGBUS
```

Same stream, same buffer. That is precisely the hole that was closed.

> Note: AddressSanitizer hangs at process start on this machine (macOS 25.5) —
> even a static `hello world` built with `-fsanitize=address` never reaches
> `main`. The guard-page technique above was used instead and is strictly
> stronger for detecting linear overruns, though it does not catch
> intra-allocation errors the way ASan would.

## 6. Gotchas

**`REF_encode` and `REF_decode` in this tree disagree on header byte order.**
`REF_encode` emits the magic little-endian (`FB 10`); `REF_decode` reads it
big-endian, so it sees `0xFB10`, takes the `&0x8000` four-byte-size branch, and
walks off the end of a buffer it just produced. Shipped game data is `10 FB` and
decodes correctly. This costs an hour if you try to build a test corpus with the
in-tree encoder and feed it straight to the in-tree decoder — swap bytes 0 and 1
after encoding. `REF_size` and `REF_is` handle both, which makes the mismatch
harder to spot. This is upstream behaviour, unchanged here.

**The 8-byte game header is not the codec header.** `decompressData` passes
`src+8`. Off-by-eight when calling a decoder directly gives plausible-looking
garbage rather than an obvious failure.

**`getCompressionType` is content sniffing, not validation.** It reads four
bytes and commits. Any hardening has to happen after dispatch, per codec.

**`DecompressMemory` (NOXLZH) returns `destLen` on success**, not the number of
bytes it actually wrote, so the caller cannot distinguish a full buffer from a
partial one. Not exploited by anything today; noted because it looks like a
length and is not one.

## 7. Not done

- `Generals/` (base game) carries the same `decompressData`. It is not compiled
  for iOS (`RTS_BUILD_GENERALS=OFF`), so it was left alone rather than shipping
  edits that cannot be compile-verified here.
- `BTREE_decode` / `HUFF_decode` are refused, not fixed. `BTREE_chase`'s
  unbounded recursion is unaddressed behind that refusal.
- `Compress.cpp:190` ignores `decompressData`'s return value entirely — it
  cannot currently tell a refusal from a success.
