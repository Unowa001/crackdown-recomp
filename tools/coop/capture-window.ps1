param([Parameter(Mandatory=$true)][int]$GamePid,
      [Parameter(Mandatory=$true)][string]$OutputPath)
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class CoopWindow {
  [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window,out Rect rect);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window,int command);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window,IntPtr dc,uint flags);
}
'@
$coopGame=Get-Process -Id $GamePid -ErrorAction Stop
$coopHandle=$coopGame.MainWindowHandle
if ($coopHandle -eq [IntPtr]::Zero) { throw 'Game has no visible window' }
$coopRect=New-Object CoopWindow+Rect
[CoopWindow]::GetWindowRect($coopHandle,[ref]$coopRect) | Out-Null
$coopBitmap=New-Object System.Drawing.Bitmap ($coopRect.Right-$coopRect.Left),($coopRect.Bottom-$coopRect.Top)
$coopGraphics=[System.Drawing.Graphics]::FromImage($coopBitmap)
try {
  $coopDc=$coopGraphics.GetHdc()
  try {
    if (![CoopWindow]::PrintWindow($coopHandle,$coopDc,2)) { throw 'PrintWindow failed' }
  } finally { $coopGraphics.ReleaseHdc($coopDc) }
  $coopBitmap.Save($OutputPath,[System.Drawing.Imaging.ImageFormat]::Png)
} finally { $coopGraphics.Dispose(); $coopBitmap.Dispose() }
