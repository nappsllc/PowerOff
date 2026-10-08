# PowerOff end-to-end tests, run inside Windows Sandbox (start with Run-Sandbox.ps1).
# Results: C:\Test\results\results.md + screenshots. Ends by shutting the sandbox down
# with PowerOff itself.
. "$PSScriptRoot\helpers.ps1"

# ----------------------------------------------------------------------------- environment
Section 'Environment'
$os = Get-CimInstance Win32_OperatingSystem
Log "- Windows $($os.Version) $($os.Caption), user $env:USERNAME"
$pc = (powercfg /a | Out-String)
$avail = ($pc -split 'The following sleep states are not available')[0]
Log "- Sleep states available: $((($avail -split "`n") | Where-Object { $_ -match '^\s{4}\S' } | ForEach-Object { $_.Trim() }) -join ', ')"
Log ("- VC++ runtime present on this machine: " + (Test-Path "$env:windir\System32\vcruntime140.dll"))

# ----------------------------------------------------------------------------- installer
Section 'Installer'
Check 'silent install exits 0' { $p = Start-Process "$T\PowerOff-setup.exe" -ArgumentList '/S' -PassThru -Wait; Note "exit $($p.ExitCode)"; $p.ExitCode -eq 0 }
Check 'files installed' { (Test-Path $exe) -and (Test-Path "$env:LOCALAPPDATA\Programs\PowerOff\uninstall.exe") }
Check 'Start menu shortcut points at the exe' { $s = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk); Note $s.TargetPath; $s.TargetPath -eq $exe }
Check 'Settings > Apps entry' { $k = Get-ItemProperty $ukey; Note "$($k.DisplayName) $($k.DisplayVersion)"; $k.DisplayName -eq 'PowerOff' -and $k.UninstallString }

# ----------------------------------------------------------------------------- CLI
Section 'Command line'
Check '--help prints usage (piped, UTF-8)' { $o = & $exe --help | Out-String; Note ($o.Split("`n")[0].Trim()); $o -match 'PowerOff' -and $o -match '--fire-now' }
Check 'unknown task: exit 2' { $c = RunCli @('--task', 'bogus', '--in', '5'); Note "exit $c"; $c -eq 2 }
Check '--task without timing: exit 2' { $c = RunCli @('--task', 'sleep'); Note "exit $c"; $c -eq 2 }
Check 'starts without the VC++ redistributable' { $p = Start-Process $exe -ArgumentList '--help' -PassThru -Wait -WindowStyle Hidden; Note "exit $($p.ExitCode)"; $p.ExitCode -eq 0 }

