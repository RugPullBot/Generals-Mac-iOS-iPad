# Plan: public crossplay through the relay

**Goal:** strangers on **iOS, iPadOS, macOS and Windows x64** find each other in a browser, fill an
8-slot lobby and play, off-LAN, with nothing hand-configured. Linux is the *server* platform, not a
client target.

**Where this stands (2026-07-29, HEAD `6377956d8`, rev 2189, two commits unpushed):** determinism
holds at **two measured shapes** — 75,000 frames macOS vs Linux solo with zero networking
(`evidence/xplat-soak-50x1500.*`), and **one** 1500-frame three-platform relay match with five Hard
AI, 1499 distinct, byte-identical, on **one map with one seed** (`evidence/xplat3-5ai-1500.*`). The
tail is unmeasured for the cross-platform case (task 53), and two rows that previously shipped as
determinism proofs were idle sims (task 55). Matchmaking carries real games in multiple rooms, eight
peers fit in one room, and Windows has hosted.

**New and it reframes the rest:** `evidence/networked-sim-freeze.meta.txt` — **networked matches
freeze on every map tested except `alpine assault`**, while the same map, seed and AI spec run
600/600 distinct solo. Nearly every networked result in this corpus used alpine assault, so this
decides how much of that corpus measured a live world. Task 56.

What is left beyond that is **a lobby nobody has looked at, a map-transfer path that does not work,
the mobile clients, and the things nobody has measured.** Full detail in
`docs/WORKDIR/STATE_2026-07-29_session7.md`.

**Compatibility gate, read before building anything:**

* macOS, Linux and Windows are **all deployed from `c72eb8d96`** and were probed at runtime:
  `rev=2187 engine=FD486019 source=B30B651C data=1839A83F ordinal=9F43F7B5 epoch=2`. They
  interoperate — a verified 1500-frame three-platform match is committed.
* **iOS/iPadOS is `2e226bf3a`** (rev 2171, 18 commits back), which is **`SIMID_EPOCH` 1**. The epoch
  feeds `engineID` (`SimulationId.cpp:100`), the first compare (`:553`), so the iPad is refused with
  `ENGINE_DIFFERS`. It is the remaining half of the deploy.
* **The gate is not the commit.** `revision` is never compared (`SimulationId.h:39`, `SimIdCompare`
  at `SimulationId.cpp:548-562`). `sourceID` digests a fixed path list **plus a dirty overlay**
  (`resources/gitinfo/simsourcedigest_watcher.cmake:66-105`, `:146-190`). An identical tree at a
  different commit joins fine; **a dirty clone at the right commit does not.** `git reset --hard`,
  never `pull`.

## The ordered list of what is left

1. **Push `68eff86c4` + `6377956d8` and preserve the soak artifacts** (task 52) — blocking, needs Karl.
2. **The networked simulation freeze** (task 56) — newest, cheapest to reproduce, and it decides how
   much of the networked corpus means anything.
3. **The mobile clients** (tasks 26-28) — iOS is the only platform that cannot join anything.
4. **Client chat** (task 47) — the relay half is done and tested, the client sends nothing.
5. **Click the Online button** (tasks 25, 41-45) — ~870 unobserved lines of UI code now own that path.
6. **The run-ahead measurement** (task 46) — was bundled into task 48 and not taken.
7. **Map transfer** (task 30) — broken in headless; run the UI-client experiment before patching.
8. **Finish the three-platform soak** (task 53) — Windows has ~4,200 frames of active-sim evidence
   against 75,000 for macOS-vs-Linux, and only 1500 of those have Windows with both other platforms.
   Downstream of task 56.
9. Everything below that, in phase order.

---

# Phase 1: eight peers in a room — DONE

- [x] 1. Relay: `MEMBERS_PER_ROOM` 2 → 8; refuse an over-full room instead of evicting a live player,
      and log the refusal. Eviction stays only for genuinely timed-out members. (`99846529d`)
- [x] 2. Relay: parse the `GXR1` header, unicast on a known `dst`, broadcast on a broadcast `dst`,
      drop with a counter on an unknown one. (`99846529d`)
- [x] 3. Client: prepend the header in `Transport::doSend`, outside the CRC/encryption. (`99846529d`)
- [x] 4. Client: strip the header in `doRecv` and use `src` as the reported address — both the live
      path and the `RTS_DEBUG` latency-sim path. (`99846529d`)
- [x] 5. Client: take the header **out of** `MAX_UDP_PAYLOAD_SIZE` rather than adding to it, on every
      platform. Also fixed the two-byte receive overrun this exposed. (`99846529d`)
- [x] 6. Drop `PeerVirtualIP` from the routing path; keep it only as the Direct Connect default.
      (`99846529d`)
- [x] 7. `tools/relay/README.md`: N-player config and the two footguns. (`99846529d`)
- [x] 8. Headless: use the virtual address when a relay is configured, instead of electing an
      interface address. (`3c2d2a7fa`)
- [x] 9. Headless: a joiner must re-assert its accept, or two joiners deadlock. (`c353d8f65`)
- [x] 10. Diff every `[GXCRC]` stream pairwise with the controls that make it mean something —
      distinct-CRC count and the serialised slot list. Done at 3 peers / 900 frames
      (`evidence/relay-3peer-900-*`).
- [x] 11. **8 peers in one room, one per network namespace on the VPS.** Done (`relay-8peer.meta.txt`).
      Relay assigned `10.42.0.1`..`.8` to eight namespace endpoints, room filled (8/8), 8 joins on
      each of 8086 and 8088, host accepted all seven and started the match, all eight in lockstep to
      frame 1500, 28/28 pairs byte-identical. **Reported as a network proof only:** distinct = 3 CRC
      values, because eight headless peers with no AI issue no orders and the sim sits still.
      Determinism at scale is the separate 7-peers-plus-AI run — 1500 frames, 1499 distinct, 21/21
      pairs identical. Eight *active* network peers is not reachable headless: eight peers fill all
      eight slots, leaving none for an AI, and headless peers issue no orders.
      **Caveat added after the freeze finding (task 56):** this run was on `killing fields`, a map now
      known to freeze in networked matches, so "no AI, so the sim sat still" may not be the whole
      explanation for distinct = 3. The network claims are unaffected. Also note only a **400-line
      head sample** of peer0 is committed, so the quoted 1500-frame md5 and "28 of 28 pairs" are the
      harness's word and cannot be recomputed (task 54c, task 55a).

