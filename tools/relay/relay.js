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

// Every table in here is keyed by a room name a client chose, and a client is free to choose a new
// one for every datagram it sends. Without a ceiling, `GXRLY <random> <random>` in a loop allocates
// a room per packet and the process is killed by MemoryMax long before the entries time out - which
// takes down every game in progress, not just the attacker's.
//
// The ceiling is per listening port and far above what one relay should ever host, so reaching it
// is an incident rather than a tuning problem. Reaching it does NOT refuse the newcomer: see
// evictWeakestRoom for why a ceiling that turns a memory problem into a lockout has fixed nothing.
const MAX_ROOMS = parseNumber(process.env.RELAY_MAX_ROOMS, 512);

const TAG = 'GXRLY';
const MAX_REGISTRATION_LEN = 64;
const TOKEN_RE = /^[A-Za-z0-9._-]{1,31}$/;

// GeneralsX @feature Server list. The relay is already the only thing that knows which games
// exist and who is in them, so it is also the matchmaker - the third role alongside NAT relay and
// desync arbiter. This exists because the stock Online tab talks to GameSpy, which was shut down
// in 2014 and is never coming back.
//
//     GXADV <room> <hostVirtualIP> <players> <slots> <name>|<map>   host -> relay, every 5s
//     GXLIST                                                        client -> relay
//     GXGAME <room> <hostVirtualIP> <players> <slots> <name>|<map>  relay -> client, one per game
//
// A room is listed only while its host keeps advertising. There is no teardown message and none is
// wanted: a host that crashes, is killed, or loses its link simply stops advertising and drops off
// the list on the next sweep, which is the one failure mode that has to work without cooperation.
// A room that never advertises is private and never appears - that is what keeps a friends game
// out of a public browser.
//
// An advertisement is accepted only from an endpoint that is REGISTERED IN THE ROOM it advertises,
// and it may not overwrite one owned by a different, still-live endpoint. Without that, GXADV is an
// unauthenticated write to a global table: a stranger who guesses a room name can republish it with
// a wrong hostVirtualIP, and everyone who joins from the browser dials a peer that does not exist -
// a game that just looks broken, with nothing in any log to say why. The client always writes GXRLY
// and GXADV back to back on the same socket (Transport::sendRelayRegistration), so the honest path
// is unaffected; the ordering is re-established every 5 s if a datagram is ever reordered or lost.
// It also makes `adverts` a subset of `rooms`, so it inherits that table's ceiling for free.
const ADV_TAG = 'GXADV';
const LIST_TAG = 'GXLIST';
const GAME_TAG = 'GXGAME';
// The reply to GXLIST is one datagram per game, so an unbounded list turns a 6-byte request into an
// arbitrarily large burst aimed at whatever source address the request carried - and a UDP source
// address is free to forge. This caps how many games one reply may carry, which is the relay's
// amplification factor. It matches the client's own MAX_RELAY_LISTINGS, so nothing that would have
// been displayed is lost by it.
const MAX_GAMES_PER_LIST = parseNumber(process.env.RELAY_MAX_LIST, 32);

// And this caps how many lobby replies the port may emit per second, across all of them, so that a
// forged source address cannot be pointed at a victim indefinitely no matter which lobby message is
// used to do it. GXWHO on its own is only a 1:1 reflector and close to useless to an attacker; this
// is what stops the total, GXLIST included, from being worth aiming. A real relay answers a handful
// of lobby messages a minute, so the ceiling is three orders of magnitude above anything legitimate.
const REPLY_BUDGET_PER_SEC = parseNumber(process.env.RELAY_REPLY_BUDGET, 1000);

// GeneralsX @feature Matchmaking. The relay ASSIGNS each client its virtual address:
//
//     GXWHO <room>                 client -> relay: who am I in this room?
//     GXYOU <room> <virtualIP>     relay -> client: you are 10.42.0.N
//     GXYOU <room> -               relay -> client: room is full
//
// Before this, LocalVirtualIP was hand-written into every player's Options.ini and had to be
// unique across the lobby by human agreement. That is workable for a handful of known machines and
// impossible for public matchmaking: everyone installs the game and everyone is 10.42.0.1. Two
// clients sharing an identity are, to the relay, one client whose NAT keeps rebinding - so it does
// not even fail cleanly, it corrupts the lobby.
//
// Assignment is sticky per endpoint so a retransmitted request returns the SAME address rather
// than burning a second slot, and expires on the same sweep as everything else, so a client that
// disappears frees its number.
const WHO_TAG = 'GXWHO';
const YOU_TAG = 'GXYOU';

