# Session 5 dropoff — the open question is ANSWERED: build the relay

Read with `DESIGN_headless_and_relay.md` and `STATE_2026-07-28_session4.md`.

## THE ANSWER

**macOS arm64 and Linux x86-64 simulate IDENTICALLY. 800 frames, byte for byte, zero differing.**

So the frame-62 divergence seen over the internet is **transport, not simulation**. The relay is the
fix, and it is now safe to build one that arbitrates CRCs — the platforms genuinely agree.

The decisive run is the `-lanseed` experiment session 4 built: two SOLO headless runs, same pinned
seed, **no networking at all**, so nothing about it can be explained by NAT or packet loss.

```bash
# Mac
cd ~/GeneralsX/GeneralsZH && ./run.sh -headless -lanhost solo \
  -lanmap 'maps\twilight flame\twilight flame.map' -lanai Hx1 -lanseed 424242 -lanframes 800
# Linux
ssh root@163.5.210.131 'bash /root/solohost.sh 2>/root/solo3.err'
```

Committed evidence — `docs/WORKDIR/evidence/lanseed-solo-800-{mac,linux}.crc` plus `.meta.txt`:

| check | result |
|---|---|
| Mac vs Linux, 800 frames | **identical** |
| distinct CRC values (control: the sim was ACTIVE, not idling) | 800 of 800 |
| `aiPlayers=1 computers=3 sides=5 sidesWithScripts=3` on both | identical — the AI path was exercised |
| `newMap EXIT seed=3044963377 delayDraws=80 noDelay=327` on both | identical — same RNG draw count |
| build digests `engine`/`source`/`data`/`ordinal`/`parse`/`asset` | identical; only `platform` differs, by design |

The `.meta.txt` files are committed **with** the CRC streams on purpose: an AI-free run would pass
this test while never touching the AI code path, so the slot list is what makes the result mean
anything. Same reason session 4 committed its slot list.

## The blocker: an unbounded OpenAL error drain. FIXED (`aa776b7eb`)

Session 4 left the Linux half hanging after `[GXLAN] game starting` with zero `[GXCRC]` lines, and
guessed it was in the match loop. **It was not** — the process never reached the match loop.

`OpenALAudioStream::bufferData` drained the AL error queue with:

```cpp
while (alGetError() != AL_NO_ERROR) {}
```

With **no current context** OpenAL Soft returns `AL_INVALID_OPERATION` from *every* call — a cached
`deferror` constant — and logs a `WARN` each time. So the loop can never exit. The process spins at
100% CPU inside the formatter for that warning.

Reached from `GameLogic::tryStartNewGame` → `MultiPlayerLoadScreen::init` →
`OpenALAudioManager::update` → … → `bufferData`, i.e. **while loading the map, before frame 0.**

