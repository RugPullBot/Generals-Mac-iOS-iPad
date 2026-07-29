#!/usr/bin/env bash
# Three real platforms and five Hard AI in one relay lobby, then diff every simulated frame.
#
# WHAT THIS IS FOR
#
# relay-8peer.sh already puts eight peers in a lobby, but all eight are the same Linux binary on
# one box - it proves the transport carries eight ways, not that different platforms simulate the
# same game. xplat-determinism-soak.sh proves macOS and Linux agree, but with zero networking and
# only two peers. This is the case neither covers and the one people actually doubt: a Windows
# x64 build, a Linux build and an Apple Silicon build in the SAME match, with AI running, every
# frame compared.
#
# WHY FIVE AI AND THREE HUMANS
#
# MAX_SLOTS is 8. Three human peers plus five AI fills the lobby exactly, which is the interesting
# case - a partly empty lobby leaves slots that never execute. The AI matter more than the count:
# skirmish scripts attach ONLY for AI players, and that path is where this project's historical
# cross-platform divergence lived. Three humans idling in a lobby agree trivially; five Hard AI
# fighting is what makes each frame do different work.
#
# But the MAP has to be able to hold them. Filling 8 slots on a 2-player map does not fail loudly:
# the surplus players get no start position, no Command Center is placed, their AI owns nothing,
# and the run still reports aiPlayers=5 and completes. The preflight below refuses that case,
# because it produced a "passing" three-platform result that was really measuring an ambient train.
#
# WHY THE LINUX PEER RUNS IN A NAMESPACE
#
# The VPS is also the relay host. Relay mode forces a WILDCARD bind, so the game's socket collides
# with the relay's own listener on 8086 and the peer dies before frame 0 with
# `UDP::Bind failed (status -6)`. A namespace gives it its own port table. Same reason as
# xplat-determinism-soak.sh; see tools/relay/README.md for creating them.
#
# WHY -lanname IS SET EXPLICITLY FOR ALL THREE
#
# The LAN player name falls back to the machine name. It happens to differ across three real
# machines, but relying on that is how relay-8peer.sh got eight identical names and a stream of
# Join Deny code 3 (RET_DUPLICATE_NAME). Naming them is free; debugging that is not.
#
# WHY WINDOWS RUNS HEADLESS HERE
#
# Earlier sessions drove the Windows peer through the GUI with scripted mouse clicks. That was
# never necessary - Windows headless works and reaches SimID in about five seconds. The reason it
# looked broken is that it needs CNC_GENERALS_ZH_PATH pointing at the Steam data install; without
# it the engine stalls forever after "[INI] ERROR: No files read from directory". Set the env and
# the whole run is scriptable.
#
# Usage:
#   FRAMES=1500 ./scripts/test/xplat-3platform-lobby.sh
#
# Env:
#   FRAMES    logic frames             (default 1500)
#   AI        -lanai spec for the host (default Hx5 - five Hard AI)
#   MAP       host's map               (default twilight flame - capacity 8)
#   VPS       linux peer / relay       (default root@163.5.210.131)
#   VPS_NETNS netns for the linux peer (default gx)
#   WIN       windows peer             (default User@192.168.10.89)
#   OUT       results dir              (default timestamped under /tmp)

set -uo pipefail

# Shared map capacity table - see the header of that file for why an over-capacity lobby is not a
# cosmetic problem.
. "$(dirname "$0")/lib-map-capacity.sh"
# Real activity attribution, from the engine's [GXACT] trace rather than the distinct CRC count.
. "$(dirname "$0")/lib-activity.sh"

