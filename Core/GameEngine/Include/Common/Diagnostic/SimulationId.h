/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

#pragma once

// GeneralsX @feature Claude 27/07/2026
// Join-time build/data compatibility identity ("SimID").
//
// SCOPE: validated for Apple targets only in this pass - macOS arm64 and iOS arm64.
// Both are the same path separators, line endings, wchar_t width and pointer size.
// Cross-platform correctness against Windows is explicitly out of scope; the places
// that will need Windows work are marked "WINDOWS HOOK" in SimulationId.cpp.
//
// This is a Core-level immutable snapshot rather than new GlobalData members: it
// avoids duplicating fields in both GlobalData copies, avoids reset()/newOverride()
// semantics, and cannot be clobbered by the RTS_DEBUG backdoor in WOLLobbyMenu that
// overwrites m_exeCRC and m_iniCRC with a remote peer's advertised values.

struct SimIdWire;   // defined in GameNetwork/LANAPI.h inside its pack(1) region

struct SimulationId
{
	Bool        valid;       // false until initialize() has run to completion
	UnsignedInt revision;    // GitRevision, informational only, never compared
	UnsignedInt engineID;    // tier1 deny
	UnsignedInt sourceID;    // tier1 deny; 0 == unknown provenance, never compared
	// tier1 deny. m_iniCRC mixed with a digest of the loose simulation-relevant files
	// that never pass through the INI parser and so were outside m_iniCRC entirely - see
	// the allow-list and, more importantly, the exclusions in SimulationId.cpp.
	UnsignedInt dataID;
	UnsignedInt ordinalID;   // tier1 deny
	UnsignedInt parseID;     // tier2 warn
	UnsignedInt assetID;     // tier2 warn
	UnsignedInt platformID;  // tier2 warn - coarse ABI/libm class

	// Supplied by the per-game GameEngine::init, because NameKeyGenerator and
	// ArchiveFileSystem values are captured at points only that file knows about.
	struct Inputs
	{
		UnsignedInt iniCRC;
		UnsignedInt nameKeyBeforeScience;  // TheNameKeyGenerator->getNextID() immediately BEFORE initSubsystem(TheScienceStore, ...)
		UnsignedInt nameKeyAfterScience;   // ... and immediately AFTER it
		UnsignedInt archiveFingerprint;    // TheArchiveFileSystem->computeMountedArchiveFingerprint()
	};

	// Call once, where m_iniCRC is assigned. Never throws: on any internal failure
	// it leaves valid == false, which fails safe to "this build cannot join".
	static void initialize( const Inputs &in );
	static const SimulationId &get();          // immutable after initialize()

	void toWire( SimIdWire &out ) const;       // writes the tag only when valid
};

enum SimIdVerdict
{
	SIMID_OK = 0,
	SIMID_LOCAL_INVALID,     // our own identity never completed - refuse before sending
	SIMID_PEER_SILENT,       // peer predates SimID, or is on a different SimID generation
	SIMID_ENGINE_DIFFERS,
	SIMID_SOURCE_DIFFERS,
	SIMID_DATA_DIFFERS,
	SIMID_ORDINAL_DIFFERS
};

enum
{
	SIMID_WARN_PARSE          = 1,
	SIMID_WARN_ASSET          = 2,
	SIMID_WARN_SOURCE_UNKNOWN = 4,   // one or both sides built without git
	SIMID_WARN_PLATFORM       = 8    // different ABI / system math library class
};

// tier1 only, evaluated in exactly this precedence so the message is always specific.
SimIdVerdict SimIdCompare( const SimIdWire &peer );

// tier2 bitmask. Returns 0 when the peer is not a valid SimID peer.
UnsignedInt  SimIdWarnFlags( const SimIdWire &peer );

// GeneralsX @feature Claude 27/07/2026 Diagnostics for refused joins.
//
// Both write to stderr, NOT through DEBUG_LOG - DEBUG_LOG is ((void)0) in every shipping
// preset (Debug.h:165-173), which is why a refused join previously produced no evidence
// on either side. `side` is a short caller tag such as "HOST" or "JOINER".
const char *SimIdVerdictName( SimIdVerdict v );
void        SimIdLogMismatch( const char *side, const char *peerAddr, SimIdVerdict verdict, const SimIdWire &peer );
