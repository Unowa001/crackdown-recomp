# Run the release exe unattended (e.g. with --input_replay, which quits by itself), then print the log name and the
# lines matching a pattern, timestamps trimmed: run_replay.ps1 <pattern> <game args...>
param([string]$pattern, [Parameter(ValueFromRemainingArguments)][string[]]$rest)
$dir = Join-Path $PSScriptRoot "..\..\crackdown\out\build\win-amd64-release" | Resolve-Path
$p = Start-Process -FilePath "$dir\crackdown.exe" -ArgumentList $rest -WorkingDirectory $dir -PassThru
if (-not $p.WaitForExit(400000)) { Stop-Process -Id $p.Id -Force; "timeout" }
$log = Get-ChildItem "$dir\logs\crackdown_*.log" | Sort-Object LastWriteTime | Select-Object -Last 1
"LOG " + $log.Name
Select-String -Path $log.FullName -Pattern $pattern | ForEach-Object {
  $l = $_.Line; $i = $l.IndexOf('] [t'); if ($i -ge 0) { $l = $l.Substring($l.IndexOf(']', $i + 3) + 2) }
  $l.Substring(0, [Math]::Min(400, $l.Length))
}
