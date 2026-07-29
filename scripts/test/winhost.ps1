# Windows as a headless relay HOST. Windows has only ever been a joiner.
#
# Two things that are mandatory and fail silently if omitted (both recorded in xplat-lan-soak.sh):
#   * generalszh.exe is a WINDOWS-subsystem binary, so PowerShell does not wait for it. Only
#     Start-Process -Wait actually blocks; invoking it directly returns immediately.
#   * CNC_GENERALS_ZH_PATH is mandatory. Without it the engine finds no data and hangs having
#     written about 1239 bytes of stderr.
#
# The command line is built as ONE string here rather than passed through ssh, because the map
# argument contains both spaces and backslashes and does not survive bash -> ssh -> PowerShell
# quoting ("A positional parameter cannot be found that accepts argument 'maps\alpine'").

$env:CNC_GENERALS_ZH_PATH = "C:\Program Files (x86)\Steam\steamapps\common\Command & Conquer Generals - Zero Hour"

$run    = 'C:\dev\GeneralsX-run'
$errlog = Join-Path $run 'winhost.err'
Remove-Item $errlog -ErrorAction SilentlyContinue

$cmdline = '/c generalszh.exe -headless -lanhost winhost -lanname winhost -lanai Hx1 ' +
           '-lanmap "maps\alpine assault\alpine assault.map" -lanwait 1 -lanframes 600 ' +
           '-lantimeout 900000 2> "' + $errlog + '"'

$p = Start-Process cmd -ArgumentList $cmdline -WorkingDirectory $run -NoNewWindow -Wait -PassThru
Write-Output "EXIT=$($p.ExitCode)"
