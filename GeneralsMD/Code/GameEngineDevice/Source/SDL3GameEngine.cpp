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

/*
** SDL3GameEngine.cpp
**
** Linux implementation of GameEngine using SDL3 for windowing/input.
**
** TheSuperHackers @feature CnC_Generals_Linux 07/02/2026
** Provides SDL3-based input and window management for Linux builds.
** Based on fighter19 reference implementation.
*/

#ifndef _WIN32

#include "SDL3GameEngine.h"
#include "OpenALAudioManager.h"
#include "OpenALAudioDevice/OpenALAudioStream.h"   // AudioDebugDump()
#include "SDL3Device/GameClient/SDL3Mouse.h"
#include "SDL3Device/GameClient/SDL3Keyboard.h"
#include "GameClient/Mouse.h"
#include "GameClient/Keyboard.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/Gadget.h"
#include "GameClient/Shell.h"
#include "W3DDevice/GameLogic/W3DGameLogic.h"
#include "W3DDevice/GameClient/W3DGameClient.h"
#include "W3DDevice/Common/W3DModuleFactory.h"
#include "W3DDevice/Common/W3DThingFactory.h"
#include "W3DDevice/Common/W3DFunctionLexicon.h"
#include "W3DDevice/Common/W3DRadar.h"
#include "W3DDevice/GameClient/W3DParticleSys.h"
#include "W3DDevice/GameClient/W3DWebBrowser.h"
#include "StdDevice/Common/StdLocalFileSystem.h"
#include "StdDevice/Common/StdBIGFileSystem.h"
#include "Common/GlobalData.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// Extern globals for input devices (set by GameClient)
extern Mouse *TheMouse;
extern Keyboard *TheKeyboard;
extern GameWindowManager *TheWindowManager;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
#include <atomic>

// ---------------------------------------------------------------------------
// iOS app lifecycle
//
// iOS suspends the process when the app leaves the foreground. Any GPU work
// submitted around suspension stalls on drawable acquisition (MoltenVK waits
// out a timeout per present), which surfaces as multi-second input hangs right
// after resuming. SDL warns that lifecycle events can arrive outside the
// normal poll cycle, so they are captured in an event watcher that fires
// immediately on the delivering thread; the engine update loop checks the
// flag and skips simulation + rendering while backgrounded.
// ---------------------------------------------------------------------------
// Two independent reasons to halt the render/sim loop on iOS:
//  - BACKGROUNDED (home / switched away): the process is about to be suspended.
//  - INACTIVE (multitasking switcher open, Control Center, a notification
//    banner): iOS snapshots the window and owns the CAMetalLayer drawable during
//    this window — and crucially, opening the app switcher fires resign-active
//    WITHOUT a full background transition.
// Acquiring a Metal drawable during EITHER state fights iOS for the layer; across
// repeated suspend/switcher cycles MoltenVK is driven into an unrecoverable
// surface state and the app crashes (the reported "crashes after backgrounding /
// multitasking a few times"). Pause whenever either is set.
static std::atomic<bool> s_appBackgrounded{false};
static std::atomic<bool> s_appInactive{false};

static inline bool iosShouldPauseRendering()
{
	return s_appBackgrounded.load() || s_appInactive.load();
}

static bool SDLCALL iosLifecycleWatcher(void *userdata, SDL_Event *event)
{
	switch (event->type) {
		case SDL_EVENT_WILL_ENTER_BACKGROUND:
		case SDL_EVENT_DID_ENTER_BACKGROUND:
			s_appBackgrounded.store(true);
			break;
		case SDL_EVENT_DID_ENTER_FOREGROUND:
			s_appBackgrounded.store(false);
			break;
		// Resign/become active. On iOS, SDL maps applicationWillResignActive ->
		// window focus lost and applicationDidBecomeActive -> window focus gained.
		// Stay paused until fully active again (focus regained), which arrives
		// after DID_ENTER_FOREGROUND.
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			s_appInactive.store(true);
			break;
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			s_appInactive.store(false);
			break;
		default:
			break;
	}
	return true;
}

// ---------------------------------------------------------------------------
// iOS touch -> mouse gesture translation
//
// SDL's automatic touch-mouse synthesis is disabled on iOS (SDL3Main.cpp sets
// SDL_HINT_TOUCH_MOUSE_EVENTS=0); every mouse event the game sees on iOS is
// synthesized here, through the same SDL3Mouse::addSDLEvent path real mice use.
//
// Gestures (matching the game's stock control scheme, which is LMB-centric):
//   1 finger tap/drag     -> left button click / drag (select, command, drag-box)
//   1 finger long-press   -> right button click (deselect), if finger stays put
//   2 finger pinch        -> mouse wheel (camera zoom), camera stays put
//   3 finger drag         -> right-button drag (camera scroll), terrain follows
//                            the fingers rather than running from them
//
// While a shell menu is up there is no camera to move, and the useful gesture is
// scrolling a list (map selection, replays, saves). Two fingers there emit wheel
// ticks from vertical movement instead, and never press the right button.
// ---------------------------------------------------------------------------
namespace {

struct TouchState {
	enum Phase {
		IDLE,        // no fingers tracked
		PENDING,     // finger1 down, gesture identity not yet known, nothing sent
		DRAGGING,    // finger1 drag in progress, LMB held
		LONGPRESSED, // long-press fired (RMB click sent), swallow until lift
		PINCH,       // two-finger zoom, camera anchored, no button held
		PAN,         // three-finger camera pan, RMB held
		SCROLL       // two-finger list scroll in a shell menu, no button held
	};

	Phase phase = IDLE;
	SDL_FingerID finger1 = 0;
	SDL_FingerID finger2 = 0;
	SDL_FingerID finger3 = 0;
	float downX = 0.0f, downY = 0.0f;   // finger1 down position (window points)
	float lastX = 0.0f, lastY = 0.0f;   // finger1 latest position
	float panX = 0.0f, panY = 0.0f;     // pan centroid
	float pinchDist = 0.0f;             // finger distance at last wheel step
	float scrollAccum = 0.0f;           // unspent vertical travel while SCROLLing
	Uint64 downTicks = 0;
	float f1x = 0.0f, f1y = 0.0f, f2x = 0.0f, f2y = 0.0f; // normalized per finger
	float f3x = 0.0f, f3y = 0.0f;
	float lastDist = 0.0f;                 // finger spread at the previous motion
	float lastCx = 0.0f, lastCy = 0.0f;    // centroid at the previous motion
	bool  suppressRightButton = false;     // placement mode: RMB would cancel
	bool  rightButtonHeld = false;         // have we actually pressed RMB?
	bool  placementAnchored = false;       // LMB held to set a building's facing
	float cursorX = 0.0f, cursorY = 0.0f;  // where the synthetic cursor actually is
	bool  relativeDrag = false;            // move the cursor by deltas, not absolutely
	int   activeFingers = 0;               // fingers currently down, any phase
	Uint64 pinchTicks = 0;                 // when the two-finger gesture started

