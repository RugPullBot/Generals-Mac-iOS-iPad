# Networking and LAN multiplayer

What the LAN stack does, which peers can actually play each other, and the POSIX
socket defects that made every failure in it invisible.

Written 2026-07-26 against commit `962c538d4` plus the `udp.cpp` / `Transport.cpp`
changes described in §4. Line numbers drift — grep the quoted identifier.

---

## 1. The short version

| Pairing | Works? | Why |
|---|---|---|
| iPad (this fork) ↔ Windows retail 1.04 | **No** | Different simulation math. Not fixable from this repo. |
| iPad ↔ 32-bit Windows build of this fork | **No** | Word-size divergence in the CRC + x87 asm paths |
| iPad ↔ macOS, both this fork | **Yes**, via Direct Connect | Same arch, same libm, same word size |
| LAN lobby *browser* on Darwin | **No** | Limited broadcast fails; see §5 |

Direct Connect is fully unicast, so the working path needs no broadcast, no
multicast entitlement, and no `Info.plist` change.

## 2. Why cross-architecture lockstep fails

The game is lockstep: it exchanges **orders, not state**, so every peer must
compute bit-identical logic frames for the entire match. Two facts make that
impossible against a retail x86 binary.

**The math differs.** `Core/Libraries/Source/WWVegas/WWMath/wwmath.h` has seven
`#if defined(_MSC_VER) && defined(_M_IX86)` gates (lines 110, 425, 446, 460, 482,
672, 720). The decisive one is `WWMath::Inv_Sqrt`: the x86 branch is hand-written
x87 assembly using a magic-constant seed plus three Newton-Raphson iterations —
an **approximation** — while the portable branch is `return 1.0f / (float)sqrt(val);`,
a correctly-rounded result. `Vector3::Normalize` is built on it, and it reaches the
per-unit-per-frame movement path through `tryToRotateVector3D`
(`GeneralsMD/…/GameLogic/Object/Locomotor.cpp:111`) and `angleBetweenVectors`
(`PhysicsUpdate.cpp`).

**The CRC has no tolerance.** `Object::crc`
(`GeneralsMD/…/GameLogic/Object/Object.cpp:3980`, at line 4007) does:

```cpp
xfer->xferUser((Matrix3D *)getTransformMatrix(), sizeof(Matrix3D));
```

48 raw bytes of transform matrix, hashed directly. One ULP of difference in any
float diverges the CRC. `NET_CRC_INTERVAL = 100` (`Network.cpp:59`), so the
mismatch dialog (`Network::setSawCRCMismatch`, `Network.cpp:376`) fires within
about a hundred logic frames of the first turning unit.

**The escape hatch is a stub.** Every `USE_DETERMINISTIC_MATH` branch in
`wwmath.h:150-232` is a `// TODO: return GameMath::Acos(x);` comment followed by
the same call as the `#else`. `SAGE_USE_DETERMINISTIC_MATH` is OFF in
`cmake/gamemath.cmake:23`. **Enabling it changes nothing today.** Do not plan
around it.

Two things that are *not* the problem, so they don't need investigating again:

- **FMA contraction is off.** All 1447 entries in
  `build/ios-vulkan/compile_commands.json` carry `-ffp-contract=off`
  (`cmake/compilers.cmake:58`).
- **The RNG is fine.** `randomValue(UnsignedInt (&seed)[6])` in
  `Core/GameEngine/Source/Common/RandomValue.cpp` is a pure integer
  add-with-carry, seeded from the host over the wire.

## 3. This fork's own ungated divergences

Even fork-to-fork, these change the logic frame and are **not** behind
`RETAIL_COMPATIBLE_CRC`. Harmless when both peers are this build; fatal against
anything else.

- `OverlordContain::syncPortablePosition`
  (`GeneralsMD/…/GameLogic/Object/Contain/OverlordContain.cpp:117`) rewrites the
  contained structure's position and orientation every frame. The transform
  matrix *is* hashed, so this is a confirmed live desync source — any China
  Overlord with a Gattling/Propaganda/Bunker.
- `ACos`/`ASin` rerouted from retail's `acosf`/`asinf` to `(float)acos`/`(float)asin`
  (`Common/System/Trig.cpp` → `WWMath::ACosTrig`, wwmath.h:628).
- `REAL_TO_INT(x)` is now `((Int)(x))` (`Core/Libraries/Include/Lib/BaseType.h:214`);
  retail used `fast_float2long_round(fast_float_trunc(x))`. Equivalent in range,
  divergent for negative→unsigned and out-of-range. Reached from
  `PartitionManager.cpp` threat/cash cells.

There are 44 GeneralsX-marked changes in `GameLogic` alone across 23 files. Three
were audited. Assume more.