**Not a Linux quirk and not headless-only.** Any context that is lost or never created freezes the
whole process the same way — including an output device disconnected mid-game on a player's machine.
Fixed by bounding the drain; a state that will not clear means there is no usable context, so the
buffer is dropped and the stream degrades to silence. The caller already handled a `false` return
(`OpenALAudioStream::update`'s refill loop breaks on its `refreshedQueued <= num_queued` guard).

Verification, all fresh:

* pre-fix: **2 of 2 hung** (session 4's `soloLinux.err`, and reproduced this session — 300 s
  timeout, 0 CRC lines, process still alive, stopped at the byte-identical point).
* post-fix: **4 of 4 clean**, 31–33 s each, 800 CRC lines, exit 0, no orphan process.
* behaviour-neutral: Mac rebuilt with the fix produces a CRC stream **identical** to the pre-fix Mac
  binary's. Same for Linux. The fix changes no simulation state.

### Why it looked intermittent

It needs BOTH an audio stream being fed AND no current context. Whether OpenAL gets a context on a
headless box depends on the session environment: **the same command hung under a login shell over
SSH and ran clean when detached with `setsid`.** That is why it looked like a flaky network problem.

### Related gap, NOT fixed (deliberately out of scope)

`SDL3GameEngine::createAudioManager(Bool dummy)` does `(void)dummy;` and always returns a real
`OpenALAudioManager` — so **headless on Mac/Linux runs the full audio backend.** Win32 honours the
flag and returns `MilesAudioManagerDummy`. There is no OpenAL dummy class to return yet, so this is
a real change rather than a one-liner. The drain fix makes it non-fatal either way.

## How it was found — the method, not the luck

`State: R` in `/proc/<pid>/status` said it was **spinning, not blocked on I/O**, which killed the
"stderr pipe backpressure" theory immediately. Then three `gdb -p <pid> -batch -ex "thread apply all
bt"` samples all landed on the same line. Backtrace committed at
`docs/WORKDIR/evidence/openal-drain-hang-backtrace.txt`.

**Instrumenting the match loop — the session 4 plan — would have found nothing**, because the hang
was upstream of it. `gdb` was not installed on the VPS; installing it cost one command and replaced
a rebuild-and-guess cycle. **Attach to the live process before adding printfs.**

## Rules this session added

1. **`ps -eo comm | grep -cx GeneralsXZH`, never `pkill -f` / `pgrep -f`.** `pkill -f GeneralsXZH`
   inside an `ssh root@host '...GeneralsXZH...'` one-liner matches the **remote shell running the
   command**, kills it, and the ssh returns 255 — twice today. Session 4 recorded this trap for
   `pgrep`; it bites identically for `pkill` over SSH. Use `pkill -x <name>`.
2. **`a && b && c || echo OK` prints OK when `a` fails.** A verification chained that way reported
   "OK: unbounded loop no longer present" after `git am` had failed and the grep never ran. Same
   family as the `grep -c` trap. Give each check its own exit-code test.
3. **A hang that stops at a byte-identical point is deterministic, not flaky.** Comparing the hung
   log against a working one from the same box localised it to a two-call window before any
   debugger was attached. Diff a failing log against a passing one FIRST.
4. **When a run mysteriously works, suspect the invocation, not the fix.** The first clean Linux run
   this session proved nothing — it differed from the hanging one only in being detached. Reproduce
   the ORIGINAL command before concluding anything.

## THE NEXT TASK: build the relay

Nothing is blocking it now. `DESIGN_headless_and_relay.md` has the plan; the short version is that
`RequestGameJoinDirectConnect` is already the off-LAN primitive, the relay and the anticheat server
are the same process, and sizing is 30-40% of one **dedicated** (never burstable) core per heavy
match. The in-game transport is the thing that does not traverse NAT — gameplay uses 8088 with peers
addressing each other from the slot list, where a peer listed by public IP has no 8088 mapping. That
is what the relay replaces.

## Still open (unchanged from session 4 unless noted)

* `Generals/` (base game) has no headless CLI.
* Windows exits `0xC0000005` at the END of a headless run, after all frames and CRCs are written.
* `RTS_BUILD_OPTION_FFMPEG=OFF` cannot work with `SAGE_USE_OPENAL=ON` (`CMakeLists.txt:305`).
* The committed replay fixture no longer loads against the current engine.
* iPad still not reinstalled.
* **NEW:** headless still builds the real OpenAL backend on Mac/Linux — see the gap above.

## Waiting on Karl, not on the next session

* **Rotate the VPS root password in the panel.** Inert for SSH (key-only, verified), but it is still
  the console password and it was pasted in chat.
* **Raise the RAM ticket.** 3.75 GB of a sold 8 GB survived a clean reinstall, `virtio_balloon`
  bound. Memory reclaimed mid-match stalls the sim for every player.
* **`aa776b7eb` is committed locally but NOT pushed** — pushes need Karl's say-so. The VPS has it
  applied as `9dd50fff4` via `git am`; once the real commit is pushed, the VPS should go back to a
  pure mirror with `git fetch && git reset --hard origin/main`.
