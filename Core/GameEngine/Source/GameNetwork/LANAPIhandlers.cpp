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

///////////////////////////////////////////////////////////////////////////////////////
// FILE: LANAPIHandlers.cpp
// Author: Matthew D. Campbell, October 2001
// Description: LAN callback handlers
///////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/crc.h"
#include "Common/GameState.h"
#include "Common/Registry.h"
#include "Common/GlobalData.h"
#include "Common/QuotedPrintable.h"
#include "Common/UserPreferences.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameClient/MapUtil.h"

// GeneralsX @build BenderAI 13/02/2026 WideCharWindows conversion helpers (fighter19 pattern)
// These functions handle conversion between WideChar (wchar_t) and WideCharWindows (uint16_t)
// Required because wchar_t size varies (2 bytes Windows, 4 bytes Linux) but network protocol needs fixed size

void CopyWcharToWindowsWideChar( WideCharWindows *dest, const WideChar *src, UnsignedInt len )
{
	// GeneralsX @bugfix The copy loop used to run exactly `len` iterations no matter how long `src`
	// actually was. Every caller passes ARRAY_SIZE(dest)-1, i.e. the *destination* capacity, so a
	// short source was over-read by up to len-1 elements: UnicodeString::str() hands back a pointer
	// to the single static &TheNullChr when the string is empty, and one caller passes a bare L""
	// literal, so the 100-element chat field read ~400 bytes off the end of a 4-byte object. The low
	// 16 bits of each stray word then went out on a UDP broadcast to the whole subnet. Stop at the
	// source terminator, and zero the remainder of the field so no residue is put on the wire
	// either. This writes dest[0..len] - exactly the range the old code wrote - so it stays in
	// bounds at every call site.
	UnsignedInt i = 0;
	while (i < len && src[i] != 0)
	{
		dest[i] = src[i];
		++i;
	}
	while (i <= len)
	{
		dest[i] = 0;
		++i;
	}
}

static wchar_t *WindowsWideCharToWchar( const WideCharWindows *src, size_t srcCount )
{
	static wchar_t buf[MAX_COMPUTERNAME_LENGTH];

	// GeneralsX @bugfix Three defects here, all reachable from a malformed or hostile LAN datagram,
	// because `src` points straight into the received packet:
	//  1. the length scan was unbounded, so a field the sender never terminated walked off the end
	//     of the receive buffer;
	//  2. the "too long" test used `>` when buf only has MAX_COMPUTERNAME_LENGTH slots, so a length
	//     of exactly MAX_COMPUTERNAME_LENGTH slipped through and buf[MAX_COMPUTERNAME_LENGTH] wrote
	//     one element past the end of the static array;
	//  3. the NULL returned on that path is dereferenced unchecked by every caller - e.g.
	//     ContainsInvalidChars() does `while (*playerName)`, which is a plain null deref in a
	//     release build, where its DEBUG_ASSERTCRASH is compiled out.
	// Scan and copy in a single pass bounded by both the source field and the buffer, and always
	// return a terminated string. Truncating is the safe answer for a malformed field: no legitimate
	// value comes anywhere near either bound, so nothing valid is ever shortened.
	if (srcCount > ARRAY_SIZE(buf) - 1)
	{
		srcCount = ARRAY_SIZE(buf) - 1;
	}

	size_t i = 0;
	if (src != nullptr)
	{
		while (i < srcCount && src[i] != 0)
		{
			buf[i] = src[i];
			++i;
		}
	}
	buf[i] = 0;
	return buf;
}

// GeneralsX @bugfix Bounding the scan by the scratch buffer alone was not enough. The buffer holds
// MAX_COMPUTERNAME_LENGTH elements, but no LANMessage field is remotely that big - name is 13
// elements, gameName 17, the chat message 101 - and `src` points into
// Transport::m_inBuffer[i].data, where doRecv() memcpy's only the bytes that actually arrived. So a
// peer that sent a short datagram with an unterminated field had the scan run on past the field and
// past the end of its own datagram into residue left by whatever packet last occupied that slot,
// including unicast ones it never saw. That residue then became a player/game name or chat line
// that is displayed in the lobby and folded back into the game options string we broadcast. Bounding
// by the field also stops a peer injecting a 255-element string where the rest of the code assumes
// the wire limit. This form deduces the element count from the field itself, so no call site can get
// the bound wrong. Everything in this file goes through it; the plain pointer form below stays for
// the signature LANAPI.h declares, which has no way to carry a length.
template <size_t N>
static wchar_t *GetWindowsWideCharFieldAsWchar( const WideCharWindows (&src)[N] )
{
	return WindowsWideCharToWchar( src, N );
}

