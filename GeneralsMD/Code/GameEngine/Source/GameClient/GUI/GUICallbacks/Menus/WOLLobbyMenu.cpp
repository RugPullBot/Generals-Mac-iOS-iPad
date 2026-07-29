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
// FILE: WOLLobbyMenu.cpp
// Author: Chris Huybregts, November 2001
// Description: WOL Lobby Menu
///////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GameEngine.h"
#include "Common/GameState.h"
#include "Common/MiniLog.h"
#include "Common/MultiplayerSettings.h"
#include "Common/PlayerTemplate.h"
#include "Common/CustomMatchPreferences.h"
#include "Common/version.h"
#include "GameClient/AnimateWindowManager.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Gadget.h"
#include "GameClient/GameClient.h"
#include "GameClient/Shell.h"
#include "GameClient/ShellHooks.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetSlider.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GameText.h"
#include "GameClient/MessageBox.h"
#include "GameClient/Mouse.h"
#include "GameClient/Display.h"
#include "GameNetwork/GameSpyOverlay.h"
#include "GameClient/GameWindowTransitions.h"

#include "GameLogic/GameLogic.h"

#include "GameClient/LanguageFilter.h"
#include "GameNetwork/GameSpy/BuddyDefs.h"
#include "GameNetwork/GameSpy/GSConfig.h"
#include "GameNetwork/GameSpy/LadderDefs.h"
#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameNetwork/GameSpy/PeerThread.h"
#include "GameNetwork/GameSpy/PersistentStorageDefs.h"
#include "GameNetwork/GameSpy/PersistentStorageThread.h"
#include "GameNetwork/GameSpy/LobbyUtils.h"
#include "GameNetwork/RankPointValue.h"

// GeneralsX @feature Matchmaking. This screen is now also driven by our own relay - see the
// "relay lobby" block below.
#include "Common/OptionPreferences.h"
#include "GameClient/GUICallbacks.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/Transport.h"

void refreshGameList( Bool forceRefresh = FALSE );
void refreshPlayerList( Bool forceRefresh = FALSE );

#ifdef DEBUG_LOGGING
#define PERF_TEST
static LogClass s_perfLog("Perf.txt");
#define PERF_LOG(x) s_perfLog.log x
#else // DEBUG_LOGGING
#define PERF_LOG(x) {}
#endif // DEBUG_LOGGING

// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
static Bool isShuttingDown = false;
static Bool buttonPushed = false;
static const char *nextScreen = nullptr;
static Bool raiseMessageBoxes = false;
static time_t gameListRefreshTime = 0;
static const time_t gameListRefreshInterval = 10000;
static time_t playerListRefreshTime = 0;
static const time_t playerListRefreshInterval = 5000;

void setUnignoreText( WindowLayout *layout, AsciiString nick, GPProfile id);
static void doSliderTrack(GameWindow *control, Int val);
Bool DontShowMainMenu = FALSE;
enum { COLUMN_PLAYERNAME = 1 };

// window ids ------------------------------------------------------------------------------
static NameKeyType parentWOLLobbyID = NAMEKEY_INVALID;
static NameKeyType buttonBackID = NAMEKEY_INVALID;
static NameKeyType buttonHostID = NAMEKEY_INVALID;
static NameKeyType buttonRefreshID = NAMEKEY_INVALID;
static NameKeyType buttonJoinID = NAMEKEY_INVALID;
static NameKeyType buttonBuddyID = NAMEKEY_INVALID;
static NameKeyType buttonEmoteID = NAMEKEY_INVALID;
static NameKeyType textEntryChatID = NAMEKEY_INVALID;
static NameKeyType listboxLobbyPlayersID = NAMEKEY_INVALID;
static NameKeyType listboxLobbyChatID = NAMEKEY_INVALID;
static NameKeyType comboLobbyGroupRoomsID = NAMEKEY_INVALID;
//static NameKeyType // sliderChatAdjustID = NAMEKEY_INVALID;

// Window Pointers ------------------------------------------------------------------------
static GameWindow *parentWOLLobby = nullptr;
static GameWindow *buttonBack = nullptr;
static GameWindow *buttonHost = nullptr;
static GameWindow *buttonRefresh = nullptr;
static GameWindow *buttonJoin = nullptr;
static GameWindow *buttonBuddy = nullptr;
static GameWindow *buttonEmote = nullptr;
static GameWindow *textEntryChat = nullptr;
static GameWindow *listboxLobbyPlayers = nullptr;
static GameWindow *listboxLobbyChat = nullptr;
static GameWindow *comboLobbyGroupRooms = nullptr;
static GameWindow *parent = nullptr;

static Int groupRoomToJoin = 0;
static Int	initialGadgetDelay = 2;
static Bool justEntered = FALSE;

// ==============================================================================================
// GeneralsX @feature Matchmaking - the relay lobby.
//
// This screen (WOLCustomLobby.wnd) is the multi-column game browser people recognise from Online.
// It ships in our archives and was unreachable, because the Online button used to start the
// GameSpy chain and GameSpy has been dead since 2014.
//
// Nothing below re-implements GameSpy. The screen and LobbyUtils are pure presentation: they read
// staging rooms out of TheGameSpyInfo, colour and sort them, and draw them. They do not care where
// the rows came from. So this is a DATA SOURCE SWAP - our relay's GXGAME replies are mapped onto
// GameSpyStagingRoom objects and handed to addStagingRoom, and the existing RefreshGameListBoxes
// draws them.
//
// The signal for "there is no GameSpy backend" is TheGameSpyPeerMessageQueue being null, which is
// what SetUpGameSpyForRelayLobby deliberately leaves alone: WOLLobbyMenuUpdate already gates its
// entire GameSpy message pump on that pointer, so a null queue disables the GameSpy half of this
// file without a single edit to it.
//
// Two things are honestly missing rather than approximated, and are called out where they bite:
//   * there is no chat channel in the relay at all, so the chat box is inert;
//   * the relay does not track player identities across rooms, so the player list can only show
//     the occupants of the SELECTED game - and of those, only the host is actually named.
// ==============================================================================================

/// TRUE when this screen is being driven by our relay rather than by GameSpy. Decided once, in
/// WOLLobbyMenuInit, off the absence of a GameSpy backend.
static Bool s_relayLobby = FALSE;

/// TRUE when WE created the GameSpy presentation singletons and must therefore tear them down
/// again on the way out. Never true if a real GameSpy session was already up.
static Bool s_relayOwnsGameSpy = FALSE;

// The reply to a GXLIST is a burst of one datagram per game with no terminator, and
// Transport::requestGameList zeroes the count the moment it asks. So the count walks 0, 1, 2, 3
// across successive pumps and there is nothing that says "that was all of them". Rebuilding on
// every step would empty and refill the listbox under the player's cursor every ten seconds.
// Rebuild when the count has stopped moving instead: one frame of latency, no flicker, and a sweep
// that legitimately returns nothing still empties the list (0 twice in a row is settled too).
static Int s_relayCountLastFrame = -1;
static Int s_relayCountLastBuilt = -1;
static time_t s_relayLastQueryTime = 0;
static Int s_relaySelectedID = -1;

/// The listbox item data is a plain Int game ID, so a row the player clicked has to be resolvable
/// back to the room it lives in. The room is not optional and is not derivable from the address:
/// the relay allocates identities PER ROOM, so the first player in every room is 10.42.0.1 and the
/// host address does not identify a game.
struct RelayListedGame
{
	Int id;
	char room[32];
	UnsignedInt hostVirtualIP;
	Int players;
	Int slots;
};
static RelayListedGame s_relayGames[Transport::MAX_RELAY_LISTINGS];
static Int s_relayGameCount = 0;
static Int s_relayNextGameID = 1;

static void relayResetGameTable()
{
	s_relayGameCount = 0;
	s_relayNextGameID = 1;
	s_relayCountLastFrame = -1;
	s_relayCountLastBuilt = -1;
	s_relayLastQueryTime = 0;
	s_relaySelectedID = -1;
}

static const RelayListedGame *relayFindGameByID( Int id )
{
	for (Int i = 0; i < s_relayGameCount; ++i)
	{
		if (s_relayGames[i].id == id)
			return &s_relayGames[i];
	}
	return nullptr;
}

/// The transport, but only when it is actually relaying. Everything that reads a game list has to
/// ask the TRANSPORT rather than the preferences file: since the relay assigns identities,
/// LocalVirtualIP is usually absent, and keying off it hides the list on exactly the setups
/// assignment was built for.
static Transport *relayTransport()
{
	Transport *t = (TheLAN != nullptr) ? TheLAN->getTransport() : nullptr;
	return (t != nullptr && t->isRelayEnabled()) ? t : nullptr;
}

/// Turn the relay's current game list into staging rooms.
///
/// Rebuilt wholesale rather than diffed, because the relay has no teardown message ON PURPOSE - a
/// host that crashes or loses its link cannot send one, so a room drops off by simply not being in
/// the next sweep. The list IS the truth; diffing it would only invent ways to disagree with it.
static void relayRebuildStagingRooms()
{
	Transport *transport = relayTransport();
	if (transport == nullptr || TheGameSpyInfo == nullptr || TheLAN == nullptr)
		return;

	// IDs must survive a rebuild or the selection moves under the player's cursor:
	// RefreshGameListBox restores the selection by ID, and LobbyUtils re-sorts every time.
	RelayListedGame previous[Transport::MAX_RELAY_LISTINGS];
	const Int previousCount = s_relayGameCount;
	for (Int i = 0; i < previousCount; ++i)
		previous[i] = s_relayGames[i];
	s_relayGameCount = 0;

	TheGameSpyInfo->clearStagingRoomList();

	// addStagingRoom only marks the list dirty once the caller has said the list is complete, and
	// hasStagingRoomListChanged() is what refreshGameList() gates the redraw on. One GXLIST sweep
	// IS the complete list, so say so before adding anything.
	TheGameSpyInfo->sawFullGameList();

	const Int count = transport->getGameListCount();
	for (Int g = 0; g < count; ++g)
	{
		const Transport::RelayGameListing *listing = transport->getGameListing(g);
		if (listing == nullptr || listing->hostVirtualIP == 0)
			continue;

		// Do not offer our own game - browsing it would be dialling ourselves. The ROOM is half of
		// that test and not an optimisation: addresses are allocated per room, so every host in the
		// browser is 10.42.0.1 and so are we. Matching on the address alone hides every game.
		if (listing->hostVirtualIP == TheLAN->GetLocalIP()
			&& strcmp(listing->room, transport->getRelayRoom()) == 0)
			continue;

		if (s_relayGameCount >= Transport::MAX_RELAY_LISTINGS)
			break;

		RelayListedGame &entry = s_relayGames[s_relayGameCount];
		entry.id = 0;
		for (Int p = 0; p < previousCount; ++p)
		{
			if (previous[p].hostVirtualIP == listing->hostVirtualIP
				&& strcmp(previous[p].room, listing->room) == 0)
			{
				entry.id = previous[p].id;
				break;
			}
		}
		if (entry.id == 0)
			entry.id = s_relayNextGameID++;
		strlcpy(entry.room, listing->room, sizeof(entry.room));
		entry.hostVirtualIP = listing->hostVirtualIP;
		entry.players = listing->players;
		entry.slots = listing->slots;
		++s_relayGameCount;

		UnicodeString hostName;
		hostName.translate(AsciiString(listing->name));

		GameSpyStagingRoom room;
		room.init();					// resets every slot; must run before the slots are filled in
		room.setID(entry.id);
		room.setGameName(hostName);
		room.setMap(AsciiString(listing->map));

		// The relay's reply carries no exe/ini CRC, so the version check this screen would normally
		// do CANNOT be performed here. Reporting our own CRCs is the honest encoding of "unknown":
		// the alternative, zero, paints every row with the mismatch colour and refuses every join,
		// which claims a version conflict we have not detected. A genuine mismatch still fails, it
		// just fails later - the lobby's own slot-list exchange is where it surfaces.
		room.setExeCRC(TheGlobalData->m_exeCRC);
		room.setIniCRC(TheGlobalData->m_iniCRC);

		room.setHasPassword(FALSE);
		room.setAllowObservers(FALSE);
		room.setLadderIP(AsciiString::TheEmptyString);
		room.setLadderPort(0);

		// No ping measurement exists for a relayed game. An EMPTY ping string is what insertGame
		// reads as "no data" and leaves the column blank, rather than rendering the absence of a
		// measurement as a bad one.
		room.setPingString(AsciiString::TheEmptyString);

		room.setReportedNumPlayers(listing->players);
		room.setReportedMaxPlayers(listing->slots);
		room.setReportedNumObservers(0);

		// Slot occupancy. The relay advertises a COUNT and the host's name, and nothing else - it
		// deliberately does not track who is in a room. So slot 0 is the host, named, and the rest
		// of the occupied slots are real (the count comes from the host's own slot list) but
		// anonymous. Numbering them rather than inventing names keeps that visible.
		for (Int s = 0; s < MAX_SLOTS; ++s)
		{
			GameSpyGameSlot *slot = room.getGameSpySlot(s);
			if (slot == nullptr)
				continue;

			if (s < listing->players)
			{
				UnicodeString slotName;
				if (s == 0)
					slotName = hostName;
				else
					slotName = TheGameText->FETCH_OR_SUBSTITUTE_FORMAT(
						"GUI:RelayLobbyOccupiedSlot", L"Player %d", s + 1);
				slot->setState(SLOT_PLAYER, slotName, (s == 0) ? listing->hostVirtualIP : 0);
			}
			else if (s < listing->slots)
			{
				slot->setState(SLOT_OPEN);
			}
			else
			{
				slot->setState(SLOT_CLOSED);
			}
			slot->setPingString(AsciiString::TheEmptyString);
		}

		TheGameSpyInfo->addStagingRoom(room);
	}
}

