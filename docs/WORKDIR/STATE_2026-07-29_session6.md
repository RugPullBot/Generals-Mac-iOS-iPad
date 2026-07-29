# Session 6 dropoff — online play works, and a human actually played it

Read with `STATE_2026-07-28_session5.md` (which this supersedes) and `DESIGN_headless_and_relay.md`.
HEAD at the end of this session is `508d6c563` (rev 2162).

## THE HEADLINE

**A real match, played by a person, across three platforms, over the internet, through our relay.
14,756 frames. Byte-identical CRC streams. Zero mismatches.**

macOS arm64 and Windows x86-64 joined a headless Linux x86-64 host in one relay room and played to a
finish — about eight minutes of someone issuing orders, not an AI idling.

| check | result |
|---|---|
| `[GXCRC]` lines, macOS vs Linux | 14,757 each, **identical**, md5 `c1af5af4b5142f2e84a8a0961051dbc6` |
| distinct CRC values | **14,757 of 14,757** — every frame did different work |
| in-game peer CRC vote | zero mismatches for the whole match (`d87460c65`); no mismatch line in the host log |
| relay counters | `headerless 0`, `unrouted 0` — every peer on the current protocol, every packet delivered to a real peer |
| traffic | 3,512 packets / 135,749 bytes on 8088 across the match, ~39 bytes each |

Evidence: `docs/WORKDIR/evidence/relay-played-match-14756-{linux,macos}.crc`,
`relay-played-match-14756-linux.meta.txt`, `relay-played-match-14756-relay.txt`.

**Two honest caveats on this run, both load-bearing:**

1. **Windows was in the match but its CRC stream was not committed.** The identity `10.42.0.4`
   registered from the same public IP as the macOS peer and the relay logged three peers on 8088,
   so Windows was there; the byte-for-byte comparison is macOS vs Linux only. Windows-vs-Linux is
   proven separately at 600 frames (below).
2. **It ran on rev 2157 binaries** (`15d18ed30`) — `SIMID source=203DE39E` on both metas. Everything
   from `973aed2d4` onward, including **both halves of relay-assigned identities**, landed *after*
   this match. The three virtual addresses in it were allocated **by hand**. See "built but
   unproven".

## What is PROVEN, with numbers

| run | peers | frames | evidence | rev |
|---|---|---|---|---|
| solo determinism, no networking | macOS, Linux | 800 | `lanseed-solo-800-{mac,linux}.crc` + `.meta.txt` | s5 |
| first full relay match over the internet | macOS + Linux | 1200 | `relay-inet-mac-vs-linux-1200-{mac,linux}.crc`, `relay-inet-registrations.txt` | 2150 |
| first >2-peer room | Linux host + Linux + macOS | 900 | `relay-3peer-900-{linux-host,linux-peer,macos}.crc`, `relay-3peer-registrations.txt` | 2148 |
| Windows on the relay | Windows + Linux | 600 | `relay-win-vs-linux-600-{windows,linux}.crc` + `.meta.txt` | 2157 |
| **human-played match** | macOS + Windows + Linux host | **14,756** | `relay-played-match-14756-*` | 2157 |
| relay game list, end to end | macOS browsing a live Linux game | — | `relay-gamelist-browse.txt` | 2156 |

All CRC md5s re-verified this session against the committed files; all distinct-value counts equal
the line counts (900/900, 600/600, 1200/1200, 14757/14757), so no run "passed" by idling. `aiPlayers=1`
and a `CH` slot in the serialised slot list on every one, so the AI path was exercised rather than
skipped — an AI-free run passes this test while never touching the code that used to break.

**The relay matrix now proven:** macOS↔Linux (1200, and 900 three-way), Windows↔Linux (600),
macOS+Windows+Linux together (14,756, macOS/Linux compared). Not proven: any run involving iPadOS
or iOS, and any run with more than 3 peers.

### The five fixes that made it work

* **`aa776b7eb` — the OpenAL error drain was an unbounded loop.** `while (alGetError() != AL_NO_ERROR) {}`
  never terminates with no current context: OpenAL Soft returns `AL_INVALID_OPERATION` from every
  call and logs a warning each time, so the process spins at 100% CPU in the log formatter. Reached
  from `MultiPlayerLoadScreen::init` while loading the map, i.e. **before frame 0**. Not a Linux
  quirk and not headless-only — an output device disconnected mid-game freezes a player's process
  identically. Backtrace at `evidence/openal-drain-hang-backtrace.txt`.
