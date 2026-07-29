// Standalone check of the relay's lobby protocol. No game involved.
//
// Two relays are started. The first runs with stock settings and carries the protocol tests. The
// second runs with a deliberately tiny timeout and tiny ceilings, which is the only way to watch
// the sweep reclaim things and the caps refuse things inside a test's patience; it runs alongside
// the first one's 22-second advertisement timeout so the suite costs no extra wall clock.
const dgram = require('dgram');
const path = require('path');
const { spawn } = require('child_process');

const RELAY = process.argv[2] ?? path.join(__dirname, 'relay.js');
const PORT_A = 19086;	// stock settings
const PORT_B = 19087;	// short timeout, small ceilings
const PORT_C = 19088;	// stock timeout, small ceilings - what happens when a table fills up

function spawnRelay(port, extraEnv) {
	const proc = spawn('node', [RELAY], {
		env: { ...process.env, RELAY_PORTS: String(port), RELAY_STATS_MS: '600000', ...extraEnv },
		stdio: ['ignore', 'pipe', 'pipe'],
	});
	const r = { proc, log: '', port };
	proc.stdout.on('data', (d) => { r.log += d.toString(); });
	proc.stderr.on('data', (d) => { r.log += d.toString(); });
	return r;
}

const relayA = spawnRelay(PORT_A, {});
const relayB = spawnRelay(PORT_B, {
	RELAY_TIMEOUT_MS: '2500',	// sweeps every 1250 ms, so a release lands inside 4 s
	RELAY_MAX_ROOMS: '4',
	RELAY_MAX_LIST: '2',
});
const relayC = spawnRelay(PORT_C, { RELAY_MAX_ROOMS: '4' });

const sockets = [];

/* One test client: its own UDP socket, so it is its own endpoint as far as the relay is
 * concerned. Faking several players from one socket is exactly what the relay now refuses - an
 * endpoint is in one room under one identity - so every simulated player needs a real socket. */
function client(port = PORT_A) {
	const sock = dgram.createSocket('udp4');
	const seen = [];
	sock.on('message', (m) => seen.push(m.toString('latin1')));
	sockets.push(sock);
	return {
		ready: new Promise((r) => sock.bind(0, r)),
		seen,
		send: (payload) => new Promise((r) => sock.send(
			Buffer.isBuffer(payload) ? payload : Buffer.from(payload, 'latin1'), port, '127.0.0.1', r)),
		clear: () => { seen.length = 0; },
		got: (needle) => seen.some((s) => s.includes(needle)),
		find: (prefix) => seen.find((s) => s.startsWith(prefix)),
	};
}

async function newClient(port = PORT_A) {
	const c = client(port);
	await c.ready;
	return c;
}

const wait = (ms) => new Promise((r) => setTimeout(r, ms));

const ipNum = (s) => s.split('.').reduce((a, o) => ((a * 256) + Number(o)), 0) >>> 0;

/* A relayed game packet: the 12-byte routing header the client puts in front of everything. */
function gxr1(src, dst, payload) {
	const buf = Buffer.alloc(12 + payload.length);
	buf.write('GXR1', 0, 'latin1');
	buf.writeUInt32BE(ipNum(src), 4);
	buf.writeUInt32BE(ipNum(dst), 8);
	buf.write(payload, 12, 'latin1');
	return buf;
}

let passed = 0;
let failed = 0;
function check(name, cond) {
	console.log(`${cond ? 'PASS' : 'FAIL'}  ${name}`);
	if (cond) {
		passed++;
	} else {
		failed++;
		process.exitCode = 1;
	}
}

/*
 * Relay B: the sweep actually gives things back, and the ceilings actually refuse.
 */
