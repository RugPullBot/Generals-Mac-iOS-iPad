# lib-map-capacity.sh - shared map capacity table for the headless test harnesses.
#
# GeneralsX @feature Claude 29/07/2026
#
# Source this, do not execute it:
#     . "$(dirname "$0")/lib-map-capacity.sh"
#
# WHY THIS EXISTS. A lobby holding more players than the map has start positions does not fail -
# it silently starves the surplus. populateRandomStartPosition caps positions at md->m_numPlayers
# (GameLogic.cpp:882-885) and pre-marks every index past it as taken (:928), so a surplus player
# keeps startPos=-1, gets no Command Center, and its skirmish AI returns early from adjustBuildList
# and owns nothing for the entire match - while the run still reports the full AI count and passes.
#
# Every "8-player" result in this project before 1566959a9 was an overfilled 2-player map. The one
# that looked healthy, alpine assault, scored 1499/1500 distinct with its AI completely inert,
# because it carries a scripted train that self-moves every frame and whose transform is hashed
# into the object CRC. It was the control every other map was judged against.
#
# The engine refuses an over-capacity lobby itself as of task 59 (HeadlessMatch::lobbyFitsTheMap),
# so a harness that gets this wrong now fails loudly instead of lying. This table is here so a
# harness can fail in preflight, in about a second, instead of after provisioning three machines.

# Capacity of a standard multiplayer map, by md->m_numPlayers. MEASURED by loading each map and
# reading the value, cross-checked against Maps\MapCache.ini extracted from MapsZH.big; see
# docs/WORKDIR/evidence/networked-sim-freeze-diagnosis.md section 1.2.
#
# Only THREE standard maps hold 8 players. Returns 0 for anything not listed - callers must treat
# 0 as "unknown", not as "zero capacity".
map_capacity() {
	case "$1" in
		*[Dd]"eath "[Vv]"alley"*|*[Dd]"estruction "[Ss]"tation"*|*[Tt]"wilight "[Ff]"lame"*) echo 8 ;;
		*[Dd]"ark "[Mm]"ountain"*)                                                           echo 4 ;;
		*[Cc]"airo "[Cc]"ommandos"*)                                                         echo 3 ;;
		*[Aa]"lpine "[Aa]"ssault"*|*[Bb]"itter "[Ww]"inter"*|*[Dd]"esert "[Ff]"ury"*)         echo 2 ;;
		*[Dd]"ust "[Dd]"evil"*|*[Ff]"inal "[Cc]"rusade"*|*[Kk]"illing "[Ff]"ields"*)          echo 2 ;;
		*) echo 0 ;;
	esac
}

# Parse an -lanai spec (E|M|H x N, e.g. "Hx5") into a count. Empty or malformed yields 0.
ai_spec_count() {
	local n
	n="$(printf '%s' "${1:-}" | sed -E 's/^[EMHemh][xX]([0-9]+).*/\1/')"
	case "$n" in ''|*[!0-9]*) echo 0 ;; *) echo "$n" ;; esac
}

# lobby_fits <map> <occupied>
#   0  the lobby fits, or the capacity is unknown (warns on stderr)
#   1  over capacity
# Prints a one-line explanation either way. Does not exit - the caller decides whether an
# over-capacity lobby is fatal or merely skipped, because a soak wants to skip and keep going
# while a single-shot run wants to die.
lobby_fits() {
	local map="$1" need="$2" cap
	cap="$(map_capacity "$map")"
	if [ "$cap" = "0" ]; then
		printf "  WARNING: unknown capacity for '%s' - cannot check it holds %s player(s)\n" \
			"${map##*\\}" "$need" >&2
		return 0
	fi
	if [ "$need" -gt "$cap" ]; then
		printf "  map '%s' holds %s player(s) but this lobby needs %s\n" "${map##*\\}" "$cap" "$need" >&2
		return 1
	fi
	printf "  map '%s' holds %s, lobby needs %s - ok\n" "${map##*\\}" "$cap" "$need"
	return 0
}
