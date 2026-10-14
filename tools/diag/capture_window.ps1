# Save a PNG of a process's main window, even when other windows cover it.
# capture_window.ps1 -ProcessId <pid> -Out <file.png>
param([int]$ProcessId, [string]$Out)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class CapWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
"@
$h = (Get-Process -Id $ProcessId).MainWindowHandle
$r = New-Object CapWin+RECT
[void][CapWin]::GetWindowRect($h, [ref]$r)
$bmp = New-Object System.Drawing.Bitmap ([Math]::Max(1, $r.R - $r.L)), ([Math]::Max(1, $r.B - $r.T))
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc(); [void][CapWin]::PrintWindow($h, $dc, 2); $g.ReleaseHdc($dc)
$bmp.Save($Out); $g.Dispose(); $bmp.Dispose()
