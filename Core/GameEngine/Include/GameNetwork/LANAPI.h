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

// LANAPI.h ///////////////////////////////////////////////////////////////
// LANAPI singleton class - defines interface to LAN broadcast communications
// Author: Matthew D. Campbell, October 2001

#pragma once

#include "GameNetwork/Transport.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/LANPlayer.h"
#include "GameNetwork/LANGameInfo.h"
// GeneralsX @feature Claude 27/07/2026 SimIdVerdict for the LANAPI members below.
// No cycle: SimulationId.h only forward-declares struct SimIdWire, which is defined
// further down this file inside the pack(1) region.
#include "Common/Diagnostic/SimulationId.h"

//static const Int g_lanPlayerNameLength = 20;
static const Int g_lanPlayerNameLength = 12; // reduced length because of game option length
//static const Int g_lanLoginNameLength = 16;
//static const Int g_lanHostNameLength = 16;
static const Int g_lanLoginNameLength = 1;
static const Int g_lanHostNameLength = 1;
//static const Int g_lanGameNameLength = 32;
static const Int g_lanGameNameLength = 16; // reduced length because of game option length
static const Int g_lanGameNameReservedLength = 16; // save N wchars for ID info
static const Int g_lanMaxChatLength = 100;
static const Int m_lanMaxOptionsLength = MAX_LANAPI_PACKET_SIZE - ( 8 + (g_lanGameNameLength+1)*2 + 4 + (g_lanPlayerNameLength+1)*2
																														+ (g_lanLoginNameLength+1) + (g_lanHostNameLength+1) );
static const Int g_maxSerialLength = 23; // including the trailing '\0'

struct LANMessage;

/**
 * The LANAPI class is used to instantiate a singleton which
 * implements the interface to all LAN broadcast communications.
 */
class LANAPIInterface : public SubsystemInterface
{
public:

	virtual ~LANAPIInterface() override { };

	virtual void setIsActive(Bool isActive ) = 0;								///< Tell TheLAN whether or not the app is active.

	// Possible types of chat messages
	enum ChatType
	{
		LANCHAT_NORMAL = 0,
		LANCHAT_EMOTE,
		LANCHAT_SYSTEM,
	};

	// Request functions generate network traffic
	virtual void RequestLocations() = 0;																				///< Request everybody to respond with where they are
	virtual void RequestGameJoin( LANGameInfo *game, UnsignedInt ip = 0 ) = 0;				///< Request to join a game
	virtual void RequestGameJoinDirectConnect( UnsignedInt ipaddress ) = 0;						///< Request to join a game at an IP address
	virtual void RequestGameLeave() = 0;																				///< Tell everyone we're leaving
	virtual void RequestAccept() = 0;																						///< Indicate we're OK with the game options
	virtual void RequestHasMap() = 0;																						///< Send our map status
	virtual void RequestChat( UnicodeString message, ChatType format ) = 0;						///< Send a chat message
	virtual void RequestGameStart() = 0;																				///< Tell everyone the game is starting
	virtual void RequestGameStartTimer( Int seconds ) = 0;
	virtual void RequestGameOptions( AsciiString gameOptions, Bool isPublic, UnsignedInt ip = 0 ) = 0;		///< Change the game options
	virtual void RequestGameCreate( UnicodeString gameName, Bool isDirectConnect ) = 0;	///< Try to host a game
	virtual void RequestGameAnnounce() = 0;																			///< Sound out current game info if host
//	virtual void RequestSlotList() = 0;																					///< Pump out the Slot info.
	virtual void RequestSetName( UnicodeString newName ) = 0;													///< Pick a new name
	virtual void RequestLobbyLeave( Bool forced ) = 0;																///< Announce that we're leaving the lobby
	virtual void ResetGameStartTimer() = 0;

