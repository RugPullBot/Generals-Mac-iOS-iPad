/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
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

// GeneralsX @feature Claude 28/07/2026 Headless LAN host/join driver. See HeadlessMatch.h.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/HeadlessMatch.h"

#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/PlayerTemplate.h"
#include "Common/PlayerList.h"
#include "Common/Player.h"
#include "Common/ThingTemplate.h"
#include "Common/Recorder.h"
#include "GameLogic/Object.h"
#include "GameClient/GameClient.h"
#include "GameClient/MapUtil.h"	// TheMapCache
#include "GameLogic/GameLogic.h"
#include "GameNetwork/GameInfo.h"
#include "GameNetwork/IPEnumeration.h"
#include "GameNetwork/LANAPI.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/LANGameInfo.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/NetworkInterface.h"	// TheNetwork, for the live desync flag
#include "GameNetwork/Transport.h"			// the relay game browser
#include "Common/UserPreferences.h"	// LANPreferences
#include "Common/OptionPreferences.h"	// relay settings, read from Options.ini
#include "GameNetwork/GUIUtil.h"			// EnableSlotListUpdates

// ------------------------------------------------------------------------------------------
// THE TRAP THAT DEFINES THIS FILE
//
// LANAPI::update() begins with `if (LANbuttonPushed) return;`, and LANAPI's own callbacks
// SET that flag: OnGameCreate (LANAPICallbacks.cpp:699) and OnGameJoin (:511) both do
// `LANbuttonPushed = true` and then `TheShell->push("Menus/LanGameOptionsMenu.wnd")`. The ONLY
// place that clears it again is LanLobbyMenuInit (LanLobbyMenu.cpp:474).
//
// So a headless driver that reuses stock LANAPI deadlocks the instant its game is created or
// joined: the pump is disabled for the rest of the process and nothing will ever re-enable it.
// The symptom is a host that appears to start fine and then never hears a single peer.
//
// Rather than clear the flag behind LANAPI's back - which would race the real menus if a UI
// build ever reached this path - the headless role gets its own subclass whose callbacks touch
// neither the flag nor TheShell. Everything that is UI-free (notably OnGameStart, which builds
// TheNetwork, parses the user list, posts MSG_NEW_GAME and seeds the RNG) still calls the base
// implementation, so the headless lobby and the real lobby converge on identical simulation
// state. That equality is the whole point: a headless peer must be indistinguishable from a
// human-driven one, or it cannot be used to prove determinism.
// ------------------------------------------------------------------------------------------

namespace
{

/// Set once OnGameStart has run, so the driver knows to leave the lobby loop.
Bool s_gameStarted = FALSE;
/// Set when the local create/join succeeded.
Bool s_lobbyReady = FALSE;
/// Set when the create/join failed; the driver aborts rather than spinning to a timeout.
Bool s_lobbyFailed = FALSE;
AsciiString s_lobbyFailureReason;

void headlessLog(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "[GXLAN] ");
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
	va_end(args);
	fflush(stderr);
}

class HeadlessLANAPI : public LANAPI
{
public:

	// --- Callbacks that stock LANAPI implements by driving the shell ------------------------

	/// LANAPI hardcodes m_actionTimeout to 5000 ms, which is a LAN figure. A direct-connect join
	/// across the internet needs several exchanges - Request GameInfo, Game Announce, Request
	/// Join, Join Accept - each gated by LANAPI::update()'s own 200 ms throttle, so on a WAN link
	/// the Join Accept routinely arrives AFTER the pending action has already expired into
	/// RET_TIMEOUT. The packets are fine; the deadline is not.
	///
	/// The failure is deeply misleading: the host logs "player joined slot 1" and the joiner logs
	/// receiving Join Accept, yet the joiner still reports "join failed". Nothing looks dropped
	/// because nothing was dropped.
	void setActionTimeout(UnsignedInt ms) { m_actionTimeout = ms; }

	virtual void OnGameCreate( ReturnType ret ) override
	{
		if (ret == RET_OK)
		{
			headlessLog("hosted game created");
			s_lobbyReady = TRUE;
			// Stock OnGameCreate does this after pushing the menu. It is not cosmetic: staying
			// registered in the lobby keeps us answering lobby traffic we no longer belong to.
			RequestLobbyLeave( false );
		}
		else
		{
			s_lobbyFailed = TRUE;
			s_lobbyFailureReason.format("game create failed, code %d", (Int)ret);
			headlessLog("%s", s_lobbyFailureReason.str());
		}
	}

	virtual void OnGameJoin( ReturnType ret, LANGameInfo *theGame ) override
	{
		if (ret == RET_OK)
		{
			headlessLog("joined game");
			s_lobbyReady = TRUE;

			// Mirror the option burst stock OnGameJoin sends. The host's slot list is built
			// from these, so omitting them leaves the joiner without a faction or colour and
			// the two peers disagree about the player set before the match even starts.
			LANPreferences pref;
			AsciiString options;
			options.format("PlayerTemplate=%d", pref.getPreferredFaction());
			RequestGameOptions(options, true);
			options.format("Color=%d", pref.getPreferredColor());
			RequestGameOptions(options, true);
			options.format("NAT=%d", FirewallHelperClass::FIREWALL_TYPE_SIMPLE);
			RequestGameOptions(options, true);
		}
		else
		{
			s_lobbyFailed = TRUE;
			// RET_CRC_MISMATCH here is a SimID mismatch, i.e. the two builds are not the same
			// build. That is by far the most common headless join failure and it is worth
			// naming explicitly, because "join failed" sends people hunting for a network fault.
			if (ret == RET_CRC_MISMATCH)
				s_lobbyFailureReason = "join refused: build identity mismatch (SimID) - rebuild both peers from the same commit";
			else
				s_lobbyFailureReason.format("join failed, code %d", (Int)ret);
			headlessLog("%s", s_lobbyFailureReason.str());
		}
	}

