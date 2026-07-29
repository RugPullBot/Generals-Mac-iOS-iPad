#!/usr/bin/env bash
# Cross-platform determinism soak: many matches, many seeds, many maps, zero networking.
#
# WHY THIS SHAPE
#
# The claim under test is "macOS and Linux simulate the same game". The strongest way to test that
# is NOT to play networked matches - a networked run conflates simulation with transport, and when
# it diverges you cannot say which failed. Instead each iteration runs the SAME pinned seed as a
# SOLO headless match on both machines, with no packets exchanged at all, and diffs every frame.
# Nothing about the result can be explained by the relay, by NAT, or by packet loss.
#
# It varies seed AND map on purpose. A single map exercises one set of scripts, one terrain, one
# starting layout; the AI-only frame-0 desync that cost this project days was invisible on maps
# without skirmish scripts attached. Volume on one map is not the same evidence as volume across
# many.
#
# Every iteration includes an AI (-lanai). Skirmish scripts attach ONLY for AI players, and that
# path is where the historical divergence lived - a soak that quietly ran human-only would rack up
# impressive frame counts while never touching the code most likely to break.
#
# Usage:
#   ITERATIONS=20 FRAMES=1500 ./scripts/test/xplat-determinism-soak.sh
#
# Env:
#   ITERATIONS  matches to run          (default 10)
#   FRAMES      logic frames per match  (default 1500)
#   VPS         linux peer              (default root@163.5.210.131)
#   VPS_NETNS   netns for the linux peer (default gx; empty to run in the root namespace)
#   SEED_BASE   first seed              (default 700000)
#   OUT         results dir             (default a timestamped dir under /tmp)
#
# WHY THE LINUX PEER RUNS IN A NAMESPACE
#
# The VPS is also the relay host. With RelayAddress set in Options.ini the game enters relay mode,
# and relay mode forces a WILDCARD bind - a virtual address exists on no interface and can never be
# bound - so the game's socket collides with the relay's own listener and every run dies instantly
# with `UDP::Bind failed (status -6) for IP 0.0.0.0 port 8086`, transportInit=FAILED, exit 1.
# That is not hypothetical: it is what this script did on its first real run, on all 50 iterations,
# while the macOS side simulated its frames perfectly. A namespace gives the peer its own port
# table and the collision goes away. Set VPS_NETNS= (empty) only if the VPS has no relay on it.

set -uo pipefail

# Shared map capacity table. This soak cycles maps and AI counts independently, so some
# combinations ask for more players than the map holds; see the skip in the iteration loop.
. "$(dirname "$0")/lib-map-capacity.sh"

ITERATIONS="${ITERATIONS:-10}"
FRAMES="${FRAMES:-1500}"
VPS="${VPS:-root@163.5.210.131}"
VPS_NETNS="${VPS_NETNS-gx}"
SEED_BASE="${SEED_BASE:-700000}"

# Prefix for the remote game command. Deliberately NOT applied to kill_games: pkill is per-host,
# not per-namespace, so it already reaches a peer inside one.
if [ -n "$VPS_NETNS" ]; then
	NS_PREFIX="ip netns exec $VPS_NETNS"
else
	NS_PREFIX=""
fi
MAC_GAME="${MAC_GAME:-$HOME/GeneralsX/GeneralsZH}"
VPS_GAME="${VPS_GAME:-/root/gamedata}"
OUT="${OUT:-/tmp/gx-soak-$(date +%Y%m%d-%H%M%S)}"

mkdir -p "$OUT"

# Standard multiplayer maps. Deliberately excludes the unit-test and BUG_ maps in the archive,
# which are not representative and in some cases do not load.
MAPS=(
	"maps\\alpine assault\\alpine assault.map"
	"maps\\bitter winter\\bitter winter.map"
	"maps\\cairo commandos\\cairo commandos.map"
	"maps\\dark mountain\\dark mountain.map"
	"maps\\death valley\\death valley.map"
	"maps\\desert fury\\desert fury.map"
	"maps\\destruction station\\destruction station.map"
	"maps\\dust devil\\dust devil.map"
	"maps\\final crusade\\final crusade.map"
	"maps\\twilight flame\\twilight flame.map"
)
AI_SPECS=("Hx1" "Mx1" "Ex1" "Hx2")

# --------------------------------------------------------------------------------------------
# Preflight. Both peers must be the same build or the comparison means nothing - and a soak that
# silently compares two different builds is worse than no soak, because it manufactures evidence.
# --------------------------------------------------------------------------------------------
say() { printf '%s\n' "$*"; }
die() { printf 'FATAL: %s\n' "$*" >&2; exit 1; }

