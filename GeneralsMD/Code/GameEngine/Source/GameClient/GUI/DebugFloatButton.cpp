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

// FILE: DebugFloatButton.cpp //////////////////////////////////////////////////////////////////////
// Desc:   Always-on-top floating button that opens the Debug overlay
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"

#include "Common/GlobalData.h"
#include "GameClient/CodeGui.h"
#include "GameClient/DebugFloatButton.h"
#include "GameClient/DebugMenu.h"
#include "GameClient/Display.h"
#include "GameClient/Gadget.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"

//-------------------------------------------------------------------------------------------------
// The host panel is a parentless window sized EXACTLY to the button. findWindowUnderMouse tests the
// ROOT window's own region before descending, so an oversized ABOVE root would swallow taps on the
// map and the control bar everywhere it overhung. SEE_THRU keeps it from painting a second box
// behind the button while still letting drawWindow recurse into it.
static GameWindow *s_hostPanel = nullptr;
static GameWindow *s_button    = nullptr;

// Latched by DebugFloatButtonDestroy so the per-frame keep-alive cannot resurrect the overlay into a
// window manager that ~GameClient is about to delete.
static Bool s_shutdown = FALSE;

// Display size the current rect was computed against, so a resolution change can be detected.
static Int s_builtForW = 0;
static Int s_builtForH = 0;

static WindowMsgHandledType DebugFloatSystem( GameWindow *window, UnsignedInt msg,
																							WindowMsgData mData1, WindowMsgData mData2 );

//-------------------------------------------------------------------------------------------------
/** Top-right corner of the display, in device pixels. */
//-------------------------------------------------------------------------------------------------
static void debugFloatComputeRect( Int *x, Int *y, Int *width, Int *height )
{
	// Floored generously: SDL3GameEngine promotes more than TAP_DEAD_ZONE_PX (8 device px) of finger
	// travel to a drag, and a drag off a push button clears WIN_STATE_SELECTED so GBM_SELECTED never
	// fires. A small target on an iPad is a button that "sometimes does nothing".
	*width	= CodeGuiAtLeast( CodeGuiScaleX( 96 ), 96 );
	*height = CodeGuiAtLeast( CodeGuiScaleY( 30 ), 44 );

	const Int margin = CodeGuiAtLeast( CodeGuiScaleX( 10 ), 12 );

	*x = CodeGuiScreenW() - *width - margin;
	*y = margin;
}

//-------------------------------------------------------------------------------------------------
/** Do the actual construction. Separate from the public Create so the keep-alive can rebuild
	* without clearing the shutdown latch. */
//-------------------------------------------------------------------------------------------------
static void debugFloatBuild( void )
{
	if( s_hostPanel != nullptr || s_shutdown )
		return;

	// TheDisplay is required, not optional: CodeGuiScreenW/H fall back to 1 when it is absent, which
	// would silently produce a 1-pixel-wide button anchored off-screen.
	if( TheWindowManager == nullptr || TheDisplay == nullptr )
		return;

	// A headless client runs a GameWindowManagerDummy whose draw funcs are all null. Nothing would
	// ever render, and there is no one to tap it.
	if( TheGlobalData && TheGlobalData->m_headless )
		return;

	Int x = 0, y = 0, w = 0, h = 0;
	debugFloatComputeRect( &x, &y, &w, &h );

	// NO_FOCUS as well as ABOVE: winSetFocus bails on a NO_FOCUS window, so a floating overlay can
	// never take the keyboard away from the screen underneath it.
	s_hostPanel = CodeGuiPanel( nullptr, x, y, w, h, DebugFloatSystem, "DebugFloat:Host",
															WIN_STATUS_ABOVE | WIN_STATUS_SEE_THRU | WIN_STATUS_NO_FOCUS );
	if( s_hostPanel == nullptr )
		return;

	// Child rects are parent-relative (winPointInChild accumulates parent origins), so the button
	// fills its host at 0,0 and rides along with any later winSetPosition on the host.
	s_button = CodeGuiButton( s_hostPanel, 0, 0, w, h,
														"DebugFloat:Button", L"DEBUG", L"Open the debug screen" );
	if( s_button == nullptr )
	{
		TheWindowManager->winDestroy( s_hostPanel );
		s_hostPanel = nullptr;
		return;
	}

	s_builtForW = CodeGuiScreenW();
	s_builtForH = CodeGuiScreenH();
}