async function shortLivedRelayChecks() {
	// --- an assignment is released when the client stops asking ------------------------------
	// The address space is eight wide. If a browser that looked at a room and left never gave its
	// number back, a busy relay would run a room out of identities and start answering "-", which
	// puts every later joiner back on the hand-configured address this mechanism exists to remove.
	const first = await newClient(PORT_B);
	await first.send('GXWHO brm');
	// A registered member left to go quiet at the same time, so one wait covers both halves of the
	// sweep: the assignment it hands back, and the membership it reaps.
	const ghost = await newClient(PORT_B);
	await ghost.send('GXRLY grm 10.42.0.3');
	await wait(300);
	const got1 = first.find('GXYOU');
	check('short-timeout relay assigns an address', !!got1 && /^GXYOU brm 10\.42\.0\.1$/.test(got1.trim()));

	await wait(4200);	// past the 2500 ms timeout and at least one 1250 ms sweep

	const second = await newClient(PORT_B);	// a different endpoint entirely
	await second.send('GXWHO brm');
	await wait(300);
	const got2 = second.find('GXYOU');
	check('a released address is handed to the next client',
		!!got2 && got2.trim() === 'GXYOU brm 10.42.0.1');
	check('the release was logged', /released 10\.42\.0\.1/.test(relayB.log));
	// The room-cap check below only fits if grm really went away, so it doubles as proof that a room
	// which empties gives its name back.
	check('the sweep reclaims a member that stopped talking',
		/grm: 10\.42\.0\.3 timed out/.test(relayB.log));

	// --- the reply to GXLIST is capped -------------------------------------------------------
	// One tiny request must not be able to make the relay emit an unbounded burst at whatever
	// source address the request claimed to come from.
	const hosts = [];
	for (let i = 1; i <= 3; i++) {
		const h = await newClient(PORT_B);
		await h.send(`GXRLY lr${i} 10.42.0.1`);
		await h.send(`GXADV lr${i} 10.42.0.1 1 8 Game ${i}|map ${i}`);
		hosts.push(h);
	}
	await wait(250);
	const capBrowser = await newClient(PORT_B);
	await capBrowser.send('GXLIST');
	await wait(400);
	const games = capBrowser.seen.filter((s) => s.startsWith('GXGAME'));
	check('a list reply is capped at RELAY_MAX_LIST', games.length === 2);

}

/*
 * Relay C: what a full table does. Every ceiling added here is a memory bound, and a memory bound
 * that refuses newcomers is a lockout an attacker can buy for a handful of datagrams - so the
 * ceilings give up the least established entry instead, and a real game must never be the one given
 * up. RELAY_MAX_ROOMS is 4 here so that "full" is reachable in a test.
 */
async function fullTableChecks() {
	// A real game: two players, in one room.
	const keepA = await newClient(PORT_C);
	const keepB = await newClient(PORT_C);
	await keepA.send('GXRLY keep 10.42.0.1');
	await keepB.send('GXRLY keep 10.42.0.2');

	// Three rooms somebody named once and never came back to. That is four rooms - the ceiling.
	for (let i = 1; i <= 3; i++) {
		const one = await newClient(PORT_C);
		await one.send(`GXRLY sp${i} 10.42.0.1`);
	}
	await wait(300);

	// The fifth room has to displace something.
	const late = await newClient(PORT_C);
	await late.send('GXRLY sp4 10.42.0.1');
	await wait(300);
	check('a full room table gives up its weakest room', /room sp1: evicted/.test(relayC.log));
	check('and lets the newcomer in rather than locking it out',
		/room sp4: 10\.42\.0\.1 joined/.test(relayC.log));

	// The two-player game must be untouched - not merely still listed, still carrying traffic.
	keepB.clear();
	await keepA.send(gxr1('10.42.0.1', '255.255.255.255', 'KEEP-ALIVE'));
	await wait(400);
	check('a room with players in it is never the one given up', keepB.got('KEEP-ALIVE'));

	// The same argument for the address book: asking about a lot of rooms must not stop the relay
	// answering the next real player.
	const prober = await newClient(PORT_C);
	for (let i = 1; i <= 6; i++) {
		await prober.send(`GXWHO probe${i}`);
	}
	await wait(400);
	const last = prober.seen.find((s) => s.startsWith('GXYOU probe6'));
	check('a full assignment table still answers a new room',
		!!last && /^GXYOU probe6 10\.42\.0\.\d+$/.test(last.trim()));

	// --- the log cannot be used as the unbounded resource ------------------------------------
	// console.log to a pipe or to journald queues in memory when the reader falls behind, so a line
	// per refused datagram is a memory exhaustion primitive that needs no table at all. Measured on
	// the unthrottled relay: six seconds of junk took it from 48 MB to 470 MB.
	// A fresh room name per datagram is the cheapest way to reach a log line, so that is what this
	// sends: on an unthrottled relay it is one line out per packet in, for as long as anyone cares
	// to keep sending.
	const noisy = await newClient(PORT_C);
	const before = relayC.log.split('\n').length;
	for (let i = 0; i < 800; i++) {
		await noisy.send(`GXWHO noise${i}`);
	}
	await wait(600);
	const written = relayC.log.split('\n').length - before;
	check('800 datagrams that each want a log line do not get 800 log lines',
		written < 150);

	// The held-back lines are not lost, they are counted onto the next one that gets through.
	await wait(5200);	// one token back
	await noisy.send('GXWHO noiseagain');
	await wait(300);
	check('the suppressed lines are accounted for', /were suppressed/.test(relayC.log));
}