# Phase 2: a server list served by the relay — DONE except the UI proof

- [x] 12. Relay: track advertisements per room, expire them on the existing sweep, answer `GXLIST`.
      (`9e6d1e5dd`)
- [x] 13. Relay: make duplicate virtual IPs within a room **visible** — count identity moves in a
      window and log loudly. Not a refusal: a real NAT rebind happens once and settles, duplicates
      flap, and refusing would break a legitimate rebind on a mobile client. (`9e6d1e5dd`)
- [x] 14. Client: send `GXADV` while hosting, from the registration keepalive, so the list entry and
      the NAT mapping expire together. (`f44e305d2`)
- [x] 15. Client: query `GXLIST`, take `GXGAME` replies off the wire in `doRecv` before the packet
      parser sees them, rebuild the list from scratch per query. (`f44e305d2`)
- [x] 16. Advertise the **host player's name**, not the LAN game name — that is a 16-char uniqueness
      token that always truncates away whatever the host asked for. (`873f12cf9`)
- [x] 17. `-lanlist`, a browser with no UI — the only way to exercise query → reply → parse against
      the real relay on a machine with no display. (`f44e305d2`, `4e7249af3`)
- [x] 18. UI: show the relay's game list on the Direct Connect screen. (`25e3212f0`)
- [x] 19. Windows: stop the multi-instance guard disabling relay mode there. (`15d18ed30`)
- [ ] 20. **Prove the UI browser.** `evidence/relay-gamelist-browse.txt` is the *headless* path. No
      committed evidence that a dropdown or list renders live rooms, that picking a row joins, or
      that a host is correctly absent from its own list. Now applies to the WOL lobby (phase 7) as
      well as the Direct Connect screen.
- [x] 21. **Two concurrent games on one relay** — each listed, each joinable, neither seeing the
      other's traffic. Done at 2 rooms / 3000 frames each, byte-identical per room and differing
      between rooms, with the idle host at 0 frames and no announces (`evidence/relay-tworooms-*`).

# Phase 3: identities and rooms — DONE except the Online button

- [x] 22. Relay + client: `GXWHO` / `GXYOU`, sticky per endpoint, excluding already-registered
      addresses, released on the sweep. Wired into the headless driver, the Direct Connect screen and
      the relay-mode gate. (`973aed2d4`, `d87460c65`, `6a2b7482e`)
- [x] 23. **Wire `Transport::setRelayRoom`.** Implemented in `43bcadd8b` and exercised end to end in
      session 7: (a) two hosts generated two different tokens with no coordination, (b) one `GXLIST`
      returned both rooms, (c) a joiner with nothing pinned browsed, took the room off the LISTING and
      entered it. Note the consequence, which is now real behaviour: addresses are per room, so every
      host is `10.42.0.1` and `-lanjoin <ip>` alone is ambiguous once two rooms exist. It exits 1 and
      names the candidates; a headless join in a multi-room world needs `GENERALSX_LANROOM`. The UI
      does not hit this because a picked row carries its room.
- [x] 24. **Play one match with no `LocalVirtualIP` configured on any machine.** Done three times in
      session 7 — both two-room matches and the Windows-hosted match. Every `Options.ini` cut to
      `RelayAddress` alone, every address relay-assigned.
- [ ] 25. **Click the Online button in a built binary.** Never done. It no longer opens the Direct
      Connect layout — since `9c6a89f2c` it opens `WOLCustomLobby.wnd` (phase 7), which has never
      been drawn. **This is now a higher-risk item than it was**, because a failure takes the whole
      Online path with it.

# Phase 4: the client matrix

- [ ] 26. **iPad Air M3** — the **only** platform that cannot join anything. Both iOS binaries embed
      git SHA `2e226bf3a` (rev 2171, 18 commits back, verified with
      `strings -a build/ios-vulkan/GeneralsMD/GeneralsXZH.app/GeneralsXZH | grep -aoE '^[0-9a-f]{40}$'`),
      and `SIMID_EPOCH` went 1 → 2 inside that range, so it is refused at `engineID` before `sourceID`
      or `dataID` is compared. Rebuild the `ios-vulkan` preset **first**
      (`cmake --preset ios-vulkan && cmake --build build/ios-vulkan --target z_generals`), confirm the
      binary's mtime moved, **then** `./scripts/build/ios/package-ios-zh.sh --install` over USB. The
      packaging script checks only that the binary **exists** (`:70-76`) and copies it (`:152`) — it
      does not build. That is how a stale engine shipped to the device twice.
      **There are four stale iOS artifacts, not one.** `build/ios-vulkan/GeneralsMD/GeneralsXZH.app/GeneralsXZH`
      (Jul 29 04:53) and `build/ios-package/GeneralsXZH.app/GeneralsXZH` (Jul 29 04:54) both embed
      `2e226bf3a`; `GeneralsXZH-ios.ipa` (Jul 27 03:51) and `GeneralsXZH-ios-codeonly.ipa`
      (Jul 27 05:05) sit in the repo root and predate both. `package-ios-zh.sh --install` over USB is
      the **only** supported path — **regenerate or delete the two `.ipa` files, never sideload
      them.** Reaching for the obvious-looking `.ipa` is how a stale engine ships a third time.
- [ ] 27. **iPhone** — not built, not installed, not tested. iOS is why `MAX_UDP_PAYLOAD_SIZE` is 1100;
      that constraint has never been exercised on a real cellular link.
