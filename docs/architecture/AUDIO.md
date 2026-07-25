# Audio Architecture (OpenAL backend)

Scope: the OpenAL device layer that replaced Miles Sound System — `OpenALAudioManager`,
`OpenALAudioStream`, `OpenALAudioFileCache` — and the engine-side plumbing
(`AudioManager`, `SoundManager`) that feeds it.

Everything below was read from the tree; every non-obvious claim cites `file:line`.
Line numbers are from the state of the repo at the time of writing — if a citation looks
off by a few lines, the surrounding function name is the reliable anchor.

---

## 1. How it fits together (read this first)

```
AudioEventRTS (game code)
      |  TheAudio->addAudioEvent()
      v
AudioManager::addAudioEvent            Core/GameEngine/Source/Common/Audio/GameAudio.cpp:384
   - resolves AudioEventInfo, applies on/off + disallowSpeech gates
   - clones the event onto the heap, allocates the AudioHandle
      |
      v
MusicManager / SoundManager::addAudioEvent   GameSounds.cpp:136
   - SoundManager::canPlayNow() admission control (distance, shroud, voice, limit, channels)
      |  AudioRequest{ AR_Play, m_pendingEvent }
      v
AudioManager::m_audioRequests  (a queue, drained once per engine tick)
      |
      v
OpenALAudioManager::update()           OpenALAudioManager.cpp:544
   processRequestList -> processPlayingList -> processFadingList -> processStoppedList
      |
      v
OpenALAudioManager::playAudioEvent()   OpenALAudioManager.cpp:727
      |
      +-- AT_SoundEffect, non-positional --> PAT_Sample   : plain AL source + whole cached buffer
      +-- AT_SoundEffect, positional     --> PAT_3DSample : plain AL source + AL_POSITION
      +-- AT_Music / AT_Streaming        --> PAT_Stream   : OpenALAudioStream, 32 queued buffers
                                                            refilled from FFmpeg
```

Three parallel lists of live sounds hang off the manager
(`OpenALAudioManager.h:218-220`): `m_playingSounds` (PAT_Sample), `m_playing3DSounds`
(PAT_3DSample), `m_playingStreams` (PAT_Stream). Plus `m_fadingAudio` and a
`m_stoppedAudio` list that is effectively unused — every call site that would push onto it
is commented out in favour of an immediate `releasePlayingAudio()` (see
`OpenALAudioManager.cpp:1047`, `:2424`, `:2460`, `:2496`, `:2549`), so
`processStoppedList()` (`:2674`) normally iterates an empty list.

The whole thing is driven synchronously from the engine tick:
`GameEngine::update()` calls `TheAudio->UPDATE()`
(`GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp:1072`). There is **no audio
thread**. Decoding, buffer refills, EOF detection and 3D position updates all happen on
the game thread at logic rate. A frame hitch is an audio underrun.

### Where the code lives

| Path | What |
| --- | --- |
| `Core/GameEngineDevice/Include/OpenALAudioDevice/OpenALAudioManager.h` | manager interface, `PlayingAudioType` |
| `Core/GameEngineDevice/Include/OpenALAudioDevice/OpenALAudioStream.h` | streaming source + audio diagnostics API |
| `Core/GameEngineDevice/Source/OpenALAudioDevice/OpenALAudioManager.cpp` | the 3.2 kloc bulk of the backend |
| `Core/GameEngineDevice/Source/OpenALAudioDevice/OpenALAudioStream.cpp` | queue/refill/underrun/EOF state machine |
| `Core/GameEngineDevice/Source/OpenALAudioDevice/OpenALAudioCache.{h,cpp}` | fully-decoded sample cache, `struct PlayingAudio` |
| `Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegFile.cpp` | demux/decode used by both the cache and the streams |
| `GeneralsMD/Code/GameEngineDevice/Source/SDL3GameEngine.cpp:1252` | `createAudioManager()` — where the backend is instantiated |

Build wiring: `Core/GameEngineDevice/CMakeLists.txt:249-278` adds the three sources when
`SAGE_USE_OPENAL` is on (the macOS/iOS presets set it, `CMakePresets.json:208/230/261`;
the option itself defaults OFF, `cmake/config-build.cmake:15`).