**Word size:** `BitFlags<NUMBITS>::xfer` (`Common/BitFlagsIO.h:218`) hashes
`sizeof(this)` — a *pointer* — on the `RETAIL_COMPATIBLE_CRC` path. That is
bug-compatibility with retail, which was 32-bit, so it hashes 4 bytes there and 8
here. Reached from `Player::crc` (`Player.cpp:4032`) whenever a player has active
battle-plan bonuses (USA Strategy Center). Irrelevant between two arm64 peers;
fatal against any 32-bit build, which is every Windows preset in
`CMakePresets.json` (`vc6*`, `win32*`, `mingw-w64-i686*`).

## 4. The POSIX socket defects (fixed)

Every assignment to `m_lastError` in
[`Core/GameEngine/Source/GameNetwork/udp.cpp`](../../Core/GameEngine/Source/GameNetwork/udp.cpp)
sat inside `#ifdef _WIN32`. On POSIX, `UDP::GetStatus()` therefore read a
permanently-zero field and returned `OK` regardless of what the socket did.

The worst consequence was not a silent failure but a **hang**. `UDP::Bind` did:

```cpp
if (retval==-1) { status=GetStatus(); return(status); }   // status == OK on POSIX
...
retval=SetBlocking(FALSE);                                 // never reached
```

So a failed bind returned success *and* skipped the non-blocking switch. Transport
received a **blocking** socket it believed was healthy, and `Transport::doRecv`'s
`while ((len = m_udpsock->Read(...)) > 0)` parked the main thread in `recvfrom()`.
**The symptom is the game freezing when you open the LAN screen** — which reads as
a renderer stall, not a network fault.

Everything downstream inherited the blindness: `Transport::init`
(`Transport.cpp:115`) left its retry loop immediately and returned true,
`LANAPI::SetLocalIP` returned TRUE, `LanLobbyMenuInit`
(`…/Menus/LanLobbyMenu.cpp:463`) never set `LANSocketErrorDetected`, and the
`GUI:SocketError` dialog was unreachable.

Fixed by:

1. `UDP::mapPosixReadError()` — maps `EAGAIN`/`EWOULDBLOCK`/`EINTR` to a `0`
   return, matching the Windows `WSAEWOULDBLOCK` remap, and records `errno` for
   everything else. **This ordering is load-bearing**: POSIX `recvfrom` reports an
   idle non-blocking socket as `-1`/`EAGAIN`, so recording `errno` without
   filtering the drain case first makes `Transport::doRecv` return FALSE on every
   idle tick and trips `LANSocketErrorDetected` every frame.
2. `errno` captured in `Bind` and `Write`; `SetBlocking(FALSE)` hoisted above the
   bind so no socket is ever left blocking; an unmapped errno coerced to `UNKNOWN`
   rather than `OK`; a diagnostic line naming the IP and port.
3. `EADDRNOTAVAIL`, `EADDRINUSE` and `EPIPE` added to the POSIX `GetStatus` switch.
   `EADDRNOTAVAIL` is the common one — a stale `Options.ini` `IPAddress` naming an
   interface that no longer exists.
4. `Transport::doSend` (`Transport.cpp:239`) now clears the queue slot on a hard
   error instead of retaining it. Retaining an undeliverable message pinned one of
   `MAX_MESSAGES = 256` slots for the life of the process.

## 5. LAN discovery is dead on Darwin

`LANAPI::LANAPI` (`LANAPI.cpp:84`) sets `m_broadcastAddr = INADDR_BROADCAST` and
`LANAPI::sendMessage` (`LANAPI.cpp:203`) queues to it on `lobbyPort = 8086`. Both
directions fail:

- **Send:** `sendto(255.255.255.255)` returns `EHOSTUNREACH` on this Mac
  regardless of `SO_BROADCAST`, `SO_DONTROUTE` or `IP_BOUND_IF`. A
  subnet-directed `192.168.10.255` succeeds. *Caveat: measured on a dual-homed
  Mac with reject routes for 255.255.255.255/32; a single-homed machine may
  differ.*
- **Receive:** `Transport::init` binds `m_localIP`, and a unicast-bound socket
  never receives a broadcast. An `INADDR_ANY`-bound socket does. This result is
  not config-dependent.

Fixing it means deriving the subnet broadcast from `getifaddrs`' `ifa_broadaddr`
and binding `INADDR_ANY`, but `IPEnumeration::getAddresses`
(`IPEnumeration.cpp:115`) reads only `ifa->ifa_addr` and `EnumeratedIP` has no
broadcast field — real plumbing, not a one-liner. On iOS it may still be gated by
`com.apple.developer.networking.multicast`, which a sideload profile cannot carry.
**Use Direct Connect instead; it needs none of this.**

## 6. Setting up an iPad ↔ macOS game

iPad hosts (it emits nothing until contacted), Mac joins by IP.