	virtual void OnGameStart() override
	{
		// Base implementation is UI-free and does all the work that matters: creates
		// TheNetwork, parses the user list, sets m_pendingFile, posts MSG_NEW_GAME and calls
		// InitRandom with the shared seed. Do NOT reimplement it - the seeding in particular
		// is what keeps the peers in lockstep.
		LANAPI::OnGameStart();

		// The base implementation has a failure path that returns without starting anything:
		// if the map is missing or DoAnyMapTransfers fails it calls OnPlayerLeave, tears down
		// TheNetwork, clears m_currentGame and returns. Reporting "started" there sends the
		// driver into the match loop with no game at all, which segfaults on the first frame.
		// m_currentGame going null is the signal, so check it rather than assuming success.
		if (GetMyGame() == nullptr)
		{
			s_lobbyFailed = TRUE;
			s_lobbyFailureReason = "game did not start - the map is missing or could not be transferred";
			headlessLog("%s", s_lobbyFailureReason.str());
			return;
		}

		s_gameStarted = TRUE;
		headlessLog("game starting");
	}

	// --- Callbacks that only paint the lobby: reduce to logging ----------------------------

	virtual void OnGameList( LANGameInfo * ) override {}
	virtual void OnPlayerList( LANPlayer * ) override {}
	virtual void OnNameChange( UnsignedInt, UnicodeString ) override {}
	virtual void OnGameStartTimer( Int ) override {}

	// CAUTION: the three callbacks below are NOT UI. LANAPI mixes lobby state mutation into its
	// On* callbacks, so overriding them without delegating silently breaks the host's
	// bookkeeping. That is not a theoretical risk - log-only versions of OnAccept and OnHasMap
	// made the host ignore a peer that had joined, had the map and had accepted, and it then
	// waited out its full timeout. Only override what touches TheShell or LANbuttonPushed.

	virtual void OnPlayerJoin( Int slot, UnicodeString playerName ) override
	{
		headlessLog("player joined slot %d", slot);
		// Base calls resetAccepted() and re-broadcasts the options. Skipping it leaves peers
		// holding a stale slot list.
		LANAPI::OnPlayerJoin(slot, playerName);
	}

	virtual void OnPlayerLeave( UnicodeString player ) override
	{
		headlessLog("a player left");
		// Delegate ONLY when the leaver is not us. The base's "we are leaving" branch sets
		// LANbuttonPushed and pops the shell; its other branch forces the slot-list resend the
		// host needs when a peer drops.
		if (GetMyName().compare(player) != 0)
			LANAPI::OnPlayerLeave(player);
	}

	virtual void OnHostLeave() override
	{
		headlessLog("host left");
		s_lobbyFailed = TRUE;
		s_lobbyFailureReason = "host left the game";
	}

	virtual void OnAccept( UnsignedInt playerIP, Bool status ) override
	{
		headlessLog("accept from %d.%d.%d.%d = %d", PRINTF_IP_AS_4_INTS(playerIP), (Int)status);
		// This is where setAccept()/unAccept() actually happens - handleSetAccept only calls
		// through to here. Without this the host never registers that anyone is ready.
		LANAPI::OnAccept(playerIP, status);
	}

	virtual void OnHasMap( UnsignedInt playerIP, Bool status ) override
	{
		if (!status)
			headlessLog("peer %d.%d.%d.%d does NOT have the map", PRINTF_IP_AS_4_INTS(playerIP));
		// This is where setMapAvailability() happens.
		LANAPI::OnHasMap(playerIP, status);
	}

	virtual void OnChat( UnicodeString player, UnsignedInt ip,
											 UnicodeString message, ChatType format ) override
	{
		// System messages carry the useful diagnostics (map transfer failures, host timeouts).
		AsciiString narrow;
		narrow.translate(message);
		headlessLog("chat: %s", narrow.str());
	}

	virtual void OnGameOptions( UnsignedInt playerIP, Int playerSlot, AsciiString options ) override
	{
		// The base implementation parses the slot list, which the joiner genuinely needs; its
		// only UI calls are lanUpdateSlotList()/updateGameOptions(), and both bail immediately
		// when slot list updates are disabled. setUpLan() disables them for the whole headless
		// run, so this is safe and we keep the real parsing rather than duplicating it.
		LANAPI::OnGameOptions(playerIP, playerSlot, options);
	}
};

// ------------------------------------------------------------------------------------------
// GeneralsX @feature Matchmaking. Which relay room this process belongs in.
//
// A host gets a room of its own (Transport::getLocalHostRoom), because the relay lists exactly one
// game per room and two hosts sharing one would overwrite each other's listing. A joiner has to
// end up in the room of the game it wants, which it does not know from -lanjoin alone: addresses
// are allocated per room, so every host on the relay is 10.42.0.1 and the address identifies
// nothing on its own.
//
// So the joiner browses first, exactly as the UI's game list does, and takes the room off the
// listing whose host address matches. When that is ambiguous or the game is not listed at all -
// a private game never advertises - the room has to be given explicitly:
//
//   GENERALSX_LANROOM=<token>    environment, highest precedence, either role
//   RelayRoom=<token>            Options.ini, the existing explicit setting
//
// The pin is an environment variable rather than a -lanroom flag ONLY because adding a flag means
// editing CommandLine.cpp and GlobalData, which are outside the file set this change is allowed to
// touch. It is read at exactly one place and behaves like a flag in every other respect.
// ------------------------------------------------------------------------------------------