> **Dead twin.** `GeneralsMD/Code/GameEngineDevice/Source/OpenALAudioManager.cpp` (639
> lines, its own class with `m_sources2D`/`m_sources3D` vectors) is an *earlier, unrelated*
> implementation. It is compiled only when `SAGE_USE_OPENAL` is **off**
> (`GeneralsMD/Code/GameEngineDevice/CMakeLists.txt:215-219`), i.e. never in the shipping
> presets. `GeneralsMD/.../Include/OpenALAudioManager.h` is a one-line forwarder to the
> Core header. Do not edit the twin thinking you are fixing the game.

---

## 2. PAT_Sample vs PAT_3DSample vs PAT_Stream

`enum PlayingAudioType { PAT_Sample, PAT_3DSample, PAT_Stream, PAT_INVALID }` —
`OpenALAudioManager.h:37-43`. It is stored on `PlayingAudio::m_type`
(`OpenALAudioCache.h:40`) and decides which list the sound lives in, how volume is
computed, and which handle is the "real" AL object.

```c
struct PlayingAudio {                    // OpenALAudioCache.h:34
    ALuint             m_source;         // real AL source for samples; ALWAYS 0 for streams
    OpenALAudioStream* m_stream;         // non-null only for PAT_Stream
    FFmpegFile*        m_ffmpegFile;     // non-null only for PAT_Stream
    PlayingAudioType   m_type;
    AudioEventRTS*     m_audioEventRTS;
    ALuint             m_bufferHandle;   // cache-owned AL buffer; only for samples
    Bool               m_requestStop;
    Bool               m_cleanupAudioEventRTS;
    Int                m_framesFaded;
};
```

**`PAT_Sample` — 2D sound effect.** One `alGenSources` source
(`OpenALAudioManager.cpp:977`), one *whole* pre-decoded AL buffer from the cache attached
with `alSourcei(AL_BUFFER, …)` and played (`playSample`, `:2956-2967`). No queueing. The
source is forced `AL_SOURCE_RELATIVE = AL_TRUE` at `:2961`, and since a fresh source's
position is the origin, it plays at the listener — i.e. dead centre, unpanned.

**`PAT_3DSample` — positional sound effect.** Same "one source, one whole buffer" model,
but `playSample3D` (`:2970-3027`) additionally sets `AL_PITCH` from the event's pitch
shift (`:2985`), `AL_REFERENCE_DISTANCE`/`AL_MAX_DISTANCE` from the event info or the
global ranges (`:3005-3012`), `AL_ROLLOFF_FACTOR = 0.5` (`:3014`) and `AL_POSITION`
(`:3015`). Its position is re-pushed every tick from `processPlayingList`
(`:2506`).

**`PAT_Stream` — music and streamed speech.** Backed by an `OpenALAudioStream`, which owns
its own source plus a ring of 32 buffers (`AL_STREAM_BUFFER_COUNT`,
`OpenALAudioStream.h:29`) that are queued/unqueued as FFmpeg produces frames. The
`PlayingAudio::m_source` field stays 0 for these; the real source is
`playing->m_stream->getSource()`. Several bugfix comments in the tree exist purely because
someone forgot that (`:600`, `:649`, `:2532`) — and at least one site still gets it wrong
(see Gotchas).

### Which sound types use which

Decided by `AudioEventInfo::m_soundType` in the `switch` at `OpenALAudioManager.cpp:744`:

| `m_soundType` | Condition | Type | AL object |
| --- | --- | --- | --- |
| `AT_Music` | — | `PAT_Stream` | `OpenALAudioStream` |
| `AT_Streaming` (speech, EVA, taunts, briefings) | — | `PAT_Stream` | `OpenALAudioStream` |
| `AT_SoundEffect` | `event->isPositionalAudio()` true | `PAT_3DSample` | plain source |
| `AT_SoundEffect` | otherwise (UI, unit acknowledgements) | `PAT_Sample` | plain source |

`isPositionalAudio()` (`Core/GameEngine/Source/Common/Audio/AudioEventRTS.cpp:666-683`)
requires the `ST_WORLD` flag **and** an owner (drawable, object, or `OT_Positional`). A
world sound with no owner attached falls through to the 2D path.

There is no default branch in that `switch`, so an event with an unexpected
`m_soundType` silently allocates and immediately frees a `PlayingAudio` (`:1016-1018`).

---

## 3. Lifecycle of one sound, end to end

### 3.1 Submission — `AudioManager::addAudioEvent` (GameAudio.cpp:384)

