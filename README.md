# PowerNapps PowerOff

**Shut down, restart or sleep your PC on a schedule — in an app that looks
like it shipped with Windows 11, weighs ~160 KB and, once you close it,
uses no memory at all.**

- 🪶 **The only power-off app that eats 0 MB of RAM** (with Tray icon
  Off) — because it doesn't have to ;) Windows Task Scheduler does the
  job; PowerOff is just the remote control you open once a month.
- ⚡ **Highly optimized, tiny, zero dependencies.** One ~160 KB exe in
  plain C. No .NET, no Electron, no VC++ Redistributable, no runtime.
  Wise Auto Shutdown ships ~3.17 MB for the same job.
- 🎨 **Designed as an extension of Windows Settings**, on purpose — same
  cards, toggles, flyouts, fonts and spacing as the real Settings app,
  with one twist: full-color Fluent icons.
- ⏰ **1-minute heads-up.** A reminder card counts down the last minute:
  run now, snooze 10 minutes, or stop.
- 🛡️ **Reliable by design.** The schedule *is* a Windows scheduled task,
  so it fires after reboots, with the app closed, even if PowerOff
  crashed — there's nothing left running that could crash.
- ♿ **Accessible and sharp everywhere.** Keyboard, Narrator, light/dark,
  per-monitor DPI.

---

## Features

### Actions

Shut down · Restart · Sleep · Hibernate · Sign out · Lock · Turn off display

### Triggers

| Trigger | Example |
|---|---|
| **Daily** at a time | Sleep at 11:00 PM every day |
| **Weekly** on chosen days | Shut down at 6:30 PM Mon–Fri |
| **Countdown** | Turn off the display in 30 minutes |
| **Idle** | Lock after 15 minutes without mouse or keyboard |

The command line adds **once at** a date/time and **repeat every** N
minutes.

### Options

- **Remind me 1 minute before** — a small card appears bottom-right one
  minute before the action, with a live countdown and three buttons:
  *Sleep now* (or Shut down now, …), *Snooze 10 min*, *Stop*. Esc stops.
  Ignore it and the action runs on time.
- **Force apps to close** — apps with unsaved work won't block a shut
  down, restart or sign out. Off, PowerOff still closes hung apps.
- **Start with Windows** — run at sign-in.
- **Tray icon** — keep a tiny tray icon after closing, or leave nothing
  running at all (see below).
- **Wake up** — wake the PC from sleep or hibernate every day at a set
  time (shown for Sleep and Hibernate, where it applies).

### The window

- One page, laid out like a Windows 11 Settings page: **Schedule** (the
  hero card: what will happen, when, how long is left, Start / Cancel),
  **Task** (Action, Schedule, Time, Repeat on), **Options**, and footer
  links to **Power settings** (opens the real Windows page) and **About**.
- WinUI-style toggle switches, dropdowns with the accent selection pill,
  the Windows 11 time picker flyout, ContentDialog-style About box, 48 px
  custom title bar, hover highlights, Segoe UI Variable, follows the
  system light/dark theme.
