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
#include "Common/Recorder.h"
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
#include "Common/UserPreferences.h"	// LANPreferences
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

} // namespace

// ------------------------------------------------------------------------------------------

Bool HeadlessMatch::isRequested()
{
	return TheGlobalData->m_lanRole != LANROLE_NONE;
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

	// Same interface election the lobby does. -defaultIP wins if it was given, which is the
	// escape hatch on a multi-homed box; otherwise take the first enumerated address.
	UnsignedInt ip = TheGlobalData->m_defaultIP;
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

	// Log the serialised slot list. Without this there is no way to confirm from the logs that
	// -lanai actually placed an AI, and a soak that silently ran human-only would "prove" a fix
	// to the AI-only desync class while never exercising it - skirmish scripts attach ONLY for AI
	// players. AI slots appear as CE/CM/CB in the S= field.
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
	}

	// Both roles land here. The host reaches OnGameStart through its own RequestGameStart
	// round trip, so it waits too rather than assuming.
	if (!pumpUntil([]{ return s_gameStarted != FALSE; }, lobbyTimeout, "game start"))
		return FALSE;

	return TRUE;
}

Bool HeadlessMatch::runMatch()
{
	const Int frameLimit = TheGlobalData->m_lanFrameLimit;
	Bool sawMismatch = FALSE;

	// Reuse GameEngine::update rather than open-coding the subsystem order. That ordering
	// (client, message stream, network, then logic, all inside VERIFY_CRC) is load-bearing
	// for determinism, and a second copy of it here would drift from the real one.
	while (!TheGameEngine->getQuitting())
	{
		TheGameEngine->update();

		// Sample the desync flag while the game is still live and latch it. This must be
		// TheNetwork's flag, NOT TheRecorder's: RecorderClass::sawCRCMismatch dereferences
		// m_crcInfo, which is only ever allocated when loading a replay (Recorder.cpp:1390),
		// so on a live LAN match that call is an unconditional null dereference.
		if (TheNetwork != nullptr && TheNetwork->sawCRCMismatch())
			sawMismatch = TRUE;

		if (frameLimit > 0 && TheGameLogic->getFrame() >= (UnsignedInt)frameLimit)
		{
			headlessLog("frame limit %d reached", frameLimit);
			break;
		}

		// Once the match ends the logic drops out of the game; without this the loop would
		// spin in the post-game shell forever.
		if (s_gameStarted && !TheGameLogic->isInGame())
		{
			headlessLog("game over at frame %d", (Int)TheGameLogic->getFrame());
			break;
		}
	}

	headlessLog("simulated %d frames", (Int)TheGameLogic->getFrame());

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
	s_lobbyFailureReason.clear();

	if (!setUpLan())
		return 1;

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
