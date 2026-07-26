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

// FILE: SkirmishMapSelectMenu.cpp ////////////////////////////////////////////////////////////////////////
// Author: Chris Brue, August 2002
// Description: MapSelect menu window callbacks
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GameEngine.h"
#include "Common/MessageStream.h"
#include "Common/UserPreferences.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/CodeGui.h"
#include "GameClient/CodeGuiSearchField.h"
#include "GameClient/Gadget.h"
#include "GameClient/GameText.h"
#include "GameClient/Mouse.h"
#include "GameClient/Shell.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetRadioButton.h"
#include "GameClient/GadgetStaticText.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameClient/MapUtil.h"

// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
static NameKeyType buttonBack = NAMEKEY_INVALID;
static NameKeyType buttonOK = NAMEKEY_INVALID;
static NameKeyType listboxMap = NAMEKEY_INVALID;
static GameWindow *parent = nullptr;
static GameWindow *mapList = nullptr;

static NameKeyType radioButtonSystemMapsID = NAMEKEY_INVALID;
static NameKeyType radioButtonUserMapsID = NAMEKEY_INVALID;

static GameWindow *buttonMapStartPosition[MAX_SLOTS] = {0};
static NameKeyType buttonMapStartPositionID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																									NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID };

static GameWindow *winMapPreview = nullptr;
static NameKeyType winMapPreviewID = NAMEKEY_INVALID;

// GeneralsX @feature Claude 26/07/2026 Map search field, built in code.
// SkirmishMapSelectMenu.wnd lives inside WindowZH.big and the .wnd editor is Windows-only, so the
// field is created at runtime against ListboxMap's real rect instead of being authored. Its
// windows are children of the layout root, so destroyWindows() on the Back/OK paths tears them
// down with the rest of the screen and GadgetTextEntrySystem's own GWM_DESTROY frees the
// EntryData and its three DisplayStrings.
// GeneralsX @feature Claude 27/07/2026 Now a CodeGuiSearchField so the other three map screens get
// the same row without a copy-paste. The filter string lives inside the object, so two screens can
// be built at once without sharing it.
static CodeGuiSearchField mapSearchField;
static Bool mapSearchUsesSystemMaps = TRUE;

static void NullifyControls()
{
	parent = nullptr;
	mapList = nullptr;
	mapSearchField.reset();
	if (winMapPreview)
	{
		winMapPreview->winSetUserData(nullptr);
		winMapPreview = nullptr;
	}
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		buttonMapStartPosition[i] = nullptr;
	}
}

extern WindowLayout *skirmishMapSelectLayout;

// Tooltips -------------------------------------------------------------------------------

static void mapListTooltipFunc(GameWindow *window,
													WinInstanceData *instData,
													UnsignedInt mouse)
{
	Int x, y, row, col;
	x = LOLONGTOSHORT(mouse);
	y = HILONGTOSHORT(mouse);

	GadgetListBoxGetEntryBasedOnXY(window, x, y, row, col);

	if (row == -1 || col == -1)
	{
		TheMouse->setCursorTooltip( UnicodeString::TheEmptyString);
		return;
	}

	// GeneralsX @build BenderAI 12/02/2026 64-bit safe pointer cast
	Int imageItemData = static_cast<Int>(reinterpret_cast<intptr_t>(GadgetListBoxGetItemData(window, row, 1)));
	UnicodeString tooltip;
	switch (imageItemData)
	{
		case 0:
			tooltip = TheGameText->fetch("TOOLTIP:MapNoSuccess");
			break;
		case 1:
			tooltip = TheGameText->fetch("TOOLTIP:MapEasySuccess");
			break;
		case 2:
			tooltip = TheGameText->fetch("TOOLTIP:MapMediumSuccess");
			break;
		case 3:
			tooltip = TheGameText->fetch("TOOLTIP:MapHardSuccess");
			break;
		case 4:
			tooltip = TheGameText->fetch("TOOLTIP:MapMaxBrutalSuccess");
			break;
	}

	TheMouse->setCursorTooltip( tooltip );
}

// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////////////////////////
void positionStartSpots( AsciiString mapName, GameWindow *buttonMapStartPositions[], GameWindow *mapWindow);
void skirmishPositionStartSpots();
void skirmishUpdateSlotList();
void showSkirmishGameOptionsUnderlyingGUIElements( Bool show )
{
	NameKeyType parentID = TheNameKeyGenerator->nameToKey( "SkirmishGameOptionsMenu.wnd:SkirmishGameOptionsMenuParent" );
	GameWindow *parent = TheWindowManager->winGetWindowFromId( nullptr, parentID );
	if (!parent)
		return;

	// hide some GUI elements of the screen underneath
	GameWindow *win;

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:MapWindow") );
	win->winHide( !show );

	//win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:StaticTextTitle") );
	//win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:StaticTextTeam") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:StaticTextFaction") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:StaticTextColor") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxTeam0") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxTeam1") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxTeam2") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxTeam3") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxTeam4") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxTeam5") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxTeam6") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxTeam7") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxColor0") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxColor1") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxColor2") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxColor3") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxColor4") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxColor5") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxColor6") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxColor7") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxPlayerTemplate0") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxPlayerTemplate1") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxPlayerTemplate2") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxPlayerTemplate3") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxPlayerTemplate4") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxPlayerTemplate5") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxPlayerTemplate6") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ComboBoxPlayerTemplate7") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:TextEntryMapDisplay") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ButtonSelectMap") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ButtonStart") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:StaticTextMapPreview") );
	win->winHide( !show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ButtonReset") );
	win->winEnable( show );

	win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:ButtonBack") );
	win->winEnable( show );
}

//-------------------------------------------------------------------------------------------------
/** Refill the map listbox from the current source radio and the current search filter */
//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Claude 26/07/2026 One choke point for every repopulate, so the source radio
// and the search text can never drift apart.
static void repopulateSkirmishMapList( void )
{
	if( mapList == nullptr )
		return;

	// Deliberately NOT calling TheMapCache->updateCache() here. Each row's item data is a raw
	// const char* into a MapCache key (MapUtil.cpp), safe only while the cache is not modified
	// under a live listbox. Rebuilding it per keystroke would dangle every row, and both
	// GLM_SELECTED and buttonOK dereference that pointer.
	const UnicodeString &mapSearchFilter = mapSearchField.getFilter();
	if( mapSearchUsesSystemMaps )
	{
		populateMapListbox( mapList, TRUE, TRUE, TheSkirmishGameInfo->getMap(), mapSearchFilter );
	}
	else
	{
		populateMapListbox( mapList, FALSE, FALSE, TheSkirmishGameInfo->getMap(), mapSearchFilter );
		populateMapListboxNoReset( mapList, FALSE, TRUE, TheSkirmishGameInfo->getMap(), mapSearchFilter );
	}

	// populateMapListbox always asks for row 0 when the map it wanted is missing, and
	// GadgetListBoxSystem's GLM_SET_SELECTION bails out before notifying the owner when row 0 has
	// no cell — i.e. exactly when the filter matched nothing. Without this the preview and the
	// start spots would keep showing a map that is no longer in the list.
	// positionStartSpots dereferences its mapWindow unconditionally on the not-found path.
	Int selected = -1;
	GadgetListBoxGetSelected( mapList, &selected );
	if( selected < 0 && winMapPreview )
		positionStartSpots( AsciiString::TheEmptyString, buttonMapStartPosition, winMapPreview );
}

//-------------------------------------------------------------------------------------------------
/** The search field changed; refill the list through the one choke point */
//-------------------------------------------------------------------------------------------------
static void skirmishMapSearchChanged( CodeGuiSearchField *field )
{
	repopulateSkirmishMapList();
}

//-------------------------------------------------------------------------------------------------
/** Initialize the MapSelect menu */
//-------------------------------------------------------------------------------------------------
void SkirmishMapSelectMenuInit( WindowLayout *layout, void *userData )
{

	// set keyboard focus to main parent
	NameKeyType parentID = TheNameKeyGenerator->nameToKey( "SkirmishMapSelectMenu.wnd:SkrimishMapSelectMenuParent" );
	parent = TheWindowManager->winGetWindowFromId( nullptr, parentID );

	TheWindowManager->winSetFocus( parent );

	LANPreferences pref;
	Bool usesSystemMapDir = pref.usesSystemMapDir();

	const MapMetaData *mmd = TheMapCache->findMap(TheSkirmishGameInfo->getMap());
	if (mmd)
	{
		usesSystemMapDir = mmd->m_isOfficial;
	}

	winMapPreviewID = TheNameKeyGenerator->nameToKey( "SkirmishMapSelectMenu.wnd:WinMapPreview" );
	winMapPreview = TheWindowManager->winGetWindowFromId(parent, winMapPreviewID);


	buttonBack = TheNameKeyGenerator->nameToKey( "SkirmishMapSelectMenu.wnd:ButtonBack" );
	buttonOK = TheNameKeyGenerator->nameToKey( "SkirmishMapSelectMenu.wnd:ButtonOK" );
	listboxMap = TheNameKeyGenerator->nameToKey( "SkirmishMapSelectMenu.wnd:ListboxMap" );

	radioButtonSystemMapsID = TheNameKeyGenerator->nameToKey( "SkirmishMapSelectMenu.wnd:RadioButtonSystemMaps" );
	radioButtonUserMapsID = TheNameKeyGenerator->nameToKey( "SkirmishMapSelectMenu.wnd:RadioButtonUserMaps" );
	GameWindow *radioButtonSystemMaps = TheWindowManager->winGetWindowFromId( parent, radioButtonSystemMapsID );
	GameWindow *radioButtonUserMaps = TheWindowManager->winGetWindowFromId( parent, radioButtonUserMapsID );
	if (usesSystemMapDir)
		GadgetRadioSetSelection( radioButtonSystemMaps, FALSE );
	else
		GadgetRadioSetSelection( radioButtonUserMaps, FALSE );

	AsciiString tmpString;
	for (Int i = 0; i < MAX_SLOTS; i++)
	{
		tmpString.format("SkirmishMapSelectMenu.wnd:ButtonMapStartPosition%d", i);
		buttonMapStartPositionID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		buttonMapStartPosition[i] = TheWindowManager->winGetWindowFromId( winMapPreview, buttonMapStartPositionID[i] );
		DEBUG_ASSERTCRASH(buttonMapStartPosition[i], ("Could not find the ButtonMapStartPosition[%d]",i ));
		buttonMapStartPosition[i]->winHide(TRUE);
		buttonMapStartPosition[i]->winEnable(FALSE);
	}

	showSkirmishGameOptionsUnderlyingGUIElements(FALSE);

	// get the listbox window
	NameKeyType mapListID = TheNameKeyGenerator->nameToKey( "SkirmishMapSelectMenu.wnd:ListboxMap" );
	mapList = TheWindowManager->winGetWindowFromId( parent, mapListID );
	if( mapList )
	{
		// GeneralsX @feature Claude 26/07/2026 Reset the search state up front rather than trusting
		// NullifyControls: winDestroy only queues the windows and GWM_DESTROY is delivered later
		// from processDestroyList, so a stale entry pointer can still be set when the screen is
		// reopened in the same frame it was closed.
		mapSearchField.reset();
		mapSearchUsesSystemMaps = usesSystemMapDir;

		// The listbox parent, not the file-static `parent`: Init looks up
		// "SkrimishMapSelectMenuParent" (transposed) while the .wnd declares
		// "SkirmishMapSelectMenuParent", so `parent` is always nullptr here and every
		// winGetWindowFromId( parent, ... ) in this file silently degrades to a whole-process
		// search. create() derives the right parent from the listbox itself.
		mapSearchField.create( mapList, skirmishMapSearchChanged,
													 "SkirmishMapSelectMenu.wnd:TextEntryMapSearch",
													 "SkirmishMapSelectMenu.wnd:ButtonMapSearchClear" );

		if (TheMapCache)
			TheMapCache->updateCache();		// the only updateCache; never once per keystroke
		repopulateSkirmishMapList();
		mapList->winSetTooltipFunc(mapListTooltipFunc);
	}

}

//-------------------------------------------------------------------------------------------------
/** MapSelect menu shutdown method */
//-------------------------------------------------------------------------------------------------
void SkirmishMapSelectMenuShutdown( WindowLayout *layout, void *userData )
{

	// hide menu
	layout->hide( TRUE );

	NullifyControls();

	// our shutdown is complete
	TheShell->shutdownComplete( layout );

}

//-------------------------------------------------------------------------------------------------
/** MapSelect menu update method */
//-------------------------------------------------------------------------------------------------
void SkirmishMapSelectMenuUpdate( WindowLayout *layout, void *userData )
{

}

//-------------------------------------------------------------------------------------------------
/** Map select menu input callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType SkirmishMapSelectMenuInput( GameWindow *window, UnsignedInt msg,
																				 WindowMsgData mData1, WindowMsgData mData2 )
{

	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
			UnsignedByte state = mData2;

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
						NameKeyType buttonID = TheNameKeyGenerator->nameToKey( "SkirmishMapSelectMenu.wnd:ButtonBack" );
						GameWindow *button = TheWindowManager->winGetWindowFromId( window, buttonID );

						TheWindowManager->winSendSystemMsg( window, GBM_SELECTED,
																								(WindowMsgData)button, buttonID );

					}

					// don't let key fall through anywhere else
					return MSG_HANDLED;

				}

			}

		}

	}

	return MSG_IGNORED;

}

//-------------------------------------------------------------------------------------------------
/** MapSelect menu window system callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType SkirmishMapSelectMenuSystem( GameWindow *window, UnsignedInt msg,
																				  WindowMsgData mData1, WindowMsgData mData2 )
{

	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CREATE:
		{
			break;

		}

		//---------------------------------------------------------------------------------------------
		case GWM_DESTROY:
		{
			NullifyControls();
			break;

		}

		// --------------------------------------------------------------------------------------------
		case GWM_INPUT_FOCUS:
		{

			// if we're givin the opportunity to take the keyboard focus we must say we want it
			if( mData1 == TRUE )
				*(Bool *)mData2 = TRUE;

			return MSG_HANDLED;

		}

		//---------------------------------------------------------------------------------------------
		case GLM_DOUBLE_CLICKED:
			{
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				if( controlID == listboxMap )
				{
					int rowSelected = mData2;

					if (rowSelected >= 0)
					{
						GadgetListBoxSetSelected( control, rowSelected );
						GameWindow *button = TheWindowManager->winGetWindowFromId( window, buttonOK );

						TheWindowManager->winSendSystemMsg( window, GBM_SELECTED,
																								(WindowMsgData)button, buttonOK );
					}
				}
				break;
			}
		//---------------------------------------------------------------------------------------------
			case GLM_SELECTED:
			{
				GameWindow *mapWindow = TheWindowManager->winGetWindowFromId( parent, listboxMap );

				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				if( controlID == listboxMap )
				{
					int rowSelected = mData2;
					if( rowSelected < 0 )
					{
						positionStartSpots( AsciiString::TheEmptyString, buttonMapStartPosition, winMapPreview);
						//winMapPreview->winClearStatus(WIN_STATUS_IMAGE);
						break;
					}
					winMapPreview->winSetStatus(WIN_STATUS_IMAGE);
					UnicodeString map;
					// get text of the map to load
					map = GadgetListBoxGetText( mapWindow, rowSelected, 0 );

					// set the map name in the global data map name
					AsciiString asciiMap;
					const char *mapFname = (const char *)GadgetListBoxGetItemData( mapWindow, rowSelected );
					DEBUG_ASSERTCRASH(mapFname, ("No map item data"));
					if (mapFname)
						asciiMap = mapFname;
					else
						asciiMap.translate( map );
					asciiMap.toLower();
					Image *image = getMapPreviewImage(asciiMap);
					winMapPreview->winSetUserData((void *)TheMapCache->findMap(asciiMap));
					if(image)
					{
						winMapPreview->winSetEnabledImage(0, image);
					}
					else
					{
						winMapPreview->winClearStatus(WIN_STATUS_IMAGE);
					}
					positionStartSpots( asciiMap, buttonMapStartPosition, winMapPreview);
				}
				break;
			}

		//---------------------------------------------------------------------------------------------
		// GeneralsX @feature Claude 26/07/2026 Live map search. GadgetTextEntryInput sends
		// GEM_UPDATE_TEXT to the entry's OWNER on every accepted character and every backspace, and
		// GEM_EDIT_DONE on Return. gogoGadgetTextEntry set that owner to the window we passed as
		// parent, which is the layout root — the same window the .wnd bound this callback to. No
		// .wnd change and no FunctionLexicon row are involved.
		case GEM_UPDATE_TEXT:
		case GEM_EDIT_DONE:
		{
			mapSearchField.handleSystemMsg( msg, (GameWindow *)mData1 );
			break;
		}

		//---------------------------------------------------------------------------------------------
		case GBM_SELECTED:
		{
			// this isn't fixed yet
			GameWindow *control = (GameWindow *)mData1;

			// GeneralsX @feature Claude 26/07/2026 The clear button is code-created and belongs to no
			// .wnd, so let the search field claim it by pointer before anything reads an id off
			// `control`.
			if ( mapSearchField.handleSystemMsg( msg, control ) )
				break;

			Int controlID = control->winGetWindowId();

			if ( controlID == radioButtonSystemMapsID )
			{
				if (TheMapCache)
					TheMapCache->updateCache();
				mapSearchUsesSystemMaps = TRUE;
				repopulateSkirmishMapList();		// keeps any active search text applied
				//LANPreferences pref;
				//pref["UseSystemMapDir"] = "yes";
				//pref.write();
			}
			else if ( controlID == radioButtonUserMapsID )
			{
				if (TheMapCache)
					TheMapCache->updateCache();
				mapSearchUsesSystemMaps = FALSE;
				repopulateSkirmishMapList();
				//LANPreferences pref;
				//pref["UseSystemMapDir"] = "no";
				//pref.write();
			}
			else if ( controlID == buttonBack )
			{
				showSkirmishGameOptionsUnderlyingGUIElements(TRUE);

				if (skirmishMapSelectLayout)
				{
					skirmishMapSelectLayout->destroyWindows();
					deleteInstance(skirmishMapSelectLayout);
					skirmishMapSelectLayout = nullptr;
				}

				skirmishPositionStartSpots();
				//TheShell->pop();
				//do you need this ??
				//PostToLanGameOptions( MAP_BACK );
			}
			else if ( controlID == buttonOK )
			{

				Int selected;
				UnicodeString map;
				GameWindow *mapWindow = TheWindowManager->winGetWindowFromId( parent, listboxMap );

				// get the selected index
				GadgetListBoxGetSelected( mapWindow, &selected );

				if( selected != -1 )
				{
					//buttonPushed = true;
					// set the map name in the global data map name
					AsciiString asciiMap;
					const char *mapFname = (const char *)GadgetListBoxGetItemData( mapWindow, selected );
					DEBUG_ASSERTCRASH(mapFname, ("No map item data"));
					if (mapFname)
						asciiMap = mapFname;
					else
						asciiMap.translate( map );
					TheSkirmishGameInfo->setMap( asciiMap );

					const MapMetaData *md = TheMapCache->findMap(asciiMap);
					if (!md)
					{
						TheSkirmishGameInfo->setMapCRC(0);
						TheSkirmishGameInfo->setMapSize(0);
					}
					else
					{
						TheSkirmishGameInfo->setMapCRC(md->m_CRC);
						TheSkirmishGameInfo->setMapSize(md->m_filesize);
					}

					// reset the start positions
					for(Int i = 0; i < MAX_SLOTS; ++i)
						TheSkirmishGameInfo->getSlot(i)->setStartPos(-1);
					GameWindow *win;
					win	= TheWindowManager->winGetWindowFromId( parent, TheNameKeyGenerator->nameToKey("SkirmishGameOptionsMenu.wnd:TextEntryMapDisplay") );
					if(win)
					{
				    if (md)
				    {
  						GadgetStaticTextSetText(win, md->m_displayName);
                    }
		         }
					//if (mapFname)
						//setupGameStart(mapFname);

				showSkirmishGameOptionsUnderlyingGUIElements(TRUE);
				skirmishPositionStartSpots();
				skirmishUpdateSlotList();

				if (skirmishMapSelectLayout)
				{
					skirmishMapSelectLayout->destroyWindows();
					deleteInstance(skirmishMapSelectLayout);
					skirmishMapSelectLayout = nullptr;
				}
					//TheShell->pop();

				}
			}

			break;

		}

		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;

}
