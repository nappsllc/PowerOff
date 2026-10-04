# UI Builder Prompt — PowerOff (copy of Windows 11 Settings)

Paste everything below the line into the UI AI builder.

---

You are designing the main window of **PowerOff**, a tiny Windows 11 desktop
utility that shuts down / restarts / sleeps the PC on a schedule (a Wise Auto
Shutdown clone). Deliverable: a **single self-contained HTML file** that is a
pixel-faithful, interactive mockup of the window, in both light and dark mode
(add a light/dark toggle at the top of the mockup page, outside the window).

## Prime directive

The window must look EXACTLY like a native Windows 11 **Settings** page
(e.g. Settings → Network & internet). Same background, same white rounded
cards, same row layout (monochrome glyph icon + bold-ish title + gray
subtitle on the left, control right-aligned), same WinUI toggle switches,
same Segoe typography, same spacing. Do NOT invent a custom design language,
no brand colors, no gradients, no shadows beyond the subtle card border.
If in doubt, copy the Settings app, not your instincts.

## Window + theme tokens

- Window: 584 × 668 px, standard Windows title bar with caption
  "PowerOff — tiny auto shutdown". Client area only in the mockup.
- Font everywhere: **Segoe UI Variable Text** (fallback Segoe UI).
- Light mode: page background `#F3F3F3`, cards `#FFFFFF` with 8 px rounded
  corners and 1 px `#E5E5E5` border, primary text `#1B1B1B`, secondary text
  `#605E5C`, row dividers `#EDEDED`.
- Dark mode: page `#202020`, cards `#2D2D2D` with `#3A3A3A` border, primary
  text `#FFFFFF`, secondary `#ADABA4`, dividers `#3E3E3E`.
- Accent: use Windows system accent blue (`#0078D4` as fallback) for the
  hero icon circle and toggle-ON fill.
- Page title at top-left: "PowerOff", 20 pt, semibold. No other header.

## Layout (top to bottom, 16 px page margins, 12 px gaps between cards)

### 1. Hero card (full width, ~92 px tall)

Left: solid accent-blue circle (52 px) with a white power symbol.
Next to it, title 14 pt semibold + gray subtitle line below.
Right side: a default push button "Start task" (turns into "Cancel task"
while armed).
- Idle state: title "Sleep daily", subtitle "Runs every day at 23:00".
- Armed state: title "Sleep daily at 23:00", subtitle "Next run in 4:12:33"
  (live countdown text).

### 2. Schedule card (3 rows, ~52 px each, 1 px dividers between rows)

- Row 1: power glyph · Title "Action" · subtitle "What the PC should do" ·
  right: dropdown with Shut down / Restart / Power off / Log off / Lock /
  Sleep (default) / Hibernate.
- Row 2: calendar glyph · Title "Schedule" · subtitle "When it runs" ·
  right: dropdown with Daily / Once / Countdown / Idle / Repeat every.
- Row 3: clock glyph · Title "Time" · subtitle = hint text that changes per
  mode ("Runs every day at this time" / "Runs once at the given date and
  time" / "Runs after the countdown elapses" / "Runs when mouse +
  keyboard are idle that long" / "Repeats the task until cancelled") ·
  right: inputs that swap per mode —
  Daily: one `HH:MM` field (`23:00`);
  Once: `YYYY-MM-DD` + `HH:MM` fields;
  Countdown: three small numeric fields (hours/min/sec);
  Idle + Repeat every: one numeric field (minutes).

### 3. Status card

- Big countdown readout, ~22 pt semibold (`4:12:33`, or `--:--` when idle).
- One description line under it ("Sleep daily at 23:00 • in 4:12:33").
- Full-width native-style progress bar under that, filling as time elapses.

### 4. Options card (3 rows with dividers)

- Row 1: bell glyph · "Reminders" · "Warn 5 minutes before it runs" ·
  right: WinUI toggle switch (ON).
- Row 2: circular-arrow glyph · "Run at startup" ·
  "Start minimized to the tray" · right: toggle (OFF).
- Row 3: monitor glyph · "Power options" · "Open Windows Settings" ·
  right: chevron "›" (opens the real Settings app).

## Controls spec (match WinUI/Settings exactly)

- Toggle switch: pill 40×20 px, 10 px radius. ON = accent fill + white knob
  docked right + gray "On" label left of it. OFF = transparent fill with
  1 px gray border + gray knob docked left + gray "Off" label. No animation
  needed in the mockup, but the ON/OFF label text is required.
- Dropdowns: native-combo look, 148 px wide, 24 px tall.
- Text fields: white (dark: dark fill), 1 px gray border, 9 pt text.
- Start task button: default-button styling, 116×32 px.
- Glyphs: 20 px monochrome line icons (power, calendar, clock, bell,
  circular arrow, monitor), secondary-gray color.

## Interactivity required in the mockup

- Switching the Schedule dropdown swaps the Time-row inputs AND its hint.
- Toggles flip On/Off on click.
- Pressing "Start task" starts a live countdown: hero subtitle ticks down,
  big readout ticks, progress bar fills; button becomes "Cancel task".
- The dark-mode toggle restyles the whole window per the tokens above.
- Reminder + tray icon are OS-level and out of scope — do not mock them.

## Constraints

- No Tailwind, no Bootstrap, no web fonts (Segoe UI is a system font).
- No emojis. No lorem ipsum — use the exact strings given above.
- Keep it clean and sparse like Settings: generous whitespace, left-aligned
  text, right-aligned controls, sentence case everywhere.