wchar_t *GetWindowsWideCharAsWchar( WideCharWindows *src )
{
	return WindowsWideCharToWchar( src, MAX_COMPUTERNAME_LENGTH - 1 );
}

void LANAPI::handleRequestLocations( LANMessage *msg, UnsignedInt senderIP )
{
	if (m_inLobby)
	{
		LANMessage reply;
		fillInLANMessage( &reply );
		reply.messageType = LANMessage::MSG_LOBBY_ANNOUNCE;

		sendMessage(&reply);
		m_lastResendTime = timeGetTime();
	}
	else
	{
		// In game - are we a game host?
		if (m_currentGame)
		{
			if (m_currentGame->getIP(0) == m_localIP)
			{
				LANMessage reply;
				fillInLANMessage( &reply );
				reply.messageType = LANMessage::MSG_GAME_ANNOUNCE;
				AsciiString gameOpts = GenerateGameOptionsString();
				strlcpy(reply.GameInfo.options, gameOpts.str(), ARRAY_SIZE(reply.GameInfo.options));
				// GeneralsX @bugfix BenderAI 13/02/2026 Use CopyWcharToWindowsWideChar (fighter19 pattern)
				CopyWcharToWindowsWideChar(reply.GameInfo.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameInfo.gameName) - 1);
				reply.GameInfo.inProgress = m_currentGame->isGameInProgress();
				reply.GameInfo.isDirectConnect = m_currentGame->getIsDirectConnect();

				sendMessage(&reply);
			}
			else
			{
				// We're a joiner
			}
		}
	}
	// Add the player to the lobby player list
	LANPlayer *player = LookupPlayer(senderIP);
	if (!player)
	{
		player = NEW LANPlayer;
		player->setIP(senderIP);
	}
	else
	{
		removePlayer(player);
	}
	// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
	player->setName(UnicodeString(GetWindowsWideCharFieldAsWchar(msg->name)));
	player->setHost(msg->hostName);
	player->setLogin(msg->userName);
	player->setLastHeard(timeGetTime());

	addPlayer(player);

	OnNameChange(player->getIP(), player->getName());
}

void LANAPI::handleGameAnnounce( LANMessage *msg, UnsignedInt senderIP )
{
	if (senderIP == m_localIP)
	{
		return; // Don't try to update own info
	}
	else if (m_currentGame && m_currentGame->isGameInProgress())
	{
		return; // Don't care about games if we're playing
	}
	else if (senderIP == m_directConnectRemoteIP)
	{

		if (m_currentGame == nullptr)
		{
			// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
			LANGameInfo *game = LookupGame(UnicodeString(GetWindowsWideCharFieldAsWchar(msg->GameInfo.gameName)));
			if (!game)
			{
				game = NEW LANGameInfo;
				// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
				game->setName(UnicodeString(GetWindowsWideCharFieldAsWchar(msg->GameInfo.gameName)));
				addGame(game);
			}
			Bool success = ParseGameOptionsString(game,AsciiString(msg->GameInfo.options));
			game->setGameInProgress(msg->GameInfo.inProgress);
			game->setIsDirectConnect(msg->GameInfo.isDirectConnect);
			game->setLastHeard(timeGetTime());
			if (!success)
			{
				// remove from list
				removeGame(game);
				delete game;
				return;
			}
			RequestGameJoin(game, m_directConnectRemoteIP);
		}
	}
	else
	{
		// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
		LANGameInfo *game = LookupGame(UnicodeString(GetWindowsWideCharFieldAsWchar(msg->GameInfo.gameName)));
		if (!game)
		{
			game = NEW LANGameInfo;
			// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
			game->setName(UnicodeString(GetWindowsWideCharFieldAsWchar(msg->GameInfo.gameName)));
			addGame(game);
		}
		Bool success = ParseGameOptionsString(game,AsciiString(msg->GameInfo.options));
		game->setGameInProgress(msg->GameInfo.inProgress);
		game->setIsDirectConnect(msg->GameInfo.isDirectConnect);
		game->setLastHeard(timeGetTime());
		if (!success)
		{
			// remove from list
			removeGame(game);
			delete game;
			game = nullptr;
		}

		OnGameList( m_games );
	//	if (game == m_currentGame && !m_inLobby)
	//		OnSlotList(RET_OK, game);
	}
}

