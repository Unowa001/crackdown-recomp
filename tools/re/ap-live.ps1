# Unattended AP bridge run: isolated user-data root (kept between runs so a later
# run can Continue/Load the same native save), menu input replay, optional
# screenshots of the child's own window. Stops only its own child process.
param([Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_-]+$')][string]$RunName,
      [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Profile,
      [string]$Replay, [int]$Seconds = 90, [int[]]$ScreenshotAt = @(),
      [string[]]$GameArgs = @(), [switch]$WaitForExit, [switch]$CloseWindow)
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$output = Join-Path $repo 'out/ap-live'
$build = Join-Path $repo 'crackdown/out/build/win-amd64-release'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$log = Join-Path $output "$RunName.log"
if (Test-Path -LiteralPath $log) { throw "Live log $log already exists" }
$arguments = @(
    ('--game_data_root="' + (Join-Path $repo 'game') + '"'),
    ('--user_data_root="' + (Join-Path $output "$Profile-user-data") + '"'),
    ('--cache_root="' + (Join-Path $output 'cache') + '"'),
    ('--log_file="' + $log + '"'),
    '--fullscreen=false','--window_width=1280','--window_height=720','--audio_mute=true')
if ($Replay) { $arguments += ('--input_replay="' + (Resolve-Path $Replay) + '"') }
$arguments += $GameArgs
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class ApLiveCapture {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int L,T,R,B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out Rect r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
'@
function Capture($process, $path) {
    $process.Refresh()
    $handle = $process.MainWindowHandle
    $rect = New-Object ApLiveCapture+Rect
    if ($handle -eq [IntPtr]::Zero -or -not [ApLiveCapture]::GetWindowRect($handle, [ref]$rect)) { return }
    $bitmap = New-Object System.Drawing.Bitmap ($rect.R-$rect.L),($rect.B-$rect.T)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    try { [void][ApLiveCapture]::PrintWindow($handle, $dc, 2) } finally { $graphics.ReleaseHdc($dc); $graphics.Dispose() }
    try { $bitmap.Save($path) } finally { $bitmap.Dispose() }
}
$process = Start-Process -FilePath (Join-Path $build 'crackdown.exe') -ArgumentList $arguments -WorkingDirectory $build -WindowStyle Hidden -PassThru
$start = Get-Date
try {
    $shots = [System.Collections.Generic.List[int]]::new([int[]]$ScreenshotAt)
    while (-not $process.HasExited -and ((Get-Date) - $start).TotalSeconds -lt $Seconds) {
        Start-Sleep -Milliseconds 250
        $elapsed = ((Get-Date) - $start).TotalSeconds
        while ($shots.Count -and $elapsed -ge $shots[0]) {
            Capture $process (Join-Path $output ("$RunName-{0:000}s.png" -f $shots[0]))
            $shots.RemoveAt(0)
        }
    }
    if ($WaitForExit -and -not $process.HasExited) { Write-Output 'timeout before exit' }
    # Graceful close (window close -> normal shutdown path); reports how long the exit took.
    if ($CloseWindow -and -not $process.HasExited) {
        $closing = Get-Date
        [void]$process.CloseMainWindow()
        if ($process.WaitForExit(30000)) { Write-Output ("closed in {0:N1}s" -f ((Get-Date) - $closing).TotalSeconds) }
        else { Write-Output 'still running 30 s after window close' }
    }
} finally {
    if (-not $process.HasExited) { Stop-Process -Id $process.Id; $process.WaitForExit() }
}
Write-Output ("exit={0} elapsed={1:N0}s log={2}" -f $process.ExitCode, ((Get-Date) - $start).TotalSeconds, $log)
