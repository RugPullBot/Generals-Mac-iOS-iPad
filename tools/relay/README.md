# GeneralsX UDP relay

Lets machines on different networks play a Direct Connect game without any of them forwarding a
port. All game UDP goes to a relay you run on a VPS, which forwards it to the other players. One
room holds **up to 8 players** — a full Zero Hour lobby — across any mix of macOS, Windows, iOS
and iPadOS.

## How it works

Each machine is given a **virtual LAN identity** — `10.42.0.1`, `10.42.0.2`, … `10.42.0.8`. The
game's transport layer sends every datagram to the relay instead of to the real destination, and
reports every datagram it receives as having come from the sending peer's virtual address.
Everything above the transport layer — the lobby, the slot list, the join handshake, the in-game
connections — therefore sees the LAN it was written for, and needed no changes at all.

The relay is deliberately dumb and **payload-blind**. It never parses, decrypts or rewrites game
traffic. It understands a handful of short plaintext lines, and one binary header.

First, a plaintext line the client repeats every 5 seconds,

```
GXRLY <room> <client-id>
```

which is how it learns each player's public address (and keeps the NAT mapping open). The
client-id is that machine's virtual address.

An endpoint is in **one room at a time**. This is not a policy, it is forced by the wire format: a
relayed game packet carries a source and a destination virtual address but no room, so the sender's
address is the only thing that says which room the packet belongs to. Registering into a different
room is therefore how a client says it has left the previous one, which is exactly what a browser
does when it lists many games and joins one.

Second, a 12-byte routing header the client puts in front of every game packet:

```
[ 'GXR1' ][ source virtual IP ][ destination virtual IP ][ ...game packet, untouched... ]
```

The relay reads only that, and delivers the packet to the peer named in it — or to everyone in the
room if the destination is a broadcast address, which is what LAN discovery expects. Nothing else
is inspected.

That header is what allows more than two players. With two, "forward it to the other one" is
unambiguous; with eight it is not, and lockstep is only *mostly* broadcast — the join handshake and
the map transfer are point-to-point, and sending those to six uninvolved peers is both wasteful and
wrong.

The port is always preserved, which is what lets one relay carry both conversations: **8086** is
the lobby, **8088** is the in-game connection. Both must be open.

The header is taken **out of** the packet budget rather than added on top, so a relayed datagram is
never larger than a LAN one. That matters because the payload cap is set for mobile MTUs, and iOS
and iPadOS are target clients: an oversized packet would fragment, and a fragmented lockstep packet
would look like a desync that only ever happens on cellular.

### The lobby side

The relay is also the only thing that knows which games exist and who is in them, so it is the
matchmaker too. Three more plaintext lines carry that, on the lobby port:

```
GXADV <room> <hostVirtualIP> <players> <slots> <name>|<map>   host  -> relay, every 5s
GXLIST                                                        client-> relay
GXGAME <room> <hostVirtualIP> <players> <slots> <name>|<map>  relay -> client, one per game

GXWHO <room>                                                  client-> relay: who am I here?
GXYOU <room> <virtualIP>                                      relay -> client: you are 10.42.0.N
GXYOU <room> -                                                relay -> client: no address free

GXCHT <room> <text>                                           client-> relay: say this
GXSAY <room> <senderVirtualIP> <text>                         relay -> everyone else in that room
```

A game is listed **only while its host keeps advertising**. There is no teardown message and none
is wanted: a host that crashes or loses its link simply stops, and drops off the list. A room that
never advertises is private and never appears — that is what keeps a friends' game out of a public
browser.

`GXWHO` is what makes public matchmaking possible: the relay hands out the virtual addresses, so
players no longer have to agree on them by hand. Assignment is per room — two rooms are separate
address spaces, and the same client may hold an address in each — sticky per endpoint so a
retransmitted request does not burn a second slot, and released when the client goes quiet.

