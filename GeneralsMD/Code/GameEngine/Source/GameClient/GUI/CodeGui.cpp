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

// FILE: CodeGui.cpp ///////////////////////////////////////////////////////////////////////////////
// Desc:   Build GUI screens in code, with no .wnd file and no art dependency
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"

#include "Common/GameMemory.h"
#include "Common/NameKeyGenerator.h"
#include "GameClient/CodeGui.h"
#include "GameClient/Color.h"
#include "GameClient/Display.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetCheckBox.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetSlider.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/WinInstanceData.h"
#include "GameClient/WindowLayout.h"

//-------------------------------------------------------------------------------------------------
Int CodeGuiScreenW( void ) { return TheDisplay ? (Int)TheDisplay->getWidth() : 1; }
Int CodeGuiScreenH( void ) { return TheDisplay ? (Int)TheDisplay->getHeight() : 1; }
Int CodeGuiScaleX( Int authoring800 ) { return ( authoring800 * CodeGuiScreenW() ) / 800; }
Int CodeGuiScaleY( Int authoring600 ) { return ( authoring600 * CodeGuiScreenH() ) / 600; }
Int CodeGuiAtLeast( Int value, Int floorValue ) { return value < floorValue ? floorValue : value; }

//-------------------------------------------------------------------------------------------------
static const CodeGuiTheme s_theme =
{
	/* panelBg */							GameMakeColor(   6,   8,  14, 235 ),
	/* panelBorder */					GameMakeColor(  90, 110, 170, 255 ),
	/* buttonBg */						GameMakeColor(  26,  34,  56, 255 ),
	/* buttonBorder */				GameMakeColor( 120, 145, 205, 255 ),
	/* buttonHotBg */					GameMakeColor(  46,  62,  98, 255 ),
	/* buttonHotBorder */			GameMakeColor( 190, 210, 255, 255 ),
	/* buttonDownBg */				GameMakeColor( 235, 190,  60, 255 ),
	/* buttonDownBorder */		GameMakeColor( 255, 255, 255, 255 ),
	/* buttonDisabledBg */		GameMakeColor(  24,  24,  28, 200 ),
	/* buttonDisabledBorder */	GameMakeColor(  90,  90,  96, 255 ),
	/* fieldBg */							GameMakeColor(   0,   0,   0, 190 ),
	/* fieldBorder */					GameMakeColor(  49,  55, 168, 255 ),
	/* textNormal */					GameMakeColor( 254, 254, 254, 255 ),
	/* textShadow */					GameMakeColor(   0,   0,   0, 255 ),
	/* textDisabled */				GameMakeColor( 140, 140, 148, 255 ),
	/* accent */							GameMakeColor( 255, 220,  60, 255 )
};
const CodeGuiTheme &CodeGuiGetTheme( void ) { return s_theme; }

//-------------------------------------------------------------------------------------------------
/** Resolve the font every code-built control uses. */
//-------------------------------------------------------------------------------------------------
static GameFont *codeGuiFont( Int pointSize )
{
	// On iOS FontCharsClass::Locate_Font_FontConfig lowercases the family, strips spaces and looks
	// for fonts/<name>.ttf relative to CWD, falling back unconditionally to fonts/arial.ttf.
	// scripts/build/ios/stage-fonts.sh stages arial / arialbold / couriernew / timesnewroman, so
	// "Arial" resolves exactly. bold=TRUE allocates a distinct GameFont with IDENTICAL glyphs
	// (IsBold is only consumed on the Win32 GDI path), so we never bother asking for it.
	if( TheWindowManager == nullptr )
		return nullptr;

	return TheWindowManager->winFindFont( "Arial", pointSize, FALSE );
}

//-------------------------------------------------------------------------------------------------
/** Stamp an id onto a freshly created window.
	*
	* winCreate copies instData wholesale (m_instData = *data) so the id is already in place; this
	* just keeps the call sites honest about which windows are addressable, and covers the gadget
	* factories that rebuild parts of instData behind our back. */
