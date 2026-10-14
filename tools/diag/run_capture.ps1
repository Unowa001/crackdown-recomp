# Like run_replay.ps1, but when the log shows a line matching -Trigger (default: a --diag_vehicle hit on the player's
# car), save window PNGs 1.5 s and 4 s later as <OutPrefix>-a.png / -b.png:
# run_capture.ps1 -OutPrefix C:\tmp\crash60 [-Trigger regex] <game args...>
param([string]$OutPrefix, [string]$Trigger = 'diag_vehicle: hit dv',
      [Parameter(ValueFromRemainingArguments)][string[]]$rest)
$dir = Join-Path $PSScriptRoot "..\..\crackdown\out\build\win-amd64-release" | Resolve-Path
$before = (Get-ChildItem "$dir\logs\crackdown_*.log" | Sort-Object LastWriteTime | Select-Object -Last 1).Name
$p = Start-Process -FilePath "$dir\crackdown.exe" -ArgumentList $rest -WorkingDirectory $dir -PassThru
$log = $null; $t0 = Get-Date
while (-not $p.HasExited -and ((Get-Date) - $t0).TotalSeconds -lt 300) {
  Start-Sleep -Milliseconds 200
  if (-not $log) {
    $l = Get-ChildItem "$dir\logs\crackdown_*.log" | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($l.Name -ne $before) { $log = $l.FullName }
    continue
  }
  if (Select-String -Path $log -Pattern $Trigger -Quiet) {
    Start-Sleep -Milliseconds 1500; & "$PSScriptRoot\capture_window.ps1" -ProcessId $p.Id -Out "$OutPrefix-a.png"
    Start-Sleep -Milliseconds 2500; & "$PSScriptRoot\capture_window.ps1" -ProcessId $p.Id -Out "$OutPrefix-b.png"
    break
  }
}
$p.WaitForExit(120000) | Out-Null
"LOG " + (Split-Path $log -Leaf)