	// Two-finger intent, decided once per gesture and then held. Comparing the
	// two per-event lets the winner flip frame to frame — a pinch where one
	// finger travels further than the other swings the midpoint a long way, so
	// "pan" keeps winning individual frames and the camera crawls while you are
	// plainly just zooming. Measure both from where the gesture started, commit
	// to whichever crosses the threshold first, and ignore the other until the
	// fingers lift.
	enum Intent { UNDECIDED, ZOOMING, PANNING };
	Intent intent = UNDECIDED;
	float startDist = 0.0f;
	float startCx = 0.0f, startCy = 0.0f;
};

TouchState s_touch;

const Uint64 LONG_PRESS_MS = 600;
const float PINCH_STEP_RATIO = 0.06f;  // 6% distance change per wheel tick
const float TAP_DEAD_ZONE_PX = 8.0f;   // jitter below this keeps a tap a tap
const float SCROLL_STEP_PX = 36.0f;    // vertical travel per wheel tick in menus
const float GESTURE_COMMIT_PX = 24.0f; // travel needed to commit to zoom or pan
const Uint64 TWO_FINGER_TAP_MS = 250;  // quick two-finger tap = dismiss/back

void sendSyntheticMouse(SDL3Mouse *mouse, SDL_Window *window, Uint32 type,
                        float x, float y, Uint8 button = 0, float wheelY = 0.0f)
{
	// The windowID must be valid: SDL3Mouse::scaleMouseCoordinates() looks the
	// window up by id to map window points into the game's internal resolution,
	// and silently skips scaling when the lookup fails.
	const SDL_WindowID windowID = SDL_GetWindowID(window);

	// Remember where the synthetic cursor is. Placement dragging moves it by a
	// delta rather than jumping it to the finger midpoint, and needs this anchor.
	if (type == SDL_EVENT_MOUSE_MOTION) {
		s_touch.cursorX = x;
		s_touch.cursorY = y;
	}

	SDL_Event ev;
	SDL_zero(ev);
	ev.type = type;
	switch (type) {
		case SDL_EVENT_MOUSE_MOTION:
			ev.motion.windowID = windowID;
			ev.motion.x = x;
			ev.motion.y = y;
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
			ev.button.windowID = windowID;
			ev.button.button = button;
			ev.button.down = (type == SDL_EVENT_MOUSE_BUTTON_DOWN);
			ev.button.clicks = 1;
			ev.button.x = x;
			ev.button.y = y;
			break;
		case SDL_EVENT_MOUSE_WHEEL:
			ev.wheel.windowID = windowID;
			ev.wheel.x = 0.0f;
			ev.wheel.y = wheelY;
			ev.wheel.mouse_x = x;
			ev.wheel.mouse_y = y;
			break;
	}
	mouse->addSDLEvent(&ev);
}

// Two fingers land: a shell menu scrolls its list, otherwise we zoom.
//
// Zoom deliberately holds no button and emits no motion. Driving the camera and
// the zoom from the same two fingers meant every pinch also dragged the view,
// which makes precise zooming impossible — the anchor slides out from under you.
// Panning now needs a third finger (beginPan below).
// A quick two-finger tap means "dismiss / back". ESC is what the engine already
// treats as cancel everywhere — menus, popups, placement, the in-game menu — so
// synthesising it reuses all of that rather than special-casing each screen.
void sendSyntheticEscape(SDL_Window *window)
{
	SDL3Keyboard *keyboard = dynamic_cast<SDL3Keyboard *>(TheKeyboard);
	if (keyboard == nullptr) {
		return;
	}
	const SDL_WindowID windowID = SDL_GetWindowID(window);
	for (int press = 1; press >= 0; --press) {
		SDL_Event ev;
		SDL_zero(ev);
		ev.type = press ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
		ev.key.windowID = windowID;
		ev.key.scancode = SDL_SCANCODE_ESCAPE;
		ev.key.key = SDLK_ESCAPE;
		ev.key.down = press ? true : false;
		ev.key.repeat = false;
		keyboard->addSDLEvent(&ev);
	}
}

void beginTwoFinger(SDL3Mouse *mouse, SDL_Window *window, int winW, int winH)
{
	s_touch.panX = (s_touch.f1x + s_touch.f2x) * 0.5f * (float)winW;
	s_touch.panY = (s_touch.f1y + s_touch.f2y) * 0.5f * (float)winH;
	const float dx = (s_touch.f1x - s_touch.f2x) * (float)winW;
	const float dy = (s_touch.f1y - s_touch.f2y) * (float)winH;
	s_touch.pinchDist = SDL_sqrtf(dx * dx + dy * dy);

	// In a shell menu two fingers mean "scroll this list", not "zoom the camera".
	// Hold no button: an RMB press on a listbox is at best meaningless and the
	// wheel is what the gadgets actually listen for.
	if (TheShell != nullptr && TheShell->isShellActive()) {
		s_touch.scrollAccum = 0.0f;
		s_touch.phase = TouchState::SCROLL;
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.panX, s_touch.panY);
		return;
	}

	// Two fingers drive both zoom and camera, but never in the same event: see the
	// PINCH motion handler, which picks whichever of the two actually moved.
	s_touch.lastDist = s_touch.pinchDist;
	s_touch.lastCx = s_touch.panX;
	s_touch.lastCy = s_touch.panY;
	s_touch.intent = TouchState::UNDECIDED;
	s_touch.pinchTicks = SDL_GetTicks();
	s_touch.startDist = s_touch.pinchDist;
	s_touch.startCx = s_touch.panX;
	s_touch.startCy = s_touch.panY;
	// While a building is waiting to be placed, the right button means "cancel", so
	// it must never be pressed during placement — two fingers there only move the
	// cursor, which drags the ghost where you want it.
	s_touch.suppressRightButton =
		(TheInGameUI != nullptr && TheInGameUI->getPendingPlaceType() != nullptr);

	// During placement, do not teleport the cursor to the midpoint between the two
	// fingers. The ghost building follows the cursor, so jumping it to the centroid
	// the instant the second finger lands moves the building away from where the
	// player was actually pointing — and that is where it then gets placed. Anchor
	// on the current cursor position and apply finger movement as a delta instead.
	if (s_touch.suppressRightButton) {
		s_touch.relativeDrag = true;
	}