- The live countdown repaints just its own text — no flicker.
- **About** has a short description, a **Contact us** button
  (hi@powernapps.net — questions, concerns, or a gig to offer), a link to
  more apps at [powernapps.net](https://powernapps.net), and Check for updates.

---

## 0 MB: how it runs

PowerOff doesn't run your schedule. **Windows does.**

- **Start** registers a task named `PowerOff` in Task Scheduler (daily,
  weekly or one-shot). Changing an option while it's active updates the
  task; **Cancel** deletes it.
- **The task is the single source of truth.** The app reads it back
  whenever it opens or regains focus. Change the time in Task Scheduler
  and the app shows the new time; delete the task there and the app goes
  back to "Not scheduled".
- With the reminder on, the task starts 1 minute early and runs
  `poweroff.exe --fire-now`, which shows the reminder card and then
  performs the action. It runs on battery too, and one-shot tasks clean
  themselves up afterwards.
- **Wake up** is a second task, `PowerOff wake`, with *Wake the computer
  to run this task* set.
- Idle is the one trigger Task Scheduler can't express, so it runs
  in-process while the app or tray icon is alive.

So after you close the window:

| Tray icon | What's left running | Memory |
|---|---|---|
| **Off** | nothing — the process exits | **0 MB** |
| On | a fresh tray-only process that maps only what one icon needs | ~1.6 MB private |

---

## Optimized to the bone

- **~160 KB exe** including 13 full-color icons (losslessly squeezed), the app icon and the
  manifest. ~4,000 lines of C, one translation unit.
- **No dependencies.** The C runtime is linked statically against the
  Universal CRT that ships with Windows 10/11 — nothing to install.
- **Delay-loaded UI.** comctl32, uxtheme, dwmapi, shell32, powrprof,
  WIC, OLE and accessibility DLLs load only when the window opens, so
  the tray process never maps them.
- **No framework controls.** Win32 has no cards, toggles or flyouts and
  WinUI can't be hosted from plain C, so every control is owner-drawn
  with GDI: rounded shapes are anti-aliased per pixel from a signed
  distance field, icons are WIC-scaled PNGs at the exact device size,
  painting is double-buffered and limited to what changed.
- Built with `/O1 /Os /GL /Gy`, `/OPT:REF /OPT:ICF`, `/FILEALIGN:512`.

## Accessible and sharp

- **Display scaling:** per-monitor DPI aware (v2). All geometry is in
  the design's 96-DPI pixels and scaled to the monitor; moving to another
  monitor rebuilds fonts, icons and layout. `POWEROFF_TEST_DPI=144` tries
  150% on any screen.
- **Keyboard:** Tab / Shift+Tab in reading order with the WinUI focus
  ring, Space / Enter to press, arrow keys in dropdowns and the time
  picker, Esc to close; focus is restored when you come back.
- **Screen readers:** names, roles, values and states via Dynamic
  Annotation, so Narrator reads "Remind me 1 minute before, check box,
  checked" and "Action, combo box, Lock".

---

## Install

Download `PowerOff-setup.exe` from
[GitHub releases](https://github.com/nappsllc/PowerOff/releases/latest).
It installs per user, **no admin rights**, to
`%LOCALAPPDATA%\Programs\PowerOff` and adds a Start menu shortcut.

Uninstall from Settings > Apps. That also removes PowerOff's scheduled
tasks (`PowerOff`, `PowerOff wake`, `PowerOff daily`), its startup entry
and its settings (`%APPDATA%\PowerOff\`).

## Command line

```bat
:: GUI
PowerOff.exe [--tray] [--dark|--light]

:: daily 11 PM sleep as a scheduled task, without opening the window
PowerOff.exe --install-daily 23:00 --task sleep
PowerOff.exe --uninstall

:: headless scheduler (runs in the console until the task fires)
PowerOff.exe --task sleep --daily 23:00
PowerOff.exe --task sleep --weekly 23:00 Mon,Tue,Wed,Thu,Fri
PowerOff.exe --task shutdown --once "2026-10-05 23:00"
PowerOff.exe --task sleep --in 3600 | --countdown 1:00:00
PowerOff.exe --task lock --idle 15 | --task sleep --every 60

:: act now, after a 1-minute reminder (what the scheduled task runs)
PowerOff.exe --task shutdown --fire-now [--warn-secs 60] [--force]

:: turn the display on after a timer wake (used by the wake task)
PowerOff.exe --wake
```

Tasks: `shutdown restart sleep hibernate logoff lock display`.
The exe is a windowed program (no console flash) that attaches to the
calling console for CLI output.

---

## Build

Requires MSVC Build Tools + a Windows 10/11 SDK, and NSIS 3 for the
installer:

```bat
build.bat                                         :: -> poweroff.exe
makensis /DVERSION=1.0.0 installer\poweroff.nsi   :: -> PowerOff-setup.exe
```

`build.bat` uses the current MSVC environment when `cl.exe` is on PATH,
otherwise finds Visual Studio with `vswhere`. CI
(`.github/workflows/build.yml`) builds both on every push and pull
request; pushing a `v*` tag publishes a GitHub release.

### Source layout

`poweroff.c` is a unity build: it includes `src/` in order, so the whole
app is one translation unit (everything `static`, one `cl` call). Each
part only uses what comes before it.

| File | Contents |
|---|---|
| `src/model.c` | plan / runtime model, time math, power actions, settings |
| `src/taskschd.c` | Task Scheduler (COM): the schedule task, wake task, reading tasks back |
| `src/theme.c` | theme tokens, DPI scaling, GDI and anti-aliasing helpers, icons |
| `src/access.c` | accessibility annotations |
| `src/flyout.c` | dropdown flyout |
| `src/layout.c` | control helpers, hover tracking, page layout |
| `src/tray.c` | notification-area icon |
| `src/views.c` | view strings, row and control painting |
| `src/timepicker.c` | time picker flyout |
| `src/about.c` | About dialog |
| `src/reminder.c` | 1-minute reminder card |
| `src/window.c` | title bar, main window, GUI startup |
| `src/cli.c` | headless scheduler and command line |

Assets: `icons/svg/` holds the design's Fluent color icons;
`tools/make-icons.ps1` renders them to the embedded 48 px PNGs and
`tools/make-icon.ps1` builds `poweroff.ico`, both with headless Edge;
`tools/optimize-png.py` then squeezes them losslessly (`python tools/optimize-png.py icons/*.png poweroff.ico`).
The generated files are committed, so builds don't need Edge.

### Under the hood

- Sleep / Hibernate use `SetSuspendState`; Shut down / Restart / Sign
  out use `ExitWindowsEx` with `SeShutdownPrivilege` and a planned
  shutdown reason (`EWX_FORCE` with Force on, `EWX_FORCEIFHUNG` off).
- Idle is measured with `GetLastInputInfo`.
- Only one copy runs: launching again brings up the running one.

---

© 2026 PowerNapps (by Denis Platonov), Los Angeles
