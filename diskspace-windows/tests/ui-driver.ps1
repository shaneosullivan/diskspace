# Test helper: locate the app's main window by class, screenshot it, click into it.
if (-not ("W" -as [type])) {
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class W {
  public delegate bool Proc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(Proc p, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int ht, uint flags);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
}
"@
}
function Get-AppWindow {
  $script:hit = [IntPtr]::Zero
  $cb = [W+Proc]{ param($h,$l)
    $c = New-Object Text.StringBuilder 256
    [W]::GetClassNameW($h, $c, 256) | Out-Null
    if ($c.ToString() -eq "DiskSpaceMainWindow") { $script:hit = $h; return $false }
    return $true
  }
  [W]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
  if ($script:hit -eq [IntPtr]::Zero) { throw "main window not found" }
  return $script:hit
}
function Focus-App {
  # SetForegroundWindow alone is refused when the caller is not the foreground
  # process, which a test script never is. Pinning the window topmost for the
  # duration of the capture is the reliable way to get it in front.
  # Windows refuses SetForegroundWindow to a process that is not already in
  # front. Attaching to the current foreground thread's input queue is the
  # documented way around it, and is exactly what a test harness needs.
  $h = Get-AppWindow
  [W]::ShowWindow($h, 9) | Out-Null
  $foreground = [W]::GetForegroundWindow()
  $theirs = [W]::GetWindowThreadProcessId($foreground, [IntPtr]::Zero)
  $mine = [W]::GetCurrentThreadId()
  [W]::AttachThreadInput($mine, $theirs, $true) | Out-Null
  [W]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0003) | Out-Null   # HWND_TOPMOST
  [W]::BringWindowToTop($h) | Out-Null
  [W]::SetForegroundWindow($h) | Out-Null
  [W]::AttachThreadInput($mine, $theirs, $false) | Out-Null
  Start-Sleep -Milliseconds 700
  return $h
}

function Unfocus-App {
  $h = Get-AppWindow
  [W]::SetWindowPos($h, [IntPtr](-2), 0, 0, 0, 0, 0x0003) | Out-Null   # HWND_NOTOPMOST
}
function Shot-App([string]$Out) {
  Add-Type -AssemblyName System.Drawing
  $h = Focus-App
  $r = New-Object W+RECT
  [W]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.R - $r.L; $ht = $r.B - $r.T
  $bmp = New-Object System.Drawing.Bitmap($w, $ht)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
  $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
  Unfocus-App
  "saved $Out ($w x $ht)"
}
function Click-App([int]$X, [int]$Y, [int]$Clicks = 1) {
  $h = Focus-App
  $r = New-Object W+RECT
  [W]::GetWindowRect($h, [ref]$r) | Out-Null
  [W]::SetCursorPos($r.L + $X, $r.T + $Y) | Out-Null
  Start-Sleep -Milliseconds 250
  for ($i = 0; $i -lt $Clicks; $i++) {
    [W]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
    [W]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 60
  }
  Start-Sleep -Milliseconds 700
}
function Menu-App([int]$Command) {
  # WM_COMMAND straight to the window, which is what a menu click posts anyway.
  $h = Get-AppWindow
  [W]::PostMessageW($h, 0x0111, [IntPtr]$Command, [IntPtr]::Zero) | Out-Null
  Start-Sleep -Milliseconds 800
}

# Captures a window by class name straight from the window itself rather than
# from the screen, so a window that is behind another still comes out right.
function Find-Window([string]$Class) {
  $script:hit = [IntPtr]::Zero
  $cb = [W+Proc]{ param($h,$l)
    $c = New-Object Text.StringBuilder 256
    [W]::GetClassNameW($h, $c, 256) | Out-Null
    if ($c.ToString() -eq $Class) { $script:hit = $h; return $false }
    return $true
  }
  [W]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
  return $script:hit
}

function Shot-Window([string]$Class, [string]$Out) {
  Add-Type -AssemblyName System.Drawing
  $h = Find-Window $Class
  if ($h -eq [IntPtr]::Zero) { throw "no window with class $Class" }
  [W]::ShowWindow($h, 5) | Out-Null
  [W]::SetForegroundWindow($h) | Out-Null
  Start-Sleep -Milliseconds 500
  $r = New-Object W+RECT
  [W]::GetWindowRect($h, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap(($r.R - $r.L), ($r.B - $r.T))
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $dc = $g.GetHdc()
  # 2 is PW_RENDERFULLCONTENT, which is what makes this work for a window that
  # composes its own content rather than one drawn by the classic path.
  [W]::PrintWindow($h, $dc, 2) | Out-Null
  $g.ReleaseHdc($dc)
  $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
  "saved $Out ($($r.R - $r.L) x $($r.B - $r.T))"
}

# Clicks at client coordinates of a named window by posting the message the
# mouse would have produced. Independent of z-order, which makes it the reliable
# way to drive a window that is not in front.
function Click-WindowClient([string]$Class, [int]$X, [int]$Y) {
  $h = Find-Window $Class
  if ($h -eq [IntPtr]::Zero) { throw "no window with class $Class" }
  $l = [IntPtr](($Y -shl 16) -bor ($X -band 0xFFFF))
  [W]::PostMessageW($h, 0x0201, [IntPtr]1, $l) | Out-Null   # WM_LBUTTONDOWN
  [W]::PostMessageW($h, 0x0202, [IntPtr]0, $l) | Out-Null   # WM_LBUTTONUP
  Start-Sleep -Milliseconds 500
}
