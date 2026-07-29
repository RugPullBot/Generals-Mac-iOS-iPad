# lib-activity.sh - read the engine's [GXACT] per-player activity trace.
#
# GeneralsX @feature Claude 29/07/2026
#
# Source this, do not execute it:
#     . "$(dirname "$0")/lib-activity.sh"
#
# WHY THIS EXISTS. Every harness in this project judged "the simulation was active" by counting
# DISTINCT [GXCRC] values. That number is satisfied by ANY moving object. alpine assault carries a
# scripted train that drives itself with no player and no AI attached, and Object::crc hashes the
# transform, so alpine read 1499 of 1500 distinct while every AI in the match owned nothing at all -
# and alpine was the control every other map was judged against.
#
# Re-measured 29/07/2026, after the capacity guard landed: alpine + Hx2 with one AI provably inert
# scored 60 distinct of 60, identical to alpine + Hx1. The distinct count cannot separate them.
#
# The engine emits [GXACT] when GX_ACTIVITY=<frame period> is set (GameLogic.cpp, beside [GXCRC]):
#
#   [GXACT] f=150 total=229 unowned=0 p0=-:11/11 p1=-:199/16 p2=A:3/2 ... p8=-:0/0
#            ^frame  ^objects  ^no owner        ^kind:objects/structures
#
# kind is A (skirmish AI), H (human, playable) or - (neutral, observer, dead, or an unoccupied
# slot). ONLY A and H are judged. ThePlayerList is not the lobby: it always carries a neutral entry
# owning the map scenery plus one entry per empty slot, and those legitimately own nothing forever.
# A harness that flagged every 0/0 entry would call a healthy match INCONCLUSIVE - measured on a
# legal twilight flame + Hx5 run, where p8 is an empty slot at 0/0 for all 300 frames.

# WHICH PEER'S LOG TO READ. Any one of them. Peers in lockstep compute identical simulation state
# by definition, and the harnesses that call this have already compared the CRC streams, so the
# per-player counts cannot differ between peers without that comparison having failed first. The
# multi-peer harnesses read the peer whose log they hold locally, which avoids fetching a second
# one over ssh purely to re-read a number that must match.

# starved_players <logfile>
#   Prints a comma-separated list of A/H players that owned NOTHING at every single sample, or
#   nothing at all if every participant owned something at some point. Empty output when the log
#   has no [GXACT] lines - use has_activity_trace to tell "measured and clean" from "not measured".
starved_players() {
	local log="$1"
	[ -f "$log" ] || return 0
	awk '/^\[GXACT\] f=/ {
			for (i = 4; i <= NF; i++)
				if ($i ~ /^p[0-9]+=[AH]:/) {
					split($i, kv, "=")
					seen[kv[1]] = 1
					split(kv[2], t, ":")
					if (t[2] != "0/0") live[kv[1]] = 1
				}
		}
		END { n = 0; for (p in seen) if (!(p in live)) { printf "%s%s", (n++ ? "," : ""), p } }' \
		"$log"
}

# has_activity_trace <logfile>  -> 0 if the run was launched with GX_ACTIVITY, 1 otherwise.
has_activity_trace() {
	[ -f "$1" ] && grep -aq '^\[GXACT\] f=' "$1"
}

# activity_summary <logfile>
#   One line for a harness summary, distinguishing all three states: not measured, measured and
#   every participant active, measured and someone starved.
activity_summary() {
	local log="$1" starved
	if ! has_activity_trace "$log"; then
		printf 'per-player activity: NOT MEASURED - re-run with GX_ACTIVITY=100'
		return 0
	fi
	starved="$(starved_players "$log")"
	if [ -n "$starved" ]; then
		printf 'players owning NOTHING all match: %s   <- [GXACT], not the distinct count' "$starved"
	else
		printf 'per-player activity: every participant owned something at every sample ([GXACT])'
	fi
}