FRAMES="${FRAMES:-1500}"
AI="${AI:-Hx5}"
# Pin the match seed. Empty means the host picks one, which is right for a single run and wrong
# for a soak: without it every iteration is a different game and "50 matches" is really one map
# sampled 50 ways. The joiners take the seed from the host's game options (the SD= field), so it
# is set on the host only.
SEED="${SEED:-}"
# A soak checks the fingerprints once up front; repeating three ~90s probes per iteration would
# be most of the wall clock and would prove the same thing every time.
SKIP_FINGERPRINT="${SKIP_FINGERPRINT:-0}"
# Must be a map whose capacity is at least 3 humans + the AI count, or the surplus players get no
# start position, no Command Center, and their AI owns nothing while still reporting aiPlayers=N.
# Only three standard maps hold 8: death valley, destruction station, twilight flame (measured -
# md->m_numPlayers, GameLogic.cpp:882-885). alpine assault is TWO, and every earlier run here used
# it; its healthy-looking distinct count came from an ambient train, not from the AI.
MAP="${MAP:-maps\\twilight flame\\twilight flame.map}"
VPS="${VPS:-root@163.5.210.131}"
VPS_NETNS="${VPS_NETNS:-gx}"
WIN="${WIN:-User@192.168.10.89}"
RELAY_IP="${RELAY_IP:-163.5.210.131}"
VPS_GAME="${VPS_GAME:-/root/gamedata}"
MAC_GAME="${MAC_GAME:-$HOME/GeneralsX/GeneralsZH}"
WIN_RUN='C:\dev\GeneralsX-run'
WIN_DATA='C:\Program Files (x86)\Steam\steamapps\common\Command & Conquer Generals - Zero Hour\'
OUT="${OUT:-/tmp/gx-xplat3-$(date +%Y%m%d-%H%M%S)}"

mkdir -p "$OUT"
say() { printf '%s\n' "$*"; }
die() { printf 'FATAL: %s\n' "$*" >&2; kill_all; exit 1; }

# PowerShell 5.1 over ssh has no `&&`, and %ERRORLEVEL% in a one-liner expands at PARSE time -
# a broken command still exits 0 and looks like a pass. Encode the script instead; it also
# removes every layer of quote mangling between here and the remote parser.
winps() {	# winps <<'EOF' ... EOF   - reads a powershell script on stdin, runs it on $WIN
	local b64
	b64="$(iconv -f UTF-8 -t UTF-16LE | base64 | tr -d '\n')"
	ssh -o ConnectTimeout=15 "$WIN" "powershell -NoProfile -EncodedCommand $b64" 2>&1 |
		grep -Ev '^#< CLIXML|^<Objs'
}

# pkill -x, never -f: -f matches our own ssh command line and has killed the remote shell
# (exit 255) in this project more than once.
kill_all() {
	pkill -x GeneralsXZH 2>/dev/null
	ssh -o ConnectTimeout=15 "$VPS" 'pkill -x GeneralsXZH 2>/dev/null; exit 0' >/dev/null 2>&1
	winps >/dev/null 2>&1 <<'EOF'
Get-Process GeneralsXZH,generalszh -ErrorAction SilentlyContinue | Stop-Process -Force
EOF
	sleep 3
}

verify_clean() {
	local mac vps win
	mac="$(pgrep -x GeneralsXZH | wc -l | tr -d ' ')"
	vps="$(ssh -o ConnectTimeout=15 "$VPS" 'ps -eo comm | grep -cx GeneralsXZH' 2>/dev/null | tr -d '[:space:]')"
	win="$(winps <<'EOF' | tr -d '[:space:]'
Write-Output ((Get-Process GeneralsXZH,generalszh -ErrorAction SilentlyContinue | Measure-Object).Count)
EOF
	)"
	# A pkill that killed nothing looks identical to one that worked - always confirm the count.
	say "  running games: mac=$mac linux=${vps:-?} windows=${win:-?}"
	[ "$mac" = "0" ] && [ "${vps:-1}" = "0" ] && [ "${win:-1}" = "0" ]
}

# ---------------------------------------------------------------------------------------------
say "=== preflight ==="
kill_all
verify_clean || die "a game is still running somewhere - refusing to start"

[ -x "$MAC_GAME/GeneralsXZH" ] || die "no mac binary at $MAC_GAME/GeneralsXZH"