/// The ID of the row the player has selected, or 0.
static Int relaySelectedGameID()
{
	GameWindow *listbox = GetGameListBox();
	if (listbox == nullptr)
		return 0;

	Int selected = -1;
	GadgetListBoxGetSelected(listbox, &selected);
	if (selected < 0)
		return 0;

	// GeneralsX @build 64-bit safe pointer cast, as elsewhere in this file
	return static_cast<Int>(reinterpret_cast<intptr_t>(GadgetListBoxGetItemData(listbox, selected)));
}

/// The player list, filled from the SELECTED game's slots.
///
/// GameSpy filled this from chat-room presence: everyone in the lobby, whether or not they were in
/// a game. We have no presence and the relay will not give us one - it does not track identities
/// across rooms, by design. The occupants of the game you are looking at is the honest equivalent,
/// and it is the only player data that actually exists.
static void relayRefreshPlayerList()
{
	if (TheGameSpyInfo == nullptr)
		return;

	PlayerInfoMap *players = TheGameSpyInfo->getPlayerInfoMap();
	players->clear();

	GameSpyStagingRoom *room = TheGameSpyInfo->findStagingRoomByID(relaySelectedGameID());
	if (room != nullptr)
	{
		room->cleanUpSlotPointers();
		for (Int s = 0; s < MAX_SLOTS; ++s)
		{
			const GameSpyGameSlot *slot = room->getGameSpySlot(s);
			if (slot == nullptr || !slot->isHuman())
				continue;

			PlayerInfo info;
			info.m_name.translate(slot->getName());
			if (info.m_name.isEmpty())
				continue;

			// The map is keyed by the displayed name and playerTooltip looks a clicked row straight
			// back up in it without checking for a miss, so every row drawn must be a key here.
			// Everything else stays at its PlayerInfo default: profile 0, no rank, no locale, which
			// is exactly what we know about these people.
			(*players)[info.m_name] = info;
		}
	}

	PopulateLobbyPlayerListbox();
}

/// Ask the relay what games exist. The replies arrive asynchronously on the transport's own socket,
/// so nothing is waited on - relayLobbyUpdate notices the count moving and rebuilds.
static void relayRequestGameList()
{
	Transport *transport = relayTransport();
	if (transport == nullptr)
		return;

	transport->requestGameList();
	s_relayLastQueryTime = timeGetTime();

	// A re-query that returns the same NUMBER of games can still be a different set of games -
	// one host leaves as another arrives - and the count alone would call that "no change" and
	// never redraw. Clearing what we last built from forces the settled count, whatever it turns
	// out to be, to rebuild.
	s_relayCountLastFrame = -1;
	s_relayCountLastBuilt = -1;
}

/// Host a game on the relay. Same two calls the Direct Connect screen makes; the LAN callbacks push
/// the game options screen from OnGameCreate.
static void relayHostGame()
{
	if (TheLAN == nullptr)
		return;

	UnsignedInt localIP = TheLAN->GetLocalIP();
	UnicodeString localIPString;
	localIPString.format(L"%d.%d.%d.%d", PRINTF_IP_AS_4_INTS(localIP));

	// This layout has no name entry - GameSpy took the name from the login, which we do not have.
	// The LAN preferences hold the same name the Direct Connect and LAN screens use, so a player
	// keeps one identity across all three rather than being renamed by which screen they came in
	// through.
	LANPreferences userprefs;
	UnicodeString name = userprefs.getUserName();
	if (name.isEmpty())
		name = TheGameText->fetch("GUI:Player");
	name.truncateTo(g_lanPlayerNameLength);

	TheLAN->RequestSetName(name);
	TheLAN->RequestGameCreate(localIPString, TRUE);
}

/// Join the selected game.
///
/// The sequence below is lifted from JoinDirectConnectGame and none of it is interchangeable:
///
///   1. take the room from the LISTING, never from Options.ini - the game we picked decides where
///      it lives, and our own configuration says nothing about it;
///   2. ask the relay for an identity IN THAT ROOM, because addresses are allocated per room and
///      the one we hold was allocated somewhere else. Carrying it over would not be a private
///      address in a new place, it would be somebody else's address;
///   3. throw TheLAN away and build a new one, so no LANGameInfo is alive across the change.
///      LANGameInfo snapshots the local IP in its constructor and AmIHost and getLocalSlotNum read
///      that snapshot rather than TheLAN - an identity that lands after one exists reaches nothing
///      that decides anything;
///   4. only then SetLocalIP, which rebinds the transport and re-runs initRelay against the new
///      room and identity;
///   5. and only then dial the host.
static void relayJoinSelectedGame()
{
	if (TheLAN == nullptr)
		return;

	const Int selectedID = relaySelectedGameID();
	const RelayListedGame *game = (selectedID > 0) ? relayFindGameByID(selectedID) : nullptr;
	if (game == nullptr)
	{
		GSMessageBoxOk(TheGameText->fetch("GUI:Error"), TheGameText->fetch("GUI:NoGameSelected"), nullptr);
		return;
	}

	if (game->slots > 0 && game->players >= game->slots)
	{
		GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"), TheGameText->fetch("GUI:JoinFailedRoomFull"));
		return;
	}

	const UnsignedInt hostIP = game->hostVirtualIP;
	AsciiString targetRoom(game->room);

	// Deliberately NOT SetLobbyAttemptHostJoin(TRUE). On the GameSpy path that flag is cleared by a
	// response from the peer thread, and there is no peer thread here: a join that times out would
	// leave it set forever, and Back is gated on it too - so a single unreachable host would lock
	// the player on this screen with every button dead. LANAPI already refuses a second attempt
	// (RequestGameJoinDirectConnect answers RET_BUSY while m_pendingAction is set), which is the
	// same protection the Direct Connect screen relies on.

	Transport *transport = TheLAN->getTransport();
	if (transport != nullptr && transport->isRelayEnabled() && !targetRoom.isEmpty()
		&& strcmp(targetRoom.str(), transport->getRelayRoom()) != 0)
	{
		OptionPreferences optprefs;
		const AsciiString relayHost = optprefs.getRelayAddress();

		UnsignedInt joinIP = 0;
		if (Transport::enterRelayRoom(relayHost.str(), targetRoom.str(), joinIP))
		{
			DEBUG_LOG(("relayJoinSelectedGame - moving from room '%s' to '%s'; we are %d.%d.%d.%d there",
				transport->getRelayRoom(), targetRoom.str(), PRINTF_IP_AS_4_INTS(joinIP)));

			delete TheLAN;
			TheLAN = NEW LANAPI();
			DEBUG_ASSERTCRASH(TheLAN->GetMyGame() == nullptr,
				("relayJoinSelectedGame - a LANGameInfo already exists and has snapshotted the old local IP; SetLocalIP below will not reach it"));
			TheLAN->init();
			TheLAN->SetLocalIP(joinIP);

			// The browser was built from the old transport's replies and the new one has heard
			// nothing yet. Re-ask, so a failed join leaves a screen that refills itself instead of
			// an empty list.
			relayRequestGameList();
		}
		else
		{
			// Dialling anyway would reach the relay but never the host: we are not a member of the
			// room the packet has to be forwarded in. Say so rather than letting it time out as a
			// generic unreachable peer.
			DEBUG_LOG(("relayJoinSelectedGame - the relay would not give us an identity in room '%s'; the join will not reach the host",
				targetRoom.str()));
		}
	}

	LANPreferences userprefs;
	UnicodeString name = userprefs.getUserName();
	if (name.isEmpty())
		name = TheGameText->fetch("GUI:Player");
	name.truncateTo(g_lanPlayerNameLength);

	TheLAN->RequestSetName(name);
	TheLAN->RequestGameJoinDirectConnect(hostIP);
}

/// Per-frame work for the relay lobby. Nothing else pumps the transport on this screen, and without
/// a pump the relay's replies sit in the socket and the browser stays empty - the same trap that
/// made the headless driver have to pump LANAPI itself.
static void relayLobbyUpdate()
{
	if (TheLAN == nullptr)
		return;

	Transport *transport = relayTransport();
	if (transport == nullptr)
		return;

	TheLAN->update();

	// Re-ask on the same cadence the screen already refreshes on, so a game that appears or dies
	// after we arrived shows up without the player pressing anything. Hosts re-advertise every 5 s
	// and the relay delists purely by not hearing from them, so this is also how a dead host leaves
	// the list.
	if (s_relayLastQueryTime == 0
		|| (time_t)(s_relayLastQueryTime + gameListRefreshInterval) <= (time_t)timeGetTime())
	{
		relayRequestGameList();
	}

	const Int count = transport->getGameListCount();
	if (count == s_relayCountLastFrame && count != s_relayCountLastBuilt)
	{
		s_relayCountLastBuilt = count;
		relayRebuildStagingRooms();

		// Redraw directly rather than through refreshGameList(), which only redraws when
		// hasStagingRoomListChanged() says so - and that flag is only ever raised by addStagingRoom.
		// A sweep that returns NO games adds nothing, so it never raises it, and the screen would go
		// on showing rows for games that have all gone away. That is the one case where a redraw
		// matters most: every row on screen is now unjoinable.
		RefreshGameListBoxes();

		// A game we had selected may be gone. Nothing else notices, because the selection ID stays
		// whatever it was and the player list would keep showing a room that no longer exists.
		s_relaySelectedID = -1;
	}
	s_relayCountLastFrame = count;

	const Int selectedID = relaySelectedGameID();
	if (selectedID != s_relaySelectedID)
	{
		s_relaySelectedID = selectedID;
		relayRefreshPlayerList();

		// Join follows the selection here too, not just on a click: a rebuild can drop the game the
		// player had highlighted, and Join would otherwise stay lit over nothing.
		if (buttonJoin)
			buttonJoin->winEnable(selectedID > 0);
	}
}

