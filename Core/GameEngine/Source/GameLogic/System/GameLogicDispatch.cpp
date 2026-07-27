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

// FILE: GameLogicDispatch.cpp ////////////////////////////////////////////////////////////////////
// Author: Mike Booth, Colin Day
// Description: Message logic to drive the game play
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/CRCDebug.h"
#include "Common/FramePacer.h"
#include "Common/GameAudio.h"
#include "Common/GameEngine.h"
#include "Common/GlobalData.h"
#include "Common/NameKeyGenerator.h"
#include "Common/ThingFactory.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/PlayerTemplate.h"
#include "Common/MessageStream.h"
#include "Common/MultiplayerSettings.h"
#include "Common/Recorder.h"
#include "Common/BuildAssistant.h"
#include "Common/SpecialPower.h"
#include "Common/ThingTemplate.h"
#include "Common/Upgrade.h"
#include "Common/StatsCollector.h"
#include "Common/Radar.h"

#include "GameLogic/AIPathfind.h"
#include "GameLogic/ExperienceTracker.h"	// GeneralsX @feature LAN-safe cheats spawn at a veterancy level
#include "GameLogic/GameLogic.h"
#include "GameLogic/Locomotor.h"
#include "GameLogic/Object.h"
#include "GameLogic/ObjectCreationList.h"
#include "GameLogic/ObjectIter.h"
#include "GameLogic/PartitionManager.h"	// GeneralsX @feature LAN-safe cheats need the shroud grid
#include "GameLogic/Module/AIUpdate.h"
#include "GameLogic/Module/BodyModule.h"
#include "GameLogic/Module/OpenContain.h"
#include "GameLogic/Module/ProductionUpdate.h"
#include "GameLogic/Module/SpecialPowerModule.h"
#include "GameLogic/ScriptActions.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/VictoryConditions.h"
#include "GameLogic/Weapon.h"

#include "GameClient/CommandXlat.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Drawable.h"
#include "GameClient/Eva.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindowTransitions.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/InGameUI.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/Mouse.h"
#include "GameClient/ParticleSys.h"
#include "GameClient/Shell.h"
#include "GameClient/Module/BeaconClientUpdate.h"
#include "GameClient/LookAtXlat.h"

#include "GameNetwork/NetworkInterface.h"

#include <algorithm>	// GeneralsX @feature std::sort, for the delete-objects cheat's stable victim order




#define MAX_PATH_SUBJECTS 64
static Bool theBuildPlan = false;
static Object *thePlanSubject[ MAX_PATH_SUBJECTS ];
static int thePlanSubjectCount = 0;
//static WindowLayout *background = nullptr;

