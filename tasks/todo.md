# Plan: public crossplay through the relay

**Goal:** strangers on **iOS, iPadOS, macOS and Windows x64** find each other in a browser, fill an
8-slot lobby and play, off-LAN, with nothing hand-configured. Linux is the *server* platform, not a
client target.

**Where this stands (2026-07-29, HEAD `508d6c563`):** a human has played a real match — macOS +
Windows + a headless Linux host, through the relay, over the internet, 14,756 frames, byte-identical
streams, 14,757 distinct CRC values, zero mismatches. Determinism is not the risk and neither is
transport. What is left is **matchmaking hygiene, the mobile clients, and the things nobody has
measured yet.** Full detail in `docs/WORKDIR/STATE_2026-07-29_session6.md`.

---

# Phase 1: eight peers in a room — DONE except the 8-peer run

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
- [ ] 11. **8 peers in one room, one per network namespace on the VPS.** Still the largest untested
      number: the biggest real game so far is 3. A netns has its own port table so each peer binds
      8086/8088 independently; multi-instance on one box will not work, since `initRelay` disables
      relay mode for multi-instance clients on macOS/Linux.

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
      committed evidence that the dropdown renders a live list, that picking a row joins, or that a
      host is correctly absent from its own list.
- [ ] 21. **Two concurrent games on one relay** — each listed, each joinable, neither seeing the
      other's traffic. Blocked on task 23; today they would land in the same room.

# Phase 3: identities and rooms — half built, ZERO real-match proof

- [x] 22. Relay + client: `GXWHO` / `GXYOU`, sticky per endpoint, excluding already-registered
      addresses, released on the sweep. Wired into the headless driver, the Direct Connect screen and
      the relay-mode gate. Covered by `test-lobby.js` 11/11.
      (`973aed2d4`, `d87460c65`, `6a2b7482e`)
- [ ] 23. **Wire `Transport::setRelayRoom`. It has zero call sites.** It is implemented, commented as
      "replaced later by `setRelayRoom` when a game is picked out of the browser", and nothing calls
      it. `RelayGameListing` parses a `room` field and discards it. Consequence: every client is in
      `prefs.getRelayRoom()` or the literal `"default"`, so **one relay is one game at a time** and
      two groups today corrupt each other's lobby. Needs (a) a host generating a unique room token,
      (b) `GXLIST` returning games across rooms, (c) the browser calling `setRelayRoom` before dialling.
- [ ] 24. **Play one match with no `LocalVirtualIP` configured on any machine.** The 14,756-frame
      match ran rev 2157 with addresses allocated *by hand*; assignment landed four commits later and
      has never been through a lobby. This is the single highest-value next run.
- [ ] 25. **Click the Online button in a built binary.** `508d6c563` has never been run.

# Phase 4: the client matrix

- [ ] 26. **iPad Air M3** — four commits behind (engine binary 02:45, package 02:46, both before
      `973aed2d4` at 02:51). `sourceID` has moved, so it cannot join at all, and it predates identity
      assignment, so it could not matchmake if it could. Rebuild the `ios-vulkan` preset **first**,
      then `./scripts/build/ios/package-ios-zh.sh --install` over USB — the script does **not** build
      the engine, it copies whatever binary is already on disk.
- [ ] 27. **iPhone** — not built, not installed, not tested. iOS is why `MAX_UDP_PAYLOAD_SIZE` is 1100;
      that constraint has never been exercised on a real cellular link.
- [ ] 28. **The real matrix in one lobby:** macOS + Windows x64 + iPadOS + iOS through the relay.
- [ ] 29. Commit a **Windows** CRC stream for a multi-platform played match. Windows was in the
      14,756-frame match but only macOS and Linux streams were compared.

# Phase 5: the things nobody has measured

- [ ] 30. **Map transfer over the relay.** Never tested. Every match so far used the same built-in map
      already present on both sides. Unicast routing exists specifically for this path.
- [ ] 31. **Latency, jitter and stall behaviour**, especially with a phone on cellular. Lockstep waits
      for the slowest peer, so one bad link stalls all eight. No measurement of any kind exists.
- [ ] 32. **Names, accounts, moderation.** Registration is `GXRLY <room> <virtualIP>` with no auth:
      whoever knows a room token is in that room. No kick, no ban, no report, no join rate limit.
- [ ] 33. **The real Online UI.** The Online button opens the Direct Connect layout — no chat, no
      staging room, no player list. Driving the original screens means reimplementing the GameSpy peer
      and staging-room objects. Generals Online is the prior art.

# Phase 6: cleanups that are now more likely to bite