# ---------------------------------------------------------------------------------------------
# Map capacity. The table moved to lib-map-capacity.sh so the harnesses cannot drift apart on it;
# the engine now enforces the same rule itself (HeadlessMatch::lobbyFitsTheMap), so getting this
# wrong here fails loudly at the host rather than producing a confident wrong answer.
AI_COUNT="$(ai_spec_count "$AI")"
NEED=$(( 3 + AI_COUNT ))
if ! lobby_fits "$MAP" "$NEED"; then
	die "this lobby needs $NEED (3 humans + $AI_COUNT AI).
       The surplus players would get no start position and their AI would own nothing, and the run
       would still report aiPlayers=$AI_COUNT and 'pass'. Pick death valley, destruction station or
       twilight flame, or lower AI."
fi

ssh -o ConnectTimeout=15 "$VPS" "ip netns list | grep -qw '$VPS_NETNS'" 2>/dev/null ||
	die "netns '$VPS_NETNS' missing on $VPS - see tools/relay/README.md"
ssh -o ConnectTimeout=15 "$VPS" 'pgrep -x node >/dev/null' 2>/dev/null ||
	say "  WARNING: no node process on the VPS - is the relay running?"

# Per-peer data dir for the linux host. XDG_DATA_HOME moves $HOME/GeneralsX/GeneralsZH, which is
# where the engine keeps Options.ini on linux.
VPS_HOME="$OUT/linuxhome"
ssh -o ConnectTimeout=15 "$VPS" "mkdir -p $VPS_HOME/GeneralsX/GeneralsZH &&
	printf 'RelayAddress = %s\n' '$RELAY_IP' > $VPS_HOME/GeneralsX/GeneralsZH/Options.ini" ||
	die "could not provision the linux data dir"

say ""
if [ "$SKIP_FINGERPRINT" = "1" ]; then
say "=== build fingerprints: SKIPPED (SKIP_FINGERPRINT=1, caller asserts) ==="
else
say "=== build fingerprints ==="
# Comparing three peers that are not the same build manufactures evidence. The join path already
# refuses on engineID, but a refusal reads as "connection timed out" and wastes an hour - assert
# it here, where the message can say what actually differs. platformID is EXPECTED to differ: it
# is the OS tag, and it is not part of the compatibility verdict.
simid_fields() { sed -E 's/.*(engine=[0-9A-F]+ source=[0-9A-F]+ data=[0-9A-F]+ ordinal=[0-9A-F]+ parse=[0-9A-F]+ asset=[0-9A-F]+).*/\1/'; }

MAC_SIM="$(cd "$MAC_GAME" && timeout 180 ./run.sh -headless -lanhost simidprobe -lanframes 5 2>&1 |
	grep -a -m1 '\[SIMID\] local')"
pkill -x GeneralsXZH 2>/dev/null; sleep 1

LIN_SIM="$(ssh -o ConnectTimeout=15 "$VPS" "cd $VPS_GAME && ip netns exec $VPS_NETNS env \
	CNC_GENERALS_ZH_PATH=$VPS_GAME timeout 180 ./GeneralsXZH -headless -lanhost simidprobe -lanframes 5 2>&1 |
	grep -a -m1 '\[SIMID\] local'; pkill -x GeneralsXZH 2>/dev/null; exit 0")"

WIN_SIM="$(winps <<EOF
\$env:CNC_GENERALS_ZH_PATH = "$WIN_DATA"
\$env:DXVK_LOG_LEVEL = "none"
Start-Process cmd -ArgumentList "/c", "generalszh.exe -headless -lanhost simidprobe -lanframes 5 2> C:\\dev\\simidprobe.err" -WorkingDirectory "$WIN_RUN" -WindowStyle Hidden
for (\$i = 0; \$i -lt 24; \$i++) {
  Start-Sleep -Seconds 5
  \$l = Select-String -Path C:\\dev\\simidprobe.err -Pattern "\\[SIMID\\] local" -ErrorAction SilentlyContinue | Select-Object -First 1
  if (\$l) { Write-Output \$l.Line; break }
}
Get-Process GeneralsXZH,generalszh -ErrorAction SilentlyContinue | Stop-Process -Force
EOF
)"

for who in MAC LIN WIN; do
	eval "s=\$${who}_SIM"
	[ -n "$s" ] || die "$who never reported a SimID"
	say "  $who: $(printf '%s' "$s" | simid_fields)"
