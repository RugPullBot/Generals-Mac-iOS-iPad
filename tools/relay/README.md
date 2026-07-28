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
traffic. It understands exactly two things.

First, a plaintext line the client repeats every 5 seconds,

```
GXRLY <room> <client-id>
```

which is how it learns each player's public address (and keeps the NAT mapping open). The
client-id is that machine's virtual address.

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

Security model: whoever knows the room token gets paired with whoever else is in that room, so
pick a random one. There is no authentication and the relay adds no encryption — it is the same
traffic the game would have put on a LAN.

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
MemoryMax=128M

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

| Variable            | Default     | Meaning                                              |
| ------------------- | ----------- | ---------------------------------------------------- |
| `RELAY_PORTS`       | `8086,8088` | Ports to listen on. Must match the game's ports.      |
| `RELAY_TIMEOUT_MS`  | `60000`     | Drop a player who has been silent this long.          |
| `RELAY_STATS_MS`    | `300000`    | How often to log a traffic summary.                   |

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
  single most likely way to break a lobby, and the relay cannot detect it — to the relay they are
  one member that keeps changing address.
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
