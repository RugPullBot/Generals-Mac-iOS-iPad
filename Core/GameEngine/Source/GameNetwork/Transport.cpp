/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////


#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/crc.h"
#include "Common/OptionPreferences.h"
#include "GameClient/ClientInstance.h"
#include "GameNetwork/Transport.h"
#include "GameNetwork/NetworkInterface.h"

// GeneralsX @feature Relay transport. The registration datagram is plaintext on purpose: the
// relay recognises its own control traffic by this tag alone and never has to look at, let alone
// decrypt, a single game packet.
static const char RELAY_REGISTRATION_TAG[] = "GXRLY";
// GeneralsX @feature Server list. Advertising a game is what puts it in the relay's browser, and
// NOT advertising is what keeps a private game out of it. See tools/relay/relay.js.
static const char RELAY_ADVERTISE_TAG[] = "GXADV";
static const Int RELAY_MAX_ROOM_LEN = 31;
static const UnsignedInt RELAY_REGISTRATION_INTERVAL = 5000;	// ms

// The relay listens on exactly these two ports, and telling them apart is the only demultiplexing
// it does. A socket on any other port must therefore never be relayed - the GameSpy NAT
// negotiation transport picks a random port in a 20000-wide range, and sending its probes to a
// port nothing is listening on would break online play for anyone who has a relay configured.
static const UnsignedShort RELAY_LOBBY_PORT = 8086;						// LANAPI::lobbyPort
static const UnsignedShort RELAY_GAME_PORT = NETWORK_BASE_PORT_NUMBER;	// what the LAN path hands ConnectionManager


//--------------------------------------------------------------------------
// Packet-level encryption is an XOR operation, for speed reasons.  To get
// the max throughput, we only XOR whole 4-byte words, so the last bytes
// can be non-XOR'd.

// This assumes the buf is a multiple of 4 bytes.  Extra is not encrypted.
static inline void encryptBuf( unsigned char *buf, Int len )
{
	UnsignedInt mask = 0x0000Fade;

	UnsignedInt *uintPtr = (UnsignedInt *) (buf);

	for (int i=0 ; i<len/4 ; i++) {
		*uintPtr = (*uintPtr) ^ mask;
		*uintPtr = htonl(*uintPtr);
		uintPtr++;
		mask += 0x00000321; // just for fun
	}
}

// This assumes the buf is a multiple of 4 bytes.  Extra is not encrypted.
static inline void decryptBuf( unsigned char *buf, Int len )
{
	UnsignedInt mask = 0x0000Fade;

	UnsignedInt *uintPtr = (UnsignedInt *) (buf);

	for (int i=0 ; i<len/4 ; i++) {
		*uintPtr = htonl(*uintPtr);
		*uintPtr = (*uintPtr) ^ mask;
		uintPtr++;
		mask += 0x00000321; // just for fun
	}
}

//--------------------------------------------------------------------------

Transport::Transport()
{
	m_winsockInit = false;
	m_udpsock = nullptr;
	m_relayEnabled = FALSE;
	m_relayAddr = 0;
	m_localVirtualIP = 0;
	m_lastRelayRegistration = 0;
	m_relayRegistration[0] = '\0';
	m_relayRegistrationLen = 0;
	m_relayRoom[0] = '\0';
	m_advertisement[0] = '\0';
	m_advertisementLen = 0;
	m_gameListCount = 0;
}