#if defined(RTS_DEBUG)
Bool g_fakeCRC = FALSE;
Bool g_debugSlots = FALSE;
#endif

std::list<PeerResponse> TheLobbyQueuedUTMs;

// Slash commands -------------------------------------------------------------------------
extern "C" {
int getQR2HostingStatus();
}
extern int isThreadHosting;

Bool handleLobbySlashCommands(UnicodeString uText)
{
	AsciiString message;
	message.translate(uText);

	if (message.getCharAt(0) != '/')
	{
		return FALSE; // not a slash command
	}

	AsciiString remainder = message.str() + 1;
	AsciiString token;
	remainder.nextToken(&token);
	token.toLower();

	if (token == "host")
	{
		UnicodeString s;
		s.format(L"Hosting qr2:%d thread:%d", getQR2HostingStatus(), isThreadHosting);
		TheGameSpyInfo->addText(s, GameSpyColor[GSCOLOR_DEFAULT], nullptr);
		return TRUE; // was a slash command
	}
	else if (token == "me" && uText.getLength()>4)
	{
		TheGameSpyInfo->sendChat(UnicodeString(uText.str()+4), TRUE, listboxLobbyPlayers);
		return TRUE; // was a slash command
	}
	else if (token == "refresh")
	{
		// Added 2/19/03 added the game refresh
		refreshGameList(TRUE);
		refreshPlayerList(TRUE);
		return TRUE; // was a slash command
	}
	/*
	if (token == "togglegamelist")
	{
		NameKeyType buttonID = NAMEKEY("WOLCustomLobby.wnd:ButtonGameListToggle");
		GameWindow *button = TheWindowManager->winGetWindowFromId(parent, buttonID);
		if (button)
		{
			button->winHide(!button->winIsHidden());
		}
		return TRUE; // was a slash command
	}
	else if (token == "adjustchat")
	{
		NameKeyType sliderID = NAMEKEY("WOLCustomLobby.wnd:SliderChatAdjust");
		GameWindow *slider = TheWindowManager->winGetWindowFromId(parent, sliderID);
		if (slider)
		{
			slider->winHide(!slider->winIsHidden());
		}
		return TRUE; // was a slash command
	}
	*/
#if defined(RTS_DEBUG)
	else if (token == "fakecrc")
	{
		g_fakeCRC = !g_fakeCRC;
		TheGameSpyInfo->addText(L"Toggled CRC fakery", GameSpyColor[GSCOLOR_DEFAULT], nullptr);
		return TRUE; // was a slash command
	}
	else if (token == "slots")
	{
		g_debugSlots = !g_debugSlots;
		TheGameSpyInfo->addText(L"Toggled SlotList debug", GameSpyColor[GSCOLOR_DEFAULT], nullptr);
		return TRUE; // was a slash command
	}
#endif

	return FALSE; // not a slash command
}

static Bool s_tryingToHostOrJoin = FALSE;
void SetLobbyAttemptHostJoin(Bool start)
{
	s_tryingToHostOrJoin = start;
}

// Tooltips -------------------------------------------------------------------------------

static void playerTooltip(GameWindow *window,
													WinInstanceData *instData,
													UnsignedInt mouse)
{
	Int x, y, row, col;
	x = LOLONGTOSHORT(mouse);
	y = HILONGTOSHORT(mouse);

	GadgetListBoxGetEntryBasedOnXY(window, x, y, row, col);

	if (row == -1 || col == -1)
	{
		TheMouse->setCursorTooltip( UnicodeString::TheEmptyString);//TheGameText->fetch("TOOLTIP:PlayersInLobby") );
		return;
	}

	UnicodeString uName = GadgetListBoxGetText(window, row, COLUMN_PLAYERNAME);
	AsciiString aName;
	aName.translate(uName);

	PlayerInfoMap::iterator it = TheGameSpyInfo->getPlayerInfoMap()->find(aName);
	PlayerInfo *info = &(it->second);
	Bool isLocalPlayer = (TheGameSpyInfo->getLocalName().compareNoCase(info->m_name) == 0);

	if (col == 0)
	{
		if (info->m_preorder)
		{
			TheMouse->setCursorTooltip( TheGameText->fetch("TOOLTIP:LobbyOfficersClub") );
		}
		else
		{
			TheMouse->setCursorTooltip( UnicodeString::TheEmptyString);
		}
		return;
	}

	AsciiString	playerLocale = info->m_locale;
	AsciiString localeIdentifier;
	localeIdentifier.format("WOL:Locale%2.2d", atoi(playerLocale.str()));
	Int					playerWins   = info->m_wins;
	Int					playerLosses = info->m_losses;
	UnicodeString	playerInfo;
	playerInfo.format(TheGameText->fetch("TOOLTIP:PlayerInfo"), TheGameText->fetch(localeIdentifier).str(), playerWins, playerLosses);

	UnicodeString tooltip = UnicodeString::TheEmptyString;//TheGameText->fetch("TOOLTIP:PlayersInLobby");
	if (isLocalPlayer)
	{
		tooltip.format(TheGameText->fetch("TOOLTIP:LocalPlayer"), uName.str());
	}
	else
	{
		// not us
		if (TheGameSpyInfo->getBuddyMap()->find(info->m_profileID) != TheGameSpyInfo->getBuddyMap()->end())
		{
			// buddy
			tooltip.format(TheGameText->fetch("TOOLTIP:BuddyPlayer"), uName.str());
		}
		else
		{
			if (info->m_profileID)
			{
				// non-buddy profiled player
				tooltip.format(TheGameText->fetch("TOOLTIP:ProfiledPlayer"), uName.str());
			}
			else
			{
				// non-profiled player
				tooltip.format(TheGameText->fetch("TOOLTIP:GenericPlayer"), uName.str());
			}
		}
	}

	if (info->isIgnored())
	{
		tooltip.concat(TheGameText->fetch("TOOLTIP:IgnoredModifier"));
	}

	if (info->m_profileID)
	{
		tooltip.concat(playerInfo);
	}

	Int rank = 0;
	Int i = 0;
	while( info->m_rankPoints >= TheRankPointValues->m_ranks[i + 1])
		++i;
	rank = i;
	AsciiString sideName = "GUI:RandomSide";
	if (info->m_side > 0)
	{
		const PlayerTemplate *fac = ThePlayerTemplateStore->getNthPlayerTemplate(info->m_side);
		if (fac)
		{
			sideName.format("SIDE:%s", fac->getSide().str());
		}
	}
	AsciiString rankName;
	rankName.format("GUI:GSRank%d", rank);
	UnicodeString tmp;
	tmp.format(L"\n%ls %ls", TheGameText->fetch(sideName).str(), TheGameText->fetch(rankName).str());
	tooltip.concat(tmp);

	TheMouse->setCursorTooltip( tooltip, -1, nullptr, 1.5f ); // the text and width are the only params used.  the others are the default values.
}

static void populateGroupRoomListbox(GameWindow *lb)
{
	if (!lb)
		return;

	GadgetComboBoxReset(lb);
	Int indexToSelect = -1;
	GroupRoomMap::iterator iter;

	// now populate the combo box
	for (iter = TheGameSpyInfo->getGroupRoomList()->begin(); iter != TheGameSpyInfo->getGroupRoomList()->end(); ++iter)
	{
		GameSpyGroupRoom room = iter->second;
		if (room.m_groupID != TheGameSpyConfig->getQMChannel())
		{
			DEBUG_LOG(("populateGroupRoomListbox(): groupID %d", room.m_groupID));
			if (room.m_groupID == TheGameSpyInfo->getCurrentGroupRoom())
			{
				Int selected = GadgetComboBoxAddEntry(lb, room.m_translatedName, GameSpyColor[GSCOLOR_CURRENTROOM]);
				GadgetComboBoxSetItemData(lb, selected, (void *)(room.m_groupID));
				indexToSelect = selected;
			}
			else
			{
				Int selected = GadgetComboBoxAddEntry(lb, room.m_translatedName, GameSpyColor[GSCOLOR_ROOM]);
				GadgetComboBoxSetItemData(lb, selected, (void *)(room.m_groupID));
			}
		}
		else
		{
			DEBUG_LOG(("populateGroupRoomListbox(): skipping QM groupID %d", room.m_groupID));
		}
	}

	GadgetComboBoxSetSelectedPos(lb, indexToSelect);
}

static const char *const rankNames[] = {
	"Private",
	"Corporal",
	"Sergeant",
	"Lieutenant",
	"Captain",
	"Major",
	"Colonel",
	"General",
	"Brigadier",
	"Commander",
};
static_assert(ARRAY_SIZE(rankNames) == MAX_RANKS, "Incorrect array size");


const Image* LookupSmallRankImage(Int side, Int rankPoints)
{
	if (rankPoints == 0)
		return nullptr;

	Int rank = 0;
	Int i = 0;
	while( rankPoints >= TheRankPointValues->m_ranks[i + 1])
		++i;
	rank = i;

	if (rank < 0 || rank >= 10)
		return nullptr;

	AsciiString sideStr = "N";
	switch(side)
	{
		case 2:  //USA
		case 5:  //Super Weapon
		case 6:  //Laser
		case 7:  //Air Force
			sideStr = "USA";
			break;

		case 3:  //China
		case 8:  //Tank
		case 9:  //Infantry
		case 10: //Nuke
			sideStr = "CHA";
			break;

		case 4:  //GLA
		case 11: //Toxin
		case 12: //Demolition
		case 13: //Stealth
			sideStr = "GLA";
			break;
	}

	AsciiString fullImageName;
	fullImageName.format("%s-%s", rankNames[rank], sideStr.str());
	const Image *img = TheMappedImageCollection->findImageByName(fullImageName);
	DEBUG_ASSERTLOG(img, ("*** Could not load small rank image '%s' from TheMappedImageCollection!", fullImageName.str()));
	return img;
}

static Int insertPlayerInListbox(const PlayerInfo& info, Color color)
{
	UnicodeString uStr;
	uStr.translate(info.m_name);

	Int currentRank = info.m_rankPoints;
	Int currentSide = info.m_side;
	/* since PersistentStorage updates now update PlayerInfo, we don't need this.
	if (info.m_profileID)
	{
		PSPlayerStats psStats = TheGameSpyPSMessageQueue->findPlayerStatsByID(info.m_profileID);
		if (psStats.id)
		{
			currentRank = CalculateRank(psStats);

			PerGeneralMap::iterator it;
			Int numGames = 0;
			for(it = psStats.games.begin(); it != psStats.games.end(); ++it)
			{
				if(it->second >= numGames)
				{
					numGames = it->second;
					currentSide = it->first;
				}
			}
			if(numGames == 0 || psStats.gamesAsRandom >= numGames )
			{
				currentSide = 0;
			}
		}
	}
	*/

	Bool isPreorder = TheGameSpyInfo->didPlayerPreorder(info.m_profileID);

	const Image *preorderImg = TheMappedImageCollection->findImageByName("OfficersClubsmall");
	Int w = (preorderImg)?preorderImg->getImageWidth():10;
	//Int h = (preorderImg)?preorderImg->getImageHeight():10;
	w = min(GadgetListBoxGetColumnWidth(listboxLobbyPlayers, 0), w);
	Int h = w;
	if (!isPreorder)
		preorderImg = nullptr;

	const Image *rankImg = LookupSmallRankImage(currentSide, currentRank);

#if 0  //Officer's Club (preorder image) no longer used in Zero Hour
	Int index = GadgetListBoxAddEntryImage(listboxLobbyPlayers, preorderImg, -1, 0, w, h);
	GadgetListBoxAddEntryImage(listboxLobbyPlayers, rankImg, index, 1, w, h);
	GadgetListBoxAddEntryText(listboxLobbyPlayers, uStr, color, index, 2);
#else
	Int index = GadgetListBoxAddEntryImage(listboxLobbyPlayers, rankImg, -1, 0, w, h);
	GadgetListBoxAddEntryText(listboxLobbyPlayers, uStr, color, index, 1);
#endif
	return index;
}