# pkill -x, never -f: -f matches the full command line INCLUDING our own ssh invocation, which has
# killed the shell running it more than once in this project.
kill_games() {
	pkill -x GeneralsXZH 2>/dev/null
	ssh -o ConnectTimeout=15 "$VPS" 'pkill -x GeneralsXZH 2>/dev/null; exit 0' >/dev/null 2>&1
	sleep 2
	local mac_left vps_left
	mac_left="$(pgrep -x GeneralsXZH | wc -l | tr -d ' ')"
	vps_left="$(ssh -o ConnectTimeout=15 "$VPS" 'ps -eo comm | grep -cx GeneralsXZH' 2>/dev/null | tr -d '[:space:]')"
	[ "$mac_left" = "0" ] || die "a GeneralsXZH is still running on this Mac ($mac_left)"
	[ "${vps_left:-1}" = "0" ] || die "a GeneralsXZH is still running on the VPS ($vps_left)"
}

say "=== preflight ==="
kill_games

MAC_BIN="$MAC_GAME/GeneralsXZH"
[ -x "$MAC_BIN" ] || die "no mac binary at $MAC_BIN"

# Fail here rather than 50 iterations later. A missing namespace does not degrade gracefully - the
# peer dies on a bind collision before frame 0 and every iteration reports "no frames simulated",
# which reads like a headless bug rather than a setup one.
if [ -n "$VPS_NETNS" ]; then
	if ! ssh -o ConnectTimeout=15 "$VPS" "ip netns list | grep -qw '$VPS_NETNS'" 2>/dev/null; then
		die "netns '$VPS_NETNS' does not exist on $VPS - create it (see tools/relay/README.md) or set VPS_NETNS="
	fi
	say "linux peer runs in netns $VPS_NETNS"
fi

# --------------------------------------------------------------------------------------------
TOTAL_FRAMES=0
TOTAL_DIFFERING=0
FAILED=0
RUN=0
SKIPPED=0