	// Possible result codes passed to On functions
	enum ReturnType
	{
		RET_OK,							// Any function
		RET_TIMEOUT,				// OnGameJoin/Leave/Start, etc
		RET_GAME_FULL,			// OnGameJoin
		RET_DUPLICATE_NAME,	// OnGameJoin
		RET_CRC_MISMATCH,		// OnGameJoin
		RET_SERIAL_DUPE,		// OnGameJoin
		RET_GAME_STARTED,		// OnGameJoin
		RET_GAME_EXISTS,		// OnGameCreate
		RET_GAME_GONE,			// OnGameJoin
		RET_BUSY,						// OnGameCreate/Join/etc if another action is in progress
		RET_UNKNOWN,				// Default message for oddity
	};
	UnicodeString getErrorStringFromReturnType( ReturnType ret );

	// On functions are (generally) the result of network traffic
	virtual void OnGameList( LANGameInfo *gameList ) = 0;																							///< List of games
	virtual void OnPlayerList( LANPlayer *playerList ) = 0;																				///< List of players in the Lobby
	virtual void OnGameJoin( ReturnType ret, LANGameInfo *theGame ) = 0;															///< Did we get in the game?
	virtual void OnPlayerJoin( Int slot, UnicodeString playerName ) = 0;													///< Someone else joined our game (host only; joiners get a slotlist)
	virtual void OnHostLeave() = 0;																													///< Host left the game
	virtual void OnPlayerLeave( UnicodeString player ) = 0;																				///< Someone left the game
	virtual void OnAccept( UnsignedInt playerIP, Bool status ) = 0;																///< Someone's accept status changed
	virtual void OnHasMap( UnsignedInt playerIP, Bool status ) = 0;																///< Someone's map status changed
	virtual void OnChat( UnicodeString player, UnsignedInt ip,
											 UnicodeString message, ChatType format ) = 0;														///< Chat message from someone
	virtual void OnGameStart() = 0;																													///< The game is starting
	virtual void OnGameStartTimer( Int seconds ) = 0;
	virtual void OnGameOptions( UnsignedInt playerIP, Int playerSlot, AsciiString options ) = 0;	///< Someone sent game options
	virtual void OnGameCreate( ReturnType ret ) = 0;																							///< Your game is created
//	virtual void OnSlotList( ReturnType ret, LANGameInfo *theGame ) = 0;															///< Slotlist for a game in setup
	virtual void OnNameChange( UnsignedInt IP, UnicodeString newName ) = 0;												///< Someone has morphed

	// Misc utility functions
	virtual LANGameInfo * LookupGame( UnicodeString gameName ) = 0;														///< return a pointer to a game we know about
	virtual LANGameInfo * LookupGameByListOffset( Int offset ) = 0;														///< return a pointer to a game we know about
	virtual LANGameInfo * LookupGameByHost( UnsignedInt hostIP ) = 0;													///< return a pointer to the most recent game associated to the host IP address
	virtual Bool SetLocalIP( UnsignedInt localIP ) = 0;																		///< For multiple NIC machines
	virtual void SetLocalIP( AsciiString localIP ) = 0;																		///< For multiple NIC machines
	virtual Bool AmIHost() = 0;																											///< Am I hosting a game?
	virtual inline UnicodeString GetMyName() = 0;																		///< What's my name?
	virtual inline LANGameInfo *GetMyGame() = 0;															          ///< What's my Game?
	virtual void fillInLANMessage( LANMessage *msg ) = 0;																	///< Fill in default params
	virtual void checkMOTD() = 0;
};


/**
 * LAN message class
 */
// GeneralsX @bugfix BenderAI 11/02/2026 - Use WideCharWindows for cross-platform LAN protocol (always 2 bytes)
// On Windows: wchar_t = 2 bytes (UTF-16), on Linux: wchar_t = 4 bytes (UTF-32)
// Network protocol must use fixed-size types, so use uint16_t for wide chars in LAN packets
typedef uint16_t WideCharWindows;

// GeneralsX @build BenderAI 13/02/2026 Helper functions for WideCharWindows <-> wchar_t conversion (fighter19 pattern)
#define MAX_COMPUTERNAME_LENGTH 256
void CopyWcharToWindowsWideChar( WideCharWindows *dest, const WideChar *src, UnsignedInt len );
wchar_t *GetWindowsWideCharAsWchar( WideCharWindows *src );