`GXCHT` is the lobby chat box. The relay **stamps the sender itself**, from its registration table —
the line carries no sender field, because a client that could name itself could name anybody, and
the obvious use of an unauthenticated line echoed to seven other players is to speak as the host.
The `<room>` argument is the client saying which room it believes it is in; a client that is not
registered there is refused rather than having its message quietly delivered somewhere it did not
intend. Text is cut to 100 characters (what the game's own chat field holds), control bytes are
replaced with spaces, and each player gets a burst of 5 messages then one every two seconds. The
relay never writes chat text to its log — the counters say how much chat there was, nothing says
what was in it.

An advertisement is accepted **only from an endpoint registered in the room it advertises**, and it
cannot overwrite a listing held by a different, still-live endpoint. Without that rule, `GXADV` is
an unauthenticated write to a global table: anyone who guesses a room name could republish it with
a wrong host address, and everyone joining from the browser would dial a peer that is not there.

### Security model

Whoever knows the room token gets paired with whoever else is in that room, so pick a random one.
There is no authentication and the relay adds no encryption — it is the same traffic the game would
have put on a LAN.

What the relay does defend, because it is reachable by anyone who can send it a UDP datagram:

- **Every table has a ceiling.** Room names come from clients, and a client may invent a new one per
  packet. Reaching a ceiling does not refuse the newcomer — that would turn a memory bound into a
  lockout costing an attacker a few packets a second. The **least established** entry is given up
  instead: fewest members first, then quietest. A room with players in it is never the candidate
  while a room somebody named once and never returned to exists.
- **A reply burst is bounded.** `GXLIST` answers with one datagram per game, and a UDP source
  address is free to forge, so the reply is capped at `RELAY_MAX_LIST` games (the client only
  displays 32 anyway) and all lobby replies share a per-second ceiling.
- **The log is rate-limited.** This is the one that bites hardest: `console.log` to journald is
  asynchronous, so lines the reader cannot keep up with are queued *in memory*. A line per hostile
  packet is a memory-exhaustion primitive that needs no table at all — measured, six seconds of junk
  took the relay from 48 MB to 470 MB. Each kind of line now gets a burst of 20 and then one per
  five seconds, with the held-back count appended to the next one. Real volumes are in the periodic
  summary, which is a fixed number of lines however hard anyone pushes.
- **Anything a client can say is bounded before it is repeated.** Chat is the only place the relay
  echoes free text from one player to another, so it is length-capped, stripped of control bytes
  (a newline would forge a line in `journalctl`, an ESC would drive a terminal escape at whoever is
  reading it), rate-limited per player, and attributed by the relay rather than by the sender. The
  rate-limit bucket lives on the member record, so it inherits the room's ceiling instead of being
  a new table keyed on something an attacker chose.
- **A failed send never takes the relay down.** Every reply carries an error callback, so an
  `ENETUNREACH` while answering a stray datagram is a log line rather than an exit that would drop
  both ports and every game on them.

None of this makes an unauthenticated UDP service safe to expose carelessly; it makes the relay
survive being found, and keeps a flood aimed at it from becoming a flood aimed at someone else.

## Deploy on an Ubuntu VPS

Node 18 or newer; no dependencies, no `npm install`.

```bash
# --- on the VPS ---
sudo apt update && sudo apt install -y nodejs
sudo adduser --system --no-create-home --group gxrelay
sudo install -d -o gxrelay -g gxrelay /opt/gxrelay
```

```bash
# --- from your machine, in the repo root ---
scp tools/relay/relay.js root@YOUR.VPS.IP:/opt/gxrelay/relay.js
```

```bash
# --- on the VPS ---
sudo chown gxrelay:gxrelay /opt/gxrelay/relay.js

sudo tee /etc/systemd/system/gxrelay.service >/dev/null <<'EOF'
[Unit]
Description=GeneralsX UDP relay
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/usr/bin/node /opt/gxrelay/relay.js
Environment=RELAY_PORTS=8086,8088
User=gxrelay
Group=gxrelay
Restart=always
RestartSec=2
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
CapabilityBoundingSet=
AmbientCapabilities=
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX
MemoryMax=256M

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable --now gxrelay

sudo ufw allow 8086/udp
sudo ufw allow 8088/udp
```

Verify it is up and listening:

```bash
systemctl status gxrelay
sudo ss -lunp | grep -E '8086|8088'
journalctl -fu gxrelay          # watch players register while you test
```

If your provider has a firewall in front of the VPS (AWS security groups, Hetzner/Oracle cloud
firewall, etc.), open UDP 8086 and 8088 there too — `ufw` alone is not enough.

### Settings

Environment variables, all optional:

| Variable              | Default     | Meaning                                                        |
| --------------------- | ----------- | -------------------------------------------------------------- |
| `RELAY_PORTS`         | `8086,8088` | Ports to listen on. Must match the game's ports.                |
| `RELAY_TIMEOUT_MS`    | `60000`     | Drop a player who has been silent this long.                    |
| `RELAY_STATS_MS`      | `300000`    | How often to log a traffic summary.                             |
| `RELAY_MAX_ROOMS`     | `512`       | Ceiling on rooms, and on rooms tracked for identity assignment. |
| `RELAY_MAX_LIST`      | `32`        | Most games one `GXLIST` reply may carry.                        |
| `RELAY_REPLY_BUDGET`  | `1000`      | Lobby reply datagrams per second, across all of them.           |

The last three are ceilings, not tuning. `512` rooms is far more than one relay should ever host,
and `32` is what the client displays; if you are hitting either, something is wrong rather than
under-configured. Game traffic is never metered by `RELAY_REPLY_BUDGET` — a forwarded packet goes to
a peer that registered itself, not to an address someone claimed.

The unit above sets `MemoryMax=256M`, not the `128M` it used to. `128M` is plenty for ordinary
service — an idle relay is about 48 MB and a busy one barely moves — but a sustained flood pushes
V8's heap to roughly 120–180 MB of garbage-collector headroom, depending on the packet rate. That is
bounded and it comes back down, but under `128M` systemd would kill and restart the relay, dropping
every game in progress, exactly when it is being attacked. If you deployed the old unit file, raise
this and `systemctl daemon-reload`.

## Client setup

Both machines edit `Options.ini`:

- macOS: `~/Library/Application Support/GeneralsX/GeneralsZH/Options.ini`
- Linux: `~/.local/share/GeneralsX/GeneralsZH/Options.ini`
- Windows: `Documents\Command and Conquer Generals Zero Hour Data\Options.ini`

The **host**, and every machine, needs only its own identity:

```ini
RelayAddress = YOUR.VPS.IP
RelayRoom = pick-something-random
LocalVirtualIP = 10.42.0.1
```

Every other player: same relay, **same room**, a different `LocalVirtualIP` — `10.42.0.2`,
`10.42.0.3`, and so on up to eight. A **joiner** may also set

```ini
PeerVirtualIP = 10.42.0.1
```

which is purely a convenience: it is the host's address, offered at the top of the remote-IP list
on the Direct Connect screen so you do not have to type it the first time. It plays no part in
routing, and a host does not need it at all.

Rules for these values:

- `RelayAddress` is what turns relay mode on, together with `LocalVirtualIP`. Leave `RelayAddress`
  out and everything behaves as a plain LAN again. It may be a hostname or an IP.
- The virtual addresses **must be inside `10.0.0.0/8`**. This is not a style preference: the join
  path builds the address it dials with a signed shift, which is undefined behaviour for a first
  octet of 128 or more. `192.168.x.x` will not work correctly; `10.42.0.x` will.
- Every machine in a room needs a **different** `LocalVirtualIP`. Two machines sharing one is the
  single most likely way to break a lobby: to the relay they are one member whose address keeps
  changing. It cannot tell that apart from a NAT rebind on sight, but it does notice the pattern —
  a rebind settles, duplicates flap — and says so loudly in the log once the moves keep undoing each
  other. Letting the relay assign addresses (`GXWHO`) avoids the problem entirely.
- `RelayRoom` may only contain letters, digits, `.`, `-` and `_`, and is cut to 31 characters.
  Anything else is replaced with `_`. Everyone who shares a room plays together, so pick a random
  one.

### The relay cannot share a machine with a player

Run the relay on a box that is **not** playing. In relay mode the game binds the wildcard address
(a virtual address exists on no interface and can never be bound), so its socket collides with the
relay's own listener and the game dies with `UDP::Bind failed` on `0.0.0.0:8086`. If you need both
on one box for testing, put the game in a network namespace — it gets its own port table:

```bash
ip netns add gx
ip link add veth-gx type veth peer name veth-in
ip link set veth-in netns gx
ip addr add 10.200.0.1/24 dev veth-gx && ip link set veth-gx up
ip netns exec gx ip link set lo up
ip netns exec gx ip addr add 10.200.0.2/24 dev veth-in
ip netns exec gx ip link set veth-in up
ip netns exec gx ip route add default via 10.200.0.1
ip netns exec gx ./GeneralsXZH ...
```

This is also how to test more clients than you have machines: one namespace per peer. Running two
copies on one box the ordinary way will **not** work — multi-instance clients need a narrow
per-instance bind, and `initRelay` disables relay mode for them.

Then play: both go to **Network → Direct Connect**. The screen shows your virtual address as your
local IP. One player hosts; the other picks the host's virtual address (offered at the top of the
remote-IP list already) and joins.