1. Empty name or `"NoSound"` → `AHSV_NoSound` (`:386`).
2. `AudioEventInfo` resolved from INI by name; missing info → `AHSV_Error` (`:393-399`).
3. Category gate: music/sound/speech each check `isOn(...)` (`:406-423`). **The speech
   disallow flag is checked here** (`:418`) — see §7.
4. `shouldPlayLocally` / logical-audio filtering → `AHSV_NotForLocal` (`:433-438`).
5. The caller's event is **copied onto the heap** (`:440`). From here the device layer owns
   the copy; the caller's stack object is untouched. A playing handle is allocated
   (`:441`), `generateFilename()` picks the actual asset from the variant list (`:442`),
   `generatePlayInfo()` rolls pitch shift and volume shift once (`:444`).
6. Runtime per-event volume overrides applied (`:446-452`), then a min-volume cull
   (`:463-469` → `AHSV_Muted`).
7. Dispatch to `MusicManager` or `SoundManager` (`:471-479`).

### 3.2 Admission — `SoundManager::canPlayNow` (GameSounds.cpp:203)

Distance cull, shroud cull, per-object voice limit, `doesViolateLimit` (which also picks
the *oldest* instance as `handleToKill`, `OpenALAudioManager.cpp:1957/1970`), then free
channel count, then priority preemption. On success an `AudioRequest{AR_Play}` is queued
(`GameSounds.cpp:147-150`); on failure the heap event is released immediately (`:152`).

Channel counts come from `initSamplePools()` (`OpenALAudioManager.cpp:3077-3089`), which
copies `m_sampleCount2D` / `m_sampleCount3D` / `m_streamCount` out of the audio settings.
Note these are *bookkeeping* limits only — the OpenAL backend generates a source per
sound on demand and has no real pool.

### 3.3 Drain — `processRequestList` (OpenALAudioManager.cpp:2373)

Each tick: requests with a remaining delay are deferred and their delay decremented
(`shouldProcessRequestThisFrame` `:2690`, `adjustRequest` `:2704`); the rest go through
`checkForSample` (`:2715`, re-running `canPlayNow`) and then `processRequest` → `AR_Play`
→ `playAudioEvent`.

### 3.4 Start — `playAudioEvent` (OpenALAudioManager.cpp:727)

Common preamble: a `PlayingAudio` is allocated up front (`:743`) and `handleToKill` is
read (`:740`).

**Sample / 3D sample path** (`:882-1010`)
1. If `handleToKill` is set, scan the matching list, `releasePlayingAudio` the victim and
   erase it (`:897-912` / `:956-972`).
2. `alGenSources(1, &source)` — but **only** if there was no `handleToKill`, or the victim
   was actually found (`:915-923` / `:975-982`). Otherwise `source = 0`.
3. The `PlayingAudio` is pushed onto the list *before* playback starts (`:929` / `:989`),
   then `playSample3D` / `playSample` loads the cached buffer and plays; the sound-manager
   counter is bumped (`notifyOf3DSampleStart` `:933`, `notifyOf2DSampleStart` `:993`).
4. If no buffer came back, the entry is popped again (`:936-942` / `:996-1001`).

**Stream path** (`:746-880`)
1. Uninterruptible `AT_Streaming` first kills every other speech stream
   (`stopAllSpeech()`, `:753-755` → `:1367`).
2. `handleToKill` handling as above, on `m_playingStreams` (`:767-783`).
3. `TheFileSystem->openFile` then `FFmpegFile::open` (`:785-798`). Both failure paths
   release the `PlayingAudio` and return — see Gotchas for what leaks there.
4. A fresh `OpenALAudioStream` is created and the two lambdas are wired (`:800-849`).
5. `audio->m_type = PAT_Stream`, stream and ffmpeg file attached (`:855-858`).
6. Uninterruptible speech sets `disallowSpeech` and records the frame (`:861-864`).
7. **`adjustPlayingVolume(audio)` runs before `playStream`** (`:873-875`). The comment at
   `:865-872` explains why: the stream constructor sets gain 1.0, and the update-loop-only
   volume pass meant every track started with a blast of full-volume audio for one frame.
8. `playStream` (`:2942`) is just `stream->play()`; the `AL_LOOPING` line for music is
   commented out (`:2946`) — correct, since `AL_LOOPING` is meaningless on a
   queued-buffer source, but it means nothing else loops music either (see Gotchas).

### 3.5 Per-tick — `processPlayingList` (OpenALAudioManager.cpp:2397)