# ----------------------------------------------------------------------------- window
Section 'Window'
SetIni 'task' 5; SetIni 'mode' 0; SetIni 'h' 23; SetIni 'm' 0; SetIni 'remind' 1; SetIni 'tray' 1; SetIni 'force' 0
$main = StartApp
Check 'window opens' { $main -ne [IntPtr]::Zero }
$dpi = [H]::GetDpiForWindow($main)
Check 'client width = 640 x DPI' { $w = [H]::ClientW($main); Note "dpi $dpi, client ${w}x$([H]::ClientH($main))"; $w -eq [math]::Round(640 * $dpi / 96) }
Snap 'PowerOffWnd' 'main'
Check 'single instance: second launch exits' { $p = Start-Process $exe -PassThru; Start-Sleep -Seconds 2; $p.Refresh(); $n = @(Get-Process poweroff).Count; Note "processes $n"; $p.HasExited -and $n -eq 1 }
Check 'Action dropdown opens with the selection on top' { Cmd 200; $f = [H]::F('PowerOffFly'); Snap 'PowerOffFly' 'dropdown'; [H]::SendMessage($f, $WM_KEYDOWN, [IntPtr]0x1B, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 300; $f -ne [IntPtr]::Zero -and [H]::F('PowerOffFly') -eq [IntPtr]::Zero }
Check 'time picker opens' { Cmd 210; $tp = [H]::F('PowerOffTimePick'); Snap 'PowerOffTimePick' 'timepicker'; [H]::SendMessage($tp, $WM_KEYDOWN, [IntPtr]0x1B, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 300; $tp -ne [IntPtr]::Zero }
Check 'window is named PowerNapps PowerOff' { $n = [H]::Uia($main, 30005); Note "name=$n"; $n -eq 'PowerNapps PowerOff' }
Check 'v3 layout: compact Options rows, links in the footer' {
    $k = $dpi / 96; $ch = [H]::ClientH($main)
    # daily, Lock (no Wake up): 56 + 28 + 96 + 4 + 36 + 3x(68+4) + 36 + 4x(48+4) + 16 + 20 + 28
    $want = [math]::Round((56 + 28 + 96 + 4 + 36 + 216 + 36 + 208 + 16 + 20 + 28) * $k)
    Note "client height $ch, expected ~$want"; [math]::Abs($ch - $want) -le 4 }
Check 'About dialog' { Cmd 251; $a = [H]::F('PowerOffAbout'); Snap 'PowerOffAbout' 'about'
    $n = [H]::Uia($a, 30005); $d = [H]::Uia($a, 30094)
    [H]::SendMessage($a, $WM_KEYDOWN, [IntPtr]0x09, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 200; $f = [H]::Uia($a, 30094)
    Note "name=$n | after Tab: $f | text has contact: $($d -match 'gig to offer' -and $d -match 'Los Angeles')"
    [H]::SendMessage($a, $WM_CLOSE, [IntPtr]0, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 300
    $n -eq 'About PowerNapps PowerOff' -and $f -eq 'Contact us button' -and [H]::F('PowerOffAbout') -eq [IntPtr]::Zero }

# ----------------------------------------------------------------------------- keyboard
Section 'Keyboard'
StopApp; $main = StartApp
Check 'Tab order: Start > Action > Schedule > Time > Remind' {
    $o = @(); for ($i = 0; $i -lt 5; $i++) { Key 0x09; $o += [H]::GetDlgCtrlID([H]::Focus($main)) }
    Note ($o -join ' > '); ($o -join ',') -eq '240,200,201,210,230' }
Check 'Space toggles Force apps to close (and back)' {
    Key 0x09; $a = Ini 'force'; Key 0x20; $b = Ini 'force'; Key 0x20; $c = Ini 'force'; Note "$a > $b > $c"; $a -eq '0' -and $b -eq '1' -and $c -eq '0' }
Check 'time picker by keyboard: Down + Enter changes the hour' {
    StopApp; $main = StartApp; for ($i = 0; $i -lt 10 -and [H]::GetDlgCtrlID([H]::Focus($main)) -ne 210; $i++) { Key 0x09 }   # focus Time
    function WaitWin($cls, $want) { for ($i = 0; $i -lt 40 -and (([H]::F($cls) -ne [IntPtr]::Zero) -ne $want); $i++) { Start-Sleep -Milliseconds 100 } }
    Key 0x20; WaitWin 'PowerOffTimePick' $true; Start-Sleep -Milliseconds 400; Key 0x28; Key 0x0D; WaitWin 'PowerOffTimePick' $false; $h1 = Ini 'h'
    Start-Sleep -Milliseconds 400; Key 0x20; WaitWin 'PowerOffTimePick' $true; Start-Sleep -Milliseconds 400; Key 0x26; Key 0x0D; WaitWin 'PowerOffTimePick' $false; $h2 = Ini 'h'
    Note "23 > $h1 > $h2"; $h1 -eq '12' -and $h2 -eq '23' }

# ----------------------------------------------------------------------------- accessibility
Section 'Screen reader (native UI Automation)'
StopApp; SetIni 'remind' 1; $main = StartApp
$names = @{ 240 = 'Start'; 200 = 'Action'; 201 = 'Schedule'; 210 = 'Time'; 230 = 'Remind me 1 minute before'; 250 = 'Power settings'; 251 = 'About' }
$types = @{ 240 = '50000'; 200 = '50003'; 201 = '50003'; 210 = '50003'; 230 = '50002'; 250 = '50005'; 251 = '50000' }
foreach ($id in 240, 200, 201, 210, 230, 250, 251) {
    $h = [H]::GetDlgItem($main, $id)
    Check "control $id is '$($names[$id])'" { $n = [H]::Uia($h, 30005); $t = [H]::Uia($h, 30003); $v = [H]::Uia($h, 30045); Note "name=$n type=$t value=$v"; $n -eq $names[$id] -and $t -eq $types[$id] }
}
Check 'Remind me 1 minute before reports On' { $s = [H]::Uia([H]::GetDlgItem($main, 230), 30086); Note "toggle=$s"; $s -eq '1' }

# ----------------------------------------------------------------------------- display scaling
Section 'Display scaling (simulated 150%)'
StopApp; $env:POWEROFF_TEST_DPI = '144'; $main = StartApp; Remove-Item Env:POWEROFF_TEST_DPI
Check 'client width = 960 at 150%' { $w = [H]::ClientW($main); Note "client ${w}x$([H]::ClientH($main))"; $w -eq 960 }
Snap 'PowerOffWnd' 'main-150'
StopApp

# ----------------------------------------------------------------------------- Task Scheduler is the schedule
Section 'Task Scheduler is the schedule'
schtasks /delete /tn PowerOff /f 2>$null | Out-Null
SetIni 'task' 5; SetIni 'mode' 0; SetIni 'h' 23; SetIni 'm' 0; SetIni 'tray' 1; SetIni 'force' 0; SetIni 'remind' 1
$main = StartApp
Cmd 240
Check 'Start registers the task right away' { $b = [H]::Uia([H]::GetDlgItem($main, 240), 30005); Note "button=$b"; $b -eq 'Cancel' -and (TaskExists 'PowerOff') }
Check 'daily trigger, 1 min early, runs on battery' { $x = TaskXml 'PowerOff'; $sb = $x.Task.Triggers.CalendarTrigger.StartBoundary; Note "start=$sb battery=$($x.Task.Settings.DisallowStartIfOnBatteries) args=$($x.Task.Actions.Exec.Arguments)"; $sb -match 'T22:59:00' -and $x.Task.Settings.DisallowStartIfOnBatteries -eq 'false' }
Check 'an edit while scheduled updates the task (Force on)' {
    for ($i = 0; $i -lt 6; $i++) { Key 0x09 }   # Start > Action > Schedule > Time > Remind > Force
    Key 0x20; $a = (TaskXml 'PowerOff').Task.Actions.Exec.Arguments; Note "args=$a"; Key 0x20
    $a -match '--force' -and (TaskXml 'PowerOff').Task.Actions.Exec.Arguments -notmatch '--force' }
[H]::PostMessage($main, $WM_CLOSE, [IntPtr]0, [IntPtr]0) | Out-Null; Start-Sleep -Seconds 4
Check 'closing keeps the task; only a small tray process is left' { $p = @(Get-Process poweroff); $pm = [int]($p[0].PrivateMemorySize64 / 1KB); Note "processes $($p.Count), private $pm KB"; (TaskExists 'PowerOff') -and $p.Count -eq 1 -and [H]::F('PowerOffWnd') -eq [IntPtr]::Zero -and $pm -lt 4096 }
StopApp
schtasks /change /tn PowerOff /st 21:29 2>$null | Out-Null
$main = StartApp
Check 'a time changed in Task Scheduler shows in the app' { $v = [H]::Uia([H]::GetDlgItem($main, 210), 30045); $b = [H]::Uia([H]::GetDlgItem($main, 240), 30005); Note "time=$v button=$b"; $v -eq '9:30 PM' -and $b -eq 'Cancel' }
schtasks /delete /tn PowerOff /f 2>$null | Out-Null
[H]::SendMessage($main, 0x0006, [IntPtr]1, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 800   # WM_ACTIVATE
Check 'a task deleted in Task Scheduler: back to Not scheduled' { $b = [H]::Uia([H]::GetDlgItem($main, 240), 30005); Note "button=$b"; $b -eq 'Start' }
Cmd 240
Check 'Cancel deletes the task' { $ok1 = TaskExists 'PowerOff'; Cmd 240; $ok2 = -not (TaskExists 'PowerOff'); Note "after Start=$ok1 after Cancel=$ok2"; $ok1 -and $ok2 }
StopApp

# ----------------------------------------------------------------------------- wake
Section 'Wake up'
SetIni 'task' 2; SetIni 'wake' 0
$main = StartApp
Cmd 234
Check 'Wake up on: wake task with WakeToRun' { $x = TaskXml 'PowerOff wake'; Note "WakeToRun=$($x.Task.Settings.WakeToRun) args=$($x.Task.Actions.Exec.Arguments)"; $x.Task.Settings.WakeToRun -eq 'true' }
Check '--wake runs' { $c = RunCli @('--wake'); Note "exit $c"; $c -eq 0 }
Cmd 234
Check 'Wake up off: task deleted' { -not (TaskExists 'PowerOff wake') }
Cmd 232
Check 'Start with Windows writes the Run key' { $v = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run').PowerOff; Note $v; $v -match '--tray' }
StopApp

# ----------------------------------------------------------------------------- reminder card
Section 'Reminder card'
Check 'card shows and counts down; Esc stops without running' {
    $p = Start-Process $exe -ArgumentList '--task', 'display', '--fire-now', '--warn-secs', '30' -PassThru
    for ($i = 0; $i -lt 20 -and [H]::F('PowerOffReminder') -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 250 }
    Start-Sleep -Seconds 2; Snap 'PowerOffReminder' 'reminder'
    $rm = [H]::F('PowerOffReminder'); [H]::SendMessage($rm, $WM_KEYDOWN, [IntPtr]0x1B, [IntPtr]0) | Out-Null
    Start-Sleep -Seconds 1; $p.Refresh(); Note "shown=$($rm -ne [IntPtr]::Zero) exited=$($p.HasExited)"; $rm -ne [IntPtr]::Zero -and $p.HasExited -and $p.ExitCode -eq 0 }
Check 'Snooze hides the card and waits' {
    $p = Start-Process $exe -ArgumentList '--task', 'display', '--fire-now', '--warn-secs', '30' -PassThru
    for ($i = 0; $i -lt 20 -and [H]::F('PowerOffReminder') -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 250 }
    $rm = [H]::F('PowerOffReminder'); [H]::SendMessage($rm, 0x0202, [IntPtr]0, [H]::MK(240, 152)) | Out-Null
    Start-Sleep -Seconds 2; $p.Refresh(); $gone = [H]::F('PowerOffReminder') -eq [IntPtr]::Zero; $alive = -not $p.HasExited
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Note "card gone=$gone waiting=$alive"; $gone -and $alive }

# ----------------------------------------------------------------------------- a time closer than the reminder
Section 'Daily time picked less than a minute ahead still runs today'
schtasks /delete /tn PowerOff /f 2>$null | Out-Null
while ((Get-Date).Second -lt 15 -or (Get-Date).Second -gt 40) { Start-Sleep -Milliseconds 500 }   # 20-45 s before the next minute
$at = (Get-Date).AddMinutes(1)
SetIni 'task' 6; SetIni 'mode' 0; SetIni 'h' $at.Hour; SetIni 'm' $at.Minute; SetIni 'remind' 1; SetIni 'tray' 0; SetIni 'force' 0
$main = StartApp; Cmd 240; $t0 = Get-Date
Check 'the task gets a catch-up trigger and --at' { $x = TaskXml 'PowerOff'; $a = $x.Task.Actions.Exec.Arguments; Note "args=$a calendar=$([bool]$x.Task.Triggers.CalendarTrigger) catchup=$([bool]$x.Task.Triggers.TimeTrigger)"; $x.Task.Triggers.CalendarTrigger -and $x.Task.Triggers.TimeTrigger -and $a -match ('--at {0:D2}:{1:D2}' -f $at.Hour, $at.Minute) }
Check 'the app still shows the daily schedule' { $v = [H]::Uia([H]::GetDlgItem($main, 201), 30045); $b = [H]::Uia([H]::GetDlgItem($main, 240), 30005); Note "schedule=$v button=$b"; $v -eq 'At a specific time' -and $b -eq 'Cancel' }
$shown = $null; while (((Get-Date) - $t0).TotalSeconds -lt 15 -and -not $shown) { if ([H]::F('PowerOffReminder') -ne [IntPtr]::Zero) { $shown = Get-Date } else { Start-Sleep -Milliseconds 300 } }
Check 'the reminder appears right away' { Note "after $(if ($shown) { [int]($shown - $t0).TotalSeconds } else { 'never' }) s"; $shown -ne $null }
while ([H]::F('PowerOffReminder') -ne [IntPtr]::Zero -and ((Get-Date) - $t0).TotalSeconds -lt 90) { Start-Sleep -Milliseconds 300 }
$gone = Get-Date; Start-Sleep -Seconds 3
Check 'the action runs at the picked time, today' {
    $i = Get-ScheduledTaskInfo -TaskName PowerOff; $off = [int]($gone - [datetime]::new($at.Year, $at.Month, $at.Day, $at.Hour, $at.Minute, 0)).TotalSeconds
    Note "card closed $off s from $('{0:HH:mm}' -f $at), LastTaskResult=$($i.LastTaskResult), next run $($i.NextRunTime)"; [math]::Abs($off) -le 5 -and $i.LastTaskResult -eq 0 }
Cmd 240
Check 'Cancel removes it' { -not (TaskExists 'PowerOff') }
StopApp

# ----------------------------------------------------------------------------- end to end: Task Scheduler fires a countdown
Section 'End to end: Windows runs a 5-minute countdown (Turn off display, reminder on)'
function CountdownToCard {
    schtasks /delete /tn PowerOff /f 2>$null | Out-Null
    SetIni 'task' 6; SetIni 'mode' 2; SetIni 'cd' 5; SetIni 'remind' 1; SetIni 'tray' 0; SetIni 'snooze_until' 0
    $m = StartApp; Cmd 240; $script:t0 = Get-Date
    [H]::PostMessage($m, $WM_CLOSE, [IntPtr]0, [IntPtr]0) | Out-Null; Start-Sleep -Seconds 4
    $script:shown = $null
    while (((Get-Date) - $script:t0).TotalSeconds -lt 330 -and -not $script:shown) { if ([H]::F('PowerOffReminder') -ne [IntPtr]::Zero) { $script:shown = Get-Date } else { Start-Sleep -Seconds 1 } }
}
CountdownToCard
Check 'tray off: closing leaves no PowerOff process' { $n = @(Get-Process poweroff -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -ne 'PowerOff reminder' }).Count; Note "card shown=$($script:shown -ne $null)"; $script:shown -ne $null }
Check 'reminder card appears 4 min after Start' { if ($script:shown) { Start-Sleep -Seconds 3; Snap 'PowerOffReminder' 'reminder-scheduled'; $s = [int]($script:shown - $script:t0).TotalSeconds; Note "after $s s"; $s -ge 225 -and $s -le 265 } else { $false } }
while (((Get-Date) - $script:t0).TotalSeconds -lt 400 -and [H]::F('PowerOffReminder') -ne [IntPtr]::Zero) { Start-Sleep -Seconds 1 }
$gone = Get-Date; Start-Sleep -Seconds 5
Check 'countdown ran out at 5 min and the action ran (task result 0)' {
    $i = Get-ScheduledTaskInfo -TaskName PowerOff -ErrorAction SilentlyContinue; $s = [int]($gone - $script:t0).TotalSeconds
    Note "card closed after $s s, LastTaskResult=$($i.LastTaskResult)"; $s -ge 285 -and $s -le 330 -and $i.LastTaskResult -eq 0 }
$main = StartApp
Check 'after the run the app shows Not scheduled' { $b = [H]::Uia([H]::GetDlgItem($main, 240), 30005); Note "button=$b"; $b -eq 'Start' }
StopApp

Section 'End to end: Snooze on the scheduled card shows in the app'
CountdownToCard
Check 'Snooze on the card from Task Scheduler' {
    $rm = [H]::F('PowerOffReminder'); [H]::SendMessage($rm, 0x0202, [IntPtr]0, [H]::MK(240, 152)) | Out-Null; Start-Sleep -Seconds 2
    $su = Ini 'snooze_until'; Note "card shown=$($script:shown -ne $null) snooze_until=$su"; $script:shown -ne $null -and [int64]$su -gt 0 }
$main = StartApp
Check 'the window shows the snoozed time (about 10 min left)' {
    $d = [H]::Uia([H]::GetDlgItem($main, 240), 30094)   # LegacyIAccessible.Description of Start = hero text
    if (-not $d) { $d = [H]::Uia([H]::GetDlgItem($main, 240), 30013) }
    Note "hero='$d'"; $d -match '(9|10):\d\d left' }
StopApp
Get-Process poweroff -ErrorAction SilentlyContinue | Stop-Process -Force   # the snoozed --fire-now process
schtasks /delete /tn PowerOff /f 2>$null | Out-Null

# ----------------------------------------------------------------------------- unsupported actions fail cleanly
Section 'Unsupported actions'
if ($avail -notmatch 'Hibernate') { Check 'Hibernate unavailable: exit 3, no hang' { $c = RunCli @('--task', 'hibernate', '--fire-now'); Note "exit $c"; $c -eq 3 } } else { Log '- SKIP Hibernate is available here' }
if ($avail -notmatch 'Standby') { Check 'Sleep unavailable: exit 3, no hang' { $c = RunCli @('--task', 'sleep', '--fire-now'); Note "exit $c"; $c -eq 3 } } else { Log '- SKIP Sleep is available here' }

# ----------------------------------------------------------------------------- lock (session stays locked afterwards)
Section 'Lock (reminder runs out)'
Check '--warn-secs 5 then Lock: card, then the session locks' {
    $p = Start-Process $exe -ArgumentList '--task', 'lock', '--fire-now', '--warn-secs', '5' -PassThru
    Start-Sleep -Seconds 2; $card = [H]::F('PowerOffReminder') -ne [IntPtr]::Zero
    Start-Sleep -Seconds 7; $p.Refresh(); $locked = [bool](Get-Process LogonUI -ErrorAction SilentlyContinue)
    Note "card=$card exited=$($p.HasExited) exit=$($p.ExitCode) LogonUI=$locked"; $card -and $p.HasExited -and $p.ExitCode -eq 0 -and $locked }

# ----------------------------------------------------------------------------- uninstall
Section 'Uninstaller'
& $exe --install-daily 23:00 --task sleep | Out-Null
Check 'before: PowerOff daily task + Run key exist' { (TaskExists 'PowerOff daily') -and (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run').PowerOff }
Check 'silent uninstall exits 0' { $p = Start-Process "$env:LOCALAPPDATA\Programs\PowerOff\uninstall.exe" -ArgumentList '/S' -PassThru -Wait; Start-Sleep -Seconds 3; Note "exit $($p.ExitCode)"; $p.ExitCode -eq 0 }
Check 'files, shortcut and Apps entry removed' { -not (Test-Path $exe) -and -not (Test-Path $lnk) -and -not (Test-Path $ukey) }
Check 'scheduled tasks removed' { -not (TaskExists 'PowerOff') -and -not (TaskExists 'PowerOff daily') -and -not (TaskExists 'PowerOff wake') }
Check 'Run key and settings removed' { -not (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run').PowerOff -and -not (Test-Path (Split-Path $ini)) }

# ----------------------------------------------------------------------------- done; shut down with PowerOff
Log "`n**$($script:pass) passed, $($script:fail) failed** - finished $(Get-Date -Format 'HH:mm:ss')"
Log "`nLast step: PowerOff shuts the sandbox down (``--task shutdown --fire-now --force``)."
Set-Content "$R\DONE" (Get-Date -Format o)
Start-Sleep -Seconds 2
$p = Start-Process "$T\poweroff.exe" -ArgumentList '--task','shutdown','--fire-now','--force' -PassThru -Wait
Add-Content "$R\DONE" "shutdown exit code: $($p.ExitCode) at $(Get-Date -Format HH:mm:ss)"