	// Do NOT press the right button yet, even outside placement.
	//
	// The engine's right-drag scroll (SCROLL_RMB) is velocity-based: while the
	// button is held the camera keeps scrolling from the cursor's displacement,
	// with no further input needed. Pressing it as soon as a second finger landed
	// meant every pinch ran on top of a live scroll gesture — the view drifted
	// during the zoom and kept drifting afterwards while the fingers rested. The
	// button is now pressed only once the gesture commits to panning.
	s_touch.rightButtonHeld = false;
	s_touch.phase = TouchState::PINCH;
}

// Third finger joins a pinch: keep dragging the camera, now from the three-finger
// centroid. The cursor tracks the centroid directly — the engine's right-drag
// scroll direction is what players already expect.
void beginPan(SDL3Mouse *mouse, SDL_Window *window, int winW, int winH)
{
	s_touch.panX = (s_touch.f1x + s_touch.f2x + s_touch.f3x) * (1.0f / 3.0f) * (float)winW;
	s_touch.panY = (s_touch.f1y + s_touch.f2y + s_touch.f3y) * (1.0f / 3.0f) * (float)winH;
	sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.panX, s_touch.panY);
	// A third finger is an explicit request to move the camera, so take the right
	// button here if the pinch had not already committed to panning. Never during
	// placement, where the right button cancels.
	if (!s_touch.suppressRightButton && !s_touch.rightButtonHeld) {
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
		                   s_touch.panX, s_touch.panY, SDL_BUTTON_RIGHT);
		s_touch.rightButtonHeld = true;
	}
	s_touch.phase = TouchState::PAN;
}