*2D samples* (`:2405-2441`): if the source reports `AL_STOPPED` and no stop was requested,
`notifyOfAudioCompletion(source, PAT_Sample)` runs the attack→sound→decay / loop
advance; if the source is *still* stopped afterwards the entry is released. Otherwise, if
`m_volumeHasChanged`, `adjustPlayingVolume`.

*3D samples* (`:2443-2522`): same, plus — while playing — the event position is re-read
(`getCurrentPositionFromEvent`, `:2853`); a dead event requests stop (`:2481`); an event
whose effective volume has dropped below `m_minVolume` is culled outright unless it is
`ST_GLOBAL` or `AP_CRITICAL` (`:2487-2500`); a null position releases the sound
(`:2515`). Surviving sounds get `alSource3f(AL_POSITION, …)` (`:2506`).

*Streams* (`:2524-2557`): volume first, then **`playing->m_stream->update()`** — the pump
that refills buffers and recovers underruns — and only then the stopped check, so a
recoverable underrun is not mistaken for the end. On a genuinely stopped stream, an
`AT_Streaming` entry clears `disallowSpeech` (`:2545-2548`) before release.

Attack/Sound/Decay advance is the one place where the port genuinely differs in mechanism
from retail: Miles used EOS callbacks, OpenAL has none, so completion is *polled* from
this function (see the bugfix note at `:2415-2417`).

### 3.6 Teardown — `releasePlayingAudio` (OpenALAudioManager.cpp:1219)

Decrements the sound-manager 2D/3D counters for sound effects (`:1223-1234`), then
`releaseOpenALHandles` (`:1197`) deletes the AL source, the `OpenALAudioStream` and the
`FFmpegFile`; then `closeBuffer` drops the cache refcount; then the heap `AudioEventRTS`
is freed unless `m_cleanupAudioEventRTS` was cleared (which `startNextLoop` does when it
re-queues the event as a delayed request, `:2919`); finally the `PlayingAudio` itself is
deleted.

---

## 4. The sample cache (`OpenALAudioFileCache`)

`getBufferForFile` (`OpenALAudioCache.cpp:86`) is the only entry point that matters. It
keys on the *filename for the current play portion* — attack, sound or decay
(`:93-107`) — so a three-part sound occupies three cache entries.

On a miss it opens the file, hands ownership to a new `FFmpegFile`, and calls
`decodeFFmpeg` (`:22-67`) which decodes **the entire file** into one `std::vector<uint8_t>`
(interleaving planar output as it goes, `:34-47`) and uploads it as a single `alBufferData`.
Duration in ms is derived from the sample count (`:64`) and is what `getFileLengthMS`
returns. Hits just bump `m_openCount` (`:124`).

Eviction (`freeEnoughSpaceForSample`, `:245`) first drops entries with refcount 0, then —
if still short — entries with a **lower `m_priority` that are still playing**, forcibly
stopping them via `TheAudio->closeAnySamplesUsingFile()` from `releaseOpenAudioFile`
(`:226` → `OpenALAudioManager.cpp:2802`). That call is what prevents a live source from
referencing a deleted AL buffer, and it is why `closeAnySamplesUsingFile` walks the
sample lists but not the stream list (streams never use cache buffers).

Two things about the cache are worth knowing before you touch it:

- `setMaxSize()` is a **no-op** (`:212-219`). The budget is hardcoded to 14 MiB in the
  constructor (`:18`) because the shipped INI value of 4 MB caused thrashing.
- The size accounting is in **compressed on-disk bytes**, not decoded PCM bytes.
  `decodeFFmpeg` accumulates decoded sizes into `m_fileSize` (`:48`), and then
  `getBufferForFile` overwrites it with `file->size()` (`:135`, `:163`). Real GPU/driver-side
  memory use is therefore several times the number the eviction logic thinks it is
  managing.

---

## 5. How FFmpeg feeds a stream, and the refill/underrun/EOF logic

Two lambdas, both created in `playAudioEvent`:

```c
// OpenALAudioManager.cpp:804 — "I need more data"
stream->setRequireDataCallback([ffmpegFile, stream]() -> bool {
    ffmpegFile->decodePacket();
    return !ffmpegFile->isAtEof();      // FALSE == true, permanent EOF
});

// OpenALAudioManager.cpp:814 — "here is a decoded frame"
ffmpegFile->setFrameCallback([stream](AVFrame* frame, …) {
    …interleave if planar…
    stream->bufferData(frameData, frameSize, format, frame->sample_rate);
});
```

