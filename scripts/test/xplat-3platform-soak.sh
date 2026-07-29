#!/usr/bin/env bash
# Volume for the CROSS-PLATFORM case: many matches, many maps, many seeds, all three desktop
# platforms in every single one.
#
# WHY THIS EXISTS
#
# The evidence base is lopsided, and the gap is exactly where a sceptic will push.
#
#   macOS vs Linux    75,000 frames, 50 matches, 10 maps, varied seeds  (xplat-determinism-soak)
#   Windows, active   ~4,200 frames: xplat3-5ai-1500, xplat-lan-ai-1500, relay-winhost-600,
#                     relay-win-vs-linux-600   (all recounted from the committed .crc files)
#   plus              relay-3platform-93017 - 93,017 frames, macOS+Linux+Windows in ONE match,
#                     all three streams md5 f65722d3252acf0737cb4b0e5ba3f13f, 93,016 distinct.
#                     On TWILIGHT FLAME, at rev 2162, with 1 AI and 4 players.
#
# So the gap is narrower than a frame count alone suggests, but it is still real: only 1500 of
# those frames have Windows in the same match as BOTH other platforms at the CURRENT revision, on
# one map with one seed. One clean counterexample falsifies "it's guaranteed to mismatch" and we
# have several. Neither answers "it desyncs eventually", which is a claim about the tail. This
# script attacks the tail by running the three-platform match repeatedly with pinned,
# non-patterned seeds.
#
# HOW IT DIFFERS FROM xplat-determinism-soak.sh
#
# That one runs SOLO headless matches with no packets at all, which isolates simulation from
# transport and is the right shape for proving determinism between two machines. This one cannot
# do that - a three-platform match IS a networked match - so a failure here is ambiguous between
# simulation and transport by construction. When one fails, re-run that exact seed and map through
# xplat-determinism-soak.sh to tell the two apart. That is the point of pinning the seed.
#
# WHAT MAKES A RESULT REAL HERE
#
# Every iteration reports its DISTINCT CRC count. Three peers agreeing on a simulation that did
# nothing is not evidence, and this project has produced that result twice already: relay-8peer
# (distinct=3) and the first run of the three-platform test (distinct=4 of 1500), both on
# `killing fields`, both with the AI correctly placed and idle. An iteration whose distinct count
# is under half its frame count is counted as INCONCLUSIVE, not as a pass.
#
# Usage:
#   ITERATIONS=20 FRAMES=1500 ./scripts/test/xplat-3platform-soak.sh
#   nohup ITERATIONS=40 ./scripts/test/xplat-3platform-soak.sh > ~/soak3.log 2>&1 &
#
# Env:
#   ITERATIONS  matches to run          (default 10)
#   FRAMES      logic frames per match  (default 1500)
#   AI          -lanai spec             (default Hx5 - five Hard AI, filling the lobby to 8/8)
#   SEED_BASE   first seed              (default 900000)
#   OUT         results dir             (default timestamped under /tmp)
#
# Everything else (VPS, WIN, netns, game paths) is inherited by xplat-3platform-lobby.sh, which
# this script drives once per iteration. It is deliberately a driver and not a copy: the three
# launch-shape traps that script documents (relative-path loose scan, the ssh subshell that holds
# the channel open, OpenSSH killing the Windows process tree) each cost real time to find, and a
# second copy of that logic would drift away from the fixed one.

set -uo pipefail

# Shared map capacity table - used to drop maps that cannot seat this lobby before the soak starts.
. "$(dirname "$0")/lib-map-capacity.sh"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOBBY="$HERE/xplat-3platform-lobby.sh"

ITERATIONS="${ITERATIONS:-10}"
FRAMES="${FRAMES:-1500}"
AI="${AI:-Hx5}"
SEED_BASE="${SEED_BASE:-900000}"
OUT="${OUT:-/tmp/gx-soak3-$(date +%Y%m%d-%H%M%S)}"
# Restrict the rotation to maps matching this substring. The reason to use it is the networked
# freeze (evidence/networked-sim-freeze.meta.txt): until that is fixed, only alpine assault
# actually runs a networked match, so a long-haul run that wants real frames rather than
# INCONCLUSIVE ones sets ONLY_MAP=alpine. Leave it empty for the full rotation, which is what you
# want when checking whether the freeze is fixed.
ONLY_MAP="${ONLY_MAP:-}"