void handleTouchEvent(SDL3Mouse *mouse, SDL_Window *window, const SDL_Event &event)
{
	int winW = 0, winH = 0;
	SDL_GetWindowSize(window, &winW, &winH);
	const float px = event.tfinger.x * (float)winW;
	const float py = event.tfinger.y * (float)winH;

	switch (event.type) {
	case SDL_EVENT_FINGER_DOWN:
		// Audio diagnostic trigger, counted independently of the gesture state
		// machine. It was previously hooked to the three-finger PAN phase, which
		// does not exist on menu screens — so on the very screen where the audio
		// fault shows up, a four-finger tap did nothing at all.
		++s_touch.activeFingers;
		if (s_touch.activeFingers == 4) {
			AudioDebugDump("four-finger tap");
		}
		else if (s_touch.activeFingers == 5) {
			// Five fingers = ESC: the in-game menu / pause, or back out of a screen.
			// Deliberately not two — that is the deselect tap — and not three, which
			// pans. Five is unreachable by accident during play.
			sendSyntheticEscape(window);
		}
		if (s_touch.phase == TouchState::IDLE) {
			// Defer all BUTTON output: a finger landing could become a tap, a
			// drag-box, a long-press, or the first finger of a camera pan. A
			// premature LMB down+up is a real click to the game (e.g. it sets a
			// rally point when a production building is selected).
			s_touch.finger1 = event.tfinger.fingerID;
			s_touch.phase = TouchState::PENDING;
			s_touch.downX = s_touch.lastX = px;
			s_touch.downY = s_touch.lastY = py;
			s_touch.f1x = event.tfinger.x;
			s_touch.f1y = event.tfinger.y;
			s_touch.downTicks = SDL_GetTicks();
			// Move the cursor to the touch point NOW (motion clicks nothing, so the
			// deferred-tap protection is intact). This lets the GUI process hover
			// over the next frame(s) before the tap commits — hover-driven widgets
			// (e.g. the Generals Challenge general buttons, which are checkboxes
			// that ignore a click unless WIN_STATE_HILITED was set by a prior
			// mouse-enter) then accept the click. Real mice hover before clicking;
			// without this, a synthetic tap teleports + clicks in one instant and
			// the widget is never hilited, so only the default/first item responds.
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, px, py);
		}
		else if (s_touch.phase == TouchState::PENDING) {
			// Second finger before the first committed to anything: zoom (or list
			// scroll in a menu), no left-click ever happened.
			s_touch.finger2 = event.tfinger.fingerID;
			s_touch.f2x = event.tfinger.x;
			s_touch.f2y = event.tfinger.y;
			beginTwoFinger(mouse, window, winW, winH);
		}
		else if (s_touch.phase == TouchState::DRAGGING) {
			// Second finger during a live drag: finish the drag-box, then zoom.
			s_touch.finger2 = event.tfinger.fingerID;
			s_touch.f2x = event.tfinger.x;
			s_touch.f2y = event.tfinger.y;
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
			                   s_touch.lastX, s_touch.lastY, SDL_BUTTON_LEFT);
			beginTwoFinger(mouse, window, winW, winH);
		}
		else if (s_touch.phase == TouchState::PINCH) {
			s_touch.finger3 = event.tfinger.fingerID;
			s_touch.f3x = event.tfinger.x;
			s_touch.f3y = event.tfinger.y;
			if (s_touch.suppressRightButton) {
				// Placing a building: the engine already has a rotate gesture — press
				// and drag sets the facing (m_placeAnchorInProgress). Two fingers have
				// the ghost where it should go, so the third takes the left button
				// there and its movement swings the angle. Releasing commits, exactly
				// as a mouse press-drag-release would.
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION,
				                   s_touch.panX, s_touch.panY);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
				                   s_touch.panX, s_touch.panY, SDL_BUTTON_LEFT);
				s_touch.placementAnchored = true;
				s_touch.phase = TouchState::PAN;
			} else {
				// Third finger promotes the gesture to a camera pan.
				beginPan(mouse, window, winW, winH);
			}
		}
		// LONGPRESSED / PAN / SCROLL with extra fingers: ignored
		break;

	case SDL_EVENT_FINGER_MOTION:
		if (event.tfinger.fingerID == s_touch.finger1) {
			s_touch.f1x = event.tfinger.x;
			s_touch.f1y = event.tfinger.y;
			s_touch.lastX = px;
			s_touch.lastY = py;
		} else if ((s_touch.phase == TouchState::PINCH || s_touch.phase == TouchState::PAN ||
		            s_touch.phase == TouchState::SCROLL) &&
		           event.tfinger.fingerID == s_touch.finger2) {
			s_touch.f2x = event.tfinger.x;
			s_touch.f2y = event.tfinger.y;
		} else if (s_touch.phase == TouchState::PAN && event.tfinger.fingerID == s_touch.finger3) {
			s_touch.f3x = event.tfinger.x;
			s_touch.f3y = event.tfinger.y;
		} else {
			break;
		}

		if (s_touch.phase == TouchState::PENDING && event.tfinger.fingerID == s_touch.finger1) {
			const float moved = SDL_fabsf(px - s_touch.downX) + SDL_fabsf(py - s_touch.downY);
			if (moved >= TAP_DEAD_ZONE_PX) {
				// Commit to a drag: anchor the LMB at the original touch point so
				// drag-boxes start where the finger first landed.
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.downX, s_touch.downY);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
				                   s_touch.downX, s_touch.downY, SDL_BUTTON_LEFT);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, px, py);
				s_touch.phase = TouchState::DRAGGING;
			}
		}
		else if (s_touch.phase == TouchState::DRAGGING && event.tfinger.fingerID == s_touch.finger1) {
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, px, py);
		}
		else if (s_touch.phase == TouchState::SCROLL) {
			// Content follows the fingers, as everywhere else on the platform:
			// dragging down reveals earlier entries, i.e. a wheel-up tick.
			const float cy = (s_touch.f1y + s_touch.f2y) * 0.5f * (float)winH;
			s_touch.scrollAccum += (cy - s_touch.panY);
			s_touch.panY = cy;
			s_touch.panX = (s_touch.f1x + s_touch.f2x) * 0.5f * (float)winW;
			while (s_touch.scrollAccum >= SCROLL_STEP_PX) {
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_WHEEL,
				                   s_touch.panX, s_touch.panY, 0, 1.0f);
				s_touch.scrollAccum -= SCROLL_STEP_PX;
			}
			while (s_touch.scrollAccum <= -SCROLL_STEP_PX) {
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_WHEEL,
				                   s_touch.panX, s_touch.panY, 0, -1.0f);
				s_touch.scrollAccum += SCROLL_STEP_PX;
			}
		}
		else if (s_touch.phase == TouchState::PINCH) {
			// A two-finger gesture is either a zoom or a pan, never both, and which
			// one it is gets decided once. Both candidates are measured from where
			// the gesture began, in the same units, and the first to travel far
			// enough wins for the rest of the gesture.
			const float cx = (s_touch.f1x + s_touch.f2x) * 0.5f * (float)winW;
			const float cy = (s_touch.f1y + s_touch.f2y) * 0.5f * (float)winH;
			const float dx = (s_touch.f1x - s_touch.f2x) * (float)winW;
			const float dy = (s_touch.f1y - s_touch.f2y) * (float)winH;
			const float dist = SDL_sqrtf(dx * dx + dy * dy);

			if (s_touch.intent == TouchState::UNDECIDED) {
				const float spreadTravel = SDL_fabsf(dist - s_touch.startDist);
				const float centreDx = cx - s_touch.startCx;
				const float centreDy = cy - s_touch.startCy;
				const float centreTravel = SDL_sqrtf(centreDx * centreDx + centreDy * centreDy);

				if (spreadTravel >= GESTURE_COMMIT_PX && spreadTravel >= centreTravel) {
					s_touch.intent = TouchState::ZOOMING;
				} else if (centreTravel >= GESTURE_COMMIT_PX) {
					s_touch.intent = TouchState::PANNING;
					// Re-base the pan so it starts from here rather than snapping the
					// camera by the distance already travelled while deciding.
					s_touch.panX = cx;
					s_touch.panY = cy;
					// Now, and only now, take hold of the right button — anchoring the
					// scroll where the pan actually begins.
					if (!s_touch.suppressRightButton && !s_touch.rightButtonHeld) {
						sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, cx, cy);
						sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
						                   cx, cy, SDL_BUTTON_RIGHT);
						s_touch.rightButtonHeld = true;
					}
				}
			}

			if (s_touch.intent == TouchState::ZOOMING) {
				if (s_touch.pinchDist > 1.0f) {
					const float ratio = dist / s_touch.pinchDist;
					if (ratio > 1.0f + PINCH_STEP_RATIO) {
						sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_WHEEL, cx, cy, 0, 1.0f);
						s_touch.pinchDist = dist;
					} else if (ratio < 1.0f - PINCH_STEP_RATIO) {
						sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_WHEEL, cx, cy, 0, -1.0f);
						s_touch.pinchDist = dist;
					}
				}
			}
			else if (s_touch.intent == TouchState::PANNING) {
				if (s_touch.relativeDrag) {
					// Placement: shift the ghost by how far the fingers moved, keeping
					// it under where the player was already pointing.
					const float nx = s_touch.cursorX + (cx - s_touch.lastCx);
					const float ny = s_touch.cursorY + (cy - s_touch.lastCy);
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, nx, ny);
					s_touch.panX = nx;
					s_touch.panY = ny;
				} else {
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, cx, cy);
					s_touch.panX = cx;
					s_touch.panY = cy;
				}
				s_touch.pinchDist = dist;
			}
			// UNDECIDED: emit nothing. A gesture below the threshold is indistinguishable
			// from resting fingers, and acting on it is what made the camera creep.

			s_touch.lastDist = dist;
			s_touch.lastCx = cx;
			s_touch.lastCy = cy;
		}
		else if (s_touch.phase == TouchState::PAN) {
			// Rotating a building follows the third finger alone: the facing is the
			// direction from the anchor to the cursor, so averaging in the two fingers
			// holding position would drag the angle back towards them.
			const float cx = s_touch.placementAnchored
				? (s_touch.f3x * (float)winW)
				: ((s_touch.f1x + s_touch.f2x + s_touch.f3x) * (1.0f / 3.0f) * (float)winW);
			const float cy = s_touch.placementAnchored
				? (s_touch.f3y * (float)winH)
				: ((s_touch.f1y + s_touch.f2y + s_touch.f3y) * (1.0f / 3.0f) * (float)winH);
			sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, cx, cy);
			s_touch.panX = cx;
			s_touch.panY = cy;
		}
		break;

	case SDL_EVENT_FINGER_UP:
	case SDL_EVENT_FINGER_CANCELED:
		if (s_touch.activeFingers > 0) {
			--s_touch.activeFingers;
		}
		// Once the screen is clear the machine MUST return to IDLE, whichever finger
		// happened to lift last. This is checked before the "not a finger we track"
		// early-out below: that early-out used to skip the reset, so lifting finger1
		// first left the phase parked at LONGPRESSED and the lift of the remaining
		// finger returned here and broke out — stranding the machine in an inert
		// state with no way back. Every subsequent touch was swallowed: the game kept
		// rendering but accepted no input at all.
		if (s_touch.activeFingers <= 0) {
			s_touch.activeFingers = 0;
			s_touch.relativeDrag = false;
			if (s_touch.rightButtonHeld) {
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
				                   s_touch.panX, s_touch.panY, SDL_BUTTON_RIGHT);
				s_touch.rightButtonHeld = false;
			}
			if (s_touch.placementAnchored) {
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
				                   s_touch.panX, s_touch.panY, SDL_BUTTON_LEFT);
				s_touch.placementAnchored = false;
			}
			// Set it here as well as at the bottom: the early-out below can skip the
			// bottom entirely, and leaving the phase non-IDLE with no fingers down is
			// exactly what killed input.
			if (s_touch.phase != TouchState::PENDING &&
			    s_touch.phase != TouchState::DRAGGING) {
				s_touch.phase = TouchState::IDLE;
			}
		}
		if (event.tfinger.fingerID != s_touch.finger1 &&
		    !((s_touch.phase == TouchState::PINCH || s_touch.phase == TouchState::PAN ||
		       s_touch.phase == TouchState::SCROLL) &&
		      event.tfinger.fingerID == s_touch.finger2) &&
		    !(s_touch.phase == TouchState::PAN && event.tfinger.fingerID == s_touch.finger3)) {
			break;
		}
		switch (s_touch.phase) {
			case TouchState::PENDING:
				// A CANCELED touch (incoming call, notification shade, palm
				// rejection) must not become a committed tap — that would be a
				// phantom select/command/rally-point click at the cancel point.
				if (event.type == SDL_EVENT_FINGER_CANCELED) {
					break;
				}
				// Clean tap: deliver the full click at the exact press position.
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.downX, s_touch.downY);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
				                   s_touch.downX, s_touch.downY, SDL_BUTTON_LEFT);
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
				                   s_touch.downX, s_touch.downY, SDL_BUTTON_LEFT);
				break;
			case TouchState::DRAGGING:
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP, px, py, SDL_BUTTON_LEFT);
				break;
			case TouchState::PAN:
				if (s_touch.placementAnchored) {
					// Releasing the rotate drag commits the building at its chosen angle.
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
					                   s_touch.panX, s_touch.panY, SDL_BUTTON_LEFT);
					s_touch.placementAnchored = false;
				}
				if (s_touch.rightButtonHeld) {
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
					                   s_touch.panX, s_touch.panY, SDL_BUTTON_RIGHT);
					s_touch.rightButtonHeld = false;
				}
				break;
			case TouchState::PINCH:
				// A CANCELED touch is the OS taking the gesture away (palm rejection,
				// a notification banner, an incoming call). It is not a decision by
				// the player, so it must not commit a placement or fire a click.
				if (event.type == SDL_EVENT_FINGER_CANCELED) {
					if (s_touch.rightButtonHeld) {
						sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
						                   s_touch.panX, s_touch.panY, SDL_BUTTON_RIGHT);
						s_touch.rightButtonHeld = false;
					}
					break;
				}
				// Two fingers on and straight back off, having committed to neither
				// zoom nor pan, is a right-click: deselect units, cancel a pending
				// placement, back out of a command. That is what the engine already
				// binds to the right button, so this reuses it everywhere instead of
				// special-casing screens. Previously this only happened by adding a
				// third finger, whose beginPan() press incidentally deselected.
				//
				// The gesture-intent lock does the hard part: UNDECIDED means neither
				// zoom nor pan travelled far enough, so a real pinch or drag can never
				// be mistaken for a tap.
				if (s_touch.intent == TouchState::UNDECIDED &&
				    (SDL_GetTicks() - s_touch.pinchTicks) <= TWO_FINGER_TAP_MS) {
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION,
					                   s_touch.panX, s_touch.panY);
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
					                   s_touch.panX, s_touch.panY, SDL_BUTTON_RIGHT);
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
					                   s_touch.panX, s_touch.panY, SDL_BUTTON_RIGHT);
					break;
				}
				if (s_touch.suppressRightButton) {
					// Placement mode: the two fingers were dragging the ghost, so
					// lifting them means "put it here". Without this the building
					// stayed pending and needed a separate tap to commit, which also
					// moved it again to wherever that tap landed.
					//
					// Not after a zoom: spreading two fingers to look around is not a
					// decision to build.
					if (s_touch.intent != TouchState::ZOOMING) {
						sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION,
						                   s_touch.panX, s_touch.panY);
						sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
						                   s_touch.panX, s_touch.panY, SDL_BUTTON_LEFT);
						sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
						                   s_touch.panX, s_touch.panY, SDL_BUTTON_LEFT);
					}
				} else if (s_touch.rightButtonHeld) {
					sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
					                   s_touch.panX, s_touch.panY, SDL_BUTTON_RIGHT);
					s_touch.rightButtonHeld = false;
				}
				break;
			case TouchState::SCROLL:
				// No button was ever pressed, so nothing to release.
				break;
			default:
				break;
		}
		// Resetting on the first lift while other fingers are still down would let
		// the leftovers read as a fresh gesture — a one-finger tap commits a click
		// wherever it happens to be, which in placement mode looks like the building
		// jumping. So stay inert until the hand actually leaves the glass; the
		// unconditional check at the top of this handler guarantees we get back to
		// IDLE even if the last finger to lift is not one we were tracking.
		if (s_touch.activeFingers <= 0) {
			s_touch.phase = TouchState::IDLE;
		} else {
			s_touch.phase = TouchState::LONGPRESSED;  // inert: swallows input until release
		}
		break;
	}
}

