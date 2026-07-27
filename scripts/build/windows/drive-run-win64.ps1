# GeneralsX @build Claude 27/07/2026 - drive the staged Windows x64 build with synthetic input.
#
# MUST run inside the INTERACTIVE session (session 1). Launched over SSH you land in session 0,
# where DXVK enumerates zero adapters and W3DDisplay::init() dies at 0xC0000005. Use a transient
# `schtasks /it` task - see Invoke-DriveInSession1 at the bottom of this file for the exact form.
# A session-1 window is also invisible from an SSH session, so the clicking AND the screenshotting
# both have to happen in here, not from the caller.
#
# Merges the two ad-hoc scripts that proved the skirmish last session (drive.ps1 for clicks and
# screenshots, longrun.ps1 for the stderr drain and liveness sampling) so a LAN match, which needs
# both at once for several minutes, is a single tool.
#
# Coordinates are real desktop pixels. Do NOT compute them from an SSH-side query:
# SystemInformation.VirtualScreen reports 1024x768 from session 0 and 1920x1080 from session 1.

param(
    [string]$Clicks   = "",                            # "x,y;x,y;..." in desktop pixels
    [int]$Settle      = 45,                            # seconds to wait after launch before click 1
    [int]$Between     = 6,                             # seconds between clicks
    [int]$Hold        = 0,                             # seconds to keep running after the last click
    [int]$ShotEvery   = 0,                             # if >0, screenshot every N seconds during Hold
    [string]$GameArgs = "-win -xres 1600 -yres 900",
    [string]$Tag      = "run"                          # prefix for this run's artifacts
)

$ErrorActionPreference = "Stop"
$run  = "C:\dev\GeneralsX-run"
$logs = "$run\logs"
New-Item -ItemType Directory -Force -Path $logs | Out-Null

