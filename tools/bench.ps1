# One benchmark phase (playbook section 8) in one command: launches the game,
# waits for you to get in position, records PresentMon for -Seconds, closes the
# game and prints the summary. Run from an elevated PowerShell (PresentMon
# needs ETW):
#   powershell -ExecutionPolicy Bypass -File tools\bench.ps1 -Name stand_1x
#   powershell -ExecutionPolicy Bypass -File tools\bench.ps1 -Name stand_2x -GameArgs "--resolution_scale=2"
# Output: crackdown\bench\<Name>.csv (game counters), pm_<Name>.csv (PresentMon).
param(
  [Parameter(Mandatory = $true)][string]$Name,
  [string]$GameArgs = "",
  [int]$Seconds = 30,
  [string]$Preset = "win-amd64-profile",
  [string]$PresentMon = "C:\tools\PresentMon.exe"
)

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  Write-Error "Run this from an elevated PowerShell: PresentMon needs ETW."
  exit 1
}

$root = Split-Path $PSScriptRoot -Parent
$game = Join-Path $root "crackdown"
$exe = Join-Path $game "out\build\$Preset\crackdown.exe"
$out = Join-Path $game "bench"
New-Item -ItemType Directory -Force $out | Out-Null
$pm = Join-Path $out "pm_$Name.csv"
Remove-Item -ErrorAction SilentlyContinue $pm, (Join-Path $out "$Name.csv")

# (PowerShell names are case-insensitive: don't reuse $GameArgs here.)
$launchArgs = @("--game_data_root=..\game", "--gpu_plugin=xenos", "--perf_log_csv=bench\$Name.csv")
$launchArgs += @($GameArgs -split '\s+' | Where-Object { $_ })
$launchLine = $launchArgs -join ' '
Write-Host "Starting: crackdown.exe $launchLine"
$proc = Start-Process -FilePath $exe -ArgumentList $launchLine -WorkingDirectory $game -PassThru

Read-Host "New Game, wait at the spawn point until streaming settles, then press Enter here (you get 5 s to switch back to the game and stand still)"
Start-Sleep -Seconds 5
Write-Host "Recording $Seconds s..."
& $PresentMon --process_name crackdown.exe --output_file $pm --timed $Seconds --terminate_after_timed `
  --no_console_stats --stop_existing_session | Out-Null
Stop-Process -Id $proc.Id -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

python (Join-Path $PSScriptRoot "bench_summary.py") $out $Name