// Called once per engine frame (not just per touch event): a perfectly
// stationary finger produces no SDL events, so the long-press timer must be
// polled from the frame loop or it would never fire.
void updateTouchLongPress(SDL3Mouse *mouse, SDL_Window *window)
{
	// Watchdog against a stranded gesture.
	//
	// The state machine tracks fingers by counting DOWN and UP events, and if that
	// count ever drifts — a lost event across an app suspend, a cancel the OS never
	// pairs, a finger that lands during a modal — the machine can sit in a
	// non-IDLE phase forever and silently swallow every touch. The symptom is
	// brutal and non-obvious: the game keeps rendering and simulating but accepts
	// no input. Ask SDL what is genuinely on the glass and recover.
	if (s_touch.phase != TouchState::IDLE) {
		int liveFingers = 0;
		int numDevices = 0;
		if (SDL_TouchID *devices = SDL_GetTouchDevices(&numDevices)) {
			for (int d = 0; d < numDevices; ++d) {
				int n = 0;
				if (SDL_Finger **f = SDL_GetTouchFingers(devices[d], &n)) {
					liveFingers += n;
					SDL_free(f);
				}
			}
			SDL_free(devices);
		}
		if (liveFingers == 0) {
			if (s_touch.rightButtonHeld) {
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
				                   s_touch.panX, s_touch.panY, SDL_BUTTON_RIGHT);
				s_touch.rightButtonHeld = false;
			}
			if (s_touch.placementAnchored) {
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
				                   s_touch.panX, s_touch.panY, SDL_BUTTON_LEFT);
				s_touch.placementAnchored = false;
			}
			if (s_touch.phase == TouchState::DRAGGING) {
				sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
				                   s_touch.lastX, s_touch.lastY, SDL_BUTTON_LEFT);
			}
			s_touch.activeFingers = 0;
			s_touch.phase = TouchState::IDLE;
		}
	}

	if (s_touch.phase == TouchState::PENDING &&
	    (SDL_GetTicks() - s_touch.downTicks) >= LONG_PRESS_MS) {
		// No LMB was sent yet (deferred), so this is a pure right-click.
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_MOTION, s_touch.downX, s_touch.downY);
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_DOWN,
		                   s_touch.downX, s_touch.downY, SDL_BUTTON_RIGHT);
		sendSyntheticMouse(mouse, window, SDL_EVENT_MOUSE_BUTTON_UP,
		                   s_touch.downX, s_touch.downY, SDL_BUTTON_RIGHT);
		s_touch.phase = TouchState::LONGPRESSED;
	}
}

} // anonymous namespace
#endif // TARGET_OS_IPHONE