(async () => {
	try {
		await wait(700);

		// =========================================================================================
		// Listing
		// =========================================================================================
		const host = await newClient();
		const browser = await newClient();

		await host.send('GXRLY testroom 10.42.0.1');
		await host.send('GXADV testroom 10.42.0.1 2 8 Karls Game|twilight flame');
		await wait(300);

		await browser.send('GXLIST');
		await wait(400);

		const game = browser.find('GXGAME');
		check('GXLIST returns a GXGAME line', !!game);
		check('game carries room, host, counts',
			!!game && game.includes('testroom') && game.includes('10.42.0.1') && game.includes(' 2 8 '));
		check('game carries name and map with spaces intact',
			!!game && game.includes('Karls Game|twilight flame'));

		// =========================================================================================
		// Identity assignment
		// The relay hands out virtual addresses so players do not have to agree on them by hand,
		// which is what makes public matchmaking possible at all.
		// =========================================================================================
		const asker = await newClient();
		await asker.send('GXWHO idroom');
		await wait(300);
		const you1 = asker.find('GXYOU');
		check('GXWHO returns an assignment', !!you1 && /^GXYOU idroom 10\.42\.0\.\d+$/.test(you1.trim()));

		// Same endpoint asking again must get the SAME address - the request is a UDP datagram and
		// may be retransmitted; a fresh address per retry would burn the room and change our identity
		// after we had already told the lobby about it.
		asker.clear();
		await asker.send('GXWHO idroom');
		await wait(300);
		const you2 = asker.find('GXYOU');
		check('assignment is sticky for the same endpoint', !!you2 && you2.trim() === you1.trim());

		// A second, distinct client must get a DIFFERENT address.
		const asker2 = await newClient();
		await asker2.send('GXWHO idroom');
		await wait(400);
		const you3 = asker2.find('GXYOU');
		check('a second client gets a different address', !!you3 && you3.trim() !== you1.trim());

		// A private room - registered but never advertised - must not be listed.
		const priv = await newClient();
		await priv.send('GXRLY secretroom 10.42.0.5');
		await wait(200);
		browser.clear();
		await browser.send('GXLIST');
		await wait(400);
		check('unadvertised room stays private', !browser.got('secretroom'));

		// =========================================================================================
		// A full room refuses rather than evicting
		// Eight real sockets: one endpoint is one member, so eight players means eight endpoints.
		// =========================================================================================
		for (let i = 1; i <= 8; i++) {
			const seat = await newClient();
			await seat.send(`GXRLY fullroom 10.42.0.${i}`);
		}
		await wait(300);
		const ninth = await newClient();
		await ninth.send('GXRLY fullroom 10.42.0.99');
		await wait(400);
		check('9th member refused, not evicted', /REFUSED 10\.42\.0\.99/.test(relayA.log));
		check('nobody was evicted to make room', !/evicting/.test(relayA.log));

		// =========================================================================================
		// Two rooms at once
		// Both hosts deliberately call themselves 10.42.0.1. Rooms are separate address spaces, so
		// that is legal - and it is the sharpest possible test of routing, because a leak between
		// rooms would land on a peer whose id matches exactly.
		// =========================================================================================
		const alphaHost = await newClient();
		const bravoHost = await newClient();
		const alphaPeer = await newClient();

		await alphaHost.send('GXRLY roomalpha 10.42.0.1');
		await alphaHost.send('GXADV roomalpha 10.42.0.1 2 8 Alpha Game|desert fury');
		await bravoHost.send('GXRLY roombravo 10.42.0.1');
		await bravoHost.send('GXADV roombravo 10.42.0.1 1 8 Bravo Game|tournament desert');
		await alphaPeer.send('GXRLY roomalpha 10.42.0.2');
		await wait(400);

		browser.clear();
		await browser.send('GXLIST');
		await wait(400);
		const alphaLine = browser.find('GXGAME roomalpha');
		const bravoLine = browser.find('GXGAME roombravo');
		check('one GXLIST lists two concurrent rooms', !!alphaLine && !!bravoLine);
		check('each listing keeps its own name and map',
			!!alphaLine && alphaLine.includes('Alpha Game|desert fury')
			&& !!bravoLine && bravoLine.includes('Bravo Game|tournament desert'));

		// --- broadcast stays inside its room -----------------------------------------------------
		alphaPeer.clear();
		bravoHost.clear();
		await alphaHost.send(gxr1('10.42.0.1', '255.255.255.255', 'ALPHA-BCAST'));
		await wait(400);
		check('a broadcast reaches the other member of the same room', alphaPeer.got('ALPHA-BCAST'));
		check('a broadcast never crosses into another room', !bravoHost.got('ALPHA-BCAST'));

		// --- unicast stays inside its room, even against an identical id -------------------------
		alphaHost.clear();
		bravoHost.clear();
		await alphaPeer.send(gxr1('10.42.0.2', '10.42.0.1', 'ALPHA-UNICAST'));
		await wait(400);
		check('a unicast reaches the peer holding that id in the sender\'s room',
			alphaHost.got('ALPHA-UNICAST'));
		check('a unicast never reaches the identical id in another room',
			!bravoHost.got('ALPHA-UNICAST'));

		// =========================================================================================
		// Assignment is per room
		// roomalpha already has 10.42.0.1 and 10.42.0.2 registered; roombravo only has 10.42.0.1.
		// =========================================================================================
		const dual = await newClient();
		await dual.send('GXWHO roomalpha');
		await wait(300);
		const inAlpha = dual.find('GXYOU roomalpha');
		check('assignment skips addresses already registered in that room',
			!!inAlpha && inAlpha.trim() === 'GXYOU roomalpha 10.42.0.3');

		dual.clear();
		await dual.send('GXWHO roombravo');
		await wait(300);
		const inBravo = dual.find('GXYOU roombravo');
		check('the same client holds a separate address in a second room',
			!!inBravo && inBravo.trim() === 'GXYOU roombravo 10.42.0.2');

		dual.clear();
		await dual.send('GXWHO roomalpha');
		await wait(300);
		const inAlphaAgain = dual.find('GXYOU roomalpha');
		check('an assignment is not disturbed by asking about another room',
			!!inAlphaAgain && inAlphaAgain.trim() === 'GXYOU roomalpha 10.42.0.3');

		// =========================================================================================
		// A listing belongs to the room it describes
		// The imposter is a real registered client - just of the wrong room. Without this, anyone who
		// guesses a room name can republish it pointing at a host that is not there.
		// =========================================================================================
		const imposter = await newClient();
		await imposter.send('GXRLY roombravo 10.42.0.6');
		await wait(200);
		await imposter.send('GXADV roomalpha 10.42.0.9 8 8 Free Wins|hijack');
		await wait(300);
		browser.clear();
		await browser.send('GXLIST');
		await wait(400);
		const alphaStill = browser.find('GXGAME roomalpha');
		check('an advert from outside the room is refused',
			!!alphaStill && alphaStill.includes('Alpha Game') && !browser.got('Free Wins'));
		check('the refused advert was logged', /REFUSED advert .* not registered in that room/.test(relayA.log));

		// =========================================================================================
		// One endpoint, one room
		// A relayed packet carries no room, so the sender's endpoint is the only thing that says
		// which room it belongs to. Joining a second room is therefore leaving the first.
		// =========================================================================================
		await alphaPeer.send('GXRLY roombravo 10.42.0.7');
		await wait(300);
		alphaPeer.clear();
		await alphaHost.send(gxr1('10.42.0.1', '255.255.255.255', 'ALPHA-AGAIN'));
		await wait(400);
		check('an endpoint that registers elsewhere stops receiving its old room',
			!alphaPeer.got('ALPHA-AGAIN'));

		alphaPeer.clear();
		await bravoHost.send(gxr1('10.42.0.1', '255.255.255.255', 'BRAVO-BCAST'));
		await wait(400);
		check('and starts receiving the room it moved to', alphaPeer.got('BRAVO-BCAST'));

		// --- and the abandoned membership cannot reach back and cut it off ---------------------
		// byEndpoint is one flat table, because a relayed packet carries no room and the sender's
		// endpoint is the only thing that says where it belongs. A membership left behind in the old
		// room still recorded this endpoint, so anything that removed THAT membership deleted the
		// entry the client's live membership was using - and the player, mid-game in the new room,
		// became an unknown sender whose every packet was dropped until its next keepalive five
		// seconds later. Taking over the abandoned identity is the version of that with no waiting
		// in it: exactly what happens when the relay assigns 10.42.0.1 in that room to somebody else.
		const drifter = await newClient();
		const anchor = await newClient();
		const taker = await newClient();
		await drifter.send('GXRLY moveout 10.42.0.1');
		await drifter.send('GXRLY movein 10.42.0.1');
		await anchor.send('GXRLY movein 10.42.0.2');
		await wait(300);
		await taker.send('GXRLY moveout 10.42.0.1');
		await wait(300);
		anchor.clear();
		await drifter.send(gxr1('10.42.0.1', '255.255.255.255', 'STILL-HERE'));
		await wait(400);
		check('a client is not blackholed when the identity it left behind is taken over',
			anchor.got('STILL-HERE'));

		// =========================================================================================
		// Delisting, and the short-lived relay's sweep, at the same time
		// The advert times out at 15s but the sweep only runs every 10s, so the log line can trail
		// the actual delisting by up to a sweep. sendList applies the deadline itself, which is why
		// the game stops being listed on time regardless - wait for the sweep before asserting on the
		// log.
		// =========================================================================================
		console.log('  (waiting out the advert timeout and a sweep, exercising the other relays meanwhile...)');
		await Promise.all([wait(22000), shortLivedRelayChecks(), fullTableChecks()]);

		browser.clear();
		await browser.send('GXLIST');
		await wait(400);
		check('game delisted after host stops advertising', !browser.got('testroom'));
		check('delist was logged', /delisted/.test(relayA.log));
	} finally {
		for (const r of [relayA, relayB, relayC]) {
			r.proc.kill();
		}
		for (const s of sockets) {
			try { s.close(); } catch { /* already closed */ }
		}
		await wait(200);
		console.log(`\n${passed + failed} checks, ${passed} passed, ${failed} failed`);
		console.log('\n--- relay A log ---\n' + relayA.log.trim());
		console.log('\n--- relay B log ---\n' + relayB.log.trim());
		console.log('\n--- relay C log ---\n' + relayC.log.trim());
	}
})();