void PopulateLobbyPlayerListbox()
{

	if (!listboxLobbyPlayers)
		return;

	// Display players
	PlayerInfoMap *players = TheGameSpyInfo->getPlayerInfoMap();
	PlayerInfoMap::iterator it;
	BuddyInfoMap *buddies = TheGameSpyInfo->getBuddyMap();
	BuddyInfoMap::iterator bIt;
	if (listboxLobbyPlayers)
	{
		// save off old selection
		Int maxSelectedItems = GadgetListBoxGetNumEntries(listboxLobbyPlayers);
		Int *selectedIndices;
		GadgetListBoxGetSelected(listboxLobbyPlayers, (Int *)(&selectedIndices));
		std::set<AsciiString> selectedNames;
		std::set<AsciiString>::const_iterator selIt;
		std::set<Int> indicesToSelect;
		UnicodeString uStr;
		Int numSelected = 0;
		Int i=0;
		for (; i<maxSelectedItems; ++i)
		{
			if (selectedIndices[i] < 0)
			{
				break;
			}
			++numSelected;
			AsciiString selectedName;
			uStr = GadgetListBoxGetText(listboxLobbyPlayers, selectedIndices[i], COLUMN_PLAYERNAME);
			selectedName.translate(uStr);
			selectedNames.insert(selectedName);
			DEBUG_LOG(("Saving off old selection %d (%s)", selectedIndices[i], selectedName.str()));
		}

		// save off old top entry
		Int previousTopIndex = GadgetListBoxGetTopVisibleEntry(listboxLobbyPlayers);

		GadgetListBoxReset(listboxLobbyPlayers);

		// Ops
		for (it = players->begin(); it != players->end(); ++it)
		{
			PlayerInfo info = it->second;
			if (info.m_flags & PEER_FLAG_OP || TheGameSpyConfig->isPlayerVIP(info.m_profileID))
			{
				Int index = insertPlayerInListbox(info, info.isIgnored()?GameSpyColor[GSCOLOR_PLAYER_IGNORED]:GameSpyColor[GSCOLOR_PLAYER_OWNER]);

				selIt = selectedNames.find(info.m_name);
				if (selIt != selectedNames.end())
				{
					DEBUG_LOG(("Marking index %d (%s) to re-select", index, info.m_name.str()));
					indicesToSelect.insert(index);
				}
			}
		}

		// Buddies
		for (it = players->begin(); it != players->end(); ++it)
		{
			PlayerInfo info = it->second;
			bIt = buddies->find(info.m_profileID);
			if ( !(info.m_flags & PEER_FLAG_OP || TheGameSpyConfig->isPlayerVIP(info.m_profileID)) && bIt != buddies->end() )
			{
				Int index = insertPlayerInListbox(info, info.isIgnored()?GameSpyColor[GSCOLOR_PLAYER_IGNORED]:GameSpyColor[GSCOLOR_PLAYER_BUDDY]);

				selIt = selectedNames.find(info.m_name);
				if (selIt != selectedNames.end())
				{
					DEBUG_LOG(("Marking index %d (%s) to re-select", index, info.m_name.str()));
					indicesToSelect.insert(index);
				}
			}
		}

		// Everyone else
		for (it = players->begin(); it != players->end(); ++it)
		{
			PlayerInfo info = it->second;
			bIt = buddies->find(info.m_profileID);
			if ( !(info.m_flags & PEER_FLAG_OP || TheGameSpyConfig->isPlayerVIP(info.m_profileID)) && bIt == buddies->end() )
			{
				Int index = insertPlayerInListbox(info, info.isIgnored()?GameSpyColor[GSCOLOR_PLAYER_IGNORED]:GameSpyColor[GSCOLOR_PLAYER_NORMAL]);

				selIt = selectedNames.find(info.m_name);
				if (selIt != selectedNames.end())
				{
					DEBUG_LOG(("Marking index %d (%s) to re-select", index, info.m_name.str()));
					indicesToSelect.insert(index);
				}
			}
		}

		// restore selection
		if (!indicesToSelect.empty())
		{
			std::set<Int>::const_iterator indexIt = indicesToSelect.begin();
			const size_t count = indicesToSelect.size();
			size_t index = 0;
			Int *newIndices = NEW Int[count];
			while (index < count)
			{
				newIndices[index] = *indexIt;
				DEBUG_LOG(("Queueing up index %d to re-select", *indexIt));
				++index;
				++indexIt;
			}
			GadgetListBoxSetSelected(listboxLobbyPlayers, newIndices, count);
			delete[] newIndices;
		}

		if (indicesToSelect.size() != numSelected)
		{
			TheWindowManager->winSetLoneWindow(nullptr);
		}

		// restore top visible entry
		GadgetListBoxSetTopVisibleEntry(listboxLobbyPlayers, previousTopIndex);
	}

}

//-------------------------------------------------------------------------------------------------
/** Initialize the WOL Lobby Menu */
//-------------------------------------------------------------------------------------------------
void WOLLobbyMenuInit( WindowLayout *layout, void *userData )
{
	nextScreen = nullptr;
	buttonPushed = false;
	isShuttingDown = false;

	SetLobbyAttemptHostJoin(FALSE); // not trying to host or join

	gameListRefreshTime = 0;
	playerListRefreshTime = 0;

	// GeneralsX @feature Matchmaking. Decide, once, who is driving this screen.
	//
	// A null peer message queue means no GameSpy backend was ever started - which is now the only
	// way anyone reaches this layout, since the Online button pushes it directly instead of running
	// the patch-check -> login -> peerchat chain. Everything GameSpy-shaped below is skipped in that
	// case, and every one of those calls would otherwise dereference a null queue.
	//
	// This runs BEFORE the singletons are touched, because SetUpOnlineLobbyLAN destroys and rebuilds
	// TheLAN and the relay identity has to be pinned before any LANGameInfo exists.
	s_relayLobby = (TheGameSpyPeerMessageQueue == nullptr);
	if (s_relayLobby)
	{
		relayResetGameTable();
		SetUpOnlineLobbyLAN();

		if (TheGameSpyInfo == nullptr)
		{
			SetUpGameSpyForRelayLobby();
			s_relayOwnsGameSpy = TRUE;
		}
	}

	parentWOLLobbyID = TheNameKeyGenerator->nameToKey( "WOLCustomLobby.wnd:WOLLobbyMenuParent" );
	parent = TheWindowManager->winGetWindowFromId(nullptr, parentWOLLobbyID);

	buttonBackID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ButtonBack");
	buttonBack = TheWindowManager->winGetWindowFromId(parent, buttonBackID);

	buttonHostID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ButtonHost");
	buttonHost = TheWindowManager->winGetWindowFromId(parent, buttonHostID);

	buttonRefreshID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ButtonRefresh");
	buttonRefresh = TheWindowManager->winGetWindowFromId(parent, buttonRefreshID);

	buttonJoinID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ButtonJoin");
	buttonJoin = TheWindowManager->winGetWindowFromId(parent, buttonJoinID);
	buttonJoin->winEnable(FALSE);

	buttonBuddyID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ButtonBuddy");
	buttonBuddy = TheWindowManager->winGetWindowFromId(parent, buttonBuddyID);

	buttonEmoteID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ButtonEmote");
	buttonEmote = TheWindowManager->winGetWindowFromId(parent, buttonEmoteID);

	textEntryChatID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:TextEntryChat");
	textEntryChat = TheWindowManager->winGetWindowFromId(parent, textEntryChatID);

	listboxLobbyPlayersID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ListboxPlayers");
	listboxLobbyPlayers = TheWindowManager->winGetWindowFromId(parent, listboxLobbyPlayersID);
	listboxLobbyPlayers->winSetTooltipFunc(playerTooltip);

	listboxLobbyChatID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ListboxChat");
	listboxLobbyChat = TheWindowManager->winGetWindowFromId(parent, listboxLobbyChatID);
	TheGameSpyInfo->registerTextWindow(listboxLobbyChat);

	comboLobbyGroupRoomsID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ComboBoxGroupRooms");
	comboLobbyGroupRooms = TheWindowManager->winGetWindowFromId(parent, comboLobbyGroupRoomsID);

	GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);

	if (s_relayLobby)
	{
		// GeneralsX @feature Matchmaking. Turn off the controls that have nothing behind them.
		//
		// Chat is a real gap, not an oversight: the relay carries no chat channel at all. It would
		// be a small protocol addition - one more plaintext line alongside GXRLY and GXADV - but
		// unauthenticated chat on a public relay is a moderation problem before it is a feature, and
		// nothing here is going to fake it locally. The box stays, inert, with one line saying so;
		// an enabled entry that swallows every message would be worse than an obviously dead one.
		//
		// Buddies are GameSpy profiles. There are no accounts on the relay, so there is nothing a
		// buddy list could be a list OF, and GameSpyOpenOverlay(GSOVERLAY_BUDDY) dereferences the
		// buddy message queue we deliberately never created.
		if (textEntryChat)
			textEntryChat->winEnable(FALSE);
		if (buttonEmote)
			buttonEmote->winEnable(FALSE);
		if (buttonBuddy)
			buttonBuddy->winEnable(FALSE);

		// The group-room dropdown listed GameSpy chat lobbies. Ours is one relay, so show which one
		// and disable it, rather than leaving an empty dropdown that reads as broken.
		if (comboLobbyGroupRooms)
		{
			GadgetComboBoxReset(comboLobbyGroupRooms);
			OptionPreferences prefs;
			AsciiString relayHost = prefs.getRelayAddress();
			UnicodeString label;
			if (relayHost.isEmpty())
				label = TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayLobbyNoRelay", L"No server configured");
			else
				label.translate(relayHost);
			GadgetComboBoxAddEntry(comboLobbyGroupRooms, label, GameSpyColor[GSCOLOR_CURRENTROOM]);
			GadgetComboBoxSetSelectedPos(comboLobbyGroupRooms, 0, FALSE);
			comboLobbyGroupRooms->winEnable(FALSE);
		}
	}
	else
	{
		populateGroupRoomListbox(comboLobbyGroupRooms);
	}

	// Show Menu
	layout->hide( FALSE );

	// if we're not in a room, this will join the best available one
	if (s_relayLobby)
	{
		// No group rooms: they are GameSpy chat channels, and joining one is a peerchat round trip.
		// Our rooms are relay rooms and we are already in ours - SetUpOnlineLobbyLAN put us there.
		TheGameSpyInfo->addText(
			TheGameText->FETCH_OR_SUBSTITUTE("GUI:RelayLobbyNoChat", L"Chat is not available on this server."),
			GameSpyColor[GSCOLOR_MOTD], listboxLobbyChat);
	}
	else if (!TheGameSpyInfo->getCurrentGroupRoom())
	{
		if (groupRoomToJoin)
		{
			DEBUG_LOG(("WOLLobbyMenuInit() - rejoining group room %d", groupRoomToJoin));
			TheGameSpyInfo->joinGroupRoom(groupRoomToJoin);
			groupRoomToJoin = 0;
		}
		else
		{
			DEBUG_LOG(("WOLLobbyMenuInit() - joining best group room"));
			TheGameSpyInfo->joinBestGroupRoom();
		}
	}
	else
	{
		DEBUG_LOG(("WOLLobbyMenuInit() - not joining group room because we're already in one"));
	}

	GrabWindowInfo();

	TheGameSpyInfo->clearStagingRoomList();
	if (s_relayLobby)
	{
		// GeneralsX @feature Matchmaking. The relay's answer to PEERREQUEST_STARTGAMELIST. One
		// GXLIST out, one GXGAME datagram per game back, asynchronously on the transport's own
		// socket - relayLobbyUpdate notices them arriving.
		relayRequestGameList();
	}
	else
	{
		PeerRequest req;
		req.peerRequestType = PeerRequest::PEERREQUEST_STARTGAMELIST;
		req.gameList.restrictGameList = TheGameSpyConfig->restrictGamesToLobby();
		TheGameSpyPeerMessageQueue->addRequest(req);
	}

	// animate controls
