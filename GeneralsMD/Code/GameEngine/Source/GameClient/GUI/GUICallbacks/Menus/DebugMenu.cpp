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

// FILE: DebugMenu.cpp /////////////////////////////////////////////////////////////////////////////
// Desc:   Code-built Debug overlay: stats, cheats, camera tuning, config and saves
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/ArchiveFile.h"
#include "Common/ArchiveFileSystem.h"
#include "Common/FileSystem.h"
#include "Common/GameEngine.h"
#include "Common/GameState.h"
#include "Common/GlobalData.h"
#include "Common/LocalFileSystem.h"
#include "Common/Money.h"
#include "Common/NameKeyGenerator.h"
#include "Common/OptionPreferences.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/ThingFactory.h"

#include "GameClient/CodeGui.h"
#include "GameClient/DebugMenu.h"
#include "GameClient/Display.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetSlider.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GameWindowTransitions.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/InGameUI.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/Shell.h"
#include "GameClient/View.h"
#include "GameClient/WindowLayout.h"

#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/PartitionManager.h"
#include "GameLogic/TerrainLogic.h"

#include "GameNetwork/NetworkDefs.h"

//-------------------------------------------------------------------------------------------------
// Layout, in the 800x600 authoring space the stock .wnd files use. CodeGuiScale* maps it onto the
// real backbuffer; runtime-created windows get none of the parser's CREATIONRESOLUTION scaling.
//-------------------------------------------------------------------------------------------------
enum
{
	DBG_PANEL_X			=  60, DBG_PANEL_Y		=  40,
	DBG_PANEL_W			= 680, DBG_PANEL_H		= 520,
	DBG_PAD					=  12,
	DBG_TITLE_H			=  22,
	DBG_TAB_Y				=  40, DBG_TAB_H			=  26, DBG_TAB_GAP = 6,
	DBG_CONTENT_Y		=  76,
	DBG_LINE_H			=  20,
	DBG_ROW_H				=  30,
	DBG_BTN_H				=  28,
	DBG_FOOTER_BTN_W = 130, DBG_FOOTER_BTN_H = 30
};

enum
{
	DBG_TAB_STATS = 0,
	DBG_TAB_CHEATS,
	DBG_TAB_CAMERA,
	DBG_TAB_CONFIG,
	DBG_TAB_SAVES,
	DBG_TAB_COUNT
};

enum { DBG_NUM_STAT_LINES = 9 };
enum { DBG_NUM_SLIDERS = 5 };

/// Read-only config report and the pref editor share one refresh; both are capped so a fat install
/// cannot spend a second building a listbox nobody scrolls to the bottom of.
enum { DBG_MAX_REPORT_ROWS = 200 };

//-------------------------------------------------------------------------------------------------
static const WideChar *s_tabLabels[ DBG_TAB_COUNT ] =
{
	L"Stats", L"Cheats", L"Camera", L"Config", L"Saves"
};

struct DebugSliderSpec
{
	const char		 *idName;
	const WideChar *label;
	const char		 *prefKey;
	Int							minVal;
	Int							maxVal;
	Int							defVal;
};

// Ranges and pref keys are carried over verbatim from the Extras screen (ExtrasMenu.cpp), which is
// still shipped and still works; this screen is the new home for the same five settings.
static const DebugSliderSpec s_sliderSpecs[ DBG_NUM_SLIDERS ] =
{
	{ "DebugMenu:SliderMaxCameraHeight", L"Max camera height",	"MaxCameraHeight",					100, 1000, 500 },
	{ "DebugMenu:SliderMinCameraHeight", L"Min camera height",	"MinCameraHeight",					 50,  300,  80 },
	{ "DebugMenu:SliderCameraPitch",		 L"Camera pitch",				"CameraPitch",							 20,   60,  37 },
	{ "DebugMenu:SliderScrollSpeed",		 L"Scroll speed",				"ScrollFactor",							  1,  200, 100 },
	{ "DebugMenu:SliderDrawDistance",		 L"Draw distance",			"TerrainDrawDistanceScale",	100,  200, 105 }
};

//-------------------------------------------------------------------------------------------------
// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------
static WindowLayout			*s_layout			= nullptr;
static GameWindow				*s_root				= nullptr;
static GameWinDrawFunc	 s_rootDraw		= nullptr;		///< chained; see debugMenuRootDraw

static GameWindow *s_tabButton[ DBG_TAB_COUNT ]	= { nullptr };
static GameWindow *s_tabPanel [ DBG_TAB_COUNT ]	= { nullptr };
static Int				 s_activeTab									= DBG_TAB_STATS;

static GameWindow *s_buttonBack			= nullptr;

// Stats
static GameWindow *s_statLine[ DBG_NUM_STAT_LINES ] = { nullptr };

// Cheats
static GameWindow *s_cheatStatus		= nullptr;
static GameWindow *s_cheatResult		= nullptr;
static GameWindow *s_buttonGiveCash	= nullptr;
static GameWindow *s_buttonReveal		= nullptr;
static GameWindow *s_buttonSpawn		= nullptr;
static GameWindow *s_entrySpawn			= nullptr;

// Camera (the five ported Extras sliders)
static GameWindow *s_slider[ DBG_NUM_SLIDERS ]			= { nullptr };
static GameWindow *s_sliderValue[ DBG_NUM_SLIDERS ]	= { nullptr };
static GameWindow *s_buttonCamDefaults	= nullptr;
static GameWindow *s_buttonCamApply			= nullptr;

// Config
static GameWindow *s_listResolved			= nullptr;
static GameWindow *s_listPrefs				= nullptr;
static GameWindow *s_entryPrefValue		= nullptr;
static GameWindow *s_buttonCfgRefresh	= nullptr;
static GameWindow *s_buttonPrefSet		= nullptr;
static GameWindow *s_buttonPrefSave		= nullptr;

// Saves
static GameWindow *s_listSaves				= nullptr;
static GameWindow *s_entrySaveDesc		= nullptr;
static GameWindow *s_buttonSaveRefresh	= nullptr;
static GameWindow *s_buttonSaveLoad			= nullptr;
static GameWindow *s_buttonSaveHere			= nullptr;

static OptionPreferences *s_pref = nullptr;

/// Row index -> pref key. The listbox item data slot only holds a void*, and an AsciiString's
/// buffer is refcounted, so parking a raw str() pointer in there would dangle on the next refresh.
static std::vector<AsciiString> s_prefRowKeys;

static UnsignedInt s_lastRefreshMs	= 0;
static Bool				 s_configLoaded		= FALSE;		///< the resolved report is built lazily, it walks the archives

//-------------------------------------------------------------------------------------------------
// FORWARD DECLARATIONS ///////////////////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------
static WindowMsgHandledType DebugMenuSystem( GameWindow *window, UnsignedInt msg,
																						 WindowMsgData mData1, WindowMsgData mData2 );
static WindowMsgHandledType DebugMenuInput( GameWindow *window, UnsignedInt msg,
																						WindowMsgData mData1, WindowMsgData mData2 );

