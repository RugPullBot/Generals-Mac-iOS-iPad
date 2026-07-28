#!/usr/bin/env bash
# GeneralsX @feature Claude 28/07/2026
#
# Cross-platform LAN determinism soak: Mac hosts headless, Windows joins headless, both emit
# one [GXCRC] line per frame, and the two streams are diffed.
#
# This is the test that can actually find determinism bugs. Two peers on ONE box agree by
# construction - 80,000+ same-platform frames have never desynced - so the comparison has to be
# cross-architecture.
#
# PREREQUISITE, and it is not optional: both peers must be built from the SAME commit. The
# join is refused on a sourceID mismatch, and sourceID hashes HEAD plus a dirty-file overlay.
# The script checks this rather than letting it surface as a confusing join failure.

set -uo pipefail

WIN_HOST="${WIN_HOST:-User@192.168.10.89}"
MAC_GAME="${MAC_GAME:-$HOME/GeneralsX/GeneralsZH}"
WIN_RUN='C:\dev\GeneralsX-run'
MAP="${MAP:-maps\\twilight flame\\twilight flame.map}"
FRAMES="${FRAMES:-2000}"
MAC_IP="${MAC_IP:-192.168.10.51}"
OUT="${OUT:-$(pwd)/soak-out}"

mkdir -p "$OUT"

say() { printf '\n=== %s ===\n' "$*"; }

# --- 1. Both peers must be at the same commit -------------------------------------------------
say "checking build identity"
LOCAL_HEAD="$(git -C "$(dirname "$0")/../.." rev-parse HEAD)"
REMOTE_HEAD="$(ssh "$WIN_HOST" 'cd C:\dev\GeneralsX; git rev-parse HEAD' 2>/dev/null | tr -d '\r')"
echo "mac:     $LOCAL_HEAD"
echo "windows: $REMOTE_HEAD"
if [ "$LOCAL_HEAD" != "$REMOTE_HEAD" ]; then
	echo "ABORT: the two machines are on different commits. The join would be refused on"
	echo "       sourceID. Sync the Windows clone with a pure mirror first:"
	echo "         ssh $WIN_HOST 'cd C:\\dev\\GeneralsX; git fetch; git reset --hard origin/main'"
	echo "       (fetch+reset, never pull - a local edit there silently blocks the sync)"
	exit 1
fi

# --- 2. No stale processes on either side -----------------------------------------------------
# A pkill that matched nothing looks identical to one that worked, so confirm the count.
say "clearing stale processes"
pkill -f GeneralsXZH 2>/dev/null
sleep 1
MAC_LEFT="$(pgrep -f GeneralsXZH | wc -l | tr -d ' ')"   # macOS pgrep has no -c
echo "mac processes remaining: $MAC_LEFT"
[ "$MAC_LEFT" = "0" ] || { echo "ABORT: could not clear Mac processes"; exit 1; }

ssh "$WIN_HOST" 'Stop-Process -Name GeneralsXZH -Force -ErrorAction SilentlyContinue; (Get-Process GeneralsXZH -ErrorAction SilentlyContinue | Measure-Object).Count' 2>/dev/null | tr -d '\r'

# --- 3. Host on the Mac, join from Windows ----------------------------------------------------
say "starting Mac host"
(
	cd "$MAC_GAME" && ./run.sh -headless -lanhost soak \
		-lanmap "$MAP" -lanwait 1 -lanframes "$FRAMES"
) > "$OUT/mac.out" 2> "$OUT/mac.err" &
MAC_PID=$!

# Give the host time to bind and start announcing before the joiner probes it.
sleep 10

say "starting Windows joiner"
ssh "$WIN_HOST" "cd $WIN_RUN; .\\GeneralsXZH.exe -headless -lanjoin $MAC_IP -lanframes $FRAMES 2> win.err" \
	> "$OUT/win.out" 2>&1
WIN_RC=$?

wait $MAC_PID
MAC_RC=$?

echo "mac exit=$MAC_RC  windows exit=$WIN_RC"

scp -q "$WIN_HOST:$WIN_RUN\\win.err" "$OUT/win.err" || echo "WARN: could not fetch win.err"

# --- 4. Diff the CRC streams ------------------------------------------------------------------
say "comparing CRC streams"
grep '^\[GXCRC\]' "$OUT/mac.err" > "$OUT/mac.crc"
grep '^\[GXCRC\]' "$OUT/win.err" > "$OUT/win.crc"

MAC_N=$(wc -l < "$OUT/mac.crc" | tr -d ' ')
WIN_N=$(wc -l < "$OUT/win.crc" | tr -d ' ')
echo "mac frames: $MAC_N   windows frames: $WIN_N"

if [ "$MAC_N" = "0" ] || [ "$WIN_N" = "0" ]; then
	echo "FAIL: a peer produced no CRC output - check $OUT/*.err for [GXLAN] lines"
	exit 1
fi

if diff -q "$OUT/mac.crc" "$OUT/win.crc" > /dev/null; then
	echo "PASS: $MAC_N frames, zero differing"
	exit 0
fi

# Name the first divergent frame - that is the only number worth acting on.
echo "FAIL: streams diverge. First differing frame:"
diff "$OUT/mac.crc" "$OUT/win.crc" | head -10
exit 1
