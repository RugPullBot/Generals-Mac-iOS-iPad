# Plan: 8-player crossplay through the relay

**Goal:** a full 8-slot lobby playing together across **iOS, iPadOS, macOS and Windows x64**,
through the relay, off-LAN. Linux is the *server* platform, not a client target.

Status of the ground beneath this: the sim is proven identical across macOS/Linux (800 frames solo,
byte for byte) and macOS/Windows (3000 frames LAN). A full match now runs mac↔linux over the relay,
1200 frames identical with 1200 distinct CRCs. So determinism is not the risk here; **peer identity
is**.

## What actually blocks 8 players

**1. The relay caps a room at two, and evicts.**
`tools/relay/relay.js:36` `MEMBERS_PER_ROOM = 2`, and a third registration does not get refused —
`relay.js:98` evicts the least-recently-seen member to make space. So player 3 silently kicks
player 1 off the relay. Everything else about the relay is already N-ready: `forward()` loops over
every member and skips only the sender.

**2. The client collapses every peer into one identity. This is the real work.**
`Transport.cpp` stamps every relayed datagram as coming from `m_peerVirtualIP` — a single
configured address — in both the live path and the latency-sim path. The comment states the
assumption outright: *"Only two machines can be in a room, so the peer is unambiguous."* With 8
players every packet would be attributed to the same peer and the lobby cannot tell who sent what.

**3. Nothing tells the relay who a packet is FOR.**
In relay mode the client overwrites the destination with the relay's address, so the relay can only
broadcast to the whole room. Fine for lockstep command traffic, wrong for the unicast paths — join
handshake, map transfer — where 7 peers would receive traffic addressed to someone else.

## Design

Give the relay a real (src, dst) to work with, by prepending a small header **outside** the game
payload. The relay stays payload-blind: it reads only its own header and never parses, decrypts or
rewrites game bytes.

```
[ magic 'GXR1' (4) ][ src virtual IP (4) ][ dst virtual IP (4) ][ ...unchanged game datagram... ]
```

* **Client send:** prepend the header. `dst` is the address the game already chose
  (`m_outBuffer[i].addr`), `src` is our own virtual IP.
* **Relay:** look up `dst` in the room. Unicast if it names a member; forward to all others if it is
  a broadcast address. Never inspect past byte 12.
* **Client receive:** strip the header and report `src` as the source address, replacing the
  `m_peerVirtualIP` substitution.

**Why this is small:** every peer already advertises its virtual IP in the slot list, so the game is
*already* addressing peers by virtual address. The relay becomes an L3 switch for a virtual /24 and
the layers above keep seeing the two-machine LAN they were written for — extended to eight.
`PeerVirtualIP` stops being needed for routing; only `LocalVirtualIP` remains as identity.

### The constraint that bites: packet budget

`MAX_UDP_PAYLOAD_SIZE = 1100`, chosen deliberately low because mobile MTUs run 1340-1500 before
PPPoE/IPv6 encapsulation — i.e. chosen for exactly the iOS/iPadOS clients that are the point of this
work. The on-wire size is already `length + sizeof(TransportMessageHeader)`, so 12 header bytes
must come **out of** the payload budget, never on top of it. Adding them naively fragments packets
on cellular and the failure would look like random desyncs on iPhone only.

## Tasks

- [x] 1. Relay: `MEMBERS_PER_ROOM` 2 -> 8; refuse an over-full room instead of evicting a live
      player, and log the refusal. Eviction stays only for genuinely timed-out members.
- [x] 2. Relay: parse the `GXR1` header, unicast on a known `dst`, broadcast on a broadcast `dst`,
      drop with a counter on an unknown one.
- [x] 3. Client: prepend the header in `Transport::doSend`, at the socket-write layer so it sits
      outside the CRC/encryption, the same layer `sendRelayRegistration` already writes at.
- [x] 4. Client: strip the header in `doRecv` and use `src` as the reported address. **Both** the
      live path and the `RTS_DEBUG` latency-sim path — they are separate copies today and the
      existing relay substitution had to be applied twice.
- [x] 5. Client: reduce the effective payload cap by the header size when relay mode is on, and
      assert the on-wire size never exceeds `MAX_UDP_PAYLOAD_SIZE`.
- [x] 6. Drop `PeerVirtualIP` from the routing path; keep `LocalVirtualIP` as identity. Update the
      relay-mode gate in `initRelay` and `NetworkDirectConnect` accordingly.
- [x] 7. Update `tools/relay/README.md`: N-player config, and the two footguns found today —
      the relay and a relay-mode peer cannot share a host, and virtual IPs must be inside 10/8.

