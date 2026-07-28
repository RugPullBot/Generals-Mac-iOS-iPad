#!/usr/bin/env node
'use strict';

/*
 * GeneralsX UDP relay.
 *
 * Forwards game traffic between machines that cannot reach each other directly, so that none of
 * them has to forward a port. Up to a full eight-player lobby shares one room. It is deliberately
 * dumb: it tracks endpoints and copies bytes.
 *
 * The client (Core/GameEngine/Source/GameNetwork/Transport.cpp) sends every datagram here while
 * keeping the original destination port, and announces itself with a plaintext line
 *
 *     GXRLY <room> <client-id>
 *
 * on startup and every 5 seconds after that. That announcement is how the relay learns a client's
 * public address, which room it belongs to, and - by repetition - keeps the NAT mapping open.
 *
 * Everything that is not such an announcement is forwarded verbatim, on the same port it arrived
 * on, to the peer named in the routing header the client puts in front of it - or to the whole
 * room if that header says broadcast. The relay never parses, decrypts, or rewrites game traffic,
 * and it does not need to: the clients address each other by virtual LAN addresses, and the one
 * this packet is for is in the clear in the header. Keeping the two ports separate is what lets
 * one relay carry both the lobby (8086) and the in-game (8088) conversation.
 */

const dgram = require('dgram');

const PORTS = parsePorts(process.env.RELAY_PORTS || '8086,8088');
const MEMBER_TIMEOUT_MS = parseNumber(process.env.RELAY_TIMEOUT_MS, 60000);
// Sweeping at half the timeout keeps the worst-case lifetime of a dead member at 1.5x the
// configured one instead of leaving it to a fixed interval that a short timeout would outrun.
const SWEEP_INTERVAL_MS = Math.max(500, Math.min(10000, Math.floor(MEMBER_TIMEOUT_MS / 2)));
const STATS_INTERVAL_MS = parseNumber(process.env.RELAY_STATS_MS, 300000);

// A full lobby, one room. Zero Hour seats eight.
//
// This used to be 2, and a third registration evicted whichever member had been quiet longest -
// so player 3 joining silently knocked player 1 off the relay, and the game they were all in
// looked like it had dropped a peer for no reason. Eviction now applies ONLY to members that have
// genuinely timed out (see sweep); a registration for a full room is refused and logged, which is
// a condition an operator can see rather than a game that quietly falls apart.
const MEMBERS_PER_ROOM = 8;

const TAG = 'GXRLY';
const MAX_REGISTRATION_LEN = 64;
const TOKEN_RE = /^[A-Za-z0-9._-]{1,31}$/;

/*
 * The routing header the client puts ahead of every relayed game packet:
 *
 *     [ 'GXR1' (4) ][ source virtual IP (4, BE) ][ destination virtual IP (4, BE) ][ payload ]
 *
 * This is the ONLY thing the relay reads. It never parses, decrypts or rewrites the payload, and
 * it never needs to: the clients address each other by virtual LAN addresses, and the destination
 * one is right here in the clear.
 *
 * With two players the relay could just send everything to "the other one". With eight it has to
 * know who a packet is for - lockstep command traffic is genuinely broadcast, but the join
 * handshake and the map transfer are not, and delivering those to all seven other peers is both
 * wasteful and wrong.
 */
const HEADER_MAGIC = 'GXR1';
const HEADER_SIZE = 12;
// The game's own broadcast address, and the limited broadcast. Either means "everyone in the room",
// which is what LAN discovery expects to happen when it shouts at the subnet.
const BROADCAST_IPS = new Set([0xFFFFFFFF, 0x00000000]);

function parseHeader(msg) {
	if (msg.length < HEADER_SIZE || msg.toString('latin1', 0, 4) !== HEADER_MAGIC) {
		return null;
	}
	return { src: msg.readUInt32BE(4), dst: msg.readUInt32BE(8) };
}