//-------------------------------------------------------------------------------------------------
void DebugFloatButtonCreate( void )
{
	s_shutdown = FALSE;			// an explicit create re-arms the overlay after a Destroy
	debugFloatBuild();
}

//-------------------------------------------------------------------------------------------------
void DebugFloatButtonDestroy( void )
{
	// Latch BEFORE destroying: the keep-alive rebuilds on a null host, and teardown must not hand a
	// fresh window to a manager that is one statement away from being deleted.
	s_shutdown = TRUE;

	if( s_hostPanel != nullptr && TheWindowManager != nullptr )
		TheWindowManager->winDestroy( s_hostPanel );		// recurses into the child button

	s_hostPanel = nullptr;
	s_button		= nullptr;
	s_builtForW = 0;
	s_builtForH = 0;
}

//-------------------------------------------------------------------------------------------------
void DebugFloatButtonUpdate( void )
{
	if( s_hostPanel == nullptr )
	{
		// Covers a Create that ran before TheDisplay was usable, and a rebuild after anything that
		// walked the whole window list out from under us (see GWM_DESTROY below).
		debugFloatBuild();
		return;
	}

	// Runtime-created windows get none of the .wnd parser's CREATIONRESOLUTION rescaling, and
	// Shell::recreateWindowLayouts() re-pushes by filename so it does not know about us either.
	// Nothing else re-anchors this on a resolution change.
	//
	// Rebuild rather than reposition: the button's size was fixed at creation, so a bare
	// winSetPosition on the host would leave a correctly-anchored button of the wrong size.
	// winDestroy unlinks from m_windowList immediately and only frees on the next
	// processDestroyList, so the old window stops drawing and hit-testing before the new one exists.
	if( CodeGuiScreenW() != s_builtForW || CodeGuiScreenH() != s_builtForH )
	{
		if( TheWindowManager )
			TheWindowManager->winDestroy( s_hostPanel );

		s_hostPanel = nullptr;
		s_button		= nullptr;
		debugFloatBuild();
	}
}

//-------------------------------------------------------------------------------------------------
/** Host panel system callback. gogoGadgetPushButton set the button's owner to this panel, so the
	* button's GBM_* messages arrive here — the panel is never given to winSetSystemFunc as a gadget,
	* which is what would orphan a gadget's own GWM_DESTROY handler. */
//-------------------------------------------------------------------------------------------------
static WindowMsgHandledType DebugFloatSystem( GameWindow *window, UnsignedInt msg,
																							WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{

		// ------------------------------------------------------------------------
		case GWM_INPUT_FOCUS:
		{
			// Refuse focus. WIN_STATUS_NO_FOCUS already makes winSetFocus bail on us directly, but
			// GadgetPushButtonSystem never answers TRUE for itself, so a winSetFocus aimed at the
			// button walks up to this panel — and whatever it answers here is final.
			if( mData1 == TRUE )
				*(Bool *)mData2 = FALSE;
			return MSG_HANDLED;
		}

		// ------------------------------------------------------------------------
		case GBM_SELECTED:
		{
			if( (GameWindow *)mData1 == s_button )
			{
				DebugMenuToggle();
				return MSG_HANDLED;
			}
			break;
		}

		// ------------------------------------------------------------------------
		case GWM_DESTROY:
		{
			// GameWindowManager::winDestroyAll() (shutdown, and the never-called reset) can take the
			// overlay without going through DebugFloatButtonDestroy. Drop the pointers so nothing
			// dereferences freed memory. The identity test matters: on a resolution rebuild this fires
			// for the OLD panel one frame after s_hostPanel already points at the new one.
			if( window == s_hostPanel )
			{
				s_hostPanel = nullptr;
				s_button		= nullptr;
			}
			return MSG_HANDLED;
		}

		// ------------------------------------------------------------------------
		default:
			break;

	}

	return MSG_IGNORED;
}