/// An explicitly pinned room, or empty. Environment first, then Options.ini.
AsciiString pinnedRelayRoom()
{
	const char *env = getenv("GENERALSX_LANROOM");
	if (env != nullptr && *env != '\0')
		return AsciiString(env);

	OptionPreferences prefs;
	return prefs.getRelayRoom();
}

/// The room to enter, or empty if a joiner could not work out which game was meant.
AsciiString chooseRelayRoom(const AsciiString &relayHost)
{
	const AsciiString pinned = pinnedRelayRoom();
	if (!pinned.isEmpty())
	{
		headlessLog("relay room pinned to '%s'", pinned.str());
		return pinned;
	}

	if (TheGlobalData->m_lanRole != LANROLE_JOIN)
	{
		// Hosting, or -lanlist, which needs a room only so the relay has somewhere to put it.
		return Transport::getLocalHostRoom();
	}

	// Browsing, over a throwaway socket, before any transport exists - see the note on
	// Transport::requestRelayGameList. Retried because a host publishes its advertisement on its
	// 5 s registration keepalive, so a joiner that starts alongside its host can easily ask before
	// the first one has gone out.
	const UnsignedInt target = TheGlobalData->m_lanJoinIP;
	for (Int attempt = 0; attempt < 4; ++attempt)
	{
		Transport::RelayGameListing games[Transport::MAX_RELAY_LISTINGS];
		const Int count = Transport::requestRelayGameList(relayHost.str(), games,
			Transport::MAX_RELAY_LISTINGS, 2000);

		Int matches = 0;
		Int firstMatch = -1;
		for (Int i = 0; i < count; ++i)
		{
			if (games[i].hostVirtualIP != target)
				continue;
			if (firstMatch < 0)
				firstMatch = i;
			++matches;
		}

		if (matches == 1)
		{
			headlessLog("game at %d.%d.%d.%d is in room '%s' (\"%s\" on %s)",
				PRINTF_IP_AS_4_INTS(target), games[firstMatch].room,
				games[firstMatch].name, games[firstMatch].map);
			return AsciiString(games[firstMatch].room);
		}

		if (matches > 1)
		{
			// Not recoverable by waiting: the address genuinely does not distinguish them.
			headlessLog("%d listed games have host %d.%d.%d.%d - set GENERALSX_LANROOM to pick one:",
				matches, PRINTF_IP_AS_4_INTS(target));
			for (Int i = 0; i < count; ++i)
			{
				if (games[i].hostVirtualIP == target)
					headlessLog("  room=%s name=\"%s\" map=\"%s\"",
						games[i].room, games[i].name, games[i].map);
			}
			return AsciiString();
		}

		headlessLog("no listed game at %d.%d.%d.%d yet (%d listed); retrying",
			PRINTF_IP_AS_4_INTS(target), count);
	}

	headlessLog("nothing on the relay advertises %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(target));
	headlessLog("a private game never advertises - join it with GENERALSX_LANROOM=<room>");
	return AsciiString();
}

/// Throttle for the joiner's re-accept, so the 10 ms pump does not flood the lobby.
UnsignedInt s_lastAcceptSent = 0;

/// Is OUR OWN slot currently marked accepted?
Bool localSlotAccepted()
{
	LANGameInfo *game = TheLAN ? TheLAN->GetMyGame() : nullptr;
	if (!game)
		return TRUE;	// not in a game yet - nothing to assert

	const Int slotNum = game->getLocalSlotNum();
	if (slotNum < 0)
		return TRUE;	// we are not in the slot list yet

	GameSlot *slot = game->getSlot(slotNum);
	return (slot == nullptr) || slot->isAccepted();
}

/// Re-assert our accept if the host has cleared it.
///
/// LANAPI::OnPlayerJoin calls resetAccepted() and re-broadcasts the slot list, on EVERY join. So a
/// peer that accepted once is silently un-accepted the moment the NEXT peer arrives, and the host
/// then waits out its entire timeout for a player that believes it is ready. In a UI lobby a human
/// sees the tick vanish and clicks again; a headless peer accepted once and never looked back.
///
/// This could not show up with a single joiner - nothing ever followed its accept - which is why
/// it survived until a second one joined. Joiners do not even receive OnPlayerJoin, so this is
/// driven off our own slot state rather than off an event: whatever clears the flag, we notice.
void reassertAcceptIfNeeded()
{
	if (localSlotAccepted())
		return;

	const UnsignedInt now = timeGetTime();
	if ((now - s_lastAcceptSent) < 250)
		return;

	s_lastAcceptSent = now;
	TheLAN->RequestAccept();
}

/// How many peers the host waits for, and how many are currently accepted.
Int countAcceptedPeers()
{
	LANGameInfo *game = TheLAN ? TheLAN->GetMyGame() : nullptr;
	if (!game)
		return 0;

	Int accepted = 0;
	for (Int i = 1; i < MAX_SLOTS; ++i)
	{
		GameSlot *slot = game->getSlot(i);
		if (slot && slot->isHuman() && slot->isAccepted())
			++accepted;
	}
	return accepted;
}

/// GeneralsX @feature Claude 29/07/2026 How many slots will become real players: occupied and not
/// an observer. This is deliberately the same loop the LAN lobby UI runs before it lets the host
/// press Start (LanGameOptionsMenu.cpp StartPressed) - the headless driver and the GUI are supposed
/// to accept and refuse exactly the same lobbies, and until now only the GUI checked.
///
/// Note isOccupied() is FALSE for SLOT_OPEN, so this counts a peer only once it has actually
/// joined. Calling it before the peers arrive counts intent, not reality; see publishGameOptions.
Int countOccupiedNonObserverSlots(GameInfo *game)
{
	if (!game)
		return 0;

	Int occupied = 0;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		GameSlot *slot = game->getSlot(i);
		if (slot && slot->isOccupied() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
			++occupied;
	}
	return occupied;
}

/// GeneralsX @feature Claude 29/07/2026 Refuse a lobby holding more players than the map has start
/// positions. This is a GUARD ONLY: it never reassigns or clamps anything, because start positions
/// decide where every player's opening units are placed and are therefore part of the frame-0 world
/// CRC. Moving them would invalidate every .crc baseline in docs/WORKDIR/evidence/ and, since this
/// engine defaults RETAIL_COMPATIBLE_CRC to 1, retail 1.04 replay compatibility with it.
///
/// What it prevents: populateRandomStartPosition caps positions at md->m_numPlayers
/// (GameLogic.cpp:882-885) and pre-marks every index past it as taken (:928). A surplus player
/// therefore finds no free position, keeps startPos=-1, gets no Command Center, and its skirmish AI
/// returns early from adjustBuildList and owns nothing for the whole match - while the run still
/// reports the full AI count and passes. Every "8-player" result in this project before this guard
/// was an overfilled 2-player map.
///
/// Fails closed when the map is not in the cache, matching the GUI. Every hosting harness in
/// scripts/ passes -lanmap, which is pre-flighted against the cache earlier in publishGameOptions.
Bool lobbyFitsTheMap(GameInfo *game, Int occupied, const char *when)
{
	if (!game)
		return FALSE;

	const MapMetaData *md = TheMapCache->findMap(game->getMap());
	if (md == nullptr)
	{
		headlessLog("capacity check (%s): map \"%s\" is not in the cache, so the lobby cannot be bounded - refusing",
			when, game->getMap().str());
		return FALSE;
	}

	if (occupied <= md->m_numPlayers)
		return TRUE;

	if (TheGlobalData->m_lanOverfill)
	{
		headlessLog("WARNING: -lanoverfill: \"%s\" holds %d player(s) but this lobby has %d occupied slot(s) (%s).",
			game->getMap().str(), md->m_numPlayers, occupied, when);
		headlessLog("WARNING: %d player(s) will get startPos=-1, no Command Center, and will own nothing all match.",
			occupied - md->m_numPlayers);
		headlessLog("WARNING: any AI count this run reports is a LIE. Do not use it as simulation evidence.");
		return TRUE;
	}

	headlessLog("REFUSING over-capacity lobby (%s): \"%s\" holds %d player(s), this lobby has %d occupied slot(s).",
		when, game->getMap().str(), md->m_numPlayers, occupied);
	headlessLog("  The %d surplus player(s) would get startPos=-1, no Command Center, and their AI would own",
		occupied - md->m_numPlayers);
	headlessLog("  nothing for the whole match while this run still reported the full AI count and passed.");
	headlessLog("  Use a higher-capacity map (death valley, destruction station and twilight flame hold 8),");
	headlessLog("  or lower -lanai/-lanwait, or pass -lanoverfill to reproduce the old behaviour deliberately.");
	return FALSE;
}

} // namespace

