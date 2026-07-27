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
	m_peerVirtualIP = 0;
	m_lastRelayRegistration = 0;
	m_relayRegistration[0] = '\0';
	m_relayRegistrationLen = 0;
}

// GeneralsX @feature Relay transport. Everything the relay needs is decided here, once per
// socket, and every check that fails leaves relay mode off rather than half on: a half-configured
// relay would send game traffic to nowhere while the lobby still believed it was on a LAN.
void Transport::initRelay( UnsignedShort port )
{
	m_relayEnabled = FALSE;
	m_relayAddr = 0;
	m_peerVirtualIP = 0;
	m_lastRelayRegistration = 0;
	m_relayRegistration[0] = '\0';
	m_relayRegistrationLen = 0;

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

	const UnsignedInt localVirtualIP = prefs.getLocalVirtualIP();
	const UnsignedInt peerVirtualIP = prefs.getPeerVirtualIP();
	if (localVirtualIP == 0 || peerVirtualIP == 0 || localVirtualIP == peerVirtualIP)
	{
		DEBUG_LOG(("Transport::initRelay - relay disabled: LocalVirtualIP and PeerVirtualIP must be two different 10.x addresses (got 0x%8.8X and 0x%8.8X)",
			localVirtualIP, peerVirtualIP));
		return;
	}

	AsciiString room = prefs.getRelayRoom();
	if (room.isEmpty())
	{
		room = "default";
		DEBUG_LOG(("Transport::initRelay - no RelayRoom set, using '%s'; anyone else on this relay in the default room would be paired with us", room.str()));
	}

	// The room token travels in a plaintext, space-separated line, so keep it to one word and to
	// characters that cannot be mistaken for a field separator.
	char roomToken[RELAY_MAX_ROOM_LEN + 1];
	Int roomLen = 0;
	for (const char *c = room.str(); (*c != '\0') && (roomLen < RELAY_MAX_ROOM_LEN); ++c)
	{
		const Bool safe = isalnum((unsigned char)*c) || (*c == '-') || (*c == '_') || (*c == '.');
		roomToken[roomLen++] = safe ? *c : '_';
	}
	roomToken[roomLen] = '\0';

	// Our own virtual address doubles as the client id: it is the one thing that already differs
	// between the two machines, and it makes the relay's log read like the lobby's.
	m_relayRegistrationLen = snprintf(m_relayRegistration, sizeof(m_relayRegistration), "%s %s %d.%d.%d.%d",
		RELAY_REGISTRATION_TAG, roomToken, PRINTF_IP_AS_4_INTS(localVirtualIP));

	if (m_relayRegistrationLen <= 0 || m_relayRegistrationLen >= (Int)sizeof(m_relayRegistration))
	{
		DEBUG_LOG(("Transport::initRelay - relay disabled: registration for room '%s' does not fit", roomToken));
		m_relayRegistrationLen = 0;
		return;
	}

	m_relayAddr = relayAddr;
	m_peerVirtualIP = peerVirtualIP;
	m_relayEnabled = TRUE;

	DEBUG_LOG(("Transport::initRelay - relaying through %d.%d.%d.%d in room '%s'; we are %d.%d.%d.%d, peer is %d.%d.%d.%d",
		PRINTF_IP_AS_4_INTS(m_relayAddr), roomToken,
		PRINTF_IP_AS_4_INTS(localVirtualIP), PRINTF_IP_AS_4_INTS(m_peerVirtualIP)));
}

// GeneralsX @feature Relay transport. Written straight to the socket rather than through
// queueSend, which would CRC and encrypt it: the relay has to be able to spot this packet without
// understanding anything else on the wire.
void Transport::sendRelayRegistration()
{
	if (!m_relayEnabled || !m_udpsock || m_relayRegistrationLen == 0)
		return;

	m_udpsock->Write((const unsigned char *)m_relayRegistration, m_relayRegistrationLen, m_relayAddr, m_port);
	m_lastRelayRegistration = timeGetTime();
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

			// GeneralsX @feature Relay transport. The message keeps the address the game chose -
			// a virtual LAN address, or the LAN broadcast - but goes out to the relay, which
			// forwards it to the other member of the room. The PORT is deliberately left alone:
			// it is what lets the relay keep lobby (8086) and in-game (8088) traffic apart
			// without ever looking inside a packet.
			const UnsignedInt destAddr = m_relayEnabled ? m_relayAddr : m_outBuffer[i].addr;

			// Send this message
			if ((bytesSent = m_udpsock->Write((unsigned char *)(&m_outBuffer[i]), bytesToSend, destAddr, m_outBuffer[i].port)) > 0)
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
//	DEBUG_LOG(("Transport::doRecv - checking"));
	while ( (len=m_udpsock->Read(buf, MAX_NETWORK_MESSAGE_LEN, &from)) > 0 )
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
					// GeneralsX @feature Relay transport. Same virtual peer address substitution
					// as the live path below, so simulated latency does not quietly opt out of it.
					m_delayedInBuffer[i].message.addr = m_relayEnabled ? m_peerVirtualIP : ntohl(from.sin_addr.s_addr);
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
					// GeneralsX @feature Relay transport. In relay mode the wire source is the
					// relay, which is not an identity anything above this layer can match against
					// a slot, so hand up the peer's virtual LAN address instead. Only two machines
					// can be in a room, so the peer is unambiguous. This has to stay ahead of the
					// memcpy, which overwrites header and data but stops short of these fields.
					m_inBuffer[i].addr = m_relayEnabled ? m_peerVirtualIP : ntohl(from.sin_addr.s_addr);
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



