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

// Transport.h ///////////////////////////////////////////////////////////////
// Transport layer - a thin layer around a UDP socket, with queues.
// Author: Matthew D. Campbell, July 2001

#pragma once

#include "GameNetwork/udp.h"
#include "GameNetwork/NetworkDefs.h"

/**
 * The transport layer handles the UDP socket for the game, and will packetize and
 * de-packetize multiple ACK/CommandPacket/etc packets into larger aggregates.
 */
// we only ever allocate one of there, and it is quite large, so we really DON'T want
// it to be a MemoryPoolObject (srj)
class Transport //: public MemoryPoolObject
{
	//MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(Transport, "Transport")
public:

	Transport();
	~Transport();

	Bool init( AsciiString ip, UnsignedShort port );
	Bool init( UnsignedInt ip, UnsignedShort port );
	void reset();
	Bool update();									///< Call this once a GameEngine tick, regardless of whether the frame advances.

	Bool doRecv();		///< call this to service the receive packets
	Bool doSend();		///< call this to service the send queue.

	Bool queueSend(UnsignedInt addr, UnsignedShort port, const UnsignedByte *buf, Int len /*,
		NetMessageFlags flags, Int id */);				///< Queue a packet for sending to the specified address and port.  This will be sent on the next update() call.

	Bool allowBroadcasts(Bool val) { if (!m_udpsock) return false; return (m_udpsock->AllowBroadcasts(val))?true:false; }

	// Latency insertion and packet loss
	void setLatency( Bool val ) { m_useLatency = val; }
	void setPacketLoss( Bool val ) { m_usePacketLoss = val; }

	// Bandwidth metrics
	Real getIncomingBytesPerSecond();
	Real getIncomingPacketsPerSecond();
	Real getOutgoingBytesPerSecond();
	Real getOutgoingPacketsPerSecond();
	Real getUnknownBytesPerSecond();
	Real getUnknownPacketsPerSecond();

	TransportMessage m_outBuffer[MAX_MESSAGES];
	TransportMessage m_inBuffer[MAX_MESSAGES];

#if defined(RTS_DEBUG)
	DelayedTransportMessage m_delayedInBuffer[MAX_MESSAGES];
#endif

	UnsignedShort m_port;
private:
	Bool m_winsockInit;
	UDP *m_udpsock;

	// GeneralsX @feature Relay transport. With a relay configured, every datagram is bounced off
	// it and peers are known by virtual LAN addresses instead of their real ones. That keeps the
	// whole IP-as-identity stack above this layer - AmIHost, the slot list, the join handshake -
	// looking at the LAN it was written for, while the machines are on different networks and none
	// of them has forwarded a port.
	//
	// Each relayed datagram carries GX_RELAY_HEADER_SIZE bytes ahead of the game packet:
	//
	//     [ 'GXR1' (4) ][ source virtual IP (4) ][ destination virtual IP (4) ][ game packet ]
	//
	// written and read HERE, outside the CRC and the XOR, because the relay has to read the
	// destination without being able to interpret anything else. The destination is whatever
	// address the game already chose: every peer advertises its virtual IP in the slot list, so
	// the layers above are already addressing peers by virtual address and needed no change. A
	// broadcast destination is forwarded to the whole room, which is what LAN discovery expects.
	//
	// This is what lifts the relay past two players. Before it, every relayed packet was reported
	// as coming from one configured peer address, which is unambiguous for two machines and
	// useless for eight.
	void initRelay( UnsignedShort port );			///< Reads the relay config; relay mode is on only if all of it validates.
	void sendRelayRegistration();					///< Unencrypted hello/keepalive that tells the relay where we are.
	Bool buildRelayRegistration( const char *room );	///< (Re)builds the registration datagram for a room.

public:
	/// Move to a different relay room at runtime. A browsable game list means many rooms on one
	/// relay, so the room can no longer be a startup-only setting read from Options.ini: picking a
	/// game out of the browser has to be able to change it without a restart.
	Bool setRelayRoom( const char *room );

	/// The room we are registered in right now, "" when relay mode is off. The browser needs it to
	/// tell "the game I picked lives somewhere else" from "it is in the room I am already in", and
	/// only the second case may skip the re-identify/rebind dance.
	const char *getRelayRoom() const { return m_relayRoom; }

	/// Advertise this game to the relay's list, refreshed on the registration keepalive so it
	/// inherits that cadence for free. Passing nothing stops the advertisement, and the relay
	/// delists a game purely by not hearing about it - so a host that crashes drops off by itself.
	void setGameAdvertisement( const char *name, const char *map, Int players, Int slots );
	void clearGameAdvertisement();
	Bool isRelayEnabled() const { return m_relayEnabled; }

	/// One game as the relay described it.
	struct RelayGameListing
	{
		char room[32];
		UnsignedInt hostVirtualIP;		///< Host order. This is the address a joiner dials.
		Int players;
		Int slots;
		char name[96];
		char map[160];
	};
	static const Int MAX_RELAY_LISTINGS = 32;

