param([int]$TimeoutSeconds = 30)
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$output = Join-Path $repo 'out/ap-discovery'
$build = Join-Path $repo 'crackdown/out/build/win-amd64-release'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$image = Join-Path $output 'guest-image.bin'
if (Test-Path -LiteralPath $image) { throw 'Discovery image already exists; inspect it before capturing another' }
$replay = Join-Path $output 'idle.rec'
'S 0 0.0 0000 0 0 0 0 0 0' | Set-Content -LiteralPath $replay -Encoding ASCII
$arguments = @(
    ('--game_data_root="' + (Join-Path $repo 'game') + '"'),
    ('--user_data_root="' + (Join-Path $output 'user-data') + '"'),
    ('--cache_root="' + (Join-Path $output 'cache') + '"'),
    ('--dump_image="' + $image + '"'),
    ('--input_replay="' + $replay + '"'),
    '--input_replay_tail_s=1','--fullscreen=false','--window_width=640',
    '--window_height=360','--audio_mute=true','--vsync=true'
)
$process = Start-Process -FilePath (Join-Path $build 'crackdown.exe') -ArgumentList $arguments -WorkingDirectory $build -WindowStyle Hidden -PassThru
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while (-not $process.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
if (-not $process.HasExited) {
    # Only the child created by this script is stopped; other game processes are untouched.
    Stop-Process -Id $process.Id
    $process.WaitForExit()
}
if (-not (Test-Path -LiteralPath $image -PathType Leaf)) { throw 'Game did not produce the discovery image' }
Get-Item -LiteralPath $image | Select-Object FullName,Length
Write-Output "Image SHA256: $((Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash)"
Write-Output 'Boot used isolated user-data/cache and idle input; no existing player save was loaded.'