// GeneralsX @feature Claude 27/07/2026 Presence sentinel for the join-time compatibility
// identity. Written only when the local SimulationId is valid, so an engine that never
// finished init can never present a comparable identity, and an old peer's uninitialised
// stack bytes have a 2^-32 chance of reading as valid. Bump the last digit ('SID2')
// whenever SimIdWire or the engineID recipe changes, so old and new builds refuse each
// other with a distinct message instead of comparing incomparable hashes.
const UnsignedInt SIMID_WIRE_TAG = 0x53494431u; // 'SID1'

#pragma pack(push, 1)

// GeneralsX @feature Claude 27/07/2026 On-the-wire form of the SimID. Lives inside the
// pack(1) region so its 36 bytes carry no padding on any target, and so SimulationId.cpp
// (which includes this header) and the LAN handlers agree on the layout byte for byte.
struct SimIdWire
{
	UnsignedInt tag;         ///< SIMID_WIRE_TAG, or garbage/zero from a pre-SimID peer
	UnsignedInt revision;    ///< GitRevision, informational only
	UnsignedInt engineID;    ///< tier1
	UnsignedInt sourceID;    ///< tier1, 0 == unknown provenance
	UnsignedInt dataID;      ///< tier1
	UnsignedInt ordinalID;   ///< tier1
	UnsignedInt parseID;     ///< tier2 warn
	UnsignedInt assetID;     ///< tier2 warn
	UnsignedInt platformID;  ///< tier2 warn
};

struct LANMessage
{
	enum Type				          ///< What kind of message are we?
	{
		// Locating everybody
		MSG_REQUEST_LOCATIONS,	///< Hey, where is everybody?
		MSG_GAME_ANNOUNCE,			///< Here I am, and here's my game info!
		MSG_LOBBY_ANNOUNCE,			///< Hey, I'm in the lobby!

		// Joining games
		MSG_REQUEST_JOIN,				///< Let me in!  Let me in!
		MSG_JOIN_ACCEPT,				///< Okay, you can join.
		MSG_JOIN_DENY,					///< Go away!  We don't want any!

		// Leaving games
		MSG_REQUEST_GAME_LEAVE,	///< I want to leave the game
		MSG_REQUEST_LOBBY_LEAVE,///< I'm leaving the lobby

		// Game options, chat, etc
		MSG_SET_ACCEPT,					///< I'm cool with everything as is.
		MSG_MAP_AVAILABILITY,		///< I do (not) have the map.
		MSG_CHAT,								///< Just spouting my mouth off.
		MSG_GAME_START,					///< Hold on; we're starting!
		MSG_GAME_START_TIMER,		///< The game will start in N seconds
		MSG_GAME_OPTIONS,				///< Here's some info about the game.
		MSG_INACTIVE,						///< I've alt-tabbed out.  Unaccept me cause I'm a poo-flinging monkey.

		MSG_REQUEST_GAME_INFO,	///< For direct connect, get the game info from a specific IP Address
	} messageType;

	WideCharWindows name[g_lanPlayerNameLength+1]; ///< My name, for convenience
	char userName[g_lanLoginNameLength+1];	///< login name, for convenience
	char hostName[g_lanHostNameLength+1];		///< machine name, for convenience

	// No additional data is required for REQUEST_LOCATIONS, LOBBY_ANNOUNCE,
	// REQUEST_LOBBY_LEAVE, GAME_START.
	union
	{
		// StartTimer is sent with GAME_START_TIMER
		struct
		{
			Int seconds;
		} StartTimer;

		// GameJoined is sent with REQUEST_GAME_LEAVE
		struct
		{
			WideCharWindows gameName[g_lanGameNameLength+1];
		} GameToLeave;

		// GameInfo if sent with GAME_ANNOUNCE
		struct
		{
			WideCharWindows gameName[g_lanGameNameLength+1];
			Bool inProgress;
			char options[m_lanMaxOptionsLength+1];
			Bool isDirectConnect;
		} GameInfo;

		// PlayerInfo is sent with REQUEST_GAME_INFO for direct connect games.
		struct
		{
			UnsignedInt ip;
			WideCharWindows playerName[g_lanPlayerNameLength+1];
		} PlayerInfo;

