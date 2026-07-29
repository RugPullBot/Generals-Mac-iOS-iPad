#!/usr/bin/env bash
# Map transfer over the relay. Runs ON the relay host.
#
# WHY THIS HAS NEVER BEEN TESTED, AND WHY IT NEEDS A CUSTOM MAP
#
# Every match this project has ever run used a built-in map - one inside MapsZH.big, which every
# peer has by definition. A map in the archive can never be the missing one, so the transfer path
# has never executed even once over the relay.
#
# The transfer is UNICAST, host -> the one peer that lacks the map. That is precisely what the GXR1
# routing header was added for: with two peers "forward it to the other one" is unambiguous, but a
# broadcast fallback would deliver the map to every peer in the room and the bug would be invisible
# - everyone would end up with the file regardless of whether routing worked.
#
# HOW THE MISSING MAP IS ARRANGED
#
# Not by deleting anything. Each peer gets its own XDG_DATA_HOME (GlobalData.cpp resolves the user
# data dir from it), and user maps live under <userdata>/Maps/<name>/<name>.map. So the host gets a
# Maps directory containing an extracted stock map under a NEW name, and the joiner gets an empty
# one. Nothing in the shared game data is touched and nothing has to be restored afterwards.
#
# The map is extracted from MapsZH.big rather than authored: entries are stored exactly as a loose
# file would be on disk (EAR-wrapped RefPack included), so the bytes are a valid loose map. Renaming
# it is what makes it custom - under its own name the joiner would already have it.
#
# Usage, on the relay host:
#   SRC_MAP='Maps\Killing Fields\Killing Fields.map' ./relay-maptransfer.sh
#
# Env:
#   SRC_MAP    entry to extract from MapsZH.big  (default Killing Fields)
#   CUSTOM     name to install it under          (default gxcustom)
#   FRAMES     logic frames                      (default 600)
#   GAME       game data dir                     (default /root/gamedata)
#   RELAY_IP   relay address                     (default 163.5.210.131)
#   BIGTOOL    path to bigtool.py                (default /root/bigtool.py)
#   OUT        results dir                       (default timestamped under /root)

set -uo pipefail

# Source map must have room for the whole lobby: host + 1 joiner + the AI. This test runs
# -lanwait 1 with AI=Hx1, so it needs THREE start positions.
#
# GeneralsX @fix Claude 29/07/2026 Was Alpine Assault, which holds TWO. The comment that chose it
# claimed alpine was "a map whose AI actually moves" on the evidence of 1499 distinct CRC values.
# That reasoning is now known to be wrong in both halves: the 1499 came from a scripted train that
# self-moves with no player attached and whose transform is hashed into the object CRC, and with
# three occupied slots on a 2-start map the AI here was getting no Command Center at all. The
# "frozen simulation" this comment blamed on missing skirmish scripts was the same overfilled-lobby
# defect. See docs/WORKDIR/evidence/networked-sim-freeze-diagnosis.md.
SRC_MAP="${SRC_MAP:-Maps\\Twilight Flame\\Twilight Flame.map}"
CUSTOM="${CUSTOM:-gxcustom}"
AI="${AI:-Hx1}"
FRAMES="${FRAMES:-600}"
GAME="${GAME:-/root/gamedata}"
RELAY_IP="${RELAY_IP:-163.5.210.131}"
BIGTOOL="${BIGTOOL:-/root/bigtool.py}"
OUT="${OUT:-/root/maptransfer-$(date +%Y%m%d-%H%M%S)}"

say() { printf '%s\n' "$*"; }
die() { printf 'FATAL: %s\n' "$*" >&2; exit 1; }

mkdir -p "$OUT"

say "=== preflight ==="
[ -x "$GAME/GeneralsXZH" ] || die "no binary at $GAME/GeneralsXZH"
[ -f "$BIGTOOL" ] || die "no bigtool at $BIGTOOL"
for ns in gx gx2; do
	ip netns list | grep -qw "$ns" || die "netns '$ns' missing"
done

pkill -x GeneralsXZH 2>/dev/null
sleep 2
LEFT="$(ps -eo comm | grep -cx GeneralsXZH)"
[ "$LEFT" = "0" ] || die "$LEFT GeneralsXZH still running"
say "no games running"

