# GeneralsX UDP relay

Lets two machines on different networks play a Direct Connect game without either one forwarding
a port. All game UDP goes to a relay you run on a VPS, which forwards it to the other player.

## How it works

Each machine is given a **virtual LAN identity** — `10.42.0.1` and `10.42.0.2`. The game's
transport layer sends every datagram to the relay instead of to the real destination, and reports
every datagram it receives as having come from the peer's virtual address. Everything above the
transport layer — the lobby, the slot list, the join handshake, the in-game connections — therefore
sees the two-machine LAN it was written for, and needed no changes at all.

The relay itself is deliberately dumb and **payload-blind**. It never parses, decrypts or rewrites
game traffic. It only understands one thing: a plaintext line the client repeats every 5 seconds,

```
GXRLY <room> <client-id>
```

which is how it learns each player's public address (and keeps the NAT mapping open). Anything
that is not such a line is copied verbatim to the other member of the same room, on the same port
it arrived on. Keeping the port is what lets one relay carry both conversations: **8086** is the
lobby, **8088** is the in-game connection. Both must be open.

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

Machine A:

```ini
RelayAddress = YOUR.VPS.IP
RelayRoom = pick-something-random
LocalVirtualIP = 10.42.0.1
PeerVirtualIP = 10.42.0.2
```

Machine B — same relay, **same room**, the two virtual addresses swapped:

```ini
RelayAddress = YOUR.VPS.IP
RelayRoom = pick-something-random
LocalVirtualIP = 10.42.0.2
PeerVirtualIP = 10.42.0.1
```

Rules for these values:

- `RelayAddress` is what turns relay mode on. Leave it out and everything behaves as a plain LAN
  again. It may be a hostname or an IP.
- The virtual addresses **must be inside `10.0.0.0/8`**. This is not a style preference: the join
  path builds the address it dials with a signed shift, which is undefined behaviour for a first
  octet of 128 or more. `192.168.x.x` will not work correctly; `10.42.0.x` will.
- They must differ from each other, and the two machines must mirror them.
- `RelayRoom` may only contain letters, digits, `.`, `-` and `_`, and is cut to 31 characters.
  Anything else is replaced with `_`.

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
| Only two `joined` lines, both from one address   | Only one machine has `RelayAddress` set.                                                                          |
| Two rooms in the log instead of one              | The `RelayRoom` values do not match.                                                                              |
| Lobby works, the game hangs on load              | Port 8088 is not open on the VPS — the lobby is 8086, the game itself is 8088.                                    |
| Joiner is accepted, then dropped from the lobby  | Both machines are using the same `LocalVirtualIP`. They must be the mirror of each other.                         |
| Game log says relay disabled: multi-instance     | Multi-instance clients share a port and need a narrow local bind, which relay mode cannot use. Run a single copy. |
| `dropped N from unknown senders`                 | Normal in small numbers (traffic that arrives just before a registration, or internet background noise).           |
