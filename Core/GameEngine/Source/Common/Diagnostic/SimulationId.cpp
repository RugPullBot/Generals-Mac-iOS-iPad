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

// GeneralsX @feature Claude 27/07/2026 Join-time SimID compatibility check.
// Apple targets (macOS arm64, iOS arm64) only in this pass. Places that will need
// work before a Windows peer can be trusted are marked "WINDOWS HOOK".

#include "PreRTS.h"

#include "Common/Diagnostic/SimulationId.h"
#include "Common/GameDefines.h"
#include "Common/INI.h"
#include "GameNetwork/LANAPI.h"        // SimIdWire, SIMID_WIRE_TAG, sizeof(LANMessage)
#include "GameNetwork/NetworkDefs.h"   // MAX_PACKET_SIZE, MAX_NETWORK_MESSAGE_LEN
#include "gitinfo.h"                   // GitRevision
#include "simsourcedigest.h"           // SimSourceDigestZeroHour / Generals

#include <string.h>

// RTS_ZEROHOUR has no 0-fallback and is defined only for the Zero Hour target, so it
// may never be used as a C++ expression - only under #if.
#if RTS_ZEROHOUR
static const UnsignedInt SIMID_GAME_VARIANT = 2u;
#define SIMID_SOURCE_DIGEST SimSourceDigestZeroHour
#elif RTS_GENERALS
static const UnsignedInt SIMID_GAME_VARIANT = 1u;
#define SIMID_SOURCE_DIGEST SimSourceDigestGenerals
#else
#error "SimulationId: neither RTS_ZEROHOUR nor RTS_GENERALS is defined"
#endif

// ---- mixer: explicit little-endian byte stream, never memcpy of a word ----
static UnsignedInt fnv1a( UnsignedInt h, const void *d, size_t n )
{
	const unsigned char *p = (const unsigned char *)d;
	for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
	return h;
}

static UnsignedInt fnv1aU32( UnsignedInt h, UnsignedInt v )
{
	const unsigned char b[4] = {
		(unsigned char)(v & 0xFFu), (unsigned char)((v >> 8) & 0xFFu),
		(unsigned char)((v >> 16) & 0xFFu), (unsigned char)((v >> 24) & 0xFFu) };
	return fnv1a(h, b, 4);
}

#define SIMBIT(n, x) (((x) ? 1u : 0u) << (n))

static const UnsignedInt FNV_SEED = 2166136261u;

