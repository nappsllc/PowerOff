# README screenshots (light and dark), run inside Windows Sandbox (Run-Sandbox.ps1 -Suite screenshots).
# Installs, schedules Sleep at 11 PM with Wake up at 7:00, and captures the window in each theme.
# The capture includes the window's invisible 8 px resize border: crop it (left, right, bottom).
. "$PSScriptRoot\helpers.ps1"

Start-Process "$T\PowerOff-setup.exe" -ArgumentList '/S' -Wait
# the README example: Sleep at 11 PM every day, reminder on, Wake up at 7:00
SetIni 'task' 2; SetIni 'mode' 0; SetIni 'h' 23; SetIni 'm' 0; SetIni 'remind' 1; SetIni 'force' 0
SetIni 'tray' 1; SetIni 'wake' 1; SetIni 'wake_h' 7; SetIni 'wake_m' 0
$theme = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
foreach ($mode in 'light', 'dark') {
    Set-ItemProperty $theme AppsUseLightTheme ([int]($mode -eq 'light'))
    Start-Sleep -Seconds 2
    StopApp; $main = StartApp
    if (-not (TaskExists 'PowerOff')) { Cmd 240 }   # Start: scheduled
    StopApp; $main = StartApp                       # fresh window: no hover, no focus ring
    [H]::PostMessage($main, 0x0200, [IntPtr]0, [H]::MK(5, 5)) | Out-Null
    Start-Sleep -Seconds 2
    try { $ok = [H]::Capture([H]::F('PowerOffWnd'), "$R\screenshot-$mode.png"); Log "- main=$main found=$([H]::F('PowerOffWnd')) ok=$ok task=$(TaskExists 'PowerOff')" } catch { Log "- error: $($_.Exception.Message)" }
    Log "- captured $mode"
}
Set-Content "$R\DONE" 'done'; Start-Sleep 2; shutdown /s /t 0