//	TheShell->registerWithAnimateManager(parent, WIN_ANIMATION_SLIDE_TOP, TRUE);
	TheShell->showShellMap(TRUE);
	TheGameSpyGame->reset();

	CustomMatchPreferences pref;
//	GameWindow *slider = TheWindowManager->winGetWindowFromId(parent, sliderChatAdjustID);
//	if (slider)
//	{
//		GadgetSliderSetPosition(slider, pref.getChatSizeSlider());
//		doSliderTrack(slider, pref.getChatSizeSlider());
//	}
//
	if (pref.usesLongGameList())
	{
		ToggleGameListType();
	}

	// Set Keyboard to chat window
	if (!s_relayLobby)
		TheWindowManager->winSetFocus( textEntryChat );
	raiseMessageBoxes = true;

	TheLobbyQueuedUTMs.clear();
	justEntered = TRUE;
	initialGadgetDelay = 2;
	GameWindow *win = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:GadgetParent"));
	if(win)
		win->winHide(TRUE);
	DontShowMainMenu = TRUE;

}

//-------------------------------------------------------------------------------------------------
/** This is called when a shutdown is complete for this menu */
//-------------------------------------------------------------------------------------------------
static void shutdownComplete( WindowLayout *layout )
{

	isShuttingDown = false;

	// hide the layout
	layout->hide( TRUE );

	// our shutdown is complete
	TheShell->shutdownComplete( layout, (nextScreen != nullptr) );

	if (nextScreen != nullptr)
	{
		TheShell->push(nextScreen);
	}

	nextScreen = nullptr;

}

//-------------------------------------------------------------------------------------------------
/** WOL Lobby Menu shutdown method */
//-------------------------------------------------------------------------------------------------
void WOLLobbyMenuShutdown( WindowLayout *layout, void *userData )
{
	CustomMatchPreferences pref;
//	GameWindow *slider = TheWindowManager->winGetWindowFromId(parent, sliderChatAdjustID);
//	if (slider)
//	{
//		pref.setChatSizeSlider(GadgetSliderGetPosition(slider));
//	}
	if (GetGameInfoListBox())
	{
		pref.setUsesLongGameList(FALSE);
	}
	else
	{
		pref.setUsesLongGameList(TRUE);
	}
	pref.write();

	ReleaseWindowInfo();

	if (TheGameSpyInfo)
		TheGameSpyInfo->unregisterTextWindow(listboxLobbyChat);

	//TheGameSpyChat->stopListingGames();
	if (TheGameSpyPeerMessageQueue)
	{
		PeerRequest req;
		req.peerRequestType = PeerRequest::PEERREQUEST_STOPGAMELIST;
		TheGameSpyPeerMessageQueue->addRequest(req);
	}

	listboxLobbyChat = nullptr;
	listboxLobbyPlayers = nullptr;

	// GeneralsX @feature Matchmaking. Give the presentation singletons back.
	//
	// They exist only for the duration of this screen. Leaving them alive would change what every
	// other part of the game sees: Shell::push and Shell::pop close all GameSpy overlays whenever
	// TheGameSpyInfo is non-null, and the score screen, the recorder and GameLogic all branch on it
	// to decide whether a match was an internet game. None of that should change because somebody
	// looked at the lobby once, and MainMenuInit's own teardown will not fire for us - it is gated
	// on the peer message queue, which we never create.
	//
	// This runs on the push into the game options screen too (Shell::push shuts the current screen
	// down first), which is exactly what we want: the match itself then runs with the same globals
	// the Direct Connect path has always used. Coming back re-creates them in Init.
	if (s_relayOwnsGameSpy)
	{
		TearDownGameSpy();
		s_relayOwnsGameSpy = FALSE;
	}
	s_relayLobby = FALSE;

	isShuttingDown = true;

	// if we are shutting down for an immediate pop, skip the animations
	Bool popImmediate = *(Bool *)userData;
	if( popImmediate )
	{

		shutdownComplete( layout );
		return;

	}

	TheShell->reverseAnimatewindow();
	DontShowMainMenu = FALSE;

	RaiseGSMessageBox();
	TheTransitionHandler->reverse("WOLCustomLobbyFade");

}

static void fillPlayerInfo(const PeerResponse *resp, PlayerInfo *info)
{
	info->m_name			= resp->nick.c_str();
	info->m_profileID	= resp->player.profileID;
	info->m_flags			= resp->player.flags;
	info->m_wins			= resp->player.wins;
	info->m_losses		= resp->player.losses;
	info->m_locale		= resp->locale.c_str();
	info->m_rankPoints= resp->player.rankPoints;
	info->m_side			= resp->player.side;
	info->m_preorder	= resp->player.preorder;
}

#ifdef PERF_TEST
static const char* getMessageString(Int t)
{
	switch(t)
	{
		case PeerResponse::PEERRESPONSE_LOGIN:
			return "login";
		case PeerResponse::PEERRESPONSE_DISCONNECT:
			return "disconnect";
		case PeerResponse::PEERRESPONSE_MESSAGE:
			return "message";
		case PeerResponse::PEERRESPONSE_GROUPROOM:
			return "group room";
		case PeerResponse::PEERRESPONSE_STAGINGROOM:
			return "staging room";
		case PeerResponse::PEERRESPONSE_STAGINGROOMPLAYERINFO:
			return "staging room player info";
		case PeerResponse::PEERRESPONSE_JOINGROUPROOM:
			return "group room join";
		case PeerResponse::PEERRESPONSE_CREATESTAGINGROOM:
			return "staging room create";
		case PeerResponse::PEERRESPONSE_JOINSTAGINGROOM:
			return "staging room join";
		case PeerResponse::PEERRESPONSE_PLAYERJOIN:
			return "player join";
		case PeerResponse::PEERRESPONSE_PLAYERLEFT:
			return "player part";
		case PeerResponse::PEERRESPONSE_PLAYERCHANGEDNICK:
			return "player nick";
		case PeerResponse::PEERRESPONSE_PLAYERINFO:
			return "player info";
		case PeerResponse::PEERRESPONSE_PLAYERCHANGEDFLAGS:
			return "player flags";
		case PeerResponse::PEERRESPONSE_ROOMUTM:
			return "room UTM";
		case PeerResponse::PEERRESPONSE_PLAYERUTM:
			return "player UTM";
		case PeerResponse::PEERRESPONSE_QUICKMATCHSTATUS:
			return "QM status";
		case PeerResponse::PEERRESPONSE_GAMESTART:
			return "game start";
		case PeerResponse::PEERRESPONSE_FAILEDTOHOST:
			return "host failure";
	}
	return "unknown";
}
#endif // PERF_TEST

//-------------------------------------------------------------------------------------------------
/** refreshGameList
		The Bool is used to force refresh if the refresh button was hit.*/
//-------------------------------------------------------------------------------------------------
void refreshGameList( Bool forceRefresh )
{
	Int refreshInterval = gameListRefreshInterval;

	if (forceRefresh || ((gameListRefreshTime == 0) || ((gameListRefreshTime + refreshInterval) <= timeGetTime())))
	{
		if (TheGameSpyInfo->hasStagingRoomListChanged())
		{
			//DEBUG_LOG(("################### refreshing game list"));
			//DEBUG_LOG(("gameRefreshTime=%d, refreshInterval=%d, now=%d", gameListRefreshTime, refreshInterval, timeGetTime()));
			RefreshGameListBoxes();
			gameListRefreshTime = timeGetTime();
		} else {
			//DEBUG_LOG(("-"));
		}
	} else {
		//DEBUG_LOG(("gameListRefreshTime: %d refreshInterval: %d", gameListRefreshTime, refreshInterval));
	}
}
//-------------------------------------------------------------------------------------------------
/** refreshPlayerList
		The Bool is used to force refresh if the refresh button was hit.*/
