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

// FILE: DebugFloatButton.h ////////////////////////////////////////////////////////////////////////
// Desc:   Always-on-top floating button that opens the Debug overlay
///////////////////////////////////////////////////////////////////////////////////////////////////

/*
** GeneralsX @feature Claude 26/07/2026 Floating DEBUG button, reachable from the shell AND in-game.
**
** This is a parentless, off-stack GameWindow carrying WIN_STATUS_ABOVE, not a control on any screen.
** That is the only construction that survives everything the game does to its GUI: Shell::push and
** Shell::pop only walk m_screenStack, WindowLayout::hide only walks its own layout, GameClient::reset
** never touches the window manager, and GameWindowManager::reset has zero call sites (the manager is
** created with a bare ->init() in GameClient::init, so it is not in m_subsystems and resetAll skips
** it). The window therefore lives from GameClient::init to ~GameClient, across every menu, every
** load screen and every match. WIN_STATUS_ABOVE puts it in winRepaint's third and last draw pass and
** in winProcessMouseEvent's first hit-test pass, so it draws over and takes taps ahead of the
** in-game control bar.
**
** Four cases where it is present but NOT usable. All are structural, none is a regression, and all
** are acceptable for a debug affordance — see DebugFloatButton.cpp for the mechanism of each:
**   1. While a modal popup is up, taps go to the modal only.
**   2. While the tactical view holds the mouse (camera drag), the window system is bypassed.
**   3. Anchored structure placement lets the button's own mouse-up ALSO reach the world.
**   4. Full-screen video, letterbox and cinematic text render after winRepaint and cover it.
*/

#pragma once

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "Lib/BaseType.h"

/// Build the overlay. Idempotent, and safe before TheDisplay exists (it simply does nothing).
/// Call once from GameClient::init(), after TheWindowManager->init().
void DebugFloatButtonCreate( void );

/// Tear it down and stop DebugFloatButtonUpdate from rebuilding it.
/// Call from ~GameClient before `delete TheWindowManager`.
void DebugFloatButtonDestroy( void );

/// Per-frame keep-alive: rebuilds after a resolution change, and covers a Create that ran too early.
/// Cheap — two integer compares in the steady state. Call from GameClient::update().
void DebugFloatButtonUpdate( void );