//-------------------------------------------------------------------------------------------------
static void codeGuiSetId( GameWindow *win, const char *idName )
{
	if( win == nullptr || idName == nullptr )
		return;

	win->winSetWindowId( (Int)TheNameKeyGenerator->nameToKey( idName ) );
}

//-------------------------------------------------------------------------------------------------
/** Undo a forced WIN_STATUS_IMAGE on a gadget the engine built for us.
	*
	* GameWindow::m_status and WinInstanceData::m_status are separate fields that the draw path and
	* the gadget code read independently, so both have to be cleared. Rebinding the draw func is the
	* part that actually matters: gogoGadget* chose the image variant at creation and nothing else
	* ever revisits that choice. */
//-------------------------------------------------------------------------------------------------
static void codeGuiForceColorDraw( GameWindow *win, GameWinDrawFunc colorDraw )
{
	if( win == nullptr )
		return;

	win->winClearStatus( WIN_STATUS_IMAGE );
	BitClear( win->winGetInstanceData()->m_status, WIN_STATUS_IMAGE );

	if( colorDraw )
		win->winSetDrawFunc( colorDraw );
}

//-------------------------------------------------------------------------------------------------
/** Paint a push button (or a slider thumb, or a scrollbar arrow) in theme colours.
	*
	* Slots 0 AND 1 both matter: W3DGadgetPushButtonDraw selects index 1 whenever WIN_STATE_SELECTED
	* is set, and assignDefaultGadgetLook leaves index 1 on its debug yellow. */
//-------------------------------------------------------------------------------------------------
static void codeGuiThemeButton( GameWindow *btn, Color bg, Color border, Color downBg, Color downBorder )
{
	if( btn == nullptr )
		return;

	GadgetButtonSetEnabledColor								( btn, bg );
	GadgetButtonSetEnabledBorderColor					( btn, border );
	GadgetButtonSetEnabledSelectedColor				( btn, downBg );
	GadgetButtonSetEnabledSelectedBorderColor	( btn, downBorder );
	GadgetButtonSetHiliteColor								( btn, s_theme.buttonHotBg );
	GadgetButtonSetHiliteBorderColor					( btn, s_theme.buttonHotBorder );
	GadgetButtonSetHiliteSelectedColor				( btn, downBg );
	GadgetButtonSetHiliteSelectedBorderColor	( btn, downBorder );
	GadgetButtonSetDisabledColor							( btn, s_theme.buttonDisabledBg );
	GadgetButtonSetDisabledBorderColor				( btn, s_theme.buttonDisabledBorder );
	GadgetButtonSetDisabledSelectedColor			( btn, s_theme.buttonDisabledBg );
	GadgetButtonSetDisabledSelectedBorderColor( btn, s_theme.buttonDisabledBorder );

	btn->winSetEnabledTextColors ( s_theme.textNormal,	 s_theme.textShadow );
	btn->winSetHiliteTextColors	 ( s_theme.textNormal,	 s_theme.textShadow );
	btn->winSetDisabledTextColors( s_theme.textDisabled, s_theme.textShadow );
}

//-------------------------------------------------------------------------------------------------
GameWindow *CodeGuiPanel( GameWindow *parent, Int x, Int y, Int width, Int height,
													GameWinSystemFunc systemFunc, const char *idName,
													UnsignedInt extraStatus )
{
	if( TheWindowManager == nullptr )
		return nullptr;

	const UnsignedInt status = WIN_STATUS_ENABLED | extraStatus;	// never WIN_STATUS_IMAGE

	WinInstanceData instData;
	instData.init();
	instData.m_style	= GWS_USER_WINDOW;		// matches the .wnd "USER" window type
	instData.m_status = status;							// GameWindow::m_status and WinInstanceData::m_status are
																					// DIFFERENT fields; keep them in sync
	if( idName )
		instData.m_id = (Int)TheNameKeyGenerator->nameToKey( idName );

	// winCreate sends GWM_CREATE BEFORE it copies instData, so id/style/status are not yet visible
	// inside a GWM_CREATE handler. Do all setup on the returned pointer.
	GameWindow *win = TheWindowManager->winCreate( parent, status, x, y, width, height,
																								 systemFunc, &instData );
	if( win == nullptr )
		return nullptr;

	codeGuiSetId( win, idName );

	if( BitIsSet( status, WIN_STATUS_SEE_THRU ) == FALSE )
	{
		// W3DGameWinDefaultDraw's colour branch, which skips any slot still holding
		// WIN_COLOR_UNDEFINED — and WinInstanceData::init() leaves every slot exactly there.
		// All three states get filled: a panel with GWS_MOUSE_TRACK children can still pick up
		// WIN_STATE_HILITED, and a disabled panel must not vanish.
		win->winSetEnabledColor				( 0, s_theme.panelBg );
		win->winSetEnabledBorderColor	( 0, s_theme.panelBorder );
		win->winSetHiliteColor				( 0, s_theme.panelBg );
		win->winSetHiliteBorderColor	( 0, s_theme.panelBorder );
		win->winSetDisabledColor			( 0, s_theme.panelBg );
		win->winSetDisabledBorderColor( 0, s_theme.panelBorder );
	}

	return win;
}