//-------------------------------------------------------------------------------------------------
void refreshPlayerList( Bool forceRefresh )
{
		Int refreshInterval = playerListRefreshInterval;

		if (forceRefresh ||((playerListRefreshTime == 0) || ((playerListRefreshTime + refreshInterval) <= timeGetTime())))
		{
				PopulateLobbyPlayerListbox();
				playerListRefreshTime = timeGetTime();
		}
}
//-------------------------------------------------------------------------------------------------
/** WOL Lobby Menu update method */
//-------------------------------------------------------------------------------------------------
void WOLLobbyMenuUpdate( WindowLayout * layout, void *userData)
{
		if(justEntered)
	{
		if(initialGadgetDelay == 1)
		{
			TheTransitionHandler->remove("MainMenuDefaultMenuLogoFade");
			TheTransitionHandler->setGroup("WOLCustomLobbyFade");
			initialGadgetDelay = 2;
			justEntered = FALSE;
		}
		else
			initialGadgetDelay--;
	}
	if (TheGameLogic->isInShellGame() && TheGameLogic->getFrame() == 1)
	{
		SignalUIInteraction(SHELL_SCRIPT_HOOK_GENERALS_ONLINE_ENTERED_FROM_GAME);
	}


	// We'll only be successful if we've requested to
	if(isShuttingDown && TheShell->isAnimFinished() && TheTransitionHandler->isFinished())
		shutdownComplete(layout);

	if (raiseMessageBoxes)
	{
		RaiseGSMessageBox();
		raiseMessageBoxes = false;
	}

	// GeneralsX @feature Matchmaking. The relay's half of the update. The GameSpy block below is
	// already gated on TheGameSpyPeerMessageQueue, which is null here, so these are exclusive.
	if (s_relayLobby && !isShuttingDown && !buttonPushed)
	{
		relayLobbyUpdate();
	}

	if (TheShell->isAnimFinished() && TheTransitionHandler->isFinished() && !buttonPushed && TheGameSpyPeerMessageQueue)
	{
		HandleBuddyResponses();
		HandlePersistentStorageResponses();

#ifdef PERF_TEST
		UnsignedInt start = timeGetTime();
		UnsignedInt end = timeGetTime();
		std::list<Int> responses;
		Int numMessages = 0;
#endif // PERF_TEST

		Int allowedMessages = TheGameSpyInfo->getMaxMessagesPerUpdate();
		Bool sawImportantMessage = FALSE;
		Bool shouldRepopulatePlayers = FALSE;
		PeerResponse resp;
		while (allowedMessages-- && !sawImportantMessage && TheGameSpyPeerMessageQueue->getResponse( resp ))
		{
#ifdef PERF_TEST
			++numMessages;
			responses.push_back(resp.peerResponseType);
#endif // PERF_TEST
			switch (resp.peerResponseType)
			{
			case PeerResponse::PEERRESPONSE_JOINGROUPROOM:
				sawImportantMessage = TRUE;
				if (resp.joinGroupRoom.ok)
				{
					//buttonPushed = true;
					TheGameSpyInfo->setCurrentGroupRoom(resp.joinGroupRoom.id);
					TheGameSpyInfo->getPlayerInfoMap()->clear();
					GroupRoomMap::iterator iter = TheGameSpyInfo->getGroupRoomList()->find(resp.joinGroupRoom.id);
					if (iter != TheGameSpyInfo->getGroupRoomList()->end())
					{
						GameSpyGroupRoom room = iter->second;
						UnicodeString msg;
						msg.format(TheGameText->fetch("GUI:LobbyJoined"), room.m_translatedName.str());
						TheGameSpyInfo->addText(msg, GameSpyColor[GSCOLOR_DEFAULT], nullptr);
					}
				}
				else
				{
					DEBUG_LOG(("WOLLobbyMenuUpdate() - joining best group room"));
					TheGameSpyInfo->joinBestGroupRoom();
				}
				populateGroupRoomListbox(comboLobbyGroupRooms);
				shouldRepopulatePlayers = TRUE;
				break;
			case PeerResponse::PEERRESPONSE_PLAYERCHANGEDFLAGS:
				{
					PlayerInfo p;
					fillPlayerInfo(&resp, &p);
					TheGameSpyInfo->updatePlayerInfo(p);
					shouldRepopulatePlayers = TRUE;
				}
				break;
			case PeerResponse::PEERRESPONSE_PLAYERCHANGEDNICK:
				{
					PlayerInfo p;
					fillPlayerInfo(&resp, &p);
					TheGameSpyInfo->updatePlayerInfo(p);
					shouldRepopulatePlayers = TRUE;
				}
				break;
			case PeerResponse::PEERRESPONSE_PLAYERINFO:
				{
					PlayerInfo p;
					fillPlayerInfo(&resp, &p);
					TheGameSpyInfo->updatePlayerInfo(p);
					shouldRepopulatePlayers = TRUE;
				}
				break;
			case PeerResponse::PEERRESPONSE_PLAYERJOIN:
				{
					if (resp.player.roomType == GroupRoom)
					{
						PlayerInfo p;
						fillPlayerInfo(&resp, &p);
						TheGameSpyInfo->updatePlayerInfo(p);
						shouldRepopulatePlayers = TRUE;
					}
				}
				break;
			case PeerResponse::PEERRESPONSE_PLAYERUTM:
			case PeerResponse::PEERRESPONSE_ROOMUTM:
				{
					DEBUG_LOG(("Putting off a UTM in the lobby"));
					TheLobbyQueuedUTMs.push_back(resp);
				}
				break;
			case PeerResponse::PEERRESPONSE_PLAYERLEFT:
				{
					PlayerInfo p;
					fillPlayerInfo(&resp, &p);
					TheGameSpyInfo->playerLeftGroupRoom(resp.nick.c_str());
					shouldRepopulatePlayers = TRUE;
				}
				break;
			case PeerResponse::PEERRESPONSE_MESSAGE:
				{
					TheGameSpyInfo->addChat(resp.nick.c_str(), resp.message.profileID,
						UnicodeString(resp.text.c_str()), !resp.message.isPrivate, resp.message.isAction, listboxLobbyChat);
				}
				break;
			case PeerResponse::PEERRESPONSE_DISCONNECT:
				{
					sawImportantMessage = TRUE;
					UnicodeString title, body;
					AsciiString disconMunkee;
					disconMunkee.format("GUI:GSDisconReason%d", resp.discon.reason);
					title = TheGameText->fetch( "GUI:GSErrorTitle" );
					body = TheGameText->fetch( disconMunkee );
					GameSpyCloseAllOverlays();
					GSMessageBoxOk( title, body );
					TheGameSpyInfo->reset();
					TheShell->pop();
				}
				break;
			case PeerResponse::PEERRESPONSE_CREATESTAGINGROOM:
				{
					sawImportantMessage = TRUE;
					SetLobbyAttemptHostJoin(FALSE);
					if (resp.createStagingRoom.result == PEERJoinSuccess)
					{
						// Woohoo!  On to our next screen!
						buttonPushed = true;
						nextScreen = "Menus/GameSpyGameOptionsMenu.wnd";
						TheShell->pop();
						TheGameSpyInfo->markAsStagingRoomHost();
						TheGameSpyInfo->setGameOptions();
					}
				}
				break;
			case PeerResponse::PEERRESPONSE_JOINSTAGINGROOM:
				{
					sawImportantMessage = TRUE;
					SetLobbyAttemptHostJoin(FALSE);
					Bool isHostPresent = TRUE;
					if (resp.joinStagingRoom.ok == PEERTrue)
					{
						GameSpyStagingRoom *room = TheGameSpyInfo->getCurrentStagingRoom();
						if (!room)
						{
							isHostPresent = FALSE;
						}
						else
						{
							isHostPresent = FALSE;
							for (Int i=0; i<MAX_SLOTS; ++i)
							{
								AsciiString hostName;
								hostName.translate(room->getConstSlot(0)->getName());
								const char *firstPlayer = resp.stagingRoomPlayerNames[i].c_str();
								if (strcmp(hostName.str(), firstPlayer) == 0)
								{
									DEBUG_LOG(("Saw host %s == %s in slot %d", hostName.str(), firstPlayer, i));
									isHostPresent = TRUE;
								}
							}
						}
					}
					if (resp.joinStagingRoom.ok == PEERTrue && isHostPresent)
					{
						// Woohoo!  On to our next screen!
						buttonPushed = true;
						nextScreen = "Menus/GameSpyGameOptionsMenu.wnd";
						TheShell->pop();
					}
					else
					{
						UnicodeString s;

						switch(resp.joinStagingRoom.result)
						{
						case PEERFullRoom:        // The room is full.
							s = TheGameText->fetch("GUI:JoinFailedRoomFull");
							break;
						case PEERInviteOnlyRoom:  // The room is invite only.
							s = TheGameText->fetch("GUI:JoinFailedInviteOnly");
							break;
						case PEERBannedFromRoom:  // The local user is banned from the room.
							s = TheGameText->fetch("GUI:JoinFailedBannedFromRoom");
							break;
						case PEERBadPassword:     // An incorrect password (or none) was given for a passworded room.
							s = TheGameText->fetch("GUI:JoinFailedBadPassword");
							break;
						case PEERAlreadyInRoom:   // The local user is already in or entering a room of the same type.
							s = TheGameText->fetch("GUI:JoinFailedAlreadyInRoom");
							break;
						case PEERNoConnection:    // Can't join a room if there's no chat connection.
							s = TheGameText->fetch("GUI:JoinFailedNoConnection");
							break;
						default:
							s = TheGameText->fetch("GUI:JoinFailedDefault");
							break;
						}
						GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"), s);
						if (groupRoomToJoin)
						{
							DEBUG_LOG(("WOLLobbyMenuUpdate() - rejoining group room %d", groupRoomToJoin));
							TheGameSpyInfo->joinGroupRoom(groupRoomToJoin);
							groupRoomToJoin = 0;
						}
						else
						{
							DEBUG_LOG(("WOLLobbyMenuUpdate() - joining best group room"));
							TheGameSpyInfo->joinBestGroupRoom();
						}
					}
				}
				break;
			case PeerResponse::PEERRESPONSE_STAGINGROOMLISTCOMPLETE:
				TheGameSpyInfo->sawFullGameList();
				break;
			case PeerResponse::PEERRESPONSE_STAGINGROOM:
				{
					GameSpyStagingRoom room;
					switch(resp.stagingRoom.action)
					{
					case PEER_CLEAR:
						TheGameSpyInfo->clearStagingRoomList();
						//TheGameSpyInfo->addText( L"gameList: PEER_CLEAR", GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
						break;
					case PEER_ADD:
					case PEER_UPDATE:
					{
						if (resp.stagingRoom.percentComplete == 100)
						{
							TheGameSpyInfo->sawFullGameList();
						}

						//if (ParseAsciiStringToGameInfo(&room, resp.stagingRoomMapName.c_str()))
						//if (ParseAsciiStringToGameInfo(&room, resp.stagingServerGameOptions.c_str()))
						Bool serverOk = TRUE;
						if (resp.stagingRoomMapName.empty())
						{
							serverOk = FALSE;
						}
						// fix for ghost game problem - need to iterate over all resp.stagingRoomPlayerNames[i]
						Bool sawSelf = FALSE;
						//for (Int i=0; i<MAX_SLOTS; ++i)
						//{
							if (TheGameSpyInfo->getLocalName() == resp.stagingRoomPlayerNames[0].c_str())
							{
								sawSelf = TRUE; // don't show ghost games for myself
							}
						//}
						if (sawSelf)
							serverOk = FALSE;

						if (serverOk)
						{
							room.setGameName(UnicodeString(resp.stagingServerName.c_str()));
							room.setID(resp.stagingRoom.id);
							room.setHasPassword(resp.stagingRoom.requiresPassword);
							room.setVersion(resp.stagingRoom.version);
							room.setExeCRC(resp.stagingRoom.exeCRC);
							room.setIniCRC(resp.stagingRoom.iniCRC);
							room.setAllowObservers(resp.stagingRoom.allowObservers);
              room.setUseStats(resp.stagingRoom.useStats);
							room.setPingString(resp.stagingServerPingString.c_str());
							room.setLadderIP(resp.stagingServerLadderIP.c_str());
							room.setLadderPort(resp.stagingRoom.ladderPort);
							room.setReportedNumPlayers(resp.stagingRoom.numPlayers);
							room.setReportedMaxPlayers(resp.stagingRoom.maxPlayers);
							room.setReportedNumObservers(resp.stagingRoom.numObservers);

							Int i;
							AsciiString gsMapName = resp.stagingRoomMapName.c_str();
							AsciiString mapName = "";
							for (i=0; i<gsMapName.getLength(); ++i)
							{
								char c = gsMapName.getCharAt(i);
								if (c != '/')
									mapName.concat(c);
								else
									mapName.concat('\\');
							}
							room.setMap(TheGameState->portableMapPathToRealMapPath(mapName));

							Int numPlayers = 0;
							for (i=0; i<MAX_SLOTS; ++i)
							{
								GameSpyGameSlot *slot = room.getGameSpySlot(i);
								if (slot)
								{
									slot->setWins( resp.stagingRoom.wins[i] );
									slot->setLosses( resp.stagingRoom.losses[i] );
									slot->setProfileID( resp.stagingRoom.profileID[i] );
									slot->setPlayerTemplate( resp.stagingRoom.faction[i] );
									slot->setColor( resp.stagingRoom.color[i] );
									if (resp.stagingRoom.profileID[i] == SLOT_EASY_AI)
									{
										slot->setState(SLOT_EASY_AI);
										++numPlayers;
									}
									else if (resp.stagingRoom.profileID[i] == SLOT_MED_AI)
									{
										slot->setState(SLOT_MED_AI);
										++numPlayers;
									}
									else if (resp.stagingRoom.profileID[i] == SLOT_BRUTAL_AI)
									{
										slot->setState(SLOT_BRUTAL_AI);
										++numPlayers;
									}
									else if (!resp.stagingRoomPlayerNames[i].empty())
									{
										UnicodeString nameUStr;
										nameUStr.translate(resp.stagingRoomPlayerNames[i].c_str());
										slot->setState(SLOT_PLAYER, nameUStr);
										++numPlayers;
									}
									else
									{
										slot->setState(SLOT_OPEN);
									}
								}
							}
							DEBUG_ASSERTCRASH(numPlayers, ("Game had no players!"));
							//DEBUG_LOG(("Saw room: hasPass=%d, allowsObservers=%d", room.getHasPassword(), room.getAllowObservers()));
							if (resp.stagingRoom.action == PEER_ADD)
							{
								TheGameSpyInfo->addStagingRoom(room);
								//TheGameSpyInfo->addText( L"gameList: PEER_ADD", GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
							}
							else
							{
								TheGameSpyInfo->updateStagingRoom(room);
								//TheGameSpyInfo->addText( L"gameList: PEER_UPDATE", GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
							}
						}
						else
						{
							room.setID(resp.stagingRoom.id);
							TheGameSpyInfo->removeStagingRoom(room);
							//TheGameSpyInfo->addText( L"gameList: PEER_UPDATE FAILED", GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
						}
						break;
					}
					case PEER_REMOVE:
						room.setID(resp.stagingRoom.id);
						TheGameSpyInfo->removeStagingRoom(room);
						//TheGameSpyInfo->addText( L"gameList: PEER_REMOVE", GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
						break;
					default:
						//TheGameSpyInfo->addText( L"gameList: Unknown", GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
						break;
					}
				}
				break;
			}
		}
#if 0
		if (shouldRepopulatePlayers)
		{
			PopulateLobbyPlayerListbox();
		}
#else
		refreshPlayerList();
#endif

#ifdef PERF_TEST
		// check performance
		end = timeGetTime();
		PERF_LOG(("Frame time was %d ms", end-start));
		std::list<Int>::const_iterator it;
		for (it = responses.begin(); it != responses.end(); ++it)
		{
			PERF_LOG(("  %s", getMessageString(*it)));
		}
		PERF_LOG((""));
#endif // PERF_TEST

#if 0
// Removed 2-17-03 to pull out into a function so we can do the same checks
		Int refreshInterval = gameListRefreshInterval;

		if ((gameListRefreshTime == 0) || ((gameListRefreshTime + refreshInterval) <= timeGetTime()))
		{
			if (TheGameSpyInfo->hasStagingRoomListChanged())
			{
				//DEBUG_LOG(("################### refreshing game list"));
				//DEBUG_LOG(("gameRefreshTime=%d, refreshInterval=%d, now=%d", gameListRefreshTime, refreshInterval, timeGetTime()));
				RefreshGameListBoxes();
				gameListRefreshTime = timeGetTime();
			} else {
				//DEBUG_LOG(("-"));
			}
		} else {
			//DEBUG_LOG(("gameListRefreshTime: %d refreshInterval: %d", gameListRefreshTime, refreshInterval));
		}
#else
	refreshGameList();
#endif
	}
}