So a refill is: stream asks → `decodePacket` reads one packet and pushes every frame it
yields straight through `bufferData` → each frame becomes one `alBufferData` +
`alSourceQueueBuffers` on the next slot of the 32-buffer ring
(`OpenALAudioStream.cpp:145-203`). `bufferData` refuses to queue when 32 are already
outstanding (`:138-143`) and validates both AL calls (`:147-156`, `:191-196`).

The return-value contract is the key design decision: **`decodePacket()` returning false
on a decode error is deliberately *not* treated as end-of-stream.** The callback keys on
`FFmpegFile::isAtEof()`, which is set only when `av_read_frame` returns `AVERROR_EOF`
(`FFmpegFile.cpp:211-217`). A transient decode failure therefore returns TRUE and the
normal underrun recovery runs; only a real EOF latches `m_endOfData`.

### `OpenALAudioStream::update()` — the state machine (`OpenALAudioStream.cpp:206-369`)

Read it in the order it executes; the ordering is load-bearing and every reorder in its
history caused an audible bug.

1. **EOF probe** (`:214-249`). If the source is `AL_STOPPED` with `processed >= queued`
   (everything queued has been played) and EOF is not yet latched, probe the callback once.
   - callback returns false → `m_endOfData = true`.
   - callback returns true but the queue did not grow → increment `m_stalledProbes`;
     **three consecutive** no-growth probes also latch EOF (`:241-243`). This exists
     because decoding is synchronous — a permanently failing packet never heals, and
     without the latch the restart guard below replayed the queue forever (the "chirping"
     loop after a voice line, which also pinned `disallowSpeech`).
   - a successful growth resets the counter (`:246`, and again at `:341` — the comment
     there is explicit that stalls must be *consecutive*, not cumulative).
2. **Restart-on-stopped, before unqueue** (`:255-272`). Restarts a stopped/initial/paused
   source that still has queued buffers — but **only if `processed < queued`**. Without
   that guard a drained source (`queued=16 processed=16`) got replayed from the start,
   which is the "burst of noise" traced on macOS (`:257-262`). Skipped entirely once
   `m_endOfData` is set, so a finished one-shot can reach a stable `AL_STOPPED`.
3. **Unqueue processed buffers** (`:274-301`). Normally only while playing/paused
   (`:279`), plus a special case: a stopped source whose queue is *entirely* spent is also
   reaped (`:289-295`), because step 2 deliberately no longer restarts it and its buffers
   would otherwise accumulate (observed climbing `queued=13,14,15…`).
4. **EOF drain** (`:303-315`). At true EOF, unqueue everything remaining so `queued`
   reaches 0 and `processPlayingList` can see the stream as stopped *the same frame the
   audio ends* — which is what makes back-to-back taunts work.
5. **Refill** (`:320-343`). If fewer than `AL_STREAM_BUFFER_COUNT / 2` (= 16) buffers are
   queued and EOF is not latched, loop asking for data until the half-mark. The loop
   breaks if the callback reports EOF, and also breaks if the queue failed to grow
   (`:336-339`) — that guard exists so a callback that enqueues nothing cannot spin.
   The half-mark rather than the full 32 is deliberate: one decoded frame can produce more
   than one buffer.
6. **Restart after refill** (`:345-368`). Same guard as step 2, but the queue depth is
   **re-read** here (`:358-361`); comparing the pre-refill `num_queued` against a fresh
   processed count made a just-refilled source look "all spent" and the new audio never
   played.

`reset()` (`:371-390`) stops, drains, rewinds the ring index and clears both EOF latches —
streams are reused across `handleToKill`/replace, so forgetting the clear would wedge them.

### Diagnostics

Streams register themselves in a live-set and record transitions into a 192-entry ring
buffer (`OpenALAudioStream.cpp:13-98`). `AudioDebugDump()` prints the recent history plus
every live stream's `state/queued/processed/atEof`. Triggers: **F9**
(`GeneralsMD/Code/GameEngineDevice/Source/SDL3GameEngine.cpp:948`, handled outside the
keybinding layer so it works on menus) and a **four-finger tap** on iOS (`:346`). Set
`GX_AUDIO_TRACE=1` to mirror every event to stderr live (`OpenALAudioStream.cpp:57`).
`bufferData` computes peak amplitude per buffer and flags `LOUD`/`CLIPPING`
(`:163-188`) — that is the discriminator between "stale audio replayed" and "decoder
handed us garbage".

---

## 6. Volume and 3D positioning — what is applied, and when

