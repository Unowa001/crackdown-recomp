param([Parameter(Mandatory=$true)][string]$InstanceDirectory,
      [ValidateSet('Host','Join')][string]$Role='Join',
      [switch]$FromTitle)
# -FromTitle: START on "Press START to play", then Campaign (first main-menu item).
# Timed replays miss the title when two instances boot at once, so take a
# screenshot first and only run this once the title is visible.
# From the Campaign screen (focus on Play Now - Solo): Play Now - Co-op ->
# Team Up (focus starts on the disabled Invite a Friend; disabled items are not
# skipped) -> Host/Join System Link Game -> Join Another Player / Host A Game.
$coopSend=Join-Path $PSScriptRoot 'send-input.ps1'
function Press([string]$Button,[int]$DelayMs=1200) {
  & $coopSend -InstanceDirectory $InstanceDirectory -Button $Button
  Start-Sleep -Milliseconds $DelayMs
}
# The first A after START is consumed on the way to the main menu (2026-10-13).
if ($FromTitle) { Press Start 6000; Press A 5000; Press A 5000 }
Press Down; Press A 3000
1..4 | ForEach-Object { Press Down }
Press A 3000
if ($Role -eq 'Host') { Press Down }
Press A 3000