// ---------------------------------------------------------------------------
// engineID - build configuration and wire geometry.
//
// WINDOWS HOOKS / deliberate omissions:
//  * sizeof(WideChar) is NOT hashed. WideChar is wchar_t (Core/Libraries/Include/
//    Lib/BaseType.h) - 4 bytes under Apple clang, 2 under MSVC - and no build flag
//    can change that, so hashing it would make every Apple<->Windows pairing
//    mathematically impossible with no remedy. Nothing on the lockstep or LAN wire
//    path depends on it: LANMessage uses WideCharWindows = uint16_t.
//  * SimulationMathCrc::calculate() is NOT hashed. It fingerprints the device's
//    libsystem_m rather than the build (an iPadOS point update alone could break a
//    working pairing), and it calls setFPMode() then restores with
//    fesetenv(FE_DFL_ENV) rather than the prior environment, which would leave the
//    FPU in a different state for postProcessLoadAll. See platformID instead.
//  * sizeof(void*) IS hashed and will refuse a 32-bit peer. That is correct on the
//    merits - BitFlagsIO.h does xfer->xferUser(this, sizeof(this)), the pointer not
//    the object, under RETAIL_COMPATIBLE_CRC - but note every Windows preset in
//    CMakePresets.json is currently 32-bit Win32, so a Windows port must be x64 to
//    ever pair with Apple.
// ---------------------------------------------------------------------------
static UnsignedInt computeEngineID( void )
{
	UnsignedInt h = FNV_SEED;
	h = fnv1a(h, "SIMID/2", 7);              // recipe version
	h = fnv1aU32(h, (UnsignedInt)SIMID_EPOCH);
	h = fnv1aU32(h, SIMID_GAME_VARIANT);

	// The 15 PRESERVE_* switches from GameDefines.h, in declaration order.
	const UnsignedInt rulesWord =
		  SIMBIT( 0, PRESERVE_BUILDING_RESUMPTION_DELAY)
		| SIMBIT( 1, PRESERVE_CHINOOK_PASSENGER_DUMPING)
		| SIMBIT( 2, PRESERVE_HARDCODED_BLACK_LOTUS_CASH_HACK)
		| SIMBIT( 3, PRESERVE_MULTI_CRATE_PICKUP)
		| SIMBIT( 4, PRESERVE_NO_XP_FROM_FLAME_KILLS)
		| SIMBIT( 5, PRESERVE_NO_XP_FROM_OCL_KILLS)
		| SIMBIT( 6, PRESERVE_NO_XP_FROM_POISON_KILLS)
		| SIMBIT( 7, PRESERVE_OCCUPANT_DETECTION_VIA_DRAG_SELECTION)
		| SIMBIT( 8, PRESERVE_PERPETUAL_HORDE_BONUS)
		| SIMBIT( 9, PRESERVE_PREMATURE_BATTLE_BUS_DEATH)
		| SIMBIT(10, PRESERVE_RADAR_WARNING_SUPPRESSION)
		| SIMBIT(11, PRESERVE_STRUCTURE_STEALTH_DURING_REPAIR)
		| SIMBIT(12, PRESERVE_TUNNEL_HEAL_STACKING)
		| SIMBIT(13, PRESERVE_UNRELIABLE_FIRESTORMS)
		| SIMBIT(14, PRESERVE_RETAIL_SCRIPTED_CAMERA);
	h = fnv1aU32(h, rulesWord);

	UnsignedInt engineWord =
		  SIMBIT(0, RETAIL_COMPATIBLE_CRC)
		| SIMBIT(1, RETAIL_COMPATIBLE_XFER_SAVE)
		| SIMBIT(2, RETAIL_COMPATIBLE_PATHFINDING)
		| SIMBIT(3, RETAIL_COMPATIBLE_PATHFINDING_ALLOCATION)
		| SIMBIT(4, RETAIL_COMPATIBLE_CIRCLE_FILL_ALGORITHM)
		| SIMBIT(5, RETAIL_COMPATIBLE_NETWORKING)
		| SIMBIT(6, RETAIL_COMPATIBLE_AIGROUP);
#if defined(RTS_DEBUG)
	engineWord |= (1u << 7);
#endif
	// Mirrors the DEBUG_CRC rule in CRCDebug.h without including that header, which
	// drags in GameLogic/GameLogic.h under DEBUG_CRC.
#if defined(DEBUG_LOGGING) && !defined(NO_DEBUG_CRC)
	engineWord |= (1u << 8);
#endif
#if defined(SAGE_USE_GLM)
	engineWord |= (1u << 9);
#endif
	// Currently a no-op in WWMath/wwmath.h (every branch calls the same function as
	// the #else) and OFF in both Apple presets, so it cannot split macOS from iOS
	// today. Kept so the identity is already correct the day the deterministic-math
	// branches actually diverge.
#if defined(USE_DETERMINISTIC_MATH)
	engineWord |= (1u << 10);
#endif
	h = fnv1aU32(h, engineWord);

	h = fnv1aU32(h, (UnsignedInt)sizeof(void *));
	h = fnv1aU32(h, (UnsignedInt)sizeof(LANMessage));
	h = fnv1aU32(h, (UnsignedInt)MAX_PACKET_SIZE);
	h = fnv1aU32(h, (UnsignedInt)MAX_NETWORK_MESSAGE_LEN);
	return h;
}