//-------------------------------------------------------------------------------------------------
/** Is it safe to mutate logic state from here?
	*
	* Every cheat below writes something the simulation reads on its next frame - the local player's
	* money, the shroud grid, the object list. Only this peer would run it, so in a networked game
	* the two simulations diverge immediately and the CRC check drops the connection. Rather than
	* "best effort", anything that is not a strictly local, non-replay, in-progress game is refused
	* and the buttons are disabled so the refusal is visible before the tap. */
//-------------------------------------------------------------------------------------------------
static Bool debugCheatsAllowed( void )
{
	if( TheGameLogic == nullptr || ThePlayerList == nullptr )
		return FALSE;

	// TheNetwork is non-null for the whole lifetime of a LAN/online game, including the lobby, so
	// this alone covers every case the game-mode test below might not have caught yet.
	if( TheNetwork != nullptr )
		return FALSE;

	if( TheGameLogic->isInGame() == FALSE )
		return FALSE;
	if( TheGameLogic->isInShellGame() )
		return FALSE;
	if( TheGameLogic->isInReplayGame() )
		return FALSE;
	if( TheGameLogic->isInMultiplayerGame() )
		return FALSE;

	const GameMode mode = TheGameLogic->getGameMode();
	return ( mode == GAME_SINGLE_PLAYER || mode == GAME_SKIRMISH );
}

//-------------------------------------------------------------------------------------------------
static void setCheatResult( const UnicodeString &text )
{
	if( s_cheatResult )
		GadgetStaticTextSetText( s_cheatResult, text );
}

//-------------------------------------------------------------------------------------------------
// CAMERA TAB - the ported Extras sliders /////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
static void refreshSliderValueLabel( Int i )
{
	// GadgetSliderGetPosition null-checks the SliderData but NOT the window (GadgetSlider.h), so the
	// window test has to happen out here.
	if( i < 0 || i >= DBG_NUM_SLIDERS || s_slider[i] == nullptr || s_sliderValue[i] == nullptr )
		return;

	UnicodeString txt;
	txt.format( L"%d", GadgetSliderGetPosition( s_slider[i] ) );
	GadgetStaticTextSetText( s_sliderValue[i], txt );
}

//-------------------------------------------------------------------------------------------------
static void loadSlidersFromPrefs( void )
{
	if( s_pref == nullptr )
		return;

	// Same conversions the Extras screen used: the two scale factors live as 0..2 reals in the pref
	// file and as integer percents on the slider.
	const Int values[ DBG_NUM_SLIDERS ] =
	{
		(Int)s_pref->getMaxCameraHeight(),
		(Int)s_pref->getMinCameraHeight(),
		(Int)s_pref->getCameraPitch(),
		(Int)( s_pref->getScrollFactor() * 100.0f ),
		(Int)( s_pref->getTerrainDrawDistanceScale() * 100.0f )
	};

	for( Int i = 0; i < DBG_NUM_SLIDERS; ++i )
	{
		if( s_slider[i] )
			GadgetSliderSetPosition( s_slider[i], values[i] );
		refreshSliderValueLabel( i );
	}
}

//-------------------------------------------------------------------------------------------------
/** Apply the sliders to the running game and to the preference file.
	*
	* Writing TheWritableGlobalData is what makes the change visible now; writing the pref map is what
	* survives the session. Both halves are carried over from ExtrasMenu::saveExtras(). */
//-------------------------------------------------------------------------------------------------
static void applySlidersToPrefs( void )
{
	if( s_pref == nullptr || TheWritableGlobalData == nullptr )
		return;

	for( Int i = 0; i < DBG_NUM_SLIDERS; ++i )
	{
		if( s_slider[i] == nullptr )
			continue;

		const Int val = GadgetSliderGetPosition( s_slider[i] );
		if( val <= 0 )
			continue;

		AsciiString prefString;

		switch( i )
		{
			case 0:
				TheWritableGlobalData->m_maxCameraHeight = (Real)val;
				prefString.format( "%d", val );
				break;
			case 1:
				TheWritableGlobalData->m_minCameraHeight = (Real)val;
				prefString.format( "%d", val );
				break;
			case 2:
				TheWritableGlobalData->m_cameraPitch = (Real)val;
				prefString.format( "%d", val );
				break;
			case 3:
				// ScrollFactor is stored as an integer percent; OptionPreferences::getScrollFactor
				// divides by 100 on the way back out.
				TheWritableGlobalData->m_keyboardScrollFactor = val / 100.0f;
				prefString.format( "%d", val );
				break;
			case 4:
				// GeneralsX @bugfix Claude 26/07/2026 Write the draw-distance scale as the real it is.
				// getTerrainDrawDistanceScale() atof's the string and clamps it to [1,2], so storing the
				// raw slider percent ("105") comes back as 2.0 and the slider snaps to max on reopen.
				TheWritableGlobalData->m_terrainDrawDistanceScale = val / 100.0f;
				prefString.format( "%.2f", val / 100.0f );
				break;
			default:
				continue;
		}

		(*s_pref)[ s_sliderSpecs[i].prefKey ] = prefString;
	}

	s_pref->write();
}

//-------------------------------------------------------------------------------------------------
static void setSliderDefaults( void )
{
	for( Int i = 0; i < DBG_NUM_SLIDERS; ++i )
	{
		if( s_slider[i] )
			GadgetSliderSetPosition( s_slider[i], s_sliderSpecs[i].defVal );
		refreshSliderValueLabel( i );
	}
}

//-------------------------------------------------------------------------------------------------
// CONFIG TAB /////////////////////////////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------

struct DebugProbeDir
{
	const char *directory;
	const char *mask;
};

// ArchiveFileSystem keeps m_archiveFileMap protected and exposes no way to enumerate the archives it
// loaded, so the loaded set is recovered by asking which archive actually provides a file. These
// four directories are the ones that matter when a screen or a rule misbehaves, and they are the
// only ones small enough to walk interactively: the override test below stats the local disk once
// per file, so a probe over Art\W3D (10k+ entries) would stall the frame for the better part of a
// second. They cover INI*.big, Window*.big, Maps*.big, the text archives, and every Patch*.big that
// shadows one of them.
static const DebugProbeDir s_probeDirs[] =
{
	{ "Data\\INI\\",	"*.ini" },
	{ "Window\\",			"*.wnd" },
	{ "Maps\\",				"*.map" },
	{ "Data\\",				"*.csf" }
};
enum { DBG_NUM_PROBE_DIRS = sizeof( s_probeDirs ) / sizeof( s_probeDirs[0] ) };

//-------------------------------------------------------------------------------------------------
static void addReportLine( Int &rows, const UnicodeString &text, Color color )
{
	if( s_listResolved == nullptr || rows >= DBG_MAX_REPORT_ROWS )
		return;

	GadgetListBoxAddEntryText( s_listResolved, text, color, -1 );
	++rows;
}

//-------------------------------------------------------------------------------------------------
/** Build the read-only "what actually resolved" report.
	*
	* Two questions, one walk. Which archives are live (an archive is live if it wins at least one
	* probed file), and what shadows what: FileSystem::doesFileExist consults TheLocalFileSystem
	* first, so a loose file on disk takes instance 0 and pushes every archive copy down one; below
	* that, instance N of getArchiveFile() is the Nth archive providing the same path, in the order
	* the multimap holds them. Anything with more than one provider is an override. */
