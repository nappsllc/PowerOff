// PowerOff — tiny Wise Auto Shutdown clone.
// Native Win32 UI only (no rich UI framework). One binary, one dependency:
// `windows-sys` (raw OS bindings, not a UI framework).
//
// Visual language: Deskwarden design system (Archivo-like type, #1b3fa0 blue,
// white cards on #f3f2f2, left section rail, sticky bottom action bar),
// implemented with owner-drawn native controls.
//
// Usage:
//   PowerOff.exe                              -> GUI (defaults: Sleep, Daily 23:00)
//   PowerOff.exe --tray                       -> GUI, start minimized to tray
//   PowerOff.exe --task sleep --daily 23:00   -> headless scheduler in console
//   PowerOff.exe --task sleep --fire-now [--warn-secs 60]
//   PowerOff.exe --install-daily 23:00 --task sleep   (Task Scheduler, survives reboot)
//   PowerOff.exe --uninstall

use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};

use windows_sys::Win32::Foundation::{
    HANDLE, HINSTANCE, HWND, LPARAM, LRESULT, LUID, POINT, RECT, WPARAM,
};
use windows_sys::Win32::Graphics::Dwm::DwmSetWindowAttribute;
use windows_sys::Win32::Graphics::Gdi::{
    CreateFontW, DeleteObject, CreateSolidBrush, CreatePen, FillRect, GetStockObject,
    RoundRect, SelectObject, SetBkMode, SetTextColor,
    COLOR_WINDOW, HFONT, TRANSPARENT, NULL_BRUSH, PS_SOLID,
};
use windows_sys::Win32::Security::{
    AdjustTokenPrivileges, LookupPrivilegeValueW, LUID_AND_ATTRIBUTES, SE_PRIVILEGE_ENABLED,
    TOKEN_ADJUST_PRIVILEGES, TOKEN_PRIVILEGES, TOKEN_QUERY,
};
use windows_sys::Win32::System::Console::GetConsoleWindow;
use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
use windows_sys::Win32::System::LibraryLoader::GetProcAddress;
use windows_sys::Win32::System::Power::SetSuspendState;
use windows_sys::Win32::System::Registry::{
    RegCloseKey, RegDeleteValueW, RegOpenKeyExW, RegQueryValueExW, RegSetValueExW, HKEY,
    HKEY_CURRENT_USER, KEY_QUERY_VALUE, KEY_SET_VALUE, REG_SZ,
};
use windows_sys::Win32::System::Shutdown::{
    ExitWindowsEx, LockWorkStation, EWX_FORCEIFHUNG, EWX_LOGOFF, EWX_POWEROFF, EWX_REBOOT,
    EWX_SHUTDOWN,
};
use windows_sys::Win32::System::SystemInformation::{GetLocalTime, GetTickCount64};
use windows_sys::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};
use windows_sys::Win32::UI::Controls::{
    BST_CHECKED, BST_UNCHECKED, SetWindowTheme, PBM_SETPOS, PBM_SETRANGE, PBS_SMOOTH,
    InitCommonControlsEx, INITCOMMONCONTROLSEX, ICC_PROGRESS_CLASS,
};
use windows_sys::Win32::UI::Input::KeyboardAndMouse::{GetLastInputInfo, LASTINPUTINFO};
use windows_sys::Win32::UI::Shell::{
    Shell_NotifyIconW, NIF_ICON, NIF_MESSAGE, NIF_TIP, NIM_ADD, NIM_DELETE, NOTIFYICONDATAW,
};
use windows_sys::Win32::UI::WindowsAndMessaging::*;

// ---------------------------------------------------------------------------
// Deskwarden palette
// ---------------------------------------------------------------------------

const fn rgb(r: u32, g: u32, b: u32) -> u32 {
    r | (g << 8) | (b << 16)
}

// Date/time entry uses plain Deskwarden-style text inputs (HH:MM,
// YYYY-MM-DD) validated on Start — synchronous user32 text, no custom
// control messages involved.

// ---------------------------------------------------------------------------
// Theme: Deskwarden light tokens + dark tokens (from the design system's
// own dark mode), selected via AppsUseLightTheme. Native controls follow
// via SetPreferredAppMode (uxtheme ordinals, best-effort) + dark title bar.
// ---------------------------------------------------------------------------

static DARK: AtomicBool = AtomicBool::new(false);

fn dark() -> bool {
    DARK.load(Ordering::SeqCst)
}

/// Dialog background: theme-aware (dark #151515, light = button face).
fn dlg_bg() -> u32 {
    if dark() {
        rgb(21, 21, 21)
    } else {
        rgb(240, 240, 240)
    }
}

/// Re-read AppsUseLightTheme (0 = dark). Defaults to light on any error.
fn read_theme() {
    let sub = to_wide("Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize");
    let val = to_wide("AppsUseLightTheme");
    let mut hkey: HKEY = std::ptr::null_mut();
    let mut is_dark = false;
    unsafe {
        if RegOpenKeyExW(HKEY_CURRENT_USER, sub.as_ptr(), 0, KEY_QUERY_VALUE, &mut hkey) == 0 {
            let mut dtype: u32 = 0;
            let mut data: u32 = 1;
            let mut cb = 4u32;
            if RegQueryValueExW(
                hkey,
                val.as_ptr(),
                std::ptr::null(),
                &mut dtype,
                &mut data as *mut u32 as *mut u8,
                &mut cb,
            ) == 0
            {
                is_dark = data == 0;
            }
            RegCloseKey(hkey);
        }
    }
    DARK.store(is_dark, Ordering::SeqCst);
}