		// GameToJoin is sent with REQUEST_JOIN
		struct
		{
			UnsignedInt gameIP;
			UnsignedInt exeCRC;
			UnsignedInt iniCRC;
			char serial[g_maxSerialLength];
			// GeneralsX @feature Claude 27/07/2026 Appended into the union's dead bytes, so
			// sizeof(LANMessage) is unchanged (GameInfo remains the largest arm). That is
			// load-bearing: sendMessage always queues sizeof(LANMessage) and the receive path
			// only tests length > 0 before blind-casting the buffer, so any size change would
			// make a new build read its new fields out of memory past a short old-peer datagram.
			SimIdWire sim;
		} GameToJoin;

		// GameJoined is sent with JOIN_ACCEPT
		struct
		{
			WideCharWindows gameName[g_lanGameNameLength+1];
			UnsignedInt gameIP;
			UnsignedInt playerIP;
			Int slotPosition;
			// GeneralsX @feature Claude 27/07/2026 The host's identity, so the joiner can verify
			// it independently - the host may be the un-upgraded machine. Appended last, so
			// GameJoined.playerIP keeps its offset (72) that handleJoinDeny relies on.
			SimIdWire sim;
		} GameJoined;

		// GameNotJoined is sent with JOIN_DENY
		struct
		{
			WideCharWindows gameName[g_lanGameNameLength+1];
			UnsignedInt gameIP;
			UnsignedInt playerIP;
			LANAPIInterface::ReturnType reason;
			// GeneralsX @feature Claude 27/07/2026 The host's identity travels on the deny too, so
			// the joiner's failure dialog can name the specific field that differs rather than
			// just saying "version mismatch". Appended last for the same offset reason as above.
			SimIdWire sim;
		} GameNotJoined;

		// Accept is sent with SET_ACCEPT
		struct
		{
			WideCharWindows gameName[g_lanGameNameLength+1];
			Bool isAccepted;
		} Accept;

		// Accept is sent with MAP_AVAILABILITY
		struct
		{
			WideCharWindows gameName[g_lanGameNameLength+1];
			UnsignedInt mapCRC;	// to make sure we're talking about the same map
			Bool hasMap;
		} MapStatus;

		// Chat is sent with CHAT
		struct
		{
			WideCharWindows gameName[g_lanGameNameLength+1];
			LANAPIInterface::ChatType chatType;
			WideCharWindows message[g_lanMaxChatLength+1];
		} Chat;

		// GameOptions is sent with GAME_OPTIONS
		struct
		{
			char options[m_lanMaxOptionsLength+1];
		} GameOptions;

	};
};
#pragma pack(pop)

static_assert(sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE, "LANMessage struct cannot be larger than the max packet size");


/**
 * The LANAPI class is used to instantiate a singleton which
 * implements the interface to all LAN broadcast communications.
 */
class LANAPI : public LANAPIInterface
{
public:

	LANAPI();
	virtual ~LANAPI() override;

	virtual void init() override;															///< Initialize or re-initialize the instance
	virtual void reset() override;															///< reset the logic system
	virtual void update() override;														///< update the world

	virtual void setIsActive(Bool isActive) override;								///< tell TheLAN whether or not

	// Request functions generate network traffic
	virtual void RequestLocations() override;																				///< Request everybody to respond with where they are
	virtual void RequestGameJoin( LANGameInfo *game, UnsignedInt ip = 0 ) override;				///< Request to join a game
	virtual void RequestGameJoinDirectConnect( UnsignedInt ipaddress ) override;						///< Request to join a game at an IP address
	virtual void RequestGameLeave() override;																				///< Tell everyone we're leaving
	virtual void RequestAccept() override;																						///< Indicate we're OK with the game options
	virtual void RequestHasMap() override;																						///< Send our map status
	virtual void RequestChat( UnicodeString message, ChatType format ) override;						///< Send a chat message
	virtual void RequestGameStart() override;																				///< Tell everyone the game is starting
	virtual void RequestGameStartTimer( Int seconds ) override;
	virtual void RequestGameOptions( AsciiString gameOptions, Bool isPublic, UnsignedInt ip = 0 ) override;		///< Change the game options
	virtual void RequestGameCreate( UnicodeString gameName, Bool isDirectConnect ) override;	///< Try to host a game
	virtual void RequestGameAnnounce() override;																			///< Send out game info if host
	virtual void RequestSetName( UnicodeString newName ) override;													///< Pick a new name
//	virtual void RequestSlotList();																					///< Pump out the Slot info.
	virtual void RequestLobbyLeave( Bool forced ) override;																///< Announce that we're leaving the lobby
	virtual void ResetGameStartTimer() override;