- [ ] 28. **The real matrix in one lobby:** macOS + Windows x64 + iPadOS + iOS through the relay.
- [ ] 29. Commit a **Windows** CRC stream for a ≥3-platform *played* (human) match. Mostly addressed:
      `evidence/xplat3-5ai-1500-win.crc` is a Windows stream from a genuine three-platform match
      (macOS + Linux + Windows + 5 Hard AI, 1500 frames, byte-identical, 1499 distinct), and
      `evidence/relay-winhost-600-*` is Windows as host. Windows' SimID is confirmed at runtime and
      currently reads `rev=2187 source=B30B651C platform=7480F925 epoch=2`. What is still open is the
      narrow original case: a **human-played** ≥3-platform match with the Windows stream captured.
      Note `evidence/relay-3platform-93017-windows.crc` does **not** close it — no Windows-side log is
      committed for that run, and since all three streams are byte-identical nothing distinguishes it
      from a copy; its provenance is the filename.

# Phase 5: the things nobody has measured

- [ ] 30. **Map transfer.** BLOCKED, and **not by the relay** — see `evidence/relay-maptransfer.meta.txt`
      and `relay-maptransfer-failure.txt`. It does not work in headless **at all**, with or without a
      relay. With a genuinely missing map the joiner detects it and the transfer then fails ("Unable
      to transfer the map"), while the host is not blocked: it starts, plays its full frame budget
      alone with the AI, and stops only when the joiner leaves — so the host's transfer mask was
      empty. **The same scenario on a plain LAN with no relay fails character-for-character
      identically**, so `GXR1` unicast routing is neither implicated nor exercised.
      **Narrowed in session 7 with positive evidence:** the host's `OnHasMap` **never fired**.
      `HeadlessMatch.cpp:226-232` logs `peer %d.%d.%d.%d does NOT have the map` whenever status is
      FALSE, and that line is **absent** from the host log in `relay-maptransfer-failure.txt` while the
      joiner's own local line is present. The joiner does send its status
      (`HeadlessMatch.cpp:714 TheLAN->RequestHasMap()`, built at `LANAPI.cpp:836-852` as
      `MSG_MAP_AVAILABILITY`), so the message is dropped host-side. That leaves exactly three
      early-outs in `LANAPIhandlers.cpp::handleHasMap`: `:911 if (!m_inLobby && m_currentGame)`,
      `:917` the `mapNameCRC` equality, `:922-926` the senderIP-to-slot match. Only past all three does
      `:927` reach `OnHasMap`. **Do not assume the relay-assigned virtual IP breaks `:922-926`** — the
      identical failure reproduces on plain LAN with no relay.
      Second lead, also unproven: the transfer is UI-driven (`FileTransfer.cpp:260-262`
      `TheShell->hideShell()`, a `MapTransferLoadScreen`, `ls->init()`; the pump is
      `ls->processProgress()` at `:91-109` against `TheNetwork->getFileTransferProgress()` at `:87`,
      inside `doFileTransfer` at `:41`) and headless never drives that load screen.
      **Decisive next step: the same missing-map join from a UI client on
      macOS**, which separates "headless cannot drive it" from "map transfer is broken for everyone".
      Two traps for whoever picks this up: a renamed stock map will **not** reproduce it — maps are
      identified by **CRC and size, not name**, so the joiner resolves it locally and the run looks
      like a clean pass; pad the file. And a loose map is addressed by its full **lowercased absolute
      path**, not the relative key an archive map uses. `tools/bigtool.py` extracts a map from
      `MapsZH.big`; `scripts/test/relay-maptransfer.sh` runs the scenario.
- [ ] 31. **Latency, jitter and stall behaviour**, especially with a phone on cellular. Lockstep waits
      for the slowest peer, so one bad link stalls all eight. No measurement of any kind exists.
- [ ] 32. **Names, accounts, moderation.** Registration is `GXRLY <room> <virtualIP>` with no auth:
      whoever knows a room token is in that room. No kick, no ban, no report, no join rate limit. The
      chat channel (task 47) is sender-stamped and rate-limited, which is the anti-abuse floor, not
      moderation.
- [x] 33. **The real Online UI.** Superseded by phase 7 — the Online button now opens Zero Hour's own
      `WOLCustomLobby.wnd` fed by our relay, with no GameSpy reimplementation. **Landed, never
      observed.** See phase 7 for what a first run has to check.

# Phase 6: cleanups that are now more likely to bite

- [ ] 34. **AWOL players block victory.** `hasSinglePlayerBeenDefeated` (`VictoryConditions.cpp:328`)
      is purely asset-based and never consults connection state, so "Exit to lobby" leaves your base
      standing and nobody can win. **CAUTION:** `VictoryConditions::update` calls `p->killPlayer()`, so
      any fix mutates simulation state and must use state all peers agree on or it desyncs.
- [ ] 35. Defeated observers are never pulled to the score screen.
- [ ] 36. Windows exits `0xC0000005` at the **end** of a headless run, after all frames and CRCs are
      written. Re-observed twice in session 7 (the Windows-host run and the three-platform run).
      `xplat-3platform-lobby.sh:249-252` deliberately does not gate on the exit code because the frames
      are already flushed. Cosmetic to the tests, a real crash regardless. (Line numbers for that
      harness are for the committed file at HEAD `6377956d8`, which includes the
      `SEED`/`SKIP_FINGERPRINT` additions.)
- [x] 37. Extend the data fingerprint to cover loose simulation-relevant files. Done in `3d6befd3c`:
      `dataID` becomes `m_iniCRC` mixed with a digest of `Data\Scripts\*.scb`, an explicit allow-list
      of one directory and one extension rather than a directory scan. `SIMID_EPOCH` 1 → 2 so an old
      peer refuses at `engineID` (`ENGINE_DIFFERS`) rather than at `dataID` with a misleading "your
      INI files differ". **The digest has now RUN at runtime** on three operating systems and agrees:
      `data-loose=300665B3` with `data-ini=FEAAE3F3` on all three peers
      (`evidence/xplat3-5ai-1500.meta.txt:14-16`). What has still never been observed is the *refusal*
      it would cause.
- [ ] 38. `SDL3GameEngine::createAudioManager(Bool dummy)` does `(void)dummy;` — headless on Mac/Linux
      runs the full OpenAL backend. Win32 returns `MilesAudioManagerDummy`; there is no OpenAL dummy.
- [ ] 39. `Generals/` (base game): **no headless LAN driver.** It does have `-headless`
      (`Generals/.../CommandLine.cpp:1150`, `parseHeadless` at `:416`) and the full `m_headless`
      plumbing; what is missing is the `-lanhost`/`-lanjoin`/`-lanai`/`-lanseed` family, which is
      ZH-only (`GeneralsMD/.../CommandLine.cpp:1369-1377`). **The `LANEnableStartButton` half of this
      item was wrong** — the guard was mirrored to `Generals/` in `a2e959ee0` (rev 2126) and is live at
      `Generals/.../LanGameOptionsMenu.cpp:388`.
- [ ] 40. Terrain draw-window fix is still **not visually verified** — zoom out and look.

# Phase 7: a real lobby UI — reuse the stock WOL screens, do not build one

**The finding that shapes this:** the multi-column lobby everyone recognises from Generals Online is
not something they built. It is Zero Hour's own `WOLCustomLobby.wnd`, and it ships in our archives
already, unused — the Online button used to start the GameSpy chain, and GameSpy died in 2014.

So the work is **not** designing a UI. It is feeding the existing one from our relay.
`WOLLobbyMenu.cpp` and `LobbyUtils.cpp` are pure presentation: they read staging rooms out of
`TheGameSpyInfo` and draw them, and do not care where the rows came from.

- [x] 41. Point the Online button at `WOLCustomLobby.wnd` instead of the Direct Connect layout.
      Done in `MainMenu.cpp` (`9c6a89f2c`). `NetworkDirectConnect.wnd` is **not** retired — it is
      still reachable from the LAN lobby and it is the browser that has actually carried real matches.
- [x] 42. Populate the staging-room list from our relay's `GXGAME` replies rather than GameSpy peer
      callbacks — a `Transport::RelayGameListing` mapped onto a `GameSpyStagingRoom` and handed to
      `addStagingRoom`. The switch that makes it work without touching the GameSpy half of that file
      is `TheGameSpyPeerMessageQueue` being **null**: `WOLLobbyMenuUpdate` already gates its whole
      GameSpy message pump on that pointer, so `SetUpGameSpyForRelayLobby` creates only the four
      presentation singletons and starts none of the threads.
- [x] 43. Wire Join to the room-switch flow from `JoinDirectConnectGame` — room from the selected
      **listing**, `enterRelayRoom`, rebuild `TheLAN`, `SetLocalIP`, dial. The ordering is
      load-bearing and is now extracted as `SetUpOnlineLobbyLAN` so the two online screens cannot
      drift apart; the Direct Connect copy is gone, not duplicated. Done as `relayJoinSelectedGame()`.
      The listing's room travels with the row in a side table keyed by the listbox item-data ID,
      because the host address alone cannot identify a game — addresses are per room, so every host in
      the browser is `10.42.0.1`.
- [x] 44. Decide what the player list shows. **Occupants of the SELECTED game**: the host by name,
      other occupied slots numbered. The count comes from the host's own slot list, so those rows are
      real; the names are not known and are not invented. A global player list would need the relay to
      track identities across rooms, which it deliberately does not.
- [x] 45. **Chat protocol.** Done relay-side in `5292259c7`: `GXCHT <room> <text>` in,
      `GXSAY <room> <senderVirtualIP> <text>` out to the rest of the room. Sender stamped **by the
      relay** out of its registration table — the wire format has no sender field, because a client
      that can name itself can name the host. Bounded (320-byte datagram, text cut to 100 =
      `g_lanMaxChatLength`), sanitised (control bytes → spaces, `|` deliberately kept), rate-limited
      (5 burst then one per 2 s, bucket on the member record so it adds no table), never logged.
      `test-lobby.js` 36 → 52 checks, all passing. **Client half is task 47.**

## Landed but NEVER OBSERVED

Everything in 41-45 is **compile-verified only** — `build-macos-zh.sh --build-only` exits 0 and
`g_gameengine` exits 0. **No one has clicked the Online button in a build containing this.** No
evidence the layout renders, that a row appears, or that Join reaches a host. What a first run has to
check:

- the screen draws at all — `WOLCustomLobby.wnd` has never been loaded by this engine;
- the game list fills, and a row's map / player-count / host-name columns are the right way round;
- Join switches rooms and dials — look for `relayJoinSelectedGame - moving from room ... to ...` and
  the host's join accept;
- Host reaches `Menus/LanGameOptionsMenu.wnd`;
- Back returns to the main menu rather than the GameSpy login screen.

Two known limits that are properties of the relay protocol, not bugs to fix here:

- **No version check on a listed game.** `GXGAME` carries no exe/ini CRC, so the mismatch colouring
  and refuse-to-join that this screen normally performs cannot run. Rooms are reported with our own
  CRCs — the honest encoding of "unknown", since zero would claim a conflict we have not detected. A
  genuine mismatch still fails, just later, in the lobby's slot-list exchange.
- **No ping.** `insertGame` leaves the ping column blank when a room has no ping string, rather than
  rendering the absence of a measurement as a bad one (`getPingValue` answers the full timeout when it
  has nothing to compare, which drew a red one-bar "unplayable" icon on every relay row).

## What NOT to do

Do not reimplement the GameSpy peer/staging-room protocol to drive these screens "properly". The
screens do not care where their rows come from. Reimplementing GameSpy is the large, faithful,
pointless version of this task.

# Phase 8: the leads opened in session 7

- [ ] 46. **The network path never got the logic/render decoupling — measure it.** In a network game
      `canUpdateNetworkGameLogic()` (`GameEngine.cpp:1011`) ticks **one logic frame per RENDERED
      frame** — no time accumulator, unlike `canUpdateRegularGameLogic()` (`GameEngine.cpp:1027`),
      which the single-player path has had since the frame-pacer work. On top of that,
      `ConnectionManager::updateRunAhead` (`ConnectionManager.cpp:1390`) computes `minFps` from
      `m_fpsAverages` — fed from `TheDisplay->getAverageFPS()`, the **render** rate
      (`FrameMetrics.cpp:90`) — clamps the shared logic rate to it (`ConnectionManager.cpp:1421`,
      commented "this clamps the logic time scale fps in network games") and sends it to everyone.
      **So one client's render hitches throttle the simulation for every player.** The simulation is
      nowhere near the bottleneck: 2,037 logic frames/sec = 67.9x realtime on the M4
      (`DESIGN_headless_and_relay.md:122`).
      **This is the highest-value lead for reducing lag, and it is NOT a networking change.**
      **Inferred from code, not measured.** Two independent audits re-derived every line reference
      above and every one held at HEAD, plus: `canUpdateNetworkGameLogic` is
      `m_frameDataReady || m_localStatus == NETLOCALSTATUS_LEFT` (`Network.cpp:842-844`), the minimum
      is taken by `getMinimumFps()` (`ConnectionManager.cpp:1556-1568`), the floor is
      `MIN_LOGIC_FRAMES = 5` (`NetworkUtil.cpp:32`), and the slowest player is separately sent
      `min(minFps * 11/10, 30)` (`:1490-1500`). Also worth knowing while designing the measurement:
      `LANAPICallbacks.cpp:260` sets `m_useFpsLimit = false` when a network game starts, so render runs
      uncapped during a match while `m_framesPerSecondLimit` still serves as the clamp ceiling at
      `:1421`.
      **Task 48 was supposed to carry this measurement and did not.** No render-fps-vs-`m_frameRate`
      data exists anywhere in `docs/WORKDIR/evidence/`. First step is still the measurement, not a
      patch: log each peer's render fps against the negotiated `m_frameRate` during a real match and
      confirm the slowest renderer is setting the pace.
- [ ] 47. **Client chat.** Send `GXCHT` from the lobby chat entry and render incoming `GXSAY` into the
      chat box, so the WOL lobby's chat stops being disabled with a "chat is unavailable" line. Relay
      half is done and unit-tested (task 45; `node tools/relay/test-lobby.js` exits 0 with
      `52 checks, 52 passed, 0 failed`, reproduced rather than trusted). **The client half does not
      exist at all:** an *unbounded* grep for `GXCHT|GXSAY` across the repo hits `tools/relay/relay.js`,
      `test-lobby.js`, `tools/relay/README.md`, `tasks/todo.md` and the WORKDIR docs — **zero `.cpp` or
      `.h` files**. The chat box is deliberately inert (`WOLLobbyMenu.cpp:1170`
      `textEntryChat->winEnable(FALSE)`, `:1172` emote, `:1174` buddy, `:1207` pushes
      `GUI:RelayLobbyNoChat`), and `Transport.h` exposes no chat API — only `sendRelayRegistration`
      (`:105`) and `enterRelayRoom` (`:178`). One send call plus one receive hook, not a feature build.
      **Confirm the deployed relay is running the chat build before testing a client against it** —
      `5292259c7` says it was not deployed and nobody has checked since (UNVERIFIED, needs the VPS).
      **The running relay is NOT the repo copy.** `tools/relay/README.md:135-197` deploys it as
      systemd unit `gxrelay` executing `/opt/gxrelay/relay.js`, installed by
      `scp tools/relay/relay.js root@VPS:/opt/gxrelay/relay.js` — so `git reset --hard` in
      `/root/GeneralsX` does **not** update it. Check with
      `ssh root@163.5.210.131 'systemctl status gxrelay; grep -c GXCHT /opt/gxrelay/relay.js'` and
      **read the number** (`grep -c` exits 0 when it finds matches — never gate on it); logs via
      `journalctl -fu gxrelay`. Redeploy is that same `scp` plus `systemctl restart gxrelay`, which
      **drops every game in progress** and needs Karl's OK. Whether the live box actually matches the
      README's layout is itself unconfirmed — check, do not assume.
- [x] 48. **A lobby with 5 Hard AIs plus macOS, Windows and Linux peers.** Done in `68eff86c4`.
      Harness `scripts/test/xplat-3platform-lobby.sh`; evidence `evidence/xplat3-5ai-1500.*`. 8 of 8
      slots (gxlinux host + gxmac + gxwin + 5× Computer Hard) on `alpine assault`, 1500 frames on each
      peer, md5 `b4903b2fe45f103c91f547f3feaef405` on all three (**value column** — the whole-file md5
      is `7ce3345597cae78890a4522a07fee551` and quoting the wrong one looks like broken evidence),
      `cmp` IDENTICAL on all three pairs, 0 differing, **1499 of 1500 distinct**,
      `aiPlayers=5 computers=7 sides=11 sidesWithScripts=7`.
      Scope, stated: **one map, one seed, one match.** Volume is task 53.
      **The task-46 measurement was NOT taken in this run** — no render-fps data exists. It is now its
      own item.
      The first attempt used `killing fields` and produced the same single md5 across all three
      platforms with only **4 distinct CRC values in 1500 frames**, with the AI correctly placed
      (`aiPlayers=5 sidesWithScripts=7`) and idle. Agreement on an idle sim is not evidence; the
      harness fails the run rather than reporting it. **That idle run is now understood as the
      networked freeze (task 56), not a map property** — killing fields runs 600/600 distinct solo
      with the same AI spec.
- [ ] 49. **Destroyed structures keep their old visuals.** Reported twice with screenshots.
      Client-side only — all peers agree they are destroyed. **A spawned task holds the full context.**
      Decisive next step: check the same building on Windows. Broken on both means game logic; broken
      only on macOS means the DXVK/MoltenVK path.
      **Before touching the DXVK submodule:** `references/fbraz3-dxvk` carries three *uncommitted*
      source edits — `src/vulkan/vulkan_loader.cpp`, `src/wsi/sdl3/wsi_platform_sdl3_funcs.h`,
      `src/wsi/sdl3/wsi_window_sdl3.cpp` — plus an untracked `subprojects/.wraplock`. They are
      **outside** the SimID digest so they cannot affect join compatibility, but they **are** on the
      macOS render path that ships as `libdxvk_d3d8.dylib`, which puts them directly next to this task
      and task 50. **A `git submodule update` would discard them.** What they change and why they are
      uncommitted is an open question — nobody has read the diffs.
- [ ] 50. **1-2 s freezes during play.** Ruled out: the simulation (host advanced 51-56 frames per 2 s
      across 80 s, zero stalls), the network (lockstep would have dragged the host), and DXVK file
      logging (disabled in both places). Leading hypothesis is **DXVK pipeline compilation** — the
      cache was cold (90 KB) and written during play. **Inferred, not proven.** Free check: if the
      freezes reduce as the cache warms, that was it. Note the coupling to task 46 — whatever causes
      the stall, the stall is shared with every other player.
- [ ] 51. **`unrouted` is no longer always zero.** The two-rooms run recorded `unrouted 1` on 8086 and
      `2` on 8088 out of ~6900 forwarded packets, where every previous run recorded 0. Both matches
      end with one side exiting its frame budget while the other is still sending, so teardown is the
      leading explanation — **read off the timing, not proven.** Pin it down before this number is
      treated as normal.

# Phase 9: hygiene the audits surfaced

- [ ] 52. **Push, and rescue the artifacts.** Three separate things, all needing Karl or a commit:
      (a) `68eff86c4` and `6377956d8` are unpushed and `origin/main` is `c72eb8d96`; the Linux and
      Windows clones sync with `git fetch && git reset --hard origin/main` and physically cannot reach
      HEAD until it is pushed — which also blocks the VPS-only freeze reproduction, since that runs
      the committed `relay-8peer.sh`. (b) **The 75,000-frame headline has no committed raw artifact** —
      only the meta, manifest and console are in git. The 100 `.crc` streams exist only at
      `/tmp/claude-501/-Users-administrator-GeneralsX-src/1f6becd6-6780-4ff5-b682-a13c6ea6d85e/scratchpad/soak50`
      (3.9 MB, verified present, all 50 pairs recomputed from there and every number holds). A reboot
      deletes it and the claim becomes unfalsifiable prose. Copy into `docs/WORKDIR/evidence/` and
      commit. (c) The remaining uncommitted working-tree changes are these docs, the `ONLY_MAP` filter
      in `scripts/test/xplat-3platform-soak.sh` (+16 lines), and `references/fbraz3-dxvk` — see the
      note under task 49 before doing anything with that last one.
- [ ] 53. **Finish the three-platform soak.** `scripts/test/xplat-3platform-soak.sh` (committed in
      `6377956d8`) drives `xplat-3platform-lobby.sh` once per iteration across an 11-map rotation
      (`:79-91` as committed; the working tree adds an uncommitted `ONLY_MAP` substring filter, +16
      lines) with pinned seeds (`SEED_BASE + 977n`, default 900000), skips the ~90 s fingerprint
      probes after iteration 0, and counts an iteration whose distinct count is under half its frame
      count as INCONCLUSIVE rather than a pass. **It has now been executed** — and its first run found
      the networked freeze (task 56), which is why the volume it was built to collect does not exist
      yet. **Downstream of task 56:** until the freeze is understood a long soak would measure
      `alpine assault` over and over.
      Why it matters: Windows has **~4,200 frames** of active-sim evidence — 1500 `xplat3-5ai-1500`
      + 1500 `xplat-lan-ai-1500` + 600 `relay-winhost-600` + 600 `relay-win-vs-linux-600` — of which
      only **1500** have Windows in a match with *both* other platforms, on one map with one seed,
      against 75,000 for macOS-vs-Linux. (An earlier draft said "~2,100 frames"; that dropped two sets
      this file lists as determinism evidence. The wrong figure is also in `6377956d8`'s commit
      message and `xplat-3platform-soak.sh:10` — fix the script header when it is next touched.)
      A failure here is ambiguous between simulation and transport by construction — that is what the
      pinned seed is for: re-run the failing seed through `xplat-determinism-soak.sh` to tell the two
      apart.
- [ ] 54. **Close the reproducibility gaps.** (a) `C:\dev\cb.bat` is the Windows build driver and
      exists **nowhere in this repo** — `find` plus an unbounded grep return doc mentions only, no
      file. Copy it in; it is the largest reproducibility gap of the four platforms. (a2) Same shape:
      `/root/vpsbuild.sh` is asserted to equal `scripts/build/linux/build-linux-relay.sh` and nobody
      has diffed them (`diff /root/vpsbuild.sh scripts/build/linux/build-linux-relay.sh`). If they
      have drifted, the Linux build runs with unknown flags. (b) Delete the stale remote branch
      `origin/fix/blocker5-residual-libm-trig` (deleted locally, never remotely; confirmed with
      `git branch -r`). (c) **Preserve every stream a multi-peer run writes** — `relay-8peer` quotes
      "28/28 pairs" and md5 `c0c918356475555c750e091c2ad82c0c` from a run where only a 400-line head
      sample survives, and `relay-7peer-ai-1500` quotes "21/21 pairs" with only peer0 committed.
      Neither is recomputable. `relay-8peer.sh:153` already writes `peer$i.crc` per peer, so keeping
      them is free.
- [ ] 55. **Re-label the idle-sim evidence wherever it is cited.** `xplat-lan-3000` (3 distinct across
      3000 frames, one value covering 2,996) and `xplat-inet-mac-vs-linux-60` (3 distinct across 60)
      are **transport proofs, not determinism proofs**, and `xplat-lan-3000` sat in the previous
      dropoff's "proven" table uncaveated. The real macOS↔Windows determinism proof is
      `xplat-lan-ai-1500` (1499 distinct, `CH` slot in `xplat-lan-ai-1500-slotlist.txt`, harness
      `scripts/test/xplat-lan-soak.sh` — which was missing from every dropoff doc until now). Fixed in
      the session 7 docs; check any other file that cites them.
      Two more labelling debts, both from the audits: (a) `relay-8peer` is committed only as a
      **400-line head sample**, so its "1500 frames / 28-of-28 pairs / md5 `c0c918...`" cannot be
      recomputed — cite the sample's numbers, not the meta's prose; and `relay-7peer-ai-1500` has only
      peer0, so "21/21 pairs" is the harness's word. (b) `xplat-lan-3000` has **no committed slot
      list**, so "no AI" is the harness default rather than an observation.
- [ ] 56. **The networked simulation freeze.** `evidence/networked-sim-freeze.meta.txt`, found by the
      first run of `xplat-3platform-soak.sh` (`6377956d8`). **In a NETWORKED match the world stops
      changing after about four frames on every map tested except `alpine assault`. The identical map,
      seed and AI spec run 600/600 distinct SOLO.** Not a desync: every peer holds the same frozen
      value, so lockstep and the transport are intact. Measured: networked 2-peer 300 frames —
      alpine 299 distinct, bitter winter 6, killing fields 5; three real platforms — bitter winter 5
      of 300, killing fields 4 of 1500. Ruled out: the map, the seed, the AI count (Hx3/Hx5/Hx7), AI
      placement (`aiPlayers=5 computers=7 sides=11 sidesWithScripts=7` identical both ways), map load
      failure, start positions.
      **Where to look:** whatever differs for AI players between the solo and the networked start
      path. They are constructed and their scripts attach in both cases; the question is why they
      issue no orders once a network game is running.
      **Check these two committed counterexamples FIRST, before touching engine code.**
      `xplat-lan-ai-1500` is an **active networked AI match on `twilight flame`** (1499 of 1500
      distinct, `M=03maps/twilight%20flame MC=F0F7E3EB`, one `CH` slot) — but it ran on a **plain LAN
      with no relay** and was built at `892738bc2` (rev 2131, 58 revisions before HEAD). Whether the
      freeze is relay-specific or a regression is an **open question**; both differences are untested,
      and no committed networked result on `twilight flame` exists at HEAD.
      `relay-3platform-93017` and `relay-played-match-14756` were also on `twilight flame` and fully
      active, but they were human-played, so human orders explain that on their own.
      **Consequences already visible:** nearly every networked result in this corpus used `alpine
      assault`, so the rest describe one map. `relay-8peer` (distinct = 3) ran on `killing fields`,
      which means its idleness may be the freeze rather than "eight peers with no AI" — re-run it on
      alpine with the same no-AI lobby to separate the two. And the stated cause of the "an AI is not
      automatically active" rule (*skirmish scripts attach only for AI players and not every map
      carries them*) is **falsified**: killing fields runs 600/600 solo with `sidesWithScripts=7`
      identical in both runs.
      **Reproduction, two minutes, VPS only:**
      `PEERS=2 FRAMES=300 AI=Hx5 MAP='maps\bitter winter\bitter winter.map' OUT=/root/freeze-repro ./scripts/test/relay-8peer.sh`
      — expect INCONCLUSIVE with ~6 distinct; swap to alpine assault for the passing control.
      **Do not "fix" a soak by dropping the frozen maps from its rotation** — they are the finding.

