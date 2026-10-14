param([int]$TimeoutSeconds = 100, [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$RunName = 'live', [switch]$RepeatMenu,
      [ValidatePattern('^-?[0-9.]+,-?[0-9.]+,-?[0-9.]+$')][string]$Teleport,
      [switch]$Probe, [switch]$NoXP, [ValidateRange(0,5)][int]$AgilityLevel = 0,
      [switch]$CaptureScreenshot, [switch]$Interact, [switch]$SixtyFPS,
      [ValidateRange(5,30)][int]$AfterTeleportSeconds = 5)
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$output = Join-Path $repo 'out/ap-discovery'
$build = Join-Path $repo 'crackdown/out/build/win-amd64-release'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$log = Join-Path $output "$RunName.log"
if (Test-Path -LiteralPath $log) { throw 'Live log already exists; inspect before another run' }
$replay = Join-Path $output 'discovery.rec'
$menus = @()
if (-not $RepeatMenu) { $menus = Get-Content (Join-Path $repo 'crackdown/bench/stairs_tp.rec') | Where-Object { $_ -match '^[SK] 0 ' } }
if ($RepeatMenu) {
    # Startup timing can vary with a cold cache. Remaining menu events are discarded
    # automatically when the existing replay hook detects gameplay.
    $menus = @('S 0 0.0 0000 0 0 0 0 0 0')
    for ($ms=6000; $ms -lt 95000; $ms+=2000) {
        $start = ($ms % 10000) -eq 0
        $button = if($start){'0010'}else{'1000'}
        $key = if($start){'5814'}else{'5800'}
        $menus += "S 0 $ms $button 0 0 0 0 0 0", "K 0 $ms $key 0000 0001 0 0"
        $release=$ms+150
        $menus += "S 0 $release 0000 0 0 0 0 0 0", "K 0 $release $key 0000 0002 0 0"
    }
}
$gameplay=@('S 1 0.0 0000 0 0 0 0 0 0')
if ($Interact) {
    foreach ($ms in @(14000,16000,18000,20000)) {
        $release=$ms+150
        # Supply-point HUD requests Y (XInput bit 0x8000, virtual key 0x5803).
        $gameplay += "S 1 $ms 8000 0 0 0 0 0 0", "K 1 $ms 5803 0000 0001 0 0",
                     "S 1 $release 0000 0 0 0 0 0 0", "K 1 $release 5803 0000 0002 0 0"
    }
}
@($menus;$gameplay;'S 1 180000.0 0000 0 0 0 0 0 0') | Set-Content -LiteralPath $replay -Encoding ASCII
$arguments = @(
    ('--game_data_root="' + (Join-Path $repo 'game') + '"'),
    ('--user_data_root="' + (Join-Path $output "$RunName-user-data") + '"'),
    ('--cache_root="' + (Join-Path $output "$RunName-cache") + '"'),
    ('--input_replay="' + $replay + '"'),
    ('--log_file="' + $log + '"'),
    '--fullscreen=false','--window_width=640','--window_height=360','--audio_mute=true'
)
if ($SixtyFPS) { $arguments += '--vsync=false','--fps_cap=60' }
else { $arguments += '--vsync=true' }
if ($Teleport) { $arguments += "--teleport=$Teleport", '--teleport_delay_s=12' }
if ($NoXP -or $AgilityLevel) { $Probe = $true }
if ($Probe) { $arguments += '--ap_probe=true' }
if ($NoXP) { $arguments += '--ap_probe_no_xp=true' }
if ($AgilityLevel) { $arguments += "--ap_probe_agility_level=$AgilityLevel" }
$process = Start-Process -FilePath (Join-Path $build 'crackdown.exe') -ArgumentList $arguments -WorkingDirectory $build -WindowStyle Hidden -PassThru
try {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $ready = $false
    while (-not $process.HasExited -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 300
        if ((Test-Path -LiteralPath $log) -and (Select-String -LiteralPath $log -Pattern 'input: gameplay segment starts' -Quiet)) { $ready=$true; break }
    }
    if (-not $ready) { throw 'Isolated New Game did not reach gameplay within the discovery timeout' }
    Start-Sleep -Seconds 3
    $arena = Select-String -LiteralPath $log -Pattern 'virtual base 0x([0-9A-Fa-f]+)' | Select-Object -First 1
    if (-not $arena) { throw 'No guest memory mapping in log' }
    $base = $arena.Matches[0].Groups[1].Value
    & python (Join-Path $PSScriptRoot 'live_snapshot.py') --pid $process.Id --base $base --output (Join-Path $output "$RunName-snapshot")
    if ($LASTEXITCODE -ne 0) { throw 'Bounded memory sampling failed' }
    if ($Teleport) {
        $deadline=(Get-Date).AddSeconds(30)
        while (-not $process.HasExited -and (Get-Date) -lt $deadline -and
               -not (Select-String -LiteralPath $log -Pattern 'teleport: controller' -Quiet)) { Start-Sleep -Milliseconds 200 }
        if (-not (Select-String -LiteralPath $log -Pattern 'teleport: controller' -Quiet)) { throw 'Diagnostic teleport did not occur' }
        Start-Sleep -Seconds $AfterTeleportSeconds
        & python (Join-Path $PSScriptRoot 'live_snapshot.py') --pid $process.Id --base $base --output (Join-Path $output "$RunName-after-snapshot")
        if ($LASTEXITCODE -ne 0) { throw 'Post-teleport sampling failed' }
    }
    if ($CaptureScreenshot) {
        # Capture only this child's rendered window; do not record the desktop.
        Add-Type -AssemblyName System.Drawing
        Add-Type @'
using System;
using System.Runtime.InteropServices;
public class ProbeWindowCapture {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int L,T,R,B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out Rect r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
'@
        $process.Refresh()
        $handle=$process.MainWindowHandle
        $rect=New-Object ProbeWindowCapture+Rect
        if ($handle -eq [IntPtr]::Zero -or -not [ProbeWindowCapture]::GetWindowRect($handle,[ref]$rect)) { throw 'No child window to capture' }
        $bitmap=New-Object System.Drawing.Bitmap ($rect.R-$rect.L),($rect.B-$rect.T)
        $graphics=[System.Drawing.Graphics]::FromImage($bitmap)
        $dc=$graphics.GetHdc()
        try {
            if (-not [ProbeWindowCapture]::PrintWindow($handle,$dc,2)) { throw 'PrintWindow failed' }
        } finally { $graphics.ReleaseHdc($dc); $graphics.Dispose() }
        try { $bitmap.Save((Join-Path $output "$RunName-window.png")) } finally { $bitmap.Dispose() }
    }
} finally {
    if (-not $process.HasExited) { Stop-Process -Id $process.Id; $process.WaitForExit() }
}
Write-Output 'Isolated gameplay sampling complete. Existing player saves were not loaded or modified.'