//-------------------------------------------------------------------------------------------------
static void refreshResolvedConfig( void )
{
	if( s_listResolved == nullptr )
		return;

	GadgetListBoxReset( s_listResolved );

	const CodeGuiTheme &theme = CodeGuiGetTheme();
	Int rows = 0;

	if( TheArchiveFileSystem == nullptr )
	{
		addReportLine( rows, UnicodeString( L"TheArchiveFileSystem is not up yet." ), theme.textNormal );
		s_configLoaded = TRUE;
		return;
	}

	typedef std::map<AsciiString, Int> ArchiveHitMap;
	ArchiveHitMap winners;						// archive name -> number of probed files it wins
	std::vector<UnicodeString> overrides;
	Int probedFiles = 0;

	for( Int d = 0; d < DBG_NUM_PROBE_DIRS; ++d )
	{
		FilenameList files;
		TheArchiveFileSystem->getFileListInDirectory( AsciiString::TheEmptyString,
																									AsciiString( s_probeDirs[d].directory ),
																									AsciiString( s_probeDirs[d].mask ),
																									files, TRUE );

		for( FilenameListIter it = files.begin(); it != files.end(); ++it )
		{
			const AsciiString &path = *it;
			++probedFiles;

			// Instance 0 is the winner as far as the archives are concerned; a loose file on disk beats
			// all of them and is reported separately because it is the single most confusing override.
			ArchiveFile *winner = TheArchiveFileSystem->getArchiveFile( path, 0 );
			if( winner == nullptr )
				continue;

			winners[ winner->getName() ]++;

			const Bool loose = ( TheLocalFileSystem != nullptr &&
													 TheLocalFileSystem->doesFileExist( path.str() ) );
			ArchiveFile *shadowed = TheArchiveFileSystem->getArchiveFile( path, 1 );

			if( loose == FALSE && shadowed == nullptr )
				continue;
			if( (Int)overrides.size() >= DBG_MAX_REPORT_ROWS )
				continue;

			UnicodeString line;
			if( loose )
				line.format( L"%S  <-  loose file on disk  (over %S)", path.str(), winner->getName().str() );
			else
				line.format( L"%S  <-  %S  (over %S)", path.str(), winner->getName().str(),
										 shadowed->getName().str() );
			overrides.push_back( line );
		}
	}

	UnicodeString line;

	line.format( L"ARCHIVES PROVIDING CONTENT  (%d probed files)", probedFiles );
	addReportLine( rows, line, theme.accent );

	for( ArchiveHitMap::const_iterator a = winners.begin(); a != winners.end(); ++a )
	{
		line.format( L"  %S   %d file(s) win", a->first.str(), a->second );
		addReportLine( rows, line, theme.textNormal );
	}

	line.format( L"OVERRIDES  (%d)", (Int)overrides.size() );
	addReportLine( rows, line, theme.accent );

	if( overrides.empty() )
		addReportLine( rows, UnicodeString( L"  nothing is being shadowed" ), theme.textNormal );

	for( size_t o = 0; o < overrides.size(); ++o )
		addReportLine( rows, overrides[o], theme.textNormal );

	// The handful of resolved values this screen can actually change, so the read-only pane and the
	// editable pane can be compared side by side.
	if( TheGlobalData )
	{
		addReportLine( rows, UnicodeString( L"RESOLVED CAMERA / TERRAIN VALUES" ), theme.accent );

		line.format( L"  MaxCameraHeight            %.1f", TheGlobalData->m_maxCameraHeight );
		addReportLine( rows, line, theme.textNormal );
		line.format( L"  MinCameraHeight            %.1f", TheGlobalData->m_minCameraHeight );
		addReportLine( rows, line, theme.textNormal );
		line.format( L"  CameraPitch                %.1f", TheGlobalData->m_cameraPitch );
		addReportLine( rows, line, theme.textNormal );
		line.format( L"  KeyboardScrollFactor       %.2f", TheGlobalData->m_keyboardScrollFactor );
		addReportLine( rows, line, theme.textNormal );
		line.format( L"  TerrainDrawDistanceScale   %.2f", TheGlobalData->m_terrainDrawDistanceScale );
		addReportLine( rows, line, theme.textNormal );
	}

	s_configLoaded = TRUE;
}

//-------------------------------------------------------------------------------------------------
/** Fill the editable pane from the live preference map.
	*
	* OptionPreferences IS a std::map<AsciiString,AsciiString> (UserPreferences derives from it), so
	* this is the real resolved user config, not a curated copy of it. */
//-------------------------------------------------------------------------------------------------
static void refreshPrefList( void )
{
	if( s_listPrefs == nullptr )
		return;

	GadgetListBoxReset( s_listPrefs );
	s_prefRowKeys.clear();

	if( s_pref == nullptr )
		return;

	const CodeGuiTheme &theme = CodeGuiGetTheme();

	for( OptionPreferences::const_iterator it = s_pref->begin(); it != s_pref->end(); ++it )
	{
		UnicodeString line;
		line.format( L"%S = %S", it->first.str(), it->second.str() );

		const Int row = GadgetListBoxAddEntryText( s_listPrefs, line, theme.textNormal, -1 );
		if( row < 0 )
			break;		// listbox is full; listLength is a hard cap

		s_prefRowKeys.push_back( it->first );
	}
}

//-------------------------------------------------------------------------------------------------
static void onPrefRowSelected( void )
{
	if( s_listPrefs == nullptr || s_entryPrefValue == nullptr || s_pref == nullptr )
		return;

	Int sel = -1;
	GadgetListBoxGetSelected( s_listPrefs, &sel );
	if( sel < 0 || sel >= (Int)s_prefRowKeys.size() )
		return;

	UnicodeString value;
	value.translate( (*s_pref)[ s_prefRowKeys[sel] ] );
	GadgetTextEntrySetText( s_entryPrefValue, value );
}

//-------------------------------------------------------------------------------------------------
static void applyPrefEdit( void )
{
	if( s_listPrefs == nullptr || s_entryPrefValue == nullptr || s_pref == nullptr )
		return;

	Int sel = -1;
	GadgetListBoxGetSelected( s_listPrefs, &sel );
	if( sel < 0 || sel >= (Int)s_prefRowKeys.size() )
		return;

	const AsciiString key = s_prefRowKeys[sel];

	AsciiString value;
	value.translate( GadgetTextEntryGetText( s_entryPrefValue ) );
	(*s_pref)[ key ] = value;

	// Keep the two panes honest with each other: the row text is stale the instant the map changes,
	// and the sliders share four of these keys.
	refreshPrefList();
	if( sel < (Int)s_prefRowKeys.size() )
		GadgetListBoxSetSelected( s_listPrefs, sel );
	loadSlidersFromPrefs();
}

//-------------------------------------------------------------------------------------------------
// SAVES TAB //////////////////////////////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
static void refreshSaveList( void )
{
	if( s_listSaves == nullptr || TheGameState == nullptr )
		return;

	// SLLT_LOAD_ONLY so the list is purely the files on disk; the "new save game" pseudo-row the
	// save layouts prepend carries null item data and would just be a dead row here.
	TheGameState->populateSaveGameListbox( s_listSaves, SLLT_LOAD_ONLY );
}