---

## Risks that have not gone away

* **8-way is not 2-way with a bigger number.** Lockstep waits for the slowest peer (tasks 31, 46).
* **The mobile packet budget hides.** An oversized packet fragments and presents as a desync that only
  ever reproduces on cellular. The 12-byte header came out of the 1100-byte budget for this reason;
  nothing has confirmed it on a real phone.
* **Maphack is unsolvable in lockstep** — every client must know the whole world to simulate it. Catch
  it behaviourally by re-simulating replays, not by trying to prevent it.
* **Never use burstable CPU for the relay.** A throttled sim does not degrade gracefully, it stalls the
  match for everyone. Budget 30-40% of one *dedicated* core per heavy match.
* **Deploys are atomic, and the gate is tree content rather than the commit.** `sourceID` digests a
  fixed path list plus a **dirty-file overlay**, so a clone that is at the right commit but not clean
  refuses every join while `git rev-parse` looks perfect. Sync remotes with
  `git fetch && git reset --hard origin/main`, never `pull`, and check `git status --porcelain` is
  empty before building. iOS is currently the one platform outside the set.
* **cwd is part of the identity.** The loose-data scan is cwd-relative (`SimulationId.cpp:275`,
  `:316`), so launching a peer from the wrong directory changes `dataID` and refuses every join with
  `DATA_DIFFERS` — which reads exactly like a genuine cross-platform data mismatch and is not one.