//-------------------------------------------------------------------------------------------------
GameWindow *CodeGuiButton( GameWindow *parent, Int x, Int y, Int width, Int height,
													 const char *idName, const WideChar *label, const WideChar *tooltip )
{
	if( TheWindowManager == nullptr )
		return nullptr;

	WinInstanceData instData;
	instData.init();
	instData.m_style	= GWS_PUSH_BUTTON | GWS_MOUSE_TRACK;	// GWS_PUSH_BUTTON is asserted
	instData.m_status = WIN_STATUS_ENABLED;
	if( idName )
		instData.m_id = (Int)TheNameKeyGenerator->nameToKey( idName );
	if( tooltip )
		instData.setTooltipText( UnicodeString( tooltip ) );

	// defaultVisual = TRUE: assignDefaultGadgetLook fills EVERY colour slot and all three text
	// colour pairs. It also installs mapped images by name, but because we did not pass
	// WIN_STATUS_IMAGE the colour draw func is already bound and those images are inert.
	GameWindow *btn = TheWindowManager->gogoGadgetPushButton(
												parent,
												WIN_STATUS_ENABLED,		// NO WIN_STATUS_IMAGE — latched at creation
												x, y, width, height,
												&instData,
												codeGuiFont( 12 ),
												TRUE );
	if( btn == nullptr )
		return nullptr;

	codeGuiSetId( btn, idName );
	codeGuiThemeButton( btn, s_theme.buttonBg, s_theme.buttonBorder,
											s_theme.buttonDownBg, s_theme.buttonDownBorder );

	// LITERAL text. GadgetButtonSetText does not go through TheGameText->fetch(), unlike the .wnd
	// TEXT= path — which is why a .wnd built outside the shipped CSF shows "MISSING: 'Back'".
	if( label )
		GadgetButtonSetText( btn, UnicodeString( label ) );

	return btn;
}

//-------------------------------------------------------------------------------------------------
GameWindow *CodeGuiLabel( GameWindow *parent, Int x, Int y, Int width, Int height,
													const char *idName, const WideChar *text, Bool centered, Int pointSize )
{
	if( TheWindowManager == nullptr )
		return nullptr;

	WinInstanceData instData;
	TextData				textData;
	instData.init();
	memset( &textData, 0, sizeof( textData ) );		// memcpy'd wholesale into a fresh TextData;
																								// textData.text is overwritten for us
	textData.centered = centered;

	instData.m_style	= GWS_STATIC_TEXT | GWS_MOUSE_TRACK;
	instData.m_status = WIN_STATUS_ENABLED;
	if( idName )
		instData.m_id = (Int)TheNameKeyGenerator->nameToKey( idName );

	// defaultVisual = FALSE here (unlike buttons) so no background box is painted behind the text:
	// every slot stays WIN_COLOR_UNDEFINED and only the glyphs draw. This is the in-tree precedent
	// from MainMenu.cpp's version/credit labels.
	// NOTE: gogoGadgetStaticText BitClears GWS_TAB_STOP on OUR struct — harmless, but it is why
	// instData must never be reused without another init().
	GameWindow *lbl = TheWindowManager->gogoGadgetStaticText(
												parent, WIN_STATUS_ENABLED,
												x, y, width, height,
												&instData, &textData,
												codeGuiFont( pointSize ),
												FALSE );
	if( lbl == nullptr )
		return nullptr;

	codeGuiSetId( lbl, idName );

	lbl->winSetEnabledTextColors ( s_theme.textNormal,	 s_theme.textShadow );
	lbl->winSetHiliteTextColors	 ( s_theme.textNormal,	 s_theme.textShadow );
	lbl->winSetDisabledTextColors( s_theme.textDisabled, s_theme.textShadow );

	if( text )
		GadgetStaticTextSetText( lbl, UnicodeString( text ) );	// do this LAST, it needs the font

	return lbl;
}

