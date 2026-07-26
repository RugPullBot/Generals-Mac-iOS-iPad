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

// FILE: DebugMenu.h ///////////////////////////////////////////////////////////////////////////////
// Desc:   Code-built Debug overlay: stats, cheats, camera tuning, config and saves
///////////////////////////////////////////////////////////////////////////////////////////////////

/*
** GeneralsX @feature Claude 26/07/2026 Debug overlay reachable from the shell AND from in-game.
**
** This is deliberately NOT a Shell screen. Shell::doPush hardcodes winCreateLayout(filename) and
** would run MainMenuShutdown on whatever is underneath, which is wrong for something that also has
** to open on top of a running match. It is an off-stack WindowLayout built with CodeGui, exactly
** the pattern Shell::getOptionsLayout already uses, so the same instance serves both entry points
** and Back is a plain destroy with no pop/shutdownComplete handshake to stall on.
*/

#pragma once

// USER INCLUDES //////////////////////////////////////////////////////////////
#include "Lib/BaseType.h"

void DebugMenuOpen  ( void );		///< build (if needed) and show the overlay
void DebugMenuClose ( void );		///< destroy it; safe to call from inside its own button handler
void DebugMenuToggle( void );		///< open if closed, close if open — for the floating button
Bool DebugMenuIsOpen( void );

/// Refresh the live readouts. Driven by the overlay's own draw func so the screen is self-contained,
/// but exposed so a per-frame owner (GameClient::update) can drive it too. Idempotent and throttled.
void DebugMenuUpdate( void );