* **`3c2d2a7fa` — headless picked the wrong identity in relay mode.** `HeadlessMatch::setUpLan` elected
  an interface address; in relay mode the lobby is a virtual LAN whose members are on no interface,
  so a headless peer advertised its NAT-side address and the other peer spent the match dialling
  something unroutable. `NetworkDirectConnectInit` had always done this correctly.
* **`99846529d` — routing by peer.** `MEMBERS_PER_ROOM` was 2 **and a third registration evicted the
  quietest member**, so player 3 silently kicked player 1 out of a live game. The real cap was on the
  client: every relayed datagram was reported as coming from one configured `PeerVirtualIP`, and
  nothing told the relay who a packet was *for*, so it could only broadcast. Both fixed by a 12-byte
  header — `[ 'GXR1' ][ src ][ dst ][ untouched game packet ]` — written in `Transport` outside the
  CRC and the XOR, big-endian, taken **out of** the 1100-byte payload budget rather than added on
  top (mobile MTU: a fragmented lockstep packet would present as a desync that only reproduces on
  cellular). Also fixed a two-byte receive overrun that the shrink exposed.
* **`c353d8f65` — two joiners deadlocked.** `LANAPI::OnPlayerJoin` calls `resetAccepted()` on *every*
  join, so a headless peer that accepted once was silently un-accepted when the next peer arrived. A
  human sees the tick vanish and clicks again; headless did not. The host counted one accepted peer
  and waited out its whole timeout while both joiners believed they were ready. Fixed by re-asserting
  off our own slot state (joiners never receive `OnPlayerJoin` — confirmed, zero occurrences).
* **`15d18ed30` — Windows could not use the relay at all.** `initRelay` refused relay mode for any
  multi-instance client. That guard is correct on macOS/Linux (per-instance `127.x` bind collides
  with the wildcard bind relay mode needs) but the code applying it is itself `#ifndef _WIN32`, and
  the Windows dev build sets `RTS_DEBUG_MULTI_INSTANCE` deliberately. So every Windows dev build lost
  relay mode **silently**: the headless log claimed relay mode was on (it reads `Options.ini`
  directly) while the transport had turned it off, and the game died at
  `UDP::Bind failed (status 10049) for IP 10.42.0.4` — WSAEADDRNOTAVAIL on an address that is on no
  interface. The bind log naming the address rather than `0.0.0.0` is what separated the two.

### Matchmaking, built this session

* **`9e6d1e5dd` / `f44e305d2` — a relay-served game list.** `GXADV <room> <hostIP> <players> <slots>
  <name>|<map>` from the host every 5 s, `GXLIST` from a browser, one `GXGAME` datagram per game back.
  Plaintext, same shape as `GXRLY`, recognised by tag alone — the relay still never reads a game
  payload. **No teardown message on purpose:** a host that crashes or loses its link cannot send one,
  and that is exactly the case that has to work, so a room drops off the next sweep instead. A room
  that never advertises is private and never listed. One datagram per game, so a lost packet costs
  one game rather than the list. Host-side hook is `LANAPI::RequestGameOptions` — the one place both
  the UI host and the headless host already pass through on every slot-list change, so the advertised
  player count tracks the lobby with no second timer. The advertisement rides the registration
  keepalive so the list entry and the NAT mapping expire together; otherwise the browser offers games
  nobody can reach.
