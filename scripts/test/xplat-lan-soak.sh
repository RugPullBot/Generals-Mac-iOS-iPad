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
# Where the retail data actually lives. See the joiner launch below - omitting this does not
# error, it hangs.
WIN_DATA="${WIN_DATA:-C:\Program Files (x86)\Steam\steamapps\common\Command & Conquer Generals - Zero Hour}"
MAP="${MAP:-maps\\twilight flame\\twilight flame.map}"
FRAMES="${FRAMES:-2000}"
# Windows engine init alone takes ~45s before the joiner reaches the lobby, so both sides need
# a lobby timeout well above that or the host gives up while the peer is still loading.
HOST_TIMEOUT="${HOST_TIMEOUT:-300000}"
WIN_TIMEOUT="${WIN_TIMEOUT:-300000}"
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

# Matching commits are NOT sufficient: sourceID is baked in at COMPILE time from HEAD plus a
# dirty-file overlay, so a binary built before the last commit reports the older revision and a
# different source digest even though `git rev-parse` now agrees on both machines. That cost two
# full soak runs. Compare each binary's mtime against the commit timestamp and refuse to run if
# either predates it.
# Date the last commit that touched the DIGESTED directories, not HEAD. sourceID is computed
# only from these (see resources/gitinfo/simsourcedigest_watcher.cmake), so a docs- or
# script-only commit does not invalidate a binary and must not abort the run.
COMMIT_EPOCH="$(git -C "$(dirname "$0")/../.." log -1 --format=%ct -- \
	Core/GameEngine GeneralsMD/Code/GameEngine)"
MAC_BIN_EPOCH="$(stat -f %m "$MAC_GAME/GeneralsXZH" 2>/dev/null || echo 0)"
WIN_BIN_EPOCH="$(ssh "$WIN_HOST" "[int][double]::Parse((Get-Date (Get-Item '$WIN_RUN\\generalszh.exe').LastWriteTimeUtc -UFormat %s))" 2>/dev/null | tr -d '\r' | tr -d '[:space:]')"
echo "commit epoch=$COMMIT_EPOCH  mac binary=$MAC_BIN_EPOCH  windows binary=$WIN_BIN_EPOCH"
if [ "${MAC_BIN_EPOCH:-0}" -lt "$COMMIT_EPOCH" ]; then
	echo "ABORT: the Mac binary predates HEAD - rebuild AFTER committing, then deploy."
	exit 1
fi
if [ -n "$WIN_BIN_EPOCH" ] && [ "$WIN_BIN_EPOCH" -lt "$COMMIT_EPOCH" ]; then
	echo "ABORT: the Windows binary predates HEAD - rebuild AFTER committing, then stage."
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

# Assert this, do not just print it. The LAN roles deliberately do NOT enable multi-instance,
# so a single leftover process holds the single-instance mutex and the new joiner stalls during
# init with almost no output - which looks like a network failure, not a stale process.
WIN_LEFT="$(ssh "$WIN_HOST" 'Stop-Process -Name generalszh -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 3; (Get-Process generalszh -ErrorAction SilentlyContinue | Measure-Object).Count' 2>/dev/null | tr -d '\r' | tr -d '[:space:]')"
echo "windows processes remaining: $WIN_LEFT"
[ "$WIN_LEFT" = "0" ] || { echo "ABORT: could not clear Windows processes"; exit 1; }

# --- 3. Host on the Mac, join from Windows ----------------------------------------------------
say "starting Mac host"
(
	cd "$MAC_GAME" && ./run.sh -headless -lanhost soak \
		-lanmap "$MAP" -lanwait 1 -lanframes "$FRAMES" -lantimeout "$HOST_TIMEOUT"
) > "$OUT/mac.out" 2> "$OUT/mac.err" &
MAC_PID=$!

# Give the host time to bind and start announcing before the joiner probes it.
sleep 10

say "starting Windows joiner"
# generalszh.exe is a WINDOWS-subsystem binary, so PowerShell does NOT wait for it: invoking it
# directly returns instantly with exit 0 and no output, which looks exactly like a run that
# produced nothing. Start-Process -Wait is what actually blocks, and the redirects have to be
# absolute paths because -WorkingDirectory does not apply to them.
#
# The working directory matters beyond convenience: loose data files are opened relative to it
# (SidesList.cpp:520), which is how SkirmishScripts.scb went missing and caused a silent desync.
#
# CNC_GENERALS_ZH_PATH is MANDATORY and its absence is silent: without it the engine finds no
# game data, stops dead at "[INI] ERROR: No files read from directory 'Data\INI\Default\GameData'"
# and hangs there forever with 1239 bytes of stderr. That looks exactly like a network failure
# from the host's side - the host just waits out its timeout with no peer. It is not session 0:
# a headless replay fails the same way without it and succeeds from session 0 with it.
ssh "$WIN_HOST" "\$env:CNC_GENERALS_ZH_PATH = '$WIN_DATA'; \
	\$p = Start-Process cmd -ArgumentList '/c','generalszh.exe -headless -lanjoin $MAC_IP -lanframes $FRAMES -lantimeout $WIN_TIMEOUT 2> $WIN_RUN\\win.err' \
	-WorkingDirectory '$WIN_RUN' -NoNewWindow -Wait -PassThru; \
	Write-Output \"EXIT=\$(\$p.ExitCode)\"" > "$OUT/win.status" 2>&1
WIN_RC=$(tr -d '\r' < "$OUT/win.status" | sed -n 's/^EXIT=//p')
cat "$OUT/win.status"

wait $MAC_PID
MAC_RC=$?

echo "mac exit=$MAC_RC  windows exit=$WIN_RC"

# Forward slashes: the remote path is re-parsed by a shell on the Windows side, which eats the
# backslashes and then reports the file as missing even though it is there.
scp -q "$WIN_HOST:C:/dev/GeneralsX-run/win.err" "$OUT/win.err" || echo "WARN: could not fetch win.err"

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