$errFile = "$logs\$Tag-stderr.txt"
$statusFile = "$logs\$Tag-status.txt"
Remove-Item $errFile, $statusFile -ErrorAction SilentlyContinue

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Mouse {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern IntPtr SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    public static RECT Window(IntPtr h) { RECT r; GetWindowRect(h, out r); return r; }
    public static POINT Cursor() { POINT p; GetCursorPos(out p); return p; }
    public static IntPtr Foreground() { return GetForegroundWindow(); }

    // Reports where the cursor ACTUALLY is at each stage of a click, plus which window owns
    // focus. If the game captures or re-centres the cursor, SetCursorPos will not stick and the
    // button under the pointer at mouse_event time is not the one we aimed at.
    // Atomic absolute move + press + release, submitted as ONE SendInput batch.
    //
    // SetCursorPos followed by mouse_event is a race: mouse_event clicks wherever the pointer
    // happens to be AT THAT MOMENT, so anything else moving the mouse - a person at the keyboard,
    // a remote-desktop session relaying pointer motion - silently redirects the click to whatever
    // is under the pointer then. Measured: an aimed (1767,515) became beforeDown=(1756,463) and
    // afterUp=(2108,660), i.e. the press and release landed on different controls than intended.
    // Bundling the move with the button events leaves no window for interference.
    //
    // Absolute coordinates are 0..65535 normalised over the VIRTUAL screen, so they must be
    // computed against VirtualScreen origin and size, not the primary display.
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT { public uint type; public MOUSEINPUT mi; }
    // Do NOT add manual padding here. SendInput validates cbSize against its own struct size and
    // silently returns 0 - inserting nothing - when it disagrees; there is no error dialog and the
    // click just does not happen. Hand-added pad fields made MOUSEINPUT 40 bytes and INPUT 48,
    // where x64 wants 32 and 40, and every click reported sendInput=0. The CLR already aligns
    // IntPtr to 8 under LayoutKind.Sequential, which produces the correct layout on its own.
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT {
        public int dx; public int dy; public uint mouseData;
        public uint dwFlags; public uint time; public IntPtr dwExtraInfo;
    }
    [DllImport("user32.dll", SetLastError = true)]
    public static extern uint SendInput(uint n, INPUT[] inputs, int cb);

    const uint MOVE = 0x0001, LDOWN = 0x0002, LUP = 0x0004, ABS = 0x8000, VDESK = 0x4000;

    public static uint MoveAbs(int x, int y, int vx, int vy, int vw, int vh) {
        int nx = (int)(((double)(x - vx)) * 65535.0 / (vw - 1));
        int ny = (int)(((double)(y - vy)) * 65535.0 / (vh - 1));
        INPUT[] seq = new INPUT[1];
        seq[0].type = 0; seq[0].mi.dx = nx; seq[0].mi.dy = ny;
        seq[0].mi.dwFlags = MOVE | ABS | VDESK;
        return SendInput(1, seq, Marshal.SizeOf(typeof(INPUT)));
    }

    // Press and release ONLY - no movement. The move must already have happened, several frames
    // earlier. The shell resolves which control is under the pointer once per frame and then
    // handles the click against THAT control, so bundling the move with the button events in one
    // SendInput batch presses before the hover has been recomputed and the click is attributed to
    // the previously hovered control. That is why clicking MULTIPLAYER worked (already hovered
    // after settle) while clicking NETWORK immediately afterwards did nothing, and why a human
    // click - which is always a move followed many frames later by a press - worked every time.
    public static uint PressRelease() {
        INPUT[] seq = new INPUT[2];
        seq[0].type = 0; seq[0].mi.dwFlags = LDOWN;
        seq[1].type = 0; seq[1].mi.dwFlags = LUP;
        return SendInput(2, seq, Marshal.SizeOf(typeof(INPUT)));
    }

    public static string ClickTraced(int x, int y) {
        SetCursorPos(x, y - 60);
        System.Threading.Thread.Sleep(120);
        POINT a = Cursor();
        SetCursorPos(x, y);
        System.Threading.Thread.Sleep(300);
        POINT b = Cursor();
        mouse_event(0x0002, 0, 0, 0, IntPtr.Zero);
        System.Threading.Thread.Sleep(80);
        POINT c = Cursor();
        mouse_event(0x0004, 0, 0, 0, IntPtr.Zero);
        System.Threading.Thread.Sleep(120);
        POINT d = Cursor();
        return "approach=(" + a.X + "," + a.Y + ") beforeDown=(" + b.X + "," + b.Y +
               ") afterDown=(" + c.X + "," + c.Y + ") afterUp=(" + d.X + "," + d.Y + ")";
    }
    // The approach move is NOT cosmetic. The shell tracks which control is under the cursor from
    // mouse-MOVE events, and SetCursorPos to the position the cursor already occupies generates no
    // move at all. So two consecutive clicks at the SAME coordinate - exactly what navigating
    // MainMenu"MULTIPLAYER" then submenu"NETWORK" needs, since they share a slot - leave the
    // engine's hovered control stale across the menu swap, and the second click is attributed to
    // slot 0. Observed precisely: clicking NETWORK's coordinate twice opened ONLINE and produced
    // the GameSpy "CANNOT CONNECT" dialog, while hovering the identical coordinate correctly
    // highlighted NETWORK. Stepping in from an offset guarantees a real WM_MOUSEMOVE first.
    public static void Click(int x, int y) {
        SetCursorPos(x, y - 60);
        System.Threading.Thread.Sleep(120);
        SetCursorPos(x, y);
        System.Threading.Thread.Sleep(300);
        mouse_event(0x0002, 0, 0, 0, IntPtr.Zero);   // LEFTDOWN
        System.Threading.Thread.Sleep(80);
        mouse_event(0x0004, 0, 0, 0, IntPtr.Zero);   // LEFTUP
    }
    // Client-relative -> screen. The game window does NOT land in the same place on every launch,
    // so absolute desktop coordinates silently miss on the next run: the clicks land on the
    // desktop, the menu never advances, and the only symptom is an absence of [LAN] traces.
    // Observed twice - the main menu sat at x=905 on one boot and x=1145 on the next.
    public static POINT ToScreen(IntPtr h, int cx, int cy) {
        POINT p = new POINT(); p.X = cx; p.Y = cy;
        ClientToScreen(h, ref p);
        return p;
    }
    public static RECT Client(IntPtr h) { RECT r; GetClientRect(h, out r); return r; }
}
"@

$log = New-Object System.Collections.Generic.List[string]
function Note($s) { $log.Add(("[{0:HH:mm:ss}] {1}" -f (Get-Date), $s)) }