// GeneralsX @feature Relay transport. Everything the relay needs is decided here, once per
// socket, and every check that fails leaves relay mode off rather than half on: a half-configured
// relay would send game traffic to nowhere while the lobby still believed it was on a LAN.
void Transport::initRelay( UnsignedShort port )
{
	m_relayEnabled = FALSE;
	m_relayAddr = 0;
	m_localVirtualIP = 0;
	m_lastRelayRegistration = 0;
	m_relayRegistration[0] = '\0';
	m_relayRegistrationLen = 0;
	m_relayRoom[0] = '\0';
	m_advertisement[0] = '\0';
	m_advertisementLen = 0;
	m_gameListCount = 0;

	OptionPreferences prefs;
	const AsciiString relayHost = prefs.getRelayAddress();

	if (relayHost.isEmpty())
		return;		// No relay configured - ordinary LAN behaviour.

	if (port != RELAY_LOBBY_PORT && port != RELAY_GAME_PORT)
	{
		DEBUG_LOG(("Transport::initRelay - not relaying port %d; the relay only carries the lobby (%d) and the game (%d)",
			port, RELAY_LOBBY_PORT, RELAY_GAME_PORT));
		return;
	}

	// Two instances on one machine share the port and are told apart only by a per-instance
	// 127.x bind, which the wildcard bind relay mode needs would collide with. They are on the
	// same machine anyway, so they have nothing to gain from a relay.
	if (rts::ClientInstance::isMultiInstance())
	{
		DEBUG_LOG(("Transport::initRelay - relay disabled: multi-instance clients need the narrow per-instance bind"));
		return;
	}

	const UnsignedInt relayAddr = ResolveIP(relayHost);
	if (relayAddr == 0 || relayAddr == INADDR_NONE)
	{
		DEBUG_LOG(("Transport::initRelay - relay disabled: could not resolve RelayAddress '%s'", relayHost.str()));
		return;
	}

	// Only OUR identity is configured now. The destination of each packet comes from the game
	// itself - peers learn each other's virtual addresses from the slot list, exactly as they
	// would learn real ones on a LAN - so there is no longer a PeerVirtualIP to keep in sync, and
	// no two-player assumption baked into the configuration.
	const UnsignedInt localVirtualIP = prefs.getLocalVirtualIP();
	if (localVirtualIP == 0)
	{
		DEBUG_LOG(("Transport::initRelay - relay disabled: LocalVirtualIP must be set to this machine's virtual 10.x address"));
		return;
	}

	AsciiString room = prefs.getRelayRoom();
	if (room.isEmpty())
	{
		room = "default";
		DEBUG_LOG(("Transport::initRelay - no RelayRoom set, using '%s'; anyone else on this relay in the default room would be paired with us", room.str()));
	}

	// Set the identity before building, since the registration carries it. The room may be
	// replaced later by setRelayRoom when a game is picked out of the browser.
	m_localVirtualIP = localVirtualIP;
	if (!buildRelayRegistration(room.str()))
		return;

	m_relayAddr = relayAddr;
	m_relayEnabled = TRUE;

	DEBUG_LOG(("Transport::initRelay - relaying through %d.%d.%d.%d in room '%s'; we are %d.%d.%d.%d",
		PRINTF_IP_AS_4_INTS(m_relayAddr), m_relayRoom, PRINTF_IP_AS_4_INTS(m_localVirtualIP)));
}

// GeneralsX @feature Relay transport. The header is written and read here rather than through
// queueSend, for the same reason sendRelayRegistration bypasses it: everything that goes through
// the normal path is CRC'd and XOR-encrypted, and the relay has to be able to read the destination
// without being able to interpret - or corrupt - anything else in the packet.
//
// Fields are big-endian on the wire so the relay, which is not C++ and does not share this
// struct, can read them with a plain readUInt32BE and the two ends cannot disagree about byte
// order across four client platforms.
Int Transport::writeRelayHeader( UnsignedByte *dest, UnsignedInt dstVirtualIP ) const
{
	dest[0] = 'G'; dest[1] = 'X'; dest[2] = 'R'; dest[3] = '1';
	dest[4]  = (UnsignedByte)((m_localVirtualIP >> 24) & 0xFF);
	dest[5]  = (UnsignedByte)((m_localVirtualIP >> 16) & 0xFF);
	dest[6]  = (UnsignedByte)((m_localVirtualIP >>  8) & 0xFF);
	dest[7]  = (UnsignedByte)( m_localVirtualIP        & 0xFF);
	dest[8]  = (UnsignedByte)((dstVirtualIP    >> 24) & 0xFF);
	dest[9]  = (UnsignedByte)((dstVirtualIP    >> 16) & 0xFF);
	dest[10] = (UnsignedByte)((dstVirtualIP    >>  8) & 0xFF);
	dest[11] = (UnsignedByte)( dstVirtualIP           & 0xFF);
	return GX_RELAY_HEADER_SIZE;
}

Bool Transport::readRelayHeader( const UnsignedByte *src, Int len, UnsignedInt &srcVirtualIP ) const
{
	if (len < GX_RELAY_HEADER_SIZE)
		return FALSE;

	if (src[0] != 'G' || src[1] != 'X' || src[2] != 'R' || src[3] != '1')
		return FALSE;

	srcVirtualIP = ((UnsignedInt)src[4] << 24) | ((UnsignedInt)src[5] << 16)
	             | ((UnsignedInt)src[6] <<  8) | ((UnsignedInt)src[7]);
	return TRUE;
}

// GeneralsX @feature Relay transport. Written straight to the socket rather than through
// queueSend, which would CRC and encrypt it: the relay has to be able to spot this packet without
// understanding anything else on the wire.
void Transport::sendRelayRegistration()
{
	if (!m_relayEnabled || !m_udpsock || m_relayRegistrationLen == 0)
		return;

	m_udpsock->Write((const unsigned char *)m_relayRegistration, m_relayRegistrationLen, m_relayAddr, m_port);

	// The game advertisement rides the same keepalive. That is not laziness: the list entry and
	// the NAT mapping have to expire together, or the browser shows games nobody can reach.
	if (m_advertisementLen > 0)
		m_udpsock->Write((const unsigned char *)m_advertisement, m_advertisementLen, m_relayAddr, m_port);

	m_lastRelayRegistration = timeGetTime();
}