// GeneralsX @feature Lobby chat. The lobby screen has a chat box and the relay carried no chat at
// all, so it was inert.
//
//     GXCHT <room> <text>                     client -> relay
//     GXSAY <room> <senderVirtualIP> <text>   relay -> every OTHER member of that room
//
// The sender's identity is supplied by the RELAY, from the registration table, and is never read
// out of the datagram. This is the whole security design in one sentence: a client that could name
// itself could name anybody, and an unauthenticated line that is echoed verbatim to seven other
// players is the ideal place to impersonate the host ("everyone leave and rejoin room X"). Same
// rule as GXADV, and for the same reason - a chat line is a claim about a room, so it has to come
// from inside that room.
//
// The <room> argument is therefore redundant with the sender's endpoint, and is required anyway: it
// is the client saying which room it BELIEVES it is in. A mismatch means the client and the relay
// disagree - a client that just switched rooms, or a forgery - and refusing loudly is better than
// silently delivering a message into a room the sender did not mean to be in.
const CHAT_TAG = 'GXCHT';
const SAY_TAG = 'GXSAY';
// The datagram bound, generous compared with the text bound below so that an over-long line is
// TRUNCATED rather than falling through the parser to be treated as a game packet and broadcast raw.
const MAX_CHAT_LEN = 320;
// And the text bound. 100 is g_lanMaxChatLength (GameNetwork/LANAPI.h) - what the game's own chat
// field holds, so nothing that would have been displayed is lost by it.
const MAX_CHAT_TEXT = 100;
// Per-member token bucket: a burst for someone typing fast, then a sustained trickle. The bucket
// lives ON THE MEMBER, so it is bounded by MEMBERS_PER_ROOM x MAX_ROOMS and adds no table of its
// own - a per-source counter keyed on something the sender chose would be the same unbounded-table
// bug the room ceiling exists to prevent. An unregistered sender is refused before any state is
// touched at all, so flooding chat from a stranger allocates nothing.
const CHAT_BURST = 5;
const CHAT_REFILL_MS = 2000;
// The virtual LAN the game believes it is on. Must stay inside 10.0.0.0/8: the join path builds
// the address it dials with a signed shift, which is undefined behaviour for a first octet >= 128.
const VIRTUAL_NET_PREFIX = [10, 42, 0];
const MAX_ADVERT_LEN = 320;
const ADVERT_TIMEOUT_MS = 15000;	// 3 missed advertisements at the client's 5 s cadence

// Two clients sharing one LocalVirtualIP look exactly like one client whose NAT rebound: same id,
// new endpoint. A single observation cannot tell them apart. What distinguishes them is that a
// real rebind happens once and settles, while duplicates FLAP - both clients keep announcing and
// each move undoes the other. So this counts moves in a window and says so loudly, rather than
// refusing, which would break a legitimate rebind on a mobile client.
const DUPLICATE_WINDOW_MS = 10000;
const DUPLICATE_MOVE_THRESHOLD = 4;

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
// The same four bytes as one big-endian word, so a datagram can be recognised as game traffic by
// comparing an integer instead of building a string for every packet that arrives.
const HEADER_MAGIC_BE = 0x47585231;
const HEADER_SIZE = 12;
// The game's own broadcast address, and the limited broadcast. Either means "everyone in the room",
// which is what LAN discovery expects to happen when it shouts at the subnet.
const BROADCAST_IPS = new Set([0xFFFFFFFF, 0x00000000]);

function isRelayedGamePacket(msg) {
	return msg.length >= HEADER_SIZE && msg.readUInt32BE(0) === HEADER_MAGIC_BE;
}