//-------------------------------------------------------------------------------------------------
GameWindow *CodeGuiCheckbox( GameWindow *parent, Int x, Int y, Int width, Int height,
														 const char *idName, const WideChar *label, Bool checked )
{
	if( TheWindowManager == nullptr )
		return nullptr;

	WinInstanceData instData;
	instData.init();
	instData.m_style	= GWS_CHECK_BOX | GWS_MOUSE_TRACK;		// GWS_CHECK_BOX is asserted
	instData.m_status = WIN_STATUS_ENABLED;
	if( idName )
		instData.m_id = (Int)TheNameKeyGenerator->nameToKey( idName );

	GameWindow *box = TheWindowManager->gogoGadgetCheckbox(
												parent, WIN_STATUS_ENABLED,
												x, y, width, height,
												&instData,
												codeGuiFont( 12 ),
												TRUE );
	if( box == nullptr )
		return nullptr;

	codeGuiSetId( box, idName );

	// Slot 0 is the row background, 1 the unchecked box, 2 the checked box. assignDefaultGadgetLook
	// deliberately leaves the unchecked FILL at WIN_COLOR_UNDEFINED so the box reads as hollow;
	// we keep that and only supply the border, otherwise checked and unchecked look identical.
	GadgetCheckBoxSetEnabledColor										( box, s_theme.fieldBg );
	GadgetCheckBoxSetEnabledBorderColor							( box, s_theme.fieldBorder );
	GadgetCheckBoxSetEnabledUncheckedBoxColor				( box, WIN_COLOR_UNDEFINED );
	GadgetCheckBoxSetEnabledUncheckedBoxBorderColor	( box, s_theme.buttonBorder );
	GadgetCheckBoxSetEnabledCheckedBoxColor					( box, s_theme.accent );
	GadgetCheckBoxSetEnabledCheckedBoxBorderColor		( box, s_theme.textNormal );

	GadgetCheckBoxSetHiliteColor										( box, s_theme.buttonHotBg );
	GadgetCheckBoxSetHiliteBorderColor							( box, s_theme.buttonHotBorder );
	GadgetCheckBoxSetHiliteUncheckedBoxColor				( box, WIN_COLOR_UNDEFINED );
	GadgetCheckBoxSetHiliteUncheckedBoxBorderColor	( box, s_theme.textNormal );
	GadgetCheckBoxSetHiliteCheckedBoxColor					( box, s_theme.accent );
	GadgetCheckBoxSetHiliteCheckedBoxBorderColor		( box, s_theme.textNormal );

	GadgetCheckBoxSetDisabledColor									( box, s_theme.buttonDisabledBg );
	GadgetCheckBoxSetDisabledBorderColor						( box, s_theme.buttonDisabledBorder );
	GadgetCheckBoxSetDisabledUncheckedBoxColor			( box, WIN_COLOR_UNDEFINED );
	GadgetCheckBoxSetDisabledUncheckedBoxBorderColor( box, s_theme.buttonDisabledBorder );
	GadgetCheckBoxSetDisabledCheckedBoxColor				( box, s_theme.textDisabled );
	GadgetCheckBoxSetDisabledCheckedBoxBorderColor	( box, s_theme.buttonDisabledBorder );

	box->winSetEnabledTextColors ( s_theme.textNormal,	 s_theme.textShadow );
	box->winSetHiliteTextColors	 ( s_theme.textNormal,	 s_theme.textShadow );
	box->winSetDisabledTextColors( s_theme.textDisabled, s_theme.textShadow );

	if( label )
		GadgetCheckBoxSetText( box, UnicodeString( label ) );

	// Set the state bit by hand rather than through GadgetCheckBoxSetChecked: that helper also
	// winSendSystemMsg's GBM_SELECTED at the owner, which during construction reaches the screen's
	// handler before the screen has finished assigning its control pointers. Use the helper for
	// every LATER change; here it would fire a click nobody made.
	if( checked )
		BitSet( box->winGetInstanceData()->m_state, WIN_STATE_SELECTED );

	return box;
}