namespace {

Bool DecodeNextUtf8Codepoint(const char* text, size_t length, size_t& offset, UnsignedInt& outCodepoint)
{
	outCodepoint = 0;
	if (!text || offset >= length) {
		return false;
	}

	const unsigned char first = static_cast<unsigned char>(text[offset]);
	if (first == 0) {
		return false;
	}

	if (first < 0x80) {
		outCodepoint = first;
		offset += 1;
		return true;
	}

	if ((first & 0xE0) == 0xC0 && offset + 1 < length) {
		const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
		if ((second & 0xC0) == 0x80) {
			outCodepoint = ((first & 0x1F) << 6) | (second & 0x3F);
			offset += 2;
			return true;
		}
	}

	if ((first & 0xF0) == 0xE0 && offset + 2 < length) {
		const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
		const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
		if ((second & 0xC0) == 0x80 && (third & 0xC0) == 0x80) {
			outCodepoint = ((first & 0x0F) << 12) | ((second & 0x3F) << 6) | (third & 0x3F);
			offset += 3;
			return true;
		}
	}

	if ((first & 0xF8) == 0xF0 && offset + 3 < length) {
		const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
		const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
		const unsigned char fourth = static_cast<unsigned char>(text[offset + 3]);
		if ((second & 0xC0) == 0x80 && (third & 0xC0) == 0x80 && (fourth & 0xC0) == 0x80) {
			outCodepoint = ((first & 0x07) << 18) | ((second & 0x3F) << 12) | ((third & 0x3F) << 6) | (fourth & 0x3F);
			offset += 4;
			return true;
		}
	}

	// Invalid UTF-8 sequence: skip one byte and keep processing.
	offset += 1;
	return false;
}

}

/**
 * Constructor: Initialize SDL3 game engine state
 */
SDL3GameEngine::SDL3GameEngine()
	: GameEngine(),
	  m_SDLWindow(nullptr),
	  m_IsInitialized(false),
	  m_IsActive(false),
	  m_IsTextInputActive(false),
	  m_TextInputFocusWindow(nullptr)
{
	fprintf(stderr, "DEBUG: SDL3GameEngine::SDL3GameEngine() created\n");
}

/**
 * Destructor: Cleanup SDL3 resources
 */
SDL3GameEngine::~SDL3GameEngine()
{
	if (m_SDLWindow && m_IsTextInputActive) {
		SDL_StopTextInput(m_SDLWindow);
		m_IsTextInputActive = false;
		m_TextInputFocusWindow = nullptr;
	}

	if (m_IsInitialized) {
		// Window cleanup is done in reset/shutdown
	}
	fprintf(stderr, "DEBUG: SDL3GameEngine::~SDL3GameEngine() destroyed\n");
}

/**
 * From GameEngine: init() - initialize subsystems
 * 
 * GeneralsX @bugfix felipebraz 16/02/2026
 * Simplified to follow fighter19 pattern - SDL3/Vulkan initialized in SDL3Main.cpp
 * before GameEngine is created. This init() only delegates to parent GameEngine::init().
 * ApplicationHWnd and TheSDL3Window are already set by main() before this is called.
 */
void SDL3GameEngine::init(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::init() starting\n");

	if (TheGlobalData && TheGlobalData->m_headless) {
		// GeneralsX @bugfix Copilot 17/05/2026 Allow headless replay path to initialize engine subsystems without an SDL window.
		fprintf(stderr, "INFO: SDL3GameEngine::init() headless mode - skipping SDL window binding\n");
		m_SDLWindow = nullptr;
		m_IsInitialized = true;
		m_IsActive = true;
		GameEngine::init();
		return;
	}

	// Verify window was created by SDL3Main.cpp
	extern SDL_Window* TheSDL3Window;
	extern HWND ApplicationHWnd;
	
	if (!TheSDL3Window || !ApplicationHWnd) {
		fprintf(stderr, "FATAL: SDL3 window not initialized before GameEngine::init()\n");
		fprintf(stderr, "FATAL: TheSDL3Window=%p, ApplicationHWnd=%p\n", TheSDL3Window, ApplicationHWnd);
		return;
	}

	// Store window reference locally
	m_SDLWindow = TheSDL3Window;
	m_IsInitialized = true;
	m_IsActive = true;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// Lifecycle events can fire outside the poll cycle on iOS; catch them
	// immediately so rendering halts before the process is suspended.
	SDL_AddEventWatch(iosLifecycleWatcher, nullptr);
#endif

	fprintf(stderr, "INFO: SDL3GameEngine using pre-initialized window\n");

	// Call parent init to initialize game subsystems
	GameEngine::init();
}

/**
 * From GameEngine: reset() - reset system to starting state
 */
void SDL3GameEngine::reset(void)
{
	fprintf(stderr, "DEBUG: SDL3GameEngine::reset()\n");
	if (m_SDLWindow && m_IsTextInputActive) {
		SDL_StopTextInput(m_SDLWindow);
		m_IsTextInputActive = false;
		m_TextInputFocusWindow = nullptr;
	}
	GameEngine::reset();
}

/**
 * From GameEngine: update() - per-frame update
 */
void SDL3GameEngine::update(void)
{
	pollSDL3Events();
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// Pause sim + render while backgrounded OR inactive (see iosLifecycleWatcher).
	// Acquiring a Metal drawable in these windows fights iOS for the layer and,
	// across repeated suspend/switcher cycles, crashes MoltenVK. Keep polling so
	// we still catch the resume events; just don't touch the GPU.
	if (iosShouldPauseRendering()) {
		SDL_Delay(50);
		return;
	}
#endif
	GameEngine::update();
}

/**
 * From GameEngine: execute() - main game loop
 */
void SDL3GameEngine::execute(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::execute() - entering main loop\n");
	GameEngine::execute();
	fprintf(stderr, "INFO: SDL3GameEngine::execute() - exited main loop\n");
}

/**
 * From GameEngine: serviceWindowsOS() - native OS service
 * On Linux, process SDL3 events
 */
void SDL3GameEngine::serviceWindowsOS(void)
{
	pollSDL3Events();
}