/// Build (or rebuild) the registration datagram for a room, and remember the room so an
/// advertisement can be built against it later.
Bool Transport::buildRelayRegistration( const char *room )
{
	// The room token travels in a plaintext, space-separated line, so keep it to one word and to
	// characters that cannot be mistaken for a field separator.
	Int roomLen = 0;
	for (const char *c = room; (*c != '\0') && (roomLen < RELAY_MAX_ROOM_LEN); ++c)
	{
		const Bool safe = isalnum((unsigned char)*c) || (*c == '-') || (*c == '_') || (*c == '.');
		m_relayRoom[roomLen++] = safe ? *c : '_';
	}
	m_relayRoom[roomLen] = '\0';

	// Our own virtual address doubles as the client id: it is the one thing that already differs
	// between the machines, and it makes the relay's log read like the lobby's.
	const Int len = snprintf(m_relayRegistration, sizeof(m_relayRegistration), "%s %s %d.%d.%d.%d",
		RELAY_REGISTRATION_TAG, m_relayRoom, PRINTF_IP_AS_4_INTS(m_localVirtualIP));

	if (len <= 0 || len >= (Int)sizeof(m_relayRegistration))
	{
		DEBUG_LOG(("Transport::buildRelayRegistration - registration for room '%s' does not fit", m_relayRoom));
		m_relayRegistrationLen = 0;
		return FALSE;
	}

	m_relayRegistrationLen = len;
	return TRUE;
}

Bool Transport::setRelayRoom( const char *room )
{
	if (!m_relayEnabled || room == nullptr || *room == '\0')
		return FALSE;

	if (!buildRelayRegistration(room))
		return FALSE;

	// Announce at once rather than waiting out the keepalive. The caller is about to try to reach
	// a peer through the new room, and the relay cannot forward for a member it has not heard from
	// - so a silent 5 s gap here would look exactly like an unreachable host.
	sendRelayRegistration();

	DEBUG_LOG(("Transport::setRelayRoom - now in room '%s'", m_relayRoom));
	return TRUE;
}

void Transport::setGameAdvertisement( const char *name, const char *map, Int players, Int slots )
{
	clearGameAdvertisement();

	if (!m_relayEnabled || m_relayRoom[0] == '\0')
		return;

	// "<name>|<map>" is the trailing free-form field, so spaces are fine but a '|' inside the name
	// would move the boundary and the browser would show a truncated name against the wrong map.
	char safeName[96];
	char safeMap[160];
	Int n = 0;
	for (const char *c = (name ? name : ""); *c && n < (Int)sizeof(safeName) - 1; ++c)
		safeName[n++] = (*c == '|' || *c == '\n' || *c == '\r') ? '/' : *c;
	safeName[n] = '\0';

	n = 0;
	for (const char *c = (map ? map : ""); *c && n < (Int)sizeof(safeMap) - 1; ++c)
		safeMap[n++] = (*c == '|' || *c == '\n' || *c == '\r') ? '/' : *c;
	safeMap[n] = '\0';

	const Int len = snprintf(m_advertisement, sizeof(m_advertisement), "%s %s %d.%d.%d.%d %d %d %s|%s",
		RELAY_ADVERTISE_TAG, m_relayRoom, PRINTF_IP_AS_4_INTS(m_localVirtualIP),
		players, slots, safeName, safeMap);

	if (len > 0 && len < (Int)sizeof(m_advertisement))
		m_advertisementLen = len;
}

void Transport::clearGameAdvertisement()
{
	m_advertisement[0] = '\0';
	m_advertisementLen = 0;
}

void Transport::requestGameList()
{
	if (!m_relayEnabled || !m_udpsock)
		return;

	// Clear first: the reply is a burst of one datagram per game, so the list is rebuilt from
	// scratch each time rather than merged, and a game that has gone simply does not reappear.
	m_gameListCount = 0;

	static const char query[] = "GXLIST";
	m_udpsock->Write((const unsigned char *)query, (Int)(sizeof(query) - 1), m_relayAddr, m_port);
}