//-------------------------------------------------------------------------------------------------
/** Load the selected save.
	*
	* Mirrors PopupSaveLoad::doLoadGame(), including the two different preambles: from the shell the
	* logic has to be told a single-player game is starting, from inside a game the quit menu has to
	* go away first. The overlay is torn down before the load because loadGame resets the engine. */
//-------------------------------------------------------------------------------------------------
static void doLoadSelectedSave( void )
{
	if( s_listSaves == nullptr || TheGameState == nullptr || TheGameLogic == nullptr )
		return;

	Int sel = -1;
	GadgetListBoxGetSelected( s_listSaves, &sel );
	if( sel < 0 )
		return;

	AvailableGameInfo *info = (AvailableGameInfo *)GadgetListBoxGetItemData( s_listSaves, sel );
	if( info == nullptr )
		return;

	// Copy before the teardown: the item data points into GameState's available-games list, which
	// clearAvailableGames() frees, and loadGame takes it by value for exactly that reason.
	AvailableGameInfo gameInfo = *info;

	DebugMenuClose();

	if( TheShell != nullptr && TheShell->isShellActive() == FALSE )
	{
		destroyQuitMenu();
	}
	else
	{
		if( TheTransitionHandler )
		{
			TheTransitionHandler->remove( "MainMenuLoadReplayMenu" );
			TheTransitionHandler->remove( "MainMenuLoadReplayMenuBack" );
		}
		TheGameLogic->prepareNewGame( GAME_SINGLE_PLAYER, DIFFICULTY_NORMAL, 0 );
	}

	if( TheGameState->loadGame( gameInfo ) != SC_OK )
	{
		if( TheGameLogic->isInGame() )
			TheGameLogic->clearGameData( FALSE );
		TheGameEngine->reset();
		TheShell->showShell( TRUE );
	}
}

//-------------------------------------------------------------------------------------------------
/** Save-anywhere.
	*
	* GameState::saveGame() itself has no notion of "you may only save between missions" - that rule
	* lives entirely in the PopupSaveLoad UI, which only ever offers SLLT_SAVE_ONLY at a mission
	* boundary. Passing an empty filename makes it mint the next Save000N.sav itself. */
//-------------------------------------------------------------------------------------------------
static void doSaveHere( void )
{
	if( TheGameState == nullptr || TheGameLogic == nullptr )
		return;

	if( TheGameLogic->isInGame() == FALSE || TheGameLogic->isInShellGame() ||
			TheGameLogic->isInReplayGame() )
		return;

	UnicodeString desc;
	if( s_entrySaveDesc )
		desc = GadgetTextEntryGetText( s_entrySaveDesc );
	if( desc.isEmpty() )
		desc = UnicodeString( L"Debug save" );

	TheGameState->saveGame( AsciiString::TheEmptyString, desc, SAVE_FILE_TYPE_NORMAL );
	refreshSaveList();
}

//-------------------------------------------------------------------------------------------------
// CHEATS /////////////////////////////////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
static void doGiveCash( void )
{
	if( debugCheatsAllowed() == FALSE )
		return;

	Player *player = ThePlayerList->getLocalPlayer();
	if( player == nullptr )
		return;

	Money *money = player->getMoney();
	if( money == nullptr )
		return;

	money->deposit( 10000 );
	setCheatResult( L"Deposited $10,000." );
}

//-------------------------------------------------------------------------------------------------
static void doRevealMap( void )
{
	if( debugCheatsAllowed() == FALSE || ThePartitionManager == nullptr )
		return;

	Player *player = ThePlayerList->getLocalPlayer();
	if( player == nullptr )
		return;

	ThePartitionManager->revealMapForPlayerPermanently( player->getPlayerIndex() );
	setCheatResult( L"Map revealed for the local player." );
}

//-------------------------------------------------------------------------------------------------
/** Spawn the named template on the local player's default team, at the camera's look-at point. */
//-------------------------------------------------------------------------------------------------
static void doSpawnUnit( void )
{
	if( debugCheatsAllowed() == FALSE || TheThingFactory == nullptr || TheTacticalView == nullptr )
		return;

	if( s_entrySpawn == nullptr )
		return;

	AsciiString name;
	name.translate( GadgetTextEntryGetText( s_entrySpawn ) );
	name.trim();
	if( name.isEmpty() )
	{
		setCheatResult( L"Type a template name first, e.g. AmericaVehicleHumvee." );
		return;
	}

	// check == FALSE: findTemplate asserts on a miss otherwise, and a typo in a debug field is an
	// expected outcome, not a bug.
	const ThingTemplate *tmpl = TheThingFactory->findTemplate( name, FALSE );
	if( tmpl == nullptr )
	{
		UnicodeString msg;
		msg.format( L"No such template: %S", name.str() );
		setCheatResult( msg );
		return;
	}

	Player *player = ThePlayerList->getLocalPlayer();
	if( player == nullptr )
		return;

	Team *team = player->getDefaultTeam();
	if( team == nullptr )
	{
		setCheatResult( L"Local player has no default team." );
		return;
	}

	Object *obj = TheThingFactory->newObject( tmpl, team );
	if( obj == nullptr )
	{
		setCheatResult( L"newObject failed." );
		return;
	}

	Coord3D pos = TheTacticalView->getPosition();
	if( TheTerrainLogic )
		pos.z = TheTerrainLogic->getGroundHeight( pos.x, pos.y );
	obj->setPosition( &pos );

	UnicodeString msg;
	msg.format( L"Spawned %S at %.0f, %.0f", name.str(), pos.x, pos.y );
	setCheatResult( msg );
}

//-------------------------------------------------------------------------------------------------
// LIVE READOUTS //////////////////////////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
static void setStatLine( Int i, const UnicodeString &text )
{
	if( i >= 0 && i < DBG_NUM_STAT_LINES && s_statLine[i] )
		GadgetStaticTextSetText( s_statLine[i], text );
}