function Shot($name) {
    $b = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.X, $b.Y, 0, 0, $bmp.Size)
    $bmp.Save("$logs\$Tag-$name.png", [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}

# Launch with stderr redirected BY THE SHELL, straight to a file.
#
# The engine's release-visible traces ([SIMID], [LAN], [CRC], [ARCHIVES]) are all
# fprintf(stderr, ...) - DEBUG_LOG is ((void)0) in shipping presets - so capturing stderr is
# mandatory, but it must NOT be captured through a pipe.
#
# Do NOT "improve" this into ProcessStartInfo + RedirectStandardError + a background drain.
# That was tried and it HANGS THE GAME: a PowerShell scriptblock cast to [Action] and handed to
# Task.Run executes on a thread-pool thread with no PowerShell runspace attached, so the body
# never runs. Nothing then reads the pipe, its buffer fills, and the child blocks forever on its
# next stderr write - presenting as a live process at ~80 MB and flat CPU with the window titled
# "(Not Responding)". Confirmed twice: longrun.ps1 used that pattern and its stderr file was
# never created on any run, while go.bat's `2> logs\err.txt` produced 515 KB on the same build.
#
# A file handle needs no reader, so the deadlock class does not exist here.
$env:CNC_GENERALS_ZH_PATH  = "C:\Program Files (x86)\Steam\steamapps\common\Command & Conquer Generals - Zero Hour\"
$env:DXVK_LOG_PATH         = $logs
$env:DXVK_STATE_CACHE_PATH = "$run\shadercache"
$env:DXVK_CONFIG_FILE      = "$run\dxvk.conf"

# Silence DXVK's stderr stream, exactly as the macOS run.sh already does. dxvk.conf's
# "dxvk.logLevel = none" governs only the log FILE; the same messages still go to stderr, and the
# engine issues SetRenderState(D3DRS_PATCHSEGMENTS) EVERY FRAME, which the D3D8 layer warns about
# every time. Measured here: 17,253 stderr lines in one run that never left the main menu, the
# tail of it solid PATCHSEGMENTS. Each one is a synchronous write on the render thread, and it
# buries the [SIMID]/[LAN]/[CRC] traces this harness exists to read.
# Set DXVK_LOG_LEVEL=warn by hand when debugging DXVK itself.
$env:DXVK_LOG_LEVEL        = "none"

Note "launching: generalszh.exe $GameArgs (stderr -> $errFile)"
Start-Process cmd -ArgumentList "/c", "generalszh.exe $GameArgs 2> `"$errFile`"" `
    -WorkingDirectory $run -WindowStyle Hidden

Start-Sleep -Seconds $Settle

$p = Get-Process generalszh -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) {
    Note "DIED during settle - no generalszh process. First 40 stderr lines:"
    if (Test-Path $errFile) { Get-Content $errFile -TotalCount 40 | ForEach-Object { Note "  $_" } }
    $log | Set-Content $statusFile
    exit 1
}

if ($p.HasExited) {
    Note ("DIED during settle, exit 0x{0:X8}" -f $p.ExitCode)
    $log | Set-Content $statusFile
    exit 1
}
Note ("alive after settle, hwnd=$($p.MainWindowHandle) cpu=$([math]::Round($p.TotalProcessorTime.TotalSeconds,2))s ws=$([math]::Round($p.WorkingSet64/1MB,1))MB")
[Mouse]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Seconds 1
Shot "step0"

$hwnd = $p.MainWindowHandle
$cr = [Mouse]::Client($hwnd)
$origin = [Mouse]::ToScreen($hwnd, 0, 0)
$wr = [Mouse]::Window($hwnd)

# Everything needed to reconcile screenshot pixels with screen coordinates, recorded FROM INSIDE
# session 1. None of it can be obtained over SSH: queried from session 0, VirtualScreen reports a
# fake 1024x768 single display, so any coordinate computed on the SSH side is wrong.
$vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
Note ("VirtualScreen X=$($vs.X) Y=$($vs.Y) W=$($vs.Width) H=$($vs.Height)")
Note ("windowRect L=$($wr.L) T=$($wr.T) R=$($wr.R) B=$($wr.B)")
Note ("clientSize $($cr.R)x$($cr.B), clientOrigin screen ($($origin.X),$($origin.Y))")
Note ("=> screenshot pixel = screen coord - ($($vs.X),$($vs.Y))")
[System.Windows.Forms.Screen]::AllScreens | ForEach-Object { Note ("screen $($_.DeviceName) bounds=$($_.Bounds) primary=$($_.Primary)") }