Bool Transport::captureGameListReply( const UnsignedByte *msg, Int len )
{
	static const char tag[] = "GXGAME ";
	static const Int tagLen = (Int)(sizeof(tag) - 1);

	if (len <= tagLen || len >= 512 || memcmp(msg, tag, tagLen) != 0)
		return FALSE;

	// Copy to a NUL-terminated scratch buffer: what is on the wire is not a C string.
	char line[512];
	Int copy = len - tagLen;
	if (copy > (Int)sizeof(line) - 1)
		copy = (Int)sizeof(line) - 1;
	memcpy(line, msg + tagLen, copy);
	line[copy] = '\0';

	// "<room> <hostIP> <players> <slots> <name>|<map>". The relay sends a single placeholder row
	// with room "-" when it has nothing to list, which parses and is then ignored below.
	char room[32] = { 0 };
	char hostIP[24] = { 0 };
	Int players = 0, slots = 0;
	Int consumed = 0;
	if (sscanf(line, "%31s %23s %d %d %n", room, hostIP, &players, &slots, &consumed) < 4)
		return TRUE;	// malformed, but it WAS ours - swallow it rather than parse it as a packet

	if (room[0] == '-' && room[1] == '\0')
		return TRUE;	// "nothing listed" placeholder

	if (m_gameListCount >= MAX_RELAY_LISTINGS)
		return TRUE;

	RelayGameListing &entry = m_gameList[m_gameListCount];
	memset(&entry, 0, sizeof(entry));
	strlcpy(entry.room, room, sizeof(entry.room));
	entry.hostVirtualIP = ResolveIP(AsciiString(hostIP));
	entry.players = players;
	entry.slots = slots;

	const char *rest = (consumed > 0 && consumed <= copy) ? (line + consumed) : "";
	const char *bar = strchr(rest, '|');
	if (bar != nullptr)
	{
		Int nameLen = (Int)(bar - rest);
		if (nameLen > (Int)sizeof(entry.name) - 1)
			nameLen = (Int)sizeof(entry.name) - 1;
		memcpy(entry.name, rest, nameLen);
		entry.name[nameLen] = '\0';
		strlcpy(entry.map, bar + 1, sizeof(entry.map));
	}
	else
	{
		strlcpy(entry.name, rest, sizeof(entry.name));
	}

	++m_gameListCount;
	return TRUE;
}

Transport::~Transport()
{
	reset();
}

Bool Transport::init( AsciiString ip, UnsignedShort port )
{
	return init(ResolveIP(ip), port);
}

Bool Transport::init( UnsignedInt ip, UnsignedShort port )
{
	// ----- Initialize Winsock -----
	if (!m_winsockInit)
	{
		WORD verReq = MAKEWORD(2, 2);
		WSADATA wsadata;

		int err = WSAStartup(verReq, &wsadata);
		if (err != 0) {
			return false;
		}

		if ((LOBYTE(wsadata.wVersion) != 2) || (HIBYTE(wsadata.wVersion) !=2)) {
			WSACleanup();
			return false;
		}
		m_winsockInit = true;
	}

	// ------- Bind our port --------
	delete m_udpsock;
	m_udpsock = NEW UDP();

	if (!m_udpsock)
		return false;

	// GeneralsX @bugfix A socket bound to a single unicast address is never handed a datagram
	// addressed to the subnet broadcast, so the LAN lobby could announce itself but never hear
	// anyone answer: the game browser stayed empty and joins timed out. Bind the wildcard
	// address so broadcast discovery is actually delivered. Only the bind moves - `ip` remains
	// the game's identity everywhere it matters (LANAPI::AmIHost, the senderIP == m_localIP
	// self-filter in LANAPI::update, slot matching), and moving that would break the lobby far
	// worse. Windows hands broadcasts to a specifically-bound socket already, so it keeps the
	// address it asked for. Multi-instance clients are the case that still needs the narrow
	// bind: they share port 8086 and are told apart only by a per-instance 127.x address, so a
	// wildcard bind would fail the second instance with EADDRINUSE.
	UnsignedInt bindIP = ip;
#ifndef _WIN32
	if (!rts::ClientInstance::isMultiInstance())
	{
		bindIP = INADDR_ANY;
	}
#endif

	// GeneralsX @feature Relay transport. In relay mode every datagram arrives from the relay's
	// public address, which is on no local subnet, so the socket must be the wildcard on Windows
	// too - `ip` is now a virtual LAN address and nothing would ever be delivered to it. This one
	// bind covers both sockets: the lobby one via LANAPI::SetLocalIP and the in-game one via
	// ConnectionManager::initTransport.
	initRelay(port);
	if (m_relayEnabled)
	{
		bindIP = INADDR_ANY;
	}

	int retval = -1;
	time_t now = timeGetTime();
	while ((retval != 0) && ((timeGetTime() - now) < 1000)) {
		retval = m_udpsock->Bind(bindIP, port);
	}

	if (retval != 0) {
		DEBUG_CRASH(("Could not bind to 0x%8.8X:%d", bindIP, port));
		DEBUG_LOG(("Transport::init - Failure to bind socket 0x%8.8X:%d (local IP 0x%8.8X) with error code %x", bindIP, port, ip, retval));
		delete m_udpsock;
		m_udpsock = nullptr;
		return false;
	}

	// ------- Clear buffers --------
	int i=0;
	for (; i<MAX_MESSAGES; ++i)
	{
		m_outBuffer[i].length = 0;
		m_inBuffer[i].length = 0;
#if defined(RTS_DEBUG)
		m_delayedInBuffer[i].message.length = 0;
#endif
	}
	for (i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		m_incomingBytes[i] = 0;
		m_outgoingBytes[i] = 0;
		m_unknownBytes[i] = 0;
		m_incomingPackets[i] = 0;
		m_outgoingPackets[i] = 0;
		m_unknownPackets[i] = 0;
	}
	m_statisticsSlot = 0;
	m_lastSecond = timeGetTime();

	m_port = port;

	// GeneralsX @feature Relay transport. Register before anything else goes out so the relay can
	// pair the room and open the return path ahead of the first game datagram.
	sendRelayRegistration();

#if defined(RTS_DEBUG)
	if (TheGlobalData->m_latencyAverage > 0 || TheGlobalData->m_latencyNoise)
		m_useLatency = true;

	if (TheGlobalData->m_packetLoss)
		m_usePacketLoss = true;
#endif

	return true;
}