//-------------------------------------------------------------------------------------------------
void DebugMenuUpdate( void )
{
	if( s_layout == nullptr || s_root == nullptr )
		return;

	// Four refreshes a second. Walking every object in the world once per rendered frame would be a
	// measurable cost in exactly the situation the readout exists to diagnose.
	const UnsignedInt now = timeGetTime();
	if( s_lastRefreshMs != 0 && ( now - s_lastRefreshMs ) < 250 )
		return;
	s_lastRefreshMs = now;

	// Cheats can become legal or illegal without the screen closing (start a skirmish with the
	// overlay up), so the gate is re-evaluated here rather than once at build time.
	const Bool cheats = debugCheatsAllowed();
	if( s_buttonGiveCash )	s_buttonGiveCash->winEnable( cheats );
	if( s_buttonReveal )		s_buttonReveal->winEnable( cheats );
	if( s_buttonSpawn )			s_buttonSpawn->winEnable( cheats );
	if( s_cheatStatus )
	{
		GadgetStaticTextSetText( s_cheatStatus, cheats
			? UnicodeString( L"Enabled: local single-player game." )
			: UnicodeString( L"Disabled: cheats desync a networked game and are refused outside a local match." ) );
	}

	if( s_buttonSaveHere )
	{
		const Bool canSave = ( TheGameLogic != nullptr && TheGameLogic->isInGame() &&
													 TheGameLogic->isInShellGame() == FALSE &&
													 TheGameLogic->isInReplayGame() == FALSE );
		s_buttonSaveHere->winEnable( canSave );
	}

	if( s_activeTab != DBG_TAB_STATS )
		return;

	UnicodeString line;
	Int n = 0;

	line.format( L"Render FPS            %.1f", TheDisplay ? TheDisplay->getAverageFPS() : 0.0f );
	setStatLine( n++, line );

	line.format( L"Display               %d x %d", CodeGuiScreenW(), CodeGuiScreenH() );
	setStatLine( n++, line );

	if( TheGameLogic == nullptr )
	{
		for( ; n < DBG_NUM_STAT_LINES; ++n )
			setStatLine( n, UnicodeString::TheEmptyString );
		return;
	}

	line.format( L"Game mode             %S", toString( TheGameLogic->getGameMode() ) );
	setStatLine( n++, line );

	line.format( L"Logic frame           %u", TheGameLogic->getFrame() );
	setStatLine( n++, line );

	Int total = 0, myUnits = 0, myStructures = 0, otherUnits = 0, dead = 0;
	const Player *local = ( ThePlayerList != nullptr ) ? ThePlayerList->getLocalPlayer() : nullptr;

	// Skip the walk while the object list is being built or torn down. This runs from the draw pass,
	// so it cannot land in the middle of a logic update, but a load or a clearGameData spans frames.
	const Bool objectListStable = TheGameLogic->isInGame() &&
																TheGameLogic->isLoadingMap() == FALSE &&
																TheGameLogic->isLoadingSave() == FALSE &&
																TheGameLogic->isClearingGameData() == FALSE;

	for( Object *obj = objectListStable ? TheGameLogic->getFirstObject() : nullptr;
			 obj; obj = obj->getNextObject() )
	{
		++total;

		if( obj->isEffectivelyDead() )
		{
			++dead;
			continue;
		}

		const Bool isStructure = obj->isKindOf( KINDOF_STRUCTURE );
		if( local != nullptr && obj->getControllingPlayer() == local )
		{
			if( isStructure )
				++myStructures;
			else
				++myUnits;
		}
		else if( isStructure == FALSE )
		{
			++otherUnits;
		}
	}

	line.format( L"Objects in world      %d  (%d effectively dead)", total, dead );
	setStatLine( n++, line );
	line.format( L"Your units            %d", myUnits );
	setStatLine( n++, line );
	line.format( L"Your structures       %d", myStructures );
	setStatLine( n++, line );
	line.format( L"Other players' units  %d", otherUnits );
	setStatLine( n++, line );

	line.format( L"Players               %d", ( ThePlayerList != nullptr )
																								? ThePlayerList->getPlayerCount() : 0 );
	setStatLine( n++, line );

	for( ; n < DBG_NUM_STAT_LINES; ++n )
		setStatLine( n, UnicodeString::TheEmptyString );
}

//-------------------------------------------------------------------------------------------------
/** Root draw func.
	*
	* The overlay is off the shell stack on purpose, so nothing in Shell::update ticks it. drawWindow
	* paints a window before its children, so refreshing the labels from here lands in the same frame
	* they are drawn, and the original default draw is chained rather than replaced. */
//-------------------------------------------------------------------------------------------------
static void debugMenuRootDraw( GameWindow *window, WinInstanceData *instData )
{
	if( s_rootDraw )
		s_rootDraw( window, instData );

	DebugMenuUpdate();
}

//-------------------------------------------------------------------------------------------------
// TAB SWITCHING //////////////////////////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
static void selectTab( Int tab )
{
	if( tab < 0 || tab >= DBG_TAB_COUNT )
		return;

	s_activeTab = tab;

	for( Int i = 0; i < DBG_TAB_COUNT; ++i )
	{
		if( s_tabPanel[i] )
			s_tabPanel[i]->winHide( i != tab );

		// The tab button for the visible pane is disabled so it reads as "you are here". Colour alone
		// would not survive the hilite state a touch leaves behind.
		if( s_tabButton[i] )
			s_tabButton[i]->winEnable( i != tab );
	}

	// The archive walk is not cheap, so it happens the first time the pane is actually looked at.
	if( tab == DBG_TAB_CONFIG && s_configLoaded == FALSE )
	{
		refreshResolvedConfig();
		refreshPrefList();
	}
	else if( tab == DBG_TAB_SAVES )
	{
		refreshSaveList();
	}

	// Hiding a pane that contained the focused text entry clears m_keyboardFocus outright
	// (GameWindowManager::windowHiding), which would silently kill the ESC handler. Take it back.
	if( TheWindowManager && s_root )
		TheWindowManager->winSetFocus( s_root );
}

//-------------------------------------------------------------------------------------------------
// CONSTRUCTION ///////////////////////////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
static void nullifyControls( void )
{
	s_root					= nullptr;
	s_rootDraw			= nullptr;
	s_buttonBack		= nullptr;

	for( Int i = 0; i < DBG_TAB_COUNT; ++i )
	{
		s_tabButton[i]	= nullptr;
		s_tabPanel[i]		= nullptr;
	}
	for( Int i = 0; i < DBG_NUM_STAT_LINES; ++i )
		s_statLine[i] = nullptr;
	for( Int i = 0; i < DBG_NUM_SLIDERS; ++i )
	{
		s_slider[i]				= nullptr;
		s_sliderValue[i]	= nullptr;
	}

	s_cheatStatus					= nullptr;
	s_cheatResult					= nullptr;
	s_buttonGiveCash			= nullptr;
	s_buttonReveal				= nullptr;
	s_buttonSpawn					= nullptr;
	s_entrySpawn					= nullptr;
	s_buttonCamDefaults		= nullptr;
	s_buttonCamApply			= nullptr;
	s_listResolved				= nullptr;
	s_listPrefs						= nullptr;
	s_entryPrefValue			= nullptr;
	s_buttonCfgRefresh		= nullptr;
	s_buttonPrefSet				= nullptr;
	s_buttonPrefSave			= nullptr;
	s_listSaves						= nullptr;
	s_entrySaveDesc				= nullptr;
	s_buttonSaveRefresh		= nullptr;
	s_buttonSaveLoad			= nullptr;
	s_buttonSaveHere			= nullptr;

	s_prefRowKeys.clear();
	s_configLoaded	= FALSE;
	s_lastRefreshMs	= 0;
}

//-------------------------------------------------------------------------------------------------
static void buildStatsTab( GameWindow *pane, Int paneW )
{
	const Int pad		= CodeGuiScaleX( DBG_PAD );
	const Int lineH	= CodeGuiAtLeast( CodeGuiScaleY( DBG_LINE_H ), 14 );
	const Int step	= CodeGuiAtLeast( CodeGuiScaleY( 22 ), 15 );

	for( Int i = 0; i < DBG_NUM_STAT_LINES; ++i )
	{
		s_statLine[i] = CodeGuiLabel( pane, pad, i * step, paneW - pad * 2, lineH,
																	nullptr, L"", FALSE, 12 );
	}
}