void LANAPI::handleLobbyAnnounce( LANMessage *msg, UnsignedInt senderIP )
{
	LANPlayer *player = LookupPlayer(senderIP);
	if (!player)
	{
		player = NEW LANPlayer;
		player->setIP(senderIP);
	}
	else
	{
		removePlayer(player);
	}
	// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
	player->setName(UnicodeString(GetWindowsWideCharFieldAsWchar(msg->name)));
	player->setHost(msg->hostName);
	player->setLogin(msg->userName);
	player->setLastHeard(timeGetTime());

	addPlayer(player);

	OnNameChange(player->getIP(), player->getName());
}

void LANAPI::handleRequestGameInfo( LANMessage *msg, UnsignedInt senderIP )
{
	// In game - are we a game host?
	if (m_currentGame)
	{
		if (m_currentGame->getIP(0) == m_localIP || (m_currentGame->isGameInProgress() && TheNetwork && TheNetwork->isPacketRouter())) // if we're in game we should reply if we're the packet router
		{
			LANMessage reply;
			fillInLANMessage( &reply );
			reply.messageType = LANMessage::MSG_GAME_ANNOUNCE;

			AsciiString gameOpts = GameInfoToAsciiString(m_currentGame);
			strlcpy(reply.GameInfo.options,gameOpts.str(), ARRAY_SIZE(reply.GameInfo.options));
			// GeneralsX @bugfix BenderAI 13/02/2026 Use CopyWcharToWindowsWideChar (fighter19 pattern)
			CopyWcharToWindowsWideChar(reply.GameInfo.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameInfo.gameName) - 1);
			reply.GameInfo.inProgress = m_currentGame->isGameInProgress();
			reply.GameInfo.isDirectConnect = m_currentGame->getIsDirectConnect();

			sendMessage(&reply, senderIP);
		}
	}
}

static Bool IsInvalidCharForPlayerName(const WideChar c)
{
	return c < L' ' // C0 control chars
		|| c == L',' || c == L':' || c == L';' // chars used for strtok in ParseAsciiStringToGameInfo
		|| (c >= L'\x007f' && c <= L'\x009f') // DEL + C1 control chars
		|| c == L'\x2028' || c == L'\x2029' // line and paragraph separators
		|| (c >= L'\xdc00' && c <= L'\xdfff') // low surrogate, for chars beyond the Unicode Basic Multilingual Plane
		|| (c >= L'\xd800' && c <= L'\xdbff'); // high surrogate, for chars beyond the BMP
}

static Bool IsSpaceCharacter(const WideChar c)
{
	return c == L' ' // space
		|| c == L'\xA0' // no-break space
		|| c == L'\x1680' // ogham space mark
		|| (c >= L'\x2000' && c <= L'\x200A') // en/em spaces, figure, punctuation, thin, hair
		|| c == L'\x202F' // narrow no-break space
		|| c == L'\x205F' // medium mathematical space
		|| c == L'\x3000'; // ideographic space
}

static Bool ContainsInvalidChars(const WideChar* playerName)
{
	DEBUG_ASSERTCRASH(playerName != nullptr, ("playerName is null"));
	while (*playerName)
	{
		if (IsInvalidCharForPlayerName(*playerName++))
			return true;
	}

	return false;
}

static Bool ContainsAnyReadableChars(const WideChar* playerName)
{
	DEBUG_ASSERTCRASH(playerName != nullptr, ("playerName is null"));
	while (*playerName)
	{
		if (!IsSpaceCharacter(*playerName++))
			return true;
	}

	return false;
}