void Transport::reset()
{
	delete m_udpsock;
	m_udpsock = nullptr;

	if (m_winsockInit)
	{
		WSACleanup();
		m_winsockInit = false;
	}
}

Bool Transport::update()
{
	Bool retval = TRUE;
	if (doRecv() == FALSE && m_udpsock && m_udpsock->GetStatus() == UDP::ADDRNOTAVAIL)
	{
		retval = FALSE;
	}
	DEBUG_ASSERTLOG(retval, ("WSA error is %s", GetWSAErrorString(WSAGetLastError()).str()));
	if (doSend() == FALSE && m_udpsock && m_udpsock->GetStatus() == UDP::ADDRNOTAVAIL)
	{
		retval = FALSE;
	}
	DEBUG_ASSERTLOG(retval, ("WSA error is %s", GetWSAErrorString(WSAGetLastError()).str()));
	return retval;
}

Bool Transport::doSend() {
	if (!m_udpsock)
	{
		DEBUG_LOG(("Transport::doSend() - m_udpSock is null!"));
		return FALSE;
	}

	Bool retval = TRUE;

	// Statistics gathering
	UnsignedInt now = timeGetTime();
	if (m_lastSecond + 1000 < now)
	{
		m_lastSecond = now;
		m_statisticsSlot = (m_statisticsSlot + 1) % MAX_TRANSPORT_STATISTICS_SECONDS;
		m_outgoingPackets[m_statisticsSlot] = 0;
		m_outgoingBytes[m_statisticsSlot] = 0;
		m_incomingPackets[m_statisticsSlot] = 0;
		m_incomingBytes[m_statisticsSlot] = 0;
		m_unknownPackets[m_statisticsSlot] = 0;
		m_unknownBytes[m_statisticsSlot] = 0;
	}

	// GeneralsX @feature Relay transport keepalive. Re-registering on a timer teaches the relay a
	// new public address if our NAT rebinds, and holds the mapping open through a quiet lobby.
	// This sits in doSend rather than update() because ConnectionManager drives the in-game socket
	// by calling doSend/doRecv directly and never calls update().
	if (m_relayEnabled && ((now - m_lastRelayRegistration) >= RELAY_REGISTRATION_INTERVAL))
	{
		sendRelayRegistration();
	}

	// GeneralsX @feature Relay transport. Staging buffer for the relay header plus the packet.
	// Declared once for the whole sweep rather than per message: this loop runs MAX_MESSAGES
	// times every send and the buffer is a kilobyte.
	UnsignedByte relayScratch[GX_RELAY_HEADER_SIZE + sizeof(TransportMessage)];

	// Send all messages
	int i;
	for (i=0; i<MAX_MESSAGES; ++i)
	{
		if (m_outBuffer[i].length != 0)
		{
			int bytesSent = 0;
			// TheSuperHackers @info The handling of data sizing of the payload within a UDP packet is confusing due to the current networking implementation
			// The max game packet size needs to be smaller than max udp payload by sizeof(TransportMessageHeader)
			// But the max network message size needs to include the bytes of the transport message header and equal the max udp payload
			// Therefore, transmitted data needs to add the extra bytes of the network header to the payloads length
			int bytesToSend = m_outBuffer[i].length + sizeof(TransportMessageHeader);

			// GeneralsX @feature Relay transport. The datagram goes out to the relay instead of
			// to the peer, and the address the game chose - a virtual LAN address, or the LAN
			// broadcast - travels in the relay header so the relay can deliver it to that one
			// peer, or to the whole room if it is a broadcast. The PORT is deliberately left
			// alone: it is what lets one relay keep lobby (8086) and in-game (8088) traffic
			// apart without ever looking inside a packet.
			const UnsignedInt destAddr = m_relayEnabled ? m_relayAddr : m_outBuffer[i].addr;

			// Send this message
			if (m_relayEnabled)
			{
				const Int headerLen = writeRelayHeader(relayScratch, m_outBuffer[i].addr);
				memcpy(relayScratch + headerLen, &m_outBuffer[i], bytesToSend);

				bytesSent = m_udpsock->Write(relayScratch, bytesToSend + headerLen, destAddr, m_outBuffer[i].port);

				// Report GAME bytes, so the short-write check below compares like with like and
				// the statistics stay directly comparable to a LAN run.
				if (bytesSent > 0)
					bytesSent -= headerLen;
			}
			else
			{
				bytesSent = m_udpsock->Write((unsigned char *)(&m_outBuffer[i]), bytesToSend, destAddr, m_outBuffer[i].port);
			}

			if (bytesSent > 0)
			{
				//DEBUG_LOG(("Sending %d bytes to %d.%d.%d.%d:%d", bytesToSend, PRINTF_IP_AS_4_INTS(m_outBuffer[i].addr), m_outBuffer[i].port));
				m_outgoingPackets[m_statisticsSlot]++;
				m_outgoingBytes[m_statisticsSlot] += m_outBuffer[i].length + sizeof(TransportMessageHeader);
				m_outBuffer[i].length = 0;  // Remove from queue
				if (bytesSent != bytesToSend)
				{
					DEBUG_LOG(("Transport::doSend - wanted to send %d bytes, only sent %d bytes to %d.%d.%d.%d:%d",
						bytesToSend, bytesSent,
						PRINTF_IP_AS_4_INTS(destAddr), m_outBuffer[i].port));
				}
			}
			else
			{
				// GeneralsX @bugfix "Not discarding message" is right only for a transient
				// failure. A hard one - EHOSTUNREACH (what Darwin returns for a limited
				// broadcast to 255.255.255.255, i.e. every LAN discovery send), EACCES,
				// ENETUNREACH, EADDRNOTAVAIL - will never succeed on retry, so retaining the
				// message pinned one of MAX_MESSAGES slots for the rest of the process. Enough
				// of those and the send queue is full of corpses and real traffic is dropped.
				// Retain only when the socket is genuinely just busy.
				const UDP::sockStat sockStatus = m_udpsock->GetStatus();
				const Bool transient = (sockStatus == UDP::OK)         // no error recorded
				                    || (sockStatus == UDP::WOULDBLOCK)
				                    || (sockStatus == UDP::AGAIN)
				                    || (sockStatus == UDP::INTR)
				                    || (sockStatus == UDP::INPROGRESS);
				if (!transient)
				{
					DEBUG_LOG(("Transport::doSend - dropping undeliverable message to %d.%d.%d.%d:%d (status %d)",
						PRINTF_IP_AS_4_INTS(destAddr), m_outBuffer[i].port, (Int)sockStatus));
					m_outBuffer[i].length = 0;  // Remove from queue; retrying cannot help
				}
				retval = FALSE;
			}
		}
	}

#if defined(RTS_DEBUG)
	// Latency simulation - deliver anything we're holding on to that is ready
	if (m_useLatency)
	{
		for (i=0; i<MAX_MESSAGES; ++i)
		{
			if (m_delayedInBuffer[i].message.length != 0 && m_delayedInBuffer[i].deliveryTime <= now)
			{
				for (int j=0; j<MAX_MESSAGES; ++j)
				{
					if (m_inBuffer[j].length == 0)
					{
						// Empty slot; use it
						memcpy(&m_inBuffer[j], &m_delayedInBuffer[i].message, sizeof(TransportMessage));
						m_delayedInBuffer[i].message.length = 0;
						break;
					}
				}
			}
		}
	}
#endif
	return retval;
}