	/// GeneralsX @feature Matchmaking. Ask the relay which virtual address we are in a room, over
	/// a throwaway socket, BEFORE any transport or lobby object exists.
	///
	/// The ordering is not negotiable. LANGameInfo snapshots the local IP when it is constructed,
	/// and AmIHost and slot matching read that snapshot rather than TheLAN - so an identity that
	/// arrives after SetLocalIP reaches nothing that matters. This is why it is a static on its
	/// own socket instead of a method on the live transport.
	///
	/// Returns FALSE if there is no relay configured or it did not answer, and the caller then
	/// falls back to the configured LocalVirtualIP - which is what keeps LAN play and existing
	/// hand-configured setups working unchanged.
	static Bool requestRelayIdentity( const char *relayHost, const char *room,
		UnsignedInt &outVirtualIP );

	/// Pin the identity and room that initRelay should use, overriding Options.ini. Set from the
	/// result of requestRelayIdentity before the transport is initialised.
	static void setAssignedIdentity( const char *room, UnsignedInt virtualIP );
	static void clearAssignedIdentity();

	/// GeneralsX @feature Matchmaking. The room THIS client hosts in.
	///
	/// One room holds one game: the relay keys its advertisement table by room name, so two hosts
	/// sharing a room are two hosts fighting over one listing - the later advertisement simply
	/// replaces the earlier one and everybody in the browser dials whoever won. Public matchmaking
	/// therefore needs a room per HOST, not a room per relay, which is what the single configured
	/// RelayRoom used to give.
	///
	/// A RelayRoom set in Options.ini is honoured unchanged: that is the explicit case, and it is
	/// how a group who already know each other arrange a private game nobody can wander into. When
	/// it is empty a token unique to this process is generated instead, once, and cached - so a
	/// host that backs out of the browser and hosts again keeps the room it was advertising in.
	static AsciiString getLocalHostRoom();

	/// Move into `room` before dialling a host that lives there: ask the relay for an identity in
	/// THAT room and pin it. Wraps requestRelayIdentity + setAssignedIdentity so the two can never
	/// drift apart - an address allocated in room A is somebody else's address in room B, and
	/// pinning the pair together is the only thing that stops that from being a silent collision.
	///
	/// The caller must still rebuild TheLAN and re-run SetLocalIP afterwards; see the ordering note
	/// on requestRelayIdentity.
	static Bool enterRelayRoom( const char *relayHost, const char *room, UnsignedInt &outVirtualIP );

	/// Ask the relay what games exist. Replies arrive asynchronously on this same socket, so the
	/// caller pumps for a moment and then reads the list; there is no blocking form on purpose.
	void requestGameList();

	/// The same query over a throwaway socket, for callers that have to browse BEFORE a transport
	/// exists. A joiner is in that position by definition now: it cannot ask the relay for an
	/// identity until it knows which room the game it wants is in, and it cannot know that without
	/// looking at the list first. Blocks for up to waitMs and returns how many games were parsed.
	static Int requestRelayGameList( const char *relayHost, RelayGameListing *out, Int maxOut,
		UnsignedInt waitMs );

	Int getGameListCount() const { return m_gameListCount; }
	const RelayGameListing *getGameListing( Int i ) const
		{ return (i >= 0 && i < m_gameListCount) ? &m_gameList[i] : nullptr; }

private:
	/// Consume a GXGAME reply. Returns TRUE if the datagram was one, so doRecv can skip it - these
	/// share the socket with game traffic but are not game packets and must not reach the parser.
	Bool captureGameListReply( const UnsignedByte *msg, Int len );

	RelayGameListing m_gameList[MAX_RELAY_LISTINGS];
	Int m_gameListCount;

	Int writeRelayHeader( UnsignedByte *dest, UnsignedInt dstVirtualIP ) const;	///< Returns bytes written.
	Bool readRelayHeader( const UnsignedByte *src, Int len, UnsignedInt &srcVirtualIP ) const;

	Bool m_relayEnabled;
	UnsignedInt m_relayAddr;						///< Host order. Stands in for every outbound destination.
	UnsignedInt m_localVirtualIP;					///< Host order. Our identity, stamped into every outgoing header.
	UnsignedInt m_lastRelayRegistration;
	char m_relayRegistration[64];					///< Prebuilt registration datagram. Never encrypted - see sendRelayRegistration.
	Int m_relayRegistrationLen;
	char m_relayRoom[32];							///< Kept so the room can be rebuilt into a new registration or advertisement.
	char m_advertisement[320];						///< Prebuilt GXADV datagram, empty when we are not hosting a listed game.
	Int m_advertisementLen;

	// Latency insertion and packet loss
	Bool m_useLatency;
	Bool m_usePacketLoss;

	// Bandwidth metrics
	UnsignedInt m_incomingBytes[MAX_TRANSPORT_STATISTICS_SECONDS];
	UnsignedInt m_unknownBytes[MAX_TRANSPORT_STATISTICS_SECONDS];
	UnsignedInt m_outgoingBytes[MAX_TRANSPORT_STATISTICS_SECONDS];
	UnsignedInt m_incomingPackets[MAX_TRANSPORT_STATISTICS_SECONDS];
	UnsignedInt m_unknownPackets[MAX_TRANSPORT_STATISTICS_SECONDS];
	UnsignedInt m_outgoingPackets[MAX_TRANSPORT_STATISTICS_SECONDS];
	Int m_statisticsSlot;
	UnsignedInt m_lastSecond;

	Bool isGeneralsPacket( TransportMessage *msg );
};