/// Apply dark title bar + dark native controls. The uxtheme ordinals are
/// undocumented but stable since 1809; every step is best-effort — if the
/// entry points are missing we simply keep light controls.
fn apply_dark_mode(hwnd: HWND) {
    let is_dark = dark();
    unsafe {
        // Documented: dark title bar.
        let on: i32 = if is_dark { 1 } else { 0 };
        DwmSetWindowAttribute(
            hwnd,
            20, // DWMWA_USE_IMMERSIVE_DARK_MODE
            &on as *const i32 as *const std::ffi::c_void,
            4,
        );
        // Best-effort: dark Win32 controls (checkbox, edit, listbox...).
        let ux = to_wide("uxtheme.dll");
        let hmod = GetModuleHandleW(ux.as_ptr());
        if !hmod.is_null() {
            // 135 = SetPreferredAppMode(AllowDark=1), 133 =
            // AllowDarkDarkModeForWindow, 104 = RefreshImmersiveColorPolicyState.
            if let Some(f) = GetProcAddress(hmod, 135 as *const u8) {
                let set_mode: unsafe extern "system" fn(i32) -> i32 = std::mem::transmute(f);
                set_mode(if is_dark { 1 } else { 0 });
            }
            if let Some(f) = GetProcAddress(hmod, 133 as *const u8) {
                let allow: unsafe extern "system" fn(HWND, i32) -> i32 = std::mem::transmute(f);
                allow(hwnd, if is_dark { 1 } else { 0 });
            }
            let sub = if is_dark { to_wide("DarkMode_Explorer") } else { Vec::new() };
            SetWindowTheme(
                hwnd,
                if is_dark { sub.as_ptr() } else { std::ptr::null() },
                std::ptr::null(),
            );
            if let Some(f) = GetProcAddress(hmod, 104 as *const u8) {
                let refresh: unsafe extern "system" fn() = std::mem::transmute(f);
                refresh();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Task {
    Shutdown,
    Restart,
    PowerOff,
    LogOff,
    Lock,
    Sleep,
    Hibernate,
}

impl Task {
    fn all() -> &'static [Task] {
        &[
            Task::Shutdown,
            Task::Restart,
            Task::PowerOff,
            Task::LogOff,
            Task::Lock,
            Task::Sleep,
            Task::Hibernate,
        ]
    }
    fn name(self) -> &'static str {
        match self {
            Task::Shutdown => "Shut down",
            Task::Restart => "Restart",
            Task::PowerOff => "Power off",
            Task::LogOff => "Log off",
            Task::Lock => "Lock",
            Task::Sleep => "Sleep",
            Task::Hibernate => "Hibernate",
        }
    }
    fn parse(s: &str) -> Option<Task> {
        match s.to_ascii_lowercase().replace(['_', '-'], "").as_str() {
            "shutdown" => Some(Task::Shutdown),
            "restart" | "reboot" => Some(Task::Restart),
            "poweroff" => Some(Task::PowerOff),
            "logoff" | "logout" => Some(Task::LogOff),
            "lock" => Some(Task::Lock),
            "sleep" | "suspend" => Some(Task::Sleep),
            "hibernate" => Some(Task::Hibernate),
            _ => None,
        }
    }
    /// Repeatable tasks (Wise semantics: idle sleep/hibernate/lock repeat).
    fn repeats_on_idle(self) -> bool {
        matches!(self, Task::Sleep | Task::Hibernate | Task::Lock)
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Mode {
    Daily,
    Once,
    Countdown,
    Idle,
    Interval,
}

impl Mode {
    fn all() -> &'static [Mode] {
        &[Mode::Daily, Mode::Once, Mode::Countdown, Mode::Idle, Mode::Interval]
    }
    fn hint(self) -> &'static str {
        match self {
            Mode::Daily => "Runs every day at this time. Example: 11:00 PM = sleep at 11 PM.",
            Mode::Once => "Runs once at the given date and time.",
            Mode::Countdown => "Runs after the countdown elapses.",
            Mode::Idle => "Runs when mouse + keyboard are idle that long. Sleep / Lock / Hibernate re-arm.",
            Mode::Interval => "Repeats the task every N minutes until cancelled.",
        }
    }
}

#[derive(Clone, Debug)]
struct Plan {
    task: Task,
    mode: Mode,
    // Params (interpretation depends on mode):
    //   Daily:    p1=hour, p2=min
    //   Once:     date=p1 "YYYY-MM-DD", time=p2 "HH:MM"
    //   Countdown: p1=h, p2=m, p3=s
    //   Idle:     p1=minutes
    //   Interval: p1=minutes
    p1: String,
    p2: String,
    p3: String,
    remind_min: u32,
}

impl Default for Plan {
    fn default() -> Self {
        Self {
            task: Task::Sleep,
            mode: Mode::Daily,
            p1: "23".into(),
            p2: "00".into(),
            p3: "00".into(),
            remind_min: 5,
        }
    }
}

// ---------------------------------------------------------------------------
// Cross-thread reminder signalling (reminder dialog runs on its own thread)
// ---------------------------------------------------------------------------

static FIRE_NOW: AtomicBool = AtomicBool::new(false);
static DELAY_SECS: AtomicI32 = AtomicI32::new(0);
static CANCEL_REQ: AtomicBool = AtomicBool::new(false);

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

fn to_wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

fn from_wide(buf: &[u16]) -> String {
    let n = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
    String::from_utf16_lossy(&buf[..n])
}

fn days_from_civil(y: i32, m: u32, d: u32) -> i64 {
    // Howard Hinnant's algorithm.
    let y = if m <= 2 { y - 1 } else { y };
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = y - era * 400;
    let doy = (153 * (if m > 2 { m - 3 } else { m + 9 }) + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy as i32;
    era as i64 * 146097 + doe as i64 - 719468
}

fn local_now_secs() -> i64 {
    unsafe {
        let mut st: windows_sys::Win32::Foundation::SYSTEMTIME = std::mem::zeroed();
        GetLocalTime(&mut st);
        days_from_civil(st.wYear as i32, st.wMonth as u32, st.wDay as u32) * 86400
            + st.wHour as i64 * 3600
            + st.wMinute as i64 * 60
            + st.wSecond as i64
    }
}

fn local_now_hms() -> (u32, u32, u32) {
    unsafe {
        let mut st: windows_sys::Win32::Foundation::SYSTEMTIME = std::mem::zeroed();
        GetLocalTime(&mut st);
        (st.wHour as u32, st.wMinute as u32, st.wSecond as u32)
    }
}

fn idle_secs() -> u64 {
    unsafe {
        let mut lii = LASTINPUTINFO {
            cbSize: std::mem::size_of::<LASTINPUTINFO>() as u32,
            dwTime: 0,
        };
        if GetLastInputInfo(&mut lii) != 0 {
            GetTickCount64().wrapping_sub(lii.dwTime as u64) / 1000
        } else {
            0
        }
    }
}

fn fmt_dur(mut secs: i64) -> String {
    if secs < 0 {
        secs = 0;
    }
    let h = secs / 3600;
    let m = (secs % 3600) / 60;
    let s = secs % 60;
    if h > 0 {
        format!("{h}:{m:02}:{s:02}")
    } else {
        format!("{m}:{s:02}")
    }
}

fn parse_hhmm(s: &str) -> Option<(u32, u32)> {
    let s = s.trim();
    let mut it = s.split(':');
    let h: u32 = it.next()?.trim().parse().ok()?;
    let m: u32 = it.next().unwrap_or("0").trim().parse().ok()?;
    if it.next().is_some() || h > 23 || m > 59 {
        return None;
    }
    Some((h, m))
}

fn parse_ymd(s: &str) -> Option<(i32, u32, u32)> {
    let p: Vec<&str> = s.trim().split('-').collect();
    if p.len() != 3 {
        return None;
    }
    let (y, m, d): (i32, u32, u32) = (p[0].parse().ok()?, p[1].parse().ok()?, p[2].parse().ok()?);
    if !(1970..=2100).contains(&y) || m < 1 || m > 12 || d < 1 || d > 31 {
        return None;
    }
    Some((y, m, d))
}

// ---------------------------------------------------------------------------
// Power actions
// ---------------------------------------------------------------------------

fn enable_shutdown_privilege() {
    unsafe {
        let mut token: HANDLE = std::ptr::null_mut();
        if OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
            &mut token,
        ) != 0
        {
            let mut luid = LUID {
                LowPart: 0,
                HighPart: 0,
            };
            let name = to_wide("SeShutdownPrivilege");
            if LookupPrivilegeValueW(std::ptr::null(), name.as_ptr(), &mut luid) != 0 {
                let tp = TOKEN_PRIVILEGES {
                    PrivilegeCount: 1,
                    Privileges: [LUID_AND_ATTRIBUTES {
                        Luid: luid,
                        Attributes: SE_PRIVILEGE_ENABLED,
                    }],
                };
                AdjustTokenPrivileges(
                    token,
                    0,
                    &tp,
                    0,
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                );
            }
        }
    }
}

fn do_task(task: Task) -> bool {
    unsafe {
        match task {
            Task::Lock => LockWorkStation() != 0,
            Task::Sleep => SetSuspendState(0, 1, 0) != 0,
            Task::Hibernate => SetSuspendState(1, 1, 0) != 0,
            Task::LogOff => {
                enable_shutdown_privilege();
                ExitWindowsEx(EWX_LOGOFF | EWX_FORCEIFHUNG, 0) != 0
            }
            Task::Shutdown => {
                enable_shutdown_privilege();
                ExitWindowsEx(EWX_SHUTDOWN | EWX_FORCEIFHUNG, 0) != 0
            }
            Task::PowerOff => {
                enable_shutdown_privilege();
                ExitWindowsEx(EWX_POWEROFF | EWX_FORCEIFHUNG, 0) != 0
            }
            Task::Restart => {
                enable_shutdown_privilege();
                ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG, 0) != 0
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Scheduler core (shared by GUI timer and headless console loop)
// ---------------------------------------------------------------------------

struct Runtime {
    plan: Plan,
    active: bool,
    reminded: bool,
    /// Absolute epoch (local) when a Countdown/Once/Interval/Delay fires.
    fire_at: i64,
    /// Total span in seconds (for progress); daily/idle compute live.
    span: i64,
    /// Extra delay added from the reminder dialog.
    delay_applied: i64,
}

impl Runtime {
    fn new(plan: Plan) -> Self {
        Self {
            plan,
            active: false,
            reminded: false,
            fire_at: 0,
            span: 0,
            delay_applied: 0,
        }
    }

    fn start(&mut self) -> Result<String, String> {
        self.reminded = false;
        self.delay_applied = 0;
        let now = local_now_secs();
        match self.plan.mode {
            Mode::Daily => {
                parse_hhmm(&self.p2_free())
                    .ok_or("Hour must be 0-23 and minute 0-59".to_string())?;
                self.fire_at = 0; // computed dynamically
                self.span = 86400;
            }
            Mode::Once => {
                let (y, mo, d) =
                    parse_ymd(&self.plan.p1).ok_or("Date must be YYYY-MM-DD".to_string())?;
                let (h, mi) = parse_hhmm(&self.plan.p2)
                    .ok_or("Time must be HH:MM (24h)".to_string())?;
                let at = days_from_civil(y, mo, d) * 86400 + h as i64 * 3600 + mi as i64 * 60;
                if at <= now {
                    return Err("That date/time is in the past.".into());
                }
                self.fire_at = at;
                self.span = at - now;
            }
            Mode::Countdown => {
                let h: i64 = self.plan.p1.trim().parse().map_err(|_| "Bad hours".to_string())?;
                let m: i64 = self.plan.p2.trim().parse().map_err(|_| "Bad minutes".to_string())?;
                let s: i64 = self.plan.p3.trim().parse().map_err(|_| "Bad seconds".to_string())?;
                let total = h * 3600 + m * 60 + s;
                if total <= 0 {
                    return Err("Countdown must be > 0.".into());
                }
                self.fire_at = now + total;
                self.span = total;
            }
            Mode::Idle => {
                let mins: i64 = self.plan.p1.trim().parse().map_err(|_| "Bad minutes".to_string())?;
                if mins <= 0 {
                    return Err("Idle minutes must be > 0.".into());
                }
                self.fire_at = 0;
                self.span = mins * 60;
            }
            Mode::Interval => {
                let mins: i64 = self.plan.p1.trim().parse().map_err(|_| "Bad minutes".to_string())?;
                if mins <= 0 {
                    return Err("Interval minutes must be > 0.".into());
                }
                self.fire_at = now + mins * 60;
                self.span = mins * 60;
            }
        }
        self.active = true;
        Ok(self.describe())
    }

    fn p2_free(&self) -> String {
        // Daily stores hour in p1 and min in p2; combine for parser.
        format!("{}:{}", self.plan.p1.trim(), self.plan.p2.trim())
    }

    fn describe(&self) -> String {
        match self.plan.mode {
            Mode::Daily => format!(
                "{} daily at {:02}:{:02}",
                self.plan.task.name(),
                self.plan.p1.trim().parse::<u32>().unwrap_or(23),
                self.plan.p2.trim().parse::<u32>().unwrap_or(0)
            ),
            Mode::Once => format!("{} once at {} {}", self.plan.task.name(), self.plan.p1.trim(), self.plan.p2.trim()),
            Mode::Countdown => format!(
                "{} in {}h {}m {}s",
                self.plan.task.name(),
                self.plan.p1.trim(),
                self.plan.p2.trim(),
                self.plan.p3.trim()
            ),
            Mode::Idle => format!("{} after {} min idle", self.plan.task.name(), self.plan.p1.trim()),
            Mode::Interval => format!("{} every {} min", self.plan.task.name(), self.plan.p1.trim()),
        }
    }

    /// Seconds until the task fires. For Daily/Idle this is computed live.
    fn remaining(&self) -> i64 {
        let now = local_now_secs();
        match self.plan.mode {
            Mode::Daily => {
                let h: i64 = self.plan.p1.trim().parse().unwrap_or(23);
                let m: i64 = self.plan.p2.trim().parse().unwrap_or(0);
                let (ch, cm, cs) = local_now_hms();
                let now_s = ch as i64 * 3600 + cm as i64 * 60 + cs as i64;
                let tgt = h * 3600 + m * 60;
                let mut d = tgt - now_s;
                if d <= 0 {
                    d += 86400;
                }
                d
            }
            Mode::Idle => {
                let mins: i64 = self.plan.p1.trim().parse().unwrap_or(15);
                mins * 60 - idle_secs() as i64
            }
            _ => self.fire_at + self.delay_applied - now,
        }
    }

    fn fraction_done(&self, rem: i64) -> f32 {
        if self.span <= 0 {
            return 0.0;
        }
        ((self.span - rem) as f32 / self.span as f32).clamp(0.0, 1.0)
    }

    fn apply_reminder_signals(&mut self) {
        if CANCEL_REQ.swap(false, Ordering::SeqCst) {
            self.active = false;
        }
        let d = DELAY_SECS.swap(0, Ordering::SeqCst);
        if d != 0 {
            if self.plan.mode == Mode::Daily {
                // emulate by shifting a delay baseline
                self.delay_applied += d as i64;
                // daily remaining() ignores fire_at; fold delay in via fire_at trick:
                // store negative offset by adjusting p-time? Simplest: convert to countdown.
                let rem = self.remaining_raw_daily() + d as i64;
                self.plan.mode = Mode::Countdown;
                self.plan.p1 = (rem / 3600).to_string();
                self.plan.p2 = ((rem % 3600) / 60).to_string();
                self.plan.p3 = (rem % 60).to_string();
                self.fire_at = local_now_secs() + rem;
                self.span = rem;
                self.delay_applied = 0;
            } else {
                self.delay_applied += d as i64;
            }
            self.reminded = true; // don't nag again immediately
        }
        if FIRE_NOW.swap(false, Ordering::SeqCst) {
            self.fire_at = local_now_secs();
            self.delay_applied = 0;
            if self.plan.mode == Mode::Daily || self.plan.mode == Mode::Idle {
                // force through the one-shot path
                self.plan.mode = Mode::Countdown;
                self.plan.p1 = "0".into();
                self.plan.p2 = "0".into();
                self.plan.p3 = "0".into();
            }
        }
    }

    fn remaining_raw_daily(&self) -> i64 {
        let h: i64 = self.plan.p1.trim().parse().unwrap_or(23);
        let m: i64 = self.plan.p2.trim().parse().unwrap_or(0);
        let (ch, cm, cs) = local_now_hms();
        let now_s = ch as i64 * 3600 + cm as i64 * 60 + cs as i64;
        let mut d = h * 3600 + m * 60 - now_s;
        if d <= 0 {
            d += 86400;
        }
        d
    }
}

// ---------------------------------------------------------------------------
// Config file + autostart (registry Run key)
// ---------------------------------------------------------------------------

fn appdata_ini() -> std::path::PathBuf {
    let base = std::env::var("APPDATA").unwrap_or_else(|_| ".".into());
    std::path::Path::new(&base).join("PowerOff").join("poweroff.ini")
}

fn save_config(plan: &Plan) {
    let p = appdata_ini();
    if let Some(dir) = p.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    let body = format!(
        "task={:?}\nmode={:?}\np1={}\np2={}\np3={}\nremind={}\n",
        plan.task, plan.mode, plan.p1.trim(), plan.p2.trim(), plan.p3.trim(), plan.remind_min
    );
    let _ = std::fs::write(p, body);
}

fn load_config() -> Plan {
    let mut plan = Plan::default();
    let Ok(body) = std::fs::read_to_string(appdata_ini()) else {
        return plan;
    };
    let get = |k: &str| -> Option<String> {
        body.lines().find_map(|l| {
            let (kk, v) = l.split_once('=')?;
            (kk.trim() == k).then(|| v.trim().to_string())
        })
    };
    if let Some(t) = get("task") {
        let t = t.to_ascii_lowercase();
        for c in Task::all() {
            if format!("{:?}", c).to_ascii_lowercase() == t {
                plan.task = *c;
            }
        }
    }
    if let Some(m) = get("mode") {
        let m = m.to_ascii_lowercase();
        for c in Mode::all() {
            if format!("{:?}", c).to_ascii_lowercase() == m {
                plan.mode = *c;
            }
        }
    }
    if let Some(v) = get("p1") {
        plan.p1 = v;
    }
    if let Some(v) = get("p2") {
        plan.p2 = v;
    }
    if let Some(v) = get("p3") {
        plan.p3 = v;
    }
    if let Some(v) = get("remind") {
        if let Ok(n) = v.parse() {
            plan.remind_min = n;
        }
    }
    plan
}

fn run_key_subkey() -> Vec<u16> {
    to_wide("Software\\Microsoft\\Windows\\CurrentVersion\\Run")
}

fn run_key_value() -> Vec<u16> {
    to_wide("PowerOff")
}

fn set_startup(enable: bool) {
    unsafe {
        let sub = run_key_subkey();
        let val = run_key_value();
        let mut hkey: HKEY = std::ptr::null_mut();
        if RegOpenKeyExW(
            HKEY_CURRENT_USER,
            sub.as_ptr(),
            0,
            KEY_SET_VALUE | KEY_QUERY_VALUE,
            &mut hkey,
        ) == 0
        {
            if enable {
                if let Ok(exe) = std::env::current_exe() {
                    let cmd = format!("\"{}\" --tray", exe.display());
                    let wv = to_wide(&cmd);
                    let bytes =
                        std::slice::from_raw_parts(wv.as_ptr() as *const u8, wv.len() * 2);
                    RegSetValueExW(hkey, val.as_ptr(), 0, REG_SZ, bytes.as_ptr(), bytes.len() as u32);
                }
            } else {
                RegDeleteValueW(hkey, val.as_ptr());
            }
            RegCloseKey(hkey);
        }
    }
}

fn get_startup() -> bool {
    unsafe {
        let sub = run_key_subkey();
        let val = run_key_value();
        let mut hkey: HKEY = std::ptr::null_mut();
        if RegOpenKeyExW(HKEY_CURRENT_USER, sub.as_ptr(), 0, KEY_QUERY_VALUE, &mut hkey) != 0 {
            return false;
        }
        let mut dtype: u32 = REG_SZ;
        let mut data = [0u16; 512];
        let mut cb = (data.len() * 2) as u32;
        let ok = RegQueryValueExW(
            hkey,
            val.as_ptr(),
            std::ptr::null(),
            &mut dtype,
            data.as_mut_ptr() as *mut u8,
            &mut cb,
        ) == 0;
        RegCloseKey(hkey);
        ok
    }
}

// ---------------------------------------------------------------------------
// Task Scheduler install (so 11PM sleep works even if the app is closed)
// ---------------------------------------------------------------------------

fn install_daily(task: Task, hhmm: &str) -> Result<String, String> {
    let (h, m) = parse_hhmm(hhmm).ok_or("Use HH:MM, e.g. 23:00".to_string())?;
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    let tr = format!("\"{}\" --task {} --fire-now --warn-secs 60", exe.display(), format!("{:?}", task).to_ascii_lowercase());
    let st = format!("{:02}:{:02}", h, m);
    let out = std::process::Command::new("schtasks")
        .args([
            "/create",
            "/tn",
            "PowerOff daily",
            "/sc",
            "daily",
            "/st",
            &st,
            "/f",
            "/tr",
            &tr,
        ])
        .output()
        .map_err(|e| format!("schtasks failed: {e}"))?;
    if out.status.success() {
        Ok(format!("Scheduled '{}' daily at {st} (Task Scheduler: 'PowerOff daily').", task.name()))
    } else {
        Err(String::from_utf8_lossy(&out.stderr).trim().to_string())
    }
}

fn uninstall_daily() -> Result<String, String> {
    let out = std::process::Command::new("schtasks")
        .args(["/delete", "/tn", "PowerOff daily", "/f"])
        .output()
        .map_err(|e| format!("schtasks failed: {e}"))?;
    if out.status.success() {
        Ok("Removed 'PowerOff daily' scheduled task.".into())
    } else {
        Err(String::from_utf8_lossy(&out.stderr).trim().to_string())
    }
}

// ---------------------------------------------------------------------------
// GUI (pure Win32, owner-drawn in the Deskwarden visual language)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// GUI: 100% native Win32 controls (dialog look), no custom painting.
// ---------------------------------------------------------------------------

const ID_TASK_LIST: isize = 101;
const ID_MODE_COMBO: isize = 102;
const ID_ETIME: isize = 103;
const ID_EDATE: isize = 104;
const ID_ETIME2: isize = 105;
const ID_EDITH: isize = 106;
const ID_EDITM: isize = 107;
const ID_EDITS: isize = 108;
const ID_EDITN: isize = 109;
const ID_HINT: isize = 110;
const ID_CHK_REMIND: isize = 111;
const ID_CHK_STARTUP: isize = 112;
const ID_BIG: isize = 113;
const ID_DESC: isize = 114;
const ID_PROG: isize = 115;
const ID_BTN_START: isize = 116;
const ID_BTN_TRAY: isize = 117;

const IDM_SHOW: u32 = 1001;
const IDM_START: u32 = 1002;
const IDM_NOW: u32 = 1003;
const IDM_EXIT: u32 = 1004;

const WM_TRAY: u32 = 0x8001;
const TIMER_ID: usize = 1;

#[derive(Clone, Copy)]
struct Gui {
    main: HWND,
    task_list: HWND,
    combo: HWND,
    etime: HWND,
    edate: HWND,
    etime2: HWND,
    edit_h: HWND,
    edit_m: HWND,
    edit_s: HWND,
    edit_n: HWND,
    hint: HWND,
    chk_remind: HWND,
    chk_startup: HWND,
    big: HWND,
    desc: HWND,
    prog: HWND,
    btn_start: HWND,
    btn_tray: HWND,
    font: HFONT,
    bigfont: HFONT,
}

static mut GUI: Option<Gui> = None;
static mut RT: Option<Runtime> = None;
static mut CUR_MODE: Mode = Mode::Daily;
/// Set once `GUI` is installed; child controls notify (EN_CHANGE, …)
/// re-entrantly during creation, before that point.
static GUI_READY: AtomicBool = AtomicBool::new(false);

#[allow(static_mut_refs)]
fn gui() -> &'static mut Gui {
    unsafe { GUI.as_mut().expect("gui") }
}
#[allow(static_mut_refs)]
fn rt() -> &'static mut Runtime {
    unsafe { RT.as_mut().expect("rt") }
}
fn cur_mode() -> Mode {
    unsafe { CUR_MODE }
}
fn set_cur_mode(m: Mode) {
    unsafe {
        CUR_MODE = m;
    }
}

fn is_checked(hwnd: HWND) -> bool {
    unsafe { SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED as isize }
}

fn set_checked(hwnd: HWND, on: bool) {
    unsafe {
        SendMessageW(hwnd, BM_SETCHECK, if on { BST_CHECKED as usize } else { BST_UNCHECKED as usize }, 0);
    }
}

// -- tray --------------------------------------------------------------------

fn add_tray(hwnd: HWND) {
    unsafe {
        let mut nid: NOTIFYICONDATAW = std::mem::zeroed();
        nid.cbSize = std::mem::size_of::<NOTIFYICONDATAW>() as u32;
        nid.hWnd = hwnd;
        nid.uID = 1;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = WM_TRAY;
        nid.hIcon = LoadIconW(std::ptr::null_mut(), IDI_APPLICATION);
        let tip = to_wide("PowerOff");
        nid.szTip[..tip.len().min(127)].copy_from_slice(&tip[..tip.len().min(127)]);
        Shell_NotifyIconW(NIM_ADD, &nid);
    }
}

fn del_tray(hwnd: HWND) {
    unsafe {
        let mut nid: NOTIFYICONDATAW = std::mem::zeroed();
        nid.cbSize = std::mem::size_of::<NOTIFYICONDATAW>() as u32;
        nid.hWnd = hwnd;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
}

fn show_tray_menu(hwnd: HWND) {
    unsafe {
        let mut pt = POINT { x: 0, y: 0 };
        GetCursorPos(&mut pt);
        let menu = CreatePopupMenu();
        let running = rt().active;
        let show_s = to_wide("Show window");
        let start_s = to_wide(if running { "Cancel task" } else { "Start task" });
        let now_s = to_wide("Sleep now");
        let exit_s = to_wide("Exit");
        AppendMenuW(menu, MF_STRING, IDM_SHOW as usize, show_s.as_ptr());
        AppendMenuW(menu, MF_STRING, IDM_START as usize, start_s.as_ptr());
        AppendMenuW(menu, MF_STRING, IDM_NOW as usize, now_s.as_ptr());
        AppendMenuW(menu, MF_SEPARATOR, 0, std::ptr::null());
        AppendMenuW(menu, MF_STRING, IDM_EXIT as usize, exit_s.as_ptr());
        SetForegroundWindow(hwnd);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, std::ptr::null());
        DestroyMenu(menu);
    }
}

fn spawn_reminder(task_name: String, secs_left: i64) {
    std::thread::spawn(move || {
        let msg = format!(
            "{task_name} in {}.\n\nYes = do it now   •   No = delay 10 min   •   Cancel = cancel task",
            fmt_dur(secs_left)
        );
        let (m, t) = (to_wide(&msg), to_wide("PowerOff — reminder"));
        let r = unsafe {
            MessageBoxW(
                std::ptr::null_mut(),
                m.as_ptr(),
                t.as_ptr(),
                MB_YESNOCANCEL | MB_ICONWARNING | MB_SYSTEMMODAL,
            )
        };
        match r {
            IDYES => FIRE_NOW.store(true, Ordering::SeqCst),
            IDNO => DELAY_SECS.store(600, Ordering::SeqCst),
            _ => CANCEL_REQ.store(true, Ordering::SeqCst),
        }
    });
}

// -- native control helpers --------------------------------------------------

fn set_text(hwnd: HWND, s: &str) {
    let w = to_wide(s);
    unsafe {
        SetWindowTextW(hwnd, w.as_ptr());
    }
}

fn get_text(hwnd: HWND) -> String {
    unsafe {
        let len = GetWindowTextLengthW(hwnd);
        let mut buf = vec![0u16; (len + 1) as usize];
        GetWindowTextW(hwnd, buf.as_mut_ptr(), len + 1);
        from_wide(&buf)
    }
}

fn combo_add(hwnd: HWND, s: &str) {
    let w = to_wide(s);
    unsafe {
        SendMessageW(hwnd, CB_ADDSTRING, 0, w.as_ptr() as isize);
    }
}

fn combo_get(hwnd: HWND) -> usize {
    unsafe { SendMessageW(hwnd, CB_GETCURSEL, 0, 0).max(0) as usize }
}

fn combo_set(hwnd: HWND, i: usize) {
    unsafe {
        SendMessageW(hwnd, CB_SETCURSEL, i, 0);
    }
}

fn lb_add(hwnd: HWND, s: &str) {
    let w = to_wide(s);
    unsafe {
        SendMessageW(hwnd, LB_ADDSTRING, 0, w.as_ptr() as isize);
    }
}

fn lb_get(hwnd: HWND) -> usize {
    unsafe { SendMessageW(hwnd, LB_GETCURSEL, 0, 0).max(0) as usize }
}

fn lb_set(hwnd: HWND, i: usize) {
    unsafe {
        SendMessageW(hwnd, LB_SETCURSEL, i, 0);
    }
}

fn show(hwnd: HWND, on: bool) {
    unsafe {
        ShowWindow(hwnd, if on { SW_SHOW } else { SW_HIDE });
    }
}

fn read_plan_from_ui() -> Plan {
    let g = gui();
    let task = Task::all()[lb_get(g.task_list).min(Task::all().len() - 1)];
    // Raw text; validation happens in Runtime::start (error dialog on Start).
    let (p1, p2, p3) = match cur_mode() {
        Mode::Daily => match parse_hhmm(&get_text(g.etime)) {
            Some((h, m)) => (h.to_string(), m.to_string(), "0".into()),
            None => (get_text(g.etime).trim().to_string(), String::new(), "0".into()),
        },
        Mode::Once => (
            get_text(g.edate).trim().to_string(),
            get_text(g.etime2).trim().to_string(),
            String::new(),
        ),
        Mode::Countdown => (get_text(g.edit_h), get_text(g.edit_m), get_text(g.edit_s)),
        Mode::Idle | Mode::Interval => (get_text(g.edit_n), String::new(), String::new()),
    };
    Plan {
        task,
        mode: cur_mode(),
        p1,
        p2,
        p3,
        remind_min: if is_checked(g.chk_remind) { 5 } else { 0 },
    }
}

fn refresh_inputs() {
    let g = gui();
    let m = cur_mode();
    show(g.etime, m == Mode::Daily);
    show(g.edate, m == Mode::Once);
    show(g.etime2, m == Mode::Once);
    let cd = m == Mode::Countdown;
    show(g.edit_h, cd);
    show(g.edit_m, cd);
    show(g.edit_s, cd);
    show(g.edit_n, m == Mode::Idle || m == Mode::Interval);
    set_text(g.hint, m.hint());
}

// -- scheduler tick -> native views ------------------------------------------

fn update_views() {
    let g = gui();
    let r = rt();
    if r.active {
        let rem = r.remaining();
        let desc = r.describe();
        set_text(g.big, &fmt_dur(rem));
        set_text(g.desc, &format!("{desc}  •  in {}", fmt_dur(rem)));
        set_text(g.btn_start, "Cancel task");
        unsafe {
            SendMessageW(g.prog, PBM_SETPOS, (r.fraction_done(rem) * 100.0) as usize, 0);
        }
        if r.plan.remind_min > 0 && !r.reminded && rem <= r.plan.remind_min as i64 * 60 && rem > 5 {
            r.reminded = true;
            spawn_reminder(r.plan.task.name().to_string(), rem);
        }
    } else {
        // idle: show the configured target
        let p = r.plan.clone();
        match p.mode {
            Mode::Daily => {
                let t = format!(
                    "{:02}:{:02}",
                    p.p1.trim().parse::<u32>().unwrap_or(23),
                    p.p2.trim().parse::<u32>().unwrap_or(0)
                );
                set_text(g.big, &t);
                set_text(g.desc, &format!("{} daily at {t}", p.task.name()));
            }
            Mode::Once => {
                set_text(g.big, p.p2.trim());
                set_text(g.desc, &format!("{} once at {} {}", p.task.name(), p.p1.trim(), p.p2.trim()));
            }
            _ => {
                set_text(g.big, "--:--");
                set_text(g.desc, &r.describe());
            }
        }
        set_text(g.btn_start, "Start task");
        unsafe {
            SendMessageW(g.prog, PBM_SETPOS, 0, 0);
        }
    }
}

/// One scheduler tick, called from WM_TIMER.
fn tick() {
    // Apply reminder-dialog decisions first.
    rt().apply_reminder_signals();

    if rt().active && rt().remaining() <= 0 {
        let task = rt().plan.task;
        let mode = rt().plan.mode;
        let ok = do_task(task);
        if !ok {
            let m = to_wide(&format!("Failed to execute '{}'. (Sleep/Hibernate may be disabled on this PC.)", task.name()));
            let t = to_wide("PowerOff");
            unsafe {
                MessageBoxW(gui().main, m.as_ptr(), t.as_ptr(), MB_OK | MB_ICONERROR);
            }
        }
        match mode {
            Mode::Interval => {
                let mins: i64 = rt().plan.p1.trim().parse().unwrap_or(60);
                rt().fire_at = local_now_secs() + mins * 60;
                rt().span = mins * 60;
                rt().delay_applied = 0;
                rt().reminded = false;
            }
            Mode::Daily => {
                rt().reminded = false;
                rt().delay_applied = 0;
            }
            Mode::Idle if task.repeats_on_idle() => {
                rt().reminded = false;
            }
            _ => {
                rt().active = false;
            }
        }
        save_config(&rt().plan.clone());
    }

    update_views();
}

fn toggle_start() {
    if rt().active {
        rt().active = false;
        update_views();
        return;
    }
    let plan = read_plan_from_ui();
    rt().plan = plan;
    match rt().start() {
        Ok(_) => {
            save_config(&rt().plan.clone());
        }
        Err(e) => {
            let (m, t) = (to_wide(&e), to_wide("PowerOff"));
            unsafe {
                MessageBoxW(gui().main, m.as_ptr(), t.as_ptr(), MB_OK | MB_ICONWARNING);
            }
        }
    }
    update_views();
}

fn hide_console_window() {
    unsafe {
        let c = GetConsoleWindow();
        if !c.is_null() {
            ShowWindow(c, SW_HIDE);
        }
    }
}

fn make_font(face: &str, px: i32, weight: i32) -> HFONT {
    let f = to_wide(face);
    // height, width, esc, orient, weight, italic, ul, strike, charset,
    // out/in precision, quality, pitch|family, face
    unsafe { CreateFontW(px, 0, 0, 0, weight, 0, 0, 0, 1, 0, 0, 5, 0, f.as_ptr()) }
}

fn mk_child(
    class: &str,
    text: &str,
    style: u32,
    x: i32,
    y: i32,
    wpx: i32,
    h: i32,
    parent: HWND,
    id: isize,
    hinst: HINSTANCE,
    font: HFONT,
) -> HWND {
    let c = to_wide(class);
    let t = to_wide(text);
    let hwnd = unsafe {
        CreateWindowExW(
            0,
            c.as_ptr(),
            t.as_ptr(),
            style,
            x,
            y,
            wpx,
            h,
            parent,
            id as HMENU,
            hinst,
            std::ptr::null(),
        )
    };
    unsafe {
        SendMessageW(hwnd, WM_SETFONT, font as usize, 1);
    }
    hwnd
}

unsafe extern "system" fn wndproc(hwnd: HWND, msg: u32, wp: WPARAM, lp: LPARAM) -> LRESULT {
    match msg {
        WM_CREATE => {
            let hinst = GetModuleHandleW(std::ptr::null());
            let font = make_font("Segoe UI Variable Text", -12, 400);
            let font = if font.is_null() { make_font("Segoe UI", -12, 400) } else { font };
            let bigfont = make_font("Segoe UI Variable Display", -22, 600);
            let bigfont = if bigfont.is_null() { make_font("Segoe UI", -22, 600) } else { bigfont };
            let btn = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON as u32;
            let chk = WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX as u32;
            let lbl = WS_CHILD | WS_VISIBLE;
            let group = WS_CHILD | WS_VISIBLE | BS_GROUPBOX as u32;
            let combo_style = WS_CHILD
                | WS_VISIBLE
                | WS_VSCROLL
                | CBS_DROPDOWNLIST as u32
                | CBS_HASSTRINGS as u32;
            let edit_style = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT as u32 | ES_NUMBER as u32;
            let text_style = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT as u32;
            let list_style = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL
                | LBS_NOTIFY as u32 | LBS_HASSTRINGS as u32 | LBS_NOINTEGRALHEIGHT as u32;

            let mut g: Gui = unsafe { std::mem::zeroed() };
            g.main = hwnd;
            g.font = font;
            g.bigfont = bigfont;
            // NOTE: GUI is installed only after all handles are stored
            // below. Child creation notifies re-entrantly (EN_CHANGE);
            // handlers check GUI_READY and stand down until then.

            // 1. Task group + plain list box
            mk_child("BUTTON", "Task", group, 12, 12, 170, 300, hwnd, 0, hinst, font);
            g.task_list = mk_child("LISTBOX", "", list_style, 24, 38, 146, 254, hwnd, ID_TASK_LIST, hinst, font);
            for t in Task::all() {
                lb_add(g.task_list, t.name());
            }
            // 2. Schedule group: native combo + text inputs + hint
            mk_child("BUTTON", "Schedule", group, 192, 12, 380, 190, hwnd, 0, hinst, font);
            g.combo = mk_child("COMBOBOX", "", combo_style, 204, 40, 220, 160, hwnd, ID_MODE_COMBO, hinst, font);
            for m in ["Daily", "Once", "Countdown", "Idle", "Repeat every"] {
                combo_add(g.combo, m);
            }
            g.etime = mk_child("EDIT", "23:00", text_style, 204, 74, 110, 24, hwnd, ID_ETIME, hinst, font);
            g.edate = mk_child("EDIT", "2026-10-04", text_style, 204, 74, 150, 24, hwnd, ID_EDATE, hinst, font);
            g.etime2 = mk_child("EDIT", "23:00", text_style, 362, 74, 100, 24, hwnd, ID_ETIME2, hinst, font);
            g.edit_h = mk_child("EDIT", "1", edit_style, 204, 74, 60, 24, hwnd, ID_EDITH, hinst, font);
            g.edit_m = mk_child("EDIT", "0", edit_style, 272, 74, 60, 24, hwnd, ID_EDITM, hinst, font);
            g.edit_s = mk_child("EDIT", "0", edit_style, 340, 74, 60, 24, hwnd, ID_EDITS, hinst, font);
            g.edit_n = mk_child("EDIT", "15", edit_style, 204, 74, 80, 24, hwnd, ID_EDITN, hinst, font);
            g.hint = mk_child("STATIC", "", lbl, 204, 106, 356, 78, hwnd, ID_HINT, hinst, font);
            // 3. Status group: big readout + description + native progress
            mk_child("BUTTON", "Status", group, 192, 212, 380, 150, hwnd, 0, hinst, font);
            g.big = mk_child("STATIC", "--:--", lbl, 204, 236, 250, 40, hwnd, ID_BIG, hinst, bigfont);
            g.desc = mk_child("STATIC", "", lbl, 204, 278, 356, 20, hwnd, ID_DESC, hinst, font);
            g.prog = mk_child(
                "msctls_progress32",
                "",
                WS_CHILD | WS_VISIBLE | PBS_SMOOTH as u32,
                204, 304, 356, 20, hwnd, ID_PROG, hinst, font,
            );
            unsafe {
                SendMessageW(g.prog, PBM_SETRANGE, 0, 100 << 16);
            }
            // options + dialog buttons (OK/Cancel pattern, right aligned)
            g.chk_remind = mk_child("BUTTON", "Remind me 5 minutes before", chk, 24, 324, 230, 20, hwnd, ID_CHK_REMIND, hinst, font);
            g.chk_startup = mk_child("BUTTON", "Run at Windows startup", chk, 24, 348, 230, 20, hwnd, ID_CHK_STARTUP, hinst, font);
            g.btn_start = mk_child("BUTTON", "Start task", btn, 348, 452, 110, 30, hwnd, ID_BTN_START, hinst, font);
            g.btn_tray = mk_child("BUTTON", "To tray", btn, 466, 452, 94, 30, hwnd, ID_BTN_TRAY, hinst, font);

            // commit handles; from here on re-entrant handlers may run
            #[allow(static_mut_refs)]
            {
                GUI = Some(g);
            }
            GUI_READY.store(true, Ordering::SeqCst);

            // restore last config
            let plan = rt().plan.clone();
            lb_set(
                g.task_list,
                Task::all().iter().position(|t| *t == plan.task).unwrap_or(5),
            );
            set_cur_mode(plan.mode);
            combo_set(
                g.combo,
                Mode::all().iter().position(|m| *m == plan.mode).unwrap_or(0),
            );
            // time fields: combine/split to the HH:MM + YYYY-MM-DD inputs
            // (etime is only visible in Daily mode, where p1 is an hour)
            let time_str = if parse_ymd(plan.p1.trim()).is_some() {
                plan.p2.trim().to_string()
            } else {
                format!("{}:{}", plan.p1.trim(), plan.p2.trim())
            };
            set_text(g.etime, &time_str);
            set_text(g.edate, plan.p1.trim());
            set_text(g.etime2, plan.p2.trim());
            set_text(g.edit_h, plan.p1.trim());
            set_text(g.edit_m, plan.p2.trim());
            set_text(g.edit_s, plan.p3.trim());
            set_text(g.edit_n, plan.p1.trim());
            set_checked(g.chk_remind, plan.remind_min > 0);
            set_checked(g.chk_startup, get_startup());

            refresh_inputs();
            update_views();
            SetTimer(hwnd, TIMER_ID, 1000, None);
            add_tray(hwnd);
            return 0;
        }
        WM_ERASEBKGND => {
            // Theme-aware dialog background (statics show through via
            // NULL_BRUSH in WM_CTLCOLORSTATIC below).
            unsafe {
                let hdc = wp as *mut std::ffi::c_void;
                let mut rc: RECT = std::mem::zeroed();
                GetClientRect(hwnd, &mut rc);
                let br = CreateSolidBrush(dlg_bg());
                FillRect(hdc, &rc, br);
                DeleteObject(br);
            }
            return 1;
        }
        WM_CTLCOLORSTATIC => {
            // Transparent statics over our background, theme-correct text.
            unsafe {
                let hdc = wp as *mut std::ffi::c_void;
                SetBkMode(hdc, TRANSPARENT as i32);
                SetTextColor(hdc, if dark() { rgb(236, 236, 236) } else { rgb(0, 0, 0) });
                return GetStockObject(NULL_BRUSH) as isize;
            }
        }
        WM_COMMAND => {
            if !GUI_READY.load(Ordering::SeqCst) {
                // Creation-time notifications arrive before GUI is installed.
                return 0;
            }
            let id = (wp & 0xFFFF) as isize;
            let notif = ((wp >> 16) & 0xFFFF) as u32;
            match id as u32 {
                IDM_SHOW => {
                    ShowWindow(hwnd, SW_SHOW);
                    SetForegroundWindow(hwnd);
                }
                IDM_START => toggle_start(),
                IDM_NOW => {
                    do_task(Task::Sleep);
                }
                IDM_EXIT => {
                    DestroyWindow(hwnd);
                }
                _ => {}
            }
            if id == ID_MODE_COMBO && notif == CBN_SELCHANGE as u32 {
                set_cur_mode(Mode::all()[combo_get(gui().combo).min(Mode::all().len() - 1)]);
                refresh_inputs();
                update_views();
            }
            if id == ID_BTN_START && notif == BN_CLICKED as u32 {
                toggle_start();
            }
            if id == ID_BTN_TRAY && notif == BN_CLICKED as u32 {
                ShowWindow(hwnd, SW_HIDE);
            }
            if (id == ID_CHK_REMIND || id == ID_CHK_STARTUP) && notif == BN_CLICKED as u32 {
                set_startup(is_checked(gui().chk_startup));
                save_config(&read_plan_from_ui());
            }
            // edits changed -> save draft silently (no validation yet)
            if (id == ID_ETIME || id == ID_EDATE || id == ID_ETIME2
                || id == ID_EDITH || id == ID_EDITM || id == ID_EDITS || id == ID_EDITN)
                && notif == EN_CHANGE as u32
            {
                let p = read_plan_from_ui();
                rt().plan.p1 = p.p1;
                rt().plan.p2 = p.p2;
                rt().plan.p3 = p.p3;
            }
            return 0;
        }
        WM_TIMER => {
            tick();
            return 0;
        }
        WM_TRAY => {
            let ev = (lp & 0xFFFF) as u32;
            if ev == WM_LBUTTONDBLCLK || ev == WM_RBUTTONUP {
                show_tray_menu(hwnd);
            }
            return 0;
        }
        WM_SETTINGCHANGE => {
            // System theme / colors changed (e.g. ImmersiveColorSet):
            // re-read light/dark, re-apply, repaint everything.
            read_theme();
            apply_dark_mode(hwnd);
            update_views();
            return 0;
        }
        WM_CLOSE => {
            if rt().active {                // like Wise: keep running in the tray
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        }
        WM_DESTROY => {
            del_tray(hwnd);
            KillTimer(hwnd, TIMER_ID);
            PostQuitMessage(0);
            return 0;
        }
        _ => {}
    }
    DefWindowProcW(hwnd, msg, wp, lp)
}

fn run_gui(start_in_tray: bool) {
    unsafe {
        // progress-bar class for the native progress control
        let mut icc = INITCOMMONCONTROLSEX {
            dwSize: std::mem::size_of::<INITCOMMONCONTROLSEX>() as u32,
            dwICC: ICC_PROGRESS_CLASS,
        };
        InitCommonControlsEx(&mut icc);
        let hinst = GetModuleHandleW(std::ptr::null());
        let cls = to_wide("PowerOffWnd");
        let wc = WNDCLASSW {
            style: CS_HREDRAW | CS_VREDRAW,
            lpfnWndProc: Some(wndproc),
            cbClsExtra: 0,
            cbWndExtra: 0,
            hInstance: hinst,
            hIcon: std::ptr::null_mut(),
            hCursor: LoadCursorW(std::ptr::null_mut(), IDC_ARROW),
            hbrBackground: (COLOR_WINDOW as isize + 1) as *mut _,
            lpszMenuName: std::ptr::null(),
            lpszClassName: cls.as_ptr(),
        };
        RegisterClassW(&wc);
        let title = to_wide("PowerOff — tiny auto shutdown");
        let hwnd = CreateWindowExW(
            0,
            cls.as_ptr(),
            title.as_ptr(),
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            600,
            540,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            hinst,
            std::ptr::null(),
        );
        if hwnd.is_null() {
            panic!("CreateWindowExW failed");
        }
        apply_dark_mode(hwnd);
        if !start_in_tray {
            ShowWindow(hwnd, SW_SHOW);
        } else {
            ShowWindow(hwnd, SW_HIDE);
        }
        let mut msg: MSG = std::mem::zeroed();
        while GetMessageW(&mut msg, std::ptr::null_mut(), 0, 0) > 0 {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

// ---------------------------------------------------------------------------
// Headless console scheduler (for `schtasks` and scripts)
// ---------------------------------------------------------------------------

fn run_headless(plan: Plan) {
    let mut r = Runtime::new(plan);
    match r.start() {
        Ok(d) => println!("PowerOff: {d}"),
        Err(e) => {
            eprintln!("PowerOff: {e}");
            std::process::exit(2);
        }
    }
    loop {
        r.apply_reminder_signals();
        if !r.active {
            println!("PowerOff: cancelled.");
            break;
        }
        let rem = r.remaining();
        if rem <= 0 {
            if do_task(r.plan.task) {
                println!("PowerOff: executed '{}'.", r.plan.task.name());
            } else {
                eprintln!("PowerOff: failed to execute '{}'.", r.plan.task.name());
                std::process::exit(3);
            }
            match r.plan.mode {
                Mode::Interval | Mode::Daily => {
                    r.reminded = false;
                    if r.plan.mode == Mode::Interval {
                        let mins: i64 = r.plan.p1.trim().parse().unwrap_or(60);
                        r.fire_at = local_now_secs() + mins * 60;
                        r.span = mins * 60;
                    }
                    std::thread::sleep(std::time::Duration::from_secs(61));
                    continue;
                }
                Mode::Idle if r.plan.task.repeats_on_idle() => {
                    r.reminded = false;
                    std::thread::sleep(std::time::Duration::from_secs(61));
                    continue;
                }
                _ => break,
            }
        }
        if r.plan.remind_min > 0 && !r.reminded && rem <= r.plan.remind_min as i64 * 60 && rem > 5 {
            println!("PowerOff: '{}' in {} …", r.plan.task.name(), fmt_dur(rem));
            r.reminded = true;
        }
        std::thread::sleep(std::time::Duration::from_secs(1));
    }
}

fn warn_then_fire(task: Task, warn_secs: u64) {
    if warn_secs > 0 {
        let msg = format!(
            "{} in {warn_secs} seconds.\n\nPress Cancel to abort.",
            task.name()
        );
        let (m, t) = (to_wide(&msg), to_wide("PowerOff"));
        let r = unsafe {
            MessageBoxW(std::ptr::null_mut(), m.as_ptr(), t.as_ptr(), MB_OKCANCEL | MB_ICONWARNING | MB_SYSTEMMODAL)
        };
        if r == IDCANCEL {
            println!("PowerOff: aborted by user.");
            return;
        }
    }
    if do_task(task) {
        println!("PowerOff: executed '{}'.", task.name());
    } else {
        eprintln!("PowerOff: failed to execute '{}'.", task.name());
        std::process::exit(3);
    }
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

fn help() -> &'static str {
    "PowerOff 1.0 — tiny Wise Auto Shutdown clone (native Win32, ~few hundred KB)\n\
     \n\
     GUI (no args, defaults to Sleep / Daily 23:00):\n  \
       PowerOff.exe [--tray]\n\
     \n\
     Headless scheduler (runs in console until the task fires):\n  \
       PowerOff.exe --task sleep --daily 23:00\n  \
       PowerOff.exe --task shutdown --once \"2026-10-05 23:00\"\n  \
       PowerOff.exe --task sleep --in 3600 | --countdown 1:00:00\n  \
       PowerOff.exe --task lock --idle 15 | --task sleep --every 60\n\
     \n\
     Fire immediately (for Task Scheduler / scripts):\n  \
       PowerOff.exe --task sleep --fire-now [--warn-secs 60]\n\
     \n\
     Daily 11 PM sleep that survives reboot (recommended):\n  \
       PowerOff.exe --install-daily 23:00 --task sleep\n  \
       PowerOff.exe --uninstall\n\
     \n\
     Tasks: shutdown restart poweroff logoff lock sleep hibernate"
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let has = |f: &str| args.iter().any(|a| a == f);
    let val = |f: &str| -> Option<String> {
        args.iter()
            .position(|a| a == f)
            .and_then(|i| args.get(i + 1).cloned())
    };

    if has("--help") || has("-h") || has("/?") {
        println!("{}", help());
        return;
    }
    if has("--uninstall") {        match uninstall_daily() {
            Ok(s) => println!("PowerOff: {s}"),
            Err(e) => {
                eprintln!("PowerOff: {e}");
                std::process::exit(1);
            }
        }
        return;
    }
    if let Some(st) = val("--install-daily") {
        let task = val("--task").and_then(|t| Task::parse(&t)).unwrap_or(Task::Sleep);
        match install_daily(task, &st) {
            Ok(s) => println!("PowerOff: {s}"),
            Err(e) => {
                eprintln!("PowerOff: {e}");
                std::process::exit(1);
            }
        }
        return;
    }

    // Fire-now path (used by the scheduled task itself).
    let task_raw = val("--task");
    if let Some(ref t) = task_raw {
        if Task::parse(t).is_none() {
            eprintln!("PowerOff: unknown task '{t}'. Tasks: shutdown restart poweroff logoff lock sleep hibernate");
            std::process::exit(2);
        }
    }
    let task_opt = task_raw.and_then(|t| Task::parse(&t));
    if has("--fire-now") {
        let Some(task) = task_opt else {
            eprintln!("PowerOff: --fire-now needs --task <name>");
            std::process::exit(2);
        };
        let warn: u64 = val("--warn-secs").and_then(|v| v.parse().ok()).unwrap_or(0);
        warn_then_fire(task, warn);
        return;
    }

    // Headless scheduler path: any timing flag selects it.
    let timing = val("--daily")
        .map(|v| (Mode::Daily, v))
        .or_else(|| val("--once").map(|v| (Mode::Once, v)))
        .or_else(|| val("--countdown").map(|v| (Mode::Countdown, v)))
        .or_else(|| val("--in").map(|v| (Mode::Countdown, v)))
        .or_else(|| val("--idle").map(|v| (Mode::Idle, v)))
        .or_else(|| val("--every").map(|v| (Mode::Interval, v)));
    if let (Some(task), Some((mode, raw))) = (task_opt, timing) {
        let plan = match mode {
            Mode::Daily => {
                let Some((h, m)) = parse_hhmm(&raw) else {
                    eprintln!("PowerOff: --daily needs HH:MM");
                    std::process::exit(2);
                };
                Plan { task, mode, p1: h.to_string(), p2: m.to_string(), p3: "0".into(), remind_min: 1 }
            }
            Mode::Once => {
                let Some((dt, tm)) = raw.split_once(' ') else {
                    eprintln!("PowerOff: --once needs \"YYYY-MM-DD HH:MM\"");
                    std::process::exit(2);
                };
                Plan { task, mode, p1: dt.into(), p2: tm.into(), p3: String::new(), remind_min: 1 }
            }
            Mode::Countdown => {
                // accept seconds or H:M:S
                let (h, m, s) = if let Some((h, m)) = parse_hhmm(&raw) {
                    (0, h as i64, m as i64)
                } else if raw.contains(':') {
                    let p: Vec<&str> = raw.split(':').collect();
                    if p.len() != 3 {
                        eprintln!("PowerOff: --countdown needs seconds or H:M:S");
                        std::process::exit(2);
                    }
                    (p[0].parse().unwrap_or(0), p[1].parse().unwrap_or(0), p[2].parse().unwrap_or(0))
                } else {
                    (0, 0, raw.parse().unwrap_or(-1))
                };
                if h < 0 || m < 0 || s <= 0 && h == 0 && m == 0 {
                    eprintln!("PowerOff: bad countdown '{raw}'");
                    std::process::exit(2);
                }
                Plan {
                    task,
                    mode,
                    p1: h.to_string(),
                    p2: m.to_string(),
                    p3: s.to_string(),
                    remind_min: 0,
                }
            }
            Mode::Idle | Mode::Interval => Plan {
                task,
                mode,
                p1: raw,
                p2: String::new(),
                p3: String::new(),
                remind_min: 0,
            },
        };
        run_headless(plan);
        return;
    }
    if task_opt.is_some() {
        eprintln!("PowerOff: --task needs a timing flag (see --help).");
        std::process::exit(2);
    }

    // GUI path.
    hide_console_window();
    read_theme();
    if has("--dark") {
        DARK.store(true, Ordering::SeqCst);
    }
    if has("--light") {
        DARK.store(false, Ordering::SeqCst);
    }
    unsafe {
        RT = Some(Runtime::new(load_config()));
    }
    run_gui(has("--tray") || has("-tray"));
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn civil_dates() {
        assert_eq!(days_from_civil(1970, 1, 1), 0);
        assert_eq!(days_from_civil(1970, 1, 2), 1);
        assert_eq!(days_from_civil(2000, 1, 1), 10957);
        assert!(days_from_civil(2026, 10, 4) > days_from_civil(2026, 10, 3));
    }

    #[test]
    fn hhmm_parses() {
        assert_eq!(parse_hhmm("23:00"), Some((23, 0)));
        assert_eq!(parse_hhmm("9"), Some((9, 0)));
        assert_eq!(parse_hhmm("07:05"), Some((7, 5)));
        assert!(parse_hhmm("24:00").is_none());
        assert!(parse_hhmm("12:60").is_none());
        assert!(parse_hhmm("abc").is_none());
    }

    #[test]
    fn durations_format() {
        assert_eq!(fmt_dur(3661), "1:01:01");
        assert_eq!(fmt_dur(61), "1:01");
        assert_eq!(fmt_dur(0), "0:00");
        assert_eq!(fmt_dur(-5), "0:00");
    }

    #[test]
    fn daily_remaining_in_range() {
        let r = Runtime::new(Plan {
            task: Task::Sleep,
            mode: Mode::Daily,
            p1: "23".into(),
            p2: "00".into(),
            p3: String::new(),
            remind_min: 5,
        });
        let rem = r.remaining();
        assert!(rem > 0 && rem <= 86400, "rem={rem}");
    }

    #[test]
    fn countdown_arms() {
        let mut r = Runtime::new(Plan {
            task: Task::Sleep,
            mode: Mode::Countdown,
            p1: "0".into(),
            p2: "1".into(),
            p3: "30".into(),
            remind_min: 0,
        });
        assert!(r.start().is_ok());
        let rem = r.remaining();
        assert!(rem > 80 && rem <= 90, "rem={rem}");
    }

    #[test]
    fn rejects_bad_plans() {
        let mut r = Runtime::new(Plan {
            task: Task::Sleep,
            mode: Mode::Countdown,
            p1: "0".into(),
            p2: "0".into(),
            p3: "0".into(),
            remind_min: 0,
        });
        assert!(r.start().is_err());
    }

    #[test]
    fn task_names_roundtrip() {
        for t in Task::all() {
            let lower = format!("{:?}", t).to_ascii_lowercase();
            assert_eq!(Task::parse(&lower), Some(*t));
        }
        assert_eq!(Task::parse("reboot"), Some(Task::Restart));
    }
}