function ipToString(ip) {
	return `${(ip >>> 24) & 0xFF}.${(ip >>> 16) & 0xFF}.${(ip >>> 8) & 0xFF}.${ip & 0xFF}`;
}

function parsePorts(value) {
	const ports = value.split(',')
		.map((p) => Number.parseInt(p.trim(), 10))
		.filter((p) => Number.isInteger(p) && p > 0 && p < 65536);

	if (ports.length === 0) {
		throw new Error(`RELAY_PORTS has no usable port in "${value}"`);
	}
	return ports;
}

function parseNumber(value, fallback) {
	const n = Number.parseInt(value ?? '', 10);
	return Number.isInteger(n) && n > 0 ? n : fallback;
}

function log(...args) {
	console.log(new Date().toISOString(), ...args);
}

function endpointKey(address, port) {
	return `${address}:${port}`;
}

/*
 * A registration, or null if this datagram is anything else. Parsing strictly and falling back to
 * "it must be game traffic" means a game packet that happens to start with the tag is forwarded
 * rather than swallowed.
 */
function parseRegistration(msg) {
	if (msg.length > MAX_REGISTRATION_LEN || msg.length <= TAG.length) {
		return null;
	}

	const parts = msg.toString('latin1').trim().split(' ');
	if (parts.length !== 3 || parts[0] !== TAG) {
		return null;
	}
	if (!TOKEN_RE.test(parts[1]) || !TOKEN_RE.test(parts[2])) {
		return null;
	}

	return { room: parts[1], id: parts[2] };
}

