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

// FILE: CodeGui.h /////////////////////////////////////////////////////////////////////////////////
// Desc:   Build GUI screens in code, with no .wnd file and no art dependency
///////////////////////////////////////////////////////////////////////////////////////////////////

/*
** GeneralsX @feature Claude 26/07/2026 CodeGui: build GUI screens in code, no .wnd required.
**
** The .wnd editor is Windows-only and the stock layouts live inside WindowZH.big, so every new
** screen in this port has to be constructed at runtime. This module encapsulates the correct
** construction sequence so callers cannot repeat the mistakes that make a code-built screen
** render as an invisible, unexitable sheet:
**
**   - NEVER pass WIN_STATUS_IMAGE. gogoGadget* latches the IMAGE draw func at creation time,
**     and a mapped image that is missing from the shipped data then draws literally nothing.
**     Colours are the only safe path for new code.
**   - defaultVisual == TRUE so assignDefaultGadgetLook fills EVERY draw-data slot
**     (enabled/enabledSelected/disabled/disabledSelected/hilite/hiliteSelected plus all three
**     text colour pairs). Overriding slot 0 alone leaves a pressed button rendering
**     WIN_COLOR_UNDEFINED, which W3DGameWinDefaultDraw skips entirely.
**   - WinInstanceData::init() before EVERY gadget; typed data structs memset to 0 because
**     gogoGadget* memcpy's them wholesale into their heap copy.
**   - Callbacks are raw function pointers. FunctionLexicon is consulted ONLY by the .wnd parser
**     (GameWindowManagerScript.cpp); code-built screens register nothing.
**   - Never winSetSystemFunc() on a gadget: that orphans its GWM_DESTROY handler and leaks the
**     typed user data it allocated. Messages reach you via the OWNER, which gogoGadget* sets to
**     the parent you pass.
*/

#pragma once

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "Lib/BaseType.h"
#include "GameClient/Color.h"
#include "GameClient/GameWindow.h"

// FORWARD REFERENCES /////////////////////////////////////////////////////////
class GameFont;
class GameWindow;
class WindowLayout;

//-------------------------------------------------------------------------------------------------
// Resolution helpers.
//
// Runtime-created windows get NO CREATIONRESOLUTION scaling; that happens only in
// GameWindowManagerScript::parseScreenRect. These map the 800x600 authoring space the stock
// .wnd files use onto the real backbuffer.
//
// NOTE the return type. TheDisplay->getWidth() is UnsignedInt, and Core/Libraries/Include/Lib/
// BaseType.h #undef's the min/max macros and replaces them with template<typename T> T max(T,T),
// so max( 16, TheDisplay->getHeight() * 22 / 600 ) fails template deduction ('int' vs
// 'unsigned int'). Everything below is Int so callers never hit that.
//-------------------------------------------------------------------------------------------------
Int CodeGuiScreenW( void );										///< (Int)TheDisplay->getWidth(), 1 if TheDisplay is null
Int CodeGuiScreenH( void );										///< (Int)TheDisplay->getHeight(), 1 if TheDisplay is null
Int CodeGuiScaleX( Int authoring800 );				///< authoring x/width -> device
Int CodeGuiScaleY( Int authoring600 );				///< authoring y/height -> device
Int CodeGuiAtLeast( Int value, Int floorValue );	///< Int-only max(), avoids the template collision

//-------------------------------------------------------------------------------------------------
// Palette. One place to retheme every code-built screen.
//-------------------------------------------------------------------------------------------------
struct CodeGuiTheme
{
	Color panelBg;
	Color panelBorder;
	Color buttonBg;
	Color buttonBorder;
	Color buttonHotBg;
	Color buttonHotBorder;
	Color buttonDownBg;
	Color buttonDownBorder;
	Color buttonDisabledBg;
	Color buttonDisabledBorder;
	Color fieldBg;
	Color fieldBorder;
	Color textNormal;
	Color textShadow;
	Color textDisabled;
	Color accent;
};
const CodeGuiTheme &CodeGuiGetTheme( void );