/**
 * Check if game has OS focus
 */
Bool SDL3GameEngine::isActive(void)
{
	return m_IsActive;
}

/**
 * Set OS focus status
 */
void SDL3GameEngine::setIsActive(Bool isActive)
{
	m_IsActive = isActive;
}

/**
 * Poll and process SDL3 events
 * Handles keyboard, mouse, window, and quit events
 */
void SDL3GameEngine::pollSDL3Events(void)
{
	if (!m_SDLWindow) {
		return;
	}

	updateTextInputState();

	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		switch (event.type) {
			case SDL_EVENT_QUIT:
				m_quitting = true;
				break;

			case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
				m_quitting = true;
				break;

			case SDL_EVENT_WINDOW_FOCUS_GAINED:
				m_IsActive = true;
				if (TheMouse) {
					TheMouse->regainFocus();
					TheMouse->refreshCursorCapture();
				}
				break;

			case SDL_EVENT_WINDOW_FOCUS_LOST:
				m_IsActive = false;
				if (m_IsTextInputActive) {
					SDL_StopTextInput(m_SDLWindow);
					m_IsTextInputActive = false;
					m_TextInputFocusWindow = nullptr;
				}
				if (TheMouse) {
					TheMouse->loseFocus();
				}
				break;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
			// App suspension/resume: mirror the desktop focus handling so audio
			// and mouse state pause cleanly (the render gate lives in update()).
			case SDL_EVENT_DID_ENTER_BACKGROUND:
				m_IsActive = false;
				if (TheMouse) {
					TheMouse->loseFocus();
				}
				break;

			case SDL_EVENT_DID_ENTER_FOREGROUND:
				m_IsActive = true;
				if (TheMouse) {
					TheMouse->regainFocus();
					TheMouse->refreshCursorCapture();
				}
				break;
#endif

			case SDL_EVENT_WINDOW_MOUSE_ENTER:
				if (TheMouse) {
					TheMouse->onCursorMovedInside();
				}
				break;

			case SDL_EVENT_WINDOW_MOUSE_LEAVE:
				if (TheMouse) {
					TheMouse->onCursorMovedOutside();
				}
				break;

			case SDL_EVENT_KEY_DOWN:
			case SDL_EVENT_KEY_UP:
				// F9 dumps the audio diagnostic ring. Handled here rather than through
				// the game's keybinding layer so it works on any screen, including
				// while a menu has focus.
				if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F9 &&
				    !event.key.repeat) {
					AudioDebugDump("F9 pressed");
				}
				// Fighter19 pattern: direct addSDLEvent() call
				// GeneralsX @refactor felipebraz 16/02/2026 Simplified event routing
				if (TheKeyboard) {
					SDL3Keyboard* keyboard = dynamic_cast<SDL3Keyboard*>(TheKeyboard);
					if (keyboard) {
						keyboard->addSDLEvent(&event);
					}
				}
				break;

			case SDL_EVENT_TEXT_INPUT:
				forwardTextInputEvent(event.text.text);
				break;

			case SDL_EVENT_MOUSE_MOTION:
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
			case SDL_EVENT_MOUSE_BUTTON_UP:
			case SDL_EVENT_MOUSE_WHEEL:
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
				// Belt-and-braces: drop SDL's own touch-synthesized mouse events.
				// The gesture translator owns all touch->mouse conversion; double
				// delivery would produce phantom second clicks.
				if (event.motion.which == SDL_TOUCH_MOUSEID) {
					break;
				}
#endif
				// Fighter19 pattern: direct addSDLEvent() call with raw SDL_Event
				// GeneralsX @refactor felipebraz 16/02/2026 Simplified event routing
				if (TheMouse) {
					SDL3Mouse* mouse = dynamic_cast<SDL3Mouse*>(TheMouse);
					if (mouse) {
						mouse->addSDLEvent(&event);
					}
				}
				break;

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
			case SDL_EVENT_FINGER_DOWN:
			case SDL_EVENT_FINGER_MOTION:
			case SDL_EVENT_FINGER_UP:
			case SDL_EVENT_FINGER_CANCELED:
				if (TheMouse && m_SDLWindow) {
					SDL3Mouse* mouse = dynamic_cast<SDL3Mouse*>(TheMouse);
					if (mouse) {
						handleTouchEvent(mouse, m_SDLWindow, event);
					}
				}
				break;
#endif

			case SDL_EVENT_WINDOW_RESIZED:
				handleWindowEvent(event.window);
				break;

			default:
				// Ignore other events for now
				break;
		}

		updateTextInputState();
	}

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// Poll the long-press timer every frame; a stationary finger emits no events.
	if (TheMouse && m_SDLWindow) {
		SDL3Mouse* touchMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (touchMouse) {
			updateTouchLongPress(touchMouse, m_SDLWindow);
		}
	}
#endif
}

// GeneralsX @bugfix felipebraz 01/04/2026 Enable SDL text input only while an entry gadget owns focus.
void SDL3GameEngine::updateTextInputState(void)
{
	if (!m_SDLWindow || !TheWindowManager) {
		return;
	}

	GameWindow* focusedWindow = TheWindowManager->winGetFocus();
	const Bool wantsTextInput =
		focusedWindow != nullptr && BitIsSet(focusedWindow->winGetStyle(), GWS_ENTRY_FIELD);

	if (wantsTextInput) {
		if (!m_IsTextInputActive) {
			if (SDL_StartTextInput(m_SDLWindow)) {
				m_IsTextInputActive = true;
			}
		}
		m_TextInputFocusWindow = focusedWindow;
	} else {
		if (m_IsTextInputActive) {
			SDL_StopTextInput(m_SDLWindow);
			m_IsTextInputActive = false;
		}
		m_TextInputFocusWindow = nullptr;
	}
}

// GeneralsX @bugfix felipebraz 01/04/2026 Forward SDL UTF-8 text input through existing GWM_IME_CHAR path.
void SDL3GameEngine::forwardTextInputEvent(const char* utf8Text)
{
	if (!utf8Text || !TheWindowManager) {
		return;
	}

	// GeneralsX @bugfix felipebraz 01/04/2026 Use tracked text-input focus window to keep SDL text delivery stable.
	GameWindow* targetWindow = m_TextInputFocusWindow;
	if (!targetWindow || !BitIsSet(targetWindow->winGetStyle(), GWS_ENTRY_FIELD)) {
		return;
	}

	const size_t textLength = strlen(utf8Text);
	size_t offset = 0;
	while (offset < textLength) {
		UnsignedInt codepoint = 0;
		if (!DecodeNextUtf8Codepoint(utf8Text, textLength, offset, codepoint)) {
			continue;
		}

		// GeneralsX @bugfix felipebraz 01/04/2026 Clamp IME char forwarding to BMP and reject UTF-16 surrogate range.
		if (codepoint == 0 || codepoint > 0x10FFFFU) {
			continue;
		}

		if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
			continue;
		}

		if (codepoint > 0xFFFFU) {
			continue;
		}

		const WideChar wideCharacter = static_cast<WideChar>(codepoint);
		TheWindowManager->winSendInputMsg(targetWindow, GWM_IME_CHAR, static_cast<WindowMsgData>(wideCharacter), 0);
	}
}

