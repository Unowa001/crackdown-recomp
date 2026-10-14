param([Parameter(Mandatory=$true)][string]$InstanceDirectory,
      [ValidateSet('Start','A','B','Up','Down','Left','Right')][string]$Button='A')
$coopButtons=@{Start=@('0010','5814'); A=@('1000','5800'); B=@('2000','5801');
  Up=@('0001','5810'); Down=@('0002','5811'); Left=@('0004','5812'); Right=@('0008','5813')}
$coopPad,$coopKey=$coopButtons[$Button]
$coopLines=@("S 0 0 $coopPad 0 0 0 0 0 0","K 0 0 $coopKey 0000 0001 0 0",
  'S 0 150 0000 0 0 0 0 0 0',"K 0 150 $coopKey 0000 0002 0 0")
$coopCommand=Join-Path $InstanceDirectory 'commands.rec'
$coopTemp=Join-Path $InstanceDirectory 'commands.rec.tmp'
$coopLines | Set-Content -LiteralPath $coopTemp
# Atomic replacement ensures the reader never sees a partial input recording.
Move-Item -LiteralPath $coopTemp -Destination $coopCommand -Force