	// On functions are (generally) the result of network traffic
	virtual void OnGameList( LANGameInfo *gameList ) override;																							///< List of games
	virtual void OnPlayerList( LANPlayer *playerList ) override;																				///< List of players in the Lobby
	virtual void OnGameJoin( ReturnType ret, LANGameInfo *theGame ) override;															///< Did we get in the game?
	virtual void OnPlayerJoin( Int slot, UnicodeString playerName ) override;													///< Someone else joined our game (host only; joiners get a slotlist)
	virtual void OnHostLeave() override;																													///< Host left the game
	virtual void OnPlayerLeave( UnicodeString player ) override;																				///< Someone left the game
	virtual void OnAccept( UnsignedInt playerIP, Bool status ) override;																///< Someone's accept status changed
	virtual void OnHasMap( UnsignedInt playerIP, Bool status ) override;																///< Someone's map status changed
	virtual void OnChat( UnicodeString player, UnsignedInt ip,
											 UnicodeString message, ChatType format ) override;														///< Chat message from someone
	virtual void OnGameStart() override;																													///< The game is starting
	virtual void OnGameStartTimer( Int seconds ) override;
	virtual void OnGameOptions( UnsignedInt playerIP, Int playerSlot, AsciiString options ) override;	///< Someone sent game options
	virtual void OnGameCreate( ReturnType ret ) override;																							///< Your game is created
	//virtual void OnSlotList( ReturnType ret, LANGameInfo *theGame );															///< Slotlist for a game in setup
	virtual void OnNameChange( UnsignedInt IP, UnicodeString newName ) override;												///< Someone has morphed
	virtual void OnInActive( UnsignedInt IP );																								///< Someone has alt-tabbed out.


	// Misc utility functions
	virtual LANGameInfo * LookupGame( UnicodeString gameName ) override;														///< return a pointer to a game we know about
	virtual LANGameInfo * LookupGameByListOffset( Int offset ) override;														///< return a pointer to a game we know about
	virtual LANGameInfo * LookupGameByHost( UnsignedInt hostIP ) override;													///< return a pointer to the most recent game associated to the host IP address
	virtual LANPlayer * LookupPlayer( UnsignedInt playerIP );													///< return a pointer to a player we know about
	virtual Bool SetLocalIP( UnsignedInt localIP ) override;																		///< For multiple NIC machines
	virtual void SetLocalIP( AsciiString localIP ) override;																		///< For multiple NIC machines
	virtual Bool AmIHost() override;																											///< Am I hosting a game?
	virtual UnicodeString GetMyName() override { return m_name; }                 ///< What's my name?
	virtual LANGameInfo* GetMyGame() override { return m_currentGame; }					      ///< What's my Game?
	virtual UnsignedInt GetLocalIP() { return m_localIP; }								///< What's my IP?
	virtual void fillInLANMessage( LANMessage *msg ) override;																	///< Fill in default params
	virtual void checkMOTD() override;
protected:

	enum PendingActionType
	{
		ACT_NONE = 0,
		ACT_JOIN,
		ACT_JOINDIRECTCONNECT,
		ACT_LEAVE,
	};

	static const UnsignedInt s_resendDelta; // in ms

protected:
	LANPlayer *					m_lobbyPlayers;			///< List of players in the lobby
	LANGameInfo *				m_games;								///< List of games
	UnicodeString				m_name;							///< Who do we think we are?
	AsciiString					m_userName;						///< login name
	AsciiString					m_hostName;						///< machine name
	UnsignedInt					m_gameStartTime;
	Int									m_gameStartSeconds;

	PendingActionType		m_pendingAction;	///< What action are we performing?
	UnsignedInt					m_expiration;						///< When should we give up on our action?
	UnsignedInt					m_actionTimeout;
	UnsignedInt					m_directConnectRemoteIP;///< The IP address of the game we are direct connecting to.