function parseHeader(msg) {
	if (!isRelayedGamePacket(msg)) {
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

/*
 * Almost every line this relay writes is triggered by one arriving datagram, which makes the log
 * itself the cheapest unbounded resource in the process. console.log to a pipe or to journald is
 * asynchronous: when the reader cannot keep up, Node queues the writes IN MEMORY. Six seconds of
 * junk aimed at the lobby port took a measured 48 MB relay to 470 MB purely in queued log output -
 * MemoryMax kills it, systemd restarts it, and every game in progress drops. Discarding stdout
 * entirely kept the same soak flat at 48 MB, which is what pinned it down.
 *
 * So each line has a token bucket. A burst of LOG_BURST prints immediately - a real eight-player
 * lobby produces far fewer than that of any one line, so the log an operator reads while
 * troubleshooting is unchanged - and a sustained stream is cut to one line per LOG_REFILL_MS with a
 * count of what was held back. The true volume is in the periodic summary, which is a fixed number
 * of lines however hard anyone pushes.
 *
 * Keys are fixed strings chosen here, never anything from a datagram: a table keyed by attacker
 * input would be the bug this is meant to prevent.
 */
const LOG_BURST = 20;
const LOG_REFILL_MS = 5000;
const logGates = new Map();

function logLimited(site, ...args) {
	const now = Date.now();
	let gate = logGates.get(site);
	if (!gate) {
		gate = { tokens: LOG_BURST, last: now, suppressed: 0 };
		logGates.set(site, gate);
	}
	gate.tokens = Math.min(LOG_BURST, gate.tokens + ((now - gate.last) / LOG_REFILL_MS));
	gate.last = now;

	if (gate.tokens < 1) {
		gate.suppressed++;
		return;
	}
	gate.tokens -= 1;
	if (gate.suppressed > 0) {
		const held = gate.suppressed;
		gate.suppressed = 0;
		log(...args, `(+${held} more like this were suppressed)`);
		return;
	}
	log(...args);
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

/*
 * The free-form tail of an advertisement is the only client-controlled text the relay ever echoes
 * or writes to its log, so it is scrubbed here rather than trusted. The stock client already
 * replaces these (Transport::setGameAdvertisement) - which is exactly why the relay must do it too:
 * the guarantee has to hold for a datagram that did not come from the stock client. A newline would
 * forge a whole line in journalctl, and a '|' would move the name/map boundary so the browser shows
 * a truncated name against somebody else's map.
 */
function sanitizeText(s) {
	// eslint-disable-next-line no-control-regex
	return s.replace(/[\x00-\x1F\x7F|]/g, '/');
}

/*
 * A game advertisement, or null. The trailing "<name>|<map>" is free-form and may contain spaces,
 * so only the fixed fields ahead of it are split on whitespace.
 */
function parseAdvertisement(msg) {
	if (msg.length > MAX_ADVERT_LEN || msg.length <= ADV_TAG.length) {
		return null;
	}

	const parts = msg.toString('latin1').trim().split(' ');
	if (parts.length < 6 || parts[0] !== ADV_TAG) {
		return null;
	}
	if (!TOKEN_RE.test(parts[1]) || !TOKEN_RE.test(parts[2])) {
		return null;
	}

	const players = Number.parseInt(parts[3], 10);
	const slots = Number.parseInt(parts[4], 10);
	if (!Number.isInteger(players) || !Number.isInteger(slots) || slots < 1 || players < 0) {
		return null;
	}

	const rest = parts.slice(5).join(' ');
	const sep = rest.indexOf('|');
	return {
		room: parts[1],
		hostIP: parts[2],
		players,
		slots,
		name: sanitizeText(sep >= 0 ? rest.slice(0, sep) : rest),
		map: sanitizeText(sep >= 0 ? rest.slice(sep + 1) : ''),
	};
}

function isListRequest(msg) {
	return msg.length <= LIST_TAG.length + 2 && msg.toString('latin1').trim() === LIST_TAG;
}

/*
 * Chat text is scrubbed harder than an advertisement's, and never with a character that could be
 * mistaken for content. It ends up in two places that both punish raw bytes:
 *
 *   - other players' chat widgets, where a control byte is at best garbage;
 *   - this relay's own log, where a newline forges a whole journalctl line and an ESC can drive a
 *     terminal escape sequence at whoever is reading `journalctl -fu gxrelay`. That is not
 *     theoretical here: the relay has already had a log-injection bug from exactly this shape of
 *     data, which is why sanitizeText exists for GXADV.
 *
 * '|' is deliberately KEPT, unlike in an advertisement: there it is a field separator and moving it
 * would misattribute a map, whereas chat has no delimiter after the sender and a player typing
 * "a|b" means it. Control bytes and DEL become a space rather than being deleted, so that padding a
 * word with them cannot silently splice two words together.
 */
function sanitizeChat(s) {
	// eslint-disable-next-line no-control-regex
	return s.replace(/[\x00-\x1F\x7F]/g, ' ');
}

/*
 * A chat line, or null. The text is free-form and may contain spaces, so only the tag and the room
 * ahead of it are split off.
 *
 * Note what is NOT parsed: a sender. See the note by CHAT_TAG - the identity is taken from the
 * registration table, never from the datagram.
 */
function parseChat(msg) {
	if (msg.length > MAX_CHAT_LEN || msg.length <= CHAT_TAG.length) {
		return null;
	}

	const line = msg.toString('latin1');
	const parts = line.split(' ');
	if (parts.length < 3 || parts[0] !== CHAT_TAG || !TOKEN_RE.test(parts[1])) {
		return null;
	}

	// Everything after "GXCHT <room> " verbatim, so spaces inside the message survive. Sanitise
	// first and trim afterwards: doing it the other way round leaves a line that ended in control
	// bytes ending in the spaces they turned into.
	const text = sanitizeChat(line.slice(CHAT_TAG.length + 1 + parts[1].length + 1))
		.slice(0, MAX_CHAT_TEXT)
		.replace(/\s+$/, '');
	if (text === '') {
		return null;	// an empty or whitespace-only line is not worth a datagram to seven peers
	}

	return { room: parts[1], text };
}

/* An identity request, or null. */
function parseWhoRequest(msg) {
	if (msg.length > MAX_REGISTRATION_LEN || msg.length <= WHO_TAG.length) {
		return null;
	}
	const parts = msg.toString('latin1').trim().split(' ');
	if (parts.length !== 2 || parts[0] !== WHO_TAG || !TOKEN_RE.test(parts[1])) {
		return null;
	}
	return { room: parts[1] };
}

function startRelay(port) {
	const rooms = new Map();		// room name -> Map(client id -> member)
	const byEndpoint = new Map();	// "address:port" -> member
	const adverts = new Map();		// room name -> advertisement + lastSeen
	const assigned = new Map();		// room name -> Map("addr:port" -> { id, lastSeen })
	const stats = { forwarded: 0, bytes: 0, dropped: 0, refused: 0, unrouted: 0, headerless: 0,
		listed: 0, dupeSuspect: 0, assigned: 0, assignFull: 0, roomsFull: 0, advRefused: 0,
		listClipped: 0, roomsEvicted: 0, assignEvicted: 0, replyDropped: 0,
			chat: 0, chatRefused: 0, chatThrottled: 0 };
	const sock = dgram.createSocket({ type: 'udp4', reuseAddr: true });

	// Refilling allowance for lobby replies, in datagrams per second. O(1) and global on purpose: a
	// per-source counter would itself be an unbounded table keyed by an address the sender chose,
	// which is the bug it exists to prevent. Game traffic is deliberately NOT metered here - a
	// forwarded packet goes to a peer that registered itself, not to an address someone claimed.
	let replyBudget = REPLY_BUDGET_PER_SEC;
	let replyBudgetAt = Date.now();

	function replyAllowance() {
		const now = Date.now();
		replyBudget = Math.min(REPLY_BUDGET_PER_SEC,
			replyBudget + ((now - replyBudgetAt) / 1000) * REPLY_BUDGET_PER_SEC);
		replyBudgetAt = now;
		return Math.floor(replyBudget);
	}

	// Every reply the relay sends goes through here. A bare sock.send() with no callback hands the
	// failure to the socket's 'error' event, and that handler exits the process - so one ENETUNREACH
	// or EPERM while answering a stray datagram would kill BOTH ports and every game on them. A send
	// that fails is worth a line, never worth the relay.
	function reply(buf, rinfo, what) {
		if (replyAllowance() < 1) {
			stats.replyDropped++;
			logLimited(`${port}/reply-budget`,
				`[${port}] dropping ${what} to ${endpointKey(rinfo.address, rinfo.port)} - `
				+ `over ${REPLY_BUDGET_PER_SEC} lobby replies/s`);
			return false;
		}
		replyBudget -= 1;

		sock.send(buf, rinfo.port, rinfo.address, (err) => {
			if (err) {
				logLimited(`${port}/reply-failed`,
					`[${port}] ${what} to ${endpointKey(rinfo.address, rinfo.port)} failed: ${err.message}`);
			}
		});
		return true;
	}

	// byEndpoint is a single flat table because a game datagram carries no room - forward() has
	// nothing but the sender's address to work out which room a packet belongs to. So an entry must
	// only ever be removed by the member that currently owns it: an endpoint that moved from room A
	// to room B is still in byEndpoint, pointing at B, and letting A's stale member delete it on
	// timeout silently blackholes a player who is mid-game somewhere else.
	function forgetEndpoint(member) {
		const key = endpointKey(member.address, member.port);
		if (byEndpoint.get(key) === member) {
			byEndpoint.delete(key);
		}
	}

	/*
	 * Make space in a full room table by giving up the LEAST established room.
	 *
	 * Refusing instead was the obvious thing and it was wrong: filling the table costs one datagram
	 * per room and holds for the whole member timeout, so about nine packets a second permanently
	 * locks every real player out of a relay that is otherwise idle. A ceiling that turns a memory
	 * problem into a lockout has not fixed anything.
	 *
	 * Weakest means fewest members first, then quietest. Both halves matter. Member count is what
	 * separates a game from a name somebody typed once - a room invented by a flood never gets a
	 * second member, so an eight-player lobby is never the candidate while any one-member room
	 * exists. lastSeen then separates a host sitting in an empty lobby, which re-registers every
	 * five seconds and is therefore always among the freshest, from a room nobody has touched since
	 * it appeared. This is the same judgement the timeout already makes, applied early because the
	 * table is under pressure rather than because a clock ran out.
	 */
	function evictWeakestRoom() {
		let worstName = null;
		let worstSize = Infinity;
		let worstSeen = Infinity;

		for (const [name, members] of rooms) {
			let newest = 0;
			for (const m of members.values()) {
				if (m.lastSeen > newest) {
					newest = m.lastSeen;
				}
			}
			if (members.size < worstSize || (members.size === worstSize && newest < worstSeen)) {
				worstName = name;
				worstSize = members.size;
				worstSeen = newest;
			}
		}
		if (worstName === null) {
			return false;
		}

		stats.roomsEvicted++;
		logLimited(`${port}/evict-room`,
			`[${port}] room ${worstName}: evicted - room table is full (${rooms.size}/${MAX_ROOMS}), `
			+ `it had ${worstSize} member(s) and was the quietest`);
		for (const m of [...rooms.get(worstName).values()]) {
			drop(m);
		}
		rooms.delete(worstName);
		adverts.delete(worstName);
		return true;
	}

	function register(reg, rinfo) {
		const key = endpointKey(rinfo.address, rinfo.port);
		let members = rooms.get(reg.room);
		if (!members) {
			if (rooms.size >= MAX_ROOMS && !evictWeakestRoom()) {
				stats.roomsFull++;
				logLimited(`${port}/rooms-full`,
					`[${port}] REFUSED room ${reg.room} from ${key} - room table is full (${rooms.size}/${MAX_ROOMS}) and nothing could be given up`);
				return;
			}
			members = new Map();
		}

		let member = members.get(reg.id);
		if (!member) {
			if (members.size >= MEMBERS_PER_ROOM) {
				// Refuse, never evict a live member. The old code made room by dropping the
				// quietest player, so an extra client joining a full room broke the game for
				// someone already in it, and nothing said so. A timed-out member is reaped by the
				// sweep below, which is the only legitimate way a slot frees up.
				stats.refused++;
				logLimited(`${port}/room-full`,
					`[${port}] room ${reg.room}: REFUSED ${reg.id} from ${key} - room is full (${members.size}/${MEMBERS_PER_ROOM})`);
				return;
			}
			member = { room: reg.room, id: reg.id, address: null, port: 0, lastSeen: 0 };
			members.set(reg.id, member);
			// Set here rather than only at the end: from this point the room is non-empty and drop()
			// below must be able to find it.
			rooms.set(reg.room, members);
			logLimited(`${port}/join`,
				`[${port}] room ${reg.room}: ${reg.id} joined from ${key} (${members.size}/${MEMBERS_PER_ROOM})`);
		} else if (member.address !== rinfo.address || member.port !== rinfo.port) {
			// The client's NAT rebound, it restarted - or two clients are sharing this
			// LocalVirtualIP, which from here is the same observation. See the note by
			// DUPLICATE_MOVE_THRESHOLD: a rebind settles, duplicates flap.
			const now = Date.now();
			member.moves = (member.moves ?? []).filter((t) => t > now - DUPLICATE_WINDOW_MS);
			member.moves.push(now);
			if (member.moves.length >= DUPLICATE_MOVE_THRESHOLD) {
				stats.dupeSuspect++;
				logLimited(`${port}/dupe`,
					`[${port}] room ${reg.room}: ${reg.id} moved ${member.moves.length} times in `
					+ `${DUPLICATE_WINDOW_MS / 1000}s - TWO CLIENTS ARE PROBABLY SHARING `
					+ `LocalVirtualIP ${reg.id}. Give every player a different one.`);
				member.moves = [];
			}

			logLimited(`${port}/move`,
				`[${port}] room ${reg.room}: ${reg.id} moved from ${endpointKey(member.address, member.port)} to ${key}`);
			forgetEndpoint(member);
		}

		// One endpoint, one room. This is forced by the wire format rather than chosen: a relayed
		// game packet has a source and destination virtual address but no room, so forward() can
		// only resolve the room from the sender's endpoint - an endpoint in two rooms at once has no
		// defined meaning. Registering into a different room is therefore how a client says it LEFT
		// the previous one, which is exactly what happens now that a browser lists many rooms and
		// picks one. Left implicit, the old membership lingers for the full timeout and then takes
		// this endpoint's live entry with it.
		const prior = byEndpoint.get(key);
		if (prior && prior !== member) {
			logLimited(`${port}/leave`,
				`[${port}] room ${prior.room}: ${prior.id} left for room ${reg.room} (same endpoint ${key})`);
			drop(prior);
		}

		member.address = rinfo.address;
		member.port = rinfo.port;
		member.lastSeen = Date.now();
		byEndpoint.set(key, member);
		rooms.set(reg.room, members);
	}

	function drop(member) {
		forgetEndpoint(member);
		const members = rooms.get(member.room);
		if (members) {
			members.delete(member.id);
			if (members.size === 0) {
				rooms.delete(member.room);
				// A room that has emptied cannot be advertised by anyone, and leaving the listing up
				// would show a game with nobody in it until the advert's own timer ran out.
				if (adverts.delete(member.room)) {
					logLimited(`${port}/delist-empty`,
						`[${port}] room ${member.room}: delisted - last member left`);
				}
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
					logLimited(`${port}/forward-failed`,
						`[${port}] send to ${endpointKey(peer.address, peer.port)} failed: ${err.message}`);
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

	/*
	 * Hand this endpoint its virtual address in a room, creating one if it has none.
	 *
	 * Sticky per endpoint: the request is a UDP datagram and may be retransmitted, and an
	 * assignment that changed on every retry would burn the room's whole address space and hand
	 * the client a different identity than the one it already told the lobby about.
	 */
	/*
	 * The same argument as evictWeakestRoom, for the address book. A room here that has no members
	 * is one somebody asked about and never joined - a browser that looked and left, or a flood -
	 * and it is always given up before a room where people are actually playing. Without this, the
	 * ceiling meant a few hundred GXWHO datagrams could stop the relay handing out identities at
	 * all, which drops every later joiner back onto the hand-configured address that this whole
	 * mechanism exists to make unnecessary.
	 */
	function evictWeakestAssignment() {
		let worstName = null;
		let worstJoined = true;
		let worstSeen = Infinity;

		for (const [name, table] of assigned) {
			const joined = rooms.has(name);
			let newest = 0;
			for (const a of table.values()) {
				if (a.lastSeen > newest) {
					newest = a.lastSeen;
				}
			}
			if ((worstJoined && !joined) || (joined === worstJoined && newest < worstSeen)) {
				worstName = name;
				worstJoined = joined;
				worstSeen = newest;
			}
		}
		if (worstName === null) {
			return false;
		}

		stats.assignEvicted++;
		logLimited(`${port}/evict-assign`,
			`[${port}] room ${worstName}: gave up its assignments - table is full (${assigned.size}/${MAX_ROOMS})`);
		assigned.delete(worstName);
		return true;
	}

	function assignIdentity(room, rinfo) {
		const key = endpointKey(rinfo.address, rinfo.port);
		const now = Date.now();
		let table = assigned.get(room);

		const existing = table?.get(key);
		if (existing) {
			existing.lastSeen = now;
			return existing.id;
		}

		// Nothing is written to `assigned` until an address is actually handed out. The table used
		// to be inserted on sight, so `GXWHO <random>` in a loop allocated a Map per datagram and
		// only the sweep - up to ten seconds later - took them back; a few seconds of that is enough
		// to walk into MemoryMax and restart the relay under everyone playing.
		if (!table && assigned.size >= MAX_ROOMS && !evictWeakestAssignment()) {
			stats.assignFull++;
			logLimited(`${port}/assign-full`,
				`[${port}] room ${room}: cannot assign an identity to ${key} - assignment table is full (${assigned.size}/${MAX_ROOMS})`);
			return null;
		}

		// Avoid anything already handed out OR already registered in this room - a client that
		// registered under a hand-configured address must not be shadowed by an assignment. Both
		// lookups are scoped to this room: two rooms are separate address spaces, and the same
		// browser may legitimately hold 10.42.0.2 in one and 10.42.0.2 in another.
		const taken = new Set();
		for (const a of table?.values() ?? []) {
			taken.add(a.id);
		}
		const members = rooms.get(room);
		if (members) {
			for (const id of members.keys()) {
				taken.add(id);
			}
		}

		for (let n = 1; n <= MEMBERS_PER_ROOM; n++) {
			const id = `${VIRTUAL_NET_PREFIX.join('.')}.${n}`;
			if (taken.has(id)) {
				continue;
			}
			if (!table) {
				table = new Map();
				assigned.set(room, table);
			}
			table.set(key, { id, lastSeen: now });
			stats.assigned++;
			logLimited(`${port}/assign`, `[${port}] room ${room}: assigned ${id} to ${key}`);
			return id;
		}

		stats.assignFull++;
		logLimited(`${port}/assign-taken`,
			`[${port}] room ${room}: cannot assign an identity to ${key} - all ${MEMBERS_PER_ROOM} taken`);
		return null;
	}

	/* See the note by ADV_TAG: an advertisement is a claim about a room, so it has to come from
	 * inside that room, and it may not take a listing away from a live owner. */
	function advertise(adv, rinfo) {
		const key = endpointKey(rinfo.address, rinfo.port);
		const sender = byEndpoint.get(key);
		if (!sender || sender.room !== adv.room) {
			stats.advRefused++;
			logLimited(`${port}/advert-outsider`,
				`[${port}] room ${adv.room}: REFUSED advert from ${key} - not registered in that room`);
			return;
		}

		const now = Date.now();
		const existing = adverts.get(adv.room);
		if (existing && existing.endpoint !== key) {
			// Someone else already owns this listing. Only take it over once they are gone - either
			// their advert went stale or their registration did, which is what a host that restarted
			// or whose NAT rebound looks like. Both are checked so a rebind re-lists at once rather
			// than leaving the game invisible for the advert timeout.
			const owner = byEndpoint.get(existing.endpoint);
			const ownerAlive = owner !== undefined && owner.room === adv.room;
			if (ownerAlive && existing.lastSeen >= now - ADVERT_TIMEOUT_MS) {
				stats.advRefused++;
				logLimited(`${port}/advert-held`,
					`[${port}] room ${adv.room}: REFUSED advert from ${key} - listing is held by ${existing.endpoint}`);
				return;
			}
		}

		if (!existing) {
			logLimited(`${port}/list`,
				`[${port}] room ${adv.room}: listed as "${adv.name}" on ${adv.map} (${adv.players}/${adv.slots})`);
		}
		adverts.set(adv.room, { ...adv, endpoint: key, lastSeen: now });
	}

	/*
	 * Deliver one chat line to the rest of the sender's room.
	 *
	 * Three refusals, in this order, and each one matters:
	 *
	 *  1. Not registered here. The sender's identity is looked up, never read from the datagram, so
	 *     an endpoint the relay does not know has no identity to speak under and is dropped before
	 *     anything is allocated on its behalf.
	 *  2. Registered, but in a different room than the one it addressed. The client and the relay
	 *     disagree about where it is; delivering into the room the relay believes would put a
	 *     message the player wrote for their friends in front of strangers.
	 *  3. Talking too fast. Per-member bucket, so one player cannot fill seven other players' chat
	 *     boxes - or the relay's send queue - by looping a datagram.
	 *
	 * The text itself is never logged. It is unauthenticated data from the internet, and the value
	 * of putting it in an operator's journal is far below the cost of having a chat log at all. The
	 * counters say how much chat there is; nothing says what was in it.
	 */
	function chat(line, rinfo) {
		const key = endpointKey(rinfo.address, rinfo.port);
		const sender = byEndpoint.get(key);
		if (!sender || sender.room !== line.room) {
			stats.chatRefused++;
			logLimited(`${port}/chat-outsider`,
				`[${port}] room ${line.room}: REFUSED chat from ${key} - not registered in that room`);
			return;
		}

		const now = Date.now();
		sender.chatTokens = Math.min(CHAT_BURST,
			(sender.chatTokens ?? CHAT_BURST) + ((now - (sender.chatAt ?? now)) / CHAT_REFILL_MS));
		sender.chatAt = now;
		if (sender.chatTokens < 1) {
			stats.chatThrottled++;
			logLimited(`${port}/chat-flood`,
				`[${port}] room ${line.room}: dropping chat from ${sender.id} at ${key} - over `
				+ `${CHAT_BURST} in a burst / 1 per ${CHAT_REFILL_MS / 1000}s`);
			return;
		}
		sender.chatTokens -= 1;

		// The relay stamps the sender. A client that could name itself could name anybody, and the
		// obvious abuse of an echoed, unauthenticated line is to speak as the host.
		const members = rooms.get(sender.room);
		if (!members) {
			return;
		}
		const buf = Buffer.from(`${SAY_TAG} ${sender.room} ${sender.id} ${line.text}`, 'latin1');

		// Sent like game traffic rather than through reply(): every destination is a peer that
		// registered ITSELF on this relay, so this cannot be pointed at a forged source address and
		// RELAY_REPLY_BUDGET - which exists to stop exactly that - does not apply. The bound here is
		// the per-member bucket above, and the room's own eight-member ceiling. The error callback is
		// not optional: a bare send() hands the failure to the socket's 'error' handler, which exits
		// the process and would drop every game on both ports.
		for (const peer of members.values()) {
			if (peer === sender || peer.address === null) {
				continue;
			}
			sock.send(buf, peer.port, peer.address, (err) => {
				if (err) {
					logLimited(`${port}/chat-failed`,
						`[${port}] chat to ${endpointKey(peer.address, peer.port)} failed: ${err.message}`);
				}
			});
		}
		stats.chat++;
	}

	function sendList(rinfo) {
		const deadline = Date.now() - ADVERT_TIMEOUT_MS;
		let allowed = Math.min(MAX_GAMES_PER_LIST, replyAllowance());
		let sent = 0;
		let skipped = 0;
		for (const adv of adverts.values()) {
			if (adv.lastSeen < deadline) {
				continue;	// the sweep will reap it; do not advertise a dead game meanwhile
			}
			if (allowed <= 0) {
				skipped++;
				continue;
			}
			const line = `${GAME_TAG} ${adv.room} ${adv.hostIP} ${adv.players} ${adv.slots} ${adv.name}|${adv.map}`;
			reply(Buffer.from(line, 'latin1'), rinfo, 'game listing');
			allowed--;
			sent++;
		}
		stats.listed++;
		if (skipped > 0) {
			stats.listClipped += skipped;
		}
		// One datagram per game rather than one packed reply: a lobby list is small, and this way
		// a browser that misses a packet is missing one game rather than the whole list.
		if (sent === 0) {
			reply(Buffer.from(`${GAME_TAG} - - 0 0 |`, 'latin1'), rinfo, 'empty listing');
		}
	}

	sock.on('message', (msg, rinfo) => {
		// The hot path, first and without allocating. A relayed game packet is identified exactly by
		// its routing header, so recognising it here costs an integer compare. Every datagram used to
		// be offered to all four line parsers before reaching this, and the first two accept anything
		// up to 64 and 320 bytes - so each forwarded game packet built two throwaway strings and two
		// throwaway arrays before being forwarded, on the one path that carries lockstep traffic for
		// eight players at once.
		if (isRelayedGamePacket(msg)) {
			forward(msg, rinfo);
			return;
		}

		const reg = parseRegistration(msg);
		if (reg) {
			register(reg, rinfo);
			return;
		}
		const adv = parseAdvertisement(msg);
		if (adv) {
			advertise(adv, rinfo);
			return;
		}
		if (isListRequest(msg)) {
			sendList(rinfo);
			return;
		}
		const say = parseChat(msg);
		if (say) {
			chat(say, rinfo);
			return;
		}
		const who = parseWhoRequest(msg);
		if (who) {
			const id = assignIdentity(who.room, rinfo);
			// One small datagram out for one small datagram in, so this needs no allowance of its
			// own the way the list burst does.
			reply(Buffer.from(`${YOU_TAG} ${who.room} ${id ?? '-'}`, 'latin1'), rinfo, 'identity');
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
		const now = Date.now();
		const deadline = now - MEMBER_TIMEOUT_MS;
		for (const [room, members] of [...rooms.entries()]) {
			for (const member of [...members.values()]) {
				if (member.lastSeen < deadline) {
					logLimited(`${port}/timeout`, `[${port}] room ${room}: ${member.id} timed out`);
					drop(member);
				}
			}
			// drop() already removes a room it empties. Belt and braces: an empty Map left behind by
			// any future path would be a room name that can never be reclaimed, and MAX_ROOMS turns
			// that from a slow leak into a relay that stops accepting new games.
			if (members.size === 0) {
				rooms.delete(room);
			}
		}

		// A game stays listed only while its host keeps saying it exists. This is the whole
		// teardown path: a host that crashes or loses its link never sends anything else.
		const advDeadline = now - ADVERT_TIMEOUT_MS;
		for (const [room, adv] of [...adverts.entries()]) {
			if (adv.lastSeen < advDeadline) {
				logLimited(`${port}/delist`,
					`[${port}] room ${room}: delisted "${adv.name}" - host stopped advertising`);
				adverts.delete(room);
			}
		}

		// Release identities from clients that went away, or the address space leaks and a room
		// stops accepting anyone long after it emptied.
		for (const [room, table] of [...assigned.entries()]) {
			for (const [key, a] of [...table.entries()]) {
				if (a.lastSeen < deadline) {
					logLimited(`${port}/release`, `[${port}] room ${room}: released ${a.id} from ${key}`);
					table.delete(key);
				}
			}
			if (table.size === 0) {
				assigned.delete(room);
			}
		}
	}, SWEEP_INTERVAL_MS);

	const report = setInterval(() => {
		// Every counter has to be in this guard, not just the traffic ones. With only the first
		// three tested, a relay busy purely with browsing and matchmaking printed nothing and never
		// reset, so the next summary that did print carried a number covering an unknown span.
		if (!Object.values(stats).some((v) => v !== 0)) {
			return;
		}
		// This is the only place the real volumes appear, because every per-datagram line is rate
		// limited. Worth watching: refused means somebody could not get in, unrouted means a packet
		// was addressed to a peer this relay does not have, headerless means a client older than this
		// relay is connected, and anything evicted, clipped or reply-dropped means a table or a
		// ceiling was reached - which on a relay this size means a flood rather than a busy evening.
		log(`[${port}] ${rooms.size} room(s), ${adverts.size} listed, forwarded ${stats.forwarded} packets / ${stats.bytes} bytes, `
			+ `dropped ${stats.dropped} from unknown senders, refused ${stats.refused}, `
			+ `unrouted ${stats.unrouted}, headerless ${stats.headerless}, `
			+ `browsed ${stats.listed}, dupe-suspect ${stats.dupeSuspect}, `
			+ `assigned ${stats.assigned}, assign-full ${stats.assignFull}, `
			+ `rooms-full ${stats.roomsFull}, advert-refused ${stats.advRefused}, `
			+ `list-clipped ${stats.listClipped}, rooms-evicted ${stats.roomsEvicted}, `
			+ `assign-evicted ${stats.assignEvicted}, reply-dropped ${stats.replyDropped}, `
			+ `chat ${stats.chat}, chat-refused ${stats.chatRefused}, chat-throttled ${stats.chatThrottled}`);
		for (const k of Object.keys(stats)) {
			stats[k] = 0;
		}
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