//-------------------------------------------------------------------------------------------------
// Fonts.
//
// Every code-built control draws in Arial, because that is the only face the port can rely on
// (scripts/build/ios/stage-fonts.sh installs Liberation Sans as fonts/arial.ttf).
//
// BOLD IS A FAMILY, NOT A FLAG, on every platform this port ships to. The Bool that
// GameWindowManager::winFindFont / FontLibrary::getFont / WW3DAssetManager::Get_FontChars pass
// around only reaches FW_BOLD on the Win32 GDI path (render2dsentence.cpp,
// FontCharsClass::Create_GDI_Font). The FreeType path used on macOS/iOS/Linux is
// Create_Freetype_Font( font_name ) — it never looks at IsBold at all, so
// winFindFont( "Arial", 12, TRUE ) yields a SECOND GameFont with byte-identical glyphs. The only
// thing that changes the weight is the family name handed to the font locator, and the correct
// spelling of that name differs per platform, so it lives in one place: here.
//
// Use this for anything the factories below do not cover — a push button, say:
//   btn->winSetFont( CodeGuiFont( 12, TRUE ) );
// GameWindow::winSetFont dispatches by style bit to GadgetListBoxSetFont / GadgetComboBoxSetFont /
// GadgetTextEntrySetFont / GadgetStaticTextSetFont, each of which re-fonts every DisplayString the
// gadget owns, so a post-creation swap is safe.
GameFont *CodeGuiFont( Int pointSize, Bool bold = FALSE );

//-------------------------------------------------------------------------------------------------
// Control factories.
//
// `idName` is hashed with TheNameKeyGenerator->nameToKey() and installed with winSetWindowId().
// Always pass one for anything you intend to look up: an id-less window gets id 0 ==
// NAMEKEY_INVALID, and winGetWindowFromId( nullptr, 0 ) would then match it from anywhere in the
// process. Pass nullptr only for decorative controls you never address again.
//
// Every child's OWNER is `parent`, so all of a screen's GBM_/GLM_/GSM_ messages arrive at the
// parent's single system func. Pass nullptr for `parent` only for a screen ROOT.
//
// All coordinates are device pixels and PARENT-RELATIVE. Run authoring-space numbers through
// CodeGuiScaleX / CodeGuiScaleY first.
//-------------------------------------------------------------------------------------------------

/// Plain (non-gadget) container. Pass parent==nullptr for a top-level screen root.
/// `extraStatus` is OR'd into the status word: WIN_STATUS_ABOVE for an overlay that must beat the
/// in-game control bar, WIN_STATUS_SEE_THRU for an invisible grouping container.
GameWindow *CodeGuiPanel( GameWindow *parent,
													Int x, Int y, Int width, Int height,
													GameWinSystemFunc systemFunc,
													const char *idName,
													UnsignedInt extraStatus = 0 );

GameWindow *CodeGuiButton( GameWindow *parent,
													 Int x, Int y, Int width, Int height,
													 const char *idName,
													 const WideChar *label,
													 const WideChar *tooltip = nullptr );

GameWindow *CodeGuiLabel( GameWindow *parent,
													Int x, Int y, Int width, Int height,
													const char *idName,
													const WideChar *text,
													Bool centered = FALSE,
													Int pointSize = 12,
													Bool bold = FALSE );

GameWindow *CodeGuiCheckbox( GameWindow *parent,
														 Int x, Int y, Int width, Int height,
														 const char *idName,
														 const WideChar *label,
														 Bool checked = FALSE );

GameWindow *CodeGuiTextEntry( GameWindow *parent,
															Int x, Int y, Int width, Int height,
															const char *idName,
															Short maxTextLen,
															Int pointSize = 12,
															Bool bold = FALSE );

GameWindow *CodeGuiHSlider( GameWindow *parent,
														Int x, Int y, Int width, Int height,
														const char *idName,
														Int minVal, Int maxVal );

/// Scrolling listbox. `listLength` is the row capacity, not the pixel height; grow it later with
/// GadgetListBoxSetListLength. The scrollbar children are the one place the engine force-ORs
/// WIN_STATUS_IMAGE on our behalf (GadgetListboxCreateScrollbar), so this factory strips it back
/// off and rebinds the colour draw funcs — otherwise the arrows and thumb are invisible whenever
/// the VSliderLarge* mapped images are absent from the shipped data.
GameWindow *CodeGuiListbox( GameWindow *parent,
														Int x, Int y, Int width, Int height,
														const char *idName,
														Int listLength,
														Bool multiSelect = FALSE,
														Bool scrollBar = TRUE,
														Int pointSize = 12 );

//-------------------------------------------------------------------------------------------------
// Off-stack layout lifecycle. Mirrors Shell::getOptionsLayout / destroyOptionsLayout (Shell.cpp),
// which is the engine's own pattern for a screen that is deliberately NOT on m_screenStack. A
// WindowLayout with no load() call behind it is fully supported: only load() touches a file, and
// it is the only thing that sets m_filenameString.
//-------------------------------------------------------------------------------------------------

/// newInstance(WindowLayout) + addWindow(root). Add ROOTS only; children ride along.
WindowLayout *CodeGuiMakeLayout( GameWindow *root );

/// destroyWindows() + deleteInstance() + nulls your pointer. Uses the DEFERRED winDestroy, so it
/// is safe to call from inside a button handler.
void CodeGuiDestroyLayout( WindowLayout *&layout );