/**
 * Handle keyboard event -dispatch to Keyboard manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleKeyboardEvent(const SDL_KeyboardEvent& event)
{
	// Dispatch to SDL3Keyboard if available
	if (TheKeyboard) {
		SDL3Keyboard* sdlKeyboard = dynamic_cast<SDL3Keyboard*>(TheKeyboard);
		if (sdlKeyboard) {
			sdlKeyboard->addSDL3KeyEvent(event);
		}
	}
}

/**
 * Handle mouse motion event - dispatch to Mouse manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleMouseMotionEvent(const SDL_MouseMotionEvent& event)
{
	// Dispatch to SDL3Mouse if available
	if (TheMouse) {
		SDL3Mouse* sdlMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (sdlMouse) {
			sdlMouse->addSDL3MouseMotionEvent(event);
		}
	}
}

/**
 * Handle mouse button event - dispatch to Mouse manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleMouseButtonEvent(const SDL_MouseButtonEvent& event)
{
	// Dispatch to SDL3Mouse if available
	if (TheMouse) {
		SDL3Mouse* sdlMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (sdlMouse) {
			sdlMouse->addSDL3MouseButtonEvent(event);
		}
	}
}

/**
 * Handle mouse wheel event - dispatch to Mouse manager
 * TheSuperHackers @build 10/02/2026 BenderAI - Phase 1.5 event wiring
 */
void SDL3GameEngine::handleMouseWheelEvent(const SDL_MouseWheelEvent& event)
{
	// Dispatch to SDL3Mouse if available
	if (TheMouse) {
		SDL3Mouse* sdlMouse = dynamic_cast<SDL3Mouse*>(TheMouse);
		if (sdlMouse) {
			sdlMouse->addSDL3MouseWheelEvent(event);
		}
	}
}

/**
 * Handle window event (resize, etc.)
 */
void SDL3GameEngine::handleWindowEvent(const SDL_WindowEvent& event)
{
	// TODO: Phase 2 - Handle window resize, notify graphics subsystem
	// fprintf(stderr, "DEBUG: Window event (type=%d)\n", event.type);
}

/**
 * Factory Methods for GameEngine subsystems
 * TheSuperHackers @build felipebraz 13/02/2026
 * Implementations in .cpp to provide complete type definitions and avoid circular includes
 */

LocalFileSystem *SDL3GameEngine::createLocalFileSystem(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createLocalFileSystem() -> StdLocalFileSystem\n");
	return NEW StdLocalFileSystem;
}

ArchiveFileSystem *SDL3GameEngine::createArchiveFileSystem(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createArchiveFileSystem() -> StdBIGFileSystem\n");
	return NEW StdBIGFileSystem;
}

GameLogic *SDL3GameEngine::createGameLogic(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createGameLogic() -> W3DGameLogic\n");
	return NEW W3DGameLogic;
}

GameClient *SDL3GameEngine::createGameClient(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createGameClient() -> W3DGameClient\n");
	return NEW W3DGameClient;
}

ModuleFactory *SDL3GameEngine::createModuleFactory(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createModuleFactory() -> W3DModuleFactory\n");
	return NEW W3DModuleFactory;
}

ThingFactory *SDL3GameEngine::createThingFactory(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createThingFactory() -> W3DThingFactory\n");
	return NEW W3DThingFactory;
}

FunctionLexicon *SDL3GameEngine::createFunctionLexicon(void)
{
	fprintf(stderr, "INFO: SDL3GameEngine::createFunctionLexicon() -> W3DFunctionLexicon\n");
	return NEW W3DFunctionLexicon;
}

// GeneralsX @bugfix Copilot 15/04/2026 Match upstream GameEngine pure-virtual signature after sync.
Radar *SDL3GameEngine::createRadar(Bool dummy)
{
	// GeneralsX @bugfix fbraz 04/05/2026 Respect headless mode and create dummy radar.
	// Upstream reference: Win32GameEngine headless factory behavior, TheSuperHackers/GeneralsGameCode
	// https://github.com/TheSuperHackers/GeneralsGameCode
	if (dummy) {
		fprintf(stderr, "INFO: SDL3GameEngine::createRadar() -> RadarDummy (headless)\n");
		return NEW RadarDummy;
	}
	fprintf(stderr, "INFO: SDL3GameEngine::createRadar() -> W3DRadar\n");
	return NEW W3DRadar;
}

// GeneralsX @bugfix Copilot 24/03/2026 Match upstream GameEngine pure-virtual signature after sync.
ParticleSystemManager* SDL3GameEngine::createParticleSystemManager(Bool dummy)
{
	// GeneralsX @bugfix fbraz 04/05/2026 Respect headless mode and create dummy particle manager.
	if (dummy) {
		fprintf(stderr, "INFO: SDL3GameEngine::createParticleSystemManager() -> ParticleSystemManagerDummy (headless)\n");
		return NEW ParticleSystemManagerDummy;
	}
	fprintf(stderr, "INFO: SDL3GameEngine::createParticleSystemManager() -> W3DParticleSystemManager\n");
	return NEW W3DParticleSystemManager;
}

WebBrowser *SDL3GameEngine::createWebBrowser(void)
{
	// WebBrowser uses Windows COM (CComObject<W3DWebBrowser>)
	// Not available on Linux - return nullptr
	fprintf(stderr, "WARNING: WebBrowser not available on Linux platform\n");
	return nullptr;
}

/**
 * Factory method: AudioManager
 * Select audio backend based on compile flags
 * GeneralsX @bugfix Copilot 15/04/2026 Match upstream GameEngine pure-virtual signature after sync.
 */
AudioManager *SDL3GameEngine::createAudioManager(Bool dummy)
{
	(void)dummy;
	fprintf(stderr, "INFO: SDL3GameEngine::createAudioManager()\n");

#ifdef SAGE_USE_OPENAL
	fprintf(stderr, "INFO: Creating OpenAL audio backend\n");
	return new OpenALAudioManager();
#else
	fprintf(stderr, "INFO: Audio backend not available (SAGE_USE_OPENAL not defined)\n");
	fprintf(stderr, "WARNING: Falls back to parent implementation or silent mode\n");
	return GameEngine::createAudioManager();  // Call parent (may return stub)
#endif
}

#endif // !_WIN32