Bool Transport::doRecv()
{
	if (!m_udpsock)
	{
		DEBUG_LOG(("Transport::doRecv() - m_udpSock is null!"));
		return FALSE;
	}

	Bool retval = TRUE;

	// Read in anything on our socket
	sockaddr_in from;
#if defined(RTS_DEBUG)
	UnsignedInt now = timeGetTime();
#endif
	// TheSuperHackers @info The handling of data sizing of the payload within a UDP packet is confusing due to the current networking implementation
	// The max game packet size needs to be smaller than max udp payload by sizeof(TransportMessageHeader)
	// But the max network message size needs to include the bytes of the transport message header and equal the max udp payload
	// Therefore, when receiving data we use the max udp payload size to receive the game packet payload and network header
	TransportMessage incomingMessage;
	unsigned char *buf = (unsigned char *)&incomingMessage;
	int len = MAX_NETWORK_MESSAGE_LEN;

	// GeneralsX @feature Relay transport. Read into a staging buffer rather than straight into
	// incomingMessage: a relayed datagram arrives with GX_RELAY_HEADER_SIZE bytes of routing
	// ahead of the game packet, and the packet has to be decrypted and inspected without them.
	// The extra copy is one sub-kilobyte memcpy against a CRC and an XOR pass over the same bytes.
	UnsignedByte recvScratch[MAX_NETWORK_MESSAGE_LEN];
	int rawLen = 0;
//	DEBUG_LOG(("Transport::doRecv - checking"));
	while ( (rawLen=m_udpsock->Read(recvScratch, MAX_NETWORK_MESSAGE_LEN, &from)) > 0 )
	{
#if defined(RTS_DEBUG)
		// Packet loss simulation
		if (m_usePacketLoss)
		{
			if ( TheGlobalData->m_packetLoss >= GameClientRandomValue(0, 100) )
			{
				continue;
			}
		}
#endif

//		DEBUG_LOG(("Transport::doRecv - Got something! len = %d", len));
		// Decrypt the packet
//		DEBUG_LOG_RAW(("buffer = "));
//		for (Int munkee = 0; munkee < len; ++munkee) {
//			DEBUG_LOG_RAW(("%02x", *(buf + munkee)));
//		}
//		DEBUG_LOG_RAW(("\n"));
		// GeneralsX @feature Relay transport. Strip our routing header and take the sender's
		// identity from it. On a LAN the sender is the socket's source address; through a relay
		// EVERY datagram arrives from the relay, which is not an identity anything above this
		// layer can match against a slot - so the sender's virtual address travels in the header.
		//
		// This replaced a single configured peer address, which was unambiguous for two machines
		// and wrong for any more: with eight players every packet was attributed to one peer.
		// GeneralsX @feature Server list. The relay's replies share this socket and are not game
		// packets, so they have to be taken off the wire before the parser sees them - otherwise
		// they are counted as unknown packets and the browser never fills in.
		if (m_relayEnabled && captureGameListReply(recvScratch, rawLen))
			continue;

		const UnsignedByte *packet = recvScratch;
		len = rawLen;
		UnsignedInt senderAddr = ntohl(from.sin_addr.s_addr);

		if (m_relayEnabled)
		{
			UnsignedInt srcVirtualIP = 0;
			if (!readRelayHeader(packet, len, srcVirtualIP))
			{
				// On a relayed socket anything without our header is not ours - a stray, a
				// scanner, or relay control traffic that bounced back. Count it as unknown
				// rather than trying to interpret it as a game packet.
				m_unknownPackets[m_statisticsSlot]++;
				m_unknownBytes[m_statisticsSlot] += len;
				continue;
			}
			senderAddr = srcVirtualIP;
			packet += GX_RELAY_HEADER_SIZE;
			len -= GX_RELAY_HEADER_SIZE;
		}

		// Bound the length BEFORE copying into a fixed-size struct. A valid packet is at most one
		// full payload plus its header, and incomingMessage is sized for exactly that; a datagram
		// larger than it - corrupt, hostile, or from a build with a different payload cap - would
		// otherwise overrun it. The raw read is already bounded, this bounds the copy.
		if (len > (Int)(MAX_PACKET_SIZE + sizeof(TransportMessageHeader)))
		{
			DEBUG_LOG(("Transport::doRecv - oversize packet! len = %d", len));
			m_unknownPackets[m_statisticsSlot]++;
			m_unknownBytes[m_statisticsSlot] += len;
			continue;
		}

		memcpy(buf, packet, len);
		decryptBuf(buf, len);

		incomingMessage.length = len - sizeof(TransportMessageHeader);

		if (len <= sizeof(TransportMessageHeader) || !isGeneralsPacket( &incomingMessage ))
		{
			DEBUG_LOG(("Transport::doRecv - unknownPacket! len = %d", len));
			m_unknownPackets[m_statisticsSlot]++;
			m_unknownBytes[m_statisticsSlot] += len;
			continue;
		}

		// Something there; stick it somewhere
//		DEBUG_LOG(("Saw %d bytes from %d:%d", len, ntohl(from.sin_addr.S_un.S_addr), ntohs(from.sin_port)));
		m_incomingPackets[m_statisticsSlot]++;
		m_incomingBytes[m_statisticsSlot] += len;

		for (int i=0; i<MAX_MESSAGES; ++i)
		{
#if defined(RTS_DEBUG)
			// Latency simulation
			if (m_useLatency)
			{
				if (m_delayedInBuffer[i].message.length == 0)
				{
					// Empty slot; use it
					m_delayedInBuffer[i].deliveryTime =
						now + TheGlobalData->m_latencyAverage +
						(Int)(TheGlobalData->m_latencyAmplitude * sin(now * TheGlobalData->m_latencyPeriod)) +
						GameClientRandomValue(-TheGlobalData->m_latencyNoise, TheGlobalData->m_latencyNoise);
					m_delayedInBuffer[i].message.length = incomingMessage.length;
					// GeneralsX @bugfix Use POSIX s_addr here too - winsock #defines s_addr onto
					// the S_un union, so it is the spelling that compiles on both, and this copy
					// of the assignment was left behind when the one below was fixed.
					// GeneralsX @feature Relay transport. Same sender resolution as the live path
					// below, so simulated latency does not quietly opt out of it.
					m_delayedInBuffer[i].message.addr = senderAddr;
					m_delayedInBuffer[i].message.port = ntohs(from.sin_port);
					memcpy(&m_delayedInBuffer[i].message, buf, len);
					break;
				}
			}
			else
			{
#endif
				if (m_inBuffer[i].length == 0)
				{
					// Empty slot; use it
					m_inBuffer[i].length = incomingMessage.length;
					// GeneralsX @bugfix BenderAI 13/02/2026 Use POSIX s_addr (no S_un union on Linux)
					// GeneralsX @feature Relay transport. senderAddr is the socket source on a
					// LAN and the sender's virtual address through a relay - resolved once, above.
					// This has to stay ahead of the memcpy, which overwrites header and data but
					// stops short of these fields.
					m_inBuffer[i].addr = senderAddr;
					m_inBuffer[i].port = ntohs(from.sin_port);
					memcpy(&m_inBuffer[i], buf, len);
					break;
				}
#if defined(RTS_DEBUG)
			}
#endif
		}
		//DEBUG_ASSERTCRASH(i<MAX_MESSAGES, ("Message lost!"));
	}

	if (len == -1) {
		// there was a socket error trying to perform a read.
		//DEBUG_LOG(("Transport::doRecv returning FALSE"));
		retval = FALSE;
	}

	return retval;
}