### Two different volume formulas

`adjustPlayingVolume` (`:1340-1364`) is what actually writes `AL_GAIN`:

| type | gain written |
| --- | --- |
| `PAT_Sample` | `m_soundVolume * volume * volumeShift` |
| `PAT_3DSample` | `m_sound3DVolume * volume * volumeShift` |
| `PAT_Stream`, music | `m_musicVolume * volume * volumeShift` |
| `PAT_Stream`, other | `m_speechVolume * volume * volumeShift` |

`getEffectiveVolume` (`:2871-2900`) computes the *same* number by a different route
(keyed on `m_soundType` first, falling back to positional-or-not) and is used only for
decisions and display: the 3D min-volume cull (`:2487`), fading (`:2644`) and the debug
overlay. Keep them in sync if you change one.

The category volumes themselves are `script * system` products maintained by
`AudioManager::setVolume` (`GameAudio.cpp:701-742`), which sets `m_volumeHasChanged`.

### When gain reaches the device

- **Streams: before the first sample is heard** — `adjustPlayingVolume` at
  `OpenALAudioManager.cpp:873`, immediately before `playStream`.
- **Samples: never at start.** Neither `playSample` (`:2956`) nor `playSample3D`
  (`:2970`) touches `AL_GAIN`, so a freshly generated source runs at the OpenAL default
  of 1.0 until some later `processPlayingList` pass with `m_volumeHasChanged` set.

In practice `m_volumeHasChanged` is set most frames, because `AudioManager::update()`
recomputes the zoom boost and calls `set3DVolumeAdjustment` unconditionally
(`GameAudio.cpp:364`), which sets the flag (`:772`). But that line is itself conditional:

```c
if ( ! has3DSensitiveStreamsPlaying() )
    m_volumeHasChanged = TRUE;          // GameAudio.cpp:771-772
```

`has3DSensitiveStreamsPlaying()` (`OpenALAudioManager.cpp:2587`) returns true whenever any
non-music stream is playing (i.e. any speech line), or any music track whose event name
does not start with `Game_`. This is a deliberate retail-era workaround for a Miles hang
(`:2577-2585`). Consequence on this backend: see Gotchas.

### 3D

- Distance model `AL_INVERSE_DISTANCE_CLAMPED`, set once in `init()` (`:517`).
- The listener is pushed every tick from `setDeviceListenerPosition` (`:2844-2850`), using
  the "microphone" position the base `AudioManager::update()` derives from the tactical
  camera (`GameAudio.cpp:285-332`) — not the camera itself. The orientation's *up* vector
  is hardcoded to `(0,0,1)` (`:2846`). Listener velocity is never set, so no Doppler.
- Per-source ranges come from `m_minDistance`/`m_maxDistance`, or the global ranges for
  `ST_GLOBAL` events (`:3005-3012`).
- **Multichannel positional assets fall back to non-spatial playback** rather than being
  dropped: `playSample3D` checks `AL_CHANNELS` and, for >1, forces
  `AL_SOURCE_RELATIVE`, origin position, zero rolloff and (where available)
  `AL_DIRECT_CHANNELS_SOFT` / `AL_SOURCE_SPATIALIZE_SOFT = FALSE` (`:2987-3001`). The
  cache logs the same situation at load time (`OpenALAudioCache.cpp:148-154`). OpenAL
  simply does not spatialize stereo buffers; this is the port's chosen compromise.
- `OpenALAudioStream`'s constructor applies the same non-spatial treatment to every stream
  source (`OpenALAudioStream.cpp:106-118`), so music and speech are always direct.

---

## 7. The speech disallow flag

The flag lives on the base class (`GameAudio.h:382`, accessors at `:300-301`). Its only
consumer is the gate in `AudioManager::addAudioEvent`:

```c
case AT_Streaming:
    // if we're currently playing uninterruptable speech, then disallow the addition of this sample
    if (getDisallowSpeech())
        return AHSV_NoSound;             // GameAudio.cpp:416-419
```

**Set** in exactly one place: `playAudioEvent`, when an `AT_Streaming` event is marked
uninterruptible *and* a stream object was actually created (`:861-864`). Note the
asymmetry — `stopAllSpeech()` runs earlier and unconditionally for such an event
(`:753-755`), so an uninterruptible line always silences existing speech even in the
`stream == NULL` path where the flag is never set.

**Cleared** in four places:
1. `processPlayingList`, when the stream's source is observed stopped (`:2545-2548`) —
   the intended path.
