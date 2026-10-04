# PowerOff — tiny Wise Auto Shutdown clone (C version)

Single-file native Win32 app in C. No UI framework, no runtime, no
dependencies beyond system DLLs.

Binary: **poweroff.exe ~46 KB** (Wise Auto Shutdown 2.1.1 ships ~3.17 MB;
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

## Your use case: sleep at 11 PM every day

```bat
PowerOff.exe --install-daily 23:00 --task sleep
```

Creates a Task Scheduler entry that warns 60 seconds (Cancel aborts)
then sleeps the PC. Remove with `PowerOff.exe --uninstall`.
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

Requires MSVC Build Tools + Windows 10 SDK (any recent version):

```bat
build.bat     :: -> poweroff.exe, embeds poweroff.exe.manifest
```

Flags: `/O1 /Os /GL /Gy /MD`, link `/OPT:REF /OPT:ICF`,
`/FILEALIGN:512`, manifest embedded with `mt.exe`.
Edit the `vcvars64.bat` / SDK paths at the top of `build.bat` if your
Visual Studio lives elsewhere. Unicode note: `--help` etc. print real
Unicode, so `wmain` sets stdout/stderr to UTF-16 mode up front.

## Notes

- Sleep/Hibernate use `SetSuspendState`; Shutdown/Restart/Log off use
  `ExitWindowsEx` with `SeShutdownPrivilege` enabled.
- Idle is measured with `GetLastInputInfo`.
- Closing the window while a task runs hides to the tray instead of
  exiting.
- Time entry is plain `HH:MM` / `YYYY-MM-DD` text, validated on Start.