* **`873f12cf9` — advertise the host's name.** `RequestGameCreate` builds `"<hexIP><hexSeed>" + requested
  name` and truncates to 16 — exactly the length of the hex prefix — so the requested part is
  **always** cut off. Confirmed in the relay log: a game hosted as "eightman" listed as
  `0A2A000101249D48`. It is a uniqueness token, never previously shown to anyone.
* **`25e3212f0` — the browser, on the Direct Connect screen.** Rows read
  `10.42.0.1 (Ubuntu-2404  2/8  twilight flame)` and stay dialable because `JoinDirectConnectGame`
  already splits on `(` before parsing an address. Three details that each would have shipped a
  quietly broken screen: the transport is pumped from `NetworkDirectConnectUpdate` (nothing else
  pumps it there, and without it the replies sit in the socket); the list is refilled only when the
  game count changes, so a dropdown does not rebuild under the player's cursor; our own game is
  skipped.
* **`973aed2d4` / `d87460c65` / `6a2b7482e` — relay-assigned identities.** `GXWHO <room>` →
  `GXYOU <room> <virtualIP>` (or `-` when full). `LocalVirtualIP` was hand-written per machine and had
  to be unique by human agreement, which cannot survive public matchmaking — everyone installs the
  game and everyone is `10.42.0.1` — and it does not fail cleanly: two clients sharing an identity
  look to the relay like **one** client whose NAT keeps rebinding, so the lobby corrupts instead of
  refusing. Assignment is sticky per endpoint (the request is UDP and will be retransmitted; a fresh
  address per retry would burn the room's space and change our identity after we announced it) and
  excludes addresses already registered, so hand-configured peers cannot be shadowed. The client asks
  **before any transport or lobby object exists** — `LANGameInfo` snapshots the local IP at
  construction and `AmIHost` and slot matching read that snapshot.
* **`508d6c563` — Online opens the relay lobby.** The button called `StartPatchCheck()`, which begins
  patch-check → login → peerchat against `gamestats.gamespy.com` / `peerchat.gamespy.com`. GameSpy shut
  down in 2014, so "CANNOT CONNECT — please check your internet connection" is the *correct* answer
  and blames the player for a service that does not exist. Now pushes
  `Menus/NetworkDirectConnect.wnd`. This is the pragmatic half: the real Online screens are wired
  through the GameSpy peer and staging-room objects and re-pointing them is a far larger job.

## What is BUILT but UNPROVEN

Nothing here should be read as working.

1. **Relay-assigned identities have never been in a match.** Server side (`973aed2d4`), client side
   (`d87460c65`), and the GUI path (`6a2b7482e`) are all in. Coverage is `tools/relay/test-lobby.js`
   only — 11/11, including sticky reassignment and two clients getting different addresses. The
   14,756-frame match ran on rev 2157, four commits earlier, with addresses allocated by hand. **The
   first thing next session should do is play one match with no `LocalVirtualIP` configured anywhere.**
2. **The Online button has never been clicked in a built binary.** `508d6c563` is 13 lines and was
   committed at 03:15; no run since.
3. **The UI game browser has no committed evidence.** `relay-gamelist-browse.txt` is the *headless*
   `-lanlist` path. The relay logged `browsed 4` and `browsed 1` in the windows around the played
   match, so something did query `GXLIST` — suggestive, not proof that the dropdown rendered the list.
4. **`Transport::setRelayRoom` has zero call sites.** Implemented, commented as "replaced later by
   `setRelayRoom` when a game is picked out of the browser", and **nothing calls it**. `RelayGameListing`
   carries a `room` field that is parsed and discarded. See gap 1 below.
5. **A room has never held more than 3 peers.** `MEMBERS_PER_ROOM = 8` and the refuse-when-full path
   are covered by `test-lobby.js`; the largest real game was 3 peers on 8088.
6. **Two concurrent games on one relay has never been run.**

## Known gaps — plainly

1. **One shared room means one game at a time.** Every client uses `prefs.getRelayRoom()`, or the
   literal string `"default"` when unset, and `setRelayRoom` is unwired (above). The protocol and the
   relay support many rooms; the client cannot pick one. Two groups on this relay today land in the
   same room and corrupt each other's lobby. Nothing generates a per-game room token either.
2. **No names, no accounts, no moderation.** Registration is `GXRLY <room> <virtualIP>` with no auth
   of any kind. Whoever knows a room token is in that room. There is no ban, no kick, no report, no
   rate limit on joining, and no way to tell two players apart except by the address the relay handed
   out.
3. **The "Online" screens are the Direct Connect layout.** Functionally right, cosmetically wrong —
   no chat, no staging room, no player list, none of the Online UI. Reimplementing the GameSpy peer
   and staging-room objects to drive the original screens is the real job and is not started.
4. **iPad is four commits behind and needs a package + install.** The engine binary in
   `build/ios-vulkan/GeneralsMD/GeneralsXZH.app/GeneralsXZH` dates 02:45 and the package 02:46 —
   both before `973aed2d4` (02:51). `sourceID` has moved, so a stale build cannot join at all; and it
   predates both halves of identity assignment, so it could not matchmake even if it could join.
   Rebuild the `ios-vulkan` preset, then `./scripts/build/ios/package-ios-zh.sh --install` over USB.
5. **iPhone is offline.** Not built, not installed, not tested. iOS is a target client and the 1100-byte
   payload cap exists for it, so its absence means the constraint that shaped the header design has
   never been exercised.
6. **Map transfer over the relay has never been tested.** `FileTransfer.cpp` / the `LANAPI` map-transfer
   path is unicast, which is precisely what `GXR1` unicast routing was added for, and every match so
   far used the same built-in map (`maps\twilight flame`) already present on both sides. A custom map
   between two peers is untried.
7. **Lockstep waits for the slowest peer, and nothing has measured a mobile client's effect.** Eight-way
   is not two-way with a bigger number: a phone on a bad link stalls all eight. No latency,
   jitter or stall measurement exists for any peer, let alone a cellular one.
8. **Headless on Mac/Linux still builds the full OpenAL backend.** `SDL3GameEngine::createAudioManager(Bool dummy)`
   does `(void)dummy;`. Win32 honours the flag and returns `MilesAudioManagerDummy`; there is no OpenAL
   dummy to return. `aa776b7eb` makes it non-fatal, not absent.

## Rules this session added

1. **Verify the ARTIFACT, not the git tree.** `scripts/build/ios/package-ios-zh.sh` **does not build
   the engine.** It errors only if `build/ios-vulkan/.../GeneralsXZH` is *missing*, then copies
   whatever is there over the Xcode stub. A stale binary packages silently and installs happily — this
   shipped an old engine to the device **twice** tonight. `git rev-parse` on a clean tree proves
   nothing about what is inside the `.app`. Check the binary's mtime against the commit, or read
   `SIMID source=` out of the running artifact.
2. **`paste a b | awk '$2!=$4'` compares shifted columns.** Once `paste` joins two `[GXCRC]` lines the
   fields renumber, so `$2` is `f=0` and `$4` is `[GXCRC]` — never equal. It reported **603 differing
   frames while printing rows that were plainly identical**. Compare the value column (`$3` vs `$6`),
   or just `cmp` two equal-length files. Recorded in `c11bcf10f`.
3. **Never chain a deploy after an unchecked build exit code.** `build ... ; scp ...` and
   `build | tail && deploy` both deploy the previous binary when the build fails, and the deploy
   succeeding is what you see. Capture the build's own `$?` before any pipe and gate on it. Same
   family as the `grep -c` and `a && b && c || echo OK` traps already recorded.
4. **Commit BEFORE building, or the binary carries a dirty-tree digest.** `sourceID` bakes HEAD plus a
   dirty-file overlay in at *compile* time. `b1d5fbfe9` is the mirror image: the working tree was
   edited *after* committing, so the Mac compiled a fix that was never in the commit and the Linux
   build broke on code the Mac had "already built".
5. **The relay and a relay-mode peer cannot share a machine.** Relay mode forces a wildcard bind (a
   virtual address can never be bound), so the game's socket collides with the relay's own listener and
   the host dies with `UDP::Bind failed on 0.0.0.0:8086`. To test more clients than you have machines,
   use **one network namespace per peer** — multi-instance disables relay mode on macOS/Linux by design.
6. **A "identical" run with few distinct CRC values is not evidence.** An earlier 600-frame relay run
   came out identical with **three** distinct values across the whole run: two idle bases doing
   nothing. It was thrown away rather than committed. Always report distinct-value count alongside the
   diff, and the serialised slot list alongside that.

## Still open, carried forward

* `Generals/` (base game) has no headless CLI, and still has the unguarded `LANEnableStartButton`
  null-deref that was fixed in `GeneralsMD/`.
* Windows exits `0xC0000005` at the **end** of a headless run, after every frame and CRC is written.
  Pre-existing, also seen on `-replay`. The exit code proves nothing either way — the CRC stream is the
  verdict, which is why the soak harness never gated on it. Still a real crash.
* **AWOL players block victory** — `hasSinglePlayerBeenDefeated` is purely asset-based and never consults
  connection state. Now more likely to be hit, since people can actually play online. **CAUTION:**
  `VictoryConditions::update` calls `p->killPlayer()`, so any fix mutates simulation state and must use
  state all peers agree on or it desyncs.
* Defeated observers are never pulled to the score screen.
* `RTS_BUILD_OPTION_FFMPEG=OFF` cannot work with `SAGE_USE_OPENAL=ON` (`CMakeLists.txt:305`).
* The committed replay fixture no longer loads against the current engine.
* `%` unescaped in the SagePatch.ini generator (`GameEngine.cpp:518`).
* Terrain draw-window fix is **still not visually verified** — zoom out and look.
* Extending the data fingerprint to cover loose simulation-relevant files (`.scb` is outside `m_iniCRC`)
  would turn a silent desync into a refused join. Still worth doing.

## Waiting on Karl, not on the next session

* **Raise the VPS RAM ticket.** `MemTotal` 3.75 GB on a plan sold as "8 GB Dedicated", `virtio_balloon`
  bound, survived a clean reinstall so it is plan-level config. Memory reclaimed mid-match stalls the
  sim for every player — and there are now real players.
* **Nothing has been pushed.** Everything this session is local commits on `main`. The VPS is running
  a build of this code; once it is pushed, put the VPS back to a pure mirror with
  `git fetch && git reset --hard origin/main`, never `pull`.
* **The relay is running as a service on `163.5.210.131`.** Assume someone is playing before you
  restart it or kill anything named `GeneralsXZH`.