# --- host gets the map, joiner does not ------------------------------------------------------
HOST_HOME="$OUT/home0"
JOIN_HOME="$OUT/home1"
HOST_MAPS="$HOST_HOME/GeneralsX/GeneralsZH/Maps/$CUSTOM"
JOIN_MAPS="$JOIN_HOME/GeneralsX/GeneralsZH/Maps"
mkdir -p "$HOST_MAPS" "$JOIN_MAPS"
for h in "$HOST_HOME" "$JOIN_HOME"; do
	printf 'RelayAddress = %s\n' "$RELAY_IP" > "$h/GeneralsX/GeneralsZH/Options.ini"
done

python3 "$BIGTOOL" "$GAME/MapsZH.big" extract "$SRC_MAP" "$HOST_MAPS/$CUSTOM.map" ||
	die "could not extract $SRC_MAP"

# RENAMING A STOCK MAP IS NOT ENOUGH, and the failure is silent. A map is identified by CRC and
# size, not by name, so an extracted stock map still matches the copy every peer already has inside
# MapsZH.big: the joiner resolves it locally, no transfer is attempted, and the run still ends with
# both peers simulating the same 600 frames byte-identically. It looks exactly like a pass.
#
# Padding the file gives it a CRC and size no peer can already hold, which is what forces the
# transfer. The engine reads the EAR header for the real payload length and ignores the tail, so
# the map still loads - verified: stock crc=DEA9E8E4 size=275491, padded crc=C8F6A91C size=275517.
printf 'GXTESTPAD_%s' "$CUSTOM" >> "$HOST_MAPS/$CUSTOM.map"
HOST_MAP_MD5="$(md5sum "$HOST_MAPS/$CUSTOM.map" | cut -d' ' -f1)"
HOST_MAP_SIZE="$(stat -c %s "$HOST_MAPS/$CUSTOM.map")"
say "host has $CUSTOM.map  size=$HOST_MAP_SIZE md5=$HOST_MAP_MD5"

# The assertion the whole test rests on. If the joiner already has the file, a transfer that never
# happens looks exactly like a transfer that worked.
[ -e "$JOIN_MAPS/$CUSTOM/$CUSTOM.map" ] && die "joiner already has the map - test would prove nothing"
say "joiner has no $CUSTOM.map (verified)"

# And the same assertion against the archive, which is where the first version of this test went
# wrong: the joiner does not need a loose copy if MapsZH.big holds one with the same CRC and size.
if python3 "$BIGTOOL" "$GAME/MapsZH.big" list | grep -q " $HOST_MAP_SIZE  "; then
	say "NOTE: an archive entry shares this size - relying on the CRC pad to force the transfer"
fi
say ""

# A loose map is NOT addressable the way a built-in one is. Maps inside MapsZH.big are cached under
# a relative key - "maps\alpine assault\alpine assault.map" - but a map in the user data dir is
# cached under its FULL, LOWERCASED absolute path:
#
#   available: /root/.../home0/generalsx/generalszh/maps/gxcustom/gxcustom.map
#
# so passing the relative form gets "is not in the map cache", the host aborts before the lobby
# starts, and the joiner reports join failed code 1 (RET_TIMEOUT) - a timeout that has nothing to do
# with the network. Address it exactly as the cache lists it.
MAPARG="$(printf '%s' "$HOST_MAPS/$CUSTOM.map" | tr '[:upper:]' '[:lower:]')"
say "map key: $MAPARG"

launch() {	# launch <index> <home> <netns> <name> <extra args...>
	local i="$1" home="$2" ns="$3" nm="$4"; shift 4
	setsid nohup ip netns exec "$ns" env \
		CNC_GENERALS_ZH_PATH="$GAME" XDG_DATA_HOME="$home" \
		${ROOM:+GENERALSX_LANROOM="$ROOM"} \
		"$GAME/GeneralsXZH" -headless "$@" \
		-lanname "$nm" -lanframes "$FRAMES" -lantimeout 900000 \
		> "$OUT/peer$i.out" 2> "$OUT/peer$i.err" < /dev/null &
	echo $!
}

ROOM=""
say "=== host, with the custom map ==="
HOST_PID="$(launch 0 "$HOST_HOME" gx gxhost -lanhost maptransfer -lanwait 1 \
	-lanmap "$MAPARG" ${AI:+-lanai "$AI"})"