void LANAPI::handleRequestJoin( LANMessage *msg, UnsignedInt senderIP )
{
	if (msg->GameToJoin.gameIP != m_localIP)
	{
		return; // Not us.  Ignore it.
	}
	LANMessage reply;
	fillInLANMessage( &reply );
	if (!m_inLobby && m_currentGame && m_currentGame->getIP(0) == m_localIP)
	{
		if (m_currentGame->isGameInProgress())
		{
			reply.messageType = LANMessage::MSG_JOIN_DENY;
			reply.GameNotJoined.reason = LANAPIInterface::RET_GAME_STARTED;
			reply.GameNotJoined.gameIP = m_localIP;
			reply.GameNotJoined.playerIP = senderIP;
			DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because game already started."));
		}
		else
		{
			int player;
			Bool canJoin = true;

			// see if the CRCs match
#if defined(RTS_DEBUG)
			if (TheGlobalData->m_netMinPlayers > 0) {
#endif
// TheSuperHackers @todo Enable CRC checks!
#if !RTS_ZEROHOUR
			if (msg->GameToJoin.iniCRC != TheGlobalData->m_iniCRC ||
					msg->GameToJoin.exeCRC != TheGlobalData->m_exeCRC)
			{
				DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because of CRC mismatch. CRCs are them/us INI:%X/%X exe:%X/%X",
					msg->GameToJoin.iniCRC, TheGlobalData->m_iniCRC,
					msg->GameToJoin.exeCRC, TheGlobalData->m_exeCRC));
				reply.messageType = LANMessage::MSG_JOIN_DENY;
				reply.GameNotJoined.reason = LANAPIInterface::RET_CRC_MISMATCH;
				reply.GameNotJoined.gameIP = m_localIP;
				reply.GameNotJoined.playerIP = senderIP;
				canJoin = false;
			}
#endif
#if defined(RTS_DEBUG)
			}
#endif

// TheSuperHackers @tweak Disables the duplicate serial check
#if 0
			// check for a duplicate serial
			AsciiString s;
			for (player = 0; canJoin && player<MAX_SLOTS; ++player)
			{
				LANGameSlot *slot = m_currentGame->getLANSlot(player);
				s.clear();
				if (player == 0)
				{
					GetStringFromRegistry("\\ergc", "", s);
				}
				else if (slot->isHuman())
				{
					s = slot->getSerial();
					if (s.isEmpty())
						s = "<Munkee>";
				}

				if (s.isNotEmpty())
				{
					DEBUG_LOG(("Checking serial '%s' in slot %d", s.str(), player));

					if (!strncmp(s.str(), msg->GameToJoin.serial, g_maxSerialLength))
					{
						// serials match!  kick the punk!
						reply.messageType = LANMessage::MSG_JOIN_DENY;
						reply.GameNotJoined.reason = LANAPIInterface::RET_SERIAL_DUPE;
						reply.GameNotJoined.gameIP = m_localIP;
						reply.GameNotJoined.playerIP = senderIP;
						canJoin = false;

						DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because of duplicate serial # (%s).", s.str()));
						break;
					}
				}
			}
#endif

			// TheSuperHackers @bugfix slurmlord 18/09/2025 need to validate the name of the connecting player before
			// allowing them to join to prevent messing up the format of game state string. Commas, colons, semicolons etc.
			// should not be in a player name. It should also not consist of only space characters.
			if (canJoin)
			{
				// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
				if (ContainsInvalidChars(GetWindowsWideCharFieldAsWchar(msg->name)) || !ContainsAnyReadableChars(GetWindowsWideCharFieldAsWchar(msg->name)))
				{
					// Just deny with a duplicate name reason, for backwards compatibility with retail
					reply.messageType = LANMessage::MSG_JOIN_DENY;
					reply.GameNotJoined.reason = LANAPIInterface::RET_DUPLICATE_NAME;
					reply.GameNotJoined.gameIP = m_localIP;
					reply.GameNotJoined.playerIP = senderIP;
					canJoin = false;

					DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because of illegal characters in the player name."));
				}
			}	

			// Then see if the player has a duplicate name
			for (player = 0; canJoin && player<MAX_SLOTS; ++player)
			{
				LANGameSlot *slot = m_currentGame->getLANSlot(player);
				// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
				if (slot->isHuman() && slot->getName().compare(GetWindowsWideCharFieldAsWchar(msg->name)) == 0)
				{
					// just deny duplicates
					reply.messageType = LANMessage::MSG_JOIN_DENY;
					reply.GameNotJoined.reason = LANAPIInterface::RET_DUPLICATE_NAME;
					reply.GameNotJoined.gameIP = m_localIP;
					reply.GameNotJoined.playerIP = senderIP;
					canJoin = false;

					DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because of duplicate names."));
					break;
				}
			}

			// TheSuperHackers @bugfix Stubbjax 26/09/2025 Players can now join open slots regardless of starting spots on the map.
			for (player = 0; canJoin && player<MAX_SLOTS; ++player)
			{
				if (m_currentGame->getLANSlot(player)->isOpen())
				{
					// OK, add him in.
					reply.messageType = LANMessage::MSG_JOIN_ACCEPT;
					// GeneralsX @bugfix BenderAI 13/02/2026 Use CopyWcharToWindowsWideChar (fighter19 pattern)
					CopyWcharToWindowsWideChar(reply.GameJoined.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameJoined.gameName) - 1);
					reply.GameJoined.slotPosition = player;
					reply.GameJoined.gameIP = m_localIP;
					reply.GameJoined.playerIP = senderIP;

					LANGameSlot newSlot;
					// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
					newSlot.setState(SLOT_PLAYER, UnicodeString(GetWindowsWideCharFieldAsWchar(msg->name)));
					newSlot.setIP(senderIP);
					newSlot.setPort(NETWORK_BASE_PORT_NUMBER);
					newSlot.setLastHeard(timeGetTime());
					newSlot.setSerial(msg->GameToJoin.serial);
					m_currentGame->setSlot(player,newSlot);
					DEBUG_LOG(("LANAPI::handleRequestJoin - added player %ls at ip 0x%08x to the game", msg->name, senderIP));

					// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
					OnPlayerJoin(player, UnicodeString(GetWindowsWideCharFieldAsWchar(msg->name)));

					break;
				}
			}

			if (canJoin && player == MAX_SLOTS)
			{
				reply.messageType = LANMessage::MSG_JOIN_DENY;
				// GeneralsX @bugfix BenderAI 13/02/2026 Use CopyWcharToWindowsWideChar (fighter19 pattern)
				CopyWcharToWindowsWideChar(reply.GameNotJoined.gameName, m_currentGame->getName().str(), ARRAY_SIZE(reply.GameNotJoined.gameName) - 1);
				reply.GameNotJoined.reason = LANAPIInterface::RET_GAME_FULL;
				reply.GameNotJoined.gameIP = m_localIP;
				reply.GameNotJoined.playerIP = senderIP;
				DEBUG_LOG(("LANAPI::handleRequestJoin - join denied because game is full."));
			}
		}
	}
	else
	{
		reply.messageType = LANMessage::MSG_JOIN_DENY;
		reply.GameNotJoined.reason = LANAPIInterface::RET_GAME_GONE;
		reply.GameNotJoined.gameIP = m_localIP;
		reply.GameNotJoined.playerIP = senderIP;
	}
	// GeneralsX @bugfix The accept path used to zero the destination before this send, which drops the
	// reply into sendMessage's broadcast branch. Darwin refuses a send to 255.255.255.255 with
	// EHOSTUNREACH, so a game hosted from the LAN lobby - as opposed to Direct Connect, whose branch
	// unicasts to each occupied slot and therefore happened to work - never put the accept on the wire
	// at all: the host showed the joiner in a slot while the joiner sat until m_actionTimeout and
	// reported RET_TIMEOUT. The reply belongs to exactly one peer in any case. It carries that peer's
	// slot position, every other recipient discards it on the "playerIP == m_localIP" test in
	// handleJoinAccept, and the rest of the lobby learns about the new player from the MSG_GAME_OPTIONS
	// slot list pushed immediately below, not from this message. So address it to the requester, the
	// same destination the deny replies above already use.
	sendMessage(&reply, senderIP);
	RequestGameOptions(GenerateGameOptionsString(), true);
}