2. `notifyOfAudioCompletion`, for any `AT_Streaming` completion (`:1655-1657`).
3. `OpenALAudioManager::reset()`, via `AudioManager::reset()` (`:534`).
4. A **frame-count backstop** (`:2559-2570`): if the flag has been set for more than
   `DISALLOW_SPEECH_MAX_FRAMES` (30 fps × 15 s, `:83`) it is force-cleared. A negative
   delta — the logic frame counter reset under us on a new game/map — also clears.

The backstop exists because the original failure mode was catastrophic-but-silent: a
finished taunt whose drained source kept being restarted never reached `AL_STOPPED`, so
the flag stuck and *every subsequent speech event in the match* returned `AHSV_NoSound`
(`:71-80`). The real fix is the EOF propagation in `OpenALAudioStream`; the timeout stays
as belt-and-braces for the iOS/OpenAL stack. If you rework the stream state machine, the
regression to watch for is "only the first taunt plays".

---

## 8. Gotchas

These are ordered roughly by how likely they are to bite you.

**1. `PlayingAudio::m_source` is 0 for streams, and not every call site remembers.**
`adjustVolumeOfPlayingAudio` writes stream gain to `alSourcef(playing->m_source, …)`
(`:2268`) instead of `playing->m_stream->getSource()`. With source 0 that is an AL error
and a silent no-op: runtime volume overrides never reach music or speech.

**2. `findPlayingAudioFrom(source, PAT_Stream)` cannot identify a stream.** It matches on
`playing->m_source` (`:1733-1740`), which is 0 for every stream. `stopAudioEvent` calls
`notifyOfAudioCompletion((UnsignedInt)audio->m_source, PAT_Stream)` (`:1065`) — passing 0
— so the lookup returns the **first** stream in `m_playingStreams`, not the one being
stopped. If music happens to be first and you stop a speech stream, the music branch of
`notifyOfAudioCompletion` (`:1698-1706`) calls `playStream` on the music stream instead.
Anything you add that routes stream completion through this function needs a different
key (the stream pointer, or the handle).

**3. Samples get their volume one frame late — or not at all during speech.** Per §6, no
`AL_GAIN` is set when a sample starts, so its first tick plays at 1.0. This is exactly the
bug that was found and fixed for streams (`:865-872`), and it was never fixed for samples.
Worse: while any speech stream is playing, `has3DSensitiveStreamsPlaying()` suppresses the
per-frame `m_volumeHasChanged` (`GameAudio.cpp:771`), so a UI click or unit ack fired
during an EVA line can stay at full gain for the whole line unless the player happens to
move a volume slider. If you are chasing "some sounds are too loud", start here.

**4. `pauseAudio` uses `alSourceStop`, not `alSourcePause`, for samples.**
(`:619`, `:628`.) Combined with `processPlayingList` reaping stopped samples (`:2413`),
pausing effectively destroys in-flight sound effects; `resumeAudio`'s `alSourcePlay`
(`:685`, `:694`) then has nothing to resume, or restarts a sample from its beginning.
Streams do use `pause()`/`play()` correctly (`:651`, `:714`). Also note `pauseAudioEvent`
(`:1170-1173`) is an empty stub, so `AR_Pause` requests do nothing at all.

**5. Stopping a sample does not stop it.** `stopAudioEvent` only sets `m_requestStop` on
2D/3D entries (`:1077`, `:1092`) — no `alSourceStop`. The flag's sole effect is to
suppress the attack→sound→decay advance when the sound finishes naturally (`:2418`,
`:2456`) and to end looping (`:2908`). Immediate silence requires
`killAudioEventImmediately` (`:1099`) or `removePlayingAudio` (`:2275`).

**6. Music does not loop, and script "track completed" conditions never fire.**
`playStream`'s `AL_LOOPING` line is commented out (`:2946`) — correct for a queued-buffer
source, but nothing replaces it: at EOF the stream is released by `processPlayingList` and
that is that. `hasMusicTrackCompleted` (`:1510-1528`) has its entire body commented out
and unconditionally returns `FALSE`, so the `ScriptConditions.cpp:2687` music condition is
dead. The `INFINITE_LOOP_COUNT` enum at `:95` is a vestige of the Miles loop counter.

