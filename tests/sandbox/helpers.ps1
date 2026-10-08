# Shared helpers for the sandbox scripts (Windows PowerShell 5.1 inside Windows Sandbox):
# results log, Check, window / UI Automation access, app start and stop, settings, tasks.
# Dot-sourced by run-tests.ps1, msix-tests.ps1 and screenshots.ps1.
$ErrorActionPreference = 'Continue'
$T = 'C:\Test'; $R = "$T\results"
New-Item -ItemType Directory -Force $R | Out-Null
$log = "$R\results.md"
Set-Content $log "# PowerOff sandbox test run`n`nStarted $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')`n"
function Log($s) { Add-Content $log $s }
$script:pass = 0; $script:fail = 0
function Note($s) { $script:d += " $s" }
function Check($name, [scriptblock]$b) {
    $script:d = ''
    $ok = $false
    try { $ok = [bool](& $b | Select-Object -Last 1) } catch { $script:d += " EXCEPTION: $($_.Exception.Message)" }
    if ($ok) { $script:pass++; Log "- PASS $name -$($script:d)" } else { $script:fail++; Log "- **FAIL** $name -$($script:d)" }
}
function Section($s) { Log "`n## $s`n" }

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Drawing;
[ComImport, Guid("d22108aa-8ac5-49a5-837b-37bbb3d7591e"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IUIAElement { void SetFocus(); void GetRuntimeId(); void FindFirst(); void FindAll(); void FindFirstBuildCache(); void FindAllBuildCache(); void BuildUpdatedCache();
  [return: MarshalAs(UnmanagedType.Struct)] object GetCurrentPropertyValue(int propertyId); }
[ComImport, Guid("30cbe57d-d9d0-452a-ab13-7ac5ac4825ee"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IUIA { void CompareElements(); void CompareRuntimeIds(); void GetRootElement(); IUIAElement ElementFromHandle(IntPtr hwnd); }
public static class H {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, IntPtr t);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll")] static extern bool GetGUIThreadInfo(uint tid, ref GTI gi);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetDpiForWindow(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] struct GTI { public int cb; public int fl; public IntPtr a, f, c, m, mv, ca; public RECT rc; }
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow(); public static IntPtr F(string c) { return FindWindowW(c, IntPtr.Zero); }
  public static IntPtr MK(int lo, int hi) { return (IntPtr)(int)((ushort)lo | ((uint)(ushort)hi << 16)); }
  public static int ClientW(IntPtr h) { RECT r; GetClientRect(h, out r); return r.R; }
  public static int ClientH(IntPtr h) { RECT r; GetClientRect(h, out r); return r.B; }
  public static IntPtr Focus(IntPtr main) { uint p; uint t = GetWindowThreadProcessId(main, out p); var g = new GTI(); g.cb = Marshal.SizeOf(g); GetGUIThreadInfo(t, ref g); return g.f; }
  public static bool Capture(IntPtr h, string path) {
    RECT r; if (!GetWindowRect(h, out r)) return false; int w = r.R - r.L, hh = r.B - r.T; if (w <= 0 || hh <= 0) return false;
    using (var b = new Bitmap(w, hh)) { using (var g = Graphics.FromImage(b)) { IntPtr dc = g.GetHdc(); PrintWindow(h, dc, 2); g.ReleaseHdc(dc); } b.Save(path); }
    return true; }
  static IUIA uia;
  public static string Uia(IntPtr h, int prop) { if (uia == null) uia = (IUIA)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("ff48dba4-60ef-4201-aa87-54103eef594e")));
    try { object o = uia.ElementFromHandle(h).GetCurrentPropertyValue(prop); return o == null ? "" : o.ToString(); } catch { return "ERR"; } }
}
'@

$exe = "$env:LOCALAPPDATA\Programs\PowerOff\poweroff.exe"
$ini = "$env:APPDATA\PowerOff\poweroff.ini"
$lnk = "$env:APPDATA\Microsoft\Windows\Start Menu\Programs\PowerOff.lnk"
$ukey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\PowerOff'
$WM_COMMAND = 0x0111; $WM_CLOSE = 0x0010; $WM_KEYDOWN = 0x0100; $WM_KEYUP = 0x0101

function StopApp { Get-Process poweroff -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Milliseconds 700 }
function StartApp([string[]]$a = @()) {
    if ($a.Count) { Start-Process $exe -ArgumentList $a | Out-Null } else { Start-Process $exe | Out-Null }
    for ($i = 0; $i -lt 30 -and [H]::F('PowerOffWnd') -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 300 }
    Start-Sleep -Milliseconds 800
    $m = [H]::F('PowerOffWnd')
    # keys only after the window really is in front: a late activation closes an open picker
    for ($i = 0; $m -ne [IntPtr]::Zero -and $i -lt 30 -and [H]::GetForegroundWindow() -ne $m; $i++) { [H]::SetForegroundWindow($m) | Out-Null; Start-Sleep -Milliseconds 200 }
    $m
}
function Ini($k) { if (-not (Test-Path $ini)) { return $null }; $l = Get-Content $ini | Where-Object { $_ -match "^$k=" } | Select-Object -First 1; if ($l) { $l.Substring($k.Length + 1) } }
function SetIni($k, $v) {
    New-Item -ItemType Directory -Force (Split-Path $ini) | Out-Null
    if (-not (Test-Path $ini)) { Set-Content $ini '[PowerOff]' }
    $c = @(Get-Content $ini)
    if ($c -match "^$k=") { $c = $c -replace "^$k=.*", "$k=$v" } else { $c += "$k=$v" }
    Set-Content $ini $c
}
function Cmd($id) { $m = [H]::F('PowerOffWnd'); [H]::PostMessage($m, $WM_COMMAND, [IntPtr]$id, [H]::GetDlgItem($m, $id)) | Out-Null; Start-Sleep -Milliseconds 700 }
function Key($vk) { $m = [H]::F('PowerOffWnd'); $t = [H]::Focus($m); if ($t -eq [IntPtr]::Zero) { $t = $m }
    [H]::PostMessage($t, $WM_KEYDOWN, [IntPtr]$vk, [IntPtr]1) | Out-Null; [H]::PostMessage($t, $WM_KEYUP, [IntPtr]$vk, [IntPtr]0xC0000001) | Out-Null; Start-Sleep -Milliseconds 350 }
function TaskExists($n) { schtasks /query /tn $n 2>$null | Out-Null; $LASTEXITCODE -eq 0 }
function TaskXml($n) { [xml](schtasks /query /tn $n /xml 2>$null | Out-String) }
function Snap($cls, $name) { $h = [H]::F($cls); if ($h -ne [IntPtr]::Zero) { [H]::Capture($h, "$R\$name.png") | Out-Null } }
function RunCli([string[]]$a) { $p = Start-Process $exe -ArgumentList $a -PassThru -Wait -WindowStyle Hidden; $p.ExitCode }