//-------------------------------------------------------------------------------------------------
static void buildCheatsTab( GameWindow *pane, Int paneW )
{
	const Int pad			= CodeGuiScaleX( DBG_PAD );
	const Int lineH		= CodeGuiAtLeast( CodeGuiScaleY( DBG_LINE_H ), 14 );
	const Int btnH		= CodeGuiAtLeast( CodeGuiScaleY( DBG_BTN_H ), 26 );
	const Int fieldH	= CodeGuiAtLeast( CodeGuiScaleY( 22 ), 22 );
	const Int innerW	= paneW - pad * 2;

	Int y = 0;
	s_cheatStatus = CodeGuiLabel( pane, pad, y, innerW, lineH, nullptr, L"", FALSE, 12 );

	y += CodeGuiScaleY( 28 );
	s_buttonGiveCash = CodeGuiButton( pane, pad, y, CodeGuiScaleX( 170 ), btnH,
																		"DebugMenu:ButtonGiveCash", L"Give $10,000",
																		L"Deposit 10000 into the local player's account" );
	s_buttonReveal = CodeGuiButton( pane, pad + CodeGuiScaleX( 180 ), y, CodeGuiScaleX( 170 ), btnH,
																	"DebugMenu:ButtonRevealMap", L"Reveal map",
																	L"Permanently reveal the shroud for the local player" );

	y += CodeGuiScaleY( 40 );
	CodeGuiLabel( pane, pad, y + CodeGuiScaleY( 2 ), CodeGuiScaleX( 130 ), lineH,
								nullptr, L"Unit template", FALSE, 12 );
	s_entrySpawn = CodeGuiTextEntry( pane, pad + CodeGuiScaleX( 140 ), y, CodeGuiScaleX( 300 ), fieldH,
																	 "DebugMenu:EntrySpawnTemplate", 64 );
	s_buttonSpawn = CodeGuiButton( pane, pad + CodeGuiScaleX( 460 ), y - CodeGuiScaleY( 3 ),
																 CodeGuiScaleX( 130 ), btnH,
																 "DebugMenu:ButtonSpawnUnit", L"Spawn",
																 L"Create the template on your team at the camera's centre" );

	y += CodeGuiScaleY( 36 );
	CodeGuiLabel( pane, pad, y, innerW, lineH, nullptr,
								L"Template names are the INI Object entries, e.g. AmericaVehicleHumvee.", FALSE, 12 );

	y += CodeGuiScaleY( 26 );
	s_cheatResult = CodeGuiLabel( pane, pad, y, innerW, lineH, nullptr, L"", FALSE, 12 );
}

//-------------------------------------------------------------------------------------------------
static void buildCameraTab( GameWindow *pane, Int paneW )
{
	const Int pad			= CodeGuiScaleX( DBG_PAD );
	const Int lineH		= CodeGuiAtLeast( CodeGuiScaleY( DBG_LINE_H ), 14 );
	const Int rowH		= CodeGuiAtLeast( CodeGuiScaleY( DBG_ROW_H ), 20 );
	const Int labelW	= CodeGuiScaleX( 200 );
	const Int sliderW	= CodeGuiScaleX( 260 );
	const Int valueW	= CodeGuiScaleX( 70 );
	const Int btnH		= CodeGuiAtLeast( CodeGuiScaleY( DBG_BTN_H ), 26 );

	Int y = 0;
	for( Int i = 0; i < DBG_NUM_SLIDERS; ++i )
	{
		CodeGuiLabel( pane, pad, y, labelW, lineH, nullptr, s_sliderSpecs[i].label, FALSE, 12 );

		s_slider[i] = CodeGuiHSlider( pane, pad + labelW, y, sliderW, lineH,
																	s_sliderSpecs[i].idName,
																	s_sliderSpecs[i].minVal, s_sliderSpecs[i].maxVal );

		s_sliderValue[i] = CodeGuiLabel( pane, pad + labelW + sliderW + CodeGuiScaleX( 12 ), y,
																		 valueW, lineH, nullptr, L"0", FALSE, 12 );
		y += rowH;
	}

	y += CodeGuiScaleY( 12 );
	s_buttonCamDefaults = CodeGuiButton( pane, pad, y, CodeGuiScaleX( 130 ), btnH,
																			 "DebugMenu:ButtonCameraDefaults", L"Defaults",
																			 L"Restore the shipped values" );
	s_buttonCamApply = CodeGuiButton( pane, pad + CodeGuiScaleX( 140 ), y, CodeGuiScaleX( 130 ), btnH,
																		"DebugMenu:ButtonCameraApply", L"Apply",
																		L"Apply now and write to Options.ini" );

	y += CodeGuiScaleY( 36 );
	CodeGuiLabel( pane, pad, y, paneW - pad * 2, lineH, nullptr,
								L"Scroll speed and draw distance are percentages.", FALSE, 12 );
}

//-------------------------------------------------------------------------------------------------
static void buildConfigTab( GameWindow *pane, Int paneW )
{
	const Int pad			= CodeGuiScaleX( DBG_PAD );
	const Int lineH		= CodeGuiAtLeast( CodeGuiScaleY( DBG_LINE_H ), 14 );
	const Int btnH		= CodeGuiAtLeast( CodeGuiScaleY( 26 ), 24 );
	const Int fieldH	= CodeGuiAtLeast( CodeGuiScaleY( 22 ), 22 );
	const Int innerW	= paneW - pad * 2;

	Int y = 0;
	CodeGuiLabel( pane, pad, y, innerW, lineH, nullptr,
								L"Resolved (read-only): loaded archives and what shadows what", FALSE, 12 );

	y += CodeGuiScaleY( 22 );
	s_listResolved = CodeGuiListbox( pane, pad, y, innerW, CodeGuiScaleY( 140 ),
																	 "DebugMenu:ListResolved", DBG_MAX_REPORT_ROWS, FALSE, TRUE, 10 );

	y += CodeGuiScaleY( 146 );
	s_buttonCfgRefresh = CodeGuiButton( pane, pad, y, CodeGuiScaleX( 130 ), btnH,
																			"DebugMenu:ButtonConfigRefresh", L"Rescan",
																			L"Walk the archives again" );

	y += CodeGuiScaleY( 34 );
	CodeGuiLabel( pane, pad, y, innerW, lineH, nullptr,
								L"User preferences (editable): pick a row, edit, Set, then Save", FALSE, 12 );

	y += CodeGuiScaleY( 22 );
	s_listPrefs = CodeGuiListbox( pane, pad, y, innerW, CodeGuiScaleY( 100 ),
																"DebugMenu:ListPrefs", 256, FALSE, TRUE, 10 );

	y += CodeGuiScaleY( 106 );
	s_entryPrefValue = CodeGuiTextEntry( pane, pad, y, CodeGuiScaleX( 300 ), fieldH,
																			 "DebugMenu:EntryPrefValue", 128 );
	s_buttonPrefSet = CodeGuiButton( pane, pad + CodeGuiScaleX( 310 ), y - CodeGuiScaleY( 2 ),
																	 CodeGuiScaleX( 100 ), btnH,
																	 "DebugMenu:ButtonPrefSet", L"Set",
																	 L"Write the value into the live preference map" );
	s_buttonPrefSave = CodeGuiButton( pane, pad + CodeGuiScaleX( 420 ), y - CodeGuiScaleY( 2 ),
																		CodeGuiScaleX( 100 ), btnH,
																		"DebugMenu:ButtonPrefSave", L"Save",
																		L"Write Options.ini to disk" );
}