## Phase 10 — the map-capacity finding (session 7, added last)

- [x] **56.** Root-cause the "networked simulation freeze". **DONE — there is no freeze.** The
      harness put 8 players into 2-player maps; surplus players get `startPos=-1`, no Command
      Center, and their AI owns nothing (`GameLogic.cpp:882-885`, `:928`). `alpine assault` only
      looked healthy because it carries a train whose transform is hashed into the object CRC.
      Falsifier: networked alpine scores 299/300 distinct with five Hard AI **and** with none.
      Commit `1566959a9`; write-up `evidence/networked-sim-freeze-diagnosis.md`.
- [x] **57.** Guard the harness against over-capacity lobbies. Preflight in
      `xplat-3platform-lobby.sh`, verified red-green (rejects alpine+Hx5, accepts twilight+Hx5).
- [x] **58.** Re-run the three-platform test on a legal map with a 0-AI control.
      `evidence/xplat3-tf-legal-1500.*` — twilight flame, 8/8 legal, 1500 frames, 1500 distinct,
      md5 `b29a9b44bd3a5ff68e77aef25f966baa` on all three, `cmp` identical on all three pairs.
- [x] **59.** Decide whether the ENGINE should refuse/clamp an over-capacity lobby.
      **DECIDED: yes, guard only, in two parts. Both are provably no-ops on a legal match.**
      - **Part 1, driver.** `HeadlessMatch::lobbyFitsTheMap` refuses an over-capacity lobby,
        mirroring `LanGameOptionsMenu.cpp:249-258`. Two call sites: an *intent* pre-check at the
        end of `publishGameOptions` (fails in ~1 s, before peers connect) and the *authoritative*
        check on the real slot list just before `RequestGameStart`. The check is NOT inside
        `publishGameOptions` alone because `isOccupied()` is FALSE for `SLOT_OPEN`, so counting
        the slot list there undercounts by `-lanwait`. Escape hatch `-lanoverfill` restores the
        old behaviour, loudly, for reproducing historical runs.
      - **Part 2, engine.** `GameLogic.cpp` guards `farthestIndex < 0` and skips the
        `taken[farthestIndex] = TRUE` out-of-bounds stack write. `setStartPos(-1)` is deliberately
        left outside the guard so the stored value is unchanged. Skipping the `teamPosIdx` write
        is provably equivalent: the branch is entered via `( team < 0 || teamPosIdx[team] == -1 )`.
        Why it was empirically harmless: `hasStartSpotBeenPicked` is declared immediately before
        `taken[]` (`:924-925`) and is already TRUE by then, so the write was idempotent under
        declaration-order layout — a coincidence, not a guarantee.
      - **Rejected: anything that reassigns, wraps to 0, or shares a start position.** That moves
        the frame-0 world CRC, invalidating every `.crc` in `evidence/` and, with
        `RETAIL_COMPATIBLE_CRC` defaulting to 1, retail 1.04 replay compatibility.
      - Harness fallout fixed in the same commit, via new shared `scripts/test/lib-map-capacity.sh`:
        `relay-8peer.sh` (killing fields, holds 2, at PEERS=8), `relay-maptransfer.sh` (alpine,
        holds 2, needed 3), `winhost.ps1` (alpine, needed 3), plus capacity-aware map selection in
        `xplat-determinism-soak.sh` (skips + counts) and `xplat-3platform-soak.sh` (filters the
        rotation up front and names every excluded map).
