#!/usr/bin/env node
'use strict';

/*
 * GeneralsX UDP relay.
 *
 * Forwards game traffic between two machines that cannot reach each other directly, so that
 * neither has to forward a port. It is deliberately dumb: it pairs endpoints and copies bytes.
 *
 * The client (Core/GameEngine/Source/GameNetwork/Transport.cpp) sends every datagram here while
 * keeping the original destination port, and announces itself with a plaintext line
 *
 *     GXRLY <room> <client-id>
 *
 * on startup and every 5 seconds after that. That announcement is how the relay learns a client's
 * public address, which room it belongs to, and - by repetition - keeps the NAT mapping open.
 *
 * Everything that is not such an announcement is forwarded verbatim to the other member of the
 * same room, on the same port it arrived on. The relay never parses, decrypts, or rewrites game
 * traffic, and it does not need to: the clients address each other by virtual LAN addresses that
 * only they know about. Keeping the two ports separate is what lets one relay carry both the
 * lobby (8086) and the in-game (8088) conversation.
 */

const dgram = require('dgram');

const PORTS = parsePorts(process.env.RELAY_PORTS || '8086,8088');
const MEMBER_TIMEOUT_MS = parseNumber(process.env.RELAY_TIMEOUT_MS, 60000);
// Sweeping at half the timeout keeps the worst-case lifetime of a dead member at 1.5x the
// configured one instead of leaving it to a fixed interval that a short timeout would outrun.
const SWEEP_INTERVAL_MS = Math.max(500, Math.min(10000, Math.floor(MEMBER_TIMEOUT_MS / 2)));
const STATS_INTERVAL_MS = parseNumber(process.env.RELAY_STATS_MS, 300000);

// Two friends, one room. A third registration evicts whichever member has been quiet longest,
// which is also how a client that restarts under a new id gets its slot back.
const MEMBERS_PER_ROOM = 2;

const TAG = 'GXRLY';
const MAX_REGISTRATION_LEN = 64;
const TOKEN_RE = /^[A-Za-z0-9._-]{1,31}$/;

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
	const stats = { forwarded: 0, bytes: 0, dropped: 0 };
	const sock = dgram.createSocket({ type: 'udp4', reuseAddr: true });

	function register(reg, rinfo) {
		let members = rooms.get(reg.room) ?? new Map();

		let member = members.get(reg.id);
		if (!member) {
			while (members.size >= MEMBERS_PER_ROOM) {
				const oldest = [...members.values()].sort((a, b) => a.lastSeen - b.lastSeen)[0];
				log(`[${port}] room ${reg.room}: evicting ${oldest.id} at ${endpointKey(oldest.address, oldest.port)} to make room for ${reg.id}`);
				drop(oldest);
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

		for (const peer of members.values()) {
			if (peer === sender || peer.address === null) {
				continue;
			}
			sock.send(msg, peer.port, peer.address, (err) => {
				if (err) {
					log(`[${port}] send to ${endpointKey(peer.address, peer.port)} failed: ${err.message}`);
				}
			});
			stats.forwarded++;
			stats.bytes += msg.length;
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
		if (stats.forwarded === 0 && stats.dropped === 0) {
			return;
		}
		log(`[${port}] ${rooms.size} room(s), forwarded ${stats.forwarded} packets / ${stats.bytes} bytes, dropped ${stats.dropped} from unknown senders`);
		stats.forwarded = 0;
		stats.bytes = 0;
		stats.dropped = 0;
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
