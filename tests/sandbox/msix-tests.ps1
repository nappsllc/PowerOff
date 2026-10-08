# PowerOff Store (MSIX) package tests, run inside Windows Sandbox (Run-Sandbox.ps1 -Suite msix).
# Registers the unsigned package layout with Developer Mode (sandbox only), then checks the
# packaged behavior: alias, tasks running the alias, Start with Windows as a logon task.
. "$PSScriptRoot\helpers.ps1"
$alias = "$env:LOCALAPPDATA\Microsoft\WindowsApps\poweroff.exe"

# Developer Mode lets an unsigned layout register (sandbox only; it is thrown away)
reg add 'HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock' /v AllowDevelopmentWithoutDevLicense /t REG_DWORD /d 1 /f | Out-Null
Copy-Item -Recurse "$T\layout" 'C:\PowerOffPkg'

Log "`n## Package`n"
Check 'MSIX layout registers' { Add-AppxPackage -Register 'C:\PowerOffPkg\AppxManifest.xml' -ErrorAction Stop; $p = Get-AppxPackage *PowerOff*; Note "$($p.PackageFullName)"; $p -ne $null }
$pkg = Get-AppxPackage *PowerOff*
Check 'Start menu entry' { $a = Get-StartApps | Where-Object Name -eq 'PowerNapps PowerOff'; Note "$($a.AppID)"; $a -ne $null }
Check 'app execution alias poweroff.exe exists' { Note $alias; Test-Path $alias }
Check 'alias runs the packaged exe (--help)' { $o = & $alias --help | Out-String; Note ($o.Split("`n")[0].Trim()); $o -match 'PowerOff' }

Log "`n## Packaged app`n"
Start-Process "shell:AppsFolder\$($pkg.PackageFamilyName)!PowerOff"
for ($i = 0; $i -lt 30 -and [H]::F('PowerOffWnd') -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 300 }
Start-Sleep -Seconds 1
$main = [H]::F('PowerOffWnd')
Check 'window opens from the Start menu entry' { $n = [H]::Uia($main, 30005); Note "name=$n"; $n -eq 'PowerNapps PowerOff' }
[H]::Capture($main, "$R\msix-main.png") | Out-Null
Check 'process runs from the package' { $p = Get-Process poweroff | Select-Object -First 1; Note $p.Path; $p.Path -like '*PowerOffPkg*' -or $p.Path -like '*WindowsApps*' }

Cmd 240   # Start
Check 'Start registers the task, which runs the alias' { $x = TaskXml 'PowerOff'; $c = $x.Task.Actions.Exec.Command; Note "command=$c args=$($x.Task.Actions.Exec.Arguments)"; (TaskExists 'PowerOff') -and $c -eq $alias }
Cmd 240   # Cancel
Check 'Cancel deletes the task' { -not (TaskExists 'PowerOff') }

Check 'Task Scheduler can start the app through the alias' {
    # a one-off task like the app's own, running the harmless --wake (display on)
    schtasks /create /tn 'PowerOff alias test' /tr "`"$alias`" --wake" /sc once /st 23:59 /f | Out-Null
    schtasks /run /tn 'PowerOff alias test' | Out-Null
    $r = $null
    for ($i = 0; $i -lt 30; $i++) { Start-Sleep -Seconds 1; $r = (Get-ScheduledTaskInfo -TaskName 'PowerOff alias test').LastTaskResult; if ($r -ne 267011 -and $r -ne 267009) { break } }
    schtasks /delete /tn 'PowerOff alias test' /f | Out-Null
    Note "LastTaskResult=$r"; $r -eq 0 }

$before = TaskExists 'PowerOff startup'
Cmd 232   # Start with Windows on
Check 'Start with Windows: a logon task runs the alias with --tray' {
    $x = TaskXml 'PowerOff startup'
    Note "before=$before user=$($x.Task.Triggers.LogonTrigger.UserId) command=$($x.Task.Actions.Exec.Command) args=$($x.Task.Actions.Exec.Arguments) limit=$($x.Task.Settings.ExecutionTimeLimit)"
    $x.Task.Triggers.LogonTrigger -and $x.Task.Actions.Exec.Command -eq $alias -and $x.Task.Actions.Exec.Arguments -eq '--tray' -and $x.Task.Settings.ExecutionTimeLimit -eq 'PT0S' }
Check 'the toggle reads On (state comes from the task)' { $s = [H]::Uia([H]::GetDlgItem($main, 232), 30086); Note "toggle=$s"; $s -eq '1' }
Check 'no Run value written' { $v = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name PowerOff -ErrorAction SilentlyContinue; $v -eq $null }
Cmd 232   # off
Check 'Start with Windows off deletes the logon task' { -not (TaskExists 'PowerOff startup') }

Check 'logon task starts the tray process' {
    Cmd 232; Get-Process poweroff | Stop-Process -Force; Start-Sleep -Seconds 1
    schtasks /run /tn 'PowerOff startup' | Out-Null
    for ($i = 0; $i -lt 20 -and [H]::F('PowerOffTray') -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 500 }
    $t = [H]::F('PowerOffTray') -ne [IntPtr]::Zero; $p = @(Get-Process poweroff -ErrorAction SilentlyContinue)
    Note "tray window=$t processes=$($p.Count)"; $t }

Log "`n**$($script:pass) passed, $($script:fail) failed** - finished $(Get-Date -Format 'HH:mm:ss')"
Set-Content "$R\DONE" (Get-Date -Format o)
Start-Sleep -Seconds 2
shutdown /s /t 0