- [ ] 34. **AWOL players block victory.** `hasSinglePlayerBeenDefeated` (`VictoryConditions.cpp:328`)
      is purely asset-based and never consults connection state, so "Exit to lobby" leaves your base
      standing and nobody can win. **CAUTION:** `VictoryConditions::update` calls `p->killPlayer()`, so
      any fix mutates simulation state and must use state all peers agree on or it desyncs.
- [ ] 35. Defeated observers are never pulled to the score screen.
- [ ] 36. Windows exits `0xC0000005` at the **end** of a headless run, after all frames and CRCs are
      written. Cosmetic today, a real crash regardless.
- [ ] 37. Extend the data fingerprint to cover loose simulation-relevant files — `.scb` is not INI, so
      it is outside `m_iniCRC`, and two peers reported identical digests while simulating different
      games. Turns a silent desync into a refused join.
- [ ] 38. `SDL3GameEngine::createAudioManager(Bool dummy)` does `(void)dummy;` — headless on Mac/Linux
      runs the full OpenAL backend. Win32 returns `MilesAudioManagerDummy`; there is no OpenAL dummy.
- [ ] 39. `Generals/` (base game): no headless CLI, and the `LANEnableStartButton` null-deref fixed in
      `GeneralsMD/` is still live there.
- [ ] 40. Terrain draw-window fix is still **not visually verified** — zoom out and look.

---

## Risks that have not gone away

* **8-way is not 2-way with a bigger number.** Lockstep waits for the slowest peer (task 31).
* **The mobile packet budget hides.** An oversized packet fragments and presents as a desync that only
  ever reproduces on cellular. The 12-byte header came out of the 1100-byte budget for this reason;
  nothing has confirmed it on a real phone.
* **Maphack is unsolvable in lockstep** — every client must know the whole world to simulate it. Catch
  it behaviourally by re-simulating replays, not by trying to prevent it.
* **Never use burstable CPU for the relay.** A throttled sim does not degrade gracefully, it stalls the
  match for everyone. Budget 30-40% of one *dedicated* core per heavy match.

---

# Phase 4: a real lobby UI — reuse the stock WOL screens, do not build one

**The finding that shapes this:** the multi-column lobby everyone recognises from Generals Online
is not something they built. It is Zero Hour's own `WOLCustomLobby.wnd`, and it ships in our
archives already, unused. Confirmed present alongside `WOLWelcomeMenu.wnd`,
`WOLQuickMatchMenu.wnd`, `WOLMapSelectMenu.wnd` and `WOLBuddyOverlay.wnd`.

So the work is **not** designing a UI. It is feeding the existing one from our relay instead of
from GameSpy. `WOLLobbyMenu.cpp` already drives that screen; it populates the game list from
`TheGameSpyInfo->getStagingRoomList()` / `hasStagingRoomListChanged()` -> `RefreshGameListBoxes()`,
and the player list into `listboxLobbyPlayers`.

That is a far better deal than the current combo box, which cannot be made good: it is one line of
text in a box narrower than a row, so every improvement is a fight over which part gets truncated.
The row reorder in `67672581f` is a stopgap, and should be deleted the day this lands.

## Tasks

- [ ] 17. Point the Online button at `WOLCustomLobby.wnd` instead of the Direct Connect layout.
- [ ] 18. Populate the staging-room list from our relay's `GXGAME` replies rather than GameSpy peer
      callbacks. `GameSpyInfo` already exposes `addStagingRoom` / `updateStagingRoom` /
      `removeStagingRoom` (`PeerDefsImplementation.h`), so this is a data-source swap, not a
      rewrite. Map a `Transport::RelayGameListing` onto a staging room: host virtual IP, room,
      players/slots, name, map.
- [ ] 19. Wire Join to the room-switch flow that already exists in `JoinDirectConnectGame` — take
      the room from the selected listing, `enterRelayRoom`, rebuild `TheLAN`, `SetLocalIP`, dial.
      The ordering is not negotiable and is documented there.
- [ ] 20. Decide what the player list shows. GameSpy filled it from chat-room presence, which we do
      not have. Slot occupants of the selected game is the honest equivalent; a global player list
      needs the relay to track identities across rooms, which it deliberately does not.
- [ ] 21. Chat is a genuine gap. The relay carries no chat channel today. It is a small protocol
      addition (a plaintext line like the others) but it is new surface, and unauthenticated chat
      on a public relay is a moderation problem before it is a feature.

## What NOT to do

Do not reimplement the GameSpy peer/staging-room protocol to drive these screens "properly". The
screens do not care where their rows come from. Reimplementing GameSpy is the large, faithful,
pointless version of this task.