//-------------------------------------------------------------------------------------------------
GameWindow *CodeGuiTextEntry( GameWindow *parent, Int x, Int y, Int width, Int height,
															const char *idName, Short maxTextLen )
{
	if( TheWindowManager == nullptr )
		return nullptr;

	WinInstanceData instData;
	EntryData				entryData;
	instData.init();
	memset( &entryData, 0, sizeof( entryData ) );		// MANDATORY: gogoGadgetTextEntry inspects
																									// entryData->text before the memcpy and
																									// allocates the DisplayStrings from it
	// maxTextLen MUST be > 1. The accept test is `charPos < maxTextLen - 1`, so 0 means `0 < -1`
	// and every keystroke is silently swallowed with the keyboard still up.
	entryData.maxTextLen = ( maxTextLen > 1 ) ? maxTextLen : (Short)32;		// clamped to 256 inside
	entryData.aSCIIOnly	 = FALSE;

	instData.m_style	= GWS_ENTRY_FIELD | GWS_MOUSE_TRACK;	// GWS_ENTRY_FIELD is also the exact bit
																												// SDL3GameEngine::updateTextInputState tests
																												// to raise the iOS keyboard
	instData.m_status = WIN_STATUS_ENABLED;
	if( idName )
		instData.m_id = (Int)TheNameKeyGenerator->nameToKey( idName );

	GameWindow *entry = TheWindowManager->gogoGadgetTextEntry(
													parent, WIN_STATUS_ENABLED,
													x, y, width, height,
													&instData, &entryData,
													codeGuiFont( 12 ),
													TRUE );
	if( entry == nullptr )
		return nullptr;

	codeGuiSetId( entry, idName );

	GadgetTextEntrySetEnabledColor			 ( entry, s_theme.fieldBg );
	GadgetTextEntrySetEnabledBorderColor ( entry, s_theme.fieldBorder );
	GadgetTextEntrySetHiliteColor				 ( entry, s_theme.fieldBg );
	GadgetTextEntrySetHiliteBorderColor	 ( entry, s_theme.accent );
	GadgetTextEntrySetDisabledColor			 ( entry, s_theme.buttonDisabledBg );
	GadgetTextEntrySetDisabledBorderColor( entry, s_theme.buttonDisabledBorder );
	GadgetTextEntrySetTextColor					 ( entry, s_theme.textNormal );

	// winSetFont already dispatched to GadgetTextEntrySetFont (GameWindow::winSetFont switches on
	// the style bits) because gogoGadgetTextEntry does winSetUserData() BEFORE
	// assignDefaultGadgetLook(). Re-asserting is idempotent and documents the dependency.
	GameFont *font = codeGuiFont( 12 );
	if( font )
		GadgetTextEntrySetFont( entry, font );

	GadgetTextEntrySetText( entry, UnicodeString::TheEmptyString );

	return entry;
}