say() { printf '%s\n' "$*"; }
die() { printf 'FATAL: %s\n' "$*" >&2; exit 1; }

[ -x "$LOBBY" ] || die "missing $LOBBY"
mkdir -p "$OUT"

# The full standard multiplayer set, killing fields INCLUDED, deliberately.
#
# GeneralsX @fix Claude 29/07/2026 THE COMMENT THAT USED TO BE HERE WAS WRONG AND IS WORTH
# RECORDING. It said most of these maps "freeze in a NETWORKED match" while alpine assault was the
# only one observed to run, and told the reader not to prune them because the INCONCLUSIVE count
# was the signal of a real engine bug.
#
# There is no freeze. This harness seats 3 humans and adds $AI on top, and most of these maps hold
# TWO players. The surplus took startPos=-1, got no Command Center, and their AI owned nothing -
# so the world genuinely did stop changing, because nothing was left in it that moves. alpine only
# looked alive because it carries a scripted train that self-moves with no player attached and
# whose transform is hashed into the object CRC. See
# docs/WORKDIR/evidence/networked-sim-freeze-diagnosis.md.
#
# So the rotation is kept in full, but a map that cannot seat 3 humans + the AI is now SKIPPED and
# COUNTED rather than run and scored. With three human peers this harness can never use a 2-start
# map at all, whatever the AI count. Use ONLY_MAP and a lower AI to reach the smaller maps
# deliberately; the three that hold 8 are death valley, destruction station and twilight flame.
MAPS=(
	"maps\\alpine assault\\alpine assault.map"
	"maps\\killing fields\\killing fields.map"
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

if [ -n "$ONLY_MAP" ]; then
	FILTERED=()
	for m in "${MAPS[@]}"; do
		case "$m" in *"$ONLY_MAP"*) FILTERED+=("$m") ;; esac
	done
	[ "${#FILTERED[@]}" -gt 0 ] || die "ONLY_MAP='$ONLY_MAP' matched no map in the rotation"
	MAPS=("${FILTERED[@]}")
	say "ONLY_MAP='$ONLY_MAP' -> ${#MAPS[@]} map(s) in rotation"
fi