// ------------------------------------------------------------------------------------------------
/** Issue the movement command to the object */
// ------------------------------------------------------------------------------------------------
static void doMoveTo( Object *obj, const Coord3D *pos )
{
	AIUpdateInterface *ai = obj->getAIUpdateInterface();
	DEBUG_ASSERTCRASH(ai, ("Attempted doMoveTo() on an Object with no AI"));
	if (ai)
	{
		if (theBuildPlan)
		{
			int i;

			// if this object isn't in the buildPlan set, add it
			for( i=0; i<thePlanSubjectCount; i++ )
				if (thePlanSubject[i] == obj)
					break;

			if (i == thePlanSubjectCount)
				thePlanSubject[ thePlanSubjectCount++ ] = obj;

			ai->queueWaypoint( pos );
		}
		else
		{
			ai->clearWaypointQueue();
			obj->leaveGroup();
			obj->releaseWeaponLock(LOCKED_TEMPORARILY);	// release any temporary locks.
			ai->aiMoveToPosition( pos, CMD_FROM_PLAYER );
		}
	}
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
static void doSetRallyPoint( Object *obj, const Coord3D& pos )
{
	Bool isLocalPlayer = obj->isLocallyControlled();

	//
	// we must be able to find a path from the object to the point they have chosen, cause setting
	// a rally point at a invalid location would suck.  To be super nice, we have to make sure
	// that every type of object that can be created from the thing setting the rally point
	// can actually find a path from the thing to the point
	//

	// to see the never-finished code to check all locomotor sets, see past revs of GUICommandTranslator.cpp -MDC

	//
	// for now, just use the basic human locomotor ... and enable the above code when Steven
	// tells me how to get the locomotor sets based on a thing template (CBD)
	//
	NameKeyType key = NAMEKEY( "BasicHumanLocomotor" );
	LocomotorSet locomotorSet;
	locomotorSet.addLocomotor( TheLocomotorStore->findLocomotorTemplate( key ) );
	if( TheAI->pathfinder()->clientSafeQuickDoesPathExist( locomotorSet, obj->getPosition(), &pos ) == FALSE )
	{

		// user feedback
		if( isLocalPlayer )
		{

			// display error message to user
			TheInGameUI->message( TheGameText->fetch( "GUI:RallyPointNoPath" ) );

			// play the no can do sound
			static AudioEventRTS rallyNotSet("UnableToSetRallyPoint");
			rallyNotSet.setPosition(&pos);
			rallyNotSet.setPlayerIndex(obj->getControllingPlayer()->getPlayerIndex());
			TheAudio->addAudioEvent(&rallyNotSet);

		}

		return;

	}

	// feedback to the player
	if( isLocalPlayer )
	{

		// print a message to the user
		UnicodeString info;
		info.format( TheGameText->fetch( "GUI:RallyPointSet" ),
								 obj->getTemplate()->getDisplayName().str() );
		TheInGameUI->message( info );

		// play a sound for setting the rally point
		static AudioEventRTS rallyPointSet("RallyPointSet");
		rallyPointSet.setPosition(&pos);
		rallyPointSet.setPlayerIndex(obj->getControllingPlayer()->getPlayerIndex());
		TheAudio->addAudioEvent(&rallyPointSet);

		// mark the UI as dirty so that we re-evaluate the selection and show the rally point
		Drawable *draw = obj->getDrawable();
		if( draw && draw->isSelected() )
			TheControlBar->markUIDirty();

	}

	// if this object has a ProductionExitUpdate interface, we are setting a rally point
	ExitInterface *exitInterface = obj->getObjectExitInterface();
	if( exitInterface )
	{
		// set the rally point
		exitInterface->setRallyPoint( &pos );

	}

}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
static Player *getMessagePlayer(GameMessage *msg)
{
	return ThePlayerList->getNthPlayer( msg->getPlayerIndex() );
}

// ------------------------------------------------------------------------------------------------
// GeneralsX @feature LAN-safe cheats - per-player bookkeeping for the reveal-map toggle.
//
// PartitionCell::addLooker is a COUNTER, not a flag (PartitionManager.cpp:1274 does
// min(m_currentShroud - 1, -1)), while undoRevealMapForPlayerPermanently issues exactly ONE
// removeLooker per cell. So reveal-reveal-undo leaves -2 -> -1, i.e. the map stays permanently lit,
// and an undo with no preceding reveal trips the "permanent shroud blob" DEBUG_ASSERTCRASH in
// removeLooker (PartitionManager.cpp:1313). s_gxRevealed makes the toggle idempotent.
//
// s_gxRadarForced remembers whether WE were the ones who forced the minimap on, so that un-revealing
// cannot stomp a force set by VictoryConditions (local defeat) or by a map script's
// doRadarForceEnable. Both arrays are written ONLY from the synchronised handlers below, so they are
// identical on every peer; neither is in any crc() and neither is xfer'd (nor should be - they
// describe a debug toggle, not game state).
// ------------------------------------------------------------------------------------------------
static Bool s_gxRevealed[ MAX_PLAYER_COUNT ] = { FALSE };
static Bool s_gxRadarForced[ MAX_PLAYER_COUNT ] = { FALSE };

static void gxCheatResetRevealState( void )
{
	memset( s_gxRevealed, 0, sizeof( s_gxRevealed ) );
	memset( s_gxRadarForced, 0, sizeof( s_gxRadarForced ) );
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
static Object * getSingleObjectFromSelection(const AIGroup *currentlySelectedGroup)
{
	if( currentlySelectedGroup && !currentlySelectedGroup->isEmpty() )
	{
		const VecObjectID& selectedObjects = currentlySelectedGroup->getAllIDs();
		DEBUG_ASSERTCRASH(selectedObjects.size() == 1, ("Trying to get single object from multiple selection!"));
		VecObjectID::const_iterator it = selectedObjects.begin();
		return TheGameLogic->findObjectByID(*it);
	}
	return nullptr;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void GameLogic::closeWindows()
{
	HideDiplomacy();
	ResetDiplomacy();
	HideInGameChat();
	ResetInGameChat();
	TheControlBar->hidePurchaseScience();
	TheControlBar->hideSpecialPowerShortcut();
	HideQuitMenu();

	// hide the options menu
	NameKeyType buttonID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ButtonBack" );
	GameWindow *button = TheWindowManager->winGetWindowFromId( nullptr, buttonID );
	GameWindow *window = TheWindowManager->winGetWindowFromId( nullptr, TheNameKeyGenerator->nameToKey("OptionsMenu.wnd:OptionsMenuParent") );
	if(window)
		TheWindowManager->winSendSystemMsg( window, GBM_SELECTED,
																			(WindowMsgData)button, buttonID );
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
void GameLogic::clearGameData( Bool showScoreScreen )
{
	if( !isInGame() )
	{
		DEBUG_CRASH(("We tried to clear the game data when we weren't in a game"));
		return;
	}

	setClearingGameData( TRUE );

//	m_background = TheWindowManager->winCreateLayout("Menus/BlankWindow.wnd");
//	DEBUG_ASSERTCRASH(m_background,("We Couldn't Load Menus/BlankWindow.wnd"));
//	m_background->hide(FALSE);
//	m_background->bringForward();
	// reset the game engine to accept data for a new game
	if(TheStatsCollector)
		TheStatsCollector->writeFileEnd();

	TheScriptActions->closeWindows(FALSE); // Close victory or defeat windows.

	Bool shellGame = FALSE;
	if ((!isInShellGame() || !isInGame()) && showScoreScreen && !TheGlobalData->m_headless)
	{
		shellGame = TRUE;
#ifdef ALLOW_DEBUG_UTILS
		// GeneralsX @bugfix Copilot 22/03/2026 Emit runtime diagnostics when ScoreScreen is pushed after game end.
		fprintf(stderr, "[SKIRMISH_DIAG] clearGameData showScoreScreen=%d isInShellGame=%d isInGame=%d headless=%d -> pushing ScoreScreen\n",
			showScoreScreen ? 1 : 0,
			isInShellGame() ? 1 : 0,
			isInGame() ? 1 : 0,
			TheGlobalData->m_headless ? 1 : 0);
		fflush(stderr);
#endif
		TheTransitionHandler->setGroup("FadeWholeScreen");
		TheShell->push("Menus/ScoreScreen.wnd");
		TheShell->showShell(FALSE); // by passing in false, we don't want to run the Init on the shell screen we just pushed on
		TheTransitionHandler->reverse("FadeWholeScreen");

		void FixupScoreScreenMovieWindow();
		FixupScoreScreenMovieWindow();

		destroyQuitMenu();
	}

	TheGameEngine->reset();
	setGameMode(GAME_NONE);
//	m_background->bringForward();
//	if(shellGame)


	if (TheGlobalData->m_initialFile.isEmpty() == FALSE || m_quitToDesktopAfterMatch)
	{
		TheGameEngine->setQuitting(TRUE);
		m_quitToDesktopAfterMatch = FALSE;
	}

	HideControlBar();
	closeWindows();

	TheMouse->setVisibility(TRUE);

	if(m_background)
	{
		m_background->destroyWindows();
		deleteInstance(m_background);
		m_background = nullptr;
	}

	setClearingGameData( FALSE );

}

// ------------------------------------------------------------------------------------------------
/** Prepare for a new game */
// ------------------------------------------------------------------------------------------------
void GameLogic::prepareNewGame( GameMode gameMode, GameDifficulty diff, Int rankPoints )
{
	//Kris: Commented this out, but leaving it around incase it bites us later. I cleaned up the
	//      nomenclature. Look for setLoadingMap() and setLoadingSave()
	//setGameLoading(TRUE);

	TheScriptEngine->setGlobalDifficulty(diff);

	if(!m_background)
	{
		m_background = TheWindowManager->winCreateLayout("Menus/BlankWindow.wnd");
		DEBUG_ASSERTCRASH(m_background,("We Couldn't Load Menus/BlankWindow.wnd"));
		m_background->hide(FALSE);
		m_background->bringForward();
	}
	m_background->getFirstWindow()->winClearStatus(WIN_STATUS_IMAGE);
	setGameMode( gameMode );
	if (!TheGlobalData->m_pendingFile.isEmpty())
	{
		TheWritableGlobalData->m_mapName = TheGlobalData->m_pendingFile;
		TheWritableGlobalData->m_pendingFile.clear();
	}

	m_rankPointsToAddAtGameStart = rankPoints;
	DEBUG_LOG(("GameLogic::prepareNewGame() - m_rankPointsToAddAtGameStart = %d", m_rankPointsToAddAtGameStart));

	// If we're about to start a game, hide the shell.
	if(!isInShellGame())
		TheShell->hideShell();

	m_startNewGame = FALSE;

}

//-------------------------------------------------------------------------------------------------
/** This message handles dispatches object command messages to the
  * appropriate objects.
	* @todo Rename this to "CommandProcessor", or similar. */
//-------------------------------------------------------------------------------------------------
void GameLogic::logicMessageDispatcher( GameMessage *msg, void *userData )
{
#ifdef RTS_DEBUG
	DEBUG_ASSERTCRASH(msg != nullptr && msg != (GameMessage*)0xdeadbeef, ("bad msg"));
#endif

	Player *msgPlayer = getMessagePlayer(msg);
	if (msgPlayer == nullptr)
	{
		DEBUG_CRASH(("logicMessageDispatcher: Processing message from unknown player (player index '%d')", msg->getPlayerIndex()));
		return;
	}

	AIGroupPtr currentlySelectedGroup = nullptr;

	if (isInGame())
	{
		if (msg->getType() >= GameMessage::MSG_BEGIN_NETWORK_MESSAGES && msg->getType() <= GameMessage::MSG_END_NETWORK_MESSAGES)
		{
			if (msg->getType() != GameMessage::MSG_LOGIC_CRC && msg->getType() != GameMessage::MSG_SET_REPLAY_CAMERA)
			{
				currentlySelectedGroup = TheAI->createGroup(); // can't do this outside a game - it'll cause sync errors galore.
				CRCGEN_LOG(( "Creating AIGroup %d in GameLogic::logicMessageDispatcher()", currentlySelectedGroup?currentlySelectedGroup->getID():0 ));
#if RETAIL_COMPATIBLE_AIGROUP
				msgPlayer->getCurrentSelectionAsAIGroup(currentlySelectedGroup);
#else
				msgPlayer->getCurrentSelectionAsAIGroup(currentlySelectedGroup.Peek());
#endif

				// We can't issue commands to groups that contain units that don't belong to the issuing player, so pretend like
				// there's nothing selected. Also, if currentlySelectedGroup is empty, go ahead and delete it, so that we can skip
				// any processing on it.
				if (currentlySelectedGroup->isEmpty())
				{
#if RETAIL_COMPATIBLE_AIGROUP
					TheAI->destroyGroup(currentlySelectedGroup);
#endif
					currentlySelectedGroup = nullptr;
				}

				// If there are any units that the player doesn't own, then remove them from the "currentlySelectedGroup"
				if (currentlySelectedGroup)
					if (currentlySelectedGroup->removeAnyObjectsNotOwnedByPlayer(msgPlayer))
						currentlySelectedGroup = nullptr;

				if(TheStatsCollector)
					TheStatsCollector->collectMsgStats(msg);
			}
		}
	}

#ifdef DEBUG_LOGGING
	AsciiString commandName;

	commandName = msg->getCommandAsString();
	if (msg->getType() < GameMessage::MSG_BEGIN_NETWORK_MESSAGES || msg->getType() > GameMessage::MSG_END_NETWORK_MESSAGES)
	{
		commandName.concat(" (NON-LOGIC-MESSAGE!!!)");
	}
	else if (msg->getType() == GameMessage::MSG_BEGIN_NETWORK_MESSAGES)
	{
		commandName = " (CRC message!)";
	}
#if 0
	if (commandName.isNotEmpty() /*&& msg->getType() != GameMessage::MSG_FRAME_TICK*/)
	{
		DEBUG_LOG(("Frame %d: GameLogic::logicMessageDispatcher() saw a %s from player %d (%ls)", getFrame(), commandName.str(),
			msgPlayer->getPlayerIndex(), msgPlayer->getPlayerDisplayName().str()));
	}
#endif
#endif // DEBUG_LOGGING

	// process the message
	GameMessage::Type msgType = msg->getType();
	switch( msgType )
	{
		case GameMessage::MSG_NEW_GAME:
		{
			gxCheatResetRevealState();	// GeneralsX @feature Radar::reset() also clears m_radarForceOn
			onNewGame(msg);
			break;
		}
		case GameMessage::MSG_CLEAR_GAME_DATA:
		{
			gxCheatResetRevealState();	// GeneralsX @feature
			onClearGameData(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_META_BEGIN_PATH_BUILD:
		{
			onBeginPathBuild(msg);
			break;
		}
		case GameMessage::MSG_META_END_PATH_BUILD:
		{
			onEndPathBuild(msg);
			break;
		}
		case GameMessage::MSG_SET_RALLY_POINT:
		{
			onSetRallyPoint(msg);
			break;
		}
		case GameMessage::MSG_DO_WEAPON:
		{
			onDoWeapon(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_COMBATDROP_AT_OBJECT:
		{
			onCombatdropAtObject(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_COMBATDROP_AT_LOCATION:
		{
			onCombatdropAtLocation(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_WEAPON_AT_OBJECT:
		{
			onDoWeaponAtObject(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_SWITCH_WEAPONS:
		{
			onDoSwitchWeapons(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_SET_MINE_CLEARING_DETAIL:
		{
			onSetMineClearingDetail(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_ENABLE_RETALIATION_MODE:
		{
			onEnableRetaliationMode(msg);
			break;
		}
		case GameMessage::MSG_DO_WEAPON_AT_LOCATION:
		{
			onDoWeaponAtLocation(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_SPECIAL_POWER:
		{
			onDoSpecialPower(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_SPECIAL_POWER_AT_LOCATION:
		{
			onDoSpecialPowerAtLocation(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_SPECIAL_POWER_AT_OBJECT:
		{
			onDoSpecialPowerAtObject(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_ATTACKMOVETO:
		{
			onDoAttackmoveto(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_FORCEMOVETO:
		{
			onDoForcemoveto(msg, currentlySelectedGroup);
			break;
		}
		// MSG_DO_SALVAGE is intentionally set up to mimic the moveto.
		case GameMessage::MSG_DO_SALVAGE:
		case GameMessage::MSG_DO_MOVETO:
		{
			onDoMoveto(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_ADD_WAYPOINT:
		{
			onAddWaypoint(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_GUARD_POSITION:
		{
			onDoGuardPosition(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_GUARD_OBJECT:
		{
			onDoGuardObject(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_STOP:
		{
			onDoStop(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_SCATTER:
		{
			onDoScatter(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_CREATE_FORMATION:
		{
			onCreateFormation(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_CLEAR_INGAME_POPUP_MESSAGE:
		{
			onClearIngamePopupMessage(msg);
			break;
		}
		case GameMessage::MSG_DO_CHEER:
		{
			onDoCheer(msg, currentlySelectedGroup);
			break;
		}

#if defined(RTS_DEBUG) || defined (_ALLOW_DEBUG_CHEATS_IN_RELEASE)

		case GameMessage::MSG_DEBUG_KILL_SELECTION:
		{
			onDebugKillSelection(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DEBUG_HURT_OBJECT:
		{
			onDebugHurtObject(msg);
			break;
		}
		case GameMessage::MSG_DEBUG_KILL_OBJECT:
		{
			onDebugKillObject(msg);
			break;
		}
#endif

		// GeneralsX @feature LAN-safe cheats. DELIBERATELY OUTSIDE the RTS_DEBUG /
		// _ALLOW_DEBUG_CHEATS_IN_RELEASE guards above. The enum values are unconditional; if the
		// handlers were conditional, a peer built without cheats would receive the message, match no
		// case (this switch has no default:), silently no-op, and then mismatch at the next
		// NET_CRC_INTERVAL. That is the single most dangerous mistake available here.
		case GameMessage::MSG_GX_CHEAT_GIVE_MONEY:
		{
			onGxCheatGiveMoney(msg);
			break;
		}
		case GameMessage::MSG_GX_CHEAT_SET_MONEY:
		{
			onGxCheatSetMoney(msg);
			break;
		}
		case GameMessage::MSG_GX_CHEAT_REVEAL_MAP:
		{
			onGxCheatRevealMap(msg);
			break;
		}
		case GameMessage::MSG_GX_CHEAT_KILL_PLAYER:
		{
			onGxCheatKillPlayer(msg);
			break;
		}
		case GameMessage::MSG_GX_CHEAT_KILL_OBJECTS:
		{
			onGxCheatKillObjects(msg);
			break;
		}
		case GameMessage::MSG_GX_CHEAT_SPAWN_UNIT:
		{
			onGxCheatSpawnUnit(msg);
			break;
		}
		case GameMessage::MSG_GX_CHEAT_DELETE_OBJECTS:
		{
			onGxCheatDeleteObjects(msg);
			break;
		}

		case GameMessage::MSG_ENTER:
		{
			onEnter(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_EXIT:
		{
			onExit(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_EVACUATE:
		{
			onEvacuate(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_EXECUTE_RAILED_TRANSPORT:
		{
			onExecuteRailedTransport(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_INTERNET_HACK:
		{
			onInternetHack(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_GET_REPAIRED:
		{
			onGetRepaired(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DOCK:
		{
			onDock(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_GET_HEALED:
		{
			onGetHealed(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_REPAIR:
		{
			onDoRepair(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_RESUME_CONSTRUCTION:
		{
			onResumeConstruction(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_SPECIAL_POWER_OVERRIDE_DESTINATION:
		{
			onDoSpecialPowerOverrideDestination(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_ATTACK_OBJECT:
		{
			onDoAttackObject(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_FORCE_ATTACK_OBJECT:
		{
			onDoForceAttackObject(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DO_FORCE_ATTACK_GROUND:
		{
			onDoForceAttackGround(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_QUEUE_UPGRADE:
		{
			onQueueUpgrade(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_CANCEL_UPGRADE:
		{
			onCancelUpgrade(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_QUEUE_UNIT_CREATE:
		{
			onQueueUnitCreate(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_CANCEL_UNIT_CREATE:
		{
			onCancelUnitCreate(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DOZER_CONSTRUCT:
		case GameMessage::MSG_DOZER_CONSTRUCT_LINE:
		{
			onDozerConstruct(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_DOZER_CANCEL_CONSTRUCT:
		{
			onDozerCancelConstruct(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_SELL:
		{
			onSell(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_TOGGLE_OVERCHARGE:
		{
			onToggleOvercharge(msg, currentlySelectedGroup);
			break;
		}

#ifdef ALLOW_SURRENDER
		case GameMessage::MSG_DO_SURRENDER:
		{
			onDoSurrender(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_PICK_UP_PRISONER:
		{
			onPickUpPrisoner(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_RETURN_TO_PRISON:
		{
			onReturnToPrison(msg, currentlySelectedGroup);
			break;
		}
#endif

		// No sound does exactly the same logical processing as the usual message. Just double them up.
		case GameMessage::MSG_CREATE_SELECTED_GROUP_NO_SOUND:
		case GameMessage::MSG_CREATE_SELECTED_GROUP:
		{
			onCreateSelectedGroup(msg);
			break;
		}
		case GameMessage::MSG_REMOVE_FROM_SELECTED_GROUP:
		{
			onRemoveFromSelectedGroup(msg);
			break;
		}
		case GameMessage::MSG_DESTROY_SELECTED_GROUP:
		{
			onDestroySelectedGroup(msg);
			break;
		}
		case GameMessage::MSG_SELECTED_GROUP_COMMAND:
		{
			break;
		}
		case GameMessage::MSG_PLACE_BEACON:
		{
			onPlaceBeacon(msg);
			break;
		}
		case GameMessage::MSG_REMOVE_BEACON:
		{
			onRemoveBeacon(msg);
			break;
		}
		case GameMessage::MSG_SET_BEACON_TEXT:
		{
			onSetBeaconText(msg, currentlySelectedGroup);
			break;
		}
		case GameMessage::MSG_SELF_DESTRUCT:
		{
			onSelfDestruct(msg);
			break;
		}
		case GameMessage::MSG_SET_REPLAY_CAMERA:
		{
			onSetReplayCamera(msg);
			break;
		}
		case GameMessage::MSG_CREATE_TEAM0:
		case GameMessage::MSG_CREATE_TEAM1:
		case GameMessage::MSG_CREATE_TEAM2:
		case GameMessage::MSG_CREATE_TEAM3:
		case GameMessage::MSG_CREATE_TEAM4:
		case GameMessage::MSG_CREATE_TEAM5:
		case GameMessage::MSG_CREATE_TEAM6:
		case GameMessage::MSG_CREATE_TEAM7:
		case GameMessage::MSG_CREATE_TEAM8:
		case GameMessage::MSG_CREATE_TEAM9:
		{
			onCreateTeam(msg);
			break;
		}
		case GameMessage::MSG_SELECT_TEAM0:
		case GameMessage::MSG_SELECT_TEAM1:
		case GameMessage::MSG_SELECT_TEAM2:
		case GameMessage::MSG_SELECT_TEAM3:
		case GameMessage::MSG_SELECT_TEAM4:
		case GameMessage::MSG_SELECT_TEAM5:
		case GameMessage::MSG_SELECT_TEAM6:
		case GameMessage::MSG_SELECT_TEAM7:
		case GameMessage::MSG_SELECT_TEAM8:
		case GameMessage::MSG_SELECT_TEAM9:
		{
			onSelectTeam(msg);
			break;
		}
		case GameMessage::MSG_ADD_TEAM0:
		case GameMessage::MSG_ADD_TEAM1:
		case GameMessage::MSG_ADD_TEAM2:
		case GameMessage::MSG_ADD_TEAM3:
		case GameMessage::MSG_ADD_TEAM4:
		case GameMessage::MSG_ADD_TEAM5:
		case GameMessage::MSG_ADD_TEAM6:
		case GameMessage::MSG_ADD_TEAM7:
		case GameMessage::MSG_ADD_TEAM8:
		case GameMessage::MSG_ADD_TEAM9:
		{
			onAddTeam(msg);
			break;
		}
		case GameMessage::MSG_LOGIC_CRC:
		{
			onLogicCrc(msg);
			break;
		}
		case GameMessage::MSG_PURCHASE_SCIENCE:
		{
			onPurchaseScience(msg);
			break;
		}
	}

#if RETAIL_COMPATIBLE_AIGROUP
	// TheSuperHackers @bugfix xezon 28/06/2025 This hack avoids crashing when players are selected during Replay playback.
	// It can read data from an already deleted AIGroup and return this function when its member size is 0, signifying that
	// it is indeed deleted.
	if (currentlySelectedGroup && currentlySelectedGroup->getCount() == 0)
		return;
#endif

	/**/ /// @todo: multiplayer semantics
	if (currentlySelectedGroup && TheRecorder->isPlaybackMode() && TheGlobalData->m_useCameraInReplay && TheControlBar->getObserverLookAtPlayer() == msgPlayer /*&& !TheRecorder->isMultiplayer()*/)
	{
		const VecObjectID& selectedObjects = currentlySelectedGroup->getAllIDs();
		TheInGameUI->deselectAllDrawables();
		for (VecObjectID::const_iterator it = selectedObjects.begin(); it != selectedObjects.end(); ++it)
		{
			const Object *obj = findObjectByID(*it);
			if (obj)
			{
				Drawable *draw = obj->getDrawable();
				if (draw)
					TheInGameUI->selectDrawable(draw);
			}
		}
	}
	/**/

	if( currentlySelectedGroup != nullptr )
	{
#if RETAIL_COMPATIBLE_AIGROUP
		TheAI->destroyGroup(currentlySelectedGroup);
#else
		currentlySelectedGroup->removeAll();
#endif
	}

}

bool GameLogic::onNewGame(MAYBE_UNUSED GameMessage *msg)
{
#if !RETAIL_COMPATIBLE_CRC
	// TheSuperHackers @fix stephanmeesters 11/03/2026
	// Make sure we're ready to start a new game. This prevents an issue where an infinite disconnect screen
	// can be force-triggered in an online match by using cheats.
	if ( isInGame() || isClearingGameData() || isLoadingMap() )
	{
		DEBUG_CRASH( ("Called MSG_NEW_GAME while game is not ready (inGame=%d, clearingData=%d, loadingMap=%d)",
			isInGame(), isClearingGameData(), isLoadingMap()) );

		return false;
	}
#endif

	//DEBUG_ASSERTCRASH(msg->getArgumentCount() == 1 || msg->getArgumentCount() == 2, ("%d arguments to MSG_NEW_GAME", msg->getArgumentCount()));
	GameMode gameMode = (GameMode)msg->getArgument( 0 )->integer;
	Int rankPoints = 0;
	GameDifficulty diff = DIFFICULTY_NORMAL;
	if ( msg->getArgumentCount() >= 2 )
		diff = (GameDifficulty)msg->getArgument( 1 )->integer;
	if ( msg->getArgumentCount() >= 3 )
		rankPoints = msg->getArgument( 2 )->integer;

	if ( msg->getArgumentCount() >= 4 )
	{
		Int maxFPS = msg->getArgument( 3 )->integer;
		if (maxFPS < 1 || maxFPS > 1000)
			maxFPS = TheGlobalData->m_framesPerSecondLimit;
		DEBUG_LOG(("Setting max FPS limit to %d FPS", maxFPS));
		TheFramePacer->setFramesPerSecondLimit(maxFPS);
		TheWritableGlobalData->m_useFpsLimit = true;

		// GeneralsX @bugfix Raising the render cap without switching the logic time scale on
		// leaves canUpdateRegularGameLogic() ticking the simulation once per rendered frame, so
		// the whole game runs at render speed instead of merely looking smoother.
		// GameEngine::init pairs these two calls; this path set the limit and never touched the
		// scale. It matters most in solo play, because reallyDoStart() turns a game-speed slider
		// at maximum into maxFPS = 1000 -- so "unlimited" made the match run absurdly fast rather
		// than unlocking the frame rate, which is why the slider was effectively unusable above 60.
		//
		// Network games are deliberately excluded. FramePacer::getActualLogicTimeScaleFps defers
		// to TheNetwork->getFrameRate() while a network game exists, and CommandXlat's
		// changeLogicTimeScale refuses outright for the same reason; the peers agree the logic
		// rate between themselves and the client must not second-guess it.
		if (TheNetwork == nullptr)
		{
			if (maxFPS > BaseFps)
			{
				TheFramePacer->setLogicTimeScaleFps(BaseFps);
				TheFramePacer->enableLogicTimeScale(TRUE);
			}
			else
			{
				// Starting a capped game after an uncapped one must not inherit the scale.
				TheFramePacer->enableLogicTimeScale(FALSE);
			}
		}
	}

	// prepare for new game
	prepareNewGame( gameMode, diff, rankPoints );

	// start new game
	startNewGame( FALSE );

	return true;
}

bool GameLogic::onClearGameData(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
#if defined(RTS_DEBUG)
	if (TheDisplay && TheGlobalData->m_dumpAssetUsage)
		TheDisplay->dumpAssetUsage(TheGlobalData->m_mapName.str());
#endif

	if (currentlySelectedGroup)
	{
#if RETAIL_COMPATIBLE_AIGROUP
		TheAI->destroyGroup(currentlySelectedGroup);
#else
		currentlySelectedGroup->removeAll();
#endif
	}
	currentlySelectedGroup = nullptr;
	clearGameData();

	return true;
}

bool GameLogic::onBeginPathBuild(MAYBE_UNUSED GameMessage *msg)
{
	DEBUG_LOG(("META: begin path build"));
	DEBUG_ASSERTCRASH(!theBuildPlan, ("mismatched theBuildPlan"));

	if (theBuildPlan == false)
	{
		theBuildPlan = true;
		thePlanSubjectCount = 0;
	}

	return true;
}

bool GameLogic::onEndPathBuild(MAYBE_UNUSED GameMessage *msg)
{
	DEBUG_LOG(("META: end path build"));
	DEBUG_ASSERTCRASH(theBuildPlan, ("mismatched theBuildPlan"));

	// tell everyone who participated in the plan to move
	for( int i=0; i<thePlanSubjectCount; i++ )
	{
		AIUpdateInterface *ai = thePlanSubject[i]->getAIUpdateInterface();
		if (ai)
			ai->executeWaypointQueue();
	}

	theBuildPlan = false;
	thePlanSubjectCount = 0;

	return true;
}

bool GameLogic::onSetRallyPoint(MAYBE_UNUSED GameMessage *msg)
{
	Object *obj = findObjectByID( msg->getArgument( 0 )->objectID );
	Coord3D dest = msg->getArgument( 1 )->location;

	if (!obj)
		return false;

#if !RETAIL_COMPATIBLE_CRC
	// TheSuperHackers @fix stephanmeesters 11/03/2026 Validate the owner of the source object
	Player *msgPlayer = getMessagePlayer(msg);
	if ( obj->getControllingPlayer() != msgPlayer )
	{
		DEBUG_CRASH( ("MSG_SET_RALLY_POINT: Player '%ls' attempted to set the rally point of object '%s' owned by player '%ls'.",
			msgPlayer->getPlayerDisplayName().str(),
			obj->getTemplate()->getName().str(),
			obj->getControllingPlayer()->getPlayerDisplayName().str()) );
		return false;
	}
#endif

	doSetRallyPoint( obj, dest );

	return true;
}

bool GameLogic::onDoWeapon(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	WeaponSlotType weaponSlot = (WeaponSlotType)msg->getArgument( 0 )->integer;
	Int maxShotsToFire = msg->getArgument( 1 )->integer;

	// lock it just till the weapon is empty or the attack is "done"
	if( currentlySelectedGroup && currentlySelectedGroup->setWeaponLockForGroup( weaponSlot, LOCKED_TEMPORARILY ))
	{
		currentlySelectedGroup->groupAttackPosition( nullptr, maxShotsToFire, CMD_FROM_PLAYER );
	}

	return true;
}

bool GameLogic::onCombatdropAtObject(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Object *targetObject = findObjectByID( msg->getArgument( 0 )->objectID );

	// issue command for either single object or for selected group
	if( currentlySelectedGroup && targetObject )
		currentlySelectedGroup->groupCombatDrop( targetObject,
																							*targetObject->getPosition(),
																							CMD_FROM_PLAYER );

	/*
	if( sourceObject && targetObject )
	{
		AIUpdateInterface* sourceAI = sourceObject->getAIUpdateInterface();
		if (sourceAI)
		{
			sourceAI->aiCombatDrop( targetObject, *targetObject->getPosition(), CMD_FROM_PLAYER );
		}
	}
	*/

	return true;
}

bool GameLogic::onCombatdropAtLocation(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Coord3D targetLoc = msg->getArgument( 0 )->location;

	if( currentlySelectedGroup )
		currentlySelectedGroup->groupCombatDrop( nullptr, targetLoc, CMD_FROM_PLAYER );

	/*
	if( sourceObject )
	{
		AIUpdateInterface* sourceAI = sourceObject->getAIUpdateInterface();
		if (sourceAI)
		{
			sourceAI->aiCombatDrop( nullptr, targetLoc, CMD_FROM_PLAYER );
		}
	}
	*/

	return true;
}

bool GameLogic::onDoWeaponAtObject(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	// Lock the weapon choice to the right weapon, then give an attack command

	WeaponSlotType weaponSlot = (WeaponSlotType)msg->getArgument( 0 )->integer;
	Object *targetObject = findObjectByID( msg->getArgument( 1 )->objectID );
	Int maxShotsToFire = msg->getArgument( 2 )->integer;

	// sanity
	if( targetObject == nullptr )
		return false;

	// issue command for either single object or for selected group
	if( currentlySelectedGroup )
	{
			// lock it just till the weapon is empty or the attack is "done"
		if (currentlySelectedGroup->setWeaponLockForGroup( weaponSlot, LOCKED_TEMPORARILY ))
			currentlySelectedGroup->groupAttackObject( targetObject, maxShotsToFire, CMD_FROM_PLAYER );
	}

	return true;
}

bool GameLogic::onDoSwitchWeapons(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	// use the selected group
	WeaponSlotType weaponSlot = (WeaponSlotType)msg->getArgument( 0 )->integer;
	// lock until un-switched, or switched to something else.
	if( currentlySelectedGroup )
		currentlySelectedGroup->setWeaponLockForGroup( weaponSlot, LOCKED_PERMANENTLY );

	return true;
}

bool GameLogic::onSetMineClearingDetail(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	if( currentlySelectedGroup )
	{
		currentlySelectedGroup->setMineClearingDetail(true);
	}

	return true;
}

bool GameLogic::onEnableRetaliationMode(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);

#if RETAIL_COMPATIBLE_CRC
	(void)msgPlayer;
	//Logically turns on or off retaliation mode for a specified player.
	const Int playerIndex = msg->getArgument( 0 )->integer;
	const Bool enableRetaliation = msg->getArgument( 1 )->boolean;

	Player *player = ThePlayerList->getNthPlayer( playerIndex );
	if( player )
	{
		DEBUG_ASSERTCRASH(player == msgPlayer,
			("Retaliation mode of player '%ls' was illegally set by player '%ls'. Before: '%d', after: '%d'.",
				player->getPlayerDisplayName().str(), msgPlayer->getPlayerDisplayName().str(),
				player->isLogicalRetaliationModeEnabled(), enableRetaliation) );

		player->setLogicalRetaliationModeEnabled( enableRetaliation );
	}
#else
	// TheSuperHackers @fix stephanmeesters 08/03/2026 Ensure that players can only set their own retaliation mode.
	const Bool enableRetaliation = msg->getArgument( 0 )->boolean;
	msgPlayer->setLogicalRetaliationModeEnabled( enableRetaliation );
#endif

	return true;
}

bool GameLogic::onDoWeaponAtLocation(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	WeaponSlotType weaponSlot = (WeaponSlotType)msg->getArgument( 0 )->integer;
	Coord3D targetLoc = msg->getArgument( 1 )->location;
	Int maxShotsToFire = msg->getArgument( 2 )->integer;

	// issue command for either single object or for selected group
	if( currentlySelectedGroup )
	{
			// lock it just till the weapon is empty or the attack is "done"
		if (currentlySelectedGroup->setWeaponLockForGroup( weaponSlot, LOCKED_TEMPORARILY ))
			currentlySelectedGroup->groupAttackPosition( &targetLoc, maxShotsToFire, CMD_FROM_PLAYER );
	}

	return true;
}

bool GameLogic::onDoSpecialPower(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	// first argument is the special power ID
	UnsignedInt specialPowerID = msg->getArgument( 0 )->integer;

	// Command button options -- special power may care about variance options
	UnsignedInt options = msg->getArgument( 1 )->integer;

	// check for possible specific source, ignoring selection.
	ObjectID sourceID = msg->getArgument(2)->objectID;
	Object* source = findObjectByID(sourceID);
	if (source != nullptr)
	{
#if !RETAIL_COMPATIBLE_CRC
		// TheSuperHackers @fix stephanmeesters 01/03/2026 Validate the origin of the source object
		Player *msgPlayer = getMessagePlayer(msg);
		if ( source->getControllingPlayer() != msgPlayer )
		{
			DEBUG_CRASH( ("MSG_DO_SPECIAL_POWER: Player '%ls' attempted to control the object '%s' owned by player '%ls'.",
					msgPlayer->getPlayerDisplayName().str(),
					source->getTemplate()->getName().str(),
					source->getControllingPlayer()->getPlayerDisplayName().str()) );
			return false;
		}
#endif

		AIGroupPtr theGroup = TheAI->createGroup();
		theGroup->add(source);
		theGroup->groupDoSpecialPower( specialPowerID, options );
#if RETAIL_COMPATIBLE_AIGROUP
		TheAI->destroyGroup(theGroup);
#else
		theGroup->removeAll();
#endif
	}
	else
	{
		//Use the selected group!
		if( currentlySelectedGroup )
		{
			currentlySelectedGroup->groupDoSpecialPower( specialPowerID, options );
		}
	}

	return true;
}

bool GameLogic::onDoSpecialPowerAtLocation(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	const Bool hasAngle = msg->getArgumentCount() >= 6;
	Int argumentIndex = 0;

	// first argument is the special power ID
	UnsignedInt specialPowerID = msg->getArgument( argumentIndex++ )->integer;

	// Location argument 2 is destination
	Coord3D targetCoord = msg->getArgument( argumentIndex++ )->location;

	// Angle argument 3 is the orientation of the special power (if applicable)
	Real angle = hasAngle ? msg->getArgument( argumentIndex++ )->real : INVALID_ANGLE;

	// Object in way -- if applicable (some specials care, others don't)
	ObjectID objectID = msg->getArgument( argumentIndex++ )->objectID;
	Object *objectInWay = findObjectByID( objectID );

	// Command button options -- special power may care about variance options
	UnsignedInt options = msg->getArgument( argumentIndex++ )->integer;

	// check for possible specific source, ignoring selection.
	ObjectID sourceID = msg->getArgument( argumentIndex++ )->objectID;
	Object* source = findObjectByID( sourceID );
	if (source != nullptr)
	{
#if !RETAIL_COMPATIBLE_CRC
		// TheSuperHackers @fix stephanmeesters 01/03/2026 Validate the origin of the source object
		Player *msgPlayer = getMessagePlayer(msg);
		if ( source->getControllingPlayer() != msgPlayer )
		{
			DEBUG_CRASH( ("MSG_DO_SPECIAL_POWER_AT_LOCATION: Player '%ls' attempted to control the object '%s' owned by player '%ls'.",
					msgPlayer->getPlayerDisplayName().str(),
					source->getTemplate()->getName().str(),
					source->getControllingPlayer()->getPlayerDisplayName().str()) );
			return false;
		}
#endif

		AIGroupPtr theGroup = TheAI->createGroup();
		theGroup->add(source);
		theGroup->groupDoSpecialPowerAtLocation( specialPowerID, &targetCoord, angle, objectInWay, options );
#if RETAIL_COMPATIBLE_AIGROUP
		TheAI->destroyGroup(theGroup);
#else
		theGroup->removeAll();
#endif
	}
	else
	{
		//Use the selected group!
		if( currentlySelectedGroup )
		{
			currentlySelectedGroup->groupDoSpecialPowerAtLocation( specialPowerID, &targetCoord, angle, objectInWay, options );
		}
	}

	return true;
}

bool GameLogic::onDoSpecialPowerAtObject(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{

	// first argument is the special power ID
	UnsignedInt specialPowerID = msg->getArgument( 0 )->integer;

	// argument 2 is target object
	ObjectID targetID = msg->getArgument(1)->objectID;
	Object *target = findObjectByID( targetID );
	if( !target )
	{
		return false;
	}

	// Command button options -- special power may care about variance options
	UnsignedInt options = msg->getArgument( 2 )->integer;

	// check for possible specific source, ignoring selection.
	ObjectID sourceID = msg->getArgument(3)->objectID;
	Object* source = findObjectByID(sourceID);
	if (source != nullptr)
	{
#if !RETAIL_COMPATIBLE_CRC
		// TheSuperHackers @fix stephanmeesters 01/03/2026 Validate the origin of the source object
		Player *msgPlayer = getMessagePlayer(msg);
		if ( source->getControllingPlayer() != msgPlayer )
		{
			DEBUG_CRASH( ("MSG_DO_SPECIAL_POWER_AT_OBJECT: Player '%ls' attempted to control the object '%s' owned by player '%ls'.",
					msgPlayer->getPlayerDisplayName().str(),
					source->getTemplate()->getName().str(),
					source->getControllingPlayer()->getPlayerDisplayName().str()) );
			return false;
		}
#endif

		AIGroupPtr theGroup = TheAI->createGroup();
		theGroup->add(source);
		theGroup->groupDoSpecialPowerAtObject( specialPowerID, target, options );
#if RETAIL_COMPATIBLE_AIGROUP
		TheAI->destroyGroup(theGroup);
#else
		theGroup->removeAll();
#endif
	}
	else
	{
		if( currentlySelectedGroup )
		{
			currentlySelectedGroup->groupDoSpecialPowerAtObject( specialPowerID, target, options );
		}
	}
	return true;
}

bool GameLogic::onDoAttackmoveto(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Coord3D dest = msg->getArgument( 0 )->location;

	if (currentlySelectedGroup)
	{
		currentlySelectedGroup->releaseWeaponLockForGroup(LOCKED_TEMPORARILY);	// release any temporary locks.
		currentlySelectedGroup->groupAttackMoveToPosition( &dest, NO_MAX_SHOTS_LIMIT, CMD_FROM_PLAYER );
	}

	return true;
}

bool GameLogic::onDoForcemoveto(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Coord3D dest = msg->getArgument( 0 )->location;

	if (currentlySelectedGroup)
	{
		currentlySelectedGroup->releaseWeaponLockForGroup(LOCKED_TEMPORARILY);	// release any temporary locks.
		currentlySelectedGroup->groupMoveToPosition( &dest, false, CMD_FROM_PLAYER );
	}

	return true;
}

bool GameLogic::onDoMoveto(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Coord3D dest = msg->getArgument( 0 )->location;

	if( currentlySelectedGroup )
	{
		//DEBUG_LOG(("GameLogicDispatch - got a MSG_DO_MOVETO command"));
		currentlySelectedGroup->releaseWeaponLockForGroup(LOCKED_TEMPORARILY);	// release any temporary locks.
		currentlySelectedGroup->groupMoveToPosition( &dest, false, CMD_FROM_PLAYER );
	}

	return true;
}

bool GameLogic::onAddWaypoint(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Coord3D dest = msg->getArgument( 0 )->location;

	if( currentlySelectedGroup )
	{
		//DEBUG_LOG(("GameLogicDispatch - got a MSG_DO_MOVETO command"));
		currentlySelectedGroup->releaseWeaponLockForGroup(LOCKED_TEMPORARILY);	// release any temporary locks.
		currentlySelectedGroup->groupMoveToPosition( &dest, true, CMD_FROM_PLAYER );
	}

	return true;
}

bool GameLogic::onDoGuardPosition(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Coord3D loc = msg->getArgument( 0 )->location;
	GuardMode gm = (GuardMode)msg->getArgument( 1 )->integer;
	if (currentlySelectedGroup)
	{
		currentlySelectedGroup->groupGuardPosition(&loc, gm, CMD_FROM_PLAYER);
	}

	return true;
}

bool GameLogic::onDoGuardObject(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Object* obj = findObjectByID( msg->getArgument( 0 )->objectID );
	if (!obj)
		return false;

	GuardMode gm = (GuardMode)msg->getArgument( 1 )->integer;
	if (currentlySelectedGroup)
	{
		currentlySelectedGroup->groupGuardObject(obj, gm, CMD_FROM_PLAYER);
	}

	return true;
}

bool GameLogic::onDoStop(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	if (currentlySelectedGroup)
	{
		currentlySelectedGroup->groupIdle(CMD_FROM_PLAYER);
	}

	return true;
}

bool GameLogic::onDoScatter(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	if (currentlySelectedGroup)
	{
		currentlySelectedGroup->groupScatter(CMD_FROM_PLAYER);
	}

	return true;
}

bool GameLogic::onCreateFormation(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	if (currentlySelectedGroup)
	{
		currentlySelectedGroup->groupCreateFormation(CMD_FROM_PLAYER);
	}

	return true;
}

bool GameLogic::onClearIngamePopupMessage(MAYBE_UNUSED GameMessage *msg)
{
	if( TheInGameUI )
	{
		TheInGameUI->clearPopupMessageData();
	}

	return true;
}

bool GameLogic::onDoCheer(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	//All selected units play cheer animation.
	if( currentlySelectedGroup )
	{
		currentlySelectedGroup->groupCheer( CMD_FROM_PLAYER );
	}

	return true;
}

#if defined(RTS_DEBUG) || defined (_ALLOW_DEBUG_CHEATS_IN_RELEASE)

bool GameLogic::onDebugKillSelection(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	//All selected units die
	if( currentlySelectedGroup )
	{
		const VecObjectID& selectedObjects = currentlySelectedGroup->getAllIDs();
		for (VecObjectID::const_iterator it = selectedObjects.begin(); it != selectedObjects.end(); ++it)
		{
			Object *obj = findObjectByID(*it);
			if (obj)
			{
				obj->kill();
			}
		}
	}

	return true;
}

bool GameLogic::onDebugHurtObject(MAYBE_UNUSED GameMessage *msg)
{
	Object* objToHurt = TheGameLogic->findObjectByID( msg->getArgument( 0 )->objectID );
	if (objToHurt)
	{
		DamageInfo damageInfo;
		damageInfo.in.m_damageType = DAMAGE_UNRESISTABLE;
		damageInfo.in.m_deathType = DEATH_NORMAL;
		damageInfo.in.m_sourceID = INVALID_ID;
		damageInfo.in.m_amount = objToHurt->getBodyModule()->getMaxHealth() / 10.0f;
		objToHurt->attemptDamage( &damageInfo );
	}

	return true;
}

bool GameLogic::onDebugKillObject(MAYBE_UNUSED GameMessage *msg)
{
	Object* objToHurt = TheGameLogic->findObjectByID( msg->getArgument( 0 )->objectID );
	if (objToHurt)
	{
		objToHurt->kill();
	}

	return true;
}

#endif

// ------------------------------------------------------------------------------------------------
// GeneralsX @feature LAN-safe cheats.
//
// These run inside GameLogic::update on EVERY peer, on the same logic frame, in the same order
// (Network::RelayCommandsToCommandList -> ConnectionManager::getFrameCommandList, slot order 0..N).
// They must therefore be a pure function of (message arguments, logic state).
//
// FORBIDDEN in here: ThePlayerList->getLocalPlayer(), TheInGameUI, TheTacticalView, TheGameClient's
// drawable list, timeGetTime(). Those differ per machine and are the only way to reintroduce a
// desync. "Who cheated" is getMessagePlayer(msg); "who gets hit" is an explicit Int argument.
//
// The target is carried as a PLAYER INDEX, which is safe because player indices are lockstep-
// identical on every peer. Proof, not assumption: PlayerList::crc walks m_players[0 .. m_playerCount)
// in index order and xfers each one, PartitionCell::crc xfers
// m_shroudLevel[MAX_PLAYER_COUNT] indexed by player index, and GameLogic::getCRC feeds both into the
// frame CRC. If two peers disagreed about which Player sits at index N, every existing LAN game would
// mismatch at the first NET_CRC_INTERVAL. Indices come from PlayerList::newGame walking TheSidesList
// in file order, which is the same map on both machines.
//
// This file is compiled into BOTH g_gameengine and z_gameengine, so it may only use API that exists
// in both trees. Note in particular obj->kill() with NO arguments: Generals/.../Object.h:215 takes no
// parameters, GeneralsMD/.../Object.h:228 has two defaulted ones.
// ------------------------------------------------------------------------------------------------

// ------------------------------------------------------------------------------------------------
/** GameMessage::getArgument() out of range returns a shared zeroed static and only DEBUG_CRASHes,
	* which is a no-op in the shipping build. A short message would therefore be silently read as
	* "player 0, amount 0" instead of being rejected. Check the count explicitly. */
// ------------------------------------------------------------------------------------------------
static Bool gxCheatArgsOk( const GameMessage *msg, Int needed )
{
	return msg != nullptr && (Int)msg->getArgumentCount() >= needed;
}

// ------------------------------------------------------------------------------------------------
/** Resolve argument 0 to the player the cheat is aimed at. */
// ------------------------------------------------------------------------------------------------
static Player *gxCheatTarget( const GameMessage *msg )
{
	if( gxCheatArgsOk( msg, 1 ) == FALSE || ThePlayerList == nullptr )
		return nullptr;

	const Int idx = msg->getArgument( 0 )->integer;

	// Index 0 is permanently the neutral player (PlayerList::init sets m_playerCount = 1), and
	// PlayerList::getNthPlayer only range-checks against MAX_PLAYER_COUNT, not the live count -
	// indices in [getPlayerCount(), MAX_PLAYER_COUNT) return a real but unconfigured Player.
	if( idx <= 0 || idx >= ThePlayerList->getPlayerCount() )
		return nullptr;

	return ThePlayerList->getNthPlayer( idx );
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
bool GameLogic::onGxCheatGiveMoney(MAYBE_UNUSED GameMessage *msg)
{
	Player *target = gxCheatTarget( msg );
	if( target == nullptr || gxCheatArgsOk( msg, 2 ) == FALSE )
		return false;

	Money *m = target->getMoney();
	if( m == nullptr )
		return false;

	// Money::deposit/withdraw take UnsignedInt, so a negative delta must be negated, not cast.
	// playSound=FALSE: this fires on every peer at once and the chime is meaningless for a remote
	// target. trackIncome=FALSE: cheated cash must not pollute the cash-per-minute readout. Same
	// choice ScriptActions::doSetMoney makes.
	const Int amount = msg->getArgument( 1 )->integer;
	if( amount < 0 )
		m->withdraw( (UnsignedInt)( -amount ), FALSE );
	else
		m->deposit( (UnsignedInt)amount, FALSE, FALSE );

	DEBUG_LOG(("GX cheat frame %d: GIVE_MONEY %d to player %d (from player %d), now %u",
		getFrame(), amount, target->getPlayerIndex(), msg->getPlayerIndex(), m->countMoney()));

	return true;
}

// ------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------
bool GameLogic::onGxCheatSetMoney(MAYBE_UNUSED GameMessage *msg)
{
	Player *target = gxCheatTarget( msg );
	if( target == nullptr || gxCheatArgsOk( msg, 2 ) == FALSE )
		return false;

	Money *m = target->getMoney();
	if( m == nullptr )
		return false;

	const Int amount = msg->getArgument( 1 )->integer;
	m->withdraw( m->countMoney(), FALSE );
	if( amount > 0 )
		m->deposit( (UnsignedInt)amount, FALSE, FALSE );

	DEBUG_LOG(("GX cheat frame %d: SET_MONEY %d on player %d (from player %d), now %u",
		getFrame(), amount, target->getPlayerIndex(), msg->getPlayerIndex(), m->countMoney()));

	return true;
}

// ------------------------------------------------------------------------------------------------
/** Reveal or re-shroud the whole map for one player, and make that player's MINIMAP actually show
	* it. Those are two independent gates and only one of them is the shroud:
	*
	*  1) The shroud grid. It is simulation state and it IS in the frame CRC (PartitionCell::crc xfers
	*     m_shroudLevel for all MAX_PLAYER_COUNT players and PartitionManager::crc walks every cell),
	*     so a one-sided reveal is a guaranteed mismatch. This MUST happen here and not on the client.
	*     revealMapForPlayerPermanently, not revealMapForPlayer: the permanent variant does addLooker
	*     with no matching removeLooker so cells land on CELLSHROUD_CLEAR, whereas the plain variant
	*     lands them on CELLSHROUD_FOGGED, where W3DRadar::canRenderObject rejects every blip
	*     (W3DRadar.cpp:665, "shroudedStatus > OBJECTSHROUD_PARTIAL_CLEAR"). Fogged gives a dim map
	*     with no units on it, which reads as "the cheat did nothing".
	*
	*  2) Whether the minimap widget is drawn AT ALL. rts::localPlayerHasRadar()
	*     (Core/.../GameUtility.cpp:52) is isRadarForced || (!isRadarHidden && player->hasRadar()), and
	*     W3DLeftHUDDraw draws nothing whatsoever when it is false. With no Command Center,
	*     Player::hasRadar() is false and the minimap stays black no matter how deshrouded the world
	*     is. So the reveal cheat also forces the radar on for its target.
	*
	*     That force is display-only and cannot desync: Radar::crc has an empty body
	*     (Radar.cpp:1254), TheRadar is absent from GameLogic::getCRC (which covers Objects, the random
	*     seed, ThePartitionManager, ThePlayerList and TheAI), and the sole reader of isRadarForced is
	*     localPlayerHasRadar, whose every consumer is client draw/input code. The engine already calls
	*     forceOn from logic - GameLogic.cpp:1755 for observers and VictoryConditions.cpp:248 on local
	*     defeat. Because forceOn is keyed on player index, forcing the TARGET's radar changes the
	*     display only on the target's own machine and correctly changes nothing on anyone else's. */
// ------------------------------------------------------------------------------------------------
bool GameLogic::onGxCheatRevealMap(MAYBE_UNUSED GameMessage *msg)
{
	Player *target = gxCheatTarget( msg );
	if( target == nullptr || ThePartitionManager == nullptr || gxCheatArgsOk( msg, 2 ) == FALSE )
		return false;

	const Int  idx    = target->getPlayerIndex();
	const Bool reveal = msg->getArgument( 1 )->boolean;

	if( idx < 0 || idx >= MAX_PLAYER_COUNT )
		return false;

	if( s_gxRevealed[ idx ] == reveal )
		return false;								// already in that state; addLooker is a counter

	s_gxRevealed[ idx ] = reveal;

	// Gate 2 - the minimap widget. Only ever release a force we ourselves took, so that un-revealing
	// cannot switch off a radar that VictoryConditions turned on for a defeated player or that a map
	// script forced on.
	if( TheRadar != nullptr )
	{
		if( reveal )
		{
			if( TheRadar->isRadarForced( idx ) == FALSE )
			{
				s_gxRadarForced[ idx ] = TRUE;
				TheRadar->forceOn( idx, TRUE );
			}
		}
		else if( s_gxRadarForced[ idx ] )
		{
			s_gxRadarForced[ idx ] = FALSE;
			TheRadar->forceOn( idx, FALSE );
		}
	}

	// Gate 1 - the shroud grid. begin/endSetShroudLevel batches the per-cell texture pushes; without
	// it W3DRadar::setShroudLevel does Get_Surface_Level + Lock + Unlock once PER CELL, thousands of
	// times in one frame. PartitionCell::addLooker pushes to TheDisplay AND TheRadar on the shroud
	// edge transition, but only when the index matches the observed/local player, so a peer revealing
	// someone else's map correctly repaints nothing and no refreshShroudForLocalPlayer() is wanted.
	if( TheRadar != nullptr )
		TheRadar->beginSetShroudLevel();

	if( reveal )
	{
		ThePartitionManager->revealMapForPlayerPermanently( idx );
	}
	else
	{
		// Order copied verbatim from MSG_META_DEMO_ENSHROUD (LookAtXlat.cpp:653): undo the permanent
		// looker FIRST, then re-shroud, or the looker wins and nothing goes dark.
		ThePartitionManager->undoRevealMapForPlayerPermanently( idx );
		ThePartitionManager->shroudMapForPlayer( idx );
	}

	if( TheRadar != nullptr )
	{
		TheRadar->endSetShroudLevel();

		// Rebuild the blips now rather than on the next multiple of OVERLAY_REFRESH_RATE frames.
		// This reads PartitionData::getShroudedStatus, which memoises into m_shroudedness /
		// m_everSeenByPlayer and can free ghost snapshots - none of which is CRC'd
		// (PartitionManager::crc walks m_cells only, not PartitionData), and all of which the client
		// draw pass would have done a few frames later anyway.
		TheRadar->refreshObjects();
	}

	DEBUG_LOG(("GX cheat frame %d: REVEAL_MAP %d for player %d (from player %d)",
		getFrame(), (Int)reveal, idx, msg->getPlayerIndex()));

	return true;
}

// ------------------------------------------------------------------------------------------------
/** Force a player to be defeated, through exactly the path the game itself uses, so that the score
	* screen, observer mode and the remaining-player checks all happen normally.
	*
	* Mirrors GameLogic::onSelfDestruct, except the victim is an ARGUMENT rather than the sender.
	* MSG_SELF_DESTRUCT cannot simply be retargeted: NetGameCommandMsg::constructGameMessage
	* (Core/.../NetCommandMsg.cpp:157) overwrites the message's player index with the Player named
	* "player<senderNetSlot>", so it can only ever kill its own sender.
	*
	* There is no Player::setDefeated - defeat is DERIVED. VictoryConditions::update polls
	* hasSinglePlayerBeenDefeated() every frame on every peer and does all the bookkeeping itself:
	* the permanent map reveal for the dead player, the GUI:PlayerHasBeenDefeated banner, the AI slot's
	* lastFrameInGame, a belt-and-braces killPlayer(), the diplomacy popup refresh, and
	* m_localPlayerDefeated + TheRadar->forceOn for a locally defeated player. All of that comes free
	* provided the kill itself is synchronised, which is the whole point of routing it through here. */
// ------------------------------------------------------------------------------------------------
bool GameLogic::onGxCheatKillPlayer(MAYBE_UNUSED GameMessage *msg)
{
	Player *target = gxCheatTarget( msg );
	if( target == nullptr || gxCheatArgsOk( msg, 2 ) == FALSE )
		return false;
	if( target == ThePlayerList->getNeutralPlayer() )
		return false;
	if( target->isPlayerObserver() || target->isPlayerDead() )
		return false;

	if( msg->getArgument( 1 )->boolean )
	{
		// hand the assets to a living mutual ally first, exactly as onSelfDestruct does
		for( Int i = 0; i < ThePlayerList->getPlayerCount(); ++i )
		{
			if( i == target->getPlayerIndex() )
				continue;

			Player *other = ThePlayerList->getNthPlayer( i );
			if( other == nullptr )
				continue;

			if( target->getRelationship( other->getDefaultTeam() ) == ALLIES &&
					other->getRelationship( target->getDefaultTeam() ) == ALLIES )
			{
				if( TheVictoryConditions && TheVictoryConditions->hasSinglePlayerBeenDefeated( other ) )
					continue;

				other->transferAssetsFromThat( target );
				break;
			}
		}
	}

	// kill unconditionally: onSelfDestruct also calls this after a successful transfer, to take out
	// the beacons and other odds and ends that do not transfer.
	target->killPlayer();

	DEBUG_LOG(("GX cheat frame %d: KILL_PLAYER %d transferToAlly=%d (from player %d)",
		getFrame(), target->getPlayerIndex(), (Int)msg->getArgument( 1 )->boolean, msg->getPlayerIndex()));

	return true;
}

// ------------------------------------------------------------------------------------------------
/** Destroy everything a player owns, optionally including structures. Distinct from KILL_PLAYER:
	* this wipes the army without marking the player dead, though VictoryConditions will then defeat
	* them on its own if nothing survives, which is usually what you want. */
// ------------------------------------------------------------------------------------------------
bool GameLogic::onGxCheatKillObjects(MAYBE_UNUSED GameMessage *msg)
{
	Player *target = gxCheatTarget( msg );
	if( target == nullptr || gxCheatArgsOk( msg, 2 ) == FALSE )
		return false;

	const Bool includeStructures = msg->getArgument( 1 )->boolean;

	// COLLECT, THEN KILL. Never kill inside the walk: killing a transport ejects its passengers,
	// which re-teams objects and can unlink them mid-iteration. Team::killTeam does the same two-pass
	// dance and its own comment explains why. Collect IDs rather than pointers, so that a deferred
	// destroy happening between the two passes cannot hand us a dangling Object*.
	std::vector<ObjectID> doomed;
	for( Object *obj = getFirstObject(); obj != nullptr; obj = obj->getNextObject() )
	{
		if( obj->isEffectivelyDead() )
			continue;
		if( obj->getControllingPlayer() != target )
			continue;
		if( includeStructures == FALSE && obj->isKindOf( KINDOF_STRUCTURE ) )
			continue;

		doomed.push_back( obj->getID() );
	}

	for( size_t i = 0; i < doomed.size(); ++i )
	{
		Object *obj = findObjectByID( doomed[ i ] );
		if( obj != nullptr )
			obj->kill();							// NO ARGUMENTS - Generals/.../Object.h:215 takes none
	}

	DEBUG_LOG(("GX cheat frame %d: KILL_OBJECTS on player %d, %d objects, structures=%d (from player %d)",
		getFrame(), target->getPlayerIndex(), (Int)doomed.size(), (Int)includeStructures, msg->getPlayerIndex()));

	return true;
}

// ------------------------------------------------------------------------------------------------
/** Upper bound on MSG_GX_CHEAT_SPAWN_UNIT's count argument.
	*
	* The count arrives from a free-text field. Without a clamp, one fat-fingered "100000" builds a
	* hundred thousand Objects inside a single logic frame - and because this is a synchronised order
	* it happens on EVERY peer at the same instant, so the whole game locks up together and reads as a
	* network fault rather than as the typo it is. That is a self-inflicted denial of service.
	*
	* 50 is chosen as "clearly more than a full test army, clearly less than a stall": the placement
	* loop below is O(count) partition ring searches plus O(count) object constructions, all inside
	* one frame's message dispatch. Raise it only with a frame-time measurement in hand. */
// ------------------------------------------------------------------------------------------------
static const Int GX_CHEAT_MAX_SPAWN_COUNT = 50;

// ------------------------------------------------------------------------------------------------
/** Pull a position from a message argument onto the playable map.
	*
	* The caller's position came from the SENDER's camera and travelled in the message. Resolving it
	* here from TheTacticalView would give a different answer on every peer. Clamp it into the map the
	* same way onPlaceBeacon does, so a bad argument cannot drop an object off the partition grid
	* (where it would have no cell intersections, hence be permanently shrouded and unreachable). */
// ------------------------------------------------------------------------------------------------
static void gxCheatClampToMap( Coord3D *pos )
{
	if( pos == nullptr || TheTerrainLogic == nullptr )
		return;

	Region3D extent;
	TheTerrainLogic->getExtent( &extent );
	if( extent.isInRegionNoZ( pos ) == FALSE )
		*pos = TheTerrainLogic->findClosestEdgePoint( pos );

	pos->z = TheTerrainLogic->getGroundHeight( pos->x, pos->y );
}

// ------------------------------------------------------------------------------------------------
/** Spawn 'count' objects for a player, clustered around a location, at a veterancy level.
	*
	* PLACEMENT. Every unit dropped on the bare message position would be one stack of N models on one
	* point. The engine's own "spawn a group" code is ObjectCreationList's m_spreadFormation path
	* (ObjectCreationList.cpp:1386) and UnitCrateCollide::executeCrateBehavior: create the object, then
	* ThePartitionManager->findPositionAround a centre, then setPosition. This does the same, with one
	* correction.
	*
	* The correction is that findPositionAround CANNOT SEE THE SIBLINGS we placed earlier in this very
	* loop. Object::setPosition only marks the PartitionData dirty; the cells it occupies are not
	* recomputed until ThePartitionManager->UPDATE() at the end of GameLogic::update (GameLogic.cpp,
	* well after processCommandList), so iteratePotentialCollisions inside tryPosition finds nothing
	* where we just put unit 3. Calling ThePartitionManager->update() between spawns - which
	* GameLogic::startNewGame does when it lays out starting units - would fix that, but it also runs
	* processContactList and would therefore fire collide modules in the middle of command dispatch,
	* which no other message handler does.
	*
	* So the separation comes from geometry, not from collision queries: units are assigned to a
	* hexagonal lattice (ring r holds 6r slots, rings are one ringStep apart) sized from the template's
	* own bounding circle. Radial gap is 2.5 body radii and the arc gap between neighbouring slots is
	* (pi/3) * ringStep ~= 2.6 body radii, so no two slots can overlap however big the unit is.
	*
	* findPositionAround is then run per slot to VALIDATE that slot - water, cliffs, z-delta, buildings,
	* pathability - starting its search at the slot's own outward angle. In the ordinary case it costs
	* one sample and changes nothing: minRadius is 0, so its first iteration is dist == minRadius, where
	* it sets angleSpacing = TWO_PI and therefore tests exactly one point, the lattice point itself. It
	* only wanders when that point is genuinely illegal, and a unit nudged a couple of metres into a
	* neighbour beats a unit standing in a cliff. Any residual overlap is transient anyway:
	* ThePartitionManager->UPDATE() ends the same frame by running processContactList, which is the
	* engine's own mechanism for pushing coincident mobile objects apart. When the search fails outright
	* we fall back to the raw lattice point rather than skipping the unit - deterministic, and the same
	* choice the patched OCL spread path makes (ObjectCreationList.cpp:1396).
	*
	* Nothing here consumes the logic RNG: the start angle is passed explicitly, so findPositionAround
	* skips its GameLogicRandomValueReal call. That is not required for sync - the handler runs
	* identically on every peer either way - but it keeps the cheat out of the shared random stream and
	* makes the same order produce the same cluster twice.
	*
	* VETERANCY is applied unconditionally rather than only when it is above LEVEL_REGULAR: a brand new
	* Object already picks up its owner's production veterancy (Object.cpp:472 calls
	* getProductionVeterancyLevel), so "spawn me a REGULAR one" has to be able to pull a unit back DOWN
	* from whatever the player's barracks upgrades grant. provideFeedback=FALSE suppresses the promotion
	* FX and sound, matching ObjectCreationList.cpp:1016 - these units were never promoted, they were
	* born this way, and the FX path consults isLogicallyVisible(), which is per-viewer. */
// ------------------------------------------------------------------------------------------------
bool GameLogic::onGxCheatSpawnUnit(MAYBE_UNUSED GameMessage *msg)
{
	Player *target = gxCheatTarget( msg );
	if( target == nullptr || TheThingFactory == nullptr || gxCheatArgsOk( msg, 3 ) == FALSE )
		return false;

	// The wire carries the NUMERIC template id, which is what MSG_DOZER_CONSTRUCT does - there is no
	// string argument type in GameMessageArgumentDataType. Both peers must be running identical game
	// data for the id to resolve to the same template; they must already be running identical
	// binaries for the protocol to line up at all, so this adds no new constraint.
	//
	// The >0xFFFF reject matters: ids are UnsignedShort, so a nonsense argument would truncate into a
	// perfectly valid id and spawn a confidently wrong unit rather than failing.
	const Int templateArg = msg->getArgument( 1 )->integer;
	if( templateArg <= 0 || templateArg > 0xFFFF )
		return false;

	const ThingTemplate *tmpl = TheThingFactory->findByTemplateID( (UnsignedShort)templateArg );
	if( tmpl == nullptr )
		return false;

	Team *team = target->getDefaultTeam();
	if( team == nullptr )
		return false;

	Coord3D centre = msg->getArgument( 2 )->location;
	gxCheatClampToMap( &centre );

	// Arguments 3 and 4 are OPTIONAL - see the contract on MSG_GX_CHEAT_SPAWN_UNIT in
	// Common/MessageStream.h. A three-argument message keeps meaning exactly "one LEVEL_REGULAR unit",
	// so a sender that predates them still behaves the way it always did instead of being misread.
	Int count = 1;
	if( gxCheatArgsOk( msg, 4 ) )
		count = msg->getArgument( 3 )->integer;
	if( count < 1 )
		count = 1;
	if( count > GX_CHEAT_MAX_SPAWN_COUNT )
		count = GX_CHEAT_MAX_SPAWN_COUNT;

	Int vetArg = (Int)LEVEL_REGULAR;
	if( gxCheatArgsOk( msg, 5 ) )
		vetArg = msg->getArgument( 4 )->integer;
	if( vetArg < (Int)LEVEL_FIRST )
		vetArg = (Int)LEVEL_FIRST;
	if( vetArg > (Int)LEVEL_LAST )
		vetArg = (Int)LEVEL_LAST;
	const VeterancyLevel vet = (VeterancyLevel)vetArg;

	// One lattice step = 2.5 body radii. The floor keeps a template with a degenerate geometry (some
	// non-combat props declare a near-zero footprint) from collapsing the whole lattice onto a point.
	const Real bodyRadius = tmpl->getTemplateGeometryInfo().getBoundingCircleRadius();
	const Real ringStep   = ( bodyRadius > 4.0f ? bodyRadius : 4.0f ) * 2.5f;

	Int spawned = 0;
	Int ring    = 0;						// 0 = the centre slot
	Int slot    = 0;						// index within the current ring
	for( Int i = 0; i < count; ++i )
	{
		Object *obj = TheThingFactory->newObject( tmpl, team );
		if( obj == nullptr )
			break;								// out of Objects; do not keep hammering the factory

		// hex lattice: ring 0 has the single centre slot, ring r >= 1 has 6r slots
		const Int  slotsInRing = ( ring == 0 ) ? 1 : 6 * ring;
		const Real slotAngle   = TWO_PI * ( (Real)slot / (Real)slotsInRing );
		const Real slotDist    = (Real)ring * ringStep;

		Coord3D pos = centre;
		pos.x += slotDist * Cos( slotAngle );
		pos.y += slotDist * Sin( slotAngle );
		gxCheatClampToMap( &pos );

		if( ThePartitionManager != nullptr )
		{
			FindPositionOptions fpOptions;
			fpOptions.minRadius     = 0.0f;
			fpOptions.maxRadius     = ringStep;
			fpOptions.startAngle    = slotAngle;					// explicit: do not touch the logic RNG
			fpOptions.flags         = FPF_USE_HIGHEST_LAYER;	// land on the bridge, not underneath it
			fpOptions.ignoreObject  = obj;								// we are the thing being placed

			Coord3D found = pos;
			if( ThePartitionManager->findPositionAround( &pos, &fpOptions, &found ) )
				pos = found;
		}

		obj->setPosition( &pos );
		if( obj->getExperienceTracker() != nullptr )
			obj->getExperienceTracker()->setVeterancyLevel( vet, FALSE );

		++spawned;

		// advance to the next lattice slot
		if( ++slot >= slotsInRing )
		{
			slot = 0;
			++ring;
		}
	}

	DEBUG_LOG(("GX cheat frame %d: SPAWN_UNIT template %d x%d vet %d for player %d around %.1f,%.1f (from player %d)",
		getFrame(), templateArg, spawned, (Int)vet, target->getPlayerIndex(),
		centre.x, centre.y, msg->getPlayerIndex()));

	return spawned > 0;
}

// ------------------------------------------------------------------------------------------------
/** Pass-1 collector for MSG_GX_CHEAT_DELETE_OBJECTS.
	*
	* Player::iterateObjects takes a plain function pointer plus a void*, so the filter state has to
	* ride along in a struct. It COLLECTS ONLY - see the handler for why nothing may be destroyed from
	* inside the walk. */
// ------------------------------------------------------------------------------------------------
struct GxCheatDeleteContext
{
	UnsignedShort					templateID;		///< 0 == every type
	std::vector<ObjectID>	doomed;
};

static void gxCheatCollectDoomed( Object *obj, void *userData )
{
	GxCheatDeleteContext *ctx = (GxCheatDeleteContext *)userData;
	if( obj == nullptr || ctx == nullptr )
		return;

	// already on TheGameLogic's destroy list; counting it again would only inflate the log line
	if( obj->isDestroyed() )
		return;

	if( ctx->templateID != 0 )
	{
		// Compare template IDs rather than ThingTemplate pointers: getTemplate() can hand back an
		// override (ThingTemplate.h - "m_override is not used here" for getTemplateID), and the
		// override shares its parent's id, which is precisely the identity the wire argument names.
		const ThingTemplate *tmpl = obj->getTemplate();
		if( tmpl == nullptr || tmpl->getTemplateID() != ctx->templateID )
			return;
	}

	ctx->doomed.push_back( obj->getID() );
}

// ------------------------------------------------------------------------------------------------
/** Remove objects a player owns: every type or one type, all of them or the oldest few.
	*
	* Argument contract, mirrored in the enum comment in Common/MessageStream.h so the sender and this
	* handler cannot drift: templateID <= 0 means every type, count <= 0 means all of them.
	*
	* DESTROY, NOT KILL. Object::kill() runs the death sequence: death FX, the corpse/wreckage OCL, the
	* slow-death update, the "unit lost" EVA and the score bookkeeping. That is the right call for
	* MSG_GX_CHEAT_KILL_OBJECTS, which is meant to simulate an army being wiped out. This cheat exists
	* to CLEAR CLUTTER, so it wants the objects gone, not gone-and-then-some-burning-hulks;
	* TheGameLogic->destroyObject is the engine's "make it not exist" path. It is also the safer of the
	* two here: it is DEFERRED (GameLogic.cpp - the object is pushed onto m_objectsToDestroy and
	* actually deleted by processDestroyList at the end of the frame) and it early-outs on
	* obj->isDestroyed(), so naming the same object twice is harmless.
	*
	* TWO PASSES, ALWAYS. Removing objects inside Player::iterateObjects is the classic crash in this
	* codebase: the walk is over intrusive DLINK team-member lists, and taking an object out re-teams
	* its passengers and unlinks nodes the iterator is standing on. Team::killTeam does the same
	* collect-then-act dance for the same reason. Collect ObjectIDs rather than Object*, so that
	* anything freed between the passes resolves to nullptr instead of to a dangling pointer.
	*
	* WHICH ONES, when count is a limit: the collected ids are SORTED, so "delete 2 Humvees" removes the
	* two OLDEST Humvees. ObjectIDs are handed out by an incrementing counter in creation order, and
	* creation order is lockstep-identical on every peer, so this is both well-defined to the user and
	* provably the same choice everywhere. The raw iterateObjects order would also have been identical
	* on every peer, but it is essentially reverse-insertion order across an arbitrary set of team
	* prototypes - same units deleted, no way to say so in a sentence. */
// ------------------------------------------------------------------------------------------------
bool GameLogic::onGxCheatDeleteObjects(MAYBE_UNUSED GameMessage *msg)
{
	Player *target = gxCheatTarget( msg );
	if( target == nullptr || gxCheatArgsOk( msg, 3 ) == FALSE )
		return false;

	const Int templateArg = msg->getArgument( 1 )->integer;
	const Int countArg    = msg->getArgument( 2 )->integer;

	GxCheatDeleteContext ctx;
	// Template ids start at 1 (ThingFactory.cpp - "m_nextTemplateID = 1; // not zero!"), so 0 is a
	// sentinel no real template can ever collide with, and 0 is what "every type" means here.
	ctx.templateID = 0;
	if( templateArg > 0 )
	{
		// Resolve through the factory rather than just casting. Ids are UnsignedShort on the wire, so
		// a nonsense argument above 65535 would otherwise TRUNCATE into a perfectly valid id and this
		// cheat would confidently delete the wrong unit type on every peer at once. Reject anything
		// that does not resolve; "delete a type that does not exist" is a no-op, not a guess.
		if( TheThingFactory == nullptr || templateArg > 0xFFFF )
			return false;

		const ThingTemplate *tmpl = TheThingFactory->findByTemplateID( (UnsignedShort)templateArg );
		if( tmpl == nullptr )
			return false;

		ctx.templateID = tmpl->getTemplateID();
	}

	// pass 1 - collect only
	target->iterateObjects( gxCheatCollectDoomed, &ctx );

	std::sort( ctx.doomed.begin(), ctx.doomed.end() );

	size_t limit = ctx.doomed.size();
	if( countArg > 0 && (size_t)countArg < limit )
		limit = (size_t)countArg;

	// pass 2 - remove
	Int removed = 0;
	for( size_t i = 0; i < limit; ++i )
	{
		Object *obj = findObjectByID( ctx.doomed[ i ] );

		// isDestroyed() is not paranoia: destroying a transport takes its passengers with it, and
		// those passengers are on this same list. destroyObject() would early-out on them anyway -
		// the check is here so 'removed' reports what this pass actually took out.
		if( obj == nullptr || obj->isDestroyed() )
			continue;

		destroyObject( obj );
		++removed;
	}

	DEBUG_LOG(("GX cheat frame %d: DELETE_OBJECTS on player %d, template %d (0=any), asked %d (<=0=all), matched %d, removed %d (from player %d)",
		getFrame(), target->getPlayerIndex(), (Int)ctx.templateID, countArg, (Int)ctx.doomed.size(),
		removed, msg->getPlayerIndex()));

	return removed > 0;
}

bool GameLogic::onEnter(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Object *enter = findObjectByID( msg->getArgument( 1 )->objectID );

	// sanity
	if( enter == nullptr )
		return false;

	if( currentlySelectedGroup )
	{
		currentlySelectedGroup->releaseWeaponLockForGroup(LOCKED_TEMPORARILY);	// release any temporary locks.
		currentlySelectedGroup->groupEnter( enter, CMD_FROM_PLAYER );
	}

	return true;
}

bool GameLogic::onExit(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Player *msgPlayer = getMessagePlayer(msg);
	Object *objectWantingToExit = findObjectByID( msg->getArgument( 0 )->objectID );
#if RETAIL_COMPATIBLE_AIGROUP
	Object *objectContainingExiter = getSingleObjectFromSelection(currentlySelectedGroup);
#else
	Object *objectContainingExiter = getSingleObjectFromSelection(currentlySelectedGroup.Peek());
#endif

	// sanity
	if( objectWantingToExit == nullptr )
		return false;

	if( objectContainingExiter == nullptr )
		return false;

	// sanity, the player must actually control this object
	if( objectWantingToExit->getControllingPlayer() != msgPlayer )
		return false;

	objectWantingToExit->releaseWeaponLock(LOCKED_TEMPORARILY);	// release any temporary locks.

	// exit whatever objectWantingToExit is INSIDE of
	AIUpdateInterface *ai = objectWantingToExit->getAIUpdateInterface();
	if( ai )
		ai->aiExit( objectContainingExiter, CMD_FROM_PLAYER );
	// Just like Enter, Exit needs to know the thing to exit.  This can no longer be assumed because of the Tunnel system.
	// If you do not specify the thing to Exit, it will Exit the thing it thinks it is in.  For a tunnel network,
	// that will be the specific Tunnel it entered.  (Scripts can talk directly to the guy to say Get Out Regardless)

	return true;
}

bool GameLogic::onEvacuate(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	// issue command for either single object or for selected group
	//	AIGroup *group = TheAI->findGroup( *selectedGroupID );
	if( currentlySelectedGroup )
	{
		//Coord3D pos;
		//Bool hasArgs = FALSE;
		//hasArgs = (msg->getArgumentCount() > 0);

		//if (hasArgs)
		//	pos = msg->getArgument(0)->location;

		currentlySelectedGroup->releaseWeaponLockForGroup(LOCKED_TEMPORARILY);	// release any temporary locks.

		// evacuate message is for the selected group
		//if (hasArgs)
		//	currentlySelectedGroup->groupMoveToAndEvacuate( &pos, CMD_FROM_PLAYER );
		//else
		currentlySelectedGroup->groupEvacuate( CMD_FROM_PLAYER );

		// no, this is bad, don't do here, do when POSTING message
		//			pickAndPlayUnitVoiceResponse( TheInGameUI->getAllSelectedDrawables(), GameMessage::MSG_EVACUATE );
	}

	return true;
}

bool GameLogic::onExecuteRailedTransport(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	// issue command to currently selected objects
	if( currentlySelectedGroup )
		currentlySelectedGroup->groupExecuteRailedTransport( CMD_FROM_PLAYER );

	return true;
}

bool GameLogic::onInternetHack(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
//			ObjectID sourceID = msg->getArgument( 0 )->objectID;
	if( currentlySelectedGroup )
	{
		currentlySelectedGroup->releaseWeaponLockForGroup(LOCKED_TEMPORARILY);	// release any temporary locks.
		currentlySelectedGroup->groupHackInternet( CMD_FROM_PLAYER );
	}

	return true;
}

bool GameLogic::onGetRepaired(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Object *repairDepot = findObjectByID( msg->getArgument( 0 )->objectID );

	// sanity
	if( repairDepot == nullptr )
		return false;

	// tell the currently selected group to go get repaired
	if( currentlySelectedGroup )
		currentlySelectedGroup->groupGetRepaired( repairDepot, CMD_FROM_PLAYER );

	return true;
}

bool GameLogic::onDock(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Object *dockBuilding = findObjectByID( msg->getArgument( 0 )->objectID );

	// sanity
	if( dockBuilding == nullptr )
		return false;

	// tell the currently selected group to go get repaired
	if( currentlySelectedGroup )
		currentlySelectedGroup->groupDock( dockBuilding, CMD_FROM_PLAYER );

	return true;
}

bool GameLogic::onGetHealed(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Object *healDest = findObjectByID( msg->getArgument( 0 )->objectID );

	// sanity
	if( healDest == nullptr )
		return false;

	// tell the currently selected group to enter the building for healing
	if( currentlySelectedGroup )
		currentlySelectedGroup->groupGetHealed( healDest, CMD_FROM_PLAYER );

	return true;
}

bool GameLogic::onDoRepair(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Object *repairTarget = findObjectByID( msg->getArgument( 0 )->objectID );

	// sanity
	if( repairTarget == nullptr )
		return false;

	//
	// tell the currently selected group of objects to go repair the target object, note
	// that only one of them will actually go ahead and do the repair
	//
	if( currentlySelectedGroup )
		currentlySelectedGroup->groupRepair( repairTarget, CMD_FROM_PLAYER );

	return true;
}

bool GameLogic::onResumeConstruction(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Object *constructTarget = findObjectByID( msg->getArgument( 0 )->objectID );

	// sanity
	if( constructTarget == nullptr )
		return false;

	//
	// tell the currently selected group of objects to resume construction on
	// the target object, note that only one of them will go off and resume construction
	// on the target
	//
	if( currentlySelectedGroup )
		currentlySelectedGroup->groupResumeConstruction( constructTarget, CMD_FROM_PLAYER );

	// no, this is bad, don't do here, do when POSTING message
	//		pickAndPlayUnitVoiceResponse( TheInGameUI->getAllSelectedDrawables(), msg->getType() );

	return true;
}

bool GameLogic::onDoSpecialPowerOverrideDestination(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{

	const Coord3D *loc = &msg->getArgument( 0 )->location;
	SpecialPowerType spType = (SpecialPowerType)msg->getArgument( 1 )->integer;

	ObjectID sourceID = msg->getArgument(2)->objectID;
	Object* source = findObjectByID(sourceID);

	if (source != nullptr)
	{
#if !RETAIL_COMPATIBLE_CRC
		// TheSuperHackers @fix stephanmeesters 01/03/2026 Validate the origin of the source object
		Player *msgPlayer = getMessagePlayer(msg);
		if ( source->getControllingPlayer() != msgPlayer )
		{
			DEBUG_CRASH( ("MSG_DO_SPECIAL_POWER_OVERRIDE_DESTINATION: Player '%ls' attempted to control the object '%s' owned by player '%ls'.",
					msgPlayer->getPlayerDisplayName().str(),
					source->getTemplate()->getName().str(),
					source->getControllingPlayer()->getPlayerDisplayName().str()) );
			return false;
		}
#endif

		AIGroupPtr theGroup = TheAI->createGroup();
		theGroup->add(source);
		theGroup->groupOverrideSpecialPowerDestination( spType, loc, CMD_FROM_PLAYER );
#if RETAIL_COMPATIBLE_AIGROUP
		TheAI->destroyGroup(theGroup);
#else
		theGroup->removeAll();
#endif
	}
	else
	{
		if( currentlySelectedGroup )
		{
			currentlySelectedGroup->groupOverrideSpecialPowerDestination( spType, loc, CMD_FROM_PLAYER );
		}
	}

	return true;
}

bool GameLogic::onDoAttackObject(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Object *enemy = findObjectByID( msg->getArgument( 0 )->objectID );

	// Check enemy, as it is possible that he died this frame.
	if (enemy)
	{
		if (currentlySelectedGroup)
		{
			currentlySelectedGroup->releaseWeaponLockForGroup(LOCKED_TEMPORARILY);	// release any temporary locks.
			currentlySelectedGroup->groupAttackObject( enemy, NO_MAX_SHOTS_LIMIT, CMD_FROM_PLAYER );
		}
	}

	return true;
}

bool GameLogic::onDoForceAttackObject(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Object *enemy = findObjectByID( msg->getArgument( 0 )->objectID );

	// Check enemy, as it is possible that he died this frame.
	if (enemy)
	{
		if (currentlySelectedGroup)
		{
			currentlySelectedGroup->releaseWeaponLockForGroup(LOCKED_TEMPORARILY);	// release any temporary locks.
			currentlySelectedGroup->groupForceAttackObject( enemy, NO_MAX_SHOTS_LIMIT, CMD_FROM_PLAYER );
		}
	}

	return true;
}

bool GameLogic::onDoForceAttackGround(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	const Coord3D *pos = &msg->getArgument( 0 )->location;

	if (currentlySelectedGroup)
	{
		/////////////////////////////////////////////////////////////////////
		//Lorenzen sez: unclear, yet how to solve this for all cases
		//Kris: This code was added to allow the toxin tractor to force attack
		//      while contaminating. When this change was made, it was causing
		//      rangers and scud launchers to reset to primary weapon mode whenever
		//      force attacking while not idle. I fixed this by enforcing the
		//      temporary and permanent modes that are already set when attempting
		//      the new lock. In this case, the temp lock attempt will fail whenever
		//      a permanent lock is in effect, thus fixing the ranger and scud and
		//      allowing the tox tractor to work as well.
		Bool forceAttackRequiresPrimaryWeapon = !currentlySelectedGroup->isIdle();
		if ( forceAttackRequiresPrimaryWeapon )
		{
			currentlySelectedGroup->setWeaponLockForGroup( PRIMARY_WEAPON, LOCKED_TEMPORARILY );
			currentlySelectedGroup->groupAttackPosition( pos, NO_MAX_SHOTS_LIMIT, CMD_FROM_PLAYER );
			currentlySelectedGroup->releaseWeaponLockForGroup(LOCKED_TEMPORARILY);
		}
		else
		///////////////////////////////////////////////////////////////////
		{
			currentlySelectedGroup->releaseWeaponLockForGroup(LOCKED_TEMPORARILY);
			currentlySelectedGroup->groupAttackPosition( pos, NO_MAX_SHOTS_LIMIT, CMD_FROM_PLAYER );
		}
	}

	return true;
}

bool GameLogic::onQueueUpgrade(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	const UpgradeTemplate *upgradeT = TheUpgradeCenter->findUpgradeByKey( (NameKeyType)(msg->getArgument( 1 )->integer) );
	if (!upgradeT)	// sanity
		return false;

	if (currentlySelectedGroup)
		currentlySelectedGroup->queueUpgrade( upgradeT );

	return true;
}

bool GameLogic::onCancelUpgrade(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Player *msgPlayer = getMessagePlayer(msg);

#if RETAIL_COMPATIBLE_AIGROUP
	Object *producer = getSingleObjectFromSelection(currentlySelectedGroup);
#else
	Object *producer = getSingleObjectFromSelection(currentlySelectedGroup.Peek());
#endif
	const UpgradeTemplate *upgradeT = TheUpgradeCenter->findUpgradeByKey( (NameKeyType)(msg->getArgument( 0 )->integer) );

	// sanity
	if( producer == nullptr || upgradeT == nullptr )
		return false;

	// the player must actually control the producer object
	if( producer->getControllingPlayer() != msgPlayer )
		return false;

	// producer must have a production update
	ProductionUpdateInterface *pu = producer->getProductionUpdateInterface();
	if( pu == nullptr )
		return false;

	// cancel the upgrade
	pu->cancelUpgrade( upgradeT );

	return true;
}

bool GameLogic::onQueueUnitCreate(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
#if RETAIL_COMPATIBLE_AIGROUP
	Object *producer = getSingleObjectFromSelection(currentlySelectedGroup);
#else
	Object *producer = getSingleObjectFromSelection(currentlySelectedGroup.Peek());
#endif
	const ThingTemplate *whatToCreate;
	ProductionID productionID;

	// get data from the message
	whatToCreate = TheThingFactory->findByTemplateID( msg->getArgument( 0 )->integer );
	productionID = (ProductionID)msg->getArgument( 1 )->integer;

	// sanity
	if ( producer == nullptr || whatToCreate == nullptr )
		return false;

	// get the production interface for the producer
	ProductionUpdateInterface *pu = producer->getProductionUpdateInterface();
	if( pu == nullptr )
	{
		DEBUG_CRASH( ("MSG_QUEUE_UNIT_CREATE: Producer '%s' doesn't have a unit production interface",
													producer->getTemplate()->getName().str()) );
		return false;
	}

	// queue the build
	pu->queueCreateUnit( whatToCreate, productionID );

	return true;
}

bool GameLogic::onCancelUnitCreate(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Player *msgPlayer = getMessagePlayer(msg);

#if RETAIL_COMPATIBLE_AIGROUP
	Object *producer = getSingleObjectFromSelection(currentlySelectedGroup);
#else
	Object *producer = getSingleObjectFromSelection(currentlySelectedGroup.Peek());
#endif
	ProductionID productionID = (ProductionID)msg->getArgument( 0 )->integer;

	// sanity
	if( producer == nullptr )
		return false;

	// sanity, the player must control the producer
	if( producer->getControllingPlayer() != msgPlayer )
		return false;

	// get the unit production interface
	ProductionUpdateInterface *pu = producer->getProductionUpdateInterface();
	if( pu == nullptr )
		return false;

	// cancel the production
	pu->cancelUnitCreate( productionID );

	return true;
}

bool GameLogic::onDozerConstruct(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	const ThingTemplate *place;
	Coord3D loc;
	Real angle;

	// get player, what to place, and location
#if RETAIL_COMPATIBLE_AIGROUP
	Object *constructorObject = getSingleObjectFromSelection(currentlySelectedGroup);
#else
	Object *constructorObject = getSingleObjectFromSelection(currentlySelectedGroup.Peek());
#endif
	place = TheThingFactory->findByTemplateID( msg->getArgument( 0 )->integer );
	loc = msg->getArgument( 1 )->location;
	angle = msg->getArgument( 2 )->real;

	if( place == nullptr || constructorObject == nullptr )
		return false;  //These are not crashes, as the object may have died before this message came in

	if( msg->getType() == GameMessage::MSG_DOZER_CONSTRUCT )
	{
		TheBuildAssistant->buildObjectNow( constructorObject, place, &loc, angle,
																				constructorObject->getControllingPlayer() );
	}
	else
	{
		Coord3D locEnd;

		// get the end of the line location in the world
		locEnd = msg->getArgument( 3 )->location;

		// place the line of structures, the end location being present will make it happen
		TheBuildAssistant->buildObjectLineNow( constructorObject, place, &loc, &locEnd, angle,
																						constructorObject->getControllingPlayer() );
	}

	// place the sound for putting a building down

	static AudioEventRTS placeBuilding("PlaceBuilding");
	placeBuilding.setObjectID(constructorObject->getID());
	TheAudio->addAudioEvent( &placeBuilding );

	// no, this is bad, don't do here, do when POSTING message
	//		pickAndPlayUnitVoiceResponse( TheInGameUI->getAllSelectedDrawables(), msg->getType() );

	return true;
}

bool GameLogic::onDozerCancelConstruct(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Player *msgPlayer = getMessagePlayer(msg);

	// get the building to cancel construction on
#if RETAIL_COMPATIBLE_AIGROUP
	Object *building = getSingleObjectFromSelection(currentlySelectedGroup);
#else
	Object *building = getSingleObjectFromSelection(currentlySelectedGroup.Peek());
#endif
	if( building == nullptr )
		return false;

	// the player sending this message must actually control this building
	if( building->getControllingPlayer() != msgPlayer )
		return false;

	// Check to make sure it is actually under construction
	if( !building->testStatus(OBJECT_STATUS_UNDER_CONSTRUCTION) )
		return false;

	// OK, refund the money to the player, unless it is a rebuilding Hole.
	if( !building->testStatus(OBJECT_STATUS_RECONSTRUCTING))
	{
		Money *money = msgPlayer->getMoney();
		UnsignedInt amount = building->getTemplate()->calcCostToBuild( msgPlayer );
		money->deposit( amount, TRUE, FALSE );
	}

	//
	// Destroy the building ... killing the
	// building will automatically cause the dozer also cancel its own building
	// behavior and it will go on its merry way doing other assigned tasks
	//
	building->kill();

	return true;
}

bool GameLogic::onSell(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	// use the selected group
	if( currentlySelectedGroup )
		currentlySelectedGroup->groupSell( CMD_FROM_PLAYER );

	return true;
}

bool GameLogic::onToggleOvercharge(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	// use the selected group
	if( currentlySelectedGroup )
		currentlySelectedGroup->groupToggleOvercharge( CMD_FROM_PLAYER );

	return true;
}

#ifdef ALLOW_SURRENDER

bool GameLogic::onDoSurrender(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	//All selected units surrender
	if( currentlySelectedGroup )
	{
		Object* objWeSurrenderedTo = TheGameLogic->findObjectByID( msg->getArgument( 0 )->objectID );
		Bool surrender = msg->getArgument( 1 )->boolean;
		currentlySelectedGroup->groupSurrender( objWeSurrenderedTo, surrender, CMD_FROM_PLAYER );
	}

	return true;
}

bool GameLogic::onPickUpPrisoner(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	Object *prisoner = TheGameLogic->findObjectByID( msg->getArgument( 0 )->objectID );

	if( prisoner )
	{
		// use selected group
		if( currentlySelectedGroup )
			currentlySelectedGroup->groupPickUpPrisoner( prisoner, CMD_FROM_PLAYER );
	}

	return true;
}

bool GameLogic::onReturnToPrison(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	// use selected group
	if( currentlySelectedGroup )
		currentlySelectedGroup->groupReturnToPrison( nullptr, CMD_FROM_PLAYER );

	return true;
}

#endif

bool GameLogic::onCreateSelectedGroup(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);
	Bool createNewGroup = msg->getArgument( 0 )->boolean;
	Bool firstObject = TRUE;

	for (Int i = 1; i < msg->getArgumentCount(); ++i) {
		Object *obj = findObjectByID( msg->getArgument( i )->objectID );
		if (!obj) {
			continue;
		}

		selectObject(obj, createNewGroup && firstObject, msgPlayer->getPlayerMask());
		firstObject = FALSE;
	}

	return true;
}

bool GameLogic::onRemoveFromSelectedGroup(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);

	for (Int i = 0; i < msg->getArgumentCount(); ++i) {
		ObjectID objID = msg->getArgument(i)->objectID;
		Object *objToRemove = findObjectByID(objID);
		if (!objToRemove) {
			continue;
		}

		deselectObject(objToRemove, msgPlayer->getPlayerMask());
	}

	return true;
}

bool GameLogic::onDestroySelectedGroup(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);
	msgPlayer->setCurrentlySelectedAIGroup(nullptr);

	return true;
}

bool GameLogic::onPlaceBeacon(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);

	if (msgPlayer->getPlayerTemplate() == nullptr)
		return false;

	Coord3D pos = msg->getArgument( 0 )->location;
	Region3D r;
	TheTerrainLogic->getExtent(&r);
	if (!r.isInRegionNoZ(&pos))
		pos = TheTerrainLogic->findClosestEdgePoint(&pos);
	const ThingTemplate *thing = TheThingFactory->findTemplate( msgPlayer->getPlayerTemplate()->getBeaconTemplate() );

	if (thing && !TheVictoryConditions->hasSinglePlayerBeenDefeated(msgPlayer))
	{
		// how many does this player have active?
		Int count;
		msgPlayer->countObjectsByThingTemplate( 1, &thing, false, &count );
		DEBUG_LOG(("Player already has %d beacons active", count));
		if (count >= TheMultiplayerSettings->getMaxBeaconsPerPlayer())
		{
			if (msgPlayer == ThePlayerList->getLocalPlayer())
			{
				// tell the user
				TheInGameUI->message( TheGameText->fetch("GUI:TooManyBeacons") );

				// play a sound
				static AudioEventRTS aSound("BeaconPlacementFailed");
				aSound.setPosition(&pos);
				aSound.setPlayerIndex(msgPlayer->getPlayerIndex());
				TheAudio->addAudioEvent(&aSound);
			}

			return false;
		}
		Object *object = TheThingFactory->newObject( thing, msgPlayer->getDefaultTeam() );
		object->setPosition( &pos );
		object->setProducer(nullptr);

		if (msgPlayer->getRelationship( ThePlayerList->getLocalPlayer()->getDefaultTeam() ) == ALLIES || ThePlayerList->getLocalPlayer()->isPlayerObserver())
		{
			// tell the user
			UnicodeString s;
			s.format(TheGameText->fetch("GUI:BeaconPlaced"), msgPlayer->getPlayerDisplayName().str());
			TheInGameUI->message( s );

			// play a sound
			static AudioEventRTS aSound("BeaconPlaced");
			aSound.setPlayerIndex(msgPlayer->getPlayerIndex());
			aSound.setPosition(&pos);
			TheAudio->addAudioEvent(&aSound);

			// beacons are a rare event; play a nifty radar event thingy
			TheRadar->createEvent( object->getPosition(), RADAR_EVENT_INFORMATION );

			if (ThePlayerList->getLocalPlayer()->getRelationship(msgPlayer->getDefaultTeam()) == ALLIES)
				TheEva->setShouldPlay(EVA_BeaconDetected);

			TheControlBar->markUIDirty(); // check if we should grey out the button
		}
		else
		{

			Int updateCount = 0;
			static NameKeyType nameKeyClientUpdate = NAMEKEY("BeaconClientUpdate");
			ClientUpdateModule ** clientModules = object->getDrawable()->getClientUpdateModules();
			if (clientModules)
			{
				while (*clientModules)
				{
					if ((*clientModules)->getModuleNameKey() == nameKeyClientUpdate)
					{
						(*(BeaconClientUpdate **)clientModules)->hideBeacon();
						++updateCount;
					}

					++clientModules;
				}
			}
			DEBUG_ASSERTCRASH(updateCount == 1, ("Saw %d update modules for the beacon!", updateCount));

		}
	}
	else
	{
		// tell the user
		TheInGameUI->message( TheGameText->fetch("GUI:BeaconPlacementFailed") );

		// play a sound
		static AudioEventRTS aSound("BeaconPlacementFailed");
		aSound.setPosition(&pos);
		aSound.setPlayerIndex(msgPlayer->getPlayerIndex());
		TheAudio->addAudioEvent(&aSound);
	}

	return true;
}

bool GameLogic::onRemoveBeacon(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);
	AIGroupPtr allSelectedObjects = TheAI->createGroup();
#if RETAIL_COMPATIBLE_AIGROUP
	msgPlayer->getCurrentSelectionAsAIGroup(allSelectedObjects); // need to act on all objects, so we can hide teammates' beacons.
#else
	msgPlayer->getCurrentSelectionAsAIGroup(allSelectedObjects.Peek()); // need to act on all objects, so we can hide teammates' beacons.
#endif
	if( allSelectedObjects )
	{
		const VecObjectID& selectedObjects = allSelectedObjects->getAllIDs();
		for (VecObjectID::const_iterator it = selectedObjects.begin(); it != selectedObjects.end(); ++it)
		{
			Object *beacon = findObjectByID(*it);
			if (beacon)
			{
				// TheSuperHackers @bugfix Prevent runtime crashing when a beacon is no longer associated with an initialized player.
				const PlayerTemplate *playerTemplate = beacon->getControllingPlayer()->getPlayerTemplate();
				if (!playerTemplate)
					continue;

				const ThingTemplate *thing = TheThingFactory->findTemplate( playerTemplate->getBeaconTemplate() );
				if (thing && thing->isEquivalentTo(beacon->getTemplate()))
				{
					if (beacon->getControllingPlayer() == msgPlayer)
					{
						destroyObject(beacon); // the owner is telling it to go away.  such is life.

						TheControlBar->markUIDirty(); // check if we should un-grey out the button
					}
					else if (msgPlayer == ThePlayerList->getLocalPlayer())
					{
						Drawable *beaconDrawable = beacon->getDrawable();
						if (beaconDrawable)
						{

							static NameKeyType nameKeyClientUpdate = NAMEKEY("BeaconClientUpdate");
							ClientUpdateModule ** clientModules = beaconDrawable->getClientUpdateModules();
							if (clientModules)
							{
								while (*clientModules)
								{
									if ((*clientModules)->getModuleNameKey() == nameKeyClientUpdate)
										(*(BeaconClientUpdate **)clientModules)->hideBeacon();

									++clientModules;
								}
							}
						}
					}
				}
			}
		}
#if RETAIL_COMPATIBLE_AIGROUP
		if (allSelectedObjects->isEmpty())
		{
			TheAI->destroyGroup(allSelectedObjects);
			allSelectedObjects = nullptr;
		}
#endif
	}

	return true;
}

bool GameLogic::onSetBeaconText(MAYBE_UNUSED GameMessage *msg, AIGroupPtr &currentlySelectedGroup)
{
	if( currentlySelectedGroup )
	{
		const VecObjectID& selectedObjects = currentlySelectedGroup->getAllIDs();
		for (VecObjectID::const_iterator it = selectedObjects.begin(); it != selectedObjects.end(); ++it)
		{
			Object *beacon = findObjectByID(*it);
			if (beacon)
			{
				Drawable *beaconDrawable = beacon->getDrawable();
				if (beaconDrawable)
				{
					UnicodeString s;
					for( int i=0; i<msg->getArgumentCount(); i++ )
					{
						s.concat( msg->getArgument(i)->wChar );
					}

					if (s.isEmpty())
						beaconDrawable->clearCaptionText();
					else
						beaconDrawable->setCaptionText(s);
				}
			}
		}
	}

	return true;
}

bool GameLogic::onSelfDestruct(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);

	if (msg->getArgument(0)->boolean)
	{
		// transfer control to any living ally
		Int i=0;
		for (; i<ThePlayerList->getPlayerCount(); ++i)
		{
			if (i != msgPlayer->getPlayerIndex())
			{
				Player *otherPlayer = ThePlayerList->getNthPlayer(i);
				if (msgPlayer->getRelationship(otherPlayer->getDefaultTeam()) == ALLIES &&
					otherPlayer->getRelationship(msgPlayer->getDefaultTeam()) == ALLIES)
				{
					if (TheVictoryConditions->hasSinglePlayerBeenDefeated(otherPlayer))
						continue;

					// a living ally!  hooray!
					otherPlayer->transferAssetsFromThat(msgPlayer);
					msgPlayer->killPlayer(); // just to be safe (and to kill beacons etc that don't transfer)
					break;
				}
			}
		}
		if (i == ThePlayerList->getPlayerCount())
		{
			// didn't find any allies.  die, loner!
			msgPlayer->killPlayer();
		}
	}
	else
	{
		msgPlayer->killPlayer();
	}

	// There is no reason to do any notification here, it now takes place in the victory conditions.
	// bonehead.

	return true;
}

bool GameLogic::onSetReplayCamera(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);

	if (TheRecorder->isPlaybackMode() && TheGlobalData->m_useCameraInReplay && TheControlBar->getObserverLookAtPlayer() == msgPlayer)
	{
		if (TheTacticalView->isCameraMovementFinished())
		{
			const Coord3D pos = msg->getArgument( 0 )->location;
			const Real angle = msg->getArgument( 1 )->real;
			const Real pitch = msg->getArgument( 2 )->real;
			const Real zoom = msg->getArgument( 3 )->real;
			const Mouse::MouseCursor mouseCursor = static_cast<Mouse::MouseCursor>(msg->getArgument( 4 )->integer);
			const ICoord2D mousePos = msg->getArgument( 5 )->pixel;

			// TheSuperHackers @info Definitely call in user mode to ensure the camera operates with auto-zoom
			// over terrain elevations, because the Replay Camera does not store the absolute camera location,
			// but key parameters relative to the terrain height at the camera pivot.
			TheTacticalView->userSetPosition(pos);
			TheTacticalView->userSetAngle(angle);
			TheTacticalView->userSetPitch(pitch);
			TheTacticalView->userSetZoom(zoom);

			// TheSuperHackers @fix Make sure there is no scrolling ever.
			const Coord2D scroll = {0, 0};
			TheTacticalView->userScrollBy(&scroll);

			if (msg->getArgumentCount() >= 8)
			{
				// TheSuperHackers @feature Override all the settings above with real camera position and view direction.
				// This ensures that the camera looks EXACTLY like it was at the time of recording, no matter how the
				// View is configured or tweaked. Note that the above settings are still required to set regardless, because
				// when the replay camera is exited, then the pivot position and angles will be needed to build the camera
				// where it was left off.
				const Coord3D camPos = msg->getArgument( 6 )->location;
				const Coord3D camDir = msg->getArgument( 7 )->location;

				TheTacticalView->setUserControlled(false);
				TheTacticalView->set3DCameraLookAt(camPos, camDir, 0.0f);
			}

			// TheSuperHackers @fix Lock the new location to avoid user input from changing the camera in this frame.
			TheTacticalView->lockUserControlUntilFrame( getFrame() + 1 );

			if (!TheLookAtTranslator->hasMouseMovedRecently())
			{
				TheMouse->setCursor( mouseCursor );
				TheMouse->setPosition( mousePos.x, mousePos.y );
				TheLookAtTranslator->setCurrentPos( mousePos );
			}
		}
	}

	return true;
}

bool GameLogic::onCreateTeam(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);
	msgPlayer->processCreateTeamGameMessage(msg->getType() - GameMessage::MSG_CREATE_TEAM0, msg);

	return true;
}

bool GameLogic::onSelectTeam(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);
	msgPlayer->processSelectTeamGameMessage(msg->getType() - GameMessage::MSG_SELECT_TEAM0);

	return true;
}

bool GameLogic::onAddTeam(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);
	msgPlayer->processAddTeamGameMessage(msg->getType() - GameMessage::MSG_ADD_TEAM0);

	return true;
}

bool GameLogic::onLogicCrc(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);
	if (TheNetwork)
	{
		Int slotIndex = -1;
		for (Int i=0; i<MAX_SLOTS; ++i)
		{
			if (msgPlayer->getPlayerType() == PLAYER_HUMAN && TheNetwork->getPlayerName(i) == msgPlayer->getPlayerDisplayName())
			{
				slotIndex = i;
				break;
			}
		}

		if (slotIndex < 0 || !TheNetwork->isPlayerConnected(slotIndex))
			return false;

		if (msgPlayer->isLocalPlayer())
		{
#if defined(RTS_DEBUG)
			// don't even put this in release, cause someone might hack it.
			if (!TheDebugIgnoreSyncErrors)
			{
#endif
				m_shouldValidateCRCs = TRUE;
#if defined(RTS_DEBUG)
			}
#endif
		}

		UnsignedInt newCRC = msg->getArgument(0)->integer;
		//DEBUG_LOG(("Received CRC of %8.8X from %ls on frame %d", newCRC,
			//msgPlayer->getPlayerDisplayName().str(), m_frame));
		m_cachedCRCs[msgPlayer->getPlayerIndex()] = newCRC;
	}
	else if (TheRecorder && TheRecorder->isPlaybackMode())
	{
		UnsignedInt newCRC = msg->getArgument(0)->integer;
		//DEBUG_LOG(("Saw CRC of %X from player %d.  Our CRC is %X.  Arg count is %d",
			//newCRC, msgPlayer->getPlayerIndex(), getCRC(), msg->getArgumentCount()));

		TheRecorder->handleCRCMessage(newCRC, msgPlayer->getPlayerIndex(), (msg->getArgument(1)->boolean));
	}

	return true;
}

bool GameLogic::onPurchaseScience(MAYBE_UNUSED GameMessage *msg)
{
	Player *msgPlayer = getMessagePlayer(msg);
	ScienceType science = (ScienceType)msg->getArgument( 0 )->integer;

	// sanity
	if( science == SCIENCE_INVALID )
		return false;

	msgPlayer->attemptToPurchaseScience(science);

	return true;
}