//-------------------------------------------------------------------------------------------------
static void buildSavesTab( GameWindow *pane, Int paneW )
{
	const Int pad			= CodeGuiScaleX( DBG_PAD );
	const Int lineH		= CodeGuiAtLeast( CodeGuiScaleY( DBG_LINE_H ), 14 );
	const Int btnH		= CodeGuiAtLeast( CodeGuiScaleY( DBG_BTN_H ), 26 );
	const Int fieldH	= CodeGuiAtLeast( CodeGuiScaleY( 22 ), 22 );
	const Int innerW	= paneW - pad * 2;

	Int y = 0;
	CodeGuiLabel( pane, pad, y, innerW, lineH, nullptr, L"Save games", FALSE, 12 );

	y += CodeGuiScaleY( 22 );
	s_listSaves = CodeGuiListbox( pane, pad, y, innerW, CodeGuiScaleY( 230 ),
																"DebugMenu:ListSaves", 128, FALSE, TRUE, 10 );

	y += CodeGuiScaleY( 238 );
	CodeGuiLabel( pane, pad, y + CodeGuiScaleY( 2 ), CodeGuiScaleX( 100 ), lineH,
								nullptr, L"Description", FALSE, 12 );
	s_entrySaveDesc = CodeGuiTextEntry( pane, pad + CodeGuiScaleX( 110 ), y, CodeGuiScaleX( 320 ), fieldH,
																			"DebugMenu:EntrySaveDesc", 64 );

	y += CodeGuiScaleY( 32 );
	s_buttonSaveRefresh = CodeGuiButton( pane, pad, y, CodeGuiScaleX( 110 ), btnH,
																			 "DebugMenu:ButtonSaveRefresh", L"Refresh",
																			 L"Re-read the save directory" );
	s_buttonSaveLoad = CodeGuiButton( pane, pad + CodeGuiScaleX( 120 ), y, CodeGuiScaleX( 110 ), btnH,
																		"DebugMenu:ButtonSaveLoad", L"Load",
																		L"Load the selected save" );
	s_buttonSaveHere = CodeGuiButton( pane, pad + CodeGuiScaleX( 240 ), y, CodeGuiScaleX( 140 ), btnH,
																		"DebugMenu:ButtonSaveHere", L"Save here",
																		L"Save the running game at this exact point" );
}

//-------------------------------------------------------------------------------------------------
static Bool buildDebugScreen( void )
{
	const Int px = CodeGuiScaleX( DBG_PANEL_X );
	const Int py = CodeGuiScaleY( DBG_PANEL_Y );
	const Int pw = CodeGuiScaleX( DBG_PANEL_W );
	const Int ph = CodeGuiScaleY( DBG_PANEL_H );

	// WIN_STATUS_ABOVE: last draw pass and first hit-test pass, so the overlay sits over the Options
	// sheet in the shell AND over the control bar in a running match.
	s_root = CodeGuiPanel( nullptr, px, py, pw, ph,
												 DebugMenuSystem, "DebugMenu:Root", WIN_STATUS_ABOVE );
	if( s_root == nullptr )
		return FALSE;

	s_root->winSetInputFunc( DebugMenuInput );		// ESC
	s_rootDraw = s_root->winGetDrawFunc();
	s_root->winSetDrawFunc( debugMenuRootDraw );

	// X and Y are scaled independently, so the two paddings are not interchangeable: on a 16:9
	// backbuffer CodeGuiScaleX(12) and CodeGuiScaleY(12) differ by roughly a factor of two, and
	// mixing them is what makes the footer collide with the content pane.
	const Int pad			= CodeGuiScaleX( DBG_PAD );
	const Int padY		= CodeGuiScaleY( DBG_PAD );
	const Int titleH	= CodeGuiAtLeast( CodeGuiScaleY( DBG_TITLE_H ), 18 );
	const Int tabH		= CodeGuiAtLeast( CodeGuiScaleY( DBG_TAB_H ), 24 );
	const Int tabGap	= CodeGuiScaleX( DBG_TAB_GAP );
	const Int footerH	= CodeGuiAtLeast( CodeGuiScaleY( DBG_FOOTER_BTN_H ), 30 );
	const Int innerW	= pw - pad * 2;

	// All child coordinates are PARENT-relative; winCreate stores m_region relative to the parent.
	CodeGuiLabel( s_root, pad, padY, innerW, titleH, "DebugMenu:Title", L"DEBUG", TRUE, 16 );

	const Int tabY = CodeGuiScaleY( DBG_TAB_Y );
	const Int tabW = ( innerW - tabGap * ( DBG_TAB_COUNT - 1 ) ) / DBG_TAB_COUNT;
	for( Int i = 0; i < DBG_TAB_COUNT; ++i )
	{
		AsciiString id;
		id.format( "DebugMenu:Tab%d", i );
		s_tabButton[i] = CodeGuiButton( s_root, pad + i * ( tabW + tabGap ), tabY, tabW, tabH,
																		id.str(), s_tabLabels[i] );
	}

	const Int contentY = CodeGuiScaleY( DBG_CONTENT_Y );
	const Int contentH = ph - contentY - footerH - padY * 2;

	// One SEE_THRU container per tab. SEE_THRU suppresses only its own fill, children still draw, and
	// hiding the container hides and un-hit-tests the whole pane in one call. Each pane carries
	// DebugMenuSystem so its children's GBM_/GLM_/GSM_ messages land in the same handler as the
	// root's - gogoGadget* sets owner == the parent you pass, not the screen root.
	for( Int i = 0; i < DBG_TAB_COUNT; ++i )
	{
		AsciiString id;
		id.format( "DebugMenu:Pane%d", i );
		s_tabPanel[i] = CodeGuiPanel( s_root, pad, contentY, innerW, contentH,
																	DebugMenuSystem, id.str(), WIN_STATUS_SEE_THRU );
		if( s_tabPanel[i] == nullptr )
			return FALSE;
	}

	buildStatsTab ( s_tabPanel[ DBG_TAB_STATS  ], innerW );
	buildCheatsTab( s_tabPanel[ DBG_TAB_CHEATS ], innerW );
	buildCameraTab( s_tabPanel[ DBG_TAB_CAMERA ], innerW );
	buildConfigTab( s_tabPanel[ DBG_TAB_CONFIG ], innerW );
	buildSavesTab ( s_tabPanel[ DBG_TAB_SAVES  ], innerW );

	s_buttonBack = CodeGuiButton( s_root,
																pw - pad - CodeGuiScaleX( DBG_FOOTER_BTN_W ),
																ph - padY - footerH,
																CodeGuiScaleX( DBG_FOOTER_BTN_W ), footerH,
																"DebugMenu:ButtonBack", L"Back", L"Close the debug screen" );

	return TRUE;
}