//-------------------------------------------------------------------------------------------------
GameWindow *CodeGuiHSlider( GameWindow *parent, Int x, Int y, Int width, Int height,
														const char *idName, Int minVal, Int maxVal )
{
	if( TheWindowManager == nullptr )
		return nullptr;

	WinInstanceData instData;
	SliderData			sliderData;
	instData.init();
	memset( &sliderData, 0, sizeof( sliderData ) );		// memcpy'd; numTicks/position are internal
	sliderData.minVal = minVal;
	sliderData.maxVal = maxVal;

	instData.m_style	= GWS_HORZ_SLIDER | GWS_MOUSE_TRACK;
	instData.m_status = WIN_STATUS_ENABLED;
	if( idName )
		instData.m_id = (Int)TheNameKeyGenerator->nameToKey( idName );

	// gogoGadgetSlider force-ORs WIN_STATUS_TAB_STOP and builds the thumb itself with
	// gogoGadgetPushButton( slider, status|ENABLED|DRAGGABLE, …, nullptr, TRUE ). Because our status
	// has no WIN_STATUS_IMAGE the thumb takes the COLOUR draw func and gets a full palette from
	// assignDefaultGadgetLook — i.e. the thumb is visible with no art.
	//
	// The size floors are load-bearing, not cosmetic: numTicks is
	// (width - HORIZONTAL_SLIDER_THUMB_WIDTH) / (maxVal - minVal), so a track narrower than the
	// thumb yields a zero or negative tick size and the thumb never tracks the value.
	const Int trackW = CodeGuiAtLeast( width, HORIZONTAL_SLIDER_THUMB_WIDTH * 3 );
	const Int trackH = CodeGuiAtLeast( height, HORIZONTAL_SLIDER_THUMB_HEIGHT );

	GameWindow *slider = TheWindowManager->gogoGadgetSlider(
													 parent, WIN_STATUS_ENABLED,
													 x, y, trackW, trackH,
													 &instData, &sliderData,
													 codeGuiFont( 10 ),
													 TRUE );
	if( slider == nullptr )
		return nullptr;

	codeGuiSetId( slider, idName );

	GadgetSliderSetEnabledColor				( slider, s_theme.fieldBg );
	GadgetSliderSetEnabledBorderColor	( slider, s_theme.fieldBorder );
	GadgetSliderSetHiliteColor				( slider, s_theme.fieldBg );
	GadgetSliderSetHiliteBorderColor	( slider, s_theme.accent );
	GadgetSliderSetDisabledColor			( slider, s_theme.buttonDisabledBg );
	GadgetSliderSetDisabledBorderColor( slider, s_theme.buttonDisabledBorder );

	codeGuiThemeButton( GadgetSliderGetThumb( slider ), s_theme.accent, s_theme.textShadow,
											s_theme.textNormal, s_theme.textShadow );

	return slider;
}