Bool Transport::queueSend(UnsignedInt addr, UnsignedShort port, const UnsignedByte *buf, Int len /*,
						  NetMessageFlags flags, Int id */)
{
	int i;

	if (len < 1 || len > MAX_PACKET_SIZE)
	{
		DEBUG_LOG(("Transport::queueSend - Invalid Packet size"));
		return false;
	}

	for (i=0; i<MAX_MESSAGES; ++i)
	{
		if (m_outBuffer[i].length == 0)
		{
			// Insert data here
			m_outBuffer[i].length = len;
			memcpy(m_outBuffer[i].data, buf, len);
			m_outBuffer[i].addr = addr;
			m_outBuffer[i].port = port;
//			m_outBuffer[i].header.flags = flags;
//			m_outBuffer[i].header.id = id;
			m_outBuffer[i].header.magic = GENERALS_MAGIC_NUMBER;

			CRC crc;
			crc.computeCRC( (unsigned char *)(&(m_outBuffer[i].header.magic)), m_outBuffer[i].length + sizeof(TransportMessageHeader) - sizeof(UnsignedInt) );
//			DEBUG_LOG(("About to assign the CRC for the packet"));
			m_outBuffer[i].header.crc = crc.get();

			// Encrypt packet
//			DEBUG_LOG(("buffer: "));
			encryptBuf((unsigned char *)&m_outBuffer[i], len + sizeof(TransportMessageHeader));
//			DEBUG_LOG((""));

			return true;
		}
	}
	DEBUG_LOG(("Send Queue is getting full, dropping packets"));
	return false;
}

Bool Transport::isGeneralsPacket( TransportMessage *msg )
{
	if (!msg)
		return false;

	if (msg->length < 0 || msg->length > MAX_NETWORK_MESSAGE_LEN)
		return false;

	CRC crc;
//	crc.computeCRC( (unsigned char *)msg->data, msg->length );
	crc.computeCRC( (unsigned char *)(&(msg->header.magic)), msg->length + sizeof(TransportMessageHeader) - sizeof(UnsignedInt) );

	if (crc.get() != msg->header.crc)
		return false;

	if (msg->header.magic != GENERALS_MAGIC_NUMBER)
		return false;

	return true;
}

// Statistics ---------------------------------------------------
Real Transport::getIncomingBytesPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_incomingBytes[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getIncomingPacketsPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_incomingPackets[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getOutgoingBytesPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_outgoingBytes[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getOutgoingPacketsPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_outgoingPackets[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getUnknownBytesPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_unknownBytes[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getUnknownPacketsPerSecond()
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_unknownPackets[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}



