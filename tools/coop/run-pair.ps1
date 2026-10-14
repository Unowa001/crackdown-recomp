param([int]$BootSeconds=45,[int]$LoadSeconds=90,[string[]]$Extra=@())
# Two-instance System Link run on one PC: A hosts, B joins, A launches.
# Stops earlier coop-live instances first. Screenshots land in each instance
# directory (pair-*.png). Delays match the 2026-10-13 runs; check the shots.
$ErrorActionPreference='Stop'
$coopTools=$PSScriptRoot
$coopRepo=(Resolve-Path "$PSScriptRoot/../..").Path
$coopRoot="$coopRepo/out/coop-live"
$coopStart="$coopRepo/out/coop/out/start-live.ps1"
Get-Process crackdown -ErrorAction SilentlyContinue | Where-Object { $_.Path -like '*coop-live*' } | Stop-Process -Force
Start-Sleep -Seconds 2
foreach ($coopRole in 'A','B') { Remove-Item "$coopRoot/$coopRole/trace.jsonl" -ErrorAction SilentlyContinue }
& $coopStart -Role A -Recording neutral.rec -Extra $Extra
Start-Sleep -Seconds $BootSeconds
& "$coopTools/nav-syslink.ps1" -InstanceDirectory "$coopRoot/A" -Role Host -FromTitle
& "$coopTools/step.ps1" -InstanceDirectory "$coopRoot/A" -Shot pair-1-host.png
& $coopStart -Role B -Recording neutral.rec -Extra $Extra
Start-Sleep -Seconds ($BootSeconds+5)
& "$coopTools/nav-syslink.ps1" -InstanceDirectory "$coopRoot/B" -Role Join -FromTitle
& "$coopTools/step.ps1" -InstanceDirectory "$coopRoot/B" -Shot pair-2-list.png
& "$coopTools/step.ps1" -InstanceDirectory "$coopRoot/B" -Buttons A -DelayMs 10000 -Shot pair-3-joined.png
& "$coopTools/step.ps1" -InstanceDirectory "$coopRoot/A" -Shot pair-3-lobby.png
& "$coopTools/step.ps1" -InstanceDirectory "$coopRoot/A" -Buttons A -DelayMs ($LoadSeconds*1000) -Shot pair-4-game.png
& "$coopTools/step.ps1" -InstanceDirectory "$coopRoot/B" -Shot pair-4-game.png