// ---------------------------------------------------------------------------
// parseID - tier2 WARN only. WINDOWS HOOK.
// INI.cpp forks the number parser on __APPLE__: macOS and iOS both go through
// sscanf("%f"), Windows through std::from_chars. On Apple-only builds this value is
// therefore always identical and this probe is inert; it exists so that the day a
// Windows peer appears, a from_chars-vs-sscanf divergence shows up as a named
// warning rather than as an unexplained desync. It is a WARNING and must never
// become a deny - gating on a probe whose whole purpose is to compare two
// deliberately different code paths is how you build a permanent wall.
//
// The per-probe catch is mandatory, not defensive style: scanReal throws
// INI_INVALID_DATA, which INI.h defines as an enumerator of an anonymous enum, so it
// matches neither `catch (ErrorCode ec)` nor `catch (INIException e)` in
// GameEngine::init and would reach `catch (...)` and kill startup with "Uncaught
// Exception during initialization." "1e-45" is deliberately absent: it is below the
// smallest float subnormal, exactly where from_chars implementations disagree about
// result_out_of_range.
// ---------------------------------------------------------------------------
static UnsignedInt computeParseID( void )
{
	static const char *const probes[] = {
		"1.0", "0.5", "-0.0", "100", "0.30000001192092896",
		"123456.789", "3.4028235e38", "0.0001", "-2.5" };
	UnsignedInt h = FNV_SEED;
	for (UnsignedInt i = 0; i < (UnsignedInt)(sizeof(probes)/sizeof(probes[0])); ++i)
	{
		UnsignedInt bits;
		try
		{
			const Real r = INI::scanReal(probes[i]);
			memcpy(&bits, &r, 4);
		}
		catch (...)
		{
			bits = 0xFFFFFFFFu ^ i;
		}
		h = fnv1aU32(h, bits);
	}
	return h;
}

// ---------------------------------------------------------------------------
// platformID - tier2 WARN only. WINDOWS HOOK, and the honest replacement for the
// SimulationMathCrc that was removed from the identity.
//
// macOS arm64 and iOS arm64 hash to the SAME token on purpose: same Apple
// libsystem_m, same arm64 ABI, same compiler family, so a Mac<->iPad pairing
// produces no warning at all. A future Windows x64 build hashes differently and
// produces "the other machine uses a different system math library; cross-platform
// play has not been validated" - informative, never a deny, because the philosophy
// forbids building a wall nobody can climb. Promote it to tier1 only if
// deterministic math is ever actually implemented and made mandatory.
// Deliberately coarse: it must NOT include __VERSION__ or a compiler patch level,
// or every Xcode update would produce noise.
// ---------------------------------------------------------------------------
static UnsignedInt computePlatformID( void )
{
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
	static const char token[] = "APPLE-ARM64-CLANG";
#elif defined(_WIN32) && defined(_M_X64)
	static const char token[] = "MSVC-X64";
#elif defined(_WIN32)
	static const char token[] = "MSVC-X86";
#else
	static const char token[] = "OTHER";
#endif
	UnsignedInt h = fnv1a(FNV_SEED, token, sizeof(token) - 1);
	return fnv1aU32(h, (UnsignedInt)sizeof(void *));
}

// ---------------------------------------------------------------------------
static SimulationId &mutableInstance( void )
{
	static SimulationId s_id = SimulationId();   // zero-initialised, valid == false
	return s_id;
}

/*static*/ const SimulationId &SimulationId::get( void )
{
	return mutableInstance();
}