done
MAC_F="$(printf '%s' "$MAC_SIM" | simid_fields)"
LIN_F="$(printf '%s' "$LIN_SIM" | simid_fields)"
WIN_F="$(printf '%s' "$WIN_SIM" | simid_fields)"
[ "$MAC_F" = "$LIN_F" ] || die "mac and linux are different builds"
[ "$MAC_F" = "$WIN_F" ] || die "mac and windows are different builds"
say "  all three identical (platformID excluded by design)"

kill_all
verify_clean || die "could not get back to a clean state after the fingerprint probes"
fi

# ---------------------------------------------------------------------------------------------
say ""
say "=== linux host: -lanai $AI, waiting for 2 joiners ==="
# The `cd` is load-bearing, not tidiness. SimulationId's loose-data scan calls
# getFileListInDirectory with RELATIVE directories, which LocalFileSystem resolves against the
# process cwd - so launching from /root instead of the game dir scans nothing, changes data-loose,
# changes dataID, and every joiner is refused with verdict=DATA_DIFFERS while engineID matches
# perfectly. That failure reads like a genuine cross-platform data mismatch and is not one.
# The redirections belong to the SUBSHELL, not to the game. `cd X && setsid game >f 2>g &`
# backgrounds the whole and-list in a subshell whose own stdout is still the ssh channel, so ssh
# never sees EOF and blocks here until the match ends - the launch looks like a hang and the room
# is never read. Wrapping in ( ... ) with the redirections on the outside closes the channel.
ssh -o ConnectTimeout=20 "$VPS" "( cd $VPS_GAME && exec setsid ip netns exec $VPS_NETNS env \
	CNC_GENERALS_ZH_PATH=$VPS_GAME XDG_DATA_HOME=$VPS_HOME \
	$VPS_GAME/GeneralsXZH -headless -lanhost xplat3 -lanwait 2 -lanai '$AI' \
	-lanmap '$MAP' -lanname gxlinux -lanframes $FRAMES -lantimeout 1800000 \
	${SEED:+-lanseed $SEED} \
	) < /dev/null > $OUT/linux.out 2> $OUT/linux.err & echo started" >/dev/null 2>&1

# The host mints its room token at start-up; the joiners must be pinned to it. Browsing works but
# adds a retry loop to a three-way start-up race for no benefit.
ROOM=""
for _ in $(seq 1 60); do
	ROOM="$(ssh -o ConnectTimeout=15 "$VPS" "grep -ao 'in room [a-z0-9]*' $OUT/linux.err 2>/dev/null | head -1 | awk '{print \$3}'" 2>/dev/null | tr -d '[:space:]')"
	[ -n "$ROOM" ] && break
	sleep 2
done
[ -n "$ROOM" ] || die "linux host never reported a room - see $VPS:$OUT/linux.err"
say "  host room: $ROOM"

say ""
say "=== joiners ==="
( cd "$MAC_GAME" && GENERALSX_LANROOM="$ROOM" nohup ./run.sh -headless -lanjoin 10.42.0.1 \
	-lanname gxmac -lanframes "$FRAMES" -lantimeout 1800000 \
	> "$OUT/mac.out" 2> "$OUT/mac.err" < /dev/null ) &
MAC_PID=$!
say "  mac joining (pid $MAC_PID)"
sleep 2

# The windows peer runs SYNCHRONOUSLY inside a held-open ssh session, and the whole thing is
# backgrounded HERE instead. Detaching it on the far side does not work: OpenSSH on Windows kills
# the process tree when the session closes, so a Start-Process launch dies within milliseconds of
# ssh returning and leaves a zero-byte stderr file - which reads as "the joiner never started"
# rather than "something killed it". Holding the session for the duration is what the SimID probe
# already does, and it is the only shape observed to survive.
( winps <<EOF
\$env:CNC_GENERALS_ZH_PATH = "$WIN_DATA"
\$env:GENERALSX_LANROOM    = "$ROOM"
\$env:DXVK_LOG_LEVEL       = "none"
Remove-Item C:\\dev\\xplat-win.err -ErrorAction SilentlyContinue
Set-Location "$WIN_RUN"
& cmd /c "generalszh.exe -headless -lanjoin 10.42.0.1 -lanname gxwin -lanframes $FRAMES -lantimeout 1800000 2> C:\\dev\\xplat-win.err"
Write-Output "WIN_GAME_RC=\$LASTEXITCODE"
EOF
) > "$OUT/win.launch.log" 2>&1 &
WIN_PID=$!
say "  windows joining (pid $WIN_PID)"