	// Resend timer ---------------------------------------------------------------------------
	UnsignedInt					m_lastResendTime; // in ms

	Bool								m_isInLANMenu;		///< true while we are in a LAN menu (lobby, game options, direct connect)
	Bool								m_inLobby;											///< Are we in the lobby (not in a game)?
	LANGameInfo *				m_currentGame;							///< Pointer to game (setup screen) we are currently in (null for lobby)
	//LANGameInfo *m_currentGameInfo;			///< Pointer to game setup info we are currently in.

	UnsignedInt					m_localIP;
	Transport*					m_transport;

	UnsignedInt					m_broadcastAddr;

	UnsignedInt					m_lastUpdate;
	AsciiString					m_lastGameopt; /// @todo: hack for demo - remove this

	Bool								m_isActive;			///< is the game currently active?

	// GeneralsX @feature Claude 27/07/2026 Join-time compatibility identity of the last peer
	// we heard from, kept for the failure dialog (OnGameJoin's signature is part of the
	// LANAPIInterface virtual contract and cannot take extra parameters). All are initialised
	// in the constructor so no path can render stack garbage as a version number.
	// m_remoteSimValid is not derivable from m_remoteSimVerdict: SimIdCompare returns
	// SIMID_LOCAL_INVALID before it ever looks at the peer's tag, so on that path the
	// verdict cannot tell you whether the peer sent a real identity or not. Currently
	// written but not yet read - it exists so the dialog can distinguish "they are old"
	// from "we are broken and never asked them".
	Bool								m_remoteSimValid;
	SimIdWire						m_remoteSim;
	SimIdVerdict				m_remoteSimVerdict;
	// Tier2 warnings cannot be printed at the moment they are discovered on the joining
	// side: LANAPI::OnChat routes to listboxChatWindowLanGame, which LanGameOptionsMenuInit
	// does not assign until Shell::update() instantiates the layout, so anything emitted from
	// handleJoinAccept is dropped by OnChat's `if (chatWindow == nullptr) return;`. Stash and
	// flush from update() once the listbox exists.
	UnsignedInt					m_pendingSimWarnFlags;

protected:
	void sendMessage(LANMessage *msg, UnsignedInt ip = 0); // Convenience function
	// GeneralsX @feature Claude 27/07/2026 Emits the stashed tier2 warnings once the LAN game
	// options chat listbox exists; no-op while it does not. Defined in LANAPIhandlers.cpp,
	// which already has the LANAPICallbacks.h declaration of that listbox.
	void flushPendingSimWarnings();
	void removePlayer(LANPlayer *player);
	void removeGame(LANGameInfo *game);
	void addPlayer(LANPlayer *player);
	void addGame(LANGameInfo *game);
	AsciiString createSlotString();

	// Functions to handle incoming messages -----------------------------------
	void handleRequestLocations( LANMessage *msg, UnsignedInt senderIP );
	void handleGameAnnounce( LANMessage *msg, UnsignedInt senderIP );
	void handleLobbyAnnounce( LANMessage *msg, UnsignedInt senderIP );
	void handleRequestGameInfo( LANMessage *msg, UnsignedInt senderIP );
	void handleRequestJoin( LANMessage *msg, UnsignedInt senderIP );
	void handleJoinAccept( LANMessage *msg, UnsignedInt senderIP );
	void handleJoinDeny( LANMessage *msg, UnsignedInt senderIP );
	void handleRequestGameLeave( LANMessage *msg, UnsignedInt senderIP );
	void handleRequestLobbyLeave( LANMessage *msg, UnsignedInt senderIP );
	void handleSetAccept( LANMessage *msg, UnsignedInt senderIP );
	void handleHasMap( LANMessage *msg, UnsignedInt senderIP );
	void handleChat( LANMessage *msg, UnsignedInt senderIP );
	void handleGameStart( LANMessage *msg, UnsignedInt senderIP );
	void handleGameStartTimer( LANMessage *msg, UnsignedInt senderIP );
	void handleGameOptions( LANMessage *msg, UnsignedInt senderIP );
	void handleInActive( LANMessage *msg, UnsignedInt senderIP );

};