## Troubleshooting

Watch `journalctl -fu gxrelay` while both players sit on the Direct Connect screen. You should see
four `joined` lines within about five seconds — each machine registers separately on 8086 and 8088:

```
[8086] room myroom: 10.42.0.1 joined from 203.0.113.9:8086 (1/2)
[8086] room myroom: 10.42.0.2 joined from 198.51.100.7:41022 (2/2)
[8088] room myroom: 10.42.0.1 joined from 203.0.113.9:8088 (1/2)
[8088] room myroom: 10.42.0.2 joined from 198.51.100.7:52310 (2/2)
```

| Symptom                                          | Cause                                                                                                            |
| ------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------- |
| No `joined` lines at all                         | UDP 8086/8088 not open on the VPS, or `RelayAddress` is wrong. Check the game log for `Transport::initRelay`.      |
| Only one machine's `joined` lines                | Only that machine has `RelayAddress` set.                                                                         |
| Two rooms in the log instead of one              | The `RelayRoom` values do not match.                                                                              |
| Lobby works, the game hangs on load              | Port 8088 is not open on the VPS — the lobby is 8086, the game itself is 8088.                                    |
| Joiner is accepted, then dropped from the lobby  | Two machines are using the same `LocalVirtualIP`. Every player needs a different one.                             |
| `REFUSED ... room is full`                       | A 9th client tried to join an 8-player room, or a stale member has not timed out yet (default 60 s).               |
| Game dies with `UDP::Bind failed` on `0.0.0.0`   | The relay is running on the same machine as the game and already holds the port. See above — use a namespace.      |
| Game log says relay disabled: multi-instance     | Multi-instance clients share a port and need a narrow local bind, which relay mode cannot use. Run a single copy. |
| `dropped N from unknown senders`                 | Normal in small numbers (traffic that arrives just before a registration, or internet background noise).           |
| `unrouted N` climbing steadily                   | Packets addressed to a peer this relay does not have — the players disagree about who is in the game.              |
| `headerless N` above zero                        | A client older than this relay is connected; it cannot be routed to a specific peer, only broadcast.               |
| `left for room X (same endpoint ...)`            | Normal. That player backed out of one lobby and joined another; one endpoint is in one room at a time.            |
| `REFUSED advert ... not registered in that room` | Someone advertised a room they are not in. Either a stale client that has not re-registered yet, or a griefer.     |
| `REFUSED advert ... listing is held by ...`      | Two hosts are claiming one room name. The one that got there first keeps it until it goes quiet.                  |
| `room X: evicted`                                | The room table hit `RELAY_MAX_ROOMS` and gave up its quietest, emptiest room. Almost always a flood of junk names. |
| `+N more like this were suppressed`              | Log rate limiting. Normal while something is flooding; the real counts are in the periodic summary.                |
| `REFUSED chat ... not registered in that room`   | Someone chatted into a room they are not in. A client that just switched rooms, or a forgery.                       |
| `dropping chat from 10.42.0.N`                   | That player is over the chat rate limit. Normal for a moment if someone holds a key; sustained means a flood.       |
| `dropping ... over N lobby replies/s`            | The lobby reply ceiling. Legitimate browsing never reaches it — assume a forged source address is being reflected. |