# GeneralsX @feature Claude 29/07/2026 Drop maps that cannot seat this lobby, ONCE, up front, and
# name every one that was dropped. Filtering here rather than skipping per-iteration keeps the
# iteration count meaningful: all $ITERATIONS matches still run, on maps where the players actually
# exist. Announcing the exclusions is the point - a soak that quietly measures a different
# experiment than the one requested is exactly what produced the "networked freeze".
NEED=$(( 3 + $(ai_spec_count "$AI") ))
SKIPPED=0
FITTING=()
EXCLUDED=""
for m in "${MAPS[@]}"; do
	if lobby_fits "$m" "$NEED" >/dev/null 2>&1; then
		FITTING+=("$m")
	else
		SKIPPED=$((SKIPPED + 1))
		EXCLUDED="$EXCLUDED
   - ${m##*\\} (holds $(map_capacity "$m"))"
	fi
done
if [ -n "$EXCLUDED" ]; then
	say ""
	say " EXCLUDED $SKIPPED map(s): this lobby needs $NEED (3 humans + $AI) and they hold less:$EXCLUDED"
	say " Reach them deliberately with a lower AI and ONLY_MAP, e.g. AI=Hx0 ONLY_MAP='cairo'."
fi
[ "${#FITTING[@]}" -gt 0 ] ||
	die "no map in the rotation can seat $NEED players (3 humans + $AI).
       Only death valley, destruction station and twilight flame hold 8. Lower AI."
MAPS=("${FITTING[@]}")
say " rotation: ${#MAPS[@]} map(s) that hold at least $NEED"

say "======================================================================"
say " three-platform soak: macOS + Linux + Windows, $AI, $ITERATIONS x $FRAMES frames"
say " results: $OUT"
say "======================================================================"

# ---------------------------------------------------------------------------------------------
# Iteration 0 runs the FULL lobby script including the fingerprint probes; every later iteration
# skips them. Checking three builds match is a per-BUILD fact, not a per-match one, and the probes
# are ~90s each. But it must happen at least once, or a soak can rack up an impressive frame count
# comparing two peers that were never the same binary.
# ---------------------------------------------------------------------------------------------
TOTAL_FRAMES=0
PASSED=0
FAILED=0
INCONCLUSIVE=0
FAILED_TAGS=""

for ((i = 0; i < ITERATIONS; i++)); do
	SEED=$((SEED_BASE + i * 977))			# 977 is prime, so the seeds fall into no pattern
	MAP="${MAPS[$((i % ${#MAPS[@]}))]}"
	TAG="$(printf '%03d' "$i")"
	IDIR="$OUT/$TAG"

	say ""
	say "=== [$TAG] seed=$SEED map=${MAP##*\\} ==="

	if [ "$i" -eq 0 ]; then SKIP_FP=0; else SKIP_FP=1; fi

	mkdir -p "$IDIR"
	# The env assignments are a prefix, so they scope to this one command; the parent's OUT is
	# untouched and still points at the soak dir.
	OUT="$IDIR" FRAMES="$FRAMES" AI="$AI" MAP="$MAP" SEED="$SEED" \
		SKIP_FINGERPRINT="$SKIP_FP" "$LOBBY" > "$IDIR/console.txt" 2>&1
	RC=$?

	# Re-derive the numbers from the iteration's own files rather than parsing the child's
	# stdout. A summary line is a claim; the .crc files are the measurement.
	if [ ! -s "$IDIR/mac.val" ] || [ ! -s "$IDIR/linux.val" ] || [ ! -s "$IDIR/win.val" ]; then
		say "  FAIL: an incomplete run (exit $RC) - see $IDIR"
		FAILED=$((FAILED + 1)); FAILED_TAGS="$FAILED_TAGS $TAG(no-frames)"
		continue
	fi

	N=$(wc -l < "$IDIR/mac.val" | tr -d ' ')
	DISTINCT=$(sort -u "$IDIR/mac.val" | wc -l | tr -d ' ')
	M1=$(md5 -q "$IDIR/mac.val" 2>/dev/null || md5sum "$IDIR/mac.val" | awk '{print $1}')
	M2=$(md5 -q "$IDIR/linux.val" 2>/dev/null || md5sum "$IDIR/linux.val" | awk '{print $1}')
	M3=$(md5 -q "$IDIR/win.val" 2>/dev/null || md5sum "$IDIR/win.val" | awk '{print $1}')

	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$TAG" "$SEED" "${MAP##*\\}" "$N" "$DISTINCT" "$M1" \
		"$([ "$M1" = "$M2" ] && [ "$M1" = "$M3" ] && echo IDENTICAL || echo DIVERGED)" \
		>> "$OUT/manifest.tsv"

	if [ "$M1" != "$M2" ] || [ "$M1" != "$M3" ]; then
		say "  DIVERGED  mac=$M1 linux=$M2 win=$M3"
		say "            re-run this seed through xplat-determinism-soak.sh to tell"
		say "            simulation divergence from a transport failure"
		FAILED=$((FAILED + 1)); FAILED_TAGS="$FAILED_TAGS $TAG(seed=$SEED)"
	elif [ "$DISTINCT" -lt $((N / 2)) ]; then
		say "  INCONCLUSIVE  $N frames identical, but only $DISTINCT distinct - sim was idle"
		INCONCLUSIVE=$((INCONCLUSIVE + 1))
	else
		say "  ok   $N frames, 0 differing, $DISTINCT distinct, md5 $M1"
		PASSED=$((PASSED + 1))
		TOTAL_FRAMES=$((TOTAL_FRAMES + N))
	fi
done

say ""
say "======================================================================"
say " matches passed  : $PASSED of $ITERATIONS"
say " inconclusive    : $INCONCLUSIVE (identical but idle - not counted as evidence)"
say " maps excluded   : $SKIPPED (too small to seat 3 humans + $AI - never run, never scored)"
say " diverged/failed : $FAILED $FAILED_TAGS"
say " frames of real  : $TOTAL_FRAMES   (passing matches only)"
say " manifest        : $OUT/manifest.tsv"
say "======================================================================"

if [ "$FAILED" -eq 0 ] && [ "$PASSED" -eq "$ITERATIONS" ]; then
	say "PASS: $TOTAL_FRAMES frames across $PASSED matches, macOS + Linux + Windows, zero differing"
	exit 0
fi
say "FAIL"
exit 1