1. **Pin the local IP on both machines.** `IPEnumeration::addNewIP`
   (`IPEnumeration.cpp:177`) sorts ascending and both screens take the *head* —
   the numerically lowest address. A VPN `utun`, a hotspot bridge at 172.20.10.1,
   or a 169.254 self-assigned address can outrank Wi-Fi. Set **both** Options.ini
   keys, because the two screens read different ones:
   - `IPAddress=<wifi ip>` → `LanLobbyMenuInit` (`LanLobbyMenu.cpp:420`)
   - `GameSpyIPAddress=<wifi ip>` → `NetworkDirectConnectInit`
     (`NetworkDirectConnect.cpp:307`)
2. **Host from Direct Connect, not the LAN lobby.** `NetworkDirectConnect.cpp:202`
   calls `RequestGameCreate(localIPString, TRUE)`; the lobby
   (`LanLobbyMenu.cpp:785`) passes FALSE, after which every lobby message reverts
   to broadcast and dies.
3. **Player names must not contain `,` `:` or `;`.** `ContainsInvalidChars`
   (`LANAPIhandlers.cpp:300`) is called unconditionally at `LANAPIhandlers.cpp:439`,
   *outside* the `#if !RTS_ZEROHOUR` block, and replies `RET_DUPLICATE_NAME` —
   which reads as a confusing "name taken".
4. **Expect the first join to time out.** `m_actionTimeout = 5000` ms
   (`LANAPI.cpp:86`) and the triggering datagram is dropped while iOS shows its
   local-network consent prompt. Grant, then retry.
5. Same L2 segment, AP client isolation off.

## 7. Gotchas

**A silent socket failure presents as a graphics hang.** See §4. Before blaming
the renderer for a freeze on a menu that touches the network, check whether a
socket went blocking.

**Two different Options.ini keys select the local IP** depending on which screen
you entered from. Setting only one gets you two peers bound to different
interfaces with no error anywhere.

**`percentEncodeMapName` is host-only and asymmetric.** `GameInfoToAsciiString`
(`GameInfo.cpp:933`) encodes the map path — every stock ZH map has a space, so
`maps/alpine assault` goes out as `maps/alpine%20assault`. `percentDecodeMapName`
(`GameInfo.cpp:1136`) is a no-op on unencoded names, so a non-fork peer joining
*our* host resolves a nonexistent path and shows as not-having-the-map. Us joining
*his* host is fine.

**In-game chat is 4 bytes per character from Apple targets, 2 from Windows.**
`network::writeStringWithoutNull` (`NetPacketStructs.h:88`) uses `sizeof(WideChar)`.
The lobby protocol was fixed for this (`typedef uint16_t WideCharWindows`,
`LANAPI.h:146`); the in-game NetPacket protocol was not. The receiver's byte offset
drifts *mid-packet*, so the rest of that packet's commands are misparsed, not just
the text. Irrelevant between two Apple targets.

**`CompressionManager` is not on the LAN wire.** Every call in
`ConnectionManager.cpp` is inside `#ifdef COMPRESS_TARGAS`, which is defined
nowhere in the tree.

**Retail does not CRC-deny the join.** The `exeCRC`/`iniCRC` check is
`#if !RTS_ZEROHOUR` (`LANAPIhandlers.cpp:376`) and is commented out in the retail
EA source. Neither side refuses on version grounds — the failure is the *runtime*
sim CRC, much later.

**The lobby wire format itself is portable.** `LANMessage` is
`#pragma pack(push,1)`, all fixed-width, `WideCharWindows = uint16_t`; on arm64
`sizeof == 471`, `alignof == 1`, union at offset 34 — identical to MSVC. Transport
framing (`0xF00D`, 476-byte packets, `encryptBuf` XOR + `htonl`) is retail-identical,
ports 8086/8088 match. The wire is not the problem; the simulation is.

## 8. Open questions

| Unknown | Cheapest test |
|---|---|
| Do Apple's arm64 libm results match between macOS 26 and iPadOS 26? Decides whether iPad↔Mac works at all. | Wire `SimulationMathCrc::calculate()` (`Common/Diagnostic/SimulationMathCrc.cpp:62` — **zero callers today**) into a startup log line and compare the two numbers by eye. One build, no gameplay. |
| Do the two hold lockstep for a full game? | Run 10 minutes with `-crcinterval 1` and Overlords + aircraft on the field. |
| Does the Direct Connect screen exist and take touch input on iPad? | Neither `LanLobbyMenu.wnd` nor `Menus/NetworkDirectConnect.wnd` is in this repo — they live in `Window.big`, and a missing button silently just isn't drawn. Open the screen and look. |
| Is `EHOSTUNREACH` on limited broadcast universal on Darwin, or an artifact of this Mac's dual default routes? | Only matters if the lobby browser is ever pursued. |
