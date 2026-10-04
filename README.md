# PowerOff — tiny Wise Auto Shutdown clone (C version)

Single-file native Win32 app in C. No UI framework, no runtime, no
dependencies beyond system DLLs.

Binary: **poweroff.exe ~67 KB** (Wise Auto Shutdown 2.1.1 ships ~3.17 MB;
the previous Rust implementation on the `rust` branch is ~255 KB).

## UI

Copies the Windows 11 Settings screen: rounded cards, WinUI-style toggle
switches, system accent color, Segoe UI Variable, light/dark theme
following. All interactive elements are genuine native controls
(combo boxes, edits, buttons, progress bar); only the container chrome
(card backgrounds, rows, glyphs, toggles) is painted by hand — Win32 has
no card or toggle controls to reuse, and WinUI/XAML can't be hosted from
C without the Windows App SDK.

Rows include monochrome GDI glyphs. The "Power options" row deep-links
to the real Settings app (`ms-settings:powersleep`) — third-party apps
cannot add pages *inside* Settings, so this is the closest legitimate
integration.

## Features (mirrors Wise Auto Shutdown)

Tasks: Shutdown, Restart, Power off, Log off, Lock, Sleep, Hibernate.
Triggers: Daily at HH:MM (e.g. sleep at 11 PM), Once at date/time,
Countdown, Idle (mouse+keyboard), Repeat every N minutes.
5-minute reminder (Run now / Delay 10 min / Cancel), tray icon with
menu, run at startup, remembers settings in `%APPDATA%\PowerOff\`.

## Install

Download `PowerOff-setup.exe` from the GitHub releases (or the latest
build's artifacts). It installs per user, without admin rights, to
`%LOCALAPPDATA%\Programs\PowerOff` and adds a Start menu shortcut.
Uninstall from Settings > Apps; that also removes PowerOff's scheduled
tasks, its startup entry and its settings.

## How it runs: Task Scheduler does the job

Closing the window while a task is active registers it as the
`PowerOff` task in Windows Task Scheduler (daily, weekly, countdown).
Windows fires it even if PowerOff isn't running or the PC restarted.
With a reminder on, the task starts 5 minutes early and shows it: OK
runs now, Cancel skips this time, no answer runs when the time is up.
Idle mode can't be expressed in Task Scheduler and stays in-process.

The **Tray icon** option decides what's left after closing:

- On: a tray icon only, restarted as a fresh process so it maps just
  what one icon needs (~1.5 MB private, ~0.3 MB working set).
- Off: nothing. PowerOff exits; open it from the Start menu to see or
  cancel the scheduled task.

## Your use case: sleep at 11 PM every day

```bat
PowerOff.exe --install-daily 23:00 --task sleep
```

Creates a Task Scheduler entry that warns for 60 seconds (Cancel
aborts, no answer continues) then sleeps the PC. Remove with `PowerOff.exe --uninstall`.
Or run the GUI (defaults to Sleep / Daily 23:00) and press Start task.

## CLI reference

```bat
PowerOff.exe [--tray] [--dark|--light]
PowerOff.exe --task sleep --daily 23:00
PowerOff.exe --task shutdown --once "2026-10-05 23:00"
PowerOff.exe --task sleep --in 3600 | --countdown 1:00:00
PowerOff.exe --task lock --idle 15 | --task sleep --every 60
PowerOff.exe --task sleep --fire-now [--warn-secs 60]
```

## Build

Requires MSVC Build Tools + Windows 10 SDK (any recent version), and
NSIS 3 for the installer:

```bat
build.bat                                  :: -> poweroff.exe, embeds poweroff.exe.manifest
makensis /DVERSION=1.0.0 installer\poweroff.nsi   :: -> PowerOff-setup.exe
```

CI (`.github/workflows/build.yml`) builds both on every push and pull
request and uploads them as artifacts; pushing a `v*` tag publishes them
as a GitHub release.

Flags: `/O1 /Os /GL /Gy /MD`, link `/OPT:REF /OPT:ICF`,
`/FILEALIGN:512`, manifest embedded with `mt.exe`.
`build.bat` uses the current MSVC environment when `cl.exe` is on PATH
and otherwise finds Visual Studio with `vswhere`. The exe is a windowed
program (no console flash), delay-loads its UI DLLs, and attaches to the
calling console for CLI use: UTF-16 to a console, UTF-8 to pipes/files.

## Notes

- Sleep/Hibernate use `SetSuspendState`; Shutdown/Restart/Log off use
  `ExitWindowsEx` with `SeShutdownPrivilege` enabled.
- Idle is measured with `GetLastInputInfo`.
- Only one copy runs: launching again brings up the running copy.
- Time entry is plain `HH:MM` / `YYYY-MM-DD` text, validated on Start.