**7. `checkForSample` tests the wrong field** — `getAudioEventInfo()->m_type !=
AT_SoundEffect` (`:2727`). `m_type` is the `ST_*` **bitmask** (`AudioEventInfo.h:106`,
`:61-71`); `m_soundType` is the `AudioType` enum (`:121`). Since `AT_SoundEffect == 2 ==
ST_WORLD`, the re-check only runs for events whose flag mask is exactly `ST_WORLD`.
**This is faithful to retail** — the Miles backend has the identical line
(`Core/GameEngineDevice/Source/MilesAudioDevice/MilesAudioManager.cpp:2404`) — so
"fixing" it changes gameplay-visible behaviour. Leave it unless you are deliberately
diverging.

**8. Leaks on the stream open-failure paths.** In `playAudioEvent`, if
`ffmpegFile->open(file)` fails (`:793-798`), the `FFmpegFile` has already been allocated
but is not yet attached to `audio`, and `releasePlayingAudio(audio)` runs before
`audio->m_audioEventRTS` was assigned (that happens at `:855`) — so both the `FFmpegFile`
and the heap `AudioEventRTS` leak. Same for the earlier `openFile` failure (`:786-790`),
which leaks the event. Low frequency (missing/corrupt asset) but real.

**9. `FFmpegFile::decodePacket` only handles `AVERROR_EOF` from `av_read_frame`.** Any
other negative return falls through and dereferences `m_packet->stream_index`
(`FFmpegFile.cpp:210-220`) on a packet that was not populated by this call. Also, the
codec is never flushed at EOF (no `avcodec_send_packet(ctx, NULL)`), so frames still
buffered inside the decoder at end-of-file are dropped — a few tens of ms off the tail of
every streamed line.

**10. Cache budget is measured in the wrong units and cannot be configured.** See §4.
`getFileLengthMS` (`:2785-2799`) also *fully decodes and caches* a file just to read its
duration, and only drops the refcount afterwards — so probing lengths pollutes the cache.

**11. The provider/speaker layer is vestigial.** `selectProvider` hardcodes
`Bool success = FALSE` (`:1849`) and always falls back to the single fake provider
`"Miles Fast 2D Positional Audio"` registered in the constructor (`:123-124`);
`createListener` and `initDelayFilter` are empty (`:3060`, `:3066`). Do not read anything
into provider indices. Because `initSamplePools` is reached only through `selectProvider`,
and both bail when `AudioAffect_Sound3D` is off (`:1777`, `:3079`), disabling 3D sound
leaves `m_num2DSamples == 0` and starves the 2D path too — consistent with the gate at
`GameAudio.cpp:413`, but surprising if you are toggling things at runtime.

**12. `m_stoppedAudio` and `processStoppedList` are dead weight.** Every producer is
commented out (§1). If you add a deferred-release path, resurrect it deliberately rather
than assuming it already works.

**13. Everything runs on the game thread.** No locks are taken anywhere in this backend
despite `OpenALAudioFileCache`'s header comments referring to mutex protection
(`OpenALAudioCache.h:99-109`) — there is no mutex in the class. The comments are stale
from the Miles version. Do not introduce a decode thread without auditing the cache,
`m_openFiles`, and the three playing lists.

---

## 9. Things this document is not sure about

- **Fading.** `processFadingList` (`:2624`) only ever receives entries from the
  `AHSV_StopTheMusicFade` path (`:1043`). Whether ambient/other fades were meant to route
  here is unclear; `pauseAmbient` (`:721`) is an empty stub, so ambient pausing is simply
  unimplemented. Start at `AudioSettings::m_fadeAudioFrames` and its INI definition.
- **Video/Bink audio.** `getHandleForBink` (`:3115`) hands the `FFmpegVideoPlayer` a bare
  `OpenALAudioStream` with **no** `requireDataCallback`; the video player pushes buffers
  itself and calls `update()` after every frame
  (`Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegVideoPlayer.cpp:440-441`). That
  means the EOF probe and the refill loop are both inert for video audio, and the only
  live logic is the restart guards. Whether video A/V sync depends on that is not
  established here — read `FFmpegVideoStream::update` (`:451-462` in the same file) next;
  it deliberately calls `play()` only when the source is not already playing, because
  `alSourcePlay` on a playing source rewinds to the first unprocessed buffer.
- **iOS specifics.** Nothing in this backend is `#ifdef`-ed for iOS; the only
  iOS-conditional audio code found is the four-finger diagnostic trigger. Whether
  interruption handling (phone call, backgrounding) is wired anywhere was not located —
  if you hit "audio dies after backgrounding", that gap is the first suspect.