- [ ] **60.** Re-run the networked corpus on capacity-8 maps (`death valley`,
      `destruction station`, `twilight flame`) and re-label every pre-`1566959a9` row that used a
      2-player map. Every "8-player" result before that commit was overfilled.
- [x] **61.** Fix the activity metric. **DONE.** The verdict no longer rests on "distinct CRC".
      - **New `[GXACT]` engine trace** (`GameLogic.cpp`, beside `[GXCRC]`), off unless
        `GX_ACTIVITY=<frame period>` is set. Reports per player `kind:objects/structures`, where
        kind is `A` skirmish AI, `H` human playable, or `-` neutral/observer/dead/empty slot.
        Only `A`/`H` are judged — `ThePlayerList` is not the lobby and always carries a neutral
        entry plus one per empty slot, which legitimately own nothing.
        **Proven inert:** the same seed with and without `GX_ACTIVITY` produces a byte-identical
        `[GXCRC]` stream (md5 `af5fdd3ed66fecfb6a1e7dfce2691ae2`).
      - **It separates what distinct could not.** Measured: legal twilight+Hx5 and overfilled
        alpine+Hx2 BOTH score 300 distinct of 300; `[GXACT]` flags `p4` in the second and nothing
        in the first. Frame 0 is always sampled (`frame % period == 0`), which is what catches a
        starved AI before the engine marks it dead.
      - **Shared `scripts/test/lib-activity.sh`** — `starved_players`, `has_activity_trace`,
        `activity_summary`. Distinguishes "not measured" from "measured and clean".
      - **`relay-8peer.sh` two-denominator bug fixed:** `DISTINCT0` was counted over the full
        peer0 stream and thresholded against `SHORTEST/2`, so it could print the impossible
        `distinct (peer0): 1500 of 3` and exit PASS. Now counted over the compared frames.
      - **Frame floor (90% of requested) added to all five harnesses** —
        `relay-8peer.sh`, `xplat-3platform-lobby.sh`, `xplat-3platform-soak.sh`,
        `xplat-determinism-soak.sh`, `xplat-lan-soak.sh`.
      - **`AI_PLACED` now asserted rather than discarded.** It was a junk grep that nothing read,
        while the PASS line printed the *requested* AI count. Now parsed from the `S=` field of
        `game options:` and the run FAILS if observed != requested. Parser validated against three
        real slot lists (1, 5 and 0 AI).
      - **`xplat-determinism-soak.sh` idle runs now FAIL** instead of warning-and-passing, matching
        its three siblings. **`xplat-lan-soak.sh` gained an idle gate and a floor at all** — it had
        neither, and it is the harness that produced the cited macOS↔Windows determinism proof.
- [ ] **62.** Long-haul soak on capacity-8 maps only. Measured throughput is **17 logic fps**
      networked, so a 30-min match is ~53 min wall and 50 full matches is ~44 h — size accordingly.
      `scripts/test/xplat-3platform-soak.sh`, `ONLY_MAP=` to pick the rotation.
