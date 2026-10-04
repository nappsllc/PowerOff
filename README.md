# PowerOff — tiny Wise Auto Shutdown clone

Native Win32 app written in Rust with **no UI framework** (no egui/iced/Tauri/Qt —
only raw `CreateWindowExW`, standard common controls, and the `windows-sys` OS
bindings crate).

Release binary: **~255 KB** (Wise Auto Shutdown 2.1.1 ships ~3.17 MB).

## UI

100% native Win32 controls, zero custom painting — the same elements as the
Windows Settings screens: group boxes, list box, combo box, edit fields,
checkboxes, push buttons, statics, and a real progress-bar control, in
Segoe UI (Variable where present). Follows the system app theme live,
dark title bar included. No UI framework, no egui, no XAML.

`poweroff.exe.manifest` next to the exe enables comctl32 v6 visual styles —
copy it alongside the binary when deploying
(`target\release\poweroff.exe.manifest` is refreshed after each local build).

## Dark mode

Follows the system app theme (`AppsUseLightTheme`) live, using the design
system's own dark tokens — dark title bar included. Native controls
(checkboxes, edits, list) go dark via uxtheme (best-effort, needs Win10 1809+).
Override with `PowerOff.exe --dark` / `--light`. Zero new dependencies:
uxtheme/dwmapi are system DLLs, so no bloat (~+1 KB).

## Features (mirrors Wise Auto Shutdown)

| | Wise | PowerOff |
|---|---|---|
| Tasks: Shutdown, Restart, Power off, Log off, Lock, Sleep, Hibernate | ✓ | ✓ |
| Daily at HH:MM (e.g. sleep at 11 PM) | ✓ | ✓ |
| Once at date/time | ✓ | ✓ |
| Countdown | ✓ | ✓ |
| Idle (mouse+keyboard) | ✓ | ✓ |
| Repeat every N minutes (v2.1.1 intervals) | ✓ | ✓ |
| 5-minute reminder (Run now / Delay 10 min / Cancel) | ✓ | ✓ |
| Minimizes to tray, keeps running, tray menu | ✓ | ✓ |
| Run at Windows startup (registry Run key) | ✓ | ✓ |
| Remembers last settings (`%APPDATA%\PowerOff\poweroff.ini`) | ✓ | ✓ |

Plus: a headless CLI and one-command Task Scheduler install, so the
11 PM sleep works even when the app isn't running (and survives reboot).

## Your use case: sleep at 11 PM every day

Recommended (no need to keep the app open):

```bat
PowerOff.exe --install-daily 23:00 --task sleep
```

This creates a Task Scheduler entry "PowerOff daily" that fires at 23:00,
shows a 60-second "press Cancel to abort" warning, then sleeps the PC.
Remove it with:

```bat
PowerOff.exe --uninstall
```

Alternative (classic Wise style — app stays in the tray):

```bat
PowerOff.exe            :: defaults to Sleep / Daily 23:00, click "Start task"
PowerOff.exe --tray     :: start minimized to tray
PowerOff.exe --dark     :: force dark mode (--light forces light)
```

## CLI reference

```bat
PowerOff.exe --help
PowerOff.exe --task sleep --daily 23:00        :: headless scheduler (console)
PowerOff.exe --task shutdown --once "2026-10-05 23:00"
PowerOff.exe --task sleep --in 3600            :: seconds, or --countdown 1:00:00
PowerOff.exe --task lock --idle 15
PowerOff.exe --task sleep --every 60
PowerOff.exe --task sleep --fire-now --warn-secs 60
```

Tasks: `shutdown restart poweroff logoff lock sleep hibernate`

## Build

```bat
cargo test
cargo build --release        :: -> target\release\poweroff.exe (~255 KB)
```

Release profile: `opt-level="z"`, LTO, single codegen unit, stripped,
`panic="abort"`, `/OPT:REF,ICF` + `/ALIGN:512`. Only dependency: `windows-sys`
(raw OS bindings, not a UI framework).

## Notes

- Sleep/Hibernate use `SetSuspendState`; Shutdown/Restart/Log off use
  `ExitWindowsEx` with `SeShutdownPrivilege` enabled.
- Idle is measured with `GetLastInputInfo` (same as Wise ≥ 2.0.6).
- Like Wise, closing the window while a task runs hides to the tray
  instead of exiting; tray menu offers Show / Start-Cancel / Sleep now / Exit.