void LANAPI::handleJoinAccept( LANMessage *msg, UnsignedInt senderIP )
{
	if (msg->GameJoined.playerIP == m_localIP) // Is it for us?
	{
		if (m_pendingAction == ACT_JOIN) // Are we trying to join?
		{
			// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
			m_currentGame = LookupGame(UnicodeString(GetWindowsWideCharFieldAsWchar(msg->GameJoined.gameName)));

			if (!m_currentGame)
			{
				DEBUG_CRASH(("Could not find game to join!"));
				OnGameJoin(RET_UNKNOWN, nullptr);
			}
			else
			{
				m_inLobby = false;
				AsciiString options = GameInfoToAsciiString(m_currentGame);
				m_currentGame->enterGame();
				ParseAsciiStringToGameInfo(m_currentGame, options);

				Int pos = msg->GameJoined.slotPosition;

				LANGameSlot slot;
				slot.setState(SLOT_PLAYER, m_name);
				slot.setIP(m_localIP);
				slot.setPort(NETWORK_BASE_PORT_NUMBER);
				slot.setLastHeard(0);
				slot.setLogin(m_userName);
				slot.setHost(m_hostName);
				m_currentGame->setSlot(pos, slot);

				m_currentGame->getLANSlot(0)->setHost(msg->hostName);
				m_currentGame->getLANSlot(0)->setLogin(msg->userName);

				LANPreferences prefs;
				AsciiString entry;
				entry.format("%d.%d.%d.%d:%s", PRINTF_IP_AS_4_INTS(senderIP), UnicodeStringToQuotedPrintable(m_currentGame->getSlot(0)->getName()).str());
				prefs["RemoteIP0"] = entry;
				prefs.write();

				OnGameJoin(RET_OK, m_currentGame);
				//DEBUG_CRASH(("setting host to %ls@%ls", m_currentGame->getLANSlot(0)->getUser()->getLogin().str(),
				//	m_currentGame->getLANSlot(0)->getUser()->getHost().str()));
			}
			m_pendingAction = ACT_NONE;
			m_expiration = 0;
		}
	}
}

