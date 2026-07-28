// Standalone check of the relay's lobby protocol. No game involved.
const dgram = require('dgram');
const { spawn } = require('child_process');

const PORT = 19086;
const relay = spawn('node', [process.argv[2] ?? require("path").join(__dirname, "relay.js")], {
	env: { ...process.env, RELAY_PORTS: String(PORT), RELAY_STATS_MS: '600000' },
	stdio: ['ignore', 'pipe', 'pipe'],
});
let relayLog = '';
relay.stdout.on('data', (d) => { relayLog += d.toString(); });
relay.stderr.on('data', (d) => { relayLog += d.toString(); });

const sock = dgram.createSocket('udp4');
const replies = [];
sock.on('message', (m) => replies.push(m.toString('latin1')));

const send = (s) => new Promise((r) => sock.send(Buffer.from(s, 'latin1'), PORT, '127.0.0.1', r));
const wait = (ms) => new Promise((r) => setTimeout(r, ms));

function check(name, cond) {
	console.log(`${cond ? 'PASS' : 'FAIL'}  ${name}`);
	if (!cond) process.exitCode = 1;
}

(async () => {
	await wait(600);
	sock.bind(0);
	await wait(200);

	// A host registers and advertises.
	await send('GXRLY testroom 10.42.0.1');
	await send('GXADV testroom 10.42.0.1 2 8 Karls Game|twilight flame');
	await wait(300);

	// A browser asks what is out there.
	await send('GXLIST');
	await wait(400);

	const game = replies.find((r) => r.startsWith('GXGAME'));
	check('GXLIST returns a GXGAME line', !!game);
	check('game carries room, host, counts', !!game && game.includes('testroom') && game.includes('10.42.0.1') && game.includes(' 2 8 '));
	check('game carries name and map with spaces intact', !!game && game.includes('Karls Game|twilight flame'));

	// A private room - registered but never advertised - must not be listed.
	await send('GXRLY secretroom 10.42.0.5');
	await wait(200);
	replies.length = 0;
	await send('GXLIST');
	await wait(400);
	check('unadvertised room stays private', !replies.some((r) => r.includes('secretroom')));

	// Fill the room to 8 and confirm the 9th is refused rather than evicting anyone.
	for (let i = 2; i <= 8; i++) {
		await send(`GXRLY testroom 10.42.0.${i}`);
	}
	await wait(300);
	await send('GXRLY testroom 10.42.0.99');
	await wait(400);
	check('9th member refused, not evicted', /REFUSED 10\.42\.0\.99/.test(relayLog));
	check('nobody was evicted to make room', !/evicting/.test(relayLog));

	// Delisting: stop advertising and the game should drop off.
	// The advert times out at 15s but the sweep only runs every 10s, so the log line can trail the
	// actual delisting by up to a sweep. sendList applies the deadline itself, which is why the
	// game stops being listed on time regardless - wait for the sweep before asserting on the log.
	console.log('  (waiting out the advert timeout and a sweep...)');
	await wait(22000);
	replies.length = 0;
	await send('GXLIST');
	await wait(400);
	check('game delisted after host stops advertising', !replies.some((r) => r.includes('testroom')));
	check('delist was logged', /delisted/.test(relayLog));

	relay.kill();
	sock.close();
	await wait(200);
	console.log('\n--- relay log ---\n' + relayLog.trim());
})();