say ""
say "=== simulating $FRAMES frames ==="
wait $MAC_PID; MAC_RC=$?
say "  mac finished (exit $MAC_RC)"
wait $WIN_PID; WIN_RC=$?
# A non-zero code here is not automatically a bad run: the windows build reaches its frame limit,
# logs "simulated N frames", and THEN faults on teardown (0xC0000005). The frames are already
# flushed at that point, so the CRC comparison below is the thing that decides, not this code.
say "  windows finished (ssh wrapper exit $WIN_RC, game code in $OUT/win.launch.log)"

# Only linux is detached, so only linux needs polling. Lockstep keeps the peers within a few
# frames of each other, but the writes are buffered - wait for the process to actually exit.
for _ in $(seq 1 120); do
	l="$(ssh -o ConnectTimeout=15 "$VPS" 'ps -eo comm | grep -cx GeneralsXZH' 2>/dev/null | tr -d '[:space:]')"
	[ "${l:-1}" = "0" ] && break
	sleep 5
done
say "  linux finished"

# ---------------------------------------------------------------------------------------------
say ""
say "=== collecting ==="
scp -q -o ConnectTimeout=15 "$VPS:$OUT/linux.err" "$OUT/linux.err" 2>/dev/null ||
	ssh -o ConnectTimeout=15 "$VPS" "cat $OUT/linux.err" > "$OUT/linux.err"
scp -q -o ConnectTimeout=15 "$WIN:C:/dev/xplat-win.err" "$OUT/win.err" 2>/dev/null ||
	die "could not retrieve the windows stderr"

for who in mac linux win; do
	grep -a '^\[GXCRC\]' "$OUT/$who.err" | tr -d '\r' > "$OUT/$who.crc"
	say "  $who: $(wc -l < "$OUT/$who.crc" | tr -d ' ') frames"
done

kill_all
verify_clean || say "  WARNING: something is still running"

# ---------------------------------------------------------------------------------------------
N=$(for w in mac linux win; do wc -l < "$OUT/$w.crc"; done | sort -n | head -1 | tr -d ' ')
say ""
say "======================================================================"
if [ "${N:-0}" -eq 0 ]; then
	say " FAIL: at least one peer simulated no frames"
	# Name the cause rather than leaving three logs to be read by hand. REFUSED prints the
	# verdict, and the field table that follows says WHICH id differs - dataID and engineID
	# fail identically from the verdict line alone but have completely different causes.
	for w in mac linux win; do
		b="$(grep -a -m1 'UDP::Bind failed\|REFUSED\|Join Deny\|transportInit=FAILED' "$OUT/$w.err" 2>/dev/null)"
		[ -n "$b" ] && say "   $w: $b"
		grep -a -A11 'REFUSED' "$OUT/$w.err" 2>/dev/null | grep -a 'DIFFERS' | sed "s/^/     $w /"
	done
	say "======================================================================"
	exit 1
fi

# Compare the CRC VALUE column only. `paste a b | awk '$2!=$4'` looks right and is not: paste
# shifts the fields, so $2 lands on the other file's tag and every row "differs". That mistake
# nearly reported a false Windows desync in this project.
for w in mac linux win; do
	head -n "$N" "$OUT/$w.crc" | awk '{print $3}' > "$OUT/$w.val"
done
MD5_MAC="$(md5 -q "$OUT/mac.val" 2>/dev/null || md5sum "$OUT/mac.val" | awk '{print $1}')"
MD5_LIN="$(md5 -q "$OUT/linux.val" 2>/dev/null || md5sum "$OUT/linux.val" | awk '{print $1}')"
MD5_WIN="$(md5 -q "$OUT/win.val" 2>/dev/null || md5sum "$OUT/win.val" | awk '{print $1}')"