//-------------------------------------------------------------------------------------------------
/** WOL Lobby Menu input callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType WOLLobbyMenuInput( GameWindow *window, UnsignedInt msg,
																			 WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
			UnsignedByte state = mData2;
			if (buttonPushed)
				break;

			switch( key )
			{

				// ----------------------------------------------------------------------------------------
				case KEY_ESC:
				{

					//
					// send a simulated selected event to the parent window of the
					// back/exit button
					//
					if( BitIsSet( state, KEY_STATE_UP ) )
					{
						TheWindowManager->winSendSystemMsg( window, GBM_SELECTED,
																							(WindowMsgData)buttonBack, buttonBackID );

					}

					// don't let key fall through anywhere else
					return MSG_HANDLED;

				}

			}

		}

	}

	return MSG_IGNORED;
}

//static void doSliderTrack(GameWindow *control, Int val)
//{
//	Int sliderW, sliderH, sliderX, sliderY;
//	control->winGetPosition(&sliderX, &sliderY);
//	control->winGetSize(&sliderW, &sliderH);
//	Real cursorY = sliderY + (100-val)*0.01f*sliderH;
//
//	extern GameWindow *listboxLobbyGamesSmall;
//	extern GameWindow *listboxLobbyGamesLarge;
//	extern GameWindow *listboxLobbyGameInfo;
//
//	static Int gwsX = 0, gwsY = 0, gwsW = 0, gwsH = 0;
//	static Int gwlX = 0, gwlY = 0, gwlW = 0, gwlH = 0;
//	static Int gwiX = 0, gwiY = 0, gwiW = 0, gwiH = 0;
//	static Int pwX = 0, pwY = 0, pwW = 0, pwH = 0;
//	static Int chatPosX = 0, chatPosY = 0, chatW = 0, chatH = 0;
//	static Int spacing = 0;
//	if (chatPosX == 0)
//	{
//		listboxLobbyChat->winGetPosition(&chatPosX, &chatPosY);
//		listboxLobbyChat->winGetSize(&chatW, &chatH);
//
////		listboxLobbyGamesSmall->winGetPosition(&gwsX, &gwsY);
////		listboxLobbyGamesSmall->winGetSize(&gwsW, &gwsH);
//
//		listboxLobbyGamesLarge->winGetPosition(&gwlX, &gwlY);
//		listboxLobbyGamesLarge->winGetSize(&gwlW, &gwlH);
//
////		listboxLobbyGameInfo->winGetPosition(&gwiX, &gwiY);
////		listboxLobbyGameInfo->winGetSize(&gwiW, &gwiH);
////
//		listboxLobbyPlayers->winGetPosition(&pwX, &pwY);
//		listboxLobbyPlayers->winGetSize(&pwW, &pwH);
//
//		spacing = chatPosY - pwY - pwH;
//	}
//
//	Int newChatY = cursorY;
//	Int newChatH = chatH + chatPosY - newChatY;
//	listboxLobbyChat->winSetPosition(chatPosX, newChatY);
//	listboxLobbyChat->winSetSize(chatW, newChatH);
//
//	Int newH = cursorY - pwY - spacing;
//	listboxLobbyPlayers->winSetSize(pwW, newH);
////	listboxLobbyGamesSmall->winSetSize(gwsW, newH);
//	listboxLobbyGamesLarge->winSetSize(gwlW, newH);
////	listboxLobbyGameInfo->winSetSize(gwiW, newH);


//-------------------------------------------------------------------------------------------------
/** WOL Lobby Menu window system callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType WOLLobbyMenuSystem( GameWindow *window, UnsignedInt msg,
														 WindowMsgData mData1, WindowMsgData mData2 )
{
	UnicodeString txtInput;
	static NameKeyType buttonGameListTypeToggleID = NAMEKEY_INVALID;

	switch( msg )
	{


		//---------------------------------------------------------------------------------------------
		case GWM_CREATE:
			{
				buttonGameListTypeToggleID = NAMEKEY("WOLCustomLobby.wnd:ButtonGameListToggle");
//				sliderChatAdjustID = NAMEKEY("WOLCustomLobby.wnd:SliderChatAdjust");

				break;
			}

		//---------------------------------------------------------------------------------------------
		case GWM_DESTROY:
			{
				break;
			}

		//---------------------------------------------------------------------------------------------
		case GWM_INPUT_FOCUS:
			{
				// if we're givin the opportunity to take the keyboard focus we must say we want it
				if( mData1 == TRUE )
					*(Bool *)mData2 = TRUE;

				return MSG_HANDLED;
			}

		//---------------------------------------------------------------------------------------------
		case GLM_SELECTED:
			{
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				if ( controlID == GetGameListBoxID() )
				{
					int rowSelected = mData2;
					if (s_relayLobby)
					{
						// GeneralsX @feature Matchmaking. There is no extended-info round trip to make:
						// the GXGAME reply is everything the relay knows about a game. Following the
						// selection with the player list is the whole of it.
						buttonJoin->winEnable(rowSelected >= 0);
						s_relaySelectedID = relaySelectedGameID();
						relayRefreshPlayerList();
						break;
					}
					if( rowSelected >= 0 )
					{
						buttonJoin->winEnable(TRUE);
						static UnsignedInt lastFrame = 0;
						static Int lastID = -1;
						UnsignedInt now = TheGameClient->getFrame();

						PeerRequest req;
						req.peerRequestType = PeerRequest::PEERREQUEST_GETEXTENDEDSTAGINGROOMINFO;
					// GeneralsX @build BenderAI 12/02/2026 64-bit safe pointer cast
					req.stagingRoom.id = static_cast<Int>(reinterpret_cast<intptr_t>(GadgetListBoxGetItemData(control, rowSelected, 0)));

						lastID = req.stagingRoom.id;
						lastFrame = now;
					}
					else
					{
						buttonJoin->winEnable(FALSE);
					}
					if (GetGameInfoListBox())
					{
						RefreshGameInfoListBox(GetGameListBox(), GetGameInfoListBox());
					}
				}

				break;
			}

		//---------------------------------------------------------------------------------------------
		case GBM_SELECTED:
			{
				if (buttonPushed)
					break;

				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				if (HandleSortButton((NameKeyType)controlID))
					break;

				// If we back out, just bail - we haven't gotten far enough to need to log out
				if ( controlID == buttonBackID )
				{
					if (s_tryingToHostOrJoin)
						break;

					if (s_relayLobby)
					{
						// GeneralsX @feature Matchmaking. Nothing to leave - a relay room is not a
						// chat channel and leaveGroupRoom() is a peerchat request. And no next
						// screen: WOLWelcomeMenu is the GameSpy login screen, so pushing it would
						// send a player who pressed Back straight back into the dead service the
						// Online button was changed to avoid. Pop to wherever we came from.
						SetLobbyAttemptHostJoin( TRUE ); // pretend, so nothing else queues up
						buttonPushed = true;
						nextScreen = nullptr;
						TheShell->pop();
						break;
					}

					// Leave any group room, then pop off the screen
					TheGameSpyInfo->leaveGroupRoom();

					SetLobbyAttemptHostJoin( TRUE ); // pretend, since we don't want to queue up another action
					buttonPushed = true;
					nextScreen = "Menus/WOLWelcomeMenu.wnd";
					TheShell->pop();

				}
				else if ( controlID == buttonRefreshID )
				{
					// Added 2/17/03 added the game refresh button
					if (s_relayLobby)
					{
						// GeneralsX @feature Matchmaking. Redrawing what we already have is not a
						// refresh - the relay only answers when asked, so ask again first.
						relayRequestGameList();
					}
					refreshGameList(TRUE);
					refreshPlayerList(TRUE);
				}
				else if ( controlID == buttonHostID )
				{
					if (s_tryingToHostOrJoin)
						break;

					if (s_relayLobby)
					{
						// GeneralsX @feature Matchmaking. GSOVERLAY_GAMEOPTIONS is the GameSpy
						// staging-room overlay and is driven end to end by the peer thread. Our
						// lobby IS a LAN game on a virtual network, so hosting is the same two
						// calls the Direct Connect screen makes, and LANAPI::OnGameCreate pushes
						// the LAN game options screen from there.
						//
						// No SetLobbyAttemptHostJoin here either - see relayJoinSelectedGame.
						relayHostGame();
						break;
					}

					SetLobbyAttemptHostJoin( TRUE );
					TheLobbyQueuedUTMs.clear();
					groupRoomToJoin = TheGameSpyInfo->getCurrentGroupRoom();
					GameSpyOpenOverlay(GSOVERLAY_GAMEOPTIONS);
				}
				else if ( controlID == buttonJoinID )
				{
					if (s_tryingToHostOrJoin)
						break;

					if (s_relayLobby)
					{
						relayJoinSelectedGame();
						break;
					}

					TheLobbyQueuedUTMs.clear();
					// Look for a game to join
					groupRoomToJoin = TheGameSpyInfo->getCurrentGroupRoom();
					Int selected;
					GadgetListBoxGetSelected(GetGameListBox(), &selected);
					if (selected >= 0)
					{
						// GeneralsX @build BenderAI 12/02/2026 64-bit safe pointer cast
						Int selectedID = static_cast<Int>(reinterpret_cast<intptr_t>(GadgetListBoxGetItemData(GetGameListBox(), selected)));
						if (selectedID > 0)
						{
							StagingRoomMap *srm = TheGameSpyInfo->getStagingRoomList();
							StagingRoomMap::iterator srmIt = srm->find(selectedID);
							if (srmIt != srm->end())
							{
								GameSpyStagingRoom *roomToJoin = srmIt->second;
								if (!roomToJoin || roomToJoin->getExeCRC() != TheGlobalData->m_exeCRC || roomToJoin->getIniCRC() != TheGlobalData->m_iniCRC)
								{
									// bad crc.  don't go.
									DEBUG_LOG(("WOLLobbyMenuSystem - CRC mismatch with the game I'm trying to join. My CRC's - EXE:0x%08X INI:0x%08X  Their CRC's - EXE:0x%08x INI:0x%08x", TheGlobalData->m_exeCRC, TheGlobalData->m_iniCRC, roomToJoin->getExeCRC(), roomToJoin->getIniCRC()));
#if defined(RTS_DEBUG)
									if (TheGlobalData->m_netMinPlayers)
									{
										GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"), TheGameText->fetch("GUI:JoinFailedCRCMismatch"));
										break;
									}
									else if (g_fakeCRC)
									{
										TheWritableGlobalData->m_exeCRC = roomToJoin->getExeCRC();
										TheWritableGlobalData->m_iniCRC = roomToJoin->getIniCRC();
									}
#else
									GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"), TheGameText->fetch("GUI:JoinFailedCRCMismatch"));
									break;
#endif
								}
								Bool unknownLadder = (roomToJoin->getLadderPort() && TheLadderList->findLadder(roomToJoin->getLadderIP(), roomToJoin->getLadderPort()) == nullptr);
								if (unknownLadder)
								{
									GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"), TheGameText->fetch("GUI:JoinFailedUnknownLadder"));
									break;
								}
								if (roomToJoin->getNumPlayers() == MAX_SLOTS)
								{
									GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"), TheGameText->fetch("GUI:JoinFailedRoomFull"));
									break;
								}
								TheGameSpyInfo->markAsStagingRoomJoiner(selectedID);
								TheGameSpyGame->setGameName(roomToJoin->getGameName());
								TheGameSpyGame->setLadderIP(roomToJoin->getLadderIP());
								TheGameSpyGame->setLadderPort(roomToJoin->getLadderPort());
								SetLobbyAttemptHostJoin( TRUE );
								if (roomToJoin->getHasPassword())
								{
									GameSpyOpenOverlay(GSOVERLAY_GAMEPASSWORD);
								}
								else
								{
									// no password - just join it
									PeerRequest req;
									req.peerRequestType = PeerRequest::PEERREQUEST_JOINSTAGINGROOM;
									req.text = srmIt->second->getGameName().str();
									req.stagingRoom.id = selectedID;
									req.password = "";
									TheGameSpyPeerMessageQueue->addRequest(req);
								}
							}
						}
						else
						{
							GSMessageBoxOk(TheGameText->fetch("GUI:Error"), TheGameText->fetch("GUI:NoGameInfo"), nullptr);
						}
					}
					else
					{
						GSMessageBoxOk(TheGameText->fetch("GUI:Error"), TheGameText->fetch("GUI:NoGameSelected"), nullptr);
					}
				}
				else if ( controlID == buttonBuddyID )
				{
					// GeneralsX @feature Matchmaking. The buddy list is a GameSpy profile feature and
					// GameSpyOpenOverlay dereferences the buddy message queue we never created. The
					// button is disabled in Init; this is the belt to that pair of braces.
					if (s_relayLobby)
						break;

					GameSpyToggleOverlay( GSOVERLAY_BUDDY );
				}
				else if ( controlID == buttonGameListTypeToggleID )
				{
					ToggleGameListType();
				}
				else if ( controlID == buttonEmoteID )
				{
					// GeneralsX @feature Matchmaking. No chat channel exists on the relay, and
					// sendChat posts to the peer message queue. See the note in Init.
					if (s_relayLobby)
						break;

				// read the user's input and clear the entry box
					UnicodeString txtInput;
					txtInput.set(GadgetTextEntryGetText( textEntryChat ));
					GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
					txtInput.trim();
					if (!txtInput.isEmpty())
					{
						// Send the message
						TheGameSpyInfo->sendChat( txtInput, FALSE, listboxLobbyPlayers ); // 'emote' button now just sends text
					}
				}

				break;
			}

		//---------------------------------------------------------------------------------------------
		case GCM_SELECTED:
			{
				if (s_tryingToHostOrJoin)
					break;
				// GeneralsX @feature Matchmaking. The group-room dropdown is inert here: it names the
				// relay we are on, and there is nothing to switch to. joinGroupRoom/leaveGroupRoom
				// are both peerchat requests.
				if (s_relayLobby)
					break;
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				if( controlID == comboLobbyGroupRoomsID )
				{
					int rowSelected = -1;
					GadgetComboBoxGetSelectedPos(control, &rowSelected);

					DEBUG_LOG(("Row selected = %d", rowSelected));
					if (rowSelected >= 0)
					{
						Int groupID;
						// GeneralsX @build BenderAI 12/02/2026 64-bit safe pointer cast
						groupID = static_cast<Int>(reinterpret_cast<intptr_t>(GadgetComboBoxGetItemData(comboLobbyGroupRooms, rowSelected)));
						DEBUG_LOG(("ItemData was %d, current Group Room is %d", groupID, TheGameSpyInfo->getCurrentGroupRoom()));
						if (groupID && groupID != TheGameSpyInfo->getCurrentGroupRoom())
						{
							TheGameSpyInfo->leaveGroupRoom();
							TheGameSpyInfo->joinGroupRoom(groupID);

							if (TheGameSpyConfig->restrictGamesToLobby())
							{
								TheGameSpyInfo->clearStagingRoomList();
								RefreshGameListBoxes();
								PeerRequest req;
								req.peerRequestType = PeerRequest::PEERREQUEST_STARTGAMELIST;
								req.gameList.restrictGameList = TRUE;
								TheGameSpyPeerMessageQueue->addRequest(req);
							}
						}
					}
				}
			}
			break;

		//---------------------------------------------------------------------------------------------
		case GLM_DOUBLE_CLICKED:
			{
				if (buttonPushed)
					break;

				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				if (controlID == GetGameListBoxID())
				{
					int rowSelected = mData2;

					if (rowSelected >= 0)
					{
						GadgetListBoxSetSelected( control, rowSelected );
						GameWindow *button = TheWindowManager->winGetWindowFromId( window, buttonJoinID );

						TheWindowManager->winSendSystemMsg( window, GBM_SELECTED,
																								(WindowMsgData)button, buttonJoinID );
					}
				}
				break;
			}

		//---------------------------------------------------------------------------------------------
		case GLM_RIGHT_CLICKED:
			{
				// GeneralsX @feature Matchmaking. Every right-click menu on this screen acts on a
				// GameSpy profile - ignore, page, add buddy, view stats. There are no accounts on the
				// relay and no profile IDs, so all of them would open a menu whose every entry is a
				// no-op or a null queue write.
				if (s_relayLobby)
					break;

				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				if( controlID == listboxLobbyPlayersID )
				{
					RightClickStruct *rc = (RightClickStruct *)mData2;
					WindowLayout *rcLayout = nullptr;
					GameWindow *rcMenu;
					if(rc->pos < 0)
					{
						GadgetListBoxSetSelected(control, -1);
						break;
					}

					GPProfile profileID = 0;
					AsciiString aName;
					aName.translate(GadgetListBoxGetText(control, rc->pos, COLUMN_PLAYERNAME));
					PlayerInfoMap::iterator it = TheGameSpyInfo->getPlayerInfoMap()->find(aName);
					if (it != TheGameSpyInfo->getPlayerInfoMap()->end())
						profileID = it->second.m_profileID;

					Bool isBuddy = FALSE;
					if (profileID <= 0)
						rcLayout = TheWindowManager->winCreateLayout("Menus/RCNoProfileMenu.wnd");
					else
					{
						if (profileID == TheGameSpyInfo->getLocalProfileID())
						{
							rcLayout = TheWindowManager->winCreateLayout("Menus/RCLocalPlayerMenu.wnd");
						}
						else if(TheGameSpyInfo->isBuddy(profileID))
						{
							rcLayout = TheWindowManager->winCreateLayout("Menus/RCBuddiesMenu.wnd");
							isBuddy = TRUE;
						}
						else
							rcLayout = TheWindowManager->winCreateLayout("Menus/RCNonBuddiesMenu.wnd");
					}
					if(!rcLayout)
						break;

					GadgetListBoxSetSelected(control, rc->pos);

					rcMenu = rcLayout->getFirstWindow();
					rcMenu->winGetLayout()->runInit();
					rcMenu->winBringToTop();
					rcMenu->winHide(FALSE);
					setUnignoreText( rcLayout, aName, profileID);
					ICoord2D rcSize, rcPos;
					rcMenu->winGetSize(&rcSize.x, &rcSize.y);
					rcPos.x = rc->mouseX;
					rcPos.y = rc->mouseY;
					if(rc->mouseX + rcSize.x > TheDisplay->getWidth())
						rcPos.x = TheDisplay->getWidth() - rcSize.x;
					if(rc->mouseY + rcSize.y > TheDisplay->getHeight())
						rcPos.y = TheDisplay->getHeight() - rcSize.y;
					rcMenu->winSetPosition(rcPos.x, rcPos.y);

					GameSpyRCMenuData *rcData = NEW GameSpyRCMenuData;
					rcData->m_id = profileID;
					rcData->m_nick = aName;
					rcData->m_itemType = (isBuddy)?ITEM_BUDDY:ITEM_NONBUDDY;
					rcMenu->winSetUserData((void *)rcData);
					TheWindowManager->winSetLoneWindow(rcMenu);
				}
				else if( controlID == GetGameListBoxID() )
				{
					RightClickStruct *rc = (RightClickStruct *)mData2;
					WindowLayout *rcLayout = nullptr;
					GameWindow *rcMenu;
					if(rc->pos < 0)
					{
						GadgetListBoxSetSelected(control, -1);
						break;
					}

					// GeneralsX @build BenderAI 12/02/2026 64-bit safe pointer cast
					Int selectedID = static_cast<Int>(reinterpret_cast<intptr_t>(GadgetListBoxGetItemData(control, rc->pos)));
					if (selectedID > 0)
					{
						StagingRoomMap *srm = TheGameSpyInfo->getStagingRoomList();
						StagingRoomMap::iterator srmIt = srm->find(selectedID);
						if (srmIt != srm->end())
						{
							GameSpyStagingRoom *theRoom = srmIt->second;
							if (!theRoom)
								break;
							const LadderInfo *linfo = TheLadderList->findLadder(theRoom->getLadderIP(), theRoom->getLadderPort());
							if (linfo)
							{
								rcLayout = TheWindowManager->winCreateLayout("Menus/RCGameDetailsMenu.wnd");
								if (!rcLayout)
									break;

								GadgetListBoxSetSelected(control, rc->pos);

								rcMenu = rcLayout->getFirstWindow();
								rcMenu->winGetLayout()->runInit();
								rcMenu->winBringToTop();
								rcMenu->winHide(FALSE);
								rcMenu->winSetPosition(rc->mouseX, rc->mouseY);

								rcMenu->winSetUserData((void *)selectedID);
								TheWindowManager->winSetLoneWindow(rcMenu);
							}
						}
					}
				}
				break;
			}

//		//---------------------------------------------------------------------------------------------
//		case GSM_SLIDER_TRACK:
//		{
//				if (buttonPushed)
//					break;
//
//			GameWindow *control = (GameWindow *)mData1;
//			Int val = (Int)mData2;
//			Int controlID = control->winGetWindowId();
//			if (controlID == sliderChatAdjustID)
//			{
//				doSliderTrack(control, val);
//			}
//			break;
//		}

		//---------------------------------------------------------------------------------------------
		case GEM_EDIT_DONE:
			{
				if (buttonPushed)
					break;

				// GeneralsX @feature Matchmaking. The chat entry is disabled in relay mode, so this
				// should not arrive - but sendChat writes to the peer message queue and the slash
				// commands query the GameSpy QR2 hosting state, so neither may run without one.
				if (s_relayLobby)
					break;

				// read the user's input and clear the entry box
				UnicodeString txtInput;
				txtInput.set(GadgetTextEntryGetText( textEntryChat ));
				GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
				txtInput.trim();
				if (!txtInput.isEmpty())
				{
					// Send the message
					if (!handleLobbySlashCommands(txtInput))
					{
						TheGameSpyInfo->sendChat( txtInput, false, listboxLobbyPlayers );
					}
				}
				break;
			}

		//---------------------------------------------------------------------------------------------
		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;
}