//-------------------------------------------------------------------------------------------------
GameWindow *CodeGuiListbox( GameWindow *parent, Int x, Int y, Int width, Int height,
														const char *idName, Int listLength, Bool multiSelect, Bool scrollBar,
														Int pointSize )
{
	if( TheWindowManager == nullptr )
		return nullptr;

	WinInstanceData instData;
	ListboxData			listboxData;
	instData.init();
	memset( &listboxData, 0, sizeof( listboxData ) );		// memcpy'd; every field after columnWidth
																											// is engine-internal bookkeeping
	// columns MUST be >= 1: gogoGadgetListBox takes the multi-column branch for anything else and
	// bails out with a half-built listbox when columnWidthPercentage is null. listLength MUST be
	// >= 1 for the same class of reason — GadgetListBoxSetListLength memcpy's from listData.
	listboxData.columns			= 1;
	listboxData.listLength	= (Short)CodeGuiAtLeast( listLength, 1 );
	listboxData.multiSelect = multiSelect;
	listboxData.scrollBar		= scrollBar;
	listboxData.autoScroll	= FALSE;
	listboxData.autoPurge		= FALSE;
	listboxData.forceSelect = FALSE;

	instData.m_style	= GWS_SCROLL_LISTBOX | GWS_MOUSE_TRACK;		// GWS_SCROLL_LISTBOX is asserted
	instData.m_status = WIN_STATUS_ENABLED;
	if( idName )
		instData.m_id = (Int)TheNameKeyGenerator->nameToKey( idName );

	// No instance text: a listbox with text is treated as having a TITLE, which shrinks
	// displayHeight by a font height and offsets every scrollbar child.
	GameWindow *list = TheWindowManager->gogoGadgetListBox(
											 parent, WIN_STATUS_ENABLED,
											 x, y, width, height,
											 &instData, &listboxData,
											 codeGuiFont( pointSize ),
											 TRUE );
	if( list == nullptr )
		return nullptr;

	codeGuiSetId( list, idName );

	// Propagates through to the scrollbar slider and both arrows, but NOT to the slider thumb.
	GadgetListBoxSetColors( list,
													s_theme.fieldBg,				// enabled
													s_theme.fieldBorder,		// enabled border
													s_theme.buttonHotBg,		// enabled selected item
													s_theme.accent,					// enabled selected item border
													s_theme.buttonDisabledBg,			// disabled
													s_theme.buttonDisabledBorder,	// disabled border
													s_theme.buttonDisabledBg,			// disabled selected item
													s_theme.buttonDisabledBorder,	// disabled selected item border
													s_theme.fieldBg,				// hilite
													s_theme.accent,					// hilite border
													s_theme.buttonHotBg,		// hilite selected item
													s_theme.accent );				// hilite selected item border

	list->winSetEnabledTextColors ( s_theme.textNormal,	 s_theme.textShadow );
	list->winSetHiliteTextColors	( s_theme.textNormal,	 s_theme.textShadow );
	list->winSetDisabledTextColors( s_theme.textDisabled, s_theme.textShadow );

	// GadgetListboxCreateScrollbar is the one place the engine force-ORs WIN_STATUS_IMAGE behind
	// our back, and assignDefaultGadgetLook then points the arrows at VSliderLarge*ButtonEnabled
	// and friends. If those mapped images are missing from the shipped data the whole scrollbar
	// draws nothing at all, so strip the bit and rebind the colour draw funcs. The scrollbar's own
	// vertical slider already escapes this — gogoGadgetSlider special-cases a listbox parent — but
	// its thumb is a plain push button and does not.
	GameWindow *scroll = GadgetListBoxGetSlider( list );
	if( scroll )
	{
		GameWindow *thumb = GadgetSliderGetThumb( scroll );

		codeGuiForceColorDraw( GadgetListBoxGetUpButton( list ),
													 TheWindowManager->getPushButtonDrawFunc() );
		codeGuiForceColorDraw( GadgetListBoxGetDownButton( list ),
													 TheWindowManager->getPushButtonDrawFunc() );
		codeGuiForceColorDraw( scroll, TheWindowManager->getVerticalSliderDrawFunc() );
		codeGuiForceColorDraw( thumb,	 TheWindowManager->getPushButtonDrawFunc() );

		codeGuiThemeButton( GadgetListBoxGetUpButton( list ), s_theme.buttonBg, s_theme.buttonBorder,
												s_theme.buttonDownBg, s_theme.buttonDownBorder );
		codeGuiThemeButton( GadgetListBoxGetDownButton( list ), s_theme.buttonBg, s_theme.buttonBorder,
												s_theme.buttonDownBg, s_theme.buttonDownBorder );
		codeGuiThemeButton( thumb, s_theme.accent, s_theme.textShadow,
												s_theme.textNormal, s_theme.textShadow );
	}

	return list;
}

//-------------------------------------------------------------------------------------------------
WindowLayout *CodeGuiMakeLayout( GameWindow *root )
{
	if( root == nullptr )
		return nullptr;

	// Public ctor + MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE, so newInstance works from any TU.
	// We never call load(), so nothing touches a .wnd and the filename stays empty. That is safe
	// ONLY because this layout is never put on m_screenStack — Shell::recreateWindowLayouts()
	// re-pushes by filename and would fail on it.
	WindowLayout *layout = newInstance(WindowLayout);
	layout->addWindow( root );

	return layout;
}

//-------------------------------------------------------------------------------------------------
void CodeGuiDestroyLayout( WindowLayout *&layout )
{
	if( layout == nullptr )
		return;

	layout->destroyWindows();		// deferred winDestroy, recurses into children -> safe in a handler
	deleteInstance(layout);
	layout = nullptr;
}