// ------------------------------------------------------------------------------------------

Bool HeadlessMatch::isRequested()
{
	// -lanlist has no host/join role but still has to enter this driver rather than the shell.
	// Without it the browse flags parse fine, the engine boots the normal UI, and the process just
	// sits there - which reads as a hang rather than as "that flag did nothing".
	return (TheGlobalData->m_lanRole != LANROLE_NONE) || TheGlobalData->m_lanListGames;
}

template <typename Predicate>
Bool HeadlessMatch::pumpUntil(Predicate predicate, UnsignedInt timeoutMs, const char *what)
{
	const UnsignedInt deadline = timeGetTime() + timeoutMs;
	while (timeGetTime() < deadline)
	{
		if (s_lobbyFailed)
			return FALSE;
		if (predicate())
			return TRUE;

		// Nothing in a headless process pumps TheLAN. In a UI build this call lives in
		// LanLobbyMenuUpdate; GameEngine::update only pumps it while alt-tabbed out.
		TheLAN->update();

		// LANAPI::update self-throttles to one pass per 200ms, so a tight spin would burn a
		// core to no purpose.
		Sleep(10);
	}
	headlessLog("timed out waiting for %s after %d ms", what, (Int)timeoutMs);
	return FALSE;
}

Bool HeadlessMatch::setUpLan()
{
	// Every UI helper that LANAPI's surviving callbacks reach is gated on this. Turning it off
	// once, for the whole run, is what lets OnGameOptions keep using the real parsing path.
	EnableSlotListUpdates(FALSE);

	if (TheLAN != nullptr)
	{
		delete TheLAN;
		TheLAN = nullptr;
	}
	TheLAN = NEW HeadlessLANAPI();

	// GeneralsX @feature Relay transport. With a relay configured our identity is the VIRTUAL LAN
	// address, not a NIC address: the lobby is a two-machine LAN whose members are 10.42.0.1 and
	// 10.42.0.2 wherever the machines physically are. NetworkDirectConnectInit does exactly this
	// and skips the interface enumeration for the same reason - the virtual address is on no
	// interface, so anything that "corrects" it back to a real one puts a NAT-side address into
	// the slot list, and the peer then spends the whole match trying to reach an IP that was
	// never routable from where it is.
	//
	// Binding is already handled: Transport::init calls initRelay() and forces INADDR_ANY when
	// relay mode is on, precisely because a virtual address can never be bound.
	OptionPreferences prefs;
	const AsciiString relayHost = prefs.getRelayAddress();

	UnsignedInt ip = 0;
	AsciiString room;
	if (!relayHost.isEmpty())
	{
		// Which room, and for a joiner that means browsing first - see chooseRelayRoom.
		room = chooseRelayRoom(relayHost);
		if (room.isEmpty())
		{
			headlessLog("cannot tell which relay room to join - aborting rather than guessing");
			return FALSE;
		}

		// Ask the relay who we are before anything snapshots an identity. A relay-allocated
		// address is what lets strangers play: hand-configured ones have to be unique by human
		// agreement, and everyone who takes the default is 10.42.0.1. The identity is pinned
		// TOGETHER with the room it was allocated in, because it means nothing anywhere else.
		UnsignedInt assignedIP = 0;
		if (Transport::enterRelayRoom(relayHost.str(), room.str(), assignedIP))
		{
			ip = assignedIP;
			headlessLog("relay assigned us %d.%d.%d.%d in room %s",
				PRINTF_IP_AS_4_INTS(ip), room.str());
		}
		else
		{
			// Falling back keeps LAN play and existing hand-configured setups working. It is
			// logged loudly because two peers that both fall back to the default address will
			// collide, and that presents as a lobby that corrupts rather than one that refuses.
			ip = prefs.getLocalVirtualIP();
			if (ip != 0)
			{
				// Still pin the ROOM. Without this initRelay falls back to the room we would host
				// in, which for a joiner is not the room the game is in - so an unanswered GXWHO
				// would quietly turn a join into a game of one.
				Transport::setAssignedIdentity(room.str(), ip);
			}
			headlessLog("relay did not assign an identity; falling back to configured LocalVirtualIP %d.%d.%d.%d",
				PRINTF_IP_AS_4_INTS(ip));
		}
	}

	if (ip != 0)
	{
		headlessLog("relay mode: local identity is the virtual address %d.%d.%d.%d (relay %s, room %s)",
			PRINTF_IP_AS_4_INTS(ip), relayHost.str(), room.str());
	}
	else
	{
		// Same interface election the lobby does. LANIPAddress wins if it was set, which is the
		// escape hatch on a multi-homed box; otherwise take the first enumerated address.
		ip = TheGlobalData->m_defaultIP;
		if (!ip)
		{
			IPEnumeration ips;
			EnumeratedIP *list = ips.getAddresses();
			if (!list)
			{
				headlessLog("no local IP addresses - cannot host or join");
				return FALSE;
			}
			ip = list->getIP();
		}
	}

	// Raise the pending-action deadline before init so a WAN join is not abandoned mid-handshake.
	// LAN play is unaffected: a local join completes in well under the stock 5 s, so a larger
	// ceiling only changes how long a genuinely unreachable peer takes to give up - and -lanjoin
	// is bounded by -lantimeout anyway.
	{
		UnsignedInt actionMs = TheGlobalData->m_lanTimeoutMs;
		if (actionMs > 60000) actionMs = 60000;	// no point waiting longer than a minute
		if (actionMs < 5000)  actionMs = 5000;	// never tighter than the stock LAN value
		static_cast<HeadlessLANAPI*>(TheLAN)->setActionTimeout(actionMs);
		headlessLog("join action timeout %u ms", actionMs);
	}

	TheLAN->init();
	if (TheLAN->SetLocalIP(ip) == FALSE)
	{
		headlessLog("failed to bind local IP %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(ip));
		return FALSE;
	}
	headlessLog("local IP %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(ip));

	UnicodeString name = TheGlobalData->m_lanPlayerName;
	if (name.isEmpty())
	{
		LANPreferences prefs;
		name = prefs.getUserName();
	}
	name.truncateTo(g_lanPlayerNameLength);
	TheLAN->RequestSetName(name);

	return TRUE;
}

Bool HeadlessMatch::publishGameOptions()
{
	LANGameInfo *game = TheLAN->GetMyGame();
	if (!game)
	{
		headlessLog("no game to configure");
		return FALSE;
	}

	if (!TheGlobalData->m_lanMap.isEmpty())
	{
		// Pre-flight the map here rather than letting OnGameStart discover it. Down there the
		// only symptom is the chat line "Unable to transfer the map", which points at the
		// network and not at the actual cause - a name that is not in the map cache. Note the
		// name wanted is the full path INCLUDING the filename
		// ("maps/twilight flame/twilight flame.map"); the M= field on the wire carries only the
		// directory, because GameInfoToAsciiString strips the last component.
		TheMapCache->updateCache();
		const MapMetaData *mapData = TheMapCache->findMap(TheGlobalData->m_lanMap);
		if (mapData == nullptr)
		{
			headlessLog("map \"%s\" is not in the map cache", TheGlobalData->m_lanMap.str());
			headlessLog("expected a full path including the filename, e.g. \"maps/twilight flame/twilight flame.map\"");
			Int shown = 0;
			for (MapCache::const_iterator it = TheMapCache->begin();
					it != TheMapCache->end() && shown < 15; ++it, ++shown)
			{
				headlessLog("  available: %s", it->first.str());
			}
			return FALSE;
		}
		game->setMap(TheGlobalData->m_lanMap);

		// The CRC and size are NOT cosmetic and nothing else fills them in - the lobby UI does
		// it when a map is picked. A joiner decides whether it has the map by comparing the
		// host's advertised CRC against its own cache entry (GameInfo::setMapCRC), so a host
		// that advertises 0 makes every peer conclude it is missing a map it actually has. The
		// peer then sits at "You do not have the map" and never becomes ready, while the host
		// only sees a peer that joined and never accepted.
		game->setMapCRC(mapData->m_CRC);
		game->setMapSize(mapData->m_filesize);

		headlessLog("map set to %s (crc=%08X size=%u)",
			TheGlobalData->m_lanMap.str(), mapData->m_CRC, mapData->m_filesize);
	}

	// Open the slots we are holding for human peers. This is NOT optional and nothing else does
	// it: GameSlot::reset defaults every slot to SLOT_CLOSED, and it is the lobby UI that opens
	// them in a normal game. handleRequestJoin accepts a peer only if some slot isOpen(), so a
	// headless host that skips this denies every join with RET_GAME_FULL while looking, from the
	// host side, like a peer that simply never arrived.
	for (Int i = 1; i <= TheGlobalData->m_lanWaitPeers && i < MAX_SLOTS; ++i)
	{
		LANGameSlot openSlot;
		openSlot.setState(SLOT_OPEN);
		game->setSlot(i, openSlot);
	}

	// Fill slots with AI, starting after the host and after any human slots we are holding
	// open for -lanwait peers.
	Int nextSlot = 1 + TheGlobalData->m_lanWaitPeers;
	for (size_t i = 0; i < TheGlobalData->m_lanAiSpecs.size(); ++i)
	{
		const LanAiSpec &spec = TheGlobalData->m_lanAiSpecs[i];
		SlotState state = SLOT_MED_AI;
		switch (spec.difficulty)
		{
			case 'E': state = SLOT_EASY_AI;   break;
			case 'M': state = SLOT_MED_AI;    break;
			case 'H': state = SLOT_BRUTAL_AI; break;
			default: break;
		}

		for (Int n = 0; n < spec.count; ++n)
		{
			if (nextSlot >= MAX_SLOTS)
			{
				headlessLog("ran out of slots placing AI - wanted more than %d", MAX_SLOTS);
				return FALSE;
			}
			// LANGameInfo::setSlot takes a LANGameSlot, not the base GameSlot.
			LANGameSlot aiSlot;
			aiSlot.setState(state);
			game->setSlot(nextSlot, aiSlot);
			++nextSlot;
		}
	}

	// GeneralsX @feature Claude 29/07/2026 Capacity pre-check on INTENT, before any peer connects.
	// nextSlot is exactly the intended final occupancy: it starts at 1 + m_lanWaitPeers (the host
	// plus the human slots held open) and advances once per AI placed. The slots reserved for peers
	// are still SLOT_OPEN here, so isOccupied() is FALSE for them and counting the slot list would
	// undercount by -lanwait - hence the arithmetic rather than countOccupiedNonObserverSlots().
	//
	// This is the cheap half of the guard: a misconfigured harness dies here in about a second,
	// instead of after a 60 s wait for peers that are then dropped. The authoritative check runs on
	// the real slot list just before RequestGameStart.
	if (!lobbyFitsTheMap(game, nextSlot, "intended lobby"))
		return FALSE;

	// GXDRAWDBG - TEMPORARY. The match loop stops as soon as the LOCAL player is defeated, which on
	// a cramped map is long before the AI have destroyed each other's bases - exactly the events a
	// structure-rendering investigation needs. Allying the host with the first AI keeps the team
	// alive after the host is wiped out, so the run continues to its frame limit. REMOVE when done.
	if (getenv("GX_ALLYHOST") != nullptr)
	{
		LANGameSlot host = *game->getLANSlot(0);
		host.setTeamNumber(0);
		game->setSlot(0, host);
		if (nextSlot > 1)
		{
			LANGameSlot ally = *game->getLANSlot(1);
			ally.setTeamNumber(0);
			game->setSlot(1, ally);
		}
		headlessLog("GX_ALLYHOST: host and slot 1 allied on team 0");
	}

	// -lanseed pins the match seed. RequestGameCreate seeds from GetTickCount(), so two runs
	// never share a starting state and cannot be compared. With a fixed seed, two SOLO headless
	// runs on different platforms produce directly diffable [GXCRC] streams with no network
	// involved at all - which is the only way to separate a simulation divergence from a
	// transport problem.
	if (TheGlobalData->m_lanSeed != 0)
	{
		game->setSeed(TheGlobalData->m_lanSeed);
		headlessLog("seed forced to %d", TheGlobalData->m_lanSeed);
	}

	// Log the serialised slot list. Without this there is no way to confirm from the logs that
	// -lanai actually placed an AI, and a soak that silently ran human-only would "prove" a fix
	// to the AI-only desync class while never exercising it - skirmish scripts attach ONLY for AI
	// players. AI slots appear as CE/CM/CH in the S= field (CH = Computer Hard = SLOT_BRUTAL_AI).
	const AsciiString opts = GameInfoToAsciiString(game);
	headlessLog("game options: %s", opts.str());

	TheLAN->RequestGameOptions(opts, true);
	return TRUE;
}

Bool HeadlessMatch::driveLobby()
{
	const UnsignedInt lobbyTimeout = TheGlobalData->m_lanTimeoutMs;

	if (TheGlobalData->m_lanRole == LANROLE_HOST)
	{
		// isDirectConnect=TRUE matters for more than discovery: RequestGameCreate's
		// "can't create a game while in one" guard is skipped on the direct-connect path,
		// and it is a direct-connect game we want anyway so peers can reach us by IP.
		TheLAN->RequestGameCreate(TheGlobalData->m_lanGameName, TRUE);
		if (!pumpUntil([]{ return s_lobbyReady != FALSE; }, lobbyTimeout, "game create"))
			return FALSE;

		if (!publishGameOptions())
			return FALSE;

		const Int wanted = TheGlobalData->m_lanWaitPeers;
		if (wanted > 0)
		{
			headlessLog("waiting for %d peer(s) to accept", wanted);
			if (!pumpUntil([wanted]{ return countAcceptedPeers() >= wanted; },
					lobbyTimeout, "peers to accept"))
				return FALSE;
		}

		// GeneralsX @feature Claude 29/07/2026 The authoritative capacity check, on the slot list as
		// it actually stands. Every accepted peer is SLOT_PLAYER by now, so this counts reality
		// rather than intent and is the exact analogue of the GUI refusing to start
		// (LanGameOptionsMenu.cpp:249-258). It is the last gate before the engine assigns start
		// positions in populateRandomStartPosition.
		if (!lobbyFitsTheMap(TheLAN->GetMyGame(), countOccupiedNonObserverSlots(TheLAN->GetMyGame()),
				"final slot list"))
			return FALSE;

		headlessLog("starting match");
		TheLAN->RequestGameStart();
	}
	else
	{
		headlessLog("joining %d.%d.%d.%d", PRINTF_IP_AS_4_INTS(TheGlobalData->m_lanJoinIP));
		TheLAN->RequestGameJoinDirectConnect(TheGlobalData->m_lanJoinIP);
		if (!pumpUntil([]{ return s_lobbyReady != FALSE; }, lobbyTimeout, "game join"))
			return FALSE;

		TheLAN->RequestHasMap();
		TheLAN->RequestAccept();
		s_lastAcceptSent = timeGetTime();
	}

	// Both roles land here. The host reaches OnGameStart through its own RequestGameStart
	// round trip, so it waits too rather than assuming.
	//
	// A joiner keeps re-asserting its accept while it waits - see reassertAcceptIfNeeded. Without
	// it, any lobby with more than one joiner deadlocks: the second join resets the first peer's
	// accept, the host never counts two, and every peer sits there believing it is ready.
	const Bool isJoiner = (TheGlobalData->m_lanRole != LANROLE_HOST);
	if (!pumpUntil([isJoiner]{
			if (isJoiner)
				reassertAcceptIfNeeded();
			return s_gameStarted != FALSE;
		}, lobbyTimeout, "game start"))
		return FALSE;

	return TRUE;
}

Bool HeadlessMatch::runMatch()
{
	const Int frameLimit = TheGlobalData->m_lanFrameLimit;
	Bool sawMismatch = FALSE;

	// OnGameStart only POSTS MSG_NEW_GAME; TheGameLogic does not report isInGame() until that
	// message has been propagated and handled. So "started" and "in game" are not the same
	// instant, and !isInGame() cannot be used as the game-over test until the game has actually
	// been entered once.
	//
	// Getting this wrong is a race that hides on fast paths: macOS and Windows both happened to
	// process the message inside the first update() and ran fine for thousands of frames, while
	// Linux did not and exited having simulated 0 frames - reporting success. A third platform
	// is what exposed it.
	Bool everInGame = FALSE;

	// Track the high-water mark as we go. TheGameLogic's frame counter is RESET when the game is
	// torn down, so reading it after the loop reports the post-teardown value: a run that
	// simulated 370 frames logged "simulated 1 frames" while its own [GXCRC] output ran to
	// f=370. The log has to agree with the evidence beside it.
	UnsignedInt maxFrame = 0;

	// Reuse GameEngine::update rather than open-coding the subsystem order. That ordering
	// (client, message stream, network, then logic, all inside VERIFY_CRC) is load-bearing
	// for determinism, and a second copy of it here would drift from the real one.
	// GXDRAWDBG - TEMPORARY deterministic forcing function for the destroyed-structure rendering
	// investigation: capture and then kill every instance of a named template at fixed frames, so
	// the POSTMORTEM audit has something to look at without waiting on the AI. REMOVE when done.
	const char *killTmpl = getenv("GX_KILLTMPL");
	const char *killFrameEnv = getenv("GX_KILLFRAME");
	const UnsignedInt killFrame = (killFrameEnv != nullptr) ? (UnsignedInt)atoi(killFrameEnv) : 400;
	const Bool wantCapture = (getenv("GX_CAPTURE") != nullptr);
	Bool didCapture = FALSE;
	Bool didKill = FALSE;

	while (!TheGameEngine->getQuitting())
	{
		TheGameEngine->update();

		if (killTmpl != nullptr && TheGameLogic->isInGame())
		{
			const UnsignedInt now = TheGameLogic->getFrame();
			if (wantCapture && !didCapture && now >= killFrame / 2)
			{
				Player *owner = nullptr;
				for (Int p = 0; p < ThePlayerList->getPlayerCount(); ++p)
				{
					Player *cand = ThePlayerList->getNthPlayer(p);
					if (cand != nullptr && cand->isPlayableSide())
					{
						owner = cand;
						break;
					}
				}
				for (Object *o = TheGameLogic->getFirstObject(); o != nullptr; o = o->getNextObject())
				{
					const Bool capMatches = (strcmp(killTmpl, "*STRUCTURE*") == 0)
						? o->isKindOf(KINDOF_STRUCTURE)
						: (strcmp(o->getTemplate()->getName().str(), killTmpl) == 0);
					if (owner != nullptr && capMatches)
					{
						o->setTeam(owner->getDefaultTeam());
						headlessLog("GX_KILLTMPL: captured %s id=%d for player %d", killTmpl, (Int)o->getID(), (Int)owner->getPlayerIndex());
					}
				}
				didCapture = TRUE;
			}
			if (!didKill && now >= killFrame)
			{
				for (Object *o = TheGameLogic->getFirstObject(); o != nullptr; o = o->getNextObject())
				{
					const Bool matches = (strcmp(killTmpl, "*STRUCTURE*") == 0)
						? o->isKindOf(KINDOF_STRUCTURE)
						: (strcmp(o->getTemplate()->getName().str(), killTmpl) == 0);
					if (matches && !o->isEffectivelyDead())
					{
						headlessLog("GX_KILLTMPL: killing %s id=%d at frame %d", o->getTemplate()->getName().str(), (Int)o->getID(), (Int)now);
						o->kill();
					}
				}
				didKill = TRUE;
			}
		}

		// Sample the desync flag while the game is still live and latch it. This must be
		// TheNetwork's flag, NOT TheRecorder's: RecorderClass::sawCRCMismatch dereferences
		// m_crcInfo, which is only ever allocated when loading a replay (Recorder.cpp:1390),
		// so on a live LAN match that call is an unconditional null dereference.
		if (TheNetwork != nullptr && TheNetwork->sawCRCMismatch())
			sawMismatch = TRUE;

		if (frameLimit > 0 && maxFrame >= (UnsignedInt)frameLimit)
		{
			headlessLog("frame limit %d reached", frameLimit);
			break;
		}

		if (TheGameLogic->isInGame())
			everInGame = TRUE;
		if (TheGameLogic->getFrame() > maxFrame)
			maxFrame = TheGameLogic->getFrame();

		// Once the match ends the logic drops out of the game; without this the loop would
		// spin in the post-game shell forever. Gate on everInGame, not s_gameStarted - see the
		// comment above the declaration.
		if (everInGame && !TheGameLogic->isInGame())
		{
			headlessLog("game over at frame %d", (Int)maxFrame);
			break;
		}
	}

	headlessLog("simulated %d frames", (Int)maxFrame);

	// A run that simulated nothing is a failure, not a success. Reporting exit 0 here is how the
	// race above stayed invisible: the process looked like a clean run that simply had no work.
	if (maxFrame == 0)
	{
		headlessLog("FAILED: the match never simulated a single frame");
		return FALSE;
	}

	if (sawMismatch)
	{
		headlessLog("CRC MISMATCH seen during this match");
		return FALSE;
	}

	// A clean exit here means only that THIS peer never noticed a mismatch. The authoritative
	// cross-platform check is diffing the two peers' [GXCRC] streams offline, because a
	// divergence can incubate in unhashed state (Object::crc covers nine fields and never walks
	// the behavior modules) long before either peer votes on it.
	return TRUE;
}

int HeadlessMatch::run()
{
	s_gameStarted = FALSE;
	s_lobbyReady = FALSE;
	s_lobbyFailed = FALSE;
	s_lastAcceptSent = 0;
	s_lobbyFailureReason.clear();

	if (!setUpLan())
		return 1;

	// -lanlist: ask the relay what games exist and print them. This is a browser without a UI, and
	// it is the only way to exercise the whole list path - query, relay reply, client parse -
	// against the real relay on a machine with no display.
	if (TheGlobalData->m_lanListGames)
	{
		Transport *transport = TheLAN->getTransport();
		if (transport == nullptr || !transport->isRelayEnabled())
		{
			headlessLog("-lanlist needs a relay: set RelayAddress and LocalVirtualIP in Options.ini");
			return 1;
		}

		transport->requestGameList();

		// The replies are a burst of datagrams, one per game, so there is nothing to wait FOR -
		// wait a fixed moment and print whatever arrived. Two seconds is far longer than a relay
		// round trip and short enough to stay usable.
		const UnsignedInt deadline = timeGetTime() + 2000;
		while (timeGetTime() < deadline)
		{
			TheLAN->update();
			Sleep(10);
		}

		const Int count = transport->getGameListCount();
		headlessLog("%d game(s) listed", count);
		for (Int i = 0; i < count; ++i)
		{
			const Transport::RelayGameListing *g = transport->getGameListing(i);
			if (g == nullptr)
				continue;
			headlessLog("  room=%s host=%d.%d.%d.%d players=%d/%d name=\"%s\" map=\"%s\"",
				g->room, PRINTF_IP_AS_4_INTS(g->hostVirtualIP), g->players, g->slots,
				g->name, g->map);
		}
		return (count > 0) ? 0 : 1;
	}

	if (!driveLobby())
	{
		if (s_lobbyFailureReason.isNotEmpty())
			headlessLog("aborting: %s", s_lobbyFailureReason.str());
		else
			headlessLog("aborting: lobby did not reach a started game");
		return 1;
	}

	const Bool ok = runMatch();

	if (TheLAN)
	{
		TheLAN->RequestGameLeave();
		TheLAN->update();
	}

	return ok ? 0 : 1;
}