void LANAPI::handleJoinDeny( LANMessage *msg, UnsignedInt senderIP )
{
	if (msg->GameJoined.playerIP == m_localIP) // Is it for us?
	{
		if (m_pendingAction == ACT_JOIN) // Are we trying to join?
		{
			// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
			OnGameJoin(msg->GameNotJoined.reason, LookupGame(UnicodeString(GetWindowsWideCharFieldAsWchar(msg->GameNotJoined.gameName))));
			m_pendingAction = ACT_NONE;
			m_expiration = 0;
		}
	}
}

void LANAPI::handleRequestGameLeave( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame && !m_currentGame->isGameInProgress())
	{
		int player;
		for (player = 0; player < MAX_SLOTS; ++player)
		{
			if (m_currentGame->getIP(player) == senderIP)
			{
				if (player == 0)
				{
					OnHostLeave();
					removeGame(m_currentGame);
					delete m_currentGame;
					m_currentGame = nullptr;

					/// @todo re-add myself to lobby?  Or just keep me there all the time?  If we send a LOBBY_ANNOUNCE things'll work out...
					LANPlayer *lanPlayer = LookupPlayer(m_localIP);
					if (!lanPlayer)
					{
						lanPlayer = NEW LANPlayer;
						lanPlayer->setIP(m_localIP);
					}
					else
					{
						removePlayer(lanPlayer);
					}
					lanPlayer->setName(UnicodeString(m_name));
					lanPlayer->setHost(m_hostName);
					lanPlayer->setLogin(m_userName);
					lanPlayer->setLastHeard(timeGetTime());
					addPlayer(lanPlayer);

				}
				else
				{
					if (AmIHost())
					{
						// remove the deadbeat
						LANGameSlot slot;
						slot.setState(SLOT_OPEN);
						m_currentGame->setSlot( player, slot );
					}
					// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
					OnPlayerLeave(UnicodeString(GetWindowsWideCharFieldAsWchar(msg->name)));
					m_currentGame->getLANSlot(player)->setState(SLOT_OPEN);
					m_currentGame->resetAccepted();
					RequestGameOptions(GenerateGameOptionsString(), false, senderIP);
					//m_currentGame->endGame();
				}
				break;
			}
			DEBUG_ASSERTCRASH(player < MAX_SLOTS, ("Didn't find player!"));
		}
	}
	else if (m_inLobby)
	{
		// Look for dissappearing games
		LANGameInfo *game = m_games;
		while (game)
		{
			// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
			if (game->getName().compare(GetWindowsWideCharFieldAsWchar(msg->GameToLeave.gameName)) == 0)
			{
				removeGame(game);
				delete game;
				OnGameList(m_games);
				break;
			}
			game = game->getNext();
		}
	}
}

void LANAPI::handleRequestLobbyLeave( LANMessage *msg, UnsignedInt senderIP )
{
	if (m_inLobby)
	{
		LANPlayer *player = m_lobbyPlayers;
		while (player)
		{
			if (player->getIP() == senderIP)
			{
				removePlayer(player);
				OnPlayerList(m_lobbyPlayers);
				break;
			}
			player = player->getNext();
		}
	}
}

void LANAPI::handleSetAccept( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame && !m_currentGame->isGameInProgress())
	{
		int player;
		for (player = 0; player < MAX_SLOTS; ++player)
		{
			if (m_currentGame->getIP(player) == senderIP)
			{
				OnAccept(senderIP, msg->Accept.isAccepted);
				break;
			}
		}
	}
}