//-------------------------------------------------------------------------------------------------
// PUBLIC API /////////////////////////////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
void DebugMenuOpen( void )
{
	if( TheWindowManager == nullptr )
		return;

	if( s_layout != nullptr )		// already open - just raise it
	{
		s_layout->hide( FALSE );
		s_layout->bringForward();
		TheWindowManager->winSetFocus( s_root );
		return;
	}

	if( s_pref == nullptr )
		s_pref = NEW OptionPreferences;

	if( buildDebugScreen() == FALSE )
	{
		if( s_root )
			TheWindowManager->winDestroy( s_root );
		nullifyControls();
		return;
	}

	s_layout = CodeGuiMakeLayout( s_root );
	if( s_layout == nullptr )
	{
		TheWindowManager->winDestroy( s_root );
		nullifyControls();
		return;
	}

	loadSlidersFromPrefs();
	selectTab( DBG_TAB_STATS );
	DebugMenuUpdate();

	s_layout->hide( FALSE );
	s_layout->bringForward();

	// The line the old Extras screen never had. Without it winProcessKey has no m_keyboardFocus and
	// ESC can never reach DebugMenuInput. The root has no WIN_STATUS_NO_FOCUS and DebugMenuSystem
	// answers GWM_INPUT_FOCUS, so this actually sticks.
	TheWindowManager->winSetFocus( s_root );
}

//-------------------------------------------------------------------------------------------------
void DebugMenuClose( void )
{
	if( s_layout == nullptr )
		return;

	// Deferred winDestroy all the way down, so this is safe from inside one of our own handlers.
	CodeGuiDestroyLayout( s_layout );
	nullifyControls();

	if( s_pref )
	{
		delete s_pref;
		s_pref = nullptr;
	}

	// Hand the keyboard back. Nothing else will: winSetFocus leaves m_keyboardFocus null once the
	// focused window is destroyed. A null result here is fine, it just clears focus.
	if( TheWindowManager )
	{
		GameWindow *opts = TheWindowManager->winGetWindowFromId(
			nullptr, (Int)TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:OptionsMenuParent" ) );
		TheWindowManager->winSetFocus( opts );
	}
}

//-------------------------------------------------------------------------------------------------
Bool DebugMenuIsOpen( void )
{
	return s_layout != nullptr;
}

//-------------------------------------------------------------------------------------------------
void DebugMenuToggle( void )
{
	if( DebugMenuIsOpen() )
		DebugMenuClose();
	else
		DebugMenuOpen();
}

//-------------------------------------------------------------------------------------------------
// CALLBACKS //////////////////////////////////////////////////////////////////////////////////////
//-------------------------------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
static WindowMsgHandledType DebugMenuSystem( GameWindow *window, UnsignedInt msg,
																						 WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{
		// ------------------------------------------------------------------------------------------
		case GWM_DESTROY:
		{
			// GeneralsX @bugfix Without this, the screen leaves dangling statics behind.
			// GameWindowManager::reset() calls winDestroyAll() (GameWindowManager.cpp), which frees
			// every window without going through DebugMenuClose -- and that reset runs on every
			// match transition, which is precisely when the floating button makes this screen
			// reachable. s_layout and all ~30 control pointers would survive as dangles, and the
			// next DebugMenuUpdate writes through them (winEnable on freed memory); the
			// "s_layout == nullptr" guard there does not help, because a freed pointer is not null.
			//
			// Running the full teardown from inside a destroy is safe: winDestroy early-returns
			// WIN_ERR_OK when WIN_STATUS_DESTROYED is already set, and WindowLayout::destroyWindows
			// only re-walks its list calling that same idempotent winDestroy. So the layout object
			// is still reclaimed rather than leaked, and no window is freed twice.
			if( window == s_root )
			{
				nullifyControls();
				CodeGuiDestroyLayout( s_layout );		// idempotent here; also nulls s_layout
				if( s_pref )
				{
					delete s_pref;
					s_pref = nullptr;
				}
			}
			return MSG_HANDLED;
		}

		// ------------------------------------------------------------------------------------------
		case GWM_INPUT_FOCUS:
		{
			// Mandatory. winSetFocus walks up asking each window whether it wants focus and resets
			// m_keyboardFocus to null if nobody says yes.
			if( mData1 == TRUE )
				*(Bool *)mData2 = TRUE;
			return MSG_HANDLED;
		}

		// ------------------------------------------------------------------------------------------
		case GBM_SELECTED:
		{
			GameWindow *control = (GameWindow *)mData1;

			for( Int i = 0; i < DBG_TAB_COUNT; ++i )
			{
				if( control == s_tabButton[i] )
				{
					selectTab( i );
					return MSG_HANDLED;
				}
			}

			if( control == s_buttonBack )
			{
				DebugMenuClose();
				return MSG_HANDLED;
			}
			if( control == s_buttonGiveCash )
			{
				doGiveCash();
				return MSG_HANDLED;
			}
			if( control == s_buttonReveal )
			{
				doRevealMap();
				return MSG_HANDLED;
			}
			if( control == s_buttonSpawn )
			{
				doSpawnUnit();
				return MSG_HANDLED;
			}
			if( control == s_buttonCamDefaults )
			{
				setSliderDefaults();
				return MSG_HANDLED;
			}
			if( control == s_buttonCamApply )
			{
				applySlidersToPrefs();
				refreshPrefList();
				return MSG_HANDLED;
			}
			if( control == s_buttonCfgRefresh )
			{
				refreshResolvedConfig();
				refreshPrefList();
				return MSG_HANDLED;
			}
			if( control == s_buttonPrefSet )
			{
				applyPrefEdit();
				return MSG_HANDLED;
			}
			if( control == s_buttonPrefSave )
			{
				if( s_pref )
					s_pref->write();
				return MSG_HANDLED;
			}
			if( control == s_buttonSaveRefresh )
			{
				refreshSaveList();
				return MSG_HANDLED;
			}
			if( control == s_buttonSaveLoad )
			{
				doLoadSelectedSave();
				return MSG_HANDLED;
			}
			if( control == s_buttonSaveHere )
			{
				doSaveHere();
				return MSG_HANDLED;
			}
			break;
		}

		// ------------------------------------------------------------------------------------------
		case GSM_SLIDER_TRACK:
		case GSM_SLIDER_DONE:
		{
			GameWindow *control = (GameWindow *)mData1;
			for( Int i = 0; i < DBG_NUM_SLIDERS; ++i )
			{
				if( control == s_slider[i] )
				{
					refreshSliderValueLabel( i );
					return MSG_HANDLED;
				}
			}
			break;
		}

		// ------------------------------------------------------------------------------------------
		case GLM_SELECTED:
		case GLM_DOUBLE_CLICKED:
		{
			GameWindow *control = (GameWindow *)mData1;
			if( control == s_listPrefs )
			{
				onPrefRowSelected();
				return MSG_HANDLED;
			}
			if( control == s_listSaves && msg == GLM_DOUBLE_CLICKED )
			{
				doLoadSelectedSave();
				return MSG_HANDLED;
			}
			break;
		}

		default:
			break;
	}

	return MSG_IGNORED;
}

//-------------------------------------------------------------------------------------------------
static WindowMsgHandledType DebugMenuInput( GameWindow *window, UnsignedInt msg,
																						WindowMsgData mData1, WindowMsgData mData2 )
{
	if( msg == GWM_CHAR )
	{
		const UnsignedByte key		= (UnsignedByte)mData1;
		const UnsignedByte state	= (UnsignedByte)mData2;

		if( key == KEY_ESC )
		{
			if( BitIsSet( state, KEY_STATE_UP ) )
				DebugMenuClose();
			return MSG_HANDLED;
		}
	}

	return MSG_IGNORED;
}