$i = 0
if ($Clicks -ne "") {
    foreach ($c in $Clicks.Split(";")) {
        if ($c.Trim() -eq "") { continue }
        $i++
        $step = $c.Trim()

        # "h:x,y" hovers without clicking. The menu highlights whatever the cursor is over, so a
        # hover plus a screenshot identifies which control actually occupies a coordinate - which
        # beats deriving it from a layout file or from the other platform's window. Use it whenever
        # a blind click produced the wrong screen.
        $hoverOnly = $false
        if ($step.StartsWith("h:")) { $hoverOnly = $true; $step = $step.Substring(2) }

        $xy = $step.Split(",")
        # Coordinates are CLIENT-relative and translated here. See Mouse.ToScreen for why.
        $s = [Mouse]::ToScreen($hwnd, [int]$xy[0], [int]$xy[1])
        if ($hoverOnly) { [Mouse]::SetCursorPos($s.X, $s.Y) | Out-Null }
        else {
            # Approach from an offset so a real MOVE is generated, settle onto the target, then
            # DWELL before pressing. The logic runs at 30 Hz, so 900 ms is ~27 frames - far more
            # than the shell needs to recompute the hovered control, and cheap next to a run.
            [Mouse]::MoveAbs($s.X, $s.Y - 60, $vs.X, $vs.Y, $vs.Width, $vs.Height) | Out-Null
            Start-Sleep -Milliseconds 250
            $mv = [Mouse]::MoveAbs($s.X, $s.Y, $vs.X, $vs.Y, $vs.Width, $vs.Height)
            Start-Sleep -Milliseconds 900
            $hoverAt = [Mouse]::Cursor()
            $sent = [Mouse]::PressRelease()
            Start-Sleep -Milliseconds 200
            $after = [Mouse]::Cursor()
            Note ("  click$i aimed=($($s.X),$($s.Y)) move=$mv press=$sent hoverAt=($($hoverAt.X),$($hoverAt.Y)) after=($($after.X),$($after.Y))")
        }
        Start-Sleep -Seconds $Between
        Shot ("step" + $i)
        $p.Refresh()
        if ($p.HasExited) { Note "after-click$i client($($xy[0]),$($xy[1])) screen($($s.X),$($s.Y)) DIED"; break }
        Note ("after-click$i client($($xy[0]),$($xy[1])) screen($($s.X),$($s.Y)) alive cpu=$([math]::Round($p.TotalProcessorTime.TotalSeconds,2))s ws=$([math]::Round($p.WorkingSet64/1MB,1))MB")
    }
}

# Hold the process up so a match can actually play out, sampling liveness so a wedged process is
# distinguishable from a busy one.
if ($Hold -gt 0 -and -not $p.HasExited) {
    Note "holding for $Hold s"
    $t = [System.Diagnostics.Stopwatch]::StartNew()
    $nextShot = $ShotEvery
    while (-not $p.HasExited -and $t.Elapsed.TotalSeconds -lt $Hold) {
        Start-Sleep -Milliseconds 500
        if ($ShotEvery -gt 0 -and $t.Elapsed.TotalSeconds -ge $nextShot) {
            Shot ("hold" + [int]$nextShot)
            $p.Refresh()
            Note ("hold t=$([int]$t.Elapsed.TotalSeconds)s cpu=$([math]::Round($p.TotalProcessorTime.TotalSeconds,2))s ws=$([math]::Round($p.WorkingSet64/1MB,1))MB")
            $nextShot += $ShotEvery
        }
    }
    if ($p.HasExited) { Note ("EXITED during hold after $([int]$t.Elapsed.TotalSeconds)s, code 0x{0:X8}" -f $p.ExitCode) }
    else { Note "still running after hold" }
}

Shot "final"
if (-not $p.HasExited) { $p.Kill() }
Start-Sleep -Seconds 2
Note ("POST process count: " + @(Get-Process generalszh -ErrorAction SilentlyContinue).Count)
$log | Set-Content $statusFile