void LANAPI::handleHasMap( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame)
	{
		CRC mapNameCRC;
//	mapNameCRC.computeCRC(m_currentGame->getMap().str(), m_currentGame->getMap().getLength());
		AsciiString portableMapName = TheGameState->realMapPathToPortableMapPath(m_currentGame->getMap());
		mapNameCRC.computeCRC(portableMapName.str(), portableMapName.getLength());
		if (mapNameCRC.get() != msg->MapStatus.mapCRC)
		{
			return;
		}

		int player;
		for (player = 0; player < MAX_SLOTS; ++player)
		{
			if (m_currentGame->getIP(player) == senderIP)
			{
				OnHasMap(senderIP, msg->MapStatus.hasMap);
				break;
			}
		}
	}
}

void LANAPI::handleChat( LANMessage *msg, UnsignedInt senderIP )
{
	if (m_inLobby)
	{
		LANPlayer *player;
		if((player=LookupPlayer(senderIP)) != nullptr)
		{
			// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
			OnChat(UnicodeString(player->getName()), player->getIP(), UnicodeString(GetWindowsWideCharFieldAsWchar(msg->Chat.message)), msg->Chat.chatType);
			player->setLastHeard(timeGetTime());
		}
	}
	else
	{
		// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
		if (LookupGame(UnicodeString(GetWindowsWideCharFieldAsWchar(msg->Chat.gameName))) != m_currentGame)
		{
			DEBUG_LOG(("Game '%ls' is not my game", msg->Chat.gameName));
			if (m_currentGame)
			{
				DEBUG_LOG(("Current game is '%ls'", m_currentGame->getName().str()));
			}
			return;
		}

		int player;
		for (player = 0; player < MAX_SLOTS; ++player)
		{
			if (m_currentGame && m_currentGame->getIP(player) == senderIP)
			{
				// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
				OnChat(UnicodeString(GetWindowsWideCharFieldAsWchar(msg->name)), m_currentGame->getIP(player), UnicodeString(GetWindowsWideCharFieldAsWchar(msg->Chat.message)), msg->Chat.chatType);
				break;
			}
		}
	}
}

void LANAPI::handleGameStart( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame && m_currentGame->getIP(0) == senderIP && !m_currentGame->isGameInProgress())
	{
		OnGameStart();
	}
}

void LANAPI::handleGameStartTimer( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame && m_currentGame->getIP(0) == senderIP && !m_currentGame->isGameInProgress())
	{
		OnGameStartTimer(msg->StartTimer.seconds);
	}
}

void LANAPI::handleGameOptions( LANMessage *msg, UnsignedInt senderIP )
{
	if (!m_inLobby && m_currentGame && !m_currentGame->isGameInProgress())
	{
		int player;
		for (player = 0; player < MAX_SLOTS; ++player)
		{
			if (m_currentGame->getIP(player) == senderIP)
			{
				OnGameOptions(senderIP, player, AsciiString(msg->GameOptions.options));
				break;
			}
		}
	}
}

void LANAPI::handleInActive(LANMessage *msg, UnsignedInt senderIP) {
	if (m_inLobby || (m_currentGame == nullptr) || (m_currentGame->isGameInProgress())) {
		return;
	}

	// check to see if we are the host of this game.
	if (m_currentGame->amIHost() == FALSE) {
		return;
	}

	UnicodeString playerName;
	// GeneralsX @bugfix BenderAI 13/02/2026 Wrap WideCharWindows with GetWindowsWideCharFieldAsWchar (fighter19 pattern)
	playerName = UnicodeString(GetWindowsWideCharFieldAsWchar(msg->name));

	Int slotNum = m_currentGame->getSlotNum(playerName);
	if (slotNum < 0)
		return;
	GameSlot *slot = m_currentGame->getSlot(slotNum);
	if (slot == nullptr) {
		return;
	}

	if (senderIP != slot->getIP()) {
		return;
	}

	// don't want to unaccept the host, that's silly.  They can't hit start alt-tabbed anyways.
	if (senderIP == GetLocalIP()) {
		return;
	}

	// only unaccept if the timer hasn't started yet.
	if (m_gameStartTime != 0) {
		return;
	}

	slot->unAccept();
	AsciiString options = GenerateGameOptionsString();
	RequestGameOptions(options, FALSE);
	lanUpdateSlotList();
}