/*static*/ void SimulationId::initialize( const SimulationId::Inputs &in )
{
	SimulationId &id = mutableInstance();
	if (id.valid)
		return;                                   // idempotent

	// Nothing here may escape: this runs inside GameEngine::init's try block and a
	// throw would reach catch (...) and abort startup. Leaving valid == false fails
	// safe to "cannot join" rather than to "compatible".
	try
	{
		id.revision   = (UnsignedInt)GitRevision;
		id.engineID   = computeEngineID();
		id.sourceID   = SIMID_SOURCE_DIGEST;
		id.dataID     = in.iniCRC;
		id.ordinalID  = fnv1aU32(fnv1aU32(FNV_SEED, in.nameKeyBeforeScience), in.nameKeyAfterScience);
		id.parseID    = computeParseID();
		id.assetID    = in.archiveFingerprint;
		id.platformID = computePlatformID();
		id.valid      = TRUE;                     // set last
	}
	catch (...)
	{
		id = SimulationId();
	}

	DEBUG_LOG(("SimID: valid=%d rev=%u engine=%08X source=%08X data=%08X ordinal=%08X parse=%08X asset=%08X platform=%08X",
		(int)id.valid, id.revision, id.engineID, id.sourceID, id.dataID, id.ordinalID, id.parseID, id.assetID, id.platformID));

	// GeneralsX @feature Claude 27/07/2026 Release-surviving SimID trace.
	//
	// The DEBUG_LOG above is ((void)0) in every shipping preset: DEBUG_LOGGING is only
	// defined via ALLOW_DEBUG_UTILS (Debug.h:66-77), which needs RTS_DEBUG, and the Apple
	// and Windows presets both compile this TU with -DNDEBUG -DRTS_RELEASE. So until now
	// the SimID was computed and never surfaced anywhere, and a refused join could not be
	// diagnosed at all. fprintf(stderr) is the house pattern for exactly this problem
	// elsewhere in the engine (INI.cpp:196-197, SubsystemInterface.cpp:165-166).
	//
	// Print the wire/ABI shape alongside the fields: against a Windows peer a packing or
	// epoch mismatch and a genuine content mismatch look identical from the verdict alone.
	fprintf(stderr,
		"[SIMID] local valid=%d rev=%u engine=%08X source=%08X data=%08X ordinal=%08X "
		"parse=%08X asset=%08X platform=%08X | epoch=%u tag=%08X sizeof(SimIdWire)=%u sizeof(LANMessage)=%u\n",
		(int)id.valid, id.revision, id.engineID, id.sourceID, id.dataID, id.ordinalID,
		id.parseID, id.assetID, id.platformID,
		(UnsignedInt)SIMID_EPOCH, (UnsignedInt)SIMID_WIRE_TAG,
		(UnsignedInt)sizeof(SimIdWire), (UnsignedInt)sizeof(LANMessage));
	fflush(stderr);
}

// GeneralsX @feature Claude 27/07/2026 Verdict names, so a refusal reads as a cause
// rather than an integer. Kept beside SimIdCompare, which owns the precedence order.
const char *SimIdVerdictName( SimIdVerdict v )
{
	switch (v)
	{
		case SIMID_OK:              return "OK";
		case SIMID_LOCAL_INVALID:   return "LOCAL_INVALID";
		case SIMID_PEER_SILENT:     return "PEER_SILENT";
		case SIMID_ENGINE_DIFFERS:  return "ENGINE_DIFFERS";
		case SIMID_SOURCE_DIFFERS:  return "SOURCE_DIFFERS";
		case SIMID_DATA_DIFFERS:    return "DATA_DIFFERS";
		case SIMID_ORDINAL_DIFFERS: return "ORDINAL_DIFFERS";
	}
	return "UNKNOWN";
}