for ((i = 0; i < ITERATIONS; i++)); do
	SEED=$((SEED_BASE + i * 977))				# 977 is prime, so seeds do not fall into a pattern
	MAP="${MAPS[$((i % ${#MAPS[@]}))]}"
	AI="${AI_SPECS[$((i % ${#AI_SPECS[@]}))]}"
	TAG="$(printf '%03d' "$i")"

	say ""
	say "=== [$TAG] seed=$SEED ai=$AI map=${MAP##*\\} ==="

	# GeneralsX @fix Claude 29/07/2026 MAPS and AI_SPECS cycle independently, so this loop produces
	# combinations that do not fit - a solo lobby is the host plus its AI, and Hx2 on a 2-start map
	# needs three positions. Those iterations used to run anyway with the surplus AI silently inert;
	# 7 of the 50 in the committed xplat-soak-50x1500 corpus were exactly that (dust devil x3,
	# bitter winter x2, desert fury x2). The engine refuses them now, so skip them here and COUNT
	# the skips - a soak that quietly shrinks its own denominator is the failure mode this suite
	# exists to catch.
	NEED=$(( 1 + $(ai_spec_count "$AI") ))
	if ! lobby_fits "$MAP" "$NEED" >/dev/null 2>&1; then
		say "    SKIPPED: holds $(map_capacity "$MAP"), this lobby needs $NEED (host + $AI)"
		SKIPPED=$((SKIPPED + 1))
		continue
	fi

	kill_games

	# Both peers run the same match with no connection between them. Run them CONCURRENTLY: they
	# are different machines and nothing is shared, so serialising would only double wall clock.
	( cd "$MAC_GAME" && ./run.sh -headless -lanhost soak \
		-lanmap "$MAP" -lanai "$AI" -lanseed "$SEED" -lanframes "$FRAMES" \
		> "$OUT/$TAG.mac.out" 2> "$OUT/$TAG.mac.err" ) &
	MAC_PID=$!

	ssh -o ConnectTimeout=20 "$VPS" \
		"cd $VPS_GAME && $NS_PREFIX env CNC_GENERALS_ZH_PATH=$VPS_GAME ./GeneralsXZH -headless -lanhost soak \
		 -lanmap '$MAP' -lanai '$AI' -lanseed $SEED -lanframes $FRAMES 2> /tmp/soak.err; \
		 echo EXIT=\$?; cat /tmp/soak.err" > "$OUT/$TAG.linux.err" 2>&1 &
	VPS_PID=$!

	wait $MAC_PID; MAC_RC=$?
	wait $VPS_PID

	grep '^\[GXCRC\]' "$OUT/$TAG.mac.err"   | tr -d '\r' > "$OUT/$TAG.mac.crc"
	grep '^\[GXCRC\]' "$OUT/$TAG.linux.err" | tr -d '\r' > "$OUT/$TAG.linux.crc"

	MAC_N=$(wc -l < "$OUT/$TAG.mac.crc" | tr -d ' ')
	LIN_N=$(wc -l < "$OUT/$TAG.linux.crc" | tr -d ' ')

	# A run that simulated nothing is a FAILURE, not a pass with no work. Reporting exit 0 on an
	# empty run is how a headless bug stayed invisible in this project once already.
	if [ "$MAC_N" -eq 0 ] || [ "$LIN_N" -eq 0 ]; then
		say "  FAIL: no frames simulated (mac=$MAC_N linux=$LIN_N, mac exit=$MAC_RC)"
		# Surface WHY rather than leaving a 300 KB log to be read by hand. A bind collision is by
		# far the most common cause and says nothing about determinism, so name it here.
		for who in mac linux; do
			BIND_ERR="$(grep -a -m1 'UDP::Bind failed' "$OUT/$TAG.$who.err" 2>/dev/null)"
			[ -n "$BIND_ERR" ] && say "    $who: $BIND_ERR"
		done
		grep -aq 'transportInit=FAILED' "$OUT/$TAG.linux.err" 2>/dev/null &&
			say "    linux transport never came up - is the relay holding 8086 in this namespace?"
		FAILED=$((FAILED + 1))
		continue
	fi

	N=$(( MAC_N < LIN_N ? MAC_N : LIN_N ))

	# GeneralsX @fix Claude 29/07/2026 FRAME FLOOR. Truncating to the shorter stream with no floor
	# silently redefines the experiment: a peer that stops at frame 3 turns a 1500-frame iteration
	# into a 3-frame one that then "agrees" and is counted as evidence. Fail it instead, before
	# anything is added to TOTAL_FRAMES.
	FLOOR=$(( FRAMES * 9 / 10 ))
	if [ "$N" -lt "$FLOOR" ]; then
		say "  FAIL  only $N frames comparable of $FRAMES requested (floor $FLOOR) - a peer stopped early"
		FAILED=$((FAILED + 1))
		continue
	fi

	head -n "$N" "$OUT/$TAG.mac.crc"   > "$OUT/$TAG.mac.cmp"
	head -n "$N" "$OUT/$TAG.linux.crc" > "$OUT/$TAG.linux.cmp"

	# Compare the CRC VALUE column. `paste a b | awk '$2!=$4'` looks right and is not: paste shifts
	# the fields, so $2 is compared against the other file's tag and every row "differs".
	DIFFERING=$(paste "$OUT/$TAG.mac.cmp" "$OUT/$TAG.linux.cmp" | awk '$3!=$6' | wc -l | tr -d ' ')
	DISTINCT=$(awk '{print $3}' "$OUT/$TAG.mac.cmp" | sort -u | wc -l | tr -d ' ')

	TOTAL_FRAMES=$((TOTAL_FRAMES + N))
	TOTAL_DIFFERING=$((TOTAL_DIFFERING + DIFFERING))
	RUN=$((RUN + 1))

	# The control. A frozen or idling simulation matches trivially; distinct == frames means every
	# frame did different work. An earlier "identical" run in this project had 3 distinct values
	# across 600 frames and proved exactly nothing.
	if [ "$DIFFERING" -eq 0 ]; then
		# GeneralsX @fix Claude 29/07/2026 This used to WARN and pass, while all three sibling
		# harnesses treated the same condition as disqualifying. An idle sim matches trivially, so
		# counting it as evidence is how a soak manufactures a frame total it did not earn.
		if [ "$DISTINCT" -lt $((N / 2)) ]; then
			say "  IDLE  $N frames, 0 differing, but only $DISTINCT distinct - not determinism evidence"
			FAILED=$((FAILED + 1))
		else
			say "  ok   $N frames, 0 differing, $DISTINCT distinct"
		fi
	else
		say "  DIVERGED  $DIFFERING of $N frames"
		paste "$OUT/$TAG.mac.cmp" "$OUT/$TAG.linux.cmp" | awk '$3!=$6' | head -3 | sed 's/^/    /'
		FAILED=$((FAILED + 1))
	fi
done

kill_games

say ""
say "======================================================================"
ATTEMPTED=$((ITERATIONS - SKIPPED))
say " matches run     : $RUN of $ATTEMPTED attempted ($ITERATIONS requested)"
say " skipped         : $SKIPPED (over-capacity map/AI combination)"
say " total frames    : $TOTAL_FRAMES"
say " differing frames: $TOTAL_DIFFERING"
say " failures        : $FAILED"
say " results         : $OUT"
say "======================================================================"

# A skip is not a failure, but it MUST show up in the claim rather than silently shrinking the
# denominator - so PASS is measured against what was attempted and always names the skip count.
if [ "$FAILED" -eq 0 ] && [ "$TOTAL_DIFFERING" -eq 0 ] && [ "$RUN" -eq "$ATTEMPTED" ] && [ "$RUN" -gt 0 ]; then
	say "PASS: $TOTAL_FRAMES frames across $RUN matches, macOS vs Linux, zero differing ($SKIPPED skipped as over-capacity)"
	exit 0
fi

say "FAIL"
exit 1