function startRelay(port) {
	const rooms = new Map();		// room name -> Map(client id -> member)
	const byEndpoint = new Map();	// "address:port" -> member
	const stats = { forwarded: 0, bytes: 0, dropped: 0, refused: 0, unrouted: 0, headerless: 0 };
	const sock = dgram.createSocket({ type: 'udp4', reuseAddr: true });

	function register(reg, rinfo) {
		let members = rooms.get(reg.room) ?? new Map();

		let member = members.get(reg.id);
		if (!member) {
			if (members.size >= MEMBERS_PER_ROOM) {
				// Refuse, never evict a live member. The old code made room by dropping the
				// quietest player, so an extra client joining a full room broke the game for
				// someone already in it, and nothing said so. A timed-out member is reaped by the
				// sweep below, which is the only legitimate way a slot frees up.
				stats.refused++;
				log(`[${port}] room ${reg.room}: REFUSED ${reg.id} from ${endpointKey(rinfo.address, rinfo.port)} - room is full (${members.size}/${MEMBERS_PER_ROOM})`);
				return;
			}
			member = { room: reg.room, id: reg.id, address: null, port: 0, lastSeen: 0 };
			members.set(reg.id, member);
			log(`[${port}] room ${reg.room}: ${reg.id} joined from ${endpointKey(rinfo.address, rinfo.port)} (${members.size}/${MEMBERS_PER_ROOM})`);
		} else if (member.address !== rinfo.address || member.port !== rinfo.port) {
			// The client's NAT rebound, or it restarted. Follow it.
			log(`[${port}] room ${reg.room}: ${reg.id} moved from ${endpointKey(member.address, member.port)} to ${endpointKey(rinfo.address, rinfo.port)}`);
			byEndpoint.delete(endpointKey(member.address, member.port));
		}

		member.address = rinfo.address;
		member.port = rinfo.port;
		member.lastSeen = Date.now();
		byEndpoint.set(endpointKey(rinfo.address, rinfo.port), member);
		// Re-asserted rather than set on creation: an eviction above can empty the room, and drop()
		// removes an empty room from the table.
		rooms.set(reg.room, members);
	}

	function drop(member) {
		byEndpoint.delete(endpointKey(member.address, member.port));
		const members = rooms.get(member.room);
		if (members) {
			members.delete(member.id);
			if (members.size === 0) {
				rooms.delete(member.room);
			}
		}
	}

	function forward(msg, rinfo) {
		const sender = byEndpoint.get(endpointKey(rinfo.address, rinfo.port));
		if (!sender) {
			// Not from anyone who has registered on this port. Could be a client whose first
			// registration has not arrived yet, or a stranger scanning the box.
			stats.dropped++;
			return;
		}

		// Game traffic counts as a sign of life too, so a busy game never expires on a lost
		// keepalive.
		sender.lastSeen = Date.now();

		const members = rooms.get(sender.room);
		if (!members) {
			return;
		}

		// Route on the client's header. Without one the relay can only shout at the whole room:
		// correct for lockstep command traffic, wrong for the unicast paths - the join handshake
		// and the map transfer - where six uninvolved peers would receive someone else's mail.
		const header = parseHeader(msg);
		if (!header) {
			// A client that is not sending the header is older than this relay. Broadcasting keeps
			// it working as it did at two players, but count it: a steady stream here means a
			// version mismatch, and at eight players it is the thing that will misbehave first.
			stats.headerless++;
		}

		const broadcast = !header || BROADCAST_IPS.has(header.dst);
		const dstId = header && !broadcast ? ipToString(header.dst) : null;

		let delivered = 0;
		for (const peer of members.values()) {
			if (peer === sender || peer.address === null) {
				continue;
			}
			if (dstId !== null && peer.id !== dstId) {
				continue;
			}
			sock.send(msg, peer.port, peer.address, (err) => {
				if (err) {
					log(`[${port}] send to ${endpointKey(peer.address, peer.port)} failed: ${err.message}`);
				}
			});
			delivered++;
			stats.forwarded++;
			stats.bytes += msg.length;
		}

		if (dstId !== null && delivered === 0) {
			// Addressed to somebody who is not in this room. Expected in small numbers while a
			// peer is still registering or has just left; a steady stream means the clients
			// disagree about who is in the game.
			stats.unrouted++;
		}
	}

	sock.on('message', (msg, rinfo) => {
		const reg = parseRegistration(msg);
		if (reg) {
			register(reg, rinfo);
			return;
		}
		forward(msg, rinfo);
	});

	sock.on('error', (err) => {
		log(`[${port}] socket error: ${err.message}`);
		process.exit(1);
	});

	sock.on('listening', () => {
		log(`[${port}] listening`);
	});

	const sweep = setInterval(() => {
		const deadline = Date.now() - MEMBER_TIMEOUT_MS;
		for (const members of [...rooms.values()]) {
			for (const member of [...members.values()]) {
				if (member.lastSeen < deadline) {
					log(`[${port}] room ${member.room}: ${member.id} timed out`);
					drop(member);
				}
			}
		}
	}, SWEEP_INTERVAL_MS);

	const report = setInterval(() => {
		if (stats.forwarded === 0 && stats.dropped === 0 && stats.refused === 0) {
			return;
		}
		// The last three are the ones worth watching at eight players: refused means somebody
		// could not get in, unrouted means a packet was addressed to a peer this relay does not
		// have, and headerless means a client older than this relay is connected.
		log(`[${port}] ${rooms.size} room(s), forwarded ${stats.forwarded} packets / ${stats.bytes} bytes, `
			+ `dropped ${stats.dropped} from unknown senders, refused ${stats.refused}, `
			+ `unrouted ${stats.unrouted}, headerless ${stats.headerless}`);
		stats.forwarded = 0;
		stats.bytes = 0;
		stats.dropped = 0;
		stats.refused = 0;
		stats.unrouted = 0;
		stats.headerless = 0;
	}, STATS_INTERVAL_MS);

	sweep.unref?.();
	report.unref?.();
	sock.bind(port);
	return sock;
}

const sockets = PORTS.map(startRelay);

for (const signal of ['SIGINT', 'SIGTERM']) {
	process.on(signal, () => {
		log(`${signal} - shutting down`);
		for (const sock of sockets) {
			sock.close();
		}
		process.exit(0);
	});
}
