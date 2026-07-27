# Stage the Windows x64 build into its OWN run folder and launch it.
# GeneralsX @build Claude 27/07/2026
#
# WHY A SEPARATE FOLDER: Generals Online runs EAC (Easy Anti-Cheat) against its signed
# exe inside the Steam directory. Dropping our unsigned DXVK d3d8.dll/d3d9.dll beside it
# would at best break Generals Online and at worst flag the account. So the Steam install
# is treated as READ-ONLY game data and nothing is ever written into it.
#
#   C:\dev\GeneralsX-run\      our exe + our DXVK DLLs + our stubs + our logs
#   Steam\...\Zero Hour\       READ-ONLY, untouched, EAC-clean
#
# Verify that claim with scripts/build/windows/steam-manifest.ps1 before and after a run:
# the MANIFEST-SHA256 must be identical.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File setup-run-win64.ps1            # stage only
#   powershell -ExecutionPolicy Bypass -File setup-run-win64.ps1 -Launch    # stage + launch

param(
    [switch]$Launch,
    [int]$Seconds = 60,
    [string]$GameArgs = "-win -xres 1600 -yres 900"
)

$run   = "C:\dev\GeneralsX-run"
$build = "C:\dev\GeneralsX\build\win64"
$zh    = "$build\GeneralsMD\Release"
$dxvk  = "C:\dev\dxvk\dxvk-2.6\x64"

# NOTE THE TRAILING BACKSLASH - it is load-bearing, not cosmetic.
# Win32LocalFileSystem::getFileListInDirectory (Win32LocalFileSystem.cpp:131-137) builds its
# search string as originalDirectory + currentDirectory + searchName with NO separator inserted.
# Without the trailing backslash the mask becomes "...Zero Hour*.big", FindFirstFile matches
# nothing, and the engine reports "did not provide BIG files" and then dies loading INI - a
# completely silent, extremely misleading failure mode.
$steam = "C:\Program Files (x86)\Steam\steamapps\common\Command & Conquer Generals - Zero Hour\"

New-Item -ItemType Directory -Force -Path $run,"$run\logs","$run\shadercache" | Out-Null

Copy-Item "$zh\generalszh.exe" $run -Force
Copy-Item "$zh\generalszh.pdb" $run -Force   # symbols beside the exe for crash triage
Copy-Item "$zh\zlib1.dll"      $run -Force   # vcpkg app-local dependency

# Windows 11 x64 ships NO d3d8.dll (only a 32-bit one in SysWOW64), so DXVK's is required.
Copy-Item "$dxvk\d3d8.dll" $run -Force
Copy-Item "$dxvk\d3d9.dll" $run -Force

# The Miles/Bink STUBS. These are x64 DLLs built from source by miles-sdk-stub / bink-sdk-stub,
# deliberately named mss32.dll / binkw32.dll so they satisfy the import table. Retail Miles and
# Bink are 32-bit only (both are "14C machine (x86)") and can NEVER load into an x64 process.
# Omitting these is not a soft failure: the exe dies at 0xC0000135 STATUS_DLL_NOT_FOUND before
# WinMain runs, with zero output on stdout and stderr.
Copy-Item "$build\_deps\miles-build\Release\mss32.dll"   $run -Force
Copy-Item "$build\_deps\bink-build\Release\binkw32.dll"  $run -Force

@"
# Logs and shader cache are pinned inside the run folder so nothing lands in Steam.
dxvk.logLevel = info
"@ | Set-Content "$run\dxvk.conf" -Encoding ASCII

# Quoting the whole assignment keeps the '&' in the Steam path literal under cmd.
@"
@echo off
set "CNC_GENERALS_ZH_PATH=$steam"
set "DXVK_LOG_PATH=$run\logs"
set "DXVK_STATE_CACHE_PATH=$run\shadercache"
set "DXVK_CONFIG_FILE=$run\dxvk.conf"
cd /d $run
generalszh.exe %* 2> logs\err.txt
echo GAME EXIT: %errorlevel% >> logs\err.txt
"@ | Set-Content "$run\go.bat" -Encoding ASCII

Write-Output "=== staged into $run ==="
Get-ChildItem $run -File | Select-Object Name,Length | Format-Table -AutoSize

if (-not $Launch) { return }

# A GUI + Vulkan app MUST run in the interactive session. Launched over SSH you land in
# session 0, where DXVK enumerates ZERO adapters and W3DDisplay::init() dies at 0xC0000005.
# A transient scheduled task with /it reaches the logged-on user's session; it is deleted below.
Write-Output ("PRE count: " + @(Get-Process generalszh -ErrorAction SilentlyContinue).Count)
schtasks /create /tn GXRun /tr "cmd /c $run\go.bat $GameArgs" /sc once /st 23:59 /f /it /ru $env:USERNAME | Out-Null
schtasks /run /tn GXRun | Out-Null

Start-Sleep -Seconds $Seconds
$g = Get-Process generalszh -ErrorAction SilentlyContinue
if ($g) {
    Write-Output ("RUNNING cpu=" + [math]::Round($g.TotalProcessorTime.TotalSeconds,2) +
                  "s ws=" + [math]::Round($g.WorkingSet64/1MB,1) + "MB")
} else {
    Write-Output "EXITED on its own"
}

Get-Process generalszh -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
schtasks /end /tn GXRun 2>$null | Out-Null
schtasks /delete /tn GXRun /f | Out-Null
# A kill that killed nothing looks identical to one that worked - always confirm the count.
Write-Output ("POST count: " + @(Get-Process generalszh -ErrorAction SilentlyContinue).Count)

if (Test-Path "$run\logs\err.txt") {
    $a = Get-Content "$run\logs\err.txt"
    Write-Output ("log lines: " + $a.Count)
    Write-Output "--- subsystems initialised ---"
    ($a | Select-String -Pattern "initSubsystem\(.*\) END").Count
    Write-Output "--- errors ---"
    # \bERROR\b deliberately: a bare "ERROR" pattern also matches WW3D_ERROR_OK, which is a
    # SUCCESS line, and reports a phantom error on a completely clean boot.
    ($a | Select-String -Pattern "\bERROR\b|FATAL|CRASH|err:").Count
}
