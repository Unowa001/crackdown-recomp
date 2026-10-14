param([Parameter(Mandatory=$true)][string]$InstanceDirectory,
      [string[]]$Buttons=@(),
      [int]$DelayMs=1500,
      [string]$Shot='step.png')
# Presses each button in turn (send-input.ps1), then captures the instance
# window (pid.txt) to $InstanceDirectory/$Shot. Check the shot before the next
# step: timed recordings desync when two instances boot at once.
foreach ($coopButton in $Buttons) {
  & (Join-Path $PSScriptRoot 'send-input.ps1') -InstanceDirectory $InstanceDirectory -Button $coopButton
  Start-Sleep -Milliseconds $DelayMs
}
$coopPid=[int](Get-Content (Join-Path $InstanceDirectory 'pid.txt'))
& (Join-Path $PSScriptRoot 'capture-window.ps1') -GamePid $coopPid -OutputPath (Join-Path $InstanceDirectory $Shot)