for _ in $(seq 1 60); do
	ROOM="$(grep -ao "in room [a-z0-9]*" "$OUT/peer0.err" 2>/dev/null | head -1 | awk '{print $3}')"
	[ -n "$ROOM" ] && break
	kill -0 "$HOST_PID" 2>/dev/null || die "host died - see $OUT/peer0.err"
	sleep 2
done
[ -n "$ROOM" ] || die "host never reported a room - see $OUT/peer0.err"
say "room: $ROOM"

# No -lanmap for the joiner. It must learn the map from the host's game options and then obtain it.
say "=== joiner, without it ==="
JOIN_PID="$(launch 1 "$JOIN_HOME" gx2 gxjoin -lanjoin 10.42.0.1)"
say "host=$HOST_PID joiner=$JOIN_PID"
say ""

for p in "$HOST_PID" "$JOIN_PID"; do
	while kill -0 "$p" 2>/dev/null; do sleep 5; done
done
say "both exited"
say ""

# --- did the file actually move? -------------------------------------------------------------
say "=== the file ==="
GOT="$JOIN_MAPS/$CUSTOM/$CUSTOM.map"
if [ -f "$GOT" ]; then
	GOT_MD5="$(md5sum "$GOT" | cut -d' ' -f1)"
	say "  joiner now has $CUSTOM.map size=$(stat -c %s "$GOT") md5=$GOT_MD5"
	if [ "$GOT_MD5" = "$HOST_MAP_MD5" ]; then
		say "  md5 MATCHES the host's copy - transferred intact"
		FILE_OK=1
	else
		say "  md5 DIFFERS from the host's copy - transferred corrupt"
		FILE_OK=0
	fi
else
	say "  joiner still has no $CUSTOM.map - no transfer happened"
	find "$JOIN_MAPS" -type f | sed 's/^/    saw: /'
	FILE_OK=0
fi
say ""

say "=== transfer trail ==="
grep -aiE "file ?transfer|sending file|map transfer|requesting map|got map|\[GXLAN\].*map" \
	"$OUT/peer0.err" "$OUT/peer1.err" | tail -12 | sed 's/^/  /'
say ""

say "=== streams ==="
for i in 0 1; do
	grep -a '^\[GXCRC\]' "$OUT/peer$i.err" | tr -d '\r' > "$OUT/peer$i.crc"
	n=$(wc -l < "$OUT/peer$i.crc" | tr -d ' ')
	k=$(awk '{print $3}' "$OUT/peer$i.crc" | sort -u | wc -l | tr -d ' ')
	printf '  peer%d frames=%-6s distinct=%-6s md5=%s\n' "$i" "$n" "$k" \
		"$(md5sum < "$OUT/peer$i.crc" | cut -d' ' -f1)"
	[ "$n" -eq 0 ] && grep -a -m1 'join failed, code\|UDP::Bind failed\|transportInit=FAILED' \
		"$OUT/peer$i.err" | sed 's/^/    /'
done

A=$(wc -l < "$OUT/peer0.crc" | tr -d ' ')
B=$(wc -l < "$OUT/peer1.crc" | tr -d ' ')
N=$(( A < B ? A : B ))
DIFF=0
if [ "$N" -gt 0 ]; then
	head -n "$N" "$OUT/peer0.crc" > "$OUT/.a"; head -n "$N" "$OUT/peer1.crc" > "$OUT/.b"
	DIFF=$(paste "$OUT/.a" "$OUT/.b" | awk '$3!=$6' | wc -l | tr -d ' ')
	rm -f "$OUT/.a" "$OUT/.b"
fi

say ""
say "======================================================================"
say " map            : $CUSTOM (from $SRC_MAP)"
say " file received  : $([ "${FILE_OK:-0}" = 1 ] && echo yes || echo NO)"
say " frames compared: $N"
say " differing      : $DIFF"
say " results        : $OUT"
say "======================================================================"
if [ "${FILE_OK:-0}" = 1 ] && [ "$N" -gt 0 ] && [ "$DIFF" -eq 0 ]; then
	say "PASS: the map crossed the relay and both peers simulated it identically"
	exit 0
fi
say "FAIL"
exit 1