## How this gets verified (not "it compiles")

- [ ] 8. **8 peers on the VPS, one per network namespace.** A netns has its own port table, so each
      peer binds 8086/8088 independently — this is how to test 8 clients without 8 machines, and it
      is already proven: today's linux peer ran in one. Multi-instance on a single box will NOT
      work, since `initRelay` disables relay mode for multi-instance clients.
- [x] 9. Diff all 8 `[GXCRC]` streams pairwise, with the controls that make it mean something:
      distinct-CRC count (a 3-distinct idle run "passed" today and proved nothing) and the
      serialised slot list showing the real player mix.
- [ ] 10. Then the real matrix: macOS + Windows x64 + iPadOS + iOS in one lobby through the relay.
      Windows needs `C:\dev\GeneralsX` synced (it is ~10 commits behind) and rebuilt; iPad and
      iPhone need reinstalling, since `sourceID` has moved and a stale build cannot join.

---

# Phase 2: a server list, served by the relay

**Why this exists:** the Online tab dials `peerchat.gamespy.com` and `gamestats.gamespy.com`.
GameSpy shut down in 2014, so "CANNOT CONNECT" is the correct result and always will be. There is
no hostname override in the tree. A server list therefore needs a backend we control, and the relay
is already most of one — it tracks rooms and knows who is in them. This makes it the third role the
design doc predicted: NAT relay, desync arbiter, **and** matchmaker.

## The thing that shapes the design

Today a room *is* a game: one room token, hardcoded in `Options.ini`, one match. A browsable list
means **many rooms on one relay**, and a client that can pick one at runtime. So:

* The relay has to distinguish an **advertised** (listed) room from a private one, or every private
  game leaks into the browser.
* The client has to be able to **change room after startup**. `initRelay` prebuilds the registration
  datagram once, so this needs a real (small) change rather than a config edit.

## Protocol — plaintext, same shape as `GXRLY`

The relay stays payload-blind; these are control datagrams on 8086, recognised by their tag alone.

```
GXADV <room> <hostVirtualIP> <players>/<slots> <name>|<map>   host -> relay, every 5s
GXLIST                                                        client -> relay
GXGAME <room> <hostVirtualIP> <players>/<slots> <name>|<map>  relay -> client, one per game
```

A room with no `GXADV` in the last interval is simply not listed — no separate teardown, and a host
that crashes drops off the list on its own.

## Tasks

- [x] 11. Relay: track advertisements per room, expire them on the existing sweep, answer `GXLIST`.
- [x] 12. Relay: refuse a **duplicate** virtual IP within a room, the same way a full room is
      refused. Today two players sharing a `LocalVirtualIP` look to the relay like one member that
      keeps moving, which is the single most likely way to break a lobby and is currently invisible.
- [ ] 13. Client: `Transport::setRelayRoom()` — rebuild the registration at runtime so a browser
      selection can switch rooms without an `Options.ini` edit and a restart.
- [ ] 14. Client: send `GXADV` while hosting, from the same place the registration keepalive is
      sent, so it inherits the NAT-keepalive cadence for free.
- [ ] 15. Client: query `GXLIST` and populate the **Direct Connect** screen's remote list with live
      games. Direct Connect, not the Online tab: the Online screens are wired through the GameSpy
      peer/staging-room objects, and re-pointing them is a far larger job than the browser itself.
- [ ] 16. Verify: two concurrent games on one relay, each listed, each joinable, neither seeing the
      other's traffic. Then the 8-man matrix.

## Known limit of this phase, stated up front

`LocalVirtualIP` is still configured per machine. That is fine for a known group of devices and
**not** fine for public matchmaking between strangers, where two players will eventually both be
`10.42.0.1`. Task 12 turns that from a silent lobby corruption into a refusal, which is the honest
interim. Properly fixing it means the relay **assigns** the virtual address on join, and that is a
deeper change: identity is currently decided before registration and is threaded through
`LANAPI::m_localIP`, `GameInfo`'s local IP and slot matching. Deliberately not in this phase.

## Risks

* **Protocol break.** The 12-byte header is incompatible with the current 2-player relay. Nothing is
  released, so this is a clean break, but both ends must ship together.
* **The mobile packet budget above** is the one that would hide, because it would only show on
  cellular iOS.
* **8-way is not 2-way with a bigger number.** Lockstep waits for the slowest peer, so a phone on a
  bad link stalls all eight. Out of scope here, but it is the next thing that will hurt.