// GeneralsX @feature Claude 27/07/2026 One place that prints both identities field by
// field and marks every one that differs, so "which field" never has to be eyeballed
// out of two hex rows. Callers supply which side they are and who the peer is.
void SimIdLogMismatch( const char *side, const char *peerAddr, SimIdVerdict verdict, const SimIdWire &peer )
{
	const SimulationId &me = SimulationId::get();
	SimIdWire mine;
	me.toWire(mine);

	fprintf(stderr, "[SIMID] %s REFUSED peer=%s verdict=%s\n",
		side, (peerAddr != NULL) ? peerAddr : "?", SimIdVerdictName(verdict));
	fprintf(stderr, "[SIMID]   %-10s %-10s %-10s %s\n", "field", "local", "peer", "");

	// tag first: a zero or foreign tag means the peer never spoke this protocol at all,
	// which is a different problem from any field below disagreeing.
	#define SIMID_ROW(name, a, b) \
		fprintf(stderr, "[SIMID]   %-10s %08X   %08X   %s\n", name, (UnsignedInt)(a), (UnsignedInt)(b), \
			((a) == (b)) ? "" : "<== DIFFERS")
	SIMID_ROW("tag",        mine.tag,        peer.tag);
	SIMID_ROW("engineID",   mine.engineID,   peer.engineID);
	SIMID_ROW("sourceID",   mine.sourceID,   peer.sourceID);
	SIMID_ROW("dataID",     mine.dataID,     peer.dataID);
	SIMID_ROW("ordinalID",  mine.ordinalID,  peer.ordinalID);
	SIMID_ROW("parseID",    mine.parseID,    peer.parseID);
	SIMID_ROW("assetID",    mine.assetID,    peer.assetID);
	SIMID_ROW("platformID", mine.platformID, peer.platformID);
	SIMID_ROW("revision",   mine.revision,   peer.revision);
	#undef SIMID_ROW

	// dataID and assetID have specific, actionable causes - say them rather than making
	// the reader rediscover them.
	if (mine.dataID != peer.dataID)
		fprintf(stderr, "[SIMID]   dataID is m_iniCRC: the INI file SET or its ORDER differs.\n");
	if (mine.assetID != peer.assetID)
		fprintf(stderr, "[SIMID]   assetID is the mounted-archive fingerprint: the .big set differs.\n");
	fflush(stderr);
}

void SimulationId::toWire( SimIdWire &out ) const
{
	out.tag        = valid ? SIMID_WIRE_TAG : 0u;
	out.revision   = revision;
	out.engineID   = engineID;
	out.sourceID   = sourceID;
	out.dataID     = dataID;
	out.ordinalID  = ordinalID;
	out.parseID    = parseID;
	out.assetID    = assetID;
	out.platformID = platformID;
}

SimIdVerdict SimIdCompare( const SimIdWire &peer )
{
	const SimulationId &me = SimulationId::get();
	if (!me.valid)                       return SIMID_LOCAL_INVALID;
	if (peer.tag != SIMID_WIRE_TAG)      return SIMID_PEER_SILENT;
	if (peer.engineID  != me.engineID)   return SIMID_ENGINE_DIFFERS;
	// 0 is the reserved "no provenance" sentinel; it is never comparable. The pair is
	// still allowed to play, but SimIdWarnFlags raises SIMID_WARN_SOURCE_UNKNOWN so a
	// degraded detector can never be mistaken for a passing check.
	if (peer.sourceID != 0u && me.sourceID != 0u && peer.sourceID != me.sourceID)
		                                 return SIMID_SOURCE_DIFFERS;
	if (peer.dataID    != me.dataID)     return SIMID_DATA_DIFFERS;
	if (peer.ordinalID != me.ordinalID)  return SIMID_ORDINAL_DIFFERS;
	return SIMID_OK;
}

UnsignedInt SimIdWarnFlags( const SimIdWire &peer )
{
	const SimulationId &me = SimulationId::get();
	if (!me.valid || peer.tag != SIMID_WIRE_TAG)
		return 0u;

	UnsignedInt flags = 0u;
	if (peer.parseID    != me.parseID)    flags |= SIMID_WARN_PARSE;
	if (peer.assetID    != me.assetID)    flags |= SIMID_WARN_ASSET;
	if (peer.platformID != me.platformID) flags |= SIMID_WARN_PLATFORM;
	if (peer.sourceID == 0u || me.sourceID == 0u) flags |= SIMID_WARN_SOURCE_UNKNOWN;
	return flags;
}