# The control. A frozen or idling simulation matches trivially - an earlier "identical" run in
# this project had 3 distinct values across 600 frames and proved nothing at all.
DISTINCT="$(sort -u "$OUT/mac.val" | wc -l | tr -d ' ')"

# GeneralsX @fix Claude 29/07/2026 AI_PLACED used to be `grep -ac 'AI player\|lanai'`, was never
# read by anything, and the PASS line below asserted "$AI AI" - the value that was REQUESTED, not
# one that was observed. A soak that silently ran human-only would print "+ Hx5 AI" and pass.
#
# Count what the host actually serialised instead. The S= field of `game options:`
# (HeadlessMatch.cpp:670-673) lists one colon-separated entry per slot, and an AI slot appears as
# CE / CM / CH (Computer Easy / Medium / Hard).
OPTS="$(grep -a -m1 'game options:' "$OUT/linux.err" 2>/dev/null | tr -d '\r' | sed 's/.*game options: //')"
AI_OBSERVED="$(printf '%s' "$OPTS" | sed -E 's/.*;S=([^;]*).*/\1/' | tr ':' '\n' | grep -c '^C[EMH]')"
AI_REQUESTED="$(ai_spec_count "$AI")"

# GeneralsX @fix Claude 29/07/2026 FRAME FLOOR. N is the shortest of the three streams and had no
# floor, so one peer dying at frame 3 turned a 1500-frame experiment into a 3-frame one that still
# printed PASS with all three md5s equal.
FLOOR=$(( FRAMES * 9 / 10 ))

# GeneralsX @feature Claude 29/07/2026 Real activity, from the [GXACT] per-player trace rather
# than the distinct count. A player that never got a start position reads 0/0 all match however
# much ambient scenery moves. Only present when the run was launched with GX_ACTIVITY set.
STARVED="$(starved_players "$OUT/mac.err")"

say " frames compared : $N of $FRAMES requested (floor $FLOOR)"
say " distinct CRCs   : $DISTINCT of $N"
say " AI slots        : $AI_OBSERVED observed in the slot list, $AI_REQUESTED requested ($AI)"
say " macOS   md5     : $MD5_MAC"
say " Linux   md5     : $MD5_LIN"
say " Windows md5     : $MD5_WIN"
say " $(activity_summary "$OUT/mac.err")"
say " results         : $OUT"
say "======================================================================"

# Every claim below is derived from an observation in the logs, never from an input parameter,
# and every verdict is computed over the same denominator it prints.
if [ "$N" -lt "$FLOOR" ]; then
	say "FAIL: only $N frames were comparable of $FRAMES requested (floor $FLOOR)."
	say "  A peer stopped early, so this is an $N-frame experiment. Read the three .err files."
	exit 1
fi
if [ "$AI_OBSERVED" -ne "$AI_REQUESTED" ]; then
	say "FAIL: $AI_REQUESTED AI were requested ($AI) but $AI_OBSERVED appear in the host's slot list."
	say "  Slot list: $OPTS"
	exit 1
fi

if [ "$MD5_MAC" = "$MD5_LIN" ] && [ "$MD5_MAC" = "$MD5_WIN" ]; then
	if [ -n "$STARVED" ]; then
		say "INCONCLUSIVE: all three peers agree, but player(s) $STARVED owned no object at any"
		say "  sample - they were in the lobby and never in the game. This is valid DETERMINISM"
		say "  evidence and is NOT simulation-activity evidence."
		exit 2
	fi
	if [ "$DISTINCT" -lt $((N / 2)) ]; then
		say "INCONCLUSIVE: identical, but only $DISTINCT distinct values in $N frames -"
		say "  the simulation may have been idle. Agreement on an idle sim proves nothing."
		exit 1
	fi
	say "PASS: $N frames (>= $FLOOR), macOS + Linux + Windows, $AI_OBSERVED AI observed in the slot list, one md5 across all three"
	exit 0
fi

say "DIVERGED - first differing frame:"
paste "$OUT/mac.crc" "$OUT/linux.crc" "$OUT/win.crc" | awk '$3!=$6 || $3!=$9' | head -3 | sed 's/^/  /'
exit 1
